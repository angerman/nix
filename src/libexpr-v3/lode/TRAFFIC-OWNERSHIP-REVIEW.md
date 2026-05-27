# v3 Eval-Traffic Ownership — A Critical Review

> **ARCHIVED-IN-PLACE 2026-05-27**: This doc is historical (pre-strategic-doc-set or one-off RCA). Preserved here because it is still referenced from one or more KEEP docs. See [`LODE_CLEANUP_REVIEW_2026-05-27.md`](LODE_CLEANUP_REVIEW_2026-05-27.md) for the archival rationale.

---


*2026-05-04. Companion to `GC-REVIEW.md`. Synthesises a code audit of
`libexpr-v3/` (BENCH, OPTIMIZATION_PLAN, REVIEW, vm.cc, lower.cc, primops.cc,
v3_hook.cc, fiber.cc) against VM literature on the primitive-vs-opcode
boundary (Smalltalk primitiveFailed, LuaJIT, V8 ICs/Torque, OCaml ZINC,
Truffle PE, PyPy meta-tracing, Vmgen threaded code).*

## 1. The single most important finding

The codebase audit surfaced one fact that reframes the whole question:

**The cutover hook fires 31–77 times across an entire `nixpkgs.hello.outPath`
/ 21k-attribute scan workload.** Per `BENCH-REAL-WORLD-2026-05-04.md`. That is
not "v3 owning some of the traffic" — that is v3 being asked to evaluate ~50
root expressions in an evaluation that does *billions* of bytecode-equivalent
steps.

The reason is buried in `REVIEW_2026-05-03.md` as **MED-21**:
`v3CallFunctionHook` (eval.hh:770) is *declared but never assigned*. So when
the tree-walker reaches `EvalState::callFunction` — the canonical place a
lambda gets entered — it never even *attempts* to dispatch through v3. v3 only
runs when the CLI's top-level cutover explicitly hands a `Value` to v3 to
evaluate, and that happens once per command invocation plus a handful of
nested cutovers.

Everything else the user is asking about — VM-native primops, inlining,
opcodes vs primops — is downstream of this. It does not matter how fast v3's
`OP_HEAD` is if 99.9% of `head` calls never enter v3 to begin with.

**The dam is the hook, not the dispatch loop.** Fix the dam first.

## 2. What "owning more traffic" actually decomposes into

There are three independently-broken levers, each blocking the next.

### Lever 1: the call-function hook (the dam)

`v3CallFunctionHook` needs to be wired — at minimum, when the tree-walker
enters a lambda whose body has been (or could be) lowered to v3 bytecode, hand
it to v3. The mechanism exists; the slot is empty (eval.cc:1834 area,
MED-21). This is a 1–2 day fix in principle, but with a long tail of "what if
v3 can't compile this lambda" cases (see Lever 2).

A useful framing: today's v3 is a **whole-evaluation cutover** ("evaluate this
root in v3"). The architecture that actually pays off is **per-call cutover**
("call this function in v3 if you can"). The infrastructure is there —
`Tag::Bridge` thunks (closure.hh:88) handle the v3 → tree-walker direction;
the missing piece is tree-walker → v3 at every call site.

### Lever 2: Phase 5 (slot pointers through `callFunction`)

`OPTIMIZATION_PLAN.md §1` is explicit: until lambda parameters can carry slot
identity from a recursive attribute set into the lambda body, every
nixpkgs-style `f = self: with self; ...` falls back. That pattern is dominant
in Nixpkgs (`callPackage`, `lib.makeOverridable`, every fixed-point
construction). Without Phase 5, even if Lever 1 is wired, v3 fails over on
the most common code shape and traffic stays in the tree-walker.

Phase 5 is described as ~500 lines and "pending" in the optimization plan. It
is the second dam.

### Lever 3: bridge cost

Once Levers 1 and 2 are working, every v3 → primop or v3 →
unsupported-construct fall-back goes through the fiber bridge: 16 MiB stack
allocation (`fiber.cc:144`), two `fiberSwap` register saves, TLS state
save/restore, plus `treeWalkerToV3` round-trip with documented identity loss
on functions (MED-1). The bench doesn't measure this in isolation, but at
non-trivial volume it's a cliff.

The mitigation here is structural: **make v3 self-sufficient enough that the
bridge fires rarely**. That's where the user's question about VM-native
primops becomes relevant — but only after Levers 1 and 2.

## 3. Bench evidence

From `BENCH-REAL-WORLD-2026-05-04.md` (aarch64-darwin):

| Workload | Mode | real (s) | user (s) | instructions |
|---|---|---|---|---|
| cardano-node | tree-walker | 2.69 | 2.32 | 31.04 G |
| cardano-node | v3 cutover hook only | 2.72 (+1.1%) | 2.37 | 31.55 G |
| cardano-node | v3 + force hook | **2.66 (-1.1%)** | 2.33 | 31.03 G |
| nixpkgs scan (~21k attrs) | tree-walker | 4.91 | 5.16 | 74.86 G |
| nixpkgs scan | v3 cutover hook only | 4.89 | 5.71 | 78.77 G |
| nixpkgs scan | v3 + force hook | **4.80 (-2.2%)** | 5.16 | 74.35 G (-0.7%) |

