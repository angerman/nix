# v3 VM defect review — 2026-07-03 (second-pass, fresh-eyes)

**Purpose.** Independent second in-depth review of `src/libexpr-v3/` answering the
same question as `DEFECT_AUDIT_2026-07-02.md`: *what concrete defects and bugs
explain why v3 is slower (CPU) and bigger (RSS) than the tree-walker?* This
review was run one day after that audit, seeded with its complete findings list
(including all handbacks and falsified levers) and instructed to report ONLY new
material or corrections. It is the companion document, not a replacement: read
the 07-02 audit first for the P0–P6 inventory and its handbacks.

**Method.** 8 parallel fresh-lens subsystem reviews at HEAD `8ca498235`
(memory representation/allocator · vm.cc 1–8600 calls/force/with ·
vm.cc 8600–end attrs/strings/lists · compiler lowering/emit/optimizer ·
primops systematic barrier+rooting sweep · GC walker/barrier completeness ·
warm/cache path · TW-vs-v3 architectural head-to-head with both codebases
read). Headline claims hand-verified against the working tree by the
synthesizing session (verified excerpts re-read at the cited lines).

**Verification legend** (same as 07-02):
- ✅ hand-verified in this synthesis (code re-read at the cited lines)
- 🔷 reviewer-verified with verbatim excerpt, high confidence
- 🔶 mechanism certain, magnitude/reachability needs measurement

Line numbers are working-tree 2026-07-03 and will drift.

**Authoritative baseline** (P0.4 re-baseline, darwin-4, de-instrumented):
COLD firefox.drvPath 3.62×TW CPU / 1.89× RSS (2.68 s/677 MB), M5 3.04×/3.00×
(10.96 s/2950 MB); WARM firefox 2.43×/1.64× (1.80 s/586 MB), M5 1.81×/2.26×
(6.54 s/2217 MB).

---

## 0. Executive summary

Yesterday's audit found the long tail of line-level defects. This review adds
four things it did not have:

1. **New correctness bugs, including one latent heap-corruption UAF on the
   default path** (`withLookup` writes through a dangling `std::vector`
   reference across a re-entrant force, §1.1), a second missed-barrier site of
   the exact primSort class (`valueLess`, §1.2), a raw pre-typecheck write in
   `replaceStrings` (§1.3), and a **disk-cache key-soundness hole**: two
   bytecode-changing env gates are missing from the cache-key fingerprint and
   the lint that supposedly enforces the list does not check it (§1.4) — which
   also puts one recent A/B measurement in doubt.

2. **A new CPU cluster the audit under-weighted: closure/thunk construction
   cost, not just count.** Every upvalue capture is a separately *dispatched*
   push opcode (plausibly 10–16 % of all executed ops, §2.1); every thunk
   minted under `with` captures the entire with-chain even when its body
   cannot perform a with-lookup (§2.2). Both are emit/lower-side, byte-identity
   testable, and attack the measured 52 %-trivial-op / 13–17 %-GET_UPVALUE
   profile directly — these are the two highest-EV unmeasured CPU levers left.

3. **Env-sharing is a structural no-op that still taxes the hot path** (§3.1):
   its intern threshold (`nUp > 8`) effectively never fires on real captures
   (average nUp ≈ 1–2), so the default-on feature shares ~nothing while adding
   a dead 8-byte field per Closure, a branch per upvalue read on the #1 opcode
   family, and a call per creation. Its ship falsifier should be re-run; the
   feature should pay or go.

4. **The architectural answer to "why can't we beat TW", grounded in both
   codebases** (§5): the TW in this repo is not the folklore naive walker — it
   has zero-allocation lambdas, one shared Env per scope, layered `//`, and
   freelist-cached 16 B Values. v3 inverted every cost polarity TW relies on
   (pay-at-creation captures vs pay-at-access Envs; out-of-line 24–40 B cells
   vs in-Value payloads; taxed mutator + gated collector vs zero-tax Boehm;
   fine-grained dispatch vs coarse composite visits) — in a lazy language
   where the dominant event is "allocate a deferred thing and never touch it
   again", v3 pays on the hot side of the distribution by design. The
   campaign verdict stands sharpened: the remaining gap is mostly these five
   load-bearing design choices, not a hidden bug.

None of the new CPU/RSS items individually closes the gap (consistent with
every prior campaign); §6 ranks them with pre-committed falsifiers. The
correctness items are unconditional fixes.

---

## 1. Correctness defects (new)

### 1.1 ✅ `withLookup` memo-writes through a dangling vector reference across re-entrant force — latent heap corruption on the DEFAULT path
`vm.cc:2322-2323` + `2455-2456`; capacity `vm.cc:13404` (`vm.withStack.reserve(64)`).

```cpp
for (size_t i = vm.withStack.size(); i-- > base; ) {
    Value & w = vm.withStack[i];          // reference into a std::vector
    ...
    try {
        w = forceValue(vm, w);            // re-enters dispatchLoop
```

`forceValue` runs the with-source thunk's body in a nested dispatch loop; that
body can execute `OP_WITH_PUSH` and call closures with captured withs — both
`push_back` onto `vm.withStack`. The vector is reserved to only **64** entries
(`ffi.cc:1102` likewise). The first time the with-stack crosses a capacity
high-water mark *inside* the nested force, the vector reallocates and `w`
dangles: the memoizing `w = forceValue(...)` writes 8 bytes into freed heap
and the following `w.isAttrs()/asAttrs()->lookup()` (`vm.cc:2466-2467`) reads
through it. Deep nixpkgs eval nests `with` scopes routinely, so >64 live
entries during a with-source force is plausible at scale; corruption is
silent. This is a candidate mechanism for the intermittent brute-audit flake
class. TW has no analog (with-scopes live in GC-allocated pointer-stable
`Env`s).
**Fix:** write back by index (`vm.withStack[i] = forced;`) after the force —
one line. The Slot branch (`vm.cc:2378`) copies and is safe.
**Tests:** a fixture forcing a with-source whose body pushes >64 with-scopes;
ASan run.

### 1.2 ✅ `valueLess` list writeback is missing its Phase-D barrier — the exact class `valueEqual` was already converted for (and primSort was fixed for)
`vm.cc:1295-1303`:

```cpp
Value & ai = a.asList()->elems[i];
...
ai = forceValue(vm, ai);   // raw write into (possibly tenured) ListVec storage
```

