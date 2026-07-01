# v3 VM defect audit — 2026-07-02

**Purpose.** Line-by-line code audit of `src/libexpr-v3/` answering: *what concrete
defects and bugs in the code as written contribute to v3 being slower (CPU) and
bigger (RSS) than the tree-walker?* This complements — and does not re-litigate —
the 2026-06 lever campaigns (BEAT_TW, BiBOP, profile-at-scale), which measured
*strategies*; this audit read the *code*. Several findings below were never
targeted by any prior campaign.

**Method.** 8 parallel subsystem reviews (allocation/representation headers;
vm.cc both halves; primops.cc; GC; compiler/lowering; FFI/caching/driver;
cross-cutting hygiene sweep), findings cross-checked against each other and the
top claims re-verified by hand against the working tree (HEAD `638233d32` +
uncommitted local mods to `mark_sweep.cc`/`primops.cc`/`primop.hh`).

**Verification legend.**
- ✅ verified by hand in this audit (code read at the cited lines, or executed against the built binary)
- 🔷 reviewer-verified with verbatim excerpt, high confidence (often self-documented in code comments)
- 🔶 mechanism certain, magnitude/reachability needs measurement

Line numbers are working-tree as of 2026-07-02 and will drift.

---

## 0. Executive summary

The prior campaign verdict — "v3 is not doing a *single* fixable thing wrong" —
survives, but it was a statement about single levers, not about code quality.
This audit found **a long tail of real, fixable defects** across five themes,
several of them cheap, plus two genuine correctness bugs and a handful of latent
UAF windows. In rough order of importance:

1. **Every benchmark to date measured an instrumented build.** `v3_release`
   defaults to `false` and nothing in the repo enables it; on top of that,
   several hot counters bypass the `V3_STATS` macros entirely (a **mutex +
   `std::string` + hash-map probe on every primop call**), and the
   resource-limit env vars our own docs mandate for probes flip the per-opcode
   slow-gate cluster on. The published 1.8–2.5× CPU gap includes an unknown
   instrumentation tax. **Re-baseline first** (§1).
2. **The thunk-churn mystery is (largely) explained** — and it is *not* the
   killed #135 "trivial maybeThunk" story. Lowering mints thunks TW never
   creates: **one wrapper thunk per formal per call** of every `{...}:` lambda,
   eager `or`-default thunks per select, `inherit`-in-rec thunk wrappers, and
   universal call-arg thunkification whose de-thunking pass **never runs on
   imported modules** (i.e. on ~100 % of nixpkgs) and whose inline-based funnel
   measured ~0 % effective even when it does (§4).
3. **Attribute lookup — the #1 dynamic operation — got structurally slower when
   chain-Bindings shipped**: chained attrsets (produced by the default-on `//`
   optimization) *bypass the inline cache entirely*, pay up-to-16-layer walks
   per lookup forever, and MapAttrs entries resolved through a shared parent
   layer are **recomputed on every access** with no memo (§3.2, §3.3).
4. **The default config cannot reclaim dead tenured cells at all** — the sweep
   is metadata-starved by construction (`cellMetaEnabled()` = false), so the
   default gen-major pays a full precise mark **plus clears every inline cache
   of every CU** and reclaims ~nothing. Combined with type-level nursery
   exclusion (Bindings/Values/Pairs/Chars/Envs never even try the nursery) and
   nursery-full→bypass-until-outer-safepoint, "arena 40–60 % dead" is the coded
   outcome, not an emergent one (§5.1).
5. **Two correctness bugs** — one live semantic divergence (branch opcodes
   accept non-bool conditions: `if 1 then a else b` evaluates instead of
   throwing; **verified by execution**), one missing Phase-D barrier in
   `primSort` (the exact missed-root UAF class that blocked the nursery flip) —
   plus several latent UAF windows (§2).

None of these individually closes the gap; together with the known structural
items (flat transitive upvalue capture, switch dispatch, NaN-box tag decode,
arena-never-releases) they form the actual defect inventory the "structural
gap" was hiding. §9 gives a priority matrix with pre-committed falsifiers.

---

## 1. Measurement hygiene — the benchmarks measure an instrumented build (P0)

Fix and re-measure these **before** investing in anything else in this report;
every CPU number we have is contaminated to an unknown (probably low-single-digit
%) degree.

### 1.1 ✅ `bumpPrimOpCallCount`: mutex + `std::string` + string-keyed hash map on EVERY primop call, unconditional
`primops.cc:9322-9328`, called from `vm.cc:12343` (OP_CALL_PRIMOP) and
`vm.cc:12408` (OP_R_PRIMOP2):

```cpp
void bumpPrimOpCallCount(const PrimOp * po) {
    if (!po) return;
    auto & c = primOpCounter();
    std::lock_guard<std::mutex> g(c.mtx);
    c.counts[std::string(po->name)]++;
}
```

The comment claims "gated on NIX_VM_STATS" — only the *dump* is gated
(`run.cc:803`); the bump is not, and it is a plain function so even
`v3_release=true` would not strip it. Millions of primop calls/eval ×
(uncontended mutex + string construct + hash + map probe) on a single-threaded
VM. Note the third caller (`invokePrimOpDirect`, `vm.cc:2176`) already takes a
`bumpStats` flag and passes `false` from `callClosure` — this is an oversight,
not a design decision. Independently flagged by 4 of 8 reviewers; the single
highest-confidence cheap fix in this report.

### 1.2 ✅ The benchmarked build config ships instrumentation
`meson.options:1-9` defaults `v3_release=false`; the configured build is
`buildtype=debugoptimized -Dlibexpr-v3:v3_release=false`, and **no** flake /
packaging / bench script enables it. So all ~97 `V3_STATS_INC/BUMP` sites are
live in every recorded number: per-alloc byte counters in every
`Alloc::alloc*` (`alloc.hh:2813,2831,2902,2960,3013,3103`), the 10-branch
attrset-size bucket cascade per `allocBindings` (`alloc.hh:3180-3201`),
`closuresAllocated` per OP_MAKE_CLOSURE, etc. Each is a magic-static guard load
(`allocStats()` is a function-local static containing an `unordered_map` →
non-trivial ctor) + RMW. The option's own pre-committed falsifier ("ship if
≥2 % wall or ≥20 MB RSS") **appears never to have been executed**.

### 1.3 🔷 Raw counters that bypass even `V3_STATS` (survive `v3_release=true`)
- `mergeBindings` — 3 unconditional shared-counter bumps + 2 size-histogram
  bucket cascades per `//` (`vm.cc:1631-1634, 1661-1662`, byte counters at
  `1761/1886/2084`), plus 4 function-local magic-static gate reads per call
  (`vm.cc:1673-1699`). `//` is the #1 Bindings producer (584 MB on HNE).
- `allocStats().selectorLambdaCalls++` unconditional on the selector-lambda
  fast path (`vm.cc:6579, 13361, 15218`) — the dominant nixpkgs callback shape.
