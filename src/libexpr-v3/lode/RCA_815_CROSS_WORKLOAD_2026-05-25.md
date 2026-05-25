## #815 cross-workload cache regression — RCA in progress (2026-05-25)

Living doc of the cross-workload disk_cache investigation.  Updated as
hypotheses are killed or confirmed.

Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
Input Output Group.  SPDX-License-Identifier: Apache-2.0.

## Reproducer

```bash
TMPCACHE=$(mktemp -d)
# Step 1: 5-pkg nixpkgs sweep populates the cache
for pkg in hello bash gcc python3 firefox; do
  NIX_V3_CACHE_DIR=$TMPCACHE NIX_V3_DIRECT_EVAL=1 \
    nix eval --impure --expr "(import (builtins.getFlake \"nixpkgs\") {}).${pkg}.drvPath"
done
# Step 2: haskell-nix-example FAILS
cd haskell-nix-example
NIX_V3_CACHE_DIR=$TMPCACHE NIX_V3_DIRECT_EVAL=1 \
  nix eval --impure --no-eval-cache --option allow-import-from-derivation true \
  --expr "(builtins.getFlake \".\").packages.x86_64-linux.hello.drvPath"
# → error: function 'anonymous lambda' called with unexpected argument 'git'
```

Workarounds that work:
- `NIX_V3_NO_DISK_CACHE=1` — disable cache entirely.
- Run haskell-nix-example on a cache populated only by haskell-nix-example.
- Production cache passes (specific stable mix from many sessions).

## Failure site