Twenty lines up, `valueEqual`'s identical writebacks use `cellWrite` with the
in-code PhD-6 comment (*"a raw `*t.aSlot = a` creates an unbarriered
tenured→nursery edge the scavenger misses — the confirmed class-3 missed
root"*, `vm.cc:1080-1086`). The comparison writeback at 1295-1303 (its own
comment says "mirrors the primElem fix" — it mirrored the memoization but not
the barrier) stores a possibly-nursery WHNF into a possibly-tenured list with
no barrier. Reachable via `<`/`<=`/`>`/`>=` over lists of unforced thunks
(list-of-list compares, `lib` sort paths).
**Fix:** `cellWrite(&ai, forced, nullptr)` mirroring valueEqual; failing-first
repro needs a LITERAL nested list (P1.2 lesson: map/genList elements are
tenured App pairs and won't trip it).

### 1.3 ✅ `replaceStrings` stores a forced non-string into a tenured list BEFORE the type check — missed-root window under `tryEval`
`primops.cc:2525-2528`:

```cpp
froms->elems[j] = forceValue(*state.vm, froms->elems[j]);
if (!froms->elems[j].isString())
    typeError("replaceStrings", "list of strings");
```

Happy path stores a String (GC leaf — fine). But when an element forces to a
non-string **nursery** value the raw store lands first, then `typeError`
throws; under `builtins.tryEval` the caller-visible list survives holding an
unbarriered nursery pointer, and the next exitDepth==0 scavenge invalidates
it. Same PhD-6 family as §1.2; narrow trigger, default path.
**Fix:** force into a local, type-check, then `cellWrite` the store (also
gives the barrier the happy path doesn't need but the error path does).

### 1.4 ✅ Disk-cache key fingerprint is missing two bytecode-changing gates; the claimed lint enforcement is fictional
`primops.cc:6736-6754` (`kGates[]`) vs `emit.cc:1115-1123` and `emit.cc:1372-1381`.

- `NIX_V3_RAW_FORMALS` (`emit.cc:1115`) conditionally emits `OP_RAW_FORMAL`
  into the code stream — **not in `kGates`**.
- `NIX_V3_NO_NONREC_ATTRS_INIT` (`emit.cc:1372`, a **default-ON** opt-out
  documented as "the A/B baseline + emergency mitigation", i.e. *designed* to
  be toggled) selects `OP_ATTRS_INIT` vs `REC_INIT` emission — **not in
  `kGates`**.

A CU compiled under either gate setting is stored under the same disk-cache
key as the other; any warm run after a toggled run silently loads
wrong-codegen bytecode — exactly the failure class the fingerprint exists to
prevent. The comment above `kGates` says `test/lint-cache-coherence.sh`
enforces the sync; that lint checks only descriptor-schema and deserializeCU
rules and never greps codegen gates, which is how both post-lint gates slipped
in. This class will recur until the lint actually checks it.
**Consequence for a shipped result:** the P2.1-a M5 A/B (git-noted on
`7840fd835`) is valid only if both arms ran cache-cold; with the gate absent
from the key, a warm gate-on arm would silently execute cached default-codegen
bytecode and measure nothing. The recorded "M5 = noise" conclusion is
plausible but should be re-confirmed cache-off if P2.1-a is ever revisited.
**Fix (hours):** add both gates to `kGates`; extend the lint to diff
`getenv("NIX_V3_*")` reads in emit.cc/opt_*.cc/lower_v3.hh/ir.cc against the
list (allowlist for dump/validate-only gates).

### 1.5 🔷 IFD eval-result cache replays results with no impurity tracking
Insert `primops.cc:7992-8014`, key = `"ifd-import" + path + narHash`
(`:7997-8004`); lookup `:7014-7063`. If the IFD-imported file's evaluation
consulted `builtins.currentTime`, `getEnv`, `currentSystem`, or read paths
outside the imported store path, the deep-forced result is persisted and
replayed across processes/days — nothing records the impure dependency.
Incidence is low for typical IFD outputs (pure functions of the store path);
`--impure` workflows and the AOT cross-machine snapshot raise the odds (a
baked `currentSystem` shipped to another machine is silently wrong). String
context IS correctly preserved (verified, `value_serialize.cc:226-245/383`).
**Fix direction:** taint bit on the VMState set by impure primops during the
cached eval; skip insert when tainted.

### 1.6 ✅ `derivationStrict` bytecode wrapper silently launders non-bool flags and coerces non-string outputs — semantic divergence from TW
`bytecode_primops.cc:956-959`: `asBool = v: v == true;` applied to
`__ignoreNulls`/`__contentAddressed`/`impure` — TW `forceBool`s these and
**throws** on non-bool; v3 treats any non-`true` as `false`
(`derivation { ... __ignoreNulls = 1; }`: TW type error, v3 builds a drv).
Similarly `outputsList = map builtins.toString args.outputs` (`:972-975`)
coerces where TW `forceStringNoCtx` rejects non-strings and contexted strings.
The in-code comment frames the laundering as deliberate defensiveness, but it
is a TW-parity break on user errors (the same class as the fixed P1.1 branch
opcodes). Fix in the wrapper source; needs negative lang fixtures.

### 1.7 Primops rooting-discipline sweep — 8 more S1.2-class sites + one new heap-vector site
🔷 The S1.2 campaign rooted filter/foldl/foldlMap/concatMap/partition/groupBy
but stopped there. Sites holding raw `ListVec*`/`Value` across re-entrant
calls with no GcRoot/re-read (grade: latent — safe today ONLY via the
exitDepth==0 gate + conservative C-stack pin, both slated for change by the
S2.1 safepoint work): **primAll/primAny** (`:1803-1841`), **primElem**
(`:2402-2432`), **primRemoveAttrs** (`:2201-2219`), **primConcatStringsSep**
(`:1471-1500`), **primReplaceStrings** (`:2525-2546`), **primListToAttrs**
src pointer (`:2063-2082`), **primSort** result/elements during the comparator
(`:9096-9125` — the P1.2 barrier runs *after* `stable_sort`; the mid-sort
window is closed only by the current trigger model). ✅ **primCatAttrs**
(`:2474-2481`) keeps a `std::vector<Value> kept` across per-element re-entrant
`forceValue` — the exact class of the 3 known unrooted-vector sites, not on
the known list (invisible to precise roots AND the C-stack scan).
**Disposition:** fold all into the S1.2 rooting backlog as a blocking
precondition for ANY change to scavenge gating; they are cheap, mechanical,
and the pattern (GcRoot + per-iteration re-read, or index-accumulation like
groupBy) is established. The full checked-clean coverage list (≈50 sites with
their barrier line numbers) is in the primops reviewer output; **every
allocBindings/allocList/allocPair fill site in primops.cc now has its barrier
or a proven-leaf element type — no primSort-class missing-barrier site remains
in primops.cc** (the two found this round are in vm.cc: §1.2, §1.3).

### 1.8 🔷 Mid-eval-GC safepoint forgets two caches (gated: `NIX_V3_MIDEVAL_GC=1 [+ _REUSE=1]`)
- **fakeClo pool**: `ClosurePool` (alloc.hh:3271-3287, thread-local, ≤16×128
  tenured Closure*) is unreachable from every root by recycle contract and is
  never cleared/marked at any GC safepoint (mark_sweep.cc: 0 refs). Under
  mideval sweep+reuse a pooled closure can be swept, its bytes re-allocated,
  and the same address later handed out by `tryPopFakeClo` → two owners.
- **String-context side table**: keyed by `const char*` payload; under
  sweep+reuse a recycled Chars address inherits the dead string's context
  entries (wrong drv closure). The safepoint clears four other ptr-keyed
  caches but not this one (this sharpens the known M-2 hazard from
  "theoretical reuse" to "the mideval safepoint specifically forgot it").
Both are pre-conditions for ever promoting mideval reuse; fix = add both to
the safepoint's clear list.

### 1.9 🔷 Latent walker/root items (no default-path bug today)
- **`Env::parent` is walked by NO walker** (declared `closure.hh:46-52`;
  walkEnv/mark/auditor all walk `values[]` only). Verified benign today — the
  sole allocEnv caller leaves `parent=nullptr` and nothing assigns it. The day
  chained Envs materialize (the field's documented purpose — and §5's
  recommended direction!), every walker silently drops the chain. Add a
  loud comment or remove the field until used.
- **Scavenger omits `walkEvalScopeRoots`** (FFI handle table; mark walks it) —
  UAF for any production embedder registering handles (test-only today,
  documented at `ffi.cc:854-856`).
- **Scavenger soundness w.r.t. C++-stack `GcRoot`s is convention, not
  contract**: sound today because GcRoots exist only inside primop C-frames
  where a scavenge cannot fire; the one same-vm `frames.empty()` re-entry
  window is covered only for print/toJson deep-force. **No assert** enforces
  "no live GcRoot at scavenge" — any new empty-frame force caller silently
  re-opens the stale-callee class. Add the assert (cheap, definitive).
- **env-intern table entries are barrier-covered only transitively**
  (`maybeInternFromStack` never calls `envPostConstructBarrier`,
  vm.cc:660-665); safe via every consumer's closure/thunk post-construct scan
  today — one new consumer away from a missed root. One-line barrier at
  creation recommended.
- 🔷 Compiler: **eager-forced-let / strict-arg inlining can reorder which
  error escapes** (`opt_strict_call_unthunk.cc:676-687` etc., default-on):
  value semantics preserved, but error-identity/divergence-order can change
  (`throw` → non-termination in a `seq`-adjacent corner). Not caught by
  byte-id suites (success paths identical). Low observability; document or
  gate the transform on effect-free prefixes.
- 🔷 Error-parity minors: `OP_ELEM_AT` reports "not a list" for a non-int
  *index* (`vm.cc:12840-12842`); STR_CONCAT `__toString`/`outPath` unwind
  capped at depth 8 vs TW unbounded (`vm.cc:12083`); C `primGroupBy` accepts
  contexted keys where TW `forceStringNoCtx` rejects (`primops.cc:4184` —
  masked while the bytecode form is default; fix together with §2.6).

---

## 2. CPU — new defect inventory

The profile these must explain (unchanged): ~52 % trivial stack ops,
OP_GET_UPVALUE 13-17 %, DISPATCH 7-23 %, ALLOC ~20 %, thunks 62-67 % never
forced.

### 2.1 ✅ Every upvalue capture is a separately DISPATCHED push — fold captures into the MAKE_THUNK/MAKE_CLOSURE trailer
`emit.cc:1091-1094` (Lambda), `1106-1126` (MkThunk): one `emitVarRef` per
lexicalWith + per freeVar before the MAKE op; runtime pops them in a loop
(`vm.cc:5912`-region). Every capture therefore costs a full dispatched
GET_LOCAL/GET_UPVALUE (fetch+decode+switch+poll) plus a pop — for data whose
source (slot vs upvalue + index) the emitter knows statically. OP_MAKE_THUNK
already reads two trailer words; extending the trailer to nUp descriptor words
(`kind|idx`, the encoding `tryEmitRPrimop2` already uses) lets the handler
copy directly from frame/closure storage with **zero dispatches per capture**.
At OP_MAKE_THUNK = 7-8 % of executed ops × ~2 avg captures, capture pushes are
plausibly **10-16 % of ALL executed ops**, and they are precisely the
GET_UPVALUE/GET_LOCAL population — i.e. a large slice of the 13-17 %
GET_UPVALUE share is closure-construction *forwarding*, not body reads; with
62-67 % of thunks never forced, most of it is dead work. This is the
highest-EV unmeasured CPU lever in this review.
**Falsifier first (cheap):** an emit-time counter splitting GETs emitted
inside capture sequences vs body reads → confirms/denies the 10-16 % before
any build. **Serialize note:** the new trailer needs remap treatment like
REC_SET slots (`serialize.cc:576-614` precedent) — slot/upvalue indices are
process-independent, so no P3.3-class footgun, but the walkers/disasm/CU
comparator must all learn the trailer length (the P3.3 adversarial lesson).

### 2.2 The `with`-capture cluster — three compounding defects
- ✅ **Dead with-chain capture (lower side):** `thunkify`
  (`lower_v3.hh:759-761`) runs `collectLexicalWiths()` unconditionally —
  every thunk/lambda minted under an active `with` captures the whole chain
  even when its body contains **no** WithLookup. The gated OP_RAW_FORMAL
  comment (`emit.cc:1112-1114`) states the general fact plainly: the captured
  withs "are DEAD for a `param.X` body (no with-lookup)" — but nothing trims
  the general case (no pass touches `lexicalWiths`). A per-function
  "body-contains-WithLookup" bit lets lower/emit clear the list; side
  benefits: the with-target vars stop inflating ancestor freeVars, and dead
  with-target thunks become DCE-able.
- ✅ **Spurious runtime snapshot (VM side):** `vm.cc:5750-5752` — a thunk with
  `nWiths==0` created while ANY runtime with is visible takes the
  `willHaveWiths` fallback: reserves the tail slot and runs
  `snapshotCurrentWiths` (hash+probe; n>1 always allocates a ListVec + barrier,
  never interns — `vm.cc:3090-3094`). This **contradicts** the
  MAKE_CLOSURE nUp==0 singleton path's stated guarantee ("bytecode-emitted
  lambdas with nWiths==0 are with-independent" → capturedWiths=nullptr,
  `vm.cc:5384-5394`). Either the guarantee holds and the fallback is pure
  per-thunk waste under `with lib;` (FP-2b measured 26 % of firefox thunks
  carrying non-null capturedWiths; HNE 544 K capWiths allocs/12.8 MB
  pre-intern), or it doesn't and the singleton path is unsafe. **Both cannot
  be right — resolve the contradiction first** (one falsifier: assert
  body-has-no-WithLookup on the fallback path under brute).
- 🔷 **Per-occurrence with-lookup rescans:** `withLookup` (`vm.cc:2314-2469`)
  walks the visible with-stack per occurrence — per entry: Slot-chase
  apparatus (thread-local visitedBuf clear + O(chain²) scan + two magic-static
  gates INSIDE the loop, `vm.cc:2356/2371`), try/catch-wrapped force, full
  binary search — with failed searches at every outer level and **no IC/memo**
  (the attrSelect IC pattern was never applied here). TW resolves the with
  *level* at parse time. An IC keyed on (with-stack epoch, symbol), or
  lower-time level pre-resolution, collapses it.

### 2.3 🔷 N-ary consumers defeat the operand-defer machinery — SET+GET pair per operand
`emit.cc:351-368`: fast paths exist only for unary/binary shapes; every
≥2-operand consumer with no fast path (`ConcatStrings` 1337-1343, `ListExpr`
1323-1327, `PrimOpCall` 1993-1995, demoted ATTRS_INIT 1404, AttrSetDyn
1572-1576, CALL_N spine head 951-956) flushes ALL pending defers
(n × SET_LOCAL) then re-loads each (n × GET_LOCAL) — ~1.5n extra ops after
peepholes vs 0 in ideal stack discipline (`"${a}-${b}"`: ideal 4 ops, actual
~9). Producer order == operand order for all these constructs, so a
generalized "pending tail matches operand suffix → pop k" matcher should hit
at high rate. Direct feeder of the 52 %-trivial-op mass; systematic
generalization of the known single-item finding.

### 2.4 ✅ OP_CALL_N / OP_TAIL_CALL_N re-introduce the synchronous callee-force A12b removed from OP_CALL
`vm.cc:7819-7820` (+`7892-7893`): `if (fun.tag()==Slot || Thunk) fun =
forceValue(vm, fun);` — (a) a Suspended-thunk callee pays a C-recursive
nested dispatchLoop (the multi-KB C-stack class the A12b comment at
`vm.cc:6169-6180` documents as the depth-5000 SIGSEGV root cause), and (b) on
the hot tail-recursion shape the comment itself names ("the recursive tail
callee `go` is a Tag::Slot"), **every loop iteration** pays a full
out-of-line forceValue merely to re-resolve the same immutable slot to the
same Closure. An inline 1-hop Slot→Closure deref (mirroring OP_CALL's
Evaluated-thunk hop at `vm.cc:6188-6199`) removes ~all of it.

### 2.5 Formals-call validation — two new specifics on the known-open item
- ✅ **Leftover "TEMP" P2.1-a sizing probe on every formals call**
  (`vm.cc:6933-6943`, duplicated at the TAIL_CALL site): walks ALL formals +
  two V3_STATS bumps per call, live in the shipping build (v3_release=false is
  the keep-decision, so V3_STATS is always compiled in). The measurement it
  served is complete (WS-2 handback). **Pure deletion** — the exact
  instrumentation-creep class WS-0 existed to stop, added after WS-0's sweep.
- 🔷 **TW's extra-arg check is O(1); v3's is O(m·log f) on every successful
  call** (`vm.cc:6976-7081` `b->forEach` + per-attr binary search vs TW
  eval.cc:1646-1663 `attrsUsed != size()` compare, scan only on the error
  path). v3's missing-arg loop already probes each formal — counting presence
  there and comparing sizes reproduces TW's trick with **no merge-scan
  needed** (supersedes the audit's P3.2-merge-scan sketch with a cheaper fix).

### 2.6 ✅ `groupBy` ships the documented-quadratic bytecode form on a stale justification — flip candidate
`bytecode_primops.cc:470-479`: the default groupBy is a bytecode
`foldl' (acc: x: acc // { ${key} = (acc.${key} or []) ++ [x]; })` — the file's
own filter/sort post-mortems call this the quadratic antipattern (full
accumulator copy per element; plus an or-default thunk per element, §4.3
class). The stated reason for staying bytecode ("C primGroupBy deepForceList
force-evaluates list ELEMENTS", `:322-332`) is **no longer true**: current C
`primGroupBy` (`primops.cc:4162-4208`) has no deepForce, passes elements to
the key function unforced, buckets indices under GcRoots — the comment's own
"could get the same fix as a follow-up" appears already done. Flip to C
default (with §1.9's NoCtx fix) after the throw-laziness A/B
(`groupBy (x: throw ...) …` and throw-in-element cases vs TW) + full brute.
Cost today: quadratic on every large `lib.groupBy` in nixpkgs eval.

### 2.7 ✅ `attrNames` (+ every realized mapAttrs entry name) heap-copies already-interned symbol strings per call
`primops.cc:786-788`: `mkStringValueOwned(symTab[sid])` → allocChars+memcpy
per name per call; TW's `prim_attrNames` is zero-copy
(`Value::toPtr(symbols[i.name])`). Same pattern in `makeMapAttrsNameValue`
(`value.cc:130-144`) which fires per *realized* mapAttrs entry — the module
system realizes these at scale. Arena chars are never freed → repeated
attrNames over large sets leaks copies into permanent RSS and burns ALLOC
CPU. Fix needs a stable per-SymbolId chars pool (the global symbol table's
std::strings are SSO/growth-unstable); one pool entry per symbol, shared by
all attrNames/mapAttrs-name materializations. CPU+RSS, both bounded but hot.

### 2.8 🔷 Attrs deep-equality: full name-walk + eager MapAttrs realization + heap vector before the first value compare
`vm.cc:1181-1234`: any chain-involved compare cursor-walks BOTH chains
completely (realizing an App3 per visible MapAttrs entry) and buffers into a
per-compare heap `pending` vector before comparing a single value; the flat
arm eagerly realizes too. TW's `eqValues` interleaves and stops at first
mismatch, zero side allocation. The shipped P3.4 scalar fast path does not
touch Attrs==Attrs. Platform-comparison shapes (`hostPlatform ==
buildPlatform`) run per-package. Counter first, then interleave + lazy
realization.

### 2.9 ✅ Interpolated Path parts compute the store path TWICE
`vm.cc:12251-12264`: the context entry (`ffi::coercePathToStoreName`) and the
payload (`coerceToString` → `ffi::coercePathToStore`) are two independent FFI
round-trips into copy-to-store machinery per `${./path}` part — each computes
the store path (content hash of file/dir). Compute once, derive both; also
removes a latent two-sources-of-truth coherence hazard.

### 2.10 Smaller verified items (bundle-scale)
- ✅ `OP_APPLY_OVERRIDES` emitted unconditionally after every LetRec
  (`emit.cc:1874` — comment says "if the rec contains it", no check exists);
  handler pays peek + chain-aware binary search per runtime construction of
  every cyclic let/rec. Entry names are static at emit time — skip unless an
  entry is literally `__overrides`. ~5 lines.
- 🔷 OP_CALL shape dispatch tests primop/PAP/functor/intrinsic/selector cold
  shapes before the dominant plain closure (`vm.cc:6251→7112`); TW tests
  `isLambda` first. Reorder behind a fused desc-flag predicate. Low
  single-digit ceiling (branches predict well).
- 🔷 SELECT_DYN / HAS / HAS_DYN / GET_UPVALUE_REC_BINDING still use
  C-recursive `forceValue`, not the A8 iterative protocol
  (`vm.cc:10758-11622` sites) — the remaining inconsistency in that
  conversion.
- 🔷 mergeByCursor runs two complete 2-cursor sweeps (count, then fill) per
  chain-involved merge, and the chained-RHS rescue's realizing `forEach`
  allocates an App3 per visible MapAttrs entry merely to copy it
  (`vm.cc:1931-2014`). A non-realizing name-only cursor for the count pass is
  free.
- 🔷 `primConcatStringsSep` builds result with no `reserve` (TW reserves
  `(n+32)·|sep|`); `primRemoveAttrs` allocates an unordered_set per call for
  1-4 names; `readFile` standalone path triple-copies via stringstream.
- 🔷 GC micro (bounded by GC ≤7 %): double `shouldScavenge` per trigger
  (vm.cc:4097 + gc.cc:1755) with the threshold re-multiplied per dispatch
  iteration (nursery.hh:306); per-scavenge fresh unordered_sets that belong in
  the persistent ScavengeBuffers; `isPhaseEActive()` tested 2-4× per fwd* on
  the default path; walkClosure/walkThunk's per-first-seen-CU IC scan is a
  Step-7 no-op but still pays the hash insert + kWays iteration; the
  post-scavenge full used-region memset (≤~24 MB) runs even in
  `NIX_USE_BOEHMGC=0` builds where its false-pin rationale vanishes.
- 🔷 New mechanistic explanation for the 21.9 % nursery hit-rate: when the
  nested-distinct-VM defer fires repeatedly, the STRESS countdown parks and
  the nursery sits ≥trigger for the whole nested phase — every eligible alloc
  bypasses to tenured; no trigger exists anywhere at exitDepth>0
  (grep-verified). This is the §5.2-known bypass, now with its precise
  parking mechanism.

---

## 3. RSS — new defect inventory

### 3.1 ✅ Env-sharing default-on is a structural no-op that taxes every closure and every upvalue read — re-run its ship falsifier
`vm.cc:594-599`: `if (nUp <= 8) return UINT32_MAX;` — the intern threshold
(added to stop a 9.9 MB python3 bring-up regression) excludes effectively ALL
real captures (avg nUp ≈ 1-2; M5's 38.5 B/thunk average implies nUp≈1). So
the default-on feature: (i) keeps a dead 8 B `upvalEnv` in every Closure
header; (ii) adds a data-dependent null-branch to every upvalue read
(`closure.hh:91` — on the 13-17 % opcode family); (iii) calls
`maybeInternUpvalueEnvFromStack` per nUp>0 creation just to early-return (the
known §3.11 item, now with the reason); (iv) leaves the "THUNK-half forceValue
lever" (fakeClo shares the thunk's Env) inert for ~100 % of thunks; (v) the
`__builtin_expect(shareUpvalues, 1)` hint at vm.cc:5448 is inverted vs
reality. Per Rule 0 this gate must pay or retire: either lower the threshold
and measure the win it was built for (with the §1.9 env-intern barrier first),
or remove the dual layout and reclaim the field + branch.
**Falsifier:** nUp histogram (one counter) → share-rate at threshold 2/4/8;
then A/B either direction on darwin-4.

### 3.2 🔷 Closure header diet — the FP-2b treatment was never applied to Closure (40 B → 16 B + flag-gated tails)
`closure.hh:60-83` (verified 40 B): `cu` derivable from `desc->cu` (known);
`capturedWiths` null for 74-97 % (the same measurement that justified FP-2b
for thunks) → flag-gated tail slot; `upvalEnv` → flag-gated tail slot (or
deleted per §3.1); `nUpvalues` duplicates `desc->nUpvalues` for real closures
(only fakeClos repurpose it — re-home the `_pad` magic in a flags byte).
16 B header ⇒ −16..−32 B/closure after rounding (48→32 at nUp≤1). Order
20-80 MB at firefox/M5 closure counts + one less cache line per closure
touch. Serialize untouched (headers are runtime-only).

### 3.3 ✅ `refill()` permanently strands the active block's tail — up to 4 MB per 16 MB block, on top of "never munmaps"
`alloc.hh:1570` + `2676-2705`: any allocation ≤ kHugeCutoff (4 MB) that
doesn't fit the current block's remainder abandons that remainder forever (no
secondary cursor, no tail retry, no production free-list). Large Bindings are
routine at nixpkgs scale (a 65 K-entry flatten ≈ 1 MB, minted every 16
merges), so blocks retire with 100 KB-4 MB unused. Realistic expectation
avg(large-alloc)/2 per block ⇒ order 1-5 % of arena (15-75 MB on M5); worst
case 25 %.
**Falsifier (one line):** count bytes abandoned at refill; if material, keep a
small bounded list of retired-block tails and try them before refill
(allocation-only change, no reclaim semantics, no GC interaction).

### 3.4 🔷 Bindings: dropping dead `aux` is a 16 B/object win (not 8) — rounding interaction
`alloc.hh:177-183` + kAlign 16: header 24 B ⇒ 24+16N ≡ 8 (mod 16) ⇒ **every**
Sorted Bindings pays +8 B pad on top of the 8 B `aux` that is dead outside
MapAttrs kind. A 16 B header (aux → MapAttrs-only side/tail) removes both:
−16 B × Bindings count with **no kAlign change** (the falsified kAlign 16→8 is
not needed for this). At Bindings = 84 % of arena and millions of objects:
order 30-100 MB on M5. Refines the known §5.8 Kind-split item with the
correct math and a cheaper scope.

### 3.5 🔷 Chain-extend copies the literal RHS — every `a // { … }` allocates two Bindings, one instantly dead
`vm.cc:9456-9462` (OP_ATTRS_INIT builds the RHS: sort + dup-check + alloc +
barrier) then `vm.cc:1917-1922` (mergeBindings copies its entries into a fresh
chain-leaf FAM and drops it). The copy is what makes the leaf privately
writable (C-1), but for a literal RHS the intermediate is avoidable: an
emit-detected fused `OP_ATTRS_UPDATE_INIT` (ATTRS_INIT immediately followed by
ATTRS_UPDATE) can build the chain leaf directly from stacked values —
**keeping the load-bearing runtime sort** (P3.3 lesson untouched) while
skipping one alloc+copy+barrier per `//`-with-literal. header+16·nb dead per
merge in a never-reclaiming arena; ties to the 584 MB HNE UPDATE bucket.
Measure literal-RHS fraction of UPDATE first.

### 3.6 🔶 The ALLOC ≈20 % CPU bucket partly IS the RSS pathology: first-touch page faults
Because the arena never reuses pages (known) and strands tails (§3.3), every
allocated byte is a first touch of a kernel zero page — M5's ~1.5 GB arena ≈
100 K+ soft faults charged to the allocation path, where TW's Boehm recycles
already-faulted pages after collections. Mid-eval free-list reuse was
measured against *peak RSS* (null) — its **CPU** effect via fault avoidance
was never the measured endpoint. Cheap falsifier: `getrusage` minor-fault
count A/B (default vs `NIX_V3_MIDEVAL_GC=1 _REUSE=1`) on firefox, no build
needed.

### 3.7 🔷 Warm-path retention specifics (firefox warm 586 MB vs TW 358)
SQLite page cache has no `PRAGMA cache_size` cap (disk_cache.cc:287-288;
prior HNE decomp attributed ~270 MB to SQLite); in-memory IFD `cache.results`
retains deep-forced full result Values for process life bounded by entry
COUNT not bytes (`primops.cc:6441-6499`); the LRU `last_used` column +
indexes are maintained on every insert for an eviction that no longer exists
(statements dead since P-12, disk_cache.cc:298-312 vs 409-414) — the DB grows
monotonically. Also 🔷 one full blob memcpy per CU disk hit
(`disk_cache.cc:405-408`, distinct from the known SQLITE_TRANSIENT bind
copies), and every warm process recompiles all bytecode primops + the root
expr with zero disk-cache integration (`run.cc:182-185`, `1725-1745`) —
per-process ms-scale, matters for short-lived CLI invocations.

---

## 4. Warm path — the definitive answers

### 4.1 ✅ The flake path does NOT bypass the disk cache — and the WS-2 "importCache is blind to flake CUs" explanation was wrong
Verified end-to-end: `primGetFlake` → `ffi::lockFlakeAndRead` →
`callFlakeV3`, whose `call-flake.nix` closure is compiled once per process
(`g_cachedCallFlake`); each flake loads via **plain `import`**
(`src/libflake/call-flake.nix:65`) → normal `primImport` → in-memory results
+ CU disk cache + IFD result cache. **M5 warm does not pay parse+lower.**
The WS-2 handback's explanation for `dumpFormalWrapperStats` reading 0 on M5
is corrected: (a) `isFormalWrapper`/`isOrDefault`/`isInheritWrapper` are
deliberately NOT serialized (closure.hh:472-485 "CACHE-COHERENCE-EXEMPT") —
any disk-cache-hit CU reports 0; (b) the stats walk covers
`importCache().cus` + a null `entryCu` and misses only the handful of
call-flake/bytecode-primop CUs. Consequence: **descriptor-flag-based
instruments are silently blind on ANY warm run** — cache-off is a validity
precondition for all of them; note this on the instrument.

### 4.2 🔷 The warm 2.43× gap is VM execution, not cache machinery
Eval-result caching covers ONLY IFD imports (deliberate, RCA'd); all ~1100
firefox literal-path imports re-EXECUTE warm (CU hit skips parse+lower only).
The measurable cache-consumption overhead found (per-IFD bridge+store
roundtrips before any cache check — `realisePath` is paid even when the
in-memory cache would hit, `primops.cc:6842-6851`; blob copies; per-process
primop recompile) is small single-digit %. Confirms the JIT-RCA framing: the
production gap lives in §2/§5, not in caching.

### 4.3 Cache soundness verified good otherwise
Blob self-validation (magic + schema + opcode-table FNV), composite (key,
schema) PK, content+path keying, per-hit mtime+size revalidation of in-memory
results, deserialized CUs are NOT second-class (intrinsics/selectorSym
round-trip; ICs cold by design and cleared at gen-major anyway) — all checked
clean. The two real holes are §1.4 (key fingerprint) and §1.5 (impurity).

---

## 5. Why TW wins — the architectural spine (both codebases read)

**Correction to folklore first:** the TW in this tree is modern upstream —
16 B niche-tagged Value (verified `alignas(16)`, tag in pointer alignment
bits), **zero-allocation lambda values** (`ExprLambda::eval` = `v.mkLambda()`
= two stores, no heap cell), one shared Env per scope (8 B + 8/slot,
freelist-cached), `maybeThunk` trivial-elision, **layered `//`** (UpdateQueue
collapses chains; rhs ≤16 entries = 24+16·rhs layer, NO full merge-copy), and
`GC_malloc_many` freelists. v3 is competing against a heavily tuned AST
interpreter.

Verified per-construct byte accounting (headers + kAlign-16 rounding):

| Construct | TW | v3 | note |
|---|---|---|---|
| scalar attr/list slot | 16 B Value cell | 8 B inline | **v3 wins** |
| thunk (nUp) | 16 B (+ shared Env amortized) | 32 B @ nUp≤1, 48 @ 2, 64 @ 4 | ~2-3× |
| closure (nUp) | **0 B** (mkLambda) | 48 B @ nUp≤1, 64 @ 2-3 | ∞ ratio; the single biggest missing elision |
| App / PrimOpApp | 0 extra (in-Value) | 8 + 32 B ValuePair | 2.5× |
| attrset N | 24+16N + N×16 B value cells | 32+16N inline | headers parity; v3 wins on scalars, loses via pointed-to cells |
| `//` intermediate | 24+16·rhs transient (Boehm reclaims) | full chain-leaf copy, **permanent** (arena) | §3.5 |
| string context | inline ptr, free when absent | global hash side-table | §5.6 known + §1.8 |

**The seven ranked reasons (each verdict-tagged):**
1. **Creation/access polarity inversion (flat transitive capture)** — TW:
   closures free, thunks 16 B, one shared Env per scope, cost paid at
   *access* (Env-chain hops); v3: per-object cells + per-capture dispatched
   push + copy re-paid at every nesting level, cost paid at *creation* — the
   wrong side when 62-67 % of thunks are never forced. QUESTIONABLE-CHOICE
   (the concrete alternative inside a bytecode VM is env-pointer/display
   capture); within the shipped design, INHERENT. §2.1/§2.2 are the
   recoverable emit-side slice.
2. **Dispatch granularity below work granularity** — TW dispatches once per
   composite AST node (ExprAttrs builds a whole Bindings per visit); v3
   executes 36.9 M opcodes on firefox, ~52 % trivial, each paying
   fetch/decode/switch/poll around a 1-3-instruction payload, plus NaN-box
   decode per stack-op tag check (TW checks per node). INHERENT to
   switch-dispatch bytecode at this op granularity (superinstructions
   measured neutral; JIT ceiling 1.5-2.2× still >1× TW).
3. **GC tax with no mid-eval payoff** — per-alloc metadata + per-object
   barriers + per-op polls, while collections are exitDepth-gated and rarely
   fire (2 scavenges/firefox; §2.10 parking mechanism); TW's Boehm costs the
   mutator ~nothing and actually reclaims. QUESTIONABLE as configured; the
   gate is INHERENT to moving-GC-under-C++-recursion (the S2.1 safepoint work
   is the escape path, and §1.7/§1.9 are its preconditions).
4. **Non-reclaiming arena × materializing merges** — merge intermediates (83 %
   of arena Bindings on HNE) become permanent RSS where TW's layered `//` +
   Boehm make them transient. QUESTIONABLE-CHOICE in combination; §3.3/§3.5
   are the allocation-side dents; real fix = a reclaiming heap (all
   arena-side reclaim levers measured dead).
5. **Per-formal wrappers + O(m·log f) per-call validation** vs TW's 1 Env +
   O(1) count — FIXABLE (partially shipped/gated; §2.5's attrsUsed trick is
   the cheap half).
6. **Representation moved out-of-line, not shrunk** — the 8 B Value pushes
   every deferred payload into 24-40 B rounded cells (table above); TW's
   982 MB M5 RSS is less than v3's non-arena overhead alone. INHERENT to the
   8 B-word design; §3.2/§3.4 recover the header slack.
7. **Global string-context side table** vs TW's inline context pointer —
   FIXABLE (context-carrying Chars variant), known W-scale.

**maybeThunk-inventory gap worth naming:** v3 reproduced TW's thunk-COUNT
elisions (#135 confirmed parity) but not the UNIT-COST elisions — zero-alloc
closures, shared inherit Envs, free cell-aliasing memoization. The wrapper
classes (formals/or-default/inherit) are v3-only inventions.

**Bottom line for the engineers:** TW wins on polarity, not tricks. v3's
remaining 1.8-2.4× warm gap decomposes into: capture-construction dispatch +
copies (§2.1/2.2, recoverable slice), dispatch granularity (INHERENT short of
JIT), GC mutator tax without reclamation (S2.1-dependent), out-of-line cell
weight (§3.2/3.4 dents; floor INHERENT), and the long tail above. A v3 that
beats TW is a different design on axes 1/2/3/6 — env-pointer capture,
register-machine or JIT execution, and either safepoint-complete moving GC or
Boehm-style zero-tax collection — which matches (and sharpens) the campaign's
"fundamental architectural change" verdict.

---

## 6. Priority matrix (new items only; judge against the P0.4 baseline)

Legend: effort H/D/W; BI = byte-identity gate (full `--brute`); every D/W item
starts with its falsifier (Rule 0).

| # | Item | § | Class | Effort | Falsifier / exit criterion |
|---|------|---|-------|--------|---------------------------|
| Q1.1 | withLookup write-by-index fix | 1.1 | correctness | H | >64-with fixture + ASan; brute |
| Q1.2 | valueLess cellWrite barrier | 1.2 | correctness | H | literal-nested-list repro, audit flags pre/0 post |
| Q1.3 | replaceStrings force-check-store order | 1.3 | correctness | H | tryEval repro under 1 MB nursery audit |
| Q1.4 | kGates += RAW_FORMALS, NO_NONREC_ATTRS_INIT; lint actually checks | 1.4 | correctness | H | toggle-then-warm test loads correct bytecode |
| Q1.5 | derivationStrict asBool/outputs parity | 1.6 | correctness | H | negative fixtures == TW errors; BI on valid |
| Q1.6 | S1.2 rooting backlog (8 sites + catAttrs vector) | 1.7 | correctness-latent | D | blocks any scavenge-gating change; audit-clean under stress |
| Q1.7 | mideval safepoint clears fakeClo pool + ctx table | 1.8 | correctness-gated | H | reuse-stress repro |
| Q1.8 | GcRoot-at-scavenge assert + env-intern barrier + Env::parent guard | 1.9 | hardening | H | assert never fires on brute |
| Q2.1 | **Capture descriptor-trailer** (kill per-capture dispatch) | 2.1 | CPU | D-W | step 0: emit-time counter — capture-GETs ≥8 % of ops → build; <4 % → close |
| Q2.2 | **with-capture cluster**: WithLookup bit + lexicalWiths trim + resolve the willHaveWiths contradiction | 2.2 | CPU+RSS | D | step 0: count fallback snapshots + dead-with captures; ≥3 % of thunk-creation cost → build |
| Q2.3 | N-ary defer suffix-match | 2.3 | CPU | D | emitted-op count on fixtures; darwin-4 |
| Q2.4 | OP_CALL_N inline Slot→Closure hop | 2.4 | CPU | H | fold-heavy micro + firefox |
| Q2.5 | DELETE the TEMP formals probe; attrsUsed-style O(1) extra-arg check | 2.5 | CPU | H | pure deletion + callPackage-heavy eval |
| Q2.6 | groupBy → C default (+NoCtx fix) | 2.6 | CPU | H-D | throw-laziness A/B vs TW; large-groupBy timing |
| Q2.7 | Symbol-chars pool for attrNames/mapAttrs names | 2.7 | CPU+RSS | D | alloc counter on module-heavy eval |
| Q2.8 | Equality interleave + lazy realization | 2.8 | CPU | D | compare-counter first |
| Q2.9 | Path-part single coercion | 2.9 | CPU | H | path-interp micro; BI (context!) |
| Q3.1 | Env-sharing: pay or retire (threshold A/B after intern barrier) | 3.1 | CPU+RSS | D | nUp histogram; then darwin-4 A/B both directions |
| Q3.2 | Closure header 40→16+tails | 3.2 | RSS | D-W | closure count × Δ; BI |
| Q3.3 | refill tail counter → bounded tail-list | 3.3 | RSS | H then D | abandoned-bytes counter ≥30 MB M5 → build |
| Q3.4 | Bindings 16 B header (aux out) | 3.4 | RSS | D-W | Bindings count × 16 B; BI |
| Q3.5 | Fused UPDATE_INIT (literal RHS) | 3.5 | RSS+CPU | D | literal-RHS fraction of UPDATE first |
| Q3.6 | Minor-fault A/B (reuse as CPU lever) | 3.6 | CPU | H | getrusage counts; ≥5 % of ALLOC bucket → revisit reuse default |
| Q4.1 | Impurity taint on IFD result cache | 1.5 | correctness | D | currentTime-in-import repro |

**Sequencing.** Q1.* first (correctness, all cheap except Q1.6's breadth) —
Q1.4 before ANY further gated A/B. Then the two step-0 counters Q2.1/Q2.2
(hours, decide the only two plausibly-material CPU builds left). Q2.5/Q2.6/
Q2.9 are safe quick wins alongside. Q3.1 is a decision the codebase owes
itself either way. Q3.2/Q3.4 fold into the existing foundational
"live-representation" program; Q3.3/Q3.5 are measure-first dents. Expect —
honestly — single-digit % from any one item; the §5 axes bound what the sum
can achieve without an architectural program.

---

## 7. Corrections to prior documents

1. **WS-2 handback (DEFECT_AUDIT):** "dumpFormalWrapperStats walks importCache,
   which is blind to M5's flake-eval CUs" — WRONG mechanism; flake CUs ARE in
   importCache (§4.1). Real causes: flags not serialized + warm hits. The M5
   P2.1-a A/B additionally needs the §1.4 caveat.
2. **07-02 audit §3.2 framing** ("chain SELECT walks up to 16 layers") was
   already corrected by the P3.1 measurement (avg depth 2.4/2.0); this review
   confirms no chain-IC revival is warranted and adds §2.8/§2.10-mergeByCursor
   as the surviving chain-related costs.
3. **"TW does a full merge-copy per `//`"** (used in several older docs as the
   baseline assumption): false for this tree — TW's UpdateQueue layers small
   RHS (§5). v3's `//` disadvantage is therefore larger than those docs imply.
4. **"v3 has leaner cells" (retired 2026-06-27) — now mechanically explained:**
   the 8 B Value moved payloads out-of-line into 24-40 B rounded cells (§5
   table); the per-construct accounting is the durable reference.
5. **07-02 audit §2.6 "stack slots zero-init to Float 0.0":** extended — the
   NaN-box maps ALL-ZERO words to Float 0.0 (`value.hh:126`), so every
   container relies on explicit Uninitialized stamping; nothing enforces the
   allocBindings "callers must fill all n entries" contract (§1-adjacent
   hardening note).

## 8. Coverage and honesty

Read in full this round: alloc.hh, value.hh, closure.hh, nursery.hh,
barrier.hh, value.cc, vm.cc (both halves, line-by-line), lower_v3.hh, ir.hh,
ir.cc, emit.cc, opt_strict_call_unthunk.cc, opt_stream_fusion.cc, gc.cc,
mark_sweep.cc walkers, disk_cache.cc, aot_cache.cc key paths,
v3_call_flake.cc, bytecode_primops.cc, ~60 % of primops.cc with a complete
alloc↔barrier cross-reference, plus TW's value.hh/attr-set.hh/eval.cc/
eval-inline.hh/primops.cc counterparts. Explicitly NOT examined (do not treat
as clean): primops.cc 6500-8130 fetch*/scopedImport bodies (barrier map only),
toXML/toJSON serializers, hashString/convertHash, filterSource internals,
derivationStrict native phases 4-7 vs TW edge cases, serialize.cc full
deserialize cost profile, value_serialize decode allocations, ~22 remaining
leaf-string elems-write sites in primops.cc, opt_beta_reduce/primop_fuse/
genlist_unroll/if_fold fire rates, parser internals, OP_WITH_LOOKUP dynamic
share (needs NIX_VM_OPCOUNTS), and every magnitude estimate above that lacks
a darwin-4 number — the step-0 falsifiers in §6 exist for exactly that
reason.

*Review executed 2026-07-03: 8 parallel fresh-lens subsystem reviews seeded
with the 07-02 known-findings list, synthesized + headline claims
hand-verified at HEAD `8ca498235`. Companion to `DEFECT_AUDIT_2026-07-02.md`
(inventory + handbacks) and `WHY_CPU_SLOWER_THAN_TW_2026-06-28.md` /
`V3_VS_TW_STRUCTURAL_ANALYSIS_2026-06-19.md` (superseded on the points in §7).*