Cutover hook fire counts: **31** (cardano-node), **77** (nixpkgs scan).

Top primops by leaf-frame frequency on nixpkgs scan: `prim_derivationStrict`
~8400, `prim_getAttr` ~8200, `prim_isAttrs` ~5600, `prim_filter` ~4150,
`prim_length` ~3970, `prim_tryEval` ~3350.

These primops are called from **tree-walker's** `EvalState::callFunction`,
not from v3. The bench is measuring tree-walker performance with v3 watching
from the sidelines.

## 4. On primops, inlining, and the C boundary

The VM literature converges on three patterns directly applicable to v3.
Ordered by what actually moves the needle *given* Levers 1 and 2 are
unblocked:

### (a) Smalltalk's `primitiveFailed` pattern

Every primop-by-name has a fast-path opcode that handles the common case and
falls through to the existing C primop on guard failure. The opcode and the
primop are semantically identical — the opcode is purely an optimisation
hint, not a separate codepath.

Concrete candidates from the audit:

- **Type predicates** (currently primops, all single-cycle tag tests):
  `OP_IS_NULL`, `OP_IS_ATTRS`, `OP_IS_LIST`, `OP_IS_STRING`, `OP_IS_INT`,
  `OP_IS_BOOL`, `OP_IS_FLOAT`, `OP_IS_PATH`, `OP_IS_FUNCTION`. Each call
  costs ~30 cycles today (dispatch + primop call + one tag check inside the
  primop body). Inlined: 1–2 cycles.
- **List selectors** (currently primops): `OP_HEAD`, `OP_TAIL`, `OP_ELEM_AT`,
  `OP_LENGTH`. Fast path: tag-check, deref `ListVec*`, return; slow path:
  existing primop (handles thunks, type errors, OOB).
- **Dynamic attr access**: `OP_GET_ATTR_DYN` exists but currently calls
  `prim_getAttr` for the dynamic-name case. An IC keyed on (Bindings shape,
  name SymbolId) — the V8 pattern — would compress this to a single load on
  hit.

Estimated saving: ~30 cycles per call × millions of invocations on a Nixpkgs
eval. Small per call, real in aggregate. The discipline here matters: **the
opcode and the primop must be semantically identical** so the fall-through is
bug-compatible. That rules out divergent fast-path implementations and keeps
the audit surface small.

References: Cog "Primitives and the Partial Read Barrier" — Miranda;
JSC/V8 stdlib intrinsic pattern.

### (b) Stdlib-in-bytecode (the OCaml ZINC pattern, the PyPy argument)

A surprising amount of `builtins.*` is expressible as Nix code: `map`,
`filter`, `foldl'`, `concatMap`, `all`, `any`, `genList` are pure-functional
wrappers around primitive operations. Today they are C primops with the
hazard catalogue from §7 of `GC-REVIEW.md`. If they were *Nix bytecode
functions* loaded at startup, every call would stay in v3, and the dangerous
primops in primops.cc shrink to the genuinely-primitive ones (`derivation`,
`import`, `readFile`, `fetchTree`, hashing, JSON, attrset construction,
string ops).

The PyPy argument applies here even without a JIT plan: a primop written in
C is *unconditionally* a CALL with marshalling. A bytecode definition of
`map` runs through the same opcode path that user code does, with all of
v3's caching, force semantics, and (eventual) GC-precise root tracking
applying uniformly. PyPy's measured 4–10x speedup over CPython on hot loops
is precisely the "no C boundary on the inner path" win.

The cost is real: `lib.lists.map` written in Nix and called as bytecode is
slower than today's hand-tuned C `prim_map` on a *single* call. The win is
at aggregate call volume and at reduced bridge frequency. **Worth measuring
before committing**; this is the lever where I'd push back on any confident
recommendation.

References: OCaml ZINC instruction set (~150 opcodes, stdlib mostly bytecode
not C); Bolz et al. "Tracing the Meta-Level".

### (c) OP_CALL_PRIMOP peephole fusion

The audit confirmed OP_CALL_PRIMOP is implemented but the peephole pass that
fuses `OP_CALL_1; OP_CALL_1; ...; OP_CALL` into a single OP_CALL_PRIMOP for
known multi-arg primops was deferred (per the existing peephole-optimizer
memory). Easy and bounded; do it in the same week as the type-predicate
opcodes.

## 5. What I'd push back on from the literature

The research synthesis recommended **inline caching for attribute access** as
its #1 lever. I think that overstates the win for v3 specifically. v3
already has a 4-way IC on `OP_ATTRS_SELECT` (bytecode.hh:239–250); the
current bottleneck per the bench is that *most* attribute accesses don't go
through `OP_ATTRS_SELECT` at all — they happen inside C++ primops called
from the tree-walker (`prim_getAttr` ~8200 calls). The IC is fine. The
traffic isn't there to use it.