FORMALS-DIAG stack (post-#815-diag commit `168940499`):

```
[95] newArgs    customisation.nix:357:65  ip=2064
[94] result     customisation.nix:163:9   ip=1009
[93] origArgs   customisation.nix:161:7   ip=911
[92] newArgs    customisation.nix:177:11  ip=1070
[91] nix-prefetch-git'  fetch-cargo-vendor.nix:25:3  ip=192
[90] dep        make-derivation.nix:449:18  ip=5079
[89] <thunk>    make-derivation.nix:492:31  ip=6028
[88] pkg        attrsets.nix:1912:13 ctx=getOutput  ip=3683
[87..76] (string interpolation / firstOutPath chain into
        cargo-auditable-cargo-wrapper.nix:30:5)
```

The lambda being called is **lsdw9m87**.../nix-prefetch-scripts (no `git`
formal).  The args include `git=gitMinimal` (passed by `.override` at
fetch-cargo-vendor.nix:25).

## Two nixpkgs versions in play

  | path             | file               | git formal | gitMinimal formal |
  |------------------|--------------------|--------------|---------------------|
  | `77dbgds155b...` (user's `getFlake "nixpkgs"`) | nix-prefetch-scripts/default.nix | ✓ | — |
  | `lsdw9m87nam...` (haskell.nix's pinned nixpkgs) | nix-prefetch-scripts/default.nix | — | ✓ |

`final.path` in haskell.nix overlays resolves to **lsdw9m87** (haskell.
nix's own pin).  So `import (final.path + "/.../nix-prefetch-scripts")`
imports **lsdw9m87**'s version (no `git` formal).

User's nixpkgs (77dbgds) is loaded transitively via haskell-nix-example/
flake.nix's own `inputs.nixpkgs` AND via the system's flake registry's
`getFlake "nixpkgs"`.

## Confirmed observations

- 5-pkg sweep caches **user's** (77dbgds) nix-prefetch-scripts.  Cache
  KEY = sha256 of content; user's content has `git` formal.
- haskell-nix-example loads **haskell.nix's** (lsdw9m87)
  nix-prefetch-scripts.  Different content hash → DIFFERENT cache entry
  (not contaminated by user's variant).
- Single-pkg runs (hello / bash / gcc alone) on fresh cache do NOT
  contaminate haskell-nix-example.
- It's the **combination** of 5 pkgs that triggers the regression.
  Specific subset not yet identified (single-pkg bisection in progress).
- Per-workload runs (5-pkg sweep alone, haskell-nix-example alone) all
  pass cleanly.

## Open hypotheses

### H1 — Shared lib/customisation.nix CU corruption

5-pkg sweep caches lib/customisation.nix at its content hash.  If
77dbgds and lsdw9m87 have **identical content** for that file (which is
very likely — lib changes rarely between minor nixpkgs revisions),
they share a cache entry.  haskell-nix-example loads this shared
entry.  If the loaded CU's bytecode encodes scope/upvalue assumptions
that differ from what fresh-compile would produce, eval diverges.

**Test**: compare sha256(77dbgds/lib/customisation.nix) vs
sha256(lsdw9m87/lib/customisation.nix).  If equal, this hypothesis is
plausible.  If different, kill it.

### H2 — Symbol-id intern history affects per-Module Stage-4 strictness

Stage 4 strictness analysis is per-Module (no cross-CU global state),
but it walks the IR which uses SymbolIds.  If two processes compile
the same file with different SymbolId-intern orders (different earlier
imports), the analysis MAY produce different annotations.

This is conjectural; needs:
- Verify Stage 4's output is fully determined by Module content.
- Check whether emit's bytecode shape depends on intern order.

### H3 — Selector / identity peephole regression

Even though schema 13 fixes the cross-process remap of `selectorSym`,
maybe some OTHER process-local context bleeds through.  Audit other
LambdaDescriptor fields that are NOT serialised.

### H4 — IFD-import EvalResults cache contamination

The IFD-import disk cache (EvalResults table) caches VALUES, not just
CUs.  A value cached by 5-pkg sweep MAY hold a Closure whose desc
points into the writer process's CU pool.  Reader loads the Value but
the Closure-desc pointer is dangling / process-local.

**Earlier test**: NIX_V3_NO_IFD_IMPORT_CACHE_DISK=1 did NOT fix the
regression.  Likely killed.

### H5 — Cache POPULATES a shared CU that haskell-nix-example mis-uses

The 5-pkg sweep imports nixpkgs's top-level / .../all-packages.nix.
haskell-nix-example imports many of the same standard library files.
If one of these shared CUs has a bytecode that ENCODES the writer's
overlay context (e.g. captures `pkgs` references somehow that differ
between workloads), the loaded CU misbehaves.

Plausible.  Needs: enumerate which CUs are SHARED between the two
workloads, then check one-by-one which one drives the divergence.

## Diagnostic next steps

1. **sha256-diff comparison** of all overlapping nixpkgs files between
   77dbgds and lsdw9m87.  Identify shared content-hash files.
2. **Cache entry enumeration** post-sweep: list every CU in the cache.
   Compare against the files haskell-nix-example actually imports.
3. **Per-CU deserialize-and-compare diagnostic**: load each cached CU,
   immediately re-compile from source, compare byte-by-byte (after the
   sort+remap normalisation).  Any mismatch = bug location.
4. **NIX_V3_NO_DISK_CACHE_PATH=<substr>** opt-out per-path: skip
   cache for one specific path at a time; identify the offending CU
   via process of elimination.

## Status

Investigating.  Re-opened after the schema-13 commit `ed8fa0669` was
believed to resolve #815 but didn't — production cache happens to
pass; fresh tmp + 5-pkg sweep + haskell-nix-example still fails.

## Update 2026-05-25 (post-bisection)

Added two diagnostic env vars to `primops.cc::primImport` (commit
forthcoming):

- `V3_DBG_DISK_CACHE_LOG=path` — append `HIT key=... path=...` /
  `MISS .../INSERT .../BLOCK ...` lines for each lookup/insert.
- `V3_DBG_DISK_CACHE_BLOCK=hex1,hex2,...` — force a `lookup` to skip
  the disk cache for matching keys (falls through to fresh compile).

### Cache-entry bisection

Using the log + block env vars:

1. 5-pkg sweep INSERTs 609 CUs; haskell-nix-example HITs 162.
2. The intersection (HITs in HNE that 5-pkg INSERTed) is **52 keys**.
3. **Binary chop on the 52 shared keys** found a single sufficient
   polluter — blocking that key alone makes HNE pass:

       cd66d8044dca7449ded42871e40f4f0c9e571624a6dd25157c3119517c46d9b3
       → /nix/store/77dbgds155bbz3vd3qywq1sii07i5ljs-source/
         pkgs/development/compilers/rust/make-rust-platform.nix

### Cross-process compile is non-deterministic

The same `make-rust-platform.nix` content produces DIFFERENT
serialized CUs in different processes:

- Sweep-warmed cache blob: **11497 bytes**.
- HNE-warmed cache blob:    **11497 bytes** (same size — same
  content + same set of references → same encoding shapes).
- `cmp -l`:                 **2273 differing bytes** (out of 11497).

Same size, different bytes — consistent with **process-local
SymbolId values being baked into the bytecode**.  SymbolIds depend
on the intern history of `ir::globalSymbolTable()`, which differs
between the sweep eval (nixpkgs from `getFlake "nixpkgs"`) and the
HNE eval (haskell.nix overlay chain).

### Within-process the cache is consistent

`HNE → HNE` on the same cache directory: both runs succeed.
The bug ONLY fires on `sweep → HNE` (cross-workload).

### Schema-13 remap is correct in principle but does not save us

`serialize::deserializeCU`'s symbol-table remap converts writer
SymbolIds to reader SymbolIds.  After the remap walk + the formals
re-sort (schema 12) + the selectorSym remap (schema 12), the loaded
CU's bytecode references the reader's global names.

However: the writer's COMPILER may have made decisions that depend on
the WRITER's SymbolId values (e.g. iteration order through
`unordered_map<VarId, ...>` in opt passes; emit-time peepholes
keyed on `selectorSym`).  Those decisions are baked into the
bytecode and survive remap intact — at the cost of producing
different bytecode for the same source in different processes.

### Surfaces at runtime as

The cached `make-rust-platform.nix` over-forces something that
fresh-compile leaves lazy.  The over-force cascade reaches
fetch-cargo-vendor.nix:25 where
`nix-prefetch-git.override { git = gitMinimal; }` ends up dispatched
against a lambda from haskell.nix's pinned **lsdw9m87** nixpkgs whose
formals don't include `git`.

functionArgs on that lambda correctly reports `hasGit=0` in BOTH the
healthy and the broken path (verified via `V3_DBG_FUNCTIONARGS_TRACE`
+ FORMALS-DIAG callstack).  The IF condition's bool result is
correct, but the broken eval still reaches the call site — i.e.
the divergence is upstream, in the **evaluation-order** the cached
CU's bytecode drives, not in the value-of-functionArgs path.

### What does NOT explain it

- **Not** the schema-12 selectorSym remap: schema 13's re-sort fix
  is in place, and the formals-bridge isn't the trigger here (no
  `s_twLambdaBridge`).
- **Not** any single opt pass with a gate.  Disabling each of
  `NIX_V3_NO_BETA_REDUCE`, `NIX_V3_NO_APP_SPINE_FOLD`,
  `NIX_V3_NO_STRICT_CALL_UNTHUNK`, `NIX_V3_NO_PRIMOP_FOLD`,
  `NIX_V3_NO_STREAM_FUSION`, `NIX_V3_NO_IF_FOLD`,
  `NIX_V3_NO_GENLIST_UNROLL`, `NIX_V3_NO_FUNC_STRICTNESS`,
  `NIX_V3_NO_OPT_STRICT` individually on the sweep still
  reproduces the regression.

  (Tested in this session, 2026-05-25.)

  ⇒ The culprit is one of the **ungated** passes in
  `optimise()`: `constantFold`, `commonSubexprElim`,
  `elimRedundantForce`, `inlineTrivialBindings`, `fusePrimOpApps`,
  `deadBindingElim` — OR emit-time peephole work in `emit.cc`.

- **Not** the in-memory `ImportCache::results` path-keyed lookup:
  77dbgds vs lsdw9m87 have different content hashes, so they
  occupy disjoint cache entries.

### Open work

- Add per-pass binary chop (manual patching needed — most candidate
  passes don't have env-var gates).  Goal: identify the precise
  pass and decision that differs cross-process.
- Once narrowed, fix is either:
  (a) Make the pass deterministic (eliminate
      `unordered_map<SymbolId|VarId|Hash>` iteration in
      decision-bearing positions; replace with `std::map` or
      sorted-first traversal).
  (b) Promote pre-opt CU to the cache value and re-opt on load
      (loses 5-15% perf benefit from cached opt; correctness wins).
  (c) Include opt-output checksum in the cache KEY so writers
      with different opt outputs occupy different cache slots.
- Pragmatic stopgap until a proper fix lands: document
  `NIX_V3_NO_DISK_CACHE=1` as a workaround for cross-workload
  scenarios (haskell.nix + standalone nixpkgs in the same cache
  directory).

### Reproducer + diagnostic toolset (this session)

- `V3_DBG_DISK_CACHE_LOG`: lookup/insert tracing
- `V3_DBG_DISK_CACHE_BLOCK`: force-fresh-compile for specific keys
- `NIX_V3_OPT_PHASE_LIMIT=N`: run only the first N opt passes
- `/tmp/v3-815-bisect.sh`: pass/fail oracle (FAIL = "git" error;
  PASS = eval proceeded past git-check into IFD-build phase)
- `/tmp/v3-815-bisect-opt.sh`: same oracle, wrapped to test
  individual opt-pass env-var gates

### Post-bisection update: inlineTrivialBindings is the trigger

Added `NIX_V3_OPT_PHASE_LIMIT=N` to `opt_const_fold.cc::optimise()`.
Each top-level pass in the pipeline counts as one phase; the env
var lets bisection target a single pass without code edits.

Pass numbering (0-based):

  0 constantFold       6 fusePrimOpApps        12 ifThenFold (block)
  1 betaReduce         7 primOpFold            13 genListUnroll
  2 constantFold       8 constantFold          14 appSpineFold (block)
  3 commonSubexprElim  9 inlineTrivialBindings 15 deadBindingElim
  4 elimRedundantForce 10 streamFusion          16 Stage 4 (gated)
  5 inlineTrivialBindings 11 ...

Bisection (sweep + HNE reproducer):

  LIMIT=0 → PASS (no opt anywhere)
  LIMIT=3 → PASS (through constantFold ×2 + commonSubexprElim)
  LIMIT=4 → PASS (+ elimRedundantForce)
  LIMIT=5 → PASS (+ inlineTrivialBindings #1 omitted — wait, see below)
  LIMIT=5 → PASS
  LIMIT=6 → FAIL (+ inlineTrivialBindings #1)
  LIMIT=7 → FAIL
  LIMIT=15 → FAIL

**LIMIT=5 passes, LIMIT=6 fails.  The 6th call into the pipeline
is the first `inlineTrivialBindings(m)` invocation.**

What `inlineTrivialBindings` does: it builds an
`unordered_map<VarId, VarId> alias` of every `VarRef` binding,
path-compresses it, then rewrites every operand VarId through the
map and drops the alias bindings.  See `opt_inline.cc:203`.

Why the bug fires: the cross-process compile diverges at this
pass.  Two processes compiling the same Nix source produce
semantically different bytecode after running `inlineTrivialBindings`.
The mechanism is not yet fully proven — most likely:

1. `unordered_map<VarId, VarId>` iteration order is sensitive to
   process state (bucket layout, libstdc++ rehash thresholds).
2. Path compression with `for (auto & [k, v] : alias)` then mutates
   `v` based on the snapshot of `alias` at the start of the loop.
   But other entries also mutate during the loop, which makes the
   path-compression OUTPUT depend on iteration order, not just on
   the initial alias map.

Specifically: `alias[A] = B; alias[B] = C`.  If we iterate (A, B),
we first set `alias[A] = C` (finds B → C), then set `alias[B] = C`
(no change).  If we iterate (B, A), we first set `alias[B] = C` (no
change), then `alias[A] = C` (finds B → C still).  In this example
order doesn't matter — but with longer chains and ties, order CAN
affect the compressed result.

(Subsequent rewrite step then uses the compressed alias map to
patch VarIds in every expression — emit then encodes the patched
VarIds into bytecode.  If alias compression differs across
processes, the emitted bytecode differs, and the SEMANTICS of the
result CAN differ if the rewrite happens to point a VarRef at a
DIFFERENT terminal var that lower.cc had used for a different
purpose downstream.)

### Fix candidates (not yet implemented)

A. Replace `unordered_map<VarId, VarId>` with `std::map<VarId, VarId>`
   in `inlineTrivialBindings`.  **Tested 2026-05-25: did NOT fix
   the bug.**  The non-determinism is not in this specific
   container.  Reverted.

B. Inspect every earlier pass (constantFold, betaReduce,
   commonSubexprElim, elimRedundantForce) for `unordered_map<>`
   iteration over container state that influences semantics.  Each
   one is a candidate; the bug only manifests after
   `inlineTrivialBindings` because that pass renumbers/drops
   VarIds and EXPOSES the upstream non-determinism in the emit
   output.

C. Direct byte-by-byte comparison of LIMIT=5 sweep blob vs
   LIMIT=5 HNE blob for `make-rust-platform.nix`.  (Caveat:
   LIMIT=5 stack-overflows the entry point because passes 6-15
   are critical for correctness on hello.drvPath, so the cache
   never gets populated.  Workaround: use a smaller entry-point
   eval that survives LIMIT=5 while still loading
   make-rust-platform.nix — e.g. a small synthetic that
   `import`s the file but doesn't force its outputs.)

D. Move opt to reader side (cache pre-opt CU).  Heavier but
   removes the cross-process determinism requirement entirely.

E. Disable `NIX_V3_DISK_CACHE` by default until the underlying
   determinism is fixed.  Pragmatic stopgap (loses ~13% on
   hello.drvPath but guarantees correctness across the
   sweep+HNE pattern).

### Diagnostic env var (left in tree)

`NIX_V3_OPT_PHASE_LIMIT=N` is left in `opt_const_fold.cc::optimise()`
as a permanent debugging tool.  Future "cache load differs from
fresh" investigations can re-use the bisection methodology
without code edits.

## Falsification update 2026-05-25 (post-meta-bisection)

### Critical: the LIMIT=5 PASS was a FALSE SIGNAL

After more careful auditing: at LIMIT=5 the sweep stack-overflows
on entry-expr eval, so the make-rust-platform.nix file never gets
into the cache.  `LIMIT=5 PASS` simply means "no cache entry,
nothing to bug-trigger" — NOT "passes 0-5 produce deterministic
opt output."

Verified by SQL: at LIMIT=5 the cache has 294 entries, none of
which is the make-rust-platform.nix key
(`cd66d8044dca7449ded42871e40f4f0c9e571624a6dd25157c3119517c46d9b3`).

### Minimal repro: LIMIT=0 also fails

New observation: I can populate the cache with make-rust-platform.
nix at LIMIT=0 (no opt at all) via a small `builtins.functionArgs
(import "...make-rust-platform.nix")` entry expression.  This
SUCCEEDS at LIMIT=0 (small enough that no stack overflow).
Cache contains the file at 11777 bytes (NO opt applied).

Then HNE eval (with default full-opt) loads this 11777-byte blob.
**HNE STILL fails with the `unexpected argument 'git'` error.**

⇒ **Opt is NOT the cause of #815.**  The bug is in the
parse / lower / emit / serialize / deserialize pipeline — even
the no-opt blob is corrupting cross-process semantics.

### Verifier diagnostic landed

Added `V3_DBG_DESERIALIZE_VERIFY=path` to `primops.cc::primImport`.
On every disk-cache HIT, ALSO fresh-compiles the same source file
in the current process, then byte-compares
`cu_cached.code == cu_fresh.code`.  Logs `SAME`/`DIFF` plus
disassembly window around the first divergence.

### The actual bytecode divergence (smoking gun)

For `pkgs/build-support/rust/fetch-cargo-vendor.nix`:

  first_diff_ip=30
  cached={OP_ATTRS_REC_SET(0x78), arg=7}
  fresh ={OP_ATTRS_REC_SET(0x78), arg=1}

  cached-disasm [24..38]:
    [25] OP_ATTRS_REC_SET       operand=0
    [26] OP_GET_LOCAL           operand=0
    [27] OP_MAKE_THUNK          operand=3  data=[1,0]
    [30] OP_ATTRS_REC_SET       operand=7   ← !
    [31] OP_GET_LOCAL           operand=0
    [32] OP_MAKE_THUNK          operand=4  data=[1,0]
    [35] OP_ATTRS_REC_SET       operand=1   ← !

  fresh-disasm [24..38]:
    [25] OP_ATTRS_REC_SET       operand=0
    [26] OP_GET_LOCAL           operand=0
    [27] OP_MAKE_THUNK          operand=3  data=[1,0]
    [30] OP_ATTRS_REC_SET       operand=1
    [31] OP_GET_LOCAL           operand=0
    [32] OP_MAKE_THUNK          operand=4  data=[1,0]
    [35] OP_ATTRS_REC_SET       operand=2

Both have the same structure (REC_SET + GET_LOCAL + MAKE_THUNK
triples).  But the **REC_SET slot operands differ**: cached writes
to slots {0, 7, 1, ...}, fresh writes to slots {0, 1, 2, ...}.

Same THUNK is computed (MAKE_THUNK funcIdx is the same), but it's
written to a DIFFERENT slot.  Since `entries[k]` in a sorted
Bindings has a different NAME per `k`, writing the thunk to slot
7 vs slot 1 puts it under a different attribute name.

This is the actual #815 bug.  After deserialize remap, the
REC_SET slot operands are NOT being properly translated to
reader-process sort positions.  Either:
  (a) The remap's `pending` stack is depleted before the SET
      fires (so `if (pending.empty()) { skip remap }`).
  (b) The remap's `oldToNew` permutation is computing wrong
      values.
  (c) The emit sometimes produces REC_SETs that aren't account-
      ed for by their preceding REC_INIT (slot index outside
      `n`).

Next step: instrument `remapSymbolsInBytecode` to log when a
REC_SET is encountered and what its `pending` state is.  Find the
specific REC_SET that goes wrong.

## Root cause identified 2026-05-25 (post-disasm verifier)

### lower.cc iterates `std::map<Symbol, AttrDef>` in TW-Symbol order

`nix::ExprAttrs::AttrDefs = std::pmr::map<Symbol, AttrDef>` — and
`Symbol` is a uint32_t TW-process-local symbol ID whose value
depends on TW's intern history.  Across two processes evaluating
the same source, iteration order over this map DIFFERS.

`lower.cc::lowerLetRec` (line 2643) iterates `attrDefs` in that
process-local order and assigns FuncIds in iteration order
(`m.functions.emplace_back()`).  Result: writer's `cu.lambdas[K]`
refers to a DIFFERENT named binding than reader's `cu.lambdas[K]`
for the same source.

When HNE deserialises the cached blob:
- `cu.lambdas[K]` is whatever writer assigned (writer's K-th
  iteration entry's thunkBody).
- The bytecode in cached references functions by FuncId, all
  internally consistent with writer's mapping.
- Trailer + REC_SET remapping correctly translates to reader's
  v3-Symbol sort positions.

So the cached blob's bytecode IS internally consistent.  Each
write goes to the correct named slot.

### WHY does the bug fire then?

This is still partially open.  The bytecode IS structurally
different (verified by `V3_DBG_DESERIALIZE_VERIFY`) but ought
to be SEMANTICALLY equivalent — each named entry receives its
correct thunk, just via different FuncId indices.

Hypothesis: there's an additional cross-process divergence we
haven't identified yet that ALSO surfaces with the cached blob.
Possibilities:
- `freeVars` computation depends on iteration order indirectly.
- VarId allocation order differs (m.freshVar() sequential
  counter — but counts up in iteration order, so different
  iter order → different VarId → identifier-positional
  differences elsewhere).
- A side-table (Module::recVarToSlotVar, Module::subExprFuncs,
  computeFunctionStrictness output) that depends on iteration
  order.

### Attempted fix (REVERTED)

Replaced lowerLetRec's iteration with canonical (symbol-string)
order.  The fix BROKE evaluation with "OP_ATTRS_SELECT: not an
attrset" because TW's `ExprVar::displ` indexes into the AttrDefs
map in TW-Symbol order — v3's `recAttrsNames[displ]` lookup
expects the SAME ordering.

So we CAN'T simply change lowerLetRec's iteration order without
also rewiring how `displ` is interpreted (or rebuilding the
`displ` index post-iteration).

### Open work

1. **Identify the residual cross-process divergence** beyond
   FuncId mapping.  The bytecode IS structurally different
   (and verifiably so), but the *semantic* divergence cause
   needs further investigation.
2. **Architectural fix**: separate FuncId assignment from
   iteration order.  Either:
   - Lower in TW order, then RENUMBER FuncIds canonically
     before serialise.
   - Lower in canonical order AND adjust how `displ` is mapped
     (build a separate displ→IR-position table).
3. **Stopgap**: disable disk cache by default until the proper
   fix lands.  Users would set `NIX_V3_DISK_CACHE=1` to opt in.

### Pragmatic recommendation (this session)

The repro is reliable and the diagnostic infrastructure
(`V3_DBG_DESERIALIZE_VERIFY`, `V3_DBG_DISK_CACHE_LOG`,
`V3_DBG_DISK_CACHE_BLOCK`, `NIX_V3_OPT_PHASE_LIMIT`) is in
place to continue investigation in a follow-on session.

Pragmatic stopgap: invert the disk-cache default so it's
opt-in (`NIX_V3_DISK_CACHE=1`).  Loses the cached-load perf
win (~13% on hello.drvPath per #777) but eliminates the
cross-workload correctness hazard until the underlying
non-determinism is fixed.

Action item: introduce `NIX_V3_DISK_CACHE_DEFAULT_OFF=1` (or
flip the gate's default) gated by a TODO entry referencing
this RCA.