- `intrinsicExtendsCalls++` / `intrinsicComposeCalls++` in `callClosure`'s
  intrinsic dispatch (`vm.cc:15148/15178`).
- `g_keepPapDisarmCount` bumped unconditionally (`vm.cc:3274-3284`), gate only
  guards the report.
- Nursery `tryAlloc` bumps `allocCount/allocBytes/overflowCount` per allocation,
  not `V3_STATS`-gated (`nursery.hh:84-97`).

### 1.4 🔷 Setting the mandated limit env vars activates the per-op slow path
`vm.cc:3841-3844` folds `limitsActive()` into `kAnySlowGate`; with any of
`NIX_V3_MAX_WALL_TIME/MAX_HEAP/MAX_CPU_TIME` set (which
`src/libexpr-v3/CLAUDE.md` mandates for *all* non-trivial probes), **every
dispatched opcode** enters both slow-gate clusters and pays a function-local
`static thread_local` poll-counter RMW (TLS-wrapper call + guard on macOS,
`vm.cc:4361-4367`). Any timing collected under the "safe" env-var set is
systematically slower than production default. Fix: plain local countdown
re-armed on frame entry, or split limits out of `kAnySlowGate`.
Related: `NIX_VM_STATS=1` flips `dbgForceStatsActive()` on (`vm.cc:3637-3641`),
adding 3 writes per force — the exact artifact class that already produced one
retracted RSS win.

**Action:** build with `-Dlibexpr-v3:v3_release=true`, fix 1.1/1.3, split
limits out of the slow gate, then re-run the darwin-4 baseline. This
re-baseline is a prerequisite for judging everything else in this report.

---

## 2. Correctness defects

### 2.1 ✅ Branch opcodes accept non-bool conditions — live semantic divergence from TW (VERIFIED BY EXECUTION)
`vm.cc:5020-5031` (OP_BRANCH_FALSE), `4981-4992` (OP_AND_BRANCH), `4993-5003`
(OP_OR_BRANCH), `5004-5017` (OP_IMPL_BRANCH), `5033-5052` (OP_R_BRANCH_FALSE):

```cpp
Value v = pop(vm);
if (v.isBool() && v.asInt() == 0) ip = operand;   // non-bool: silently truthy
```

Verified against the built `v3-eval`:
- `if 1 then "then-taken" else "else-taken"` → `"then-taken"` (TW: *error: expected a Boolean*)
- `1 && true` → `true`; `1 && false` → `false` (TW: error). OP_AND_BRANCH's
  else-arm pops the non-bool, so `1 && x` returns `x` — a **wrong value**, not
  just a missed diagnostic.

nixpkgs never hits this (valid code), which is why byte-identity suites pass —
but user errors propagate as wrong answers. Fix is one predicted branch per
opcode; negligible cost. Needs lang-test fixtures (positive + negative per the
always-add-tests rule).

### 2.2 ✅ `primSort` result list is missing its Phase-D barrier (PhD-6 missed-root UAF class)
`primops.cc:9077-9105`: allocates `result`, copies arbitrary (possibly
nursery-resident) elements, runs `stable_sort` with re-entrant `callClosure2`,
then `out.mkList(result)` — **no `listPostConstructBarrier(result)`**. Sixteen
comparable sites in primops.cc call the barrier for exactly this reason
(`primFilter`, `primCatAttrs`, `primConcatMap`, `primGroupBy`,
`primZipAttrsWith`, even `primTail`). If `result` is tenured (nursery full —
the common state, §5.2) and any kept element is a nursery cell, the next
scavenge moves/frees it under the list. One-line fix + a `--brute` sort-heavy
stress repro.

### 2.3 🔷 Disk-cache-hit path destroys the CU on *any* exception from `run()` — dangling `Closure::cu` + duplicated IFD side effects
`primops.cc:7271-7789`: the hit branch wraps `deserializeCU` **and**
`out = run(cache.cus.back())` in one try; the catch does
`cache.cus.pop_back()` and falls through to fresh parse+compile+**re-run**.
Written for corrupt blobs, it also catches eval errors /
`WallTimeExceededError` / OOM thrown mid-eval — at which point partially
evaluated closures/thunks already reference the just-destructed CU (code,
lambdas, ICs freed). Anything that escaped into shared state (nested import
results, tryEval recovery) is a dangling-CU deref on later force; the re-run
duplicates IFD side effects. Scope the pop to the deserialize step only (or
deliberately leak, as the invalidation path already does).

### 2.4 🔶 Default-path huge-block UAF window: interior-only-referenced huge Bindings can be freed while live
Chain (all on the **default** config): huge (≥4 MB) block reclaim runs
unconditionally in `runMajorMarkSweep` (`mark_sweep.cc:2404-2415`) and tests
`hugeMarked_` by **block-begin** address; `tryMark` on an interior `Tag::Slot`
pointer inserts the **interior** address (`mark_sweep.cc:117-120`); the rescue
path (`findContainingCellStart`) is dead when `cellMetaEnabled()` is false
(`alloc.hh:2067`) — which is the default. The codebase itself documents that
slot-only-reachable Bindings exist (M5 found 1320, `mark_sweep.cc:1270-1277`).
Preconditions: default gen-major actually fires (exitDepth==0 + 256 MB
threshold) while a slot-only-reachable ≥4 MB Bindings is live — rare, M5-scale
attrsets are exactly this size class. Needs a targeted repro before fixing.

### 2.5 🔷 Self-documented latent: `deepForceList` writeback pointer goes stale across scavenge
`vm.cc:12281-12310` (GC_AUDIT_ROUND_2 #6, documented in-code): if a scavenge
fires during the force chain, `frame.forceWriteTarget = &list->elems[i]`
dangles; the WHNF is written into dead nursery memory and the element is
silently re-forced. Correctness currently survives by re-derivation; one
allocation-pattern change away from corruption. Three candidate fixes are
listed in the comment; none implemented.

### 2.6 🔶 Smaller latent items
- **Blackhole early-return vs EVAC-mode invariant**: `gc.cc:453-456` returns a
  tenured Blackhole before the Step-7 gate, but `walkThunk` documents that a
  Blackhole's tail/capturedWiths are live (exception unwind reverts
  Blackhole→Suspended). Hole in the non-default `NO_PHASE_D`/`EVAC` modes only.
- **Raw-word peephole hazard**: the tail-call and R_RETURN rewrites decode raw
  code words (`emit.cc:2342-2399`) — the exact pattern the GET_LOCAL2 fusion
  was hardened against (`emit.cc:2162-2169`). Improbable today (needs SymbolId
  ≥ 86 M), a time bomb as tables grow; fix idiom (record emit positions)
  already exists in the file.
- **Stack slots zero-init to Float 0.0, not Uninitialized** (`vm.cc:4593-4596`,
  `6923/7491/7627`): `Value{}` decodes as `Tag::Float 0.0`, so a
  read-before-write bug in emitted bytecode yields silent `0.0` instead of a
  diagnosable error. GC-safe, but masks emitter bugs.