This is the same point as the dam: optimisations to v3's hot path are
conditional on v3 being on the hot path. Until Levers 1 and 2 land, the IC
delta is in the noise.

## 6. Critical ordering

The ordering implied by the audit + literature, weighted by what actually
moves the bench numbers:

1. **Wire `v3CallFunctionHook` (MED-21).** Highest-leverage single change in
   the entire roadmap. Until this is done, every other v3 perf win is
   bottlenecked by traffic volume. ~1–2 days for the wiring; longer for the
   "what if v3 can't compile this lambda" tail.
2. **Land Phase 5 slot-threading.** Without it, the wired hook still fails
   over on Nixpkgs's dominant call shape. ~500 lines per
   OPTIMIZATION_PLAN.md.
3. **Add fast-path opcodes with primop fall-through** for type predicates
   (`OP_IS_*`) and list selectors (`OP_HEAD`, `OP_TAIL`, `OP_LENGTH`,
   `OP_ELEM_AT`). Roughly one week; touches `bytecode.hh`, `vm.cc`
   dispatch, `lower.cc` emission. Low risk because the C primops remain as
   the slow path.
4. **Implement OP_CALL_PRIMOP peephole fusion.** Cheap; the deferred memory
   item.
5. **Audit which `builtins.*` can be moved to shipped bytecode.** Measure
   before committing — this is the only item where the literature
   recommendation might not hold under v3's specific cost model.
6. **Computed-goto dispatch.** vm.cc:2 says "comes later once the opcode set
   is stable." After (3) and (4), reassess.
7. **Wider IC and PIC work** (dynamic attr access, megamorphic call sites).
   Only after (1)–(2) demonstrate real traffic hitting the existing ICs.

Items 1 and 2 are the dam. Items 3–4 are the day-after-the-dam-breaks
productivity. Items 5–7 are downstream and need the bench to be re-measured
to justify.

## 7. The bottom line

The framing the question instinctively reached for — "do we need to end up
so much in primops, can we have VM-native primops, inline them more?" — is
exactly right *as a destination*. The literature unanimously agrees: keep
the C boundary thin, prefer opcodes-with-fall-through over primops, prefer
bytecode stdlib over C stdlib, use ICs for property access.

But the precondition is non-obvious from the question itself. **v3 is barely
running.** The cutover hook fires dozens of times, not millions. The dispatch
loop is not the bottleneck because v3 is not the bottleneck — it's not even
on the path. Every primop optimisation, every opcode addition, every IC
tweak is gated on v3 actually executing user code, which today depends on a
hook (`v3CallFunctionHook`) that is declared but never assigned, and a
lambda compatibility story (Phase 5) that hasn't shipped.

So the answer to "how do we make v3 own more traffic" is, in priority order:
**wire the hook, ship Phase 5, then everything else**. The "everything else"
is real and the literature points clearly at what to do — but it's the
second sentence, not the first.

## References

VM-design literature consulted for this review:

- Eliot Miranda. "Primitives and the Partial Read Barrier" (Cog blog).
  <http://www.mirandabanda.org/cogblog/2014/02/08/primitives-and-the-partial-read-barrier/>
- Mike Pall. LuaJIT design notes (lua-users wiki).
  <http://lua-users.org/wiki/MikePall>
- Mathias Bynens. "Shapes and Inline Caches".
  <https://mathiasbynens.be/notes/shapes-ics>
- V8 Torque user manual. <https://v8.dev/docs/torque>
- WebKit blog. "Optimizing JavaScript Standard Library Functions in JSC."
  <https://webkit.org/blog/11934/optimizing-javascript-standard-library-functions-in-jsc/>
- Caolán Lima. "JSC Inline Cache deep dive."
  <https://caiolima.github.io/jsc/2020/03/12/jsc-inline-cache.html>
- Caml Virtual Machine instruction set (ZINC).
  <https://cadmium.x9c.fr/distrib/caml-instructions.pdf>
- Würthinger et al. "Truffle: a self-optimizing runtime system."
  <https://dl.acm.org/doi/10.1145/2384716.2384723>
- Bolz, Cuni, Fijałkowski, Rigo. "Tracing the Meta-Level: PyPy's
  Tracing JIT Compiler."
  <https://dl.acm.org/doi/pdf/10.1145/1565824.1565827>
- Ertl, Gregg. "Vmgen — A Generator of Efficient Virtual Machine
  Interpreters."
  <https://link.springer.com/content/pdf/10.1007/3-540-45937-5_2.pdf>
- Anton Ertl. "Threaded Code."
  <https://www.complang.tuwien.ac.at/forth/threaded-code.html>
- Hölzle, Chambers, Ungar. "Polymorphic Inline Caches."
  <https://bibliography.selflanguage.org/_static/pics.pdf>
- The BEAM Book (Erlang VM internals).
  <https://blog.stenmans.org/theBeamBook/>
