# Ideas to steal from Unison (2026-05-07)

A survey of Unison's design choices that map onto open problems in
v3, ranked by feasibility × leverage. Unison and Nix sit on opposite
sides of several axes (Unison is strict, statically typed, has
algebraic data types and abilities; Nix is lazy, dynamic, attrset-
based) — but several of Unison's *infrastructural* choices are not
language-specific and travel well.

The five ideas below are concrete enough to map to existing v3 files.
The two non-applicable directions are listed at the end so they
don't keep coming up.

---

## 1. Content-addressed IR fragments — highest leverage

### What Unison does

Every term in the Unison codebase is identified by a structural hash
of its AST plus the hashes of its free variables. Two
semantically-identical functions in different files have the same
hash. Names are metadata; the codebase is conceptually a
`Map<Hash, Term>`. Renaming a function doesn't break callers because
callers refer to the hash.

**References:** Unison docs `unison-lang.org`, "Why we chose
content-addressed code"; the `ucm` codebase manager; the
`unison-runtime` ABT representation.

### Where v3 is part-way there

`src/libexpr-v3/disk_cache.cc` content-addresses *compiled bytecode*
by source SHA-256. That's file-level content addressing — re-running
`nix eval` on the same `.nix` file finds the bytecode unit cached.

But this stops at file granularity. Two files that both contain
`let f = x: x + 1 in f 42` re-lower and re-emit independently. They
hit the cache only if the *whole file* is byte-identical.

### The Unison move applied to v3

Push content-addressing down to *IR fragments*. Hash-cons every
`Function` and `MkThunk` IR node by structural content (recursively
including the hashes of free variables). The disk cache key becomes
the IR-fragment hash, not the source SHA.

For nixpkgs the payoff is large: the same `mkDerivation`-shape
lambdas, the same `mapAttrs` callbacks, the same `lib.fix` invocations
recur across thousands of files. With fragment-level addressing,
each unique IR shape compiles once per machine, ever.

### Concrete mapping

- **Hashing:** add a `structuralHash()` on `ir::Function` /
  `ir::Lambda` / `ir::MkThunk` in `ir.cc`, computed bottom-up
  alongside the existing `computeFreeVars()` pass at `ir.cc:287–456`.
- **Cache key:** extend `disk_cache.cc` schema to key by IR-fragment
  hash in addition to source SHA. The opcode-table fingerprint
  (already in C5) stays as a salt.
- **Sharing within a module:** at lower-time, dedupe in
  `Module::functions` — two structurally-equal functions reduce to
  one entry. Already partly done by alias-collapse for VarRefs;
  generalise to whole-function bodies.
- **Sharing across modules:** the disk cache becomes a *content-
  addressed store for IR*, queryable by hash. Conceptually `/nix/store`
  but for compiled bytecode.

### Why this is the top pick

It pairs naturally with three things v3 is already pursuing:

- **Shapes (S1 from prior reviews).** Shape descriptors are
  themselves hash-cons targets — a shape is just a sorted symbol
  vector, hashable. Shape interning *is* content addressing applied
  to attrset structure.