- **String-context table keyed by buffer address** (`alloc.hh:4313-4317`,
  self-documented): correctness depends on `allocChars` buffers never being
  reused within an eval. True today *only because* the arena never reclaims;
  `NIX_V3_MIDEVAL_REUSE` recycling Chars cells would silently inject wrong drv
  edges. The default-path sweeper does erase freed ranges
  (`mark_sweep.cc:1654-1672`) — the hazard is specifically stale entries vs
  *reused* addresses under reuse modes.
- **Unrooted heap vectors across VM re-entry** in `primListToAttrs`
  (`primops.cc:2069-2084`), `primZipAttrsWith` C fallback (`3187-3196`),
  `primGenericClosure` (`3554-3606`): safe under exitDepth==0 gating, UAF mines
  for any future default-on mid-eval GC (the `GcRootVec` discipline was applied
  to filter/foldl'/concatMap/partition/groupBy but not these).

---

## 3. CPU — the interpreter-tax catalog

Ordered by (frequency × cost). The profile facts these must explain: DISPATCH
7-23 %, OP_GET_UPVALUE 13-17 % of executed ops, ~52 % trivial stack ops,
ALLOC ~20 %.

### 3.1 🔷 Formals-lambda call validation: per-call `std::string` + unguarded `forceValue` + O(n·log m) re-validation
`vm.cc:6709, 6748-6750` (OP_CALL), duplicated verbatim at `7410, 7438-7440`
(OP_TAIL_CALL). Every call of every `{ stdenv, lib, ... }:` lambda — i.e.
every `callPackage` / `mkDerivation` call — pays:
- a full out-of-line `forceValue()` on the arg even when already `Tag::Attrs`
  (no `needsForce` pre-check, despite the comment claiming "a tag check");
- a `std::string lambdaName = ... : std::string("anonymous lambda")` built
  **before any error condition is known** (heap alloc past SSO);
- extra-arg check `b->forEach(λ{ binary-search formals })` + missing-arg loop —
  both lists are sorted by SymbolId, so a single merge scan is O(n+m); no
  per-(descriptor, shape) memo.

### 3.2 ✅ Chain-Bindings SELECT bypasses the inline cache entirely; every lookup pays O(depth × log n) forever
`vm.cc:9853-9931` — comment verbatim: *"v1 skips the inline cache for chain
operands."* `mergeBindings` (default-on) turns `base // small` — the dominant
nixpkgs shape — into a Chain up to `kMaxLayers=16`; for those attrsets every
`x.attr` walks layers with a fresh binary search per layer, misses (`x.foo or
default`) always pay the full walk, `OP_ATTRS_HAS` and Cursor iteration pay the
same tax. The countDistinct memo (shipped) fixed the *counting* cost, not the
*per-lookup* cost. Missing piece: a chain-aware read IC caching
`(chainLeaf, sym) → (ownerLayer, slot)` — the C-1 shared-parent hazard applies
to writes, not to caching a resolved read slot. The flat-path IC also installs
on every miss with no megamorphic backoff (`vm.cc:10090-10105, 10300-10305`);
measure per-site hit rates.

### 3.3 🔷 MapAttrs entries in shared parent layers are recomputed on EVERY access
`vm.cc:3362-3410` (`tryPushDirectMapAttrsEntry`, `memoize=false` branch),
reached from chain-SELECT parent hits (`vm.cc:9880-9891, 10636-10645`).
`mapAttrs f s // overlay` re-runs `f` per access with no memo anywhere — the
#696 recompute class the App-memo exists to prevent, silently re-opened for
the chain-parent case. A leaf-owned memo (or the result App's own `evaluated`)
preserves the no-shared-writes rule.

### 3.4 ✅ `OP_ATTRS_INIT` re-sorts compile-time-constant names at runtime, every execution
`vm.cc:9232-9243`: literal `{ a=…; b=…; }` inside a lambda re-sorts 24-B
entries + re-runs a statically-decidable duplicate check per call.
OP_ATTRS_REC_INIT already gets pre-sorted names from emit (`vm.cc:9386-9390`,
`emit.cc` LetRec path); ATTRS_INIT — made *more* common by the 2026-06-16
non-rec demotion lever — was left with the runtime sort. Emit the (name,pos)
trailer sorted and push values in sorted order; runtime becomes a straight fill.

### 3.5 ✅ `valueEqual` heap-allocates its work stack per comparison
`vm.cc:1010-1013`: every non-int-int `OP_EQ`/`OP_NEQ` — i.e. **every string
comparison**, `system == "x86_64-linux"`-class checks included — mallocs a
16×40 B task vector (and the chain-attrs case allocates a second one) even for
a single scalar compare. Fast top-level scalar path before building the stack,
or a small stack buffer with heap spill.

### 3.6 🔷 String concat: 3 payload copies + a global hash probe per part + context copy-sort per concat
`vm.cc:11820-12110`: per part, `lookupStringContextEntries` = global
`unordered_map<const char*, vector<string>>` probe even for context-free
strings (TW: inline context-pointer check); `coerceToString` returns the whole
payload as a fresh `std::string` (copy 1) appended into `out` (copy 2) then
`allocChars`+memcpy (copy 3); contexts propagate by value with per-concat
`sort`+`unique` — O(k²·|ctx|) along interpolation chains on drv-heavy evals.
Related: `mkStringValueOwned` takes `std::string` **by value** and
`primConcatStringsSep`/`primReplaceStrings` pass lvalues → one extra full copy
of every concatenated string (`primops.cc:704-713, 1520, 2559`; fix =
`std::move` at two sites). `unsafeDiscardStringContext` clones the entire
payload to drop context (`primops.cc:2843-2865`) — forced by the
pointer-keyed side-table design (§5.6).

### 3.7 ✅ Write barriers: hard-ON but hinted never-taken; post-construct O(n) rescans of every tenured container
`barrier.cc:120` hardcodes `g_phaseDActive = true` (opt-out retired), yet every
helper is `if (__builtin_expect(phaseDActive(), 0)) [[unlikely]]`
(`barrier.hh:226,250,269,286,329,363,385,421,447,475,506`) — the always-taken
body is laid out cold, per pointer store, and as an `extern const` in another
TU it cannot constant-fold without LTO. Each barrier pays `threadNursery()` TLS
+ full NaN-box tag decode + range compares. The post-construct barriers
(`bindingsPostConstructBarrier` etc.) re-walk **every element of every freshly
built container**; since ~80-93 % of containers are tenured-direct (§5.2) and
can't be nursery-resident-parents, most of these full scans find nothing —
and once the nursery is full they are provably pure waste, yet still run.
Fixes: `constexpr phaseDActive() { return true; }` + flip hints; hoist nursery
bounds into cached globals (base/end fixed after init) so `contains()` is two
compares without TLS; short-circuit scans on a "nursery empty/full" bool.
Same inverted-hint bug in dispatch: `__builtin_expect(nursery != nullptr, 0)`
at `vm.cc:3941` is always-true at exitDepth==0; env-sharing hints contradict
each other (`vm.cc:5254` unlikely vs `5279` likely).

### 3.8 🔷 Per-opcode safepoint polling in the outermost dispatch loop
`vm.cc:4049-4075`: at exitDepth==0, **every dispatched opcode** pays a
function-local `static thread_local` threshold access (TLS-wrapper + guard) +
`threadArena()` (second TLS) + `bytesAllocated()` load + compare, plus
`threadNursery().shouldScavenge()` which recomputes `(sizeBytes*triggerPct)/100`
per instruction (`nursery.hh:306`). At exitDepth>0 a residual
`g_midEvalGcEnabled` load+branch per op remains for a default-off feature.
Standard fix: allocator sets a single "GC requested" flag; loop checks one
memory word. Also `cu->code[ip++]` re-loads the vector data pointer per
iteration — cache `const Instruction * code` in a dispatch local (§3.12).

### 3.9 🔷 dispatchLoop re-entry prologue: ~50-100 instructions per higher-order-primop callback element
`vm.cc:3698-3756`: each re-entry recomputes `kAnySlowGate`, reads trace
statics, resolves TLS, saves/restores 4 OPCYCLES TLS slots **unconditionally**
(even with `g_countOpCycles` off), pushes/pops the active-VM vector. Every
`map`/`filter` element application pays this; `reuseScope` (measured −5.7 % on
foldl) is plumbed only to the foldl-family `callClosure2` callers. Extending
reuseScope to the remaining arity-1 strict callers was the promising-untested
L3 idea from the autoresearch run — this is its code-level basis. The
`__functor` path is worse: two full synchronous `callClosure` re-entries per
functor application (`vm.cc:6240-6254`).

### 3.10 🔷 OP_GET_UPVALUE (13-17 % of all ops): 3-4 dependent branches + a leftover getenv magic-static per read
`vm.cc:4608-4692`: per read — `frames.back()` recompute, closure-vs-thunk
`frameHasUpvalues` branch, `frameNUpvalues` branch, bounds check with a large
cold diagnostic block inline, env-sharing `upvalEnv ? env->values[i] :
upvalues[i]` branch (closure.hh:91), then a `V3_DBG_SELECT_AT_CODEOFF`
magic-static guard (`vm.cc:~4686`) **on the hottest opcode in the VM**.
A dispatch-local `upvalBase/nUp` computed once per frame entry reduces the body
to bounds-check + indexed load + push. (Why the op is so *frequent* is a
lowering defect — §4.4.)

### 3.11 🔷 OP_MAKE_THUNK: redundant descriptor store + speculative env-intern call per thunk
`vm.cc:5692`: `t->suspended.desc->cu = cu;` unconditionally dirties the shared
libc-resident LambdaDescriptor cache line on **every** thunk creation (millions
per eval; same line is concurrently read for codeOffset/nLocals on forces) —
stamp once at CU install or guard with `desc->cu != cu`. `vm.cc:5594`: for
every thunk with 0<nUp≤8 (the overwhelming majority),
`maybeInternUpvalueEnvFromStack` is called only to discover `shareAfter(nUp)
== UINT32_MAX` and return null — hoist the `nUp > 8` test to the call sites.
More broadly, env-sharing default-on shares almost nothing (`vm.cc:596-600`:
share only when nUp>8, observed ≥2) but taxes every MAKE (the call), every
upvalue read (the `upvalEnv` branch), and every GC walk (dual layout) —
re-run its ship falsifier.

### 3.12 🔷 Dispatch anatomy (context for the 7-23 % DISPATCH share)
Plain `switch` at `vm.cc:4485` (computed-goto known-absent, P-6): a trivial
opcode pays ~25-30 instructions / 8-9 branches (one shared indirect jump =
worst-case BTB) of which ~2 are useful work. Operand encoding itself is good
(fixed 32-bit words, aligned). The two cheap wins short of token-threading:
cache `code.data()` in a local; hoist the per-op gate cluster (§3.8).

### 3.13 🔷 Blackhole encounters scan the entire frame stack linearly
`vm.cc:8758-8765`, `14219-14224`: every Black-thunk hit does an O(frames) scan
(thousands of frames deep in nixpkgs); the miss case scans *all* frames.
Black hits are normal control flow under blackhole-as-value (rec/`with self;`
fix-points). Per-thunk "on-my-frames" bit or in-flight hash → O(1).

### 3.14 🔷 Miscellaneous verified hot-path leftovers
- ~15 function-local magic-static env gates inside hot opcode bodies
  (OP_MAKE_CLOSURE ×3, OP_CALL ×~6, force paths ×4-6; list in reviewer output:
  `vm.cc:5191,5247,5586,5930,5965,6301,6407,6657,6764,6930,7092,7356,7447,7496,
  8437,13770,13790,14928`) — the exact #768 pattern already measured at ~2 %
  and fixed elsewhere; promote to namespace-scope `inline const`.
- Debug ring buffers zero-initialized per force: ~416 B memset in
  `op_force_slow` (`vm.cc:8446-8450`) + ~288 B in `forceValue`
  (`vm.cc:13791-13794`), read only under `V3_DBG_CHASE`. Confirm via disasm the
  memsets survive; if so, drop initializers or move inside the gate.
- `withLookup` uses `catch (BlackholeError)` as expected control flow during
  normal delayed-with resolution (`vm.cc:2357-2366`) — µs per throw vs ns per
  hit (opt-out chase path mitigates; verify which path production takes).
- Dead code: OP_ATTRS_SELECT computes an unused `chase` value
  (`vm.cc:9639-9651`); SELECT_DYN evaluates `shouldForceSelectedEntry` twice
  back-to-back (`vm.cc:10647-10668`); duplicate-name detection loop in
  `emit.cc:1434-1444` computes then does nothing.
- `getNixEvalState()` TLS read + `EvalState` construct per primop call
  (`vm.cc:12352-12355`); hoistable into VMState. (TLS was 6 % on M5; the
  per-alloc TLS lever T1a is falsified — this is the per-primop-call slice,
  untested.)
- `frames.reserve(4096)` < `kMaxCallDepth=5000` (`vm.cc:13205` vs `181`):
  one guaranteed 160 KB realloc+copy on deep chains, contradicting the
  "never reallocates" comment at `vm.cc:7043-7047`.
- Regex cache is a 64-entry thread-local LRU (`primops.cc:3645-3670`) vs TW's
  unbounded per-EvalState cache — dynamically-interpolated patterns can cycle
  it and recompile per call.
- `functionArgs` rebuilds + re-sorts the formals attrset per call
  (`primops.cc:8738-8753`) though it is immutable per descriptor; nixpkgs calls
  it once per `callPackage` (`intersectAttrs (functionArgs f) pkgs`). Memoize a
  `Bindings*` in the descriptor.
- GC walkers dispatch per-Value through `std::function`
  (`gc.cc:947-948, 1477-1505`) — bounded by GC's ≤7 % share; mechanical
  template-visitor refactor.

---

## 4. The thunk story — why 62-67 % of thunks are never forced

The killed #135 lever measured *trivial var/const* thunks (0.7 %) and concluded
the population is "99.3 % real". Correct — and misleading: the audit found the
*real* thunks are largely **compiler-manufactured wrappers TW never creates**.
These are simultaneously CPU (alloc + MAKE_THUNK dispatch) and peak-RSS
findings (every never-forced thunk is permanent arena, since nothing reclaims
mid-eval).

### 4.1 ✅ One wrapper thunk per formal per call (the big one)
`cli/lower_v3.hh:599-635` (wrapper bodies), `:666-685` (demoted path — the
`MkThunk`s sit in the lambda body's entry block). Every `{ a, b ? d, ... }:`
lambda lowers each formal to its own thunk Function whose body is
`if param ? X then param.X else default`; **every call executes K
OP_MAKE_THUNKs** (DCE removes only formals referenced nowhere). TW binds
`env.values[displ] = attr->value` directly — zero allocations for supplied
args, one thunk only for a *missing* defaulted formal. nixpkgs is
formals-saturated (every package function; mkDerivation's ~40-formal set): a
formal unused on the taken branch = a never-forced thunk per call; a used
formal = double force (wrapper → arg-entry thunk). Statically each formal also
mints an `ir::Function` + 168-B LambdaDescriptor + bytecode body (§5.4).
**Fix direction:** supplied-arg fast path — bind `param.X` directly when
present (the wrapper is only needed for the missing-with-default case), or
lazy per-formal thunk creation on first upvalue capture. This is the highest-
leverage single item in the report for the thunk/alloc/RSS cluster; needs
byte-identity care (force-order of defaults).

### 4.2 ✅ Call-arg de-thunking never runs on imported modules; the funnel is ~0 % effective anyway
`primops.cc:7856` (import compile path: `optimise → computeFreeVars → compile`
— **no `applyStrictnessPasses`**) vs `run.cc:266` (root expr only; #774
comment documents the choice and its falsifier: moving into `optimise()`
measured **1 elision in 40,961 Apps** because `isInlinableMkThunk` requires
same-block + single-use + cloneable — ~never true in real code).
`computeFunctionStrictness` additionally only scans the linear prefix of the
entry block and bails at the first branch (`opt_func_strictness.cc:29-33`).
So `thunkifyForAttr` (`lower_v3.hh:416-417`) thunkifies every non-trivial call
arg in ~100 % of nixpkgs. **The right lever was designed but never built:** an
`OP_CALL_STRICT` that consumes `Function::strictArgs` at emit/call time
(forcing at the call site, no IR surgery) — documented as future work at
`ir.hh:610-616`. The #774/#776 falsification killed the *inline-based* v4
approach, not call-site strictness itself.

### 4.3 🔷 More per-evaluation thunks TW doesn't allocate
- **`x.y or <non-trivial default>`**: default thunkified in the *parent* block,
  allocated on every select evaluation, forced only on miss
  (`lower_v3.hh:1246`; trivial defaults escape). Lower the default inside the
  else block.
- **`inherit x` in let/rec**: a full thunk Function wrapping an existing
  binding — thunk-wrapping-a-thunk (`lower_v3.hh:840-843`); `inherit (e) x`
  costs two layers. TW aliases the env slot. Pervasive in `lib`.
- **Lazy `map`/`genList` App pairs are tenured at birth** — see §5.3.

### 4.4 🔷 Flat *transitive* upvalue capture explains GET_UPVALUE dominance and part of the trivial-op bloat
`ir.hh:500-516` (freeVars are transitive), `ir.cc:312-496` (propagation),
`emit.cc:1085-1111` (one `emitVarRef` push per capture): a var used only by a
great-grandchild thunk is re-captured (pushed + stored) at **every intermediate
Lambda/MkThunk level**, and each forwarding push in a nested context is itself
an OP_GET_UPVALUE. With ~2.9 M thunk creations × several captures (+
`lexicalWiths` pushes under the ubiquitous `with lib;`), capture-forwarding
plausibly accounts for a large share of both the 13-17 % GET_UPVALUE and the
~52 % trivial-op population. TW pays one `Env*` per closure. This is the
known-structural item (env-sharing was aimed here) — but the *emit-side*
mitigations are unexplored: don't re-push captures that merely forward
(capture the ancestor's upvalue index instead), and §4.5.
- Also 🔷 `emit.cc:351-368`: the operand-defer machinery flushes on the first
  `emitVarRef`, so all multi-operand shapes (PrimOpCall args, list elems,
  concat parts, **all capture pushes**) spill to SET_LOCAL/GET_LOCAL pairs even
  when the pending suffix matches operand order — generalize
  `tryFastPathBinary` to N-ary suffix consumption.

---

## 5. RSS — retention and allocation-shape defects

Frame: the killed levers proved *reclaim* doesn't lower peak (arena pins,
no sparse blocks). The path that remains — and that these findings serve — is
**allocate/retain less in the first place**.

### 5.1 ✅ The default config cannot reclaim dead tenured cells — and pays full GC cost anyway
`alloc.hh:1250-1252`: `cellMetaEnabled() = g_majorGcEnabled(hard false) ||
g_midEvalGcEnabled(default off)` → false ⇒ `alloc()` never records cell-start
bits ⇒ the sweep loop breaks at the first regular block
(`mark_sweep.cc:2348`: `if (regularBlockIdx >= cellStarts.size()) break;`).
Free-list binning and whole-block-free are additionally gated off. Yet the
default-on gen-major (`vm.cc:378`), when it fires, pays a **full precise mark
of the reachable graph + conservative C-stack scan + clears every IC of every
CU in the registry** (`vm.cc:4112-4117` — followed by cold-IC refill) — for
~zero reclaim (only dead ≥4 MB calloc'd blocks, whose `std::free` returns no
RSS per the code's own comment at `alloc.hh:1406`). Decision needed, either
way: (a) accept that default gen-major is pure overhead and stop firing it /
stop the IC clear, or (b) turn cell metadata on by default and let sweeps at
least recycle (known: doesn't lower *peak*, but caps *growth* between peaks —
and the current branch's bounded-memory M2 work needs the metadata anyway).

### 5.2 ✅/🔷 Nursery: the dominant byte population is excluded by type; nursery-full poisons the rest
Only `allocClosure` / `allocThunkSuspended*` / `allocList` route through
`nurseryOrArena` (`alloc.hh:2805-2809`); `allocValue/Env/Pair/Chars/Bindings`
are tenured-direct *by design* (`alloc.hh:2798-2803`) — Bindings alone ≈84 % of
arena bytes. For the eligible minority, `tryAlloc` returns null on overflow
and the scavenge trigger lives only in the outermost loop
(`vm.cc:3907-3911`, exitDepth==0 + nested-VM defer) — deep evals are ~always
nested, so the nursery fills once (~2 scavenges/firefox) and every subsequent
eligible alloc pays the full nursery attempt (TLS + 4 branches + counter) and
tenures anyway. Cheap CPU fix: latch "nursery permanently full" and collapse
`nurseryOrArena` to one predictable branch. RSS note: the 32 MB stays resident
**and is a registered Boehm root** (`nursery.hh:566`) — conservatively scanned
by every Boehm collection; the "zero perf cost" comment is unverified.

### 5.3 🔷 Lazy `map`/`genList` results are tenured at birth
`allocPair` is arena-direct (`alloc.hh:3009-3015`; `barrier.hh:444-451`
confirms "ValuePair is always tenured"). `primMap`/`primGenList` build one
App pair per element → with 62-67 % never forced, map-heavy code permanently
retains dead App pairs (pairs measured 253 MB / 13.7 % on M5). Distinct from
the killed pair-*shrink* lever: this is allocation *routing*. Needs a design
pass on pair pointer-stability (the reason they were tenured) before attempting.

### 5.4 🔷 CU-side malloc bloat: LambdaDescriptor 168 B × one per *thunkified expression*, double-lowered orphans included
- `sizeof(LambdaDescriptor) = 168` (measured; `closure.hh:324-555`): two
  always-present `std::string` headers (`name` populated for every function,
  `"<thunk>"` etc.) + 3 mutable stat counters (24 B, space unconditional) +
  `astLambda` + `cachedSingletonClosure` + 3 intrinsic vars ⇒ 72-96 B
  diagnostics/rare fields per descriptor. One descriptor per thunk body — every
  let binding, attr value, call arg, formal wrapper — hence the array (25.5 MB
  firefox) exceeding the bytecode itself. Pack names into a side string table
  (u32 id), move counters/formals/intrinsics to sparse side tables ⇒ ~15+ MB
  on firefox alone. Warm loads re-materialize the strings per lambda per
  process (`serialize.cc:1046-1047`).
- 🔷 **DAG-demotion lowers every acyclic sibling-referencing let-group TWICE**
  (`lower_v3.hh:831-854` then `:1003-1094`), keeping the orphaned first copy's
  Functions + descriptors (body-cleared stubs still emitted,
  `emit.cc:2661-2663`). Their own probe: ~70 % of recursive lets are
  acyclic-DAG ⇒ roughly doubles descriptor count and lower time for a large
  fraction of `let`s. Classify deps *before* lowering, or re-wire instead of
  re-lower.

### 5.5 🔷 Grow-only pools with avoidable duplication
- **Lowerer literal pools are never freed and never deduped**:
  `lower_v3.hh:196-201, 352-356` — every string/path literal occurrence copied
  into a process-lifetime static deque; the CU constant pool then interns a
  second copy (M-10). One copy is unreachable after emit. Intern at lower time
  into the M-10 pool (IR needs only a stable view).
- **PosSnapshot pool stores the file path twice per unique position**
  (`alloc.hh:4178-4258`): pool entry `PosSnapshot.file` + index key
  `PosSnapshotKey.file` — 2 × 60-120-char heap strings per (file,line,col),
  positions mostly unique. Intern file paths to a shared entry ⇒ ~1 copy per
  file. Feeds the MALLOC_SMALL bucket.
- **`Module::internSymbol` mirror resizes to global-table length per module**
  (`ir.cc:99-107`): any module touching one late-interned symbol allocates a
  ~global-size `vector<string>`; the mirror is diagnostics-only — delete it.
- **Dynamic attr names intern permanently into the global symbol table** on
  SELECT_DYN/HAS_DYN misses (`vm.cc:10576`; arbitrary strings from
  `hasAttr`/`getAttr` accumulate for process life). Minor; relevant to daemon
  reuse.

### 5.6 🔷 String-context side table: wrong shape for drv-heavy workloads
`alloc.hh:4281-4300`: global `unordered_map<const char*, vector<std::string>>`
of *encoded text tokens*; read path re-parses the same token through
`NixStringContextElem::parse` at every consuming drv (self-documented at
`primops.cc:309-316` — the parse memo exists but is debug-gated and its
pre-committed decision was never closed); context vectors are *copied* into
every derived string (`substring`/`baseNameOf`/`dirOf`/`concatStringsSep`);
entries for dead strings are never removed mid-eval; and the pointer-keying
creates the §2.6 reuse hazard. TW stores context inline per Value with shared
elements. The scoped lever (parsed, pointer-shared context objects — "lever
1.1" in the code's own comment) fixes CPU (re-parse), RSS (copies), and the
correctness hazard at once. Week-scale; the biggest drv-workload item in
primops.

### 5.7 🔷 Chain flatten every 16 layers: O(N²/16) copies, each generation pinned
`vm.cc:1816-1825` (chain-extend cap) + `1862-1919` (full Cursor merge):
`foldl' (//)` accumulators flatten every 16th merge — better than TW's O(N²)
CPU, but each flatten allocates a full-accumulator Bindings in the
never-reclaimed arena ⇒ ~N/16 dead generations become permanent RSS (TW's die
to Boehm). Concrete feeder of the "arena dead ~970 MB" bucket. Mitigations to
evaluate: recycle the previous flat generation via free-list bins (needs §5.1
metadata), raise/adapt the cap, or in-place growth for uniquely-owned
accumulators (`countDistinct`-owned leaf).

### 5.8 🔷 Bindings header: 16 of 24 B are variant fields dead in the dominant case
`alloc.hh:177-183`: `parent` (Chain-only) + `aux` (MapAttrs-only) present in
every Sorted Bindings — ~16 B × millions ⇒ tens of MB on M5-class evals.
A Kind-split allocation (Sorted header 8 B) is a pure layout change no prior
kill covers. Same class: `Closure::cu` (8 B/closure) derivable from
`desc->cu` — the identical redundancy FP-2a already removed from Thunk
(`closure.hh:67` vs `539-554`; requires OP_MAKE_CLOSURE to stamp `desc->cu`).

### 5.9 🔷 Import path waste (cold and warm)
- **Every import reads + SHA-256-hashes the whole file to compute the disk key
  even on warm hits; cold path reads the file twice and `resolveSymlinks()`
  three times** (`primops.cc:7185-7213` vs `:7830`). Keep `content` alive for
  the parse; memo `(path,mtime,size)→key` per process.
- **AOT mmap zero-copy is defeated by the first consumer**:
  `aot_cache::lookup` returns a `string_view` into the mmap; `disk_cache.cc:382-389`
  immediately materializes `std::string(*sv)` (same for EvalResults at
  `:477-481`) — add a view-returning overload into
  `deserializeCU(string_view)`.
- **`deserializeCU` warm-path waste**: temp `std::string` per string constant
  (`serialize.cc:949` — pass `r.strv()`), two full bytecode decode walks
  (symbol remap + position remap, fusable: `serialize.cc:1121,1129`),
  per-lambda formals re-sort even on identity remap (`:1143-1149`).
- **`SQLITE_TRANSIENT` on blob binds** forces an extra full-blob memcpy per
  insert; backing store outlives the step ⇒ `SQLITE_STATIC`
  (`disk_cache.cc:439-440, 520-521`).
- **ImportCache `cus` deque never shrinks** — not on LRU (results-only), not on
  invalidation (CU *deliberately leaked*, `primops.cc:6944-6948`) ⇒ unbounded
  growth per file edit in daemon/LSP reuse; this is malloc-side memory
  (distinct from the killed arena-pinned *results* eviction) and is exactly the
  bounded-memory plan's M2 target.
- **`derivationStrict` bytecode wrapper allocates ~6 intermediate collections +
  2 full attrset merges per derivation** (`bytecode_primops.cc:912-1030`,
  default-ON): `attrNames → map(2-entry attrset each) → filter → listToAttrs →
  base // {...} → // {outputs}`; the `derivation` wrapper adds another
  map+listToAttrs+2 merges. TW does one C++ pass, zero intermediates. Lands in
  the measured OP_ATTRS_UPDATE ~584 MB HNE bucket. Replace env-building with a
  single C leaf over the WHNF-forced `args`, keep only the force iteration in
  bytecode.

### 5.10 🔷 GC-side waste (bounded by GC's ≤7 % share, but pure)
- `dirtyContainers`: push-per-write, no per-object dirty bit, no insert dedup;
  self-documented >1 M entries transient on python3.drvPath (`gc.cc:1122-1126`);
  under `NIX_V3_MIDEVAL_GC` every entry is a mark root and the list is never
  cleared by the mid-eval collector (`mark_sweep.cc:2250-2268`) — mechanical
  over-retention that plausibly contributes to "blocks cluster 25-75 % live".
- Scavenger builds `liveTenuredRanges` (24 B per walked tenured object)
  unconditionally; consumed only under `V3_DBG_NURSERY_BRUTE`
  (`gc.cc:220-227` vs `1720-1727`).
- The scavenger's CU-IC `fwdBindings` walk is a no-op under the shipped
  Phase-D-Step-7 default (`gc.cc:549` returns immediately) — the whole R9 walk
  (`gc.cc:651-658, 703-710, 969-1000`) is dead weight in the default config.
- Mark phase: `std::upper_bound` block lookup per pointer + unconditional
  `markLinesForCell` call early-outing on a permanently-false gate
  (`mark_sweep.cc:113-132`, `alloc.hh:1705`).

---

## 6. Compile-time (PARSE+LOWER = 20-29 % of cold CPU)

- 🔷 **Double lowering of acyclic let-groups** (§5.4) — also the largest
  compile-time item.
- 🔷 **String-keyed scope resolution**: parser carries `std::string` per
  identifier (`parser/v3-parser.y:75-83`), `Scope.byName` is
  `std::map<std::string, VarId>` walked per variable reference
  (`lower_v3.hh:255-260, 303-341`), and whole scope maps are **copied** per
  formal / letrec entry / DAG entry (`:609, :849, :1082`) ⇒ O(K²) map copies
  per group. TW interns to Symbol at lex time and resolves to (level, displ).
- 🔷 **`optimise()` = ~20 full-module walks per CU**; `deadBindingElim` up to
  8 rounds × full-module `unordered_set` rebuild (`opt_dce.cc:164-206`) while
  the bounded 2-walk `deadBindingElimViaOccur` is **built, validated, and
  default-off** (`NIX_V3_OCCUR_DCE`); `computeFreeVars` does a global
  propagation sweep per fixpoint iteration (up to ~50 on module-system evals,
  `ir.cc:420-490`). VarIds are dense ints — use `vector<uint32> useCount` +
  worklist.
- 🔷 `opt_strict_call_unthunk.cc:897-922`: the "inline funnel diagnostic"
  (chase + recursive `bodyIsCloneable`) runs ungated per strict-hit App per
  round.
- Minor: `internPrimOp` linear scan per PrimOpCall emit (`emit.cc:1856-1861`);
  int/float constants appended per occurrence without per-CU dedup
  (`emit.cc:638-654`); `preassignSlotsInBlock` gives one frame slot per binding
  with no liveness reuse (`emit.cc:2069-2114`); stale "16 iterations" error
  text (`ir.cc:491-495`).

---

## 7. Latent / dormant (fix before their features activate)

- `fiber.cc:221-222` + `fiber.hh:87`: 16 MiB stack per fiber, wholesale
  `GC_add_roots`-registered (Boehm scans all of it; scan faults pages into
  RSS); zero non-test callers today.
- `ffi.cc:1099-1102`: `applyClosure` allocates a ~0.75 MB fresh VMState per
  FFI call; EvalScope handle ops take a global mutex each (test-only today).
- `runFunction*`/`runLambda` entry points reserve a 512 KB valueStack per fresh
  VMState (`vm.cc:13353-13412`) — fine at top level, wasteful per-callback.
- Thread-local Nursery has no destructor: thread exit leaks 32 MB + leaves a
  stale Boehm root over freed memory (`nursery.hh:649-658`) — blocking
  prerequisite for multi-threaded eval.
- `value_serialize.cc` shadow caches (`evalResultCacheMap`/`drvHashCacheMap`)
  are unbounded process-global blob maps (env-gated off; cap before enabling in
  a daemon).
- `ForceChainGuard` (`primops.cc:223-259`): ~120 LoC dead diagnostic machinery,
  zero instantiation sites — Rule-0 cleanup.
- Linux `getProcessRssBytes` uses `ru_maxrss` (**peak**, not current) for the
  "current RSS ≥ cap" check (`limits.cc:305-310`) — a transient spike
  permanently trips the cap on Linux; macOS uses current. Also the 100 ms
  SIGALRM watchdog stays armed for process life once any heap cap is set.
- `primV3CompileCallFlake` returns a pointer into a per-call-overwritten
  function-local static string (`v3_call_flake.cc:547-549`).
- `isBytecodePrimopInstalled` builds a `std::string` per query against
  `std::set<std::string>` from the optimizer (`bytecode_primops.cc:155-159`) —
  transparent comparator.

---

## 8. What this audit does NOT re-litigate (killed levers — do not conflate)

ImportCache **results** eviction (arena pins) · arena page-release / munmap /
Immix evacuation (no sparse blocks) · mid-eval GC as a *peak-RSS* lever ·
trivial-maybeThunk avoidance (#135 — see §4 for why the never-forced story is
different) · ValuePair 32→24 B shrink / kAlign 16→8 (FP-3) · default nursery
resize (L2) · runtime string dedup (≤25 MB ceiling) · Boehm tuning knobs ·
per-alloc TLS caching (T1a) · GET_LOCAL+ATTRS_SELECT superinstruction ·
genList fusion (parity at build; gap is callClosure dispatch — §3.9 attacks
that instead) · JIT (deferred by ceiling, not by falsification).

Findings above that *touch* these areas are code-level defects in the same
region, not revivals: e.g. §5.9 CU-deque eviction is malloc-side (≠ the killed
arena-side results eviction); §5.3 is allocation routing (≠ the killed pair
shrink); §3.9 reuseScope extension is the autoresearch L3 idea (graded
promising-untested, never falsified).

---

## 9. Priority matrix

Legend: effort H=hours, D=days, W=weeks. "BI" = must stay byte-identical
(full `--brute` 22/22 gate). Falsifiers per Rule 0 — measure on darwin-4.

| # | Item | § | Class | Effort | Falsifier / exit criterion |
|---|------|---|-------|--------|---------------------------|
| P0.1 | Gate/inline `bumpPrimOpCallCount`; fix raw counters (mergeBindings, selector/intrinsic, keepPap) | 1.1, 1.3 | CPU | H | darwin-4 firefox/M5 CPU delta; BI trivially |
| P0.2 | A/B `-Dv3_release=true` build; execute the A2 falsifier that never ran | 1.2 | CPU | H | ≥2 % wall or ≥20 MB RSS ⇒ flip packaging default |
| P0.3 | Split `limitsActive()` out of `kAnySlowGate`; local poll counter | 1.4 | CPU (measurement) | H | probe-config CPU == default-config CPU |
| P0.4 | **Re-baseline v3-vs-TW after P0.1-P0.3** | 1 | — | H | new authoritative rows in darwin4-rows.tsv |
| P1.1 | Type-check branch opcodes + lang fixtures | 2.1 | correctness | H | TW-parity on error cases; BI on valid code |
| P1.2 | `primSort` barrier + brute repro | 2.2 | correctness | H | repro fails before, passes after |
| P1.3 | Scope disk-hit catch to deserialize only | 2.3 | correctness | H | eval-error-through-import test |
| P1.4 | Huge-block interior-mark repro attempt; fix if reproducible | 2.4 | correctness | D | targeted repro |
| P2.1 | **Formals supplied-arg fast path** (kill per-formal wrapper thunks per call) | 4.1 | CPU+RSS | W | thunk-churn counter (allocated vs forced) + M5/ff CPU+arena; BI hard requirement |
| P2.2 | `OP_CALL_STRICT` consuming strictArgs at call site (the unbuilt lever) | 4.2 | CPU+RSS | W | never-forced % drops; BI |
| P2.3 | `or`-default lowered into else block; `inherit`-in-rec aliasing | 4.3 | CPU+RSS | D | thunk counters; BI |
| P3.1 | Chain-aware read IC + MapAttrs parent memo | 3.2, 3.3 | CPU | D-W | SELECT-heavy workloads (git/firefox); BI |
| P3.2 | Formals-call validation: needsForce guard, error-path string, merge-scan | 3.1 | CPU | H-D | callPackage-heavy eval; BI |
| P3.3 | Emit-side ATTRS_INIT pre-sort | 3.4 | CPU | D | BI (same final Bindings order) |
| P3.4 | valueEqual scalar fast path / small-buffer | 3.5 | CPU | H | string-compare micro + firefox |
| P3.5 | Barrier constexpr + hint flip + cached nursery bounds + post-construct short-circuit | 3.7 | CPU | D | ALLOC/BINDINGS profile share; BI |
| P3.6 | Dispatch: GC-requested flag word; code-ptr local; magic-static sweep (#768 pattern) | 3.8, 3.14 | CPU | D | dispatch share; BI |
| P3.7 | reuseScope for arity-1 strict primop callers (autoresearch L3) | 3.9 | CPU | D | map/filter-heavy workloads; BI |
| P3.8 | Single-pass concat + `std::move` at mkStringValueOwned sites | 3.6 | CPU | D | drvPath workloads; BI |
| P4.1 | LambdaDescriptor diet (name-ids + sparse side tables) | 5.4 | RSS | D-W | CU malloc bucket (M0.1 report); BI |
| P4.2 | Fix DAG double-lowering | 5.4, 6 | compile+RSS | D | lower-time + descriptor count |
| P4.3 | Lowerer literal-pool interning; PosSnapshot file-path interning; delete symbol mirror | 5.5 | RSS | D | MALLOC_SMALL bucket |
| P4.4 | String-context lever 1.1 (parsed shared context objects) | 5.6 | CPU+RSS+corr. | W | M5 CPU + MALLOC_SMALL; closes §2.6 hazard |
| P4.5 | derivationStrict env-building as one C leaf | 5.9 | CPU+RSS | D | mergeBindings-by-site table; BI (drv hashes!) |
| P4.6 | Import: single read+key memo; AOT string_view; deserializeCU fixes; SQLITE_STATIC | 5.9 | warm CPU | D | warm-run import timing buckets |
| P4.7 | Bindings Kind-split header; drop Closure::cu | 5.8 | RSS | D-W | arena bytes per workload; BI |
| P5.1 | Decide default gen-major: stop paying (no-op fires + IC clear) or enable metadata | 5.1 | CPU/policy | D | per-fire cost; aligns with bounded-memory M2 |
| P5.2 | Nursery "permanently full" latch; Boehm-root measurement | 5.2 | CPU | H-D | alloc-path branch count; Boehm GC time |
| P5.3 | dirtyContainers dedup bit; drop liveTenuredRanges when un-brute'd; skip dead R9 walk | 5.10 | GC CPU | D | scavenge time |
| P6 | Compile-time: occur-DCE default-on decision, scope-map interning, freeVars worklist | 6 | cold CPU | D-W | PARSE+LOWER share (cold + CI) |

**Suggested sequencing:** P0 (re-baseline — everything else is judged against
it) → P1 (correctness, all cheap except 1.4) → P2.1/P2.2 (the thunk cluster —
the only items here with plausible >10 % single-lever upside on both CPU and
RSS) → P3/P4 as a paired CPU/RSS sweep → P5/P6 opportunistically.

**Honest expectation.** The audit does not overturn the structural verdict:
flat transitive capture, switch dispatch, NaN-box decode, and
live-representation weight remain. But the defect inventory above — an
instrumented baseline, a mutex per primop call, an IC-less lookup path for the
most common attrset shape, one thunk per formal per call, and a compiler that
never de-thunks imports — is not "nothing fixable." If the P0-P3 items deliver
even half their plausible ranges, the *measured* gap shrinks materially before
any architectural program starts, and the P2 thunk cluster attacks the exact
churn that feeds the arena.

---

*Audit executed 2026-07-02 by 8-way parallel subsystem review + hand
verification of all headline findings. Reviewer transcripts:
session task outputs (afdd… alloc/headers, a3105… vm.cc-1, af9f3… vm.cc-2,
a4631… primops, a1160… GC, a7581… compiler, aac19… FFI/caching, a7921…
hygiene).*