- **The `lib.fix` intrinsic dispatch (#495).** Today this matches
  *canonical AST patterns* with a hand-written matcher (commits
  acd7bd96a, 45ba23f5d, a1b56910f). Content-addressed IR makes the
  match structural — you compare hashes, not patterns. Less code,
  more coverage.
- **The disk cache.** Already exists; the schema needs extending,
  not replacing.

### Risks

- Hash collisions. Use SHA-256 (already the cache hash) and accept
  the same threat model.
- Hash stability across compiler versions. Salt with the opcode-
  table fingerprint (already done for B8 / C5). When the IR shape
  changes, hashes change; the cache invalidates. Same story as
  today.
- Source-position metadata. Two structurally-identical functions
  from different files have different positions for error
  messages. Solution: positions live in a side-table keyed by
  call site, not by function. v3 already does this via the position
  side-table in `bytecode.hh:314–331`.

### Estimate

1–2 weeks. ~600 LOC, mostly in `ir.cc` (hashing) and `disk_cache.cc`
(schema). No semantics change; cache hit-rate goes up; cold-cache
eval gets faster.

---

## 2. ABT representation — alpha-equivalent identity

### What Unison does

Unison's runtime uses Abstract Binding Trees (ABTs) where bound
variables are de Bruijn indices. Alpha-equivalence is decidable by
structural `==` on the tree. This is what makes content-addressing
sound — you can't have two distinct hashes for what's morally the
same function.

**Reference:** Robert Harper's *Practical Foundations for Programming
Languages*, ch. 1; the `unison-abt` Haskell package.

### What this means for v3

`\x: x.foo` and `\y: y.foo` should be the same function. Today they
have different `VarId`s (module-unique counters in `ir.hh`) and are
treated as distinct. Selector thunks, function dedup, and CSE all
miss this.

### Concrete mapping

The IR is already largely there: A-normal form with `VarId` operands
(`ir.hh:287–303`). The change is to switch `VarId` from
"module-unique counter" to a de Bruijn-style `(depth, index)` pair
within each function's binding scopes.

Implications:

- Free-var capture lists become trivial: any `VarId` with `depth >
  current_depth` is free.
- Hash-cons (item 1) works without a separate alpha-renaming pass.
- Lambda equality is `==` on the IR tree.
- Selector lambdas (#424) and selector thunks (recommendation #2 in
  the optimizer report) automatically dedupe.

### Risks

This is the only structural refactor on the list. It touches every
pass that walks IR (~6 optimizer files + emit.cc + lower.cc). The
ABT change is mechanical but extensive.

### Estimate

~1 week refactor + test stabilisation. ~500 LOC churn. Prerequisite
for #1 to work cleanly; deferring it means hash-cons has to do its
own alpha-renaming, which is messy.

---

## 3. Hash-keyed evaluation cache

### What Unison does

Unison's runtime can cache the result of a pure function call by
`hash(function) + hash(inputs)`. Pure recomputation across runs is
free; dependent values short-circuit on cache hit.

### What v3 has

At the *build* level: derivation outputs are content-addressed and
substituted from caches.

At the *eval* level: **nothing**. Every `nix eval` re-runs every
pure subexpression from scratch.

### The opportunity

For primops marked `pure` in `PrimOpFlags`, and for user functions
provably pure (transitive flag propagation; see #4), cache the
result keyed on `(function_hash, input_hash)`. Expensive recursive
lib functions — `lib.systems.elaborate`, `extends`-chains, the
nixpkgs lib-fixed-points — would short-circuit completely on warm
runs.

This pairs with shapes: once attrset shapes are stable identities,
hashing an attrset is fast (it's the shape pointer + the slot
hashes, mostly already cached). Without shapes, hashing an attrset
costs O(n) per access — expensive enough to negate the win.

### Concrete mapping

- **Cache:** SQLite alongside `disk_cache`, or in-memory with bloom
  filter for the cold path. Key:
  `(function_content_hash, arg_content_hash)`.
- **Hit path:** before invoking a pure primop or pure user function,
  hash arguments, look up. Skip the call if hit.
- **Miss path:** invoke as today; on return, store the result.
- **Eviction:** LRU bounded; the cache is a perf optimisation, not
  load-bearing.

### Risks

- **`pure` correctness.** If a primop is marked pure but actually
  reads ambient state (env vars, settings, file system), the cache
  poisons. Mitigation: opt-in via `PrimOpFlags` with `RequiresPure`
  enforced at registration; default-off for user functions until
  ability propagation (#4) lands.
- **Hashing cost.** Hashing a deep attrset every call is itself
  expensive. Shapes (S1) make attrset hashing O(1) on the fast
  path. Without shapes, this is premature.

### Estimate

~1 week + cache infrastructure. Starts as opt-in
(`NIX_V3_EVAL_CACHE=1`); promotes to default-on after a profile-
driven validation period.

---

## 4. Abilities → propagated `PrimOpFlags`

### What Unison does

Unison's type system has row-polymorphic effects called *abilities*.
A function's type carries which abilities it uses
(`{IO, Throw, Random, Storage}`). Ability handlers interpret
effects; pure functions have an empty ability row.

**Reference:** Unison docs on abilities; the underlying theory is
algebraic effects (Plotkin/Power; Bauer/Pretnar's Eff).

### What v3 has

`PrimOpFlags` (`IMPURE`, `RESTRICTED`, `INTERNAL`, `EXPERIMENTAL`,
`NO_TRACE`) — a fixed bitset on each registered primop. Static at
registration. No propagation.

### The natural extension

Make flags propagate through user code:

- A lambda that calls an `IMPURE` primop is transitively `IMPURE`.
- A `let` binding's flag set is the union of its RHS's.
- An attrset's flag set is the union of its values.
- Forcing a thunk reveals the body's flags.

Once flags propagate, the entire pure-eval / restricted-eval gating
becomes a *static* check at lower-time rather than a runtime trap.

### What this unlocks

- **Static IFD (import-from-derivation) detection.** IFD has been a
  long-standing wish for static detection. As an ability —
  `RequiresStore` — it propagates: any function that transitively
  depends on `derivationStrict` is marked `RequiresStore`. Calling it
  in a context that statically forbids store access is a
  lower-time error with a precise call site.
- **Memoisation safety (#3).** Only functions with empty ability
  set (truly pure) are eligible for the eval cache.
- **Better error messages.** "This expression requires the store"
  with the chain of calls leading to the offending primop.

### Concrete mapping

- Each `PrimOp` already has `flags`. Add a structural `Effects`
  bitset to IR `Function` / `Lambda`, computed bottom-up alongside
  free-vars at `ir.cc:287–456`.
- `lower.cc` checks effects against the current evaluation mode
  (`pure-eval`, `restricted-eval`) at function definition site.
- `vm.cc` no longer needs to check at every primop call; the static
  check at lower-time replaces it.

### Risks

- **`with`-scopes** make static analysis hard generally; effect
  propagation is no exception. If a `with` introduces a function
  that's invoked dynamically, its effects can't be known
  statically. Cheapest answer: any `with` shadow taints the
  enclosing function with `Unknown` effect, which the dispatch
  treats as "could-be-anything".
- **Breaking change.** Today some users rely on impure primops
  silently working in pure-eval until they're called. Static
  checking is louder. Mitigation: opt-in initially via a settings
  flag.

### Estimate

~2 weeks. Requires occurrence analysis (recommended for the
optimizer anyway) as a substrate. The static-check version is
~500 LOC; the user-facing error-reporting story is the other half.
This is the most ambitious item on the list — it's a small but
real type-like layer added to a dynamically-typed language.

---

## 5. Hash-queryable disk cache

### What Unison does

`ucm` lets you ask "what's the source for this hash?" The codebase
is queryable by hash, decompilable, browsable. `view <hash>` shows
you the term.

### Maps to v3 trivially given items 1 and 2

If IR fragments are content-addressed, the disk cache is a
queryable index. Add a CLI:

```
nix v3-cache-stat <hash>          # is this hash cached?
nix v3-disasm <hash>              # show bytecode for this hash
nix v3-cache-deps <hash>          # what does this hash depend on?
nix v3-cache-search <pattern>     # functions whose source matches X
```

The infrastructure mostly exists: `disasm.cc` (212 lines) already
disassembles bytecode. The cache lookup is straightforward.

### Why this matters

Three weeks of debugging on cardano-node-class regressions has
repeatedly hit the same wall: "the cache says we're using bytecode
version X, but the source has changed; what fired?" Today the only
way to see is to delete the cache. With a hash-queryable index,
post-mortem analysis is fast.

### Estimate

~2 days after items 1 and 2 land. ~150 LOC. Pure ergonomics; cheap.

---

## What NOT to steal

These come up but don't fit Nix:

- **Strict-by-default evaluation.** Unison is strict. Nix's whole
  identity is laziness — the selector-thunk / lazy-black-hole /
  single-entry-thunk machinery in v3 is exactly what Unison
  *doesn't* have, and is exactly what makes Nix expressions express
  what they do.
- **Distributed runtime / Unison Cloud.** Nix evaluation is single-
  process; build distribution is a separate (already-solved)
  problem. Cross-machine compute movement isn't a Nix-language
  problem.
- **Type-driven program search.** Nix is dynamically typed; there's
  no signature to search by. The closest analog (find-by-attr-name)
  is a flake-output search, which is a different tool.
- **Pattern-match exhaustiveness for sum types.** Nix has no sum
  types. Attrset-with-tag conventions exist but aren't
  syntactically distinguished.
- **Algebraic effects with full handlers.** Item 4 above takes the
  *propagation* idea without taking the *handler* mechanism.
  Handlers would require user-level effect declaration, which is a
  major language change. Not worth it for a config language.
- **Content-addressed names as the canonical identifier.** Unison
  treats names as metadata; the canonical identifier is the hash.
  For a config language whose readers are humans, names matter as
  much as identity. Keep names; add hashes alongside.

---

## Ranked feasibility

| Idea | Effort | Leverage | Pairs with | Notes |
|---|---|---|---|---|
| 1. Content-addressed IR fragments | 1–2 weeks | **HIGH** | `disk_cache.cc`, shapes (S1), #495 intrinsic dispatch | Top pick. Natural extension of existing infrastructure. |
| 2. ABT-style alpha-equivalent identity | ~1 week | **MEDIUM**, **HIGH** as enabler | Item 1, selector thunks | Refactor; touches every IR walker. Prerequisite for clean #1. |
| 3. Hash-keyed evaluation cache | ~1 week + cache infra | **MEDIUM-HIGH** on warm runs | `PrimOpFlags`, shapes (for fast hashing), item 4 | Opt-in initially. Profile-validate before default-on. |
| 4. Effect / ability propagation through `PrimOpFlags` | ~2 weeks | **MEDIUM** | Existing `PrimOpFlags`, occurrence analysis | Static IFD detection alone is a long-asked-for win. Most ambitious item. |
| 5. Hash-queryable cache CLI | ~2 days (after 1+2) | **LOW** (ergonomic) | Items 1, 2; `disasm.cc` | Cheap; debugging payoff for cache-hit/miss diagnostics. |

---

## Strategic note

The cleanest sequence is **2 → 1 → 5 → 3 → 4**:

1. ABT identity refactor first — it's the substrate for everything
   else.
2. Content-addressed IR — the headline win, immediate cache-hit
   improvement.
3. Hash-queryable CLI — falls out of (1)+(2); useful for debugging
   the previous two.
4. Hash-keyed eval cache — needs (1)+(2) and shapes; pairs with
   primop purity.
5. Effect propagation — long horizon; sized to its own milestone.

That's a 4–6 week arc on top of the current FFI/optimizer track,
delivering one big architectural win (content-addressed IR) plus
the foundation for several follow-ons. Item 4 is the standalone
research direction — worth flagging now, scope later.

The unifying observation: Nix already *has* content-addressed
storage. The store, derivations, NAR hashing — Nix invented the
modern instances of these patterns. Applying the same idea to the
evaluator's own internals (IR fragments, compiled bytecode, eval
results) is "Nix philosophy applied to itself." Every Nix engineer
will recognise the shape of the win without further argument.

---

## Prior art and viability

The natural follow-up question: is content-addressed IR actually
viable, or is this a "novel-feeling" idea that's been tried and
quietly failed? Honest answer: the technique is well-trodden; the
risks are well-understood; v3's specific situation is unusually
favourable.

### Where the prior art clearly proves viability

- **Unison itself.** Production-ish use for ~5 years with content-
  addressed terms as a load-bearing design choice, not an
  experiment. The `ucm` codebase manager works; the runtime caches
  by hash; refactoring-by-hash works in practice. Existence proof
  is solid.
- **Hash-consing in proof assistants.** Coq's `Hashcons.ml` (~2
  decades old), Lean 4's bottom-up-hashed `Expr` representation
  (default-on), Agda's normalised forms. *Exactly* the IR-level
  content-addressing pattern, applied to dependent type theory
  rather than Nix expressions. The compile-time and memory wins
  are documented in their internals docs.
- **V8 CodeCaching.** Chrome ships a per-script bytecode cache
  fingerprinted by source content + V8 version. Default-on; every
  `<script src=...>` you visit hits it. Per-script, not per-
  function — coarser than the v3 proposal — but proves the
  schema-versioning + fingerprint-as-salt model works at internet
  scale.
- **GHC interface files (`.hi`) + ABI hashing.** GHC computes a
  hash of each module's exported ABI; downstream modules cache
  against the hash. If a module's internals change but its ABI
  doesn't, downstream modules don't recompile. Module-granularity
  content-addressing in production since the early 2000s.
- **`sccache` / `ccache`.** Compiler caches keyed on
  `(preprocessed source, compiler flags)`. Mozilla and many others
  run sccache cluster-wide. Distributed content-addressed
  compilation cache; battle-tested at scale.
- **Java Class Data Sharing (CDS).** OpenJDK ships compiled class
  metadata in a shared archive that multiple JVM processes mmap.
  Hash-keyed; default-on for system classes since JDK 12.

### Where the literature is honest about the cost

- **Hash-consing inside a single compilation gives modest wins.**
  Filliâtre & Conchon, *Type-safe modular hash-consing*
  (ML Workshop 2006) — the canonical reference. Their reported
  perf delta is in the 5–15 % range for compile time. So
  "hash-cons every IR node" alone is not transformative. The
  *transformative* wins come from caching *across* compilations,
  which is the v3 disk-cache extension story (item 1 above).
- **Unison's tooling investment was real.** Their content-
  addressing required building `ucm` from scratch to be usable,
  because the content-addressed view is hostile to standard Git
  workflows. Lesson: content-addressing the *internals* of an
  evaluator (the v3 proposal) is much cheaper than content-
  addressing *user-visible* code (Unison's choice). v3 doesn't
  need a `ucm`-equivalent because users never see the hashes.
- **Cache invalidation across compiler versions.** Every system
  that does this has had pain here. v3's existing opcode-table
  fingerprint (B8 / C5) is the right mitigation. GHC, V8, and
  sccache all use the same pattern: salt the hash with a compiler
  version + ABI fingerprint.

### Where the v3-specific case is unusually favourable

Three reasons this is *easier* in v3 than in most languages:

1. **The disk cache already exists.** `disk_cache.cc` is content-
   addressed by source SHA. Extending its key from "source SHA" to
   "IR-fragment hash" is a schema change, not a new subsystem.
2. **Nix culture accepts content-addressing as the default mental
   model.** Every Nix user already understands
   `/nix/store/<hash>-name`. There's no education cost.
3. **No user-facing exposure.** Unlike Unison, where the user sees
   hashes in `ucm`, v3's IR hashes are entirely internal. Cache
   miss → recompile, same as today. No UX cost.

### Where to be cautious

- **Don't over-promise the *first*-evaluation speedup.** Content-
  addressing primarily wins on *repeated* evaluation across
  processes/sessions and *cross-file* sharing. A single cold-cache
  `nix eval` sees a small win (intra-module hash-cons) but not a
  big one. The headline benefit is "the second time anyone
  evaluates anything that uses `lib.fix`, it's compiled once
  globally."
- **Watch for the GHC-Backpack class of bug.** Their content-
  addressed package store ran into edge cases around module
  signatures and instance resolution that took years to settle.
  Nix doesn't have type classes, so this specific class of bug is
  absent — but the lesson is "expect a long tail of edge cases
  when the cache key is structural."
- **Hash collisions.** SHA-256 is fine for any realistic threat
  model; don't try to save bytes with a 64-bit hash.

### Net assessment

The technique is established (Coq, Lean, Unison, V8, GHC, sccache,
JDK CDS all use forms of it). The risks are well-understood
(compiler version drift, cache invalidation, tooling). The
v3-specific case has unusually good preconditions (existing disk
cache, no UX exposure, culturally aligned).

It is not a research project. It is an engineering project with
known shape. The Unison-specific contribution to this
recommendation is the framing — *structural hash of the AST*
rather than *hash of the source bytes* — which is what enables
the cross-file sharing wins. That part is the genuinely
distinguishing trick, and the part that's least common in
production compilers today.
