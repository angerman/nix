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

### 3.4 ✅→❌ `OP_ATTRS_INIT` re-sorts compile-time-constant names at runtime, every execution — **PROPOSED FIX FALSIFIED (2026-07-02); the runtime sort is LOAD-BEARING**
`vm.cc:9232-9243`: literal `{ a=…; b=…; }` inside a lambda re-sorts 24-B
entries + re-runs a statically-decidable duplicate check per call.
OP_ATTRS_REC_INIT already gets pre-sorted names from emit (`vm.cc:9386-9390`,
`emit.cc` LetRec path); ATTRS_INIT — made *more* common by the 2026-06-16
non-rec demotion lever — was left with the runtime sort. The audit proposed:
"Emit the (name,pos) trailer sorted and push values in sorted order; runtime
becomes a straight fill."

**That proposal is UNSAFE and was reverted (see WS-3 handback below).** The
runtime sort cannot be moved to emit time because it is load-bearing for the
CROSS-PROCESS DISK CACHE. `OP_ATTRS_INIT` pushes its values POSITIONALLY on the
stack (in trailer order), with no per-value operand. On a cross-process cache
hit, `remapAllSymbols` (serialize.cc) translates the writer's SymbolIds to the
reader's — an order-changing permutation in general (this is why deserialize
re-sorts formals, #814) — but it cannot reorder the already-emitted value-push
instructions to match a re-sorted trailer. Only the RUNTIME, sorting by the
reader's local SymbolIds, gets the order right. `OP_ATTRS_REC_INIT` escapes this
solely because its values are SLOT-indexed (`OP_ATTRS_REC_SET`) and the remap
rewrites the slots (serialize.cc:576-614) — `OP_ATTRS_INIT` has no such handle.
A safe variant (set a presorted flag on cold emit, CLEAR it on deserialize so
warm CUs re-sort) yields only a COLD-path win on a small-`n` sort — below the
darwin-4 noise floor — at the cost of a permanent operand-flag footgun (an
adversarial review of the first attempt found that four serialize walkers + the
CU comparator + the disassembler all read the operand as a raw count and would
overshoot the trailer once bit 23 was set → warm-cache corruption). **Net: do
NOT re-attempt.** Guardrail added: `test/run-attrs-init-cache-roundtrip-tests.sh`
(in the brute core suite) locks the warm-cache round-trip invariant.

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
arg in ~100 % of nixpkgs. The audit originally framed the fix as an
`OP_CALL_STRICT` consuming `Function::strictArgs` at the call site (documented
as future work at `ir.hh:610-616`).

> ⚠ **CORRECTED 2026-07-02 (same day, Addendum A.0):** cross-checking
> `lode/C1_P01_STRICT_CEILING_2026-06-23.md` shows this lever is already
> bounded below the bar. The static analysis proves strictness for **0.1 %
> (firefox) / 0.03 % (M5)** of the runtime-forced opportunity (142 of 100,781
> args), and the *entire* call-arg ceiling — perfect analysis + perfect
> consumer — is ~3.5 % of thunks ≈ ~1 % CPU
> (`lode/THUNK_LEVER_VERDICT_2026-06-28.md`). Runtime speculation (#143) was
> separately killed as un-deopt-safe. **P2.2 is CLOSED — do not build.** The
> imports-skip-strictness fact above stands as a fact, but fixing it is
> worthless at 0.1 % analysis coverage. Note this does NOT touch §4.1/§4.3:
> formals wrappers and or-default/inherit wrappers are *not* call-arg thunks
> (C1's instrument never counted them) and are not semantic laziness — see
> Addendum A.0.

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

**P4.4 step-0 FALSIFIER RESULT (2026-07-02, commit 362f87be9, darwin-4): the
CPU half is NOT MATERIAL — lever 1.1 is UNFUNDED FOR CPU.** Ran the in-code
falsifier (`V3_DBG_CTX_PARSE_MEMO=1`) on firefox.drvPath (drv-heavy, full
inputDrvs context) cache-off, user-CPU median-of-5: memo-OFF = **2.67s** vs
memo-ON = **2.66s** = **−0.4% (noise)**, far below the pre-committed ≥3% bar.
So the per-drv `NixStringContextElem::parse` re-parse the memo attacks is
negligible CPU; the cost the audit attributed partly to re-parse lives instead
in the token COPIES (an RSS/MALLOC_SMALL concern the beat-tw campaign already
graded modest/foundational) and the pointer-keyed side-table shape (the §2.6
hazard). VERDICT: do NOT build lever 1.1 as a CPU play. It survives ONLY as (a)
an RSS/span-sharing lever (bounded, foundational) and (b) the §2.6 correctness
hazard fix — both W-scale and BI-critical on drvPaths; neither is a
single-lever CPU win. M5 (more drv-heavy) is IFD-blocked on aarch64, but the
firefox null result already refutes the CPU hypothesis on a genuine drv
workload. The debug memo (`V3_DBG_CTX_PARSE_MEMO`) has now served its Rule-0
purpose and may be deleted when §5.6's RSS scope is decided.

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
  (distinct from the killed arena-pinned *results* eviction). *(⚠ scoped by
  M2.1, commit `fd8768a4f`, same day: CUs are fully live at peak ⇒ eviction
  recovers ~0 MB peak RSS — the finding's value is daemon-reuse boundedness +
  the invalidation leak only. See A.0 item 5.)*
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
| P2.1 | **Formals supplied-arg fast path** (kill per-formal wrapper thunks per call) — measure-first step 0 is decisive, see A.4 | 4.1 | CPU+RSS | W | wrapper share of thunk allocs ≥10 % → build; <5 % → close (A.4) |
| ~~P2.2~~ | ~~`OP_CALL_STRICT`~~ **CLOSED by C1 ceiling (0.1 % analysis coverage; full call-arg ceiling ≈1 % CPU)** — see §4.2 correction + A.0 | 4.2 | — | — | already falsified — do not build |
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
it) → P1 (correctness, all cheap except 1.4) → P2.1/P2.3 (the compiler-plumbing
thunk cluster; expectations tempered per A.0 — plausibly low-to-mid single-digit
% CPU, small RSS, decided by the A.4 measure-first steps) → P3/P4 as a paired
CPU/RSS sweep → P5/P6 opportunistically. **Execution details, dependency graph,
work packets, and the shorthand glossary are in Addendum A below.**

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

---
---

# Addendum A — execution handoff (2026-07-02)

Written for the engineer/agent executing §9. Read `src/libexpr-v3/CLAUDE.md`
FIRST (it is the process contract: Rule 0, the `--brute` gate, darwin-4
protocol, Phase-D constraint #0); this addendum does not repeat it, only
cross-references it.

## A.0 Corrections & reconciliation with the 2026-06-28 verdicts (read before touching P2)

Cross-checking `lode/THUNK_LEVER_VERDICT_2026-06-28.md` and
`lode/C1_P01_STRICT_CEILING_2026-06-23.md` after the audit was written:

1. **P2.2 (`OP_CALL_STRICT`) is CLOSED, not open work.** C1's runtime
   instrument measured the static `strictArgs` analysis proving **142 of
   100,781** runtime-forced call-arg thunks on firefox (0.1 %; M5 0.03 %), and
   bounded the *entire* call-arg ceiling (perfect analysis + perfect consumer +
   PAP completions) at ~3.5-10 % of thunks ≈ ~1 % CPU. Runtime speculation
   (#143) was separately killed as un-deopt-safe. §4.2 carries an inline
   correction; the §9 row is struck. Do not rebuild this in any form without
   new data that overturns C1.
2. **P2.1 and P2.3 SURVIVE the reconciliation — they are a different
   population.** C1 counted thunks passed *as call arguments*. The formals
   wrappers (§4.1) are allocated *inside the callee body on entry* — C1's
   instrument never saw them — and the 06-28 verdict's "65.7 % never-forced =
   real lazy data, structurally untouchable" classification does not cover
   them either: a wrapper whose body is `if param ? X then param.X else def`
   for a *supplied* argument is compiler plumbing around a pure attrset
   lookup, not semantic laziness. Removing it forces nothing user-visible
   (`param` is already WHNF at formals-validation time; `HasAttr` is pure; the
   underlying attr value stays lazy). Same reasoning for §4.3's or-default and
   inherit wrappers. **If the A.4/P2.1 step-0 measurement shows wrappers are a
   material share of the never-forced population, it partially overturns the
   06-28 "structurally untouchable" claim; if not, P2.1 closes.** Either
   outcome is a Rule-0 result.
3. **Tempered expectations for P2.1/P2.3.** #135 measured the v3-vs-TW total
   thunk-count excess at ~730 K on firefox ≈ ≤17-25 MB arena — so the RSS side
   is small. The CPU side (alloc + double-force per used formal, on every
   `callPackage`/`mkDerivation` call) is plausibly low-to-mid single-digit %.
   The earlier ">10 % single-lever upside" phrasing in §9 was corrected.
4. **Baseline supersession.** Every ×-factor and % in this document was
   measured (or inherited from measurements) on instrumented builds (§1).
   **P0.4's re-baseline supersedes all of them**; after P0 lands, judge every
   subsequent item against the new darwin4-rows.tsv rows, not against numbers
   quoted here.
5. **(added same day, after commit `fd8768a4f`) CU eviction for peak RSS is
   NO-GO per M2.1** — the CU-liveness instrument measured **0 MB cold CUs at
   peak** on firefox and M5 (every cached CU is referenced by a live
   thunk/closure during eval; CU bytecode is fully LIVE mid-eval), falsifying
   bounded-memory M2.2-M2.4. Consequences for this report: §5.9's ImportCache
   CU-deque finding retains its value ONLY for daemon/LSP-reuse unboundedness
   (the invalidation leak) and teardown memory — NOT peak RSS. Conversely this
   *strengthens* P4.1 (LambdaDescriptor diet): since CU bytes are fully live
   at peak, shrinking each descriptor reduces peak directly, which eviction
   cannot. The bounded-memory program's interim conclusion (only remaining
   reclaim lever = M3/the arena wall) is consistent with this audit's frame:
   the RSS items here attack *allocation and live-representation size*, not
   reclaim.

## A.1 Glossary of campaign shorthand used in this report

| Tag | Meaning | Where documented |
|---|---|---|
| Rule 0 | Every commit must kill/confirm a hypothesis | `src/libexpr-v3/CLAUDE.md` |
| PhD-6 | Missed-root UAF class: tenured container holding nursery payload without barrier + scavenger walker | CLAUDE.md constraint 0; `lode/NURSERY_PHASE_D_DESIGN_2026-05-18.md` |
| M-3 trap | Re-enabling per-op major GC (`alloc.hh g_majorGcEnabled`) alongside the always-on nursery = UAF | `alloc.hh:1170` comment; CLAUDE.md constraint 0 |
| M-2 | Pointer-keyed side-table aliasing class (stale entry inherited by a reused address) | `alloc.hh:4313` comment |
| M-10 | Process-wide string-constant intern pool | `serialize.cc:39-53` |
| M-1, C-1, C-3, C-13, C-21, P-1…P-6 | Codebase-review action items (C-1 = chain shared-parent write corruption; C-3 = IC name-validation; P-6 = kAnySlowGate folding) | `lode/CODEBASE_REVIEW_2026-06-11.md`, `_2026-06-15.md` |
| #131-#143 | BEAT_TW-v2 todo numbers (#134 ImportCache-results eviction KILLED; #135 trivial-maybeThunk KILLED, commit `207c9b414`; #136 page-release KILLED `9729bf873`; #139 CU-shrink RCA `134d04911`; #143 speculative strictness KILLED) | `lode/BEAT_TW_PLAN_2026-06-23.md`, `lode/ARCH_BEAT_TW_PROGRAM_2026-06-23.md` |
| #696 | App-memo-drop CPU-regression class | App-memo comments in vm.cc |
| #733 / #768 | Counter-gating sweep / magic-static→namespace-scope promotion (~2 % measured) | `alloc.hh` + `barrier.hh:145-153` comments |
| #774 / #776 | Strictness passes kept root-only (1 elision / 40,961 Apps) / inline funnel 0-of-12,356 | `run.cc:250-266` comment |
| A2 | `v3_release` strip option + its never-run falsifier (≥2 % wall or ≥20 MB) | `meson.options`, `meson.build:26-30` |
| C1, P0.1/P0.2 (program) | Strict-ceiling RCA / warm head-to-head (NOT this report's P0.x) | `lode/C1_P01_STRICT_CEILING_2026-06-23.md`, `lode/P02_WARM_HEADTOHEAD_2026-06-23.md` |
| FP-2 / FP-3 | Thunk header at 24 B floor (done) / pair-tax shrink (closed, not built) | `lode/THUNK_LEVER_VERDICT_2026-06-28.md` recap; git notes |
| L1 / L2 / L3 | countDistinct memo (SHIPPED) / nursery sizing (CLOSED, 32 MB stays) / reuseScope extension to arity-1 strict primop callers (PROMISING-UNTESTED — this report's P3.7) | `lode/PROFILE_AT_SCALE_2026-06-21.md`, `lode/L2_NURSERY_SIZING_2026-06-22.md`, `lode/AUTORESEARCH_V3_DESIGN_2026-06-16.md` |
| T1a | Per-alloc TLS caching lever FALSIFIED | profile-campaign git notes (2026-06-22) |
| WS-A | Chain-lookup SELECT workstream | comments at vm.cc:9854 |
| #821 | mergeBindings-by-site instrumentation | `run.cc:516-580` |
| GC_AUDIT_ROUND_2 | GC audit that documented the deepForceList stale-writeback (§2.5) | `lode/GC_AUDIT_ROUND_2_2026-05-21.md` |
| M0-M4 (bounded memory) | Active bounded-memory plan; M2 = CU-graph eviction (aligns with §5.9) | `lode/BOUNDED_MEMORY_PLAN_2026-06-29.md` |
| brute battery | The 22-suite pre-merge gate (1 MB nursery + audit stress) | CLAUDE.md "Pre-merge gate" |

## A.2 Invariants & traps checklist (applies to EVERY item)

1. **Gate:** `nix develop -c bash src/libexpr-v3/test/all-v3-tests.sh --brute`
   → expect **22/22** before every commit. Subsets (lang/drv-parity/…) are
   inner-loop only; they have shipped regressions before.
2. **Byte-identity (BI):** where the §9 matrix says BI, `hello`/`git`/
   `firefox` drvPath must be byte-identical pre/post change (and to TW where
   the suite checks it). Extra care on drv-hash-critical surfaces: string
   context, sort stability (C-21), attrset entry order.
3. **Moving-GC semantics are always on** (nursery + Phase-D barriers +
   gen-major; opt-outs retired). Any new tenured container that can hold
   nursery payloads needs a post-construct/entry barrier AND scavenger-walker
   coverage, or it is a PhD-6 UAF. When in doubt, the brute battery's 1 MB
   nursery + `V3_DBG_NURSERY_AUDIT=1` is the detector.
4. **Never** set `alloc.hh g_majorGcEnabled` true (M-3). `NIX_V3_EVAC` stays
   unrevived.
5. **Perf numbers only from darwin-4** (`aarch64-darwin-4.lan`), commit-stamped
   in `bench/baselines/darwin4-rows.tsv` + git-noted. Laptop = correctness
   only (~5-10 % noise floor). Until P0.3 lands, do NOT measure with
   `NIX_V3_MAX_*` env vars set (they activate per-op slow gates, §1.4);
   after P0.3, re-verify they are cost-free.
6. **Every bug fix ships a failing-first regression test** (positive +
   negative); keep repros forever (`test/repro-*.nix` convention).
7. **Rule 0 commit bodies** (which hypothesis does this kill/confirm); no new
   env gate without an inline retirement criterion at the first getenv site;
   `test/lint-no-inline-getenv.sh` must pass.
8. **V3-NATIVE:** never route pure data ops through the FFI (LESSONS §1.2).
9. **Measure-first:** every D/W-scale packet starts with its instrument step
   and pre-committed threshold. Below threshold ⇒ close with a dated doc note
   here, don't build.

## A.3 Workstreams, dependencies, conflicts (parallelization map)

At most ONE agent per workstream (they share files); workstreams marked ∥ may
run concurrently.

- **WS-0 — de-instrumentation + re-baseline** (P0.1→P0.2→P0.3→P0.4). Runs
  FIRST and ALONE; blocks all perf judgments. Files: primops.cc (counter),
  vm.cc (mergeBindings counters, kAnySlowGate, selector/intrinsic counters),
  meson options/packaging, bench scripts.
- **WS-1 — correctness** (P1.1, P1.2, P1.3 independent of each other; P1.4
  timeboxed repro attempt). ∥ with WS-6. P1.2 must land before WS-3's P3.5
  (both touch barrier call sites).
- **WS-2 — lowering/thunk plumbing** (P2.1 → P2.3; P2.2 closed per A.0).
  Files: `cli/lower_v3.hh`, emit.cc, vm.cc call/validation paths. Sequential
  within; conflicts with WS-3 (vm.cc) and WS-4/P4.1 (emit.cc, closure.hh) —
  coordinate or serialize.
- **WS-3 — VM hot path** (P3.1-P3.8). Nearly all touch vm.cc ⇒ one workstream.
  P3.5 after P1.2. P3.3's emit half coordinates with WS-2.
- **WS-4 — RSS/CU** (P4.1-P4.7). P4.6 (disk_cache/serialize) is independent ∥
  anytime after WS-0. P4.1 (closure.hh/serialize.cc/emit.cc) serializes
  against WS-2. P4.4 is wide (alloc.hh/primops.cc/vm.cc/ffi.cc/mark_sweep.cc)
  — run it when WS-2/WS-3 are quiescent. P4.5 is BI-critical (drv hashes).
- **WS-5 — GC/policy** (P5.1 decision gates P5.3; P5.2 independent). Note
  P5.1 should be decided WITH the bounded-memory plan owner (M2 needs the same
  metadata).
- **WS-6 — compile-time** (P6). Independent except emit.cc contact with WS-2.

Suggested schedule: WS-0 alone → (WS-1 ∥ WS-6 ∥ WS-4/P4.6) → (WS-2 ∥ WS-3-non-
emit items) → WS-4 remainder → WS-5.

## A.4 Work packets

### P0 packet (hours each; land as separate commits)
1. `bumpPrimOpCallCount`: either gate behind a namespace-scope cached
   `NIX_VM_STATS` bool, or (better) replace with a per-`PrimOp` inline
   `uint64_t` counter (registry entries are stable; the exit dump iterates the
   registry). Keep the exit dump working. Also pass/plumb the existing
   `bumpStats=false` discipline consistently (vm.cc:6112 vs 7652).
2. mergeBindings: convert the raw `allocStats()` bumps/histograms
   (vm.cc:1631-1663, 1761, 1886, 2084) to `V3_STATS_*` macros; promote the 4
   function-local static gates (vm.cc:1673-1699) to namespace-scope
   `inline const` (#768 pattern).
3. Gate `selectorLambdaCalls` (vm.cc:6579, 13361, 15218),
   `intrinsicExtends/ComposeCalls` (vm.cc:15148/15178), `g_keepPapDisarmCount`
   (vm.cc:3283).
4. `kAnySlowGate`: remove `limitsActive()` from the fold (vm.cc:3841-3844);
   poll limits via a plain function-local countdown re-armed at dispatchLoop
   entry (keep the 10 000-op interval); verify with limits set that per-op
   cost is unchanged vs unset.
5. Execute the A2 falsifier: A/B `-Dlibexpr-v3:v3_release=true` on darwin-4
   (firefox + M5, cache-off + warm). ≥2 % wall or ≥20 MB ⇒ flip the default in
   packaging + bench scripts; either way record the number.
6. **P0.4:** re-run the standard baseline (bench/beat-tw-compare.sh + warm
   mode; darwin-4), append rows to darwin4-rows.tsv, git-note, and add a dated
   note at the top of this file stating the new authoritative numbers.

### P1.1 packet (branch-opcode type check)
Add `if (!v.isBool()) throwTypeError(...)` (predicted-true isBool) to
OP_BRANCH_FALSE / OP_AND_BRANCH / OP_OR_BRANCH / OP_IMPL_BRANCH /
OP_R_BRANCH_FALSE (vm.cc:4981-5052). Match TW's message ("expected a Boolean
but found …" class). Tests: `if 1 then "a" else "b"`, `1 && true`, `"x" ||
true`, `1 -> true` must all error identically to TW; valid-code BI unchanged.
Check the lang-test suite for existing error-format fixtures first.

### P1.2 packet (primSort barrier) — repro recipe included
- **Why suites missed it:** the UAF window needs (a) `result` TENURED (nursery
  full at alloc time), (b) source elements NURSERY-resident (only direct
  Closure/Thunk/ListVec values qualify — `map`-produced elements are tenured
  App pairs, which is why sort-over-map fixtures never trip it), (c) a
  scavenge at the NEXT outer safepoint before the list dies. Scavenge cannot
  fire during the sort itself (exitDepth>0), so the corruption is deferred —
  classic PhD-6.
- **Repro:** fixture whose source list holds directly-allocated
  nursery-eligible cells, e.g. literal inner lists
  (`[ [3 "c"] [1 "a"] [2 "b"] ]` scaled up) or closures sorted via a key
  attrset; sort at top level; then allocate churn at exitDepth==0 to cross the
  75 % trigger; then deep-read the sorted result. Run under
  `NIX_V3_NURSERY_SIZE=1 V3_DBG_NURSERY_AUDIT=1 V3_DBG_NURSERY_BRUTE=1`
  (optionally `V3_DBG_GC_STRESS`) — the audit must flag a missed root
  PRE-fix and be clean POST-fix. If the window won't open with literals,
  force `result` tenuring by pre-filling the nursery first.
- **Fix:** `Barrier::listPostConstructBarrier(result)` before `out.mkList`
  publication — copy the exact placement from `primFilter`
  (primops.cc:1669). Add the fixture to the brute battery permanently.

### P1.3 packet (disk-hit catch)
Split the try at primops.cc:7271-7789: only `deserializeCU` (+ subChecks
verification) inside the corrupt-blob catch; `run()` executes OUTSIDE it. On
eval-error the CU must stay in `cus` (matching the invalidation path's
deliberate-leak policy) and the error must propagate — no silent re-parse +
re-run. Test: an import whose evaluation throws, evaluated twice warm — assert
single execution + stable error.

### P2.1 packet (formals supplied-arg fast path)
- **Step 0 — measure (0.5 d, decides everything):** tag formal-wrapper
  descriptors at lower time (a flags bit on `LambdaDescriptor`, set for
  Functions minted at lower_v3.hh:599-635) and count their runtime
  allocations + forces under `NIX_VM_STATS` on firefox + M5.
  **Pre-committed:** wrappers ≥10 % of total thunk allocs → build; <5 % →
  close (dated note here + update A.0); 5-10 % → judgment call with a CPU
  estimate (alloc-cost × count + double-force × forced-count).
- **Design sketch (v1 scope = the demoted "independent formals" path ONLY;**
  the sibling-referencing `formalsRec` path keeps current lowering**):** at
  formals-validation time (vm.cc OP_CALL ~6645-6780) `param` is already WHNF
  and formals are checked. Bind each *supplied* formal's local directly to the
  attrset entry Value (itself usually a lazy thunk — laziness of the value is
  preserved); allocate the wrapper thunk ONLY for missing-with-default.
  Options: (a) new `OP_BIND_FORMALS` doing one O(n+m) merge scan over sorted
  formals × sorted Bindings, filling K locals + allocating default thunks for
  the missing subset (this also subsumes P3.2's merge-scan fix); (b) keep the
  lowering shape but emit per-formal `HasAttr`-guarded direct binds. Prefer
  (a).
- **Invariants:** default exprs stay lazy (allocated only when the attr is
  missing); defaults referencing siblings unchanged (rec path); `@`-binding
  unchanged; missing/unexpected-attr error text unchanged (coordinate with
  P3.2, same code region); full BI gate.
- **Expectations (honest, per A.0):** RSS small (≤ ~25 MB firefox-class);
  CPU plausibly low-to-mid single-digit % on formals-heavy workloads.
  Validation: thunk-churn deterministic counters (profile-at-scale), brute,
  BI, darwin-4.

### P2.3 packet (or-defaults + inherit aliasing)
- Measure-first with the same descriptor-tagging trick (or-default thunks and
  inherit-wrapper thunks separately); pre-commit ≥2 % of thunk allocs each.
- **or-default:** lower the default body inside the else-block of the select
  (lower_v3.hh:1246 + emitSelectChain) instead of thunkifying in the parent
  block. Watch multi-step chains (`a.b.c or d`): the default fires on ANY
  missing step — the else block is shared across steps; ensure single
  lowering with shared branch target, and that a default that *is* a
  side-effect-free literal keeps the existing trivial escape.
- **inherit-in-rec:** bind the name to the parent-scope VarId directly
  (aliasing, as non-rec attrset inherit already does at lower_v3.hh:1166-1169)
  instead of minting a wrapper Function (lower_v3.hh:840-843). Trap: the
  alias must resolve in the PARENT scope (pre-rec), not the rec scope —
  shadowing tests required (`let x = 1; in let x = 2; inherit-in-rec…`
  shapes + `inherit (e) x` two-layer case).

### P4.4 packet (string-context representation)
- **Step 0 — run the falsifier the code already defines** (primops.cc:309-316):
  `V3_DBG_CTX_PARSE_MEMO=1` on firefox + M5; the in-code pre-commit is
  "firefox CPU drop ≥3 % ⇒ fund lever 1.1". Also count side-table bytes +
  entry duplication on M5 (drv-heavy = the workload that matters).
- **Target design ("lever 1.1"):** global intern pool of PARSED context
  elements (u32 id each); side table maps `char* → sorted id-array`; derived
  strings share/merge id-arrays (no per-string token copies, no re-parse);
  FFI crossings encode from the parsed form on demand.
- **Files:** alloc.hh:4281-4321 (table), primops.cc context helpers
  (~299-426, 2843-3008, 4546+), vm.cc OP_STR_CONCAT (11820-12110),
  ffi.cc crossings (~15 `to_string()`/parse sites), mark_sweep.cc
  sweepStringContextRanges (1654-1672) must track the new table shape.
- **Open design decision** (document the choice): the table stays keyed by
  buffer pointer ⇒ `unsafeDiscardStringContext`/`appendContext` still need a
  distinct key (clone payload — status quo) OR a context-id slot moves into
  the Chars cell header (+bytes per string, kills the M-2 hazard class
  outright). Bound both before choosing.
- **Gate:** BI on drvPaths is absolute (context = inputDrvs). Expect CPU on
  M5-class (kill the per-drv re-parse) + MALLOC_SMALL RSS (kill token copies).

### P3.1 note (chain-aware IC)
Key `(leaf Bindings*, SymbolId) → (ownerLayer*, slot)` with the flat IC's
4-way + name-validation discipline (C-3). Chain STRUCTURE is immutable after
construction (only leaf-owned entry writebacks occur — C-1), so a resolved
read slot is cacheable; gen-major already clears ICs (vm.cc:4112). The
MapAttrs parent memo (§3.3) must write only into leaf-owned storage or a
side memo — never the shared parent (C-1). Measure per-site IC hit rates
first (cheap counter) to size both this and the §3.14 megamorphic question.

## A.5 Validation & measurement command reference

```bash
# pre-merge gate (expect 22/22):
nix develop -c bash src/libexpr-v3/test/all-v3-tests.sh --brute
# pinned nixpkgs for ANY <nixpkgs> eval:
source src/libexpr-v3/test/nixpkgs-pin.sh
# deterministic counters + phase breakdown (exact counters; CPU % directional):
bench/profile-at-scale.sh          # or: make profile / make profile-note
# baseline comparison (cold + WARM=1):
bench/beat-tw-compare.sh
# darwin-4 sync + build (push key often fails):
rsync -az --exclude='*.o' --exclude='*.dylib' src/libexpr-v3/ \
  aarch64-darwin-4.lan:Projects/iohk/nix/src/libexpr-v3/
ssh aarch64-darwin-4.lan 'cd ~/Projects/iohk/nix && nix develop -c ninja -C build src/libexpr-v3/v3-eval src/nix/nix'
# darwin-4 caveat: its checkout lags the rsync'd binary — pass COMMIT=<laptop HEAD> to profile scripts.
```
Probe-safety env vars (`NIX_V3_MAX_WALL_TIME` etc.): until P0.3 lands these
activate per-op slow gates (§1.4) — never measure with them set.

## A.6 Handback format

Per phase/workstream, append a dated section to THIS file (do not silently
diverge from the audit) containing: commits landed; each falsifier's measured
result vs its pre-committed threshold (including closes — a kill is a
deliverable); corrections to any audit claim found wrong; new darwin-4 rows
(also in darwin4-rows.tsv + git notes). Update the project memory index the
same way the campaigns did.

---
---

# Handback — WS-0 (de-instrumentation + re-baseline), 2026-07-02

**Commits landed** (branch `angerman/2.35-eval-profiling-v2`):
- `d6729f753` P0.1a — gate `bumpPrimOpCallCount` behind a cached `NIX_VM_STATS` bool.
- `016bef7f0` P0.1b — gate mergeBindings #821 counters (V3_STATS_BLOCK); hoist the 4 chain knobs to file scope (#768).
- `2b565401d` P0.1c — gate selector/intrinsic/keepPap fast-path counters (V3_STATS_INC/BLOCK).
- `2d7efdb97` P0.3 — decouple resource-limit polling from `kAnySlowGate` (plain-local countdown; poll at loop top).

Each landed under full `--brute` 22/22 + an independent adversarial review
(all NOT-REFUTED). P0.4/P0.2/P0.3 numbers git-noted to `2d7efdb97`; rows in
`bench/baselines/darwin4-rows.tsv`.

**Falsifier results vs pre-committed thresholds:**
- **P0.2 (A2, `-Dv3_release=true`, ship if ≥2 % wall OR ≥20 MB RSS): NO-GO.**
  darwin-4 A/B (2d7efdb97): firefox cold release 2.69 s/679 MB vs false
  2.68 s/677 MB (+0.4 % / +2 MB, *worse*); M5 cold 11.07 s/3005 MB vs
  10.96 s/2950 MB (+1.0 % / +55 MB, *worse*); warm ~0 both. Within noise,
  marginally worse. **Keep `v3_release=false`.**
- **P0.3 (probe-config CPU == default-config CPU): PASS.** firefox user-CPU
  median-of-5, caps UNSET 2.68 s vs caps SET 2.69 s = +0.4 % (noise). The
  CLAUDE.md-mandated `NIX_V3_MAX_*` probe vars are now cost-free (§1.4 was
  real, now fixed).
- **P0.4 re-baseline (authoritative, supersedes every ×-factor in this doc):**
  COLD firefox 3.62×CPU/1.89×RSS (2.68 s/677 MB), M5 3.04×/3.00×
  (10.96 s/2950 MB); WARM firefox 2.43×/1.64× (1.80 s/586 MB), M5 1.81×/2.26×
  (6.54 s/2217 MB). vs the audit's instrumented numbers: firefox warm
  2.49→2.43×, cold 3.68→3.62×; M5 cold v3 11.10→10.96 s — a small, real
  default-build CPU win from P0.1a.

**Corrections to audit claims:**
1. **§1.2 overstated.** The "benchmarks measure an instrumented build ⇒
   unknown CPU tax" concern is now bounded: the ONLY *material* instrumentation
   was §1.1's per-primop `mutex + std::string + unordered_map` counter (fixed
   by P0.1a — it ran in *every* build regardless of `v3_release`). The ~97
   `V3_STATS` byte/histogram/fastpath counters (§1.2/§1.3) are noise-level:
   stripping them via `v3_release=true` bought <2 % wall and <20 MB RSS (P0.2
   NO-GO). So the published gap was NOT meaningfully inflated by the V3_STATS
   counters; it *was* slightly inflated by the primop counter, now removed.
2. **P0.1a design pivot (§1.1's "better inline counter" is WRONG).** The
   audit's preferred "per-`PrimOp` inline `uint64_t callCount` field" was
   implemented, then FALSIFIED by execution: some PrimOps are `static const`
   (read-only memory — the `plusOnePo`/`returnSecondPo` fixtures in
   `test/smoke.cc`), so a mutable inline counter faults (EXC_BAD_ACCESS/SIGBUS
   — v3-smoke exit 138) the moment the VM dispatches them. Shipped the audit's
   *first* option instead (name-keyed side map gated on cached `NIX_VM_STATS`);
   it only ever READS `po->name`, safe on read-only PrimOps. This is a latent
   landmine for any future production `static const PrimOp`, now documented at
   the counter.
3. **P0.1c scope note.** A.4 packet 3 named the vm.cc counters; the nursery
   `tryAlloc` counters (§1.3's 5th bullet, nursery.hh:84-97) were deliberately
   NOT gated — `nursery.hh` cannot reach the `V3_STATS` macros without breaking
   the `alloc.hh`↔`nursery.hh` include cycle, and they are plain cache-resident
   increments (negligible). Revisit only if a future profile shows residual.

**Net:** WS-0 removed the one material instrumentation cost (primop counter),
made probe-config timing cost-free (P0.3), proved the V3_STATS-counter tax is
noise (P0.2 NO-GO ⇒ no packaging change), and re-baselined. Every subsequent
item is now judged against the P0.4 rows above, not the numbers quoted earlier
in this document.

---
---

# Handback — WS-1 (correctness), 2026-07-02

**Commits landed:**
- `c3657c94b` P1.1 — branch opcodes (OP_BRANCH_FALSE / AND / OR / IMPL /
  R_BRANCH_FALSE) reject non-Boolean conditions with TW-parity errors.
- `c9948463f` P1.2 — primSort `listPostConstructBarrier(result)` (PhD-6).
- `5235df5e9` P1.3 — disk-cache-HIT catch scoped to deserialize; run() errors
  propagate.

Each shipped a failing-first regression test wired into the core/brute battery
(now 25 suites) and an independent adversarial review (all NOT-REFUTED); each
landed under full `--brute` 25/25.

**Falsifier / verification results vs the always-add-tests + failing-first rule:**
- **P1.1** (test/run-branch-bool-typecheck-tests.sh, 17 cases): 6 negative
  (`if 1`, `1 && true`, `"x" || true`, `1 -> true`, forced/slot cond, `null`)
  now error with byte-parity to TW's `expected a Boolean but found <type>:
  <value>` (eval.cc:1228) — verified v3==TW; 11 positive valid-boolean cases
  unchanged + TW-equal. Pre-fix the negatives *evaluated* (`if 1`→then,
  `1 && false`→false); post-fix they throw. Corrections: OP_NOT already errors
  (isTrueValue, non-parity message — out of §2.1 scope); OP_ASSERT unchanged;
  string-with-context/primop-named/external message deviations are edge cases
  that never occur in boolean position.
- **P1.2** (test/run-primsort-barrier-tests.sh): a 40 000-element literal-list
  sort fills the 1 MB nursery so `result` tenures with nursery-resident cells;
  under `NIX_V3_NURSERY_SIZE=1 V3_DBG_NURSERY_AUDIT` the scavenge audit flags
  ~33 000 "reachable via ListVec(sz=40000).elems[N] lastWriter=(no-recorded-
  writer=raw/bulk-path)" missed roots PRE-fix, **0** POST-fix (both directions
  verified; also clean under +BRUTE). Correction to the A.4 recipe: genList/map
  sources do NOT trip it (tenured App-pair elements) — a LITERAL nested list is
  required to make the elements direct nursery cells.
- **P1.3** (test/run-primimport-eval-error-tests.sh): a throwing import is
  disk-cached (CU written before run()), then evaluated in a second process
  (real disk HIT). A seq-forced trace marker (a plain `trace msg (throw)` does
  NOT fire — trace's value arg is strict, so the throw beats the print) counts
  warm-HIT executions: **A/B verified PRE-fix = 2 (dup re-run), POST-fix = 1**;
  stable error; post-error cache uncorrupted.
- **P1.4 (§2.4 huge-block interior-mark UAF) — CLOSED, NO FIX (fix-if-
  reproducible rule).** Mechanism CONFIRMED by code inspection: visitSlot's
  interior-owner rescue (mark_sweep.cc:313-350) calls `findContainingCellStart`
  which returns null when `cellMetaEnabled()` is false (the default), so a
  Tag::Slot into a huge (≥ kHugeCutoff = 4 MB) Bindings interior marks the
  entry value but NOT the container's block-begin; the huge-block reclaim
  (mark_sweep.cc:2404) tests `isMarked(block-begin)` and frees it. Reachable by
  default (huge blocks are calloc'd, pushed to `hugeBlocks`, and reclaimed).
  **Repro NOT achieved** in two bounded attempts (250 k-entry attrset with
  in-place selects + churn → correct 374 625 000; mapAttrs + `//` overlay +
  churn → correct 62 375 507), because gen-major is exitDepth==0-gated so the
  transient in-place-force slots (created at exitDepth>0) are gone when it
  fires — the M5 "slot-only-reachable huge Bindings at exitDepth==0" is a
  persistent, M5-specific pattern, and M5 is IFD-blocked on aarch64. The UAF is
  latent even when triggered (calloc'd block → `std::free`; needs freed-memory
  reuse before the dangling access to manifest). Per the fix-if-reproducible
  rule this is CLOSED without a fix (an unvalidated change to GC-critical
  mark_sweep.cc is higher-risk than the rare, unmeasured bug).
  **Recommended fix when M5-validation is available:** in visitSlot's
  interior-owner rescue, handle the huge-block case independently of
  `cellMetaEnabled()` — resolve an interior slot pointer to its huge-block
  begin via an `arena.hugeBlockRanges()` range lookup (which needs no cell
  metadata) and mark THAT, so the reclaim's `isMarked(block-begin)` succeeds.

**Discovered (orthogonal to WS-1, logged for follow-up):** the brute-audit
firefox-name workload intermittently (~1-in-7; 1 hit then 7 consecutive clean)
reports "SCAVENGE BRUTE: 40 tenured words point into nursery" — a rare
pre-existing scavenger missed-root, P1.3-orthogonal (primImport touches no
walker). A real (if rare) latent bug; captured as a separate investigation
task (identify the object class/field at the hit address → the missing gc.cc
walk → failing-first repro + fix).

**INVESTIGATION RESOLVED (2026-07-02, task #14): known latent, RCA'd,
default-path barrier-complete, currently UNREPRODUCIBLE — no default-path fix
warranted.**  A dedicated investigation (56+ stressed evals: 30× firefox-name @
1 MB nursery + AUDIT/BRUTE, `V3_DBG_GC_STRESS` variants on hello/git, and a full
`run-brute-audit.sh` @ STRESS=50 = 17/17) could NOT reproduce a single LIVE
missed-root on this branch — the earlier once-off (6-case) brute hit this
session did not recur (plausibly closed by the WS-1 primSort barrier
`c9948463f`, or simply the rare window not opening).  The bug is already RCA'd
in `SAFEPOINT_FOUNDATION_S2.1_RCA_2026-06-25.md` (fix `6d6805a4a`): the holder is
an ORIGINAL (non-relocated) tenured `Bindings` at `entries[k].value` holding a
nursery `Closure`; the mechanism is the scavenger's `fwd*()` Step-7
"trust-the-dirty-list" optimization (`gc.cc:132` `phaseDStep7Active`, default-on)
early-returning root-reached tenured cells WITHOUT a transitive walk, so a
tenured→nursery edge is caught only if the write went through a `barrier.hh`
helper into `dirtyContainers`.  A raw/bulk write bypassing the barrier is then
in neither the walk nor the remembered set → missed root.  It is
LATENT-BENIGN on the default path (scavenge fires only at `exitDepth==0`, so the
post-write/pre-overwrite window is almost never open) and surfaced
deterministically only under EVAC's raw-memcpy relocation — which is ALREADY
FIXED (`6d6805a4a` makes Step-7 full-walk under `NIX_V3_EVAC`).  A static audit
of every raw/bulk `Bindings`/`ValuePair`/`ListVec`/`Env` writer on the default
path found COMPLETE post-construct-barrier coverage (OP_ATTRS_INIT/_DYN, OP_UPDATE
merge, listToAttrs/mapAttrs/intersectAttrs, all `cellWrite`/`bindingsSetValue`
sites).  **Disposition: no reproducible default-path bug to fix; do NOT chase a
specific site.**  Recommended future HARDENING (not a correctness fix, so not
built here — unreproducible ⇒ no failing-first test possible): the RCA's
belt-and-suspenders instrument (extend `cellWriteSiteMap` to record EVERY
`Bindings.entries[].value` store incl. raw ones) so any future recurrence
self-names its writer for a one-line barrier add — worth landing WITH the S2.1
safepoint / EVAC work that needs Step-7 disabled anyway.

**Net:** WS-1 fixed both live correctness divergences (P1.1 semantic, P1.2
missed-root UAF) and the latent import-cache exception-safety bug (P1.3), each
with a failing-first regression permanently in the brute battery. P1.4 is a
documented close (mechanism real, repro M5-scale/unavailable, fix deferred).

---
---

# Handback — WS-2 (lowering/thunk plumbing), 2026-07-02 (in progress)

## P2.1 step-0 measure — GO (commit `09bb48f51`)

Per-formal WRAPPER thunks are a MATERIAL share of runtime thunk allocation
(audit §4.1).  firefox (cache-off, deterministic per-descriptor allocCount via
a temporary `isFormalWrapper` flag): **alloc = 313,787 = 12.79 %** of 2,453,220
descriptor thunk allocs (8,712 / 110,978 descriptors); 166,468 forced = 53.1 %
⇒ **~47 % of wrapper thunks are NEVER forced.**  ≥10 % threshold ⇒ GO.  This
partially overturns A.0's tempered "maybe-close" expectation and the 06-28
"structurally untouchable" framing for the ALLOC side.

## P2.1 build — design (b) FALSIFIED; design (a) is the remaining path (DEFERRED)

**Design (b) — bind no-default formals directly to `AttrSelect{param, X}` at
lambda entry (instead of a per-call wrapper thunk) — is FALSIFIED by
execution.**  Implemented + built; hello/git/firefox drvPath all threw
**"infinite recursion encountered … referencing `config` in `imports`"** (the
nixpkgs module system).  Root cause: the wrapper thunk defers both the
attr-select AND the `forceVal(param)` to the formal's USE-time; the module
system builds its config fix-point by evaluating formals in a specific
use-order, so accessing a formal (selecting through `param`, forcing `param`
as a mid-construction fix-point) at lambda ENTRY breaks the cycle.  **The
wrapper thunk's deferral-to-use is load-bearing, not pure plumbing** — so the
"12.79 % is removable" reading is too optimistic for design (b).  Reverted;
hello.drvPath byte-identity confirmed restored.  Correction to §4.1: the fix
"bind param.X directly" is unsafe if it forces/selects at entry.

**Design (a) — a new `OP_BIND_FORMALS` at OP_CALL formals-validation (where
`param` is ALREADY WHNF, safely, exactly as the wrapper path relies on) doing a
RAW `Bindings::lookup` per supplied formal (a pointer read, NO re-force of
`param`, NO OP_ATTRS_SELECT fix-point chase) and binding the formal local to
the entry's (still-lazy) Value, allocating a default thunk only for the missing
subset** — is the correct remaining candidate.  It removes the per-call wrapper
thunk ALLOC (the 12.79 %) while preserving use-time deferral (the entry Value
stays a lazy thunk, forced only when the formal is used).  It is W-scale +
BI-critical (new opcode + emit + VM; drv-hash byte-identity) and requires the
lookup to be provably force-free — DEFERRED to a dedicated effort.  The
`isFormalWrapper` instrument + `dumpFormalWrapperStats` are retained to verify
the eventual alloc drop.

## P2.1 design-b RCA — COMPLETE (2026-07-02, reproduced + instrumented; task #33)

The design-b recursion is now definitively RCA'd (reproduced gated + bisected
with two discriminator flags), correcting BOTH the a7948a2f3 commit's account
AND an intermediate hypothesis of mine:

- **Reproduced:** re-implementing design-b gated (bind each no-default demoted
  formal to `AttrSelect{Force(param), X}` at ENTRY instead of the wrapper thunk)
  reliably throws "infinite recursion … referencing `config` in `imports` … while
  evaluating the module argument `config`" on hello/git/firefox.drvPath;
  gate-off is correct.
- **Discriminator 1 — NOT `shouldForceSelectedEntry`:** a `V3_DBG_NO_FORCE_SELECT`
  gate making `shouldForceSelectedEntry` always false did NOT clear the recursion.
  So the App-force-memoize is NOT the cause (this refutes the intermediate
  "config is a Tag::App the select force-memoizes" hypothesis).
- **Discriminator 2 — IT IS `tryPushDirectMapAttrsEntry`:** ALSO making that
  helper a no-op (skip mapAttrs realization) DID clear the config recursion (it
  then failed later with an unrelated `assertion failed` from globally breaking
  mapAttrs elsewhere — a different, expected artifact).
- **MECHANISM:** the module argument attrset `param` is a **mapAttrs Bindings**
  (evalModules builds module args by mapping over `_module.args`).  design-b's
  entry-time `AttrSelect{param, config}` hits `tryPushDirectMapAttrsEntry`, which
  **realizes** the `config` mapAttrs entry by CALLING the arg-map function AT
  ENTRY — during the module collect phase, before `config` is ready → the config
  fix-point re-enters → recursion.  The wrapper thunk's load-bearing property is
  that it DEFERS the whole select-from-param (and thus the mapAttrs realization)
  to the formal's USE-time, when config is ready.  (Corrects a7948a2f3's
  "forceVal(param) deferral" framing — param is force-validated at OP_CALL
  regardless; the real deferral that matters is of the mapAttrs-entry
  REALIZATION.)

**Consequence for design-a:** a raw copy of a mapAttrs entry to a formal local is
UNSAFE — the unrealized mapAttrs placeholder is meaningful only inside its
Bindings' realization context, so binding it standalone loses that context.
Therefore raw-binding is safe ONLY when `param` is a PLAIN sorted Bindings
(non-mapAttrs, non-chain) — the callPackage/`intersectAttrs` case.  Module-system
formals (mapAttrs param) MUST keep the wrapper (deferral).  So design-a is
**param-shape-aware and PARTIAL** (a runtime plain-vs-mapAttrs check per call),
not the audit's blanket "raw Bindings::lookup."  Its capturable win = the
plain-param fraction of the 12.79% wrapper allocs (measured via
`formalsRawBindable` vs `formalsDeferredComplex` — the P2.1-a sizing counter).

**Sizing RESULT (2026-07-02, NIX_VM_STATS): ~70% of no-default formal instances
are on a PLAIN arg (raw-bindable).**  firefox = 36,936 raw-bindable (70.2%) vs
15,688 deferred (mapAttrs/chain), git = 12,889 (71.8%) vs 5,066.  So design-a
(param-shape-aware) can eliminate the wrapper for a MAJORITY of no-default
formals (the callPackage/`intersectAttrs`-arg case), deferring only the ~30%
module-system (mapAttrs/chain-arg) formals — the build is justified.  **BUILD
SPEC (safe by construction):** a new entry-block opcode (e.g.
`OP_BIND_FORMALS_RAW skipTarget [K slot words]`) for demoted formals lambdas —
read the sorted formal names from `desc->formals` (already serialize-remapped, so
the trailer carries only PROCESS-INDEPENDENT local-slot indices → NO SymbolId
remap, avoiding the P3.3 serialize footgun); at runtime, if `param` (slot 0) is a
plain sorted Bindings (`!isChain() && !isMapAttrs()`), raw
`lookupLocalEntry` each supplied no-default formal → bind its local to the raw
lazy entry Value (NO force, NO mapAttrs realize) + jump `skipTarget` past the
no-default wrapper MkThunks; else fall through to the wrappers.  Default formals +
`@`-arg keep their current bindings on both paths.  Gate default-off; validate
drv-hash byte-identity (hello/git/firefox) + adversarial + `--brute` 28/28 +
darwin-4 A/B.  The `formalsRawBindable`/`formalsDeferredComplex` counters verify
the eventual wrapper-alloc drop.

## P2.1-a DESIGN REFINED + DE-RISKED (2026-07-02, bytecode-evidence) — NOT a greenfield opcode; a cycle-safety/strictness-analysis extension

A full calling-convention map + bytecode inspection this session **reframes**
P2.1-a and explains WHY it is genuinely W-scale (not a quick opcode add):

1. **v3 ALREADY inlines formal selects at entry when SAFE.**  `{ a, b ? 5 }:
   a + b` emits NO wrapper thunk — the entry block does
   `OP_GET_LOCAL_FORCE 0; OP_ATTRS_SELECT a; OP_SET_LOCAL 2` (binds the local
   directly to the lazy `param.a` select) + the inlined `if param?b …` for the
   default.  This is the DEFAULT shipping path, so the inlined-select approach
   is already proven byte-identity-safe for the cases it fires on.
2. **The 12.79 % residual wrappers are the cases the inliner CONSERVATIVELY
   SKIPS.**  `{ a }: a` (a lazy tail-return of the formal) keeps the wrapper
   MkThunk (`OP_MAKE_THUNK` of a `Force(param).a` body).  The inlining is driven
   by strictness/cycle-safety analysis: `opt_func_strictness.cc` populates
   `strictArgs`; `opt_strict_call_unthunk.cc` uses it to skip MkThunk wraps —
   but it EXPLICITLY handles **`callee.hasFormals == false` (single-arg lambdas
   only)** and notes *"formals-style would need attrset-entry-level rewriting."*
   So formals-lambda residuals are not covered by the current analysis at all.
3. **Naive extension = design (b) = FALSIFIED.**  Inlining the select at entry
   UNCONDITIONALLY for all no-default formals is exactly design (b), which threw
   module-system infinite recursion — the deferral past entry is load-bearing
   for the config fix-point.  So capturing the residual REQUIRES the
   cycle-safety analysis, not a blanket inline.

**Refined disposition:** P2.1-a = extend the strictness/cycle-safety analysis
(`opt_func_strictness` + `opt_strict_call_unthunk`) to **formals-style
attrset-entry rewriting** — the exact capability the code flags as not-yet-built
— gated + validated byte-identical (drv-hash) across nixpkgs.  This is a
focused-but-real W-scale analysis change, drv-hash-critical, whose naive
shortcut is already falsified; it is deliberately NOT rushed at session-tail
(that risks re-introducing the design-b recursion class).  Realized-win prior:
~2.5 % CPU (the one micro-lever plausibly above darwin-4 noise) + churn/RSS.
Recommended top next dedicated effort alongside P3.1.  Design + insertion points
+ byte-identity hazards are fully mapped (this section + the WS-2 handback).

## P2.3 (or-defaults + inherit-in-rec) — MEASURED; SPLIT verdict

Measured on firefox.drvPath cache-off (deterministic per-descriptor
allocCount/forceCount via temporary `isOrDefault`/`isInheritWrapper` flags +
extended `dumpFormalWrapperStats`, host-independent counters):

- **or-default thunks: alloc = 89,841 = 3.66 % of 2,453,220 thunk allocs, only
  29.0 % forced (347 descriptors).  ≥2 % ⇒ GO.**  The 71 %-never-forced confirms
  the §4.3 waste exactly: `x.y or DEFAULT` thunkifies the default in the PARENT
  block (lowerSelect), but the attr is usually PRESENT, so ~63.8 K of those
  thunk allocations are pure churn that is never forced.
- **inherit-in-rec thunks: alloc = 1,313 = 0.05 % of thunk allocs (81
  descriptors).  <2 % ⇒ CLOSE.**  Negligible, as the prior predicted.

**Disposition: inherit-in-rec CLOSED; or-default GO-on-threshold but LOW
REALIZED VALUE ⇒ scoped, not built this session.**  The fix (relower the
default inside a SINGLE shared else-block of `emitSelectChain` instead of
pre-thunkifying in the parent — audit P2.3 packet) eliminates the ~63.8 K
wasted allocs, but that is only ~2.6 % of thunk allocs → by the alloc-share→CPU
math (ALLOC ≈20 % of CPU) and this session's CPU-NEUTRAL WS-3 batch result,
the realized win is ~0.5 % CPU (below the darwin-4 noise floor) and ~1.5 MB RSS
(63.8 K × 24 B) — negligible.  It is a clean, downside-free churn reduction
(the wasted thunks are never forced, so laziness is trivially preserved) worth
doing WITH the WS-2 build pass, but it ranks below P3.1 (chain IC) and
P2.1-design-a (12.79 %).  The `isOrDefault`/`isInheritWrapper` instrument is
retained (like `isFormalWrapper`) to verify the eventual alloc drop.  Another
instance of the FP-2 "allocated ≠ realized" pattern: a construct clears the
alloc-share proxy threshold yet the realized CPU/RSS win is sub-noise.

---
---

# Handback — WS-3 (VM hot path), 2026-07-02

Commits (branch `angerman/2.35-eval-profiling-v2`), each gated full `--brute`
28/28 + independent adversarial review where non-trivial:

- **P3.2** (§3.1, `4a7055a22` prior) formals-call `needsForce` guard + lazy
  error-path `lambdaName` string.  Shipped.
- **P3.4** (§3.5, `4a7055a22` prior) `valueEqual` scalar fast path.  Shipped.
- **P3.5a** (§3.7, prior) barrier `phaseDActive()` hints `[[unlikely]]→[[likely]]`.
- **§3.14 dead-code** (`803b0af11`): SELECT_DYN double `shouldForceSelectedEntry`
  hoisted; emit.cc dead dup-detection loop removed; (earlier `c27d12f09`
  removed the OP_ATTRS_SELECT dead `chase` block).  BI-neutral.
- **P3.3** (§3.4) — **FALSIFIED + REVERTED** (`152c6605f`).  Moving the
  OP_ATTRS_INIT runtime sort to emit time is UNSAFE: positional value pushes
  can't be remapped to a pre-sorted trailer across the cross-process disk cache
  (only slot-indexed REC_INIT can).  Adversarial review caught a warm-cache
  corruption in the first attempt.  New guardrail suite
  `run-attrs-init-cache-roundtrip-tests.sh` (brute core, now 28) + a §3.4 note.
  See the §3.4 write-up above.  DELIVERABLE = the finding + guardrail (kill).
- **P3.5 (constexpr `phaseDActive`) + P3.8 (`std::move`)** (`362f87be9`):
  `phaseDActive()` is now `constexpr … return true` (barrier guards fold across
  all TUs without LTO; dead `g_phaseDActive` retired); two `mkStringValueOwned`
  call sites in `primConcatStringsSep`/`primReplaceStrings` pass `std::move`
  (one fewer full-payload copy per concat/replace).  BI-neutral.  (Lesson: a
  barrier.hh change needs `ninja -C build` for ALL test binaries — a stale
  v3-smoke referencing the removed symbol tripped the brute once.)

**darwin-4 CPU validation** (`995983bcf`, git-noted): the entire shipped WS-3
batch is **CPU-NEUTRAL / cost-free** (firefox cold 2.68→2.67s, warm 1.80→1.79s
vs the P0.4 baseline).  No regression; no material CPU win at firefox scale —
each micro-op is individually noise-level, consistent with the beat-tw campaign
verdict.  WS-1's value is CORRECTNESS; WS-3's committed value is dead-code
removal + BI-neutral cleanup + one falsification with a durable guardrail.

## WS-3 remaining items — dispositions

- **P3.1 (§3.2 chain-aware read IC) — BUILT, validated, MEASURED, and DELETED
  (measure-first NO-GO: chains are shallow → the IC is a structural wash).**  §3.2
  claimed chain-Bindings SELECT "walks all ≤16 layers" as the single biggest
  per-lookup tax.  Built as designed (commit `0cb2f88b5`; gated `NIX_V3_CHAIN_IC`;
  caches `(chainLeaf, ownerLayer, slot)` in the shared per-call-site
  `attrSelectCache`, full moving-GC integration), adversarially NOT-REFUTED (5
  angles), `--brute` 28/28 gate-on + off, ON-vs-OFF byte-identical.  **Then a
  chain-SELECT depth/hit-rate counter settled WHY the darwin-4 A/B measured 0%:**
  firefox.drvPath — chain-SELECTs are **60.4 %** of all 511,776 selects (common,
  NOT rare), gate-off **avg chain depth = 2.44 layers** (SHALLOW), gate-on IC
  **hit-rate 45.9 %**; git.drvPath — **57.5 %**, depth **2.35**, hit-rate 44.9 %.
  So the IC fires + hits ~45 %, but each hit only saves a ~2.4-hop walk over small
  overlays — about the same cost as the IC's own 4-way scan + name-validation → a
  **structural wash → 0 % CPU** (firefox 2.66→2.66s, git 1.41→1.41s).  §3.2's
  "walks all ≤16 layers" is refuted: `mergeBindings` flattens every 16 layers AND
  SELECTs short-circuit at the top overlay, so effective depth is ~2.4 even though
  chains dominate.  The audit's named deep-chain target M5 is IFD-blocked on
  aarch64 (unmeasured), but for the IC to win M5 would need avg depth ≫ 4 —
  contradicted by the flatten-cap + the observed short-circuit.  **DELETED per
  Rule 0** (a gate with no proven win must retire, not coexist): code restored to
  pre-P3.1.  The validated implementation is preserved at commit `0cb2f88b5` for a
  trivial re-apply IF a future x86_64 run first measures M5 avg chain depth ≫ 4
  (re-add the cheap depth counter to check — do NOT re-build the IC blind).  Third
  measure-first "the §-headline lever doesn't materialize" result (cf. P3.3,
  P4.4-CPU).  (§3.3 MapAttrs parent memo — the recompute-on-parent-hit fix — is a
  separate remaining sub-lever, not part of this IC.)
- **P3.6 §3.8 (magic-static env-gate sweep + code-ptr dispatch local) —
  ASSESSED, DEFERRED (low EV).**  (a) The ~40 function-local `static const bool
  s_*` env gates in the dispatch region each cost a magic-static guard load per
  access; hoisting to file scope removes it — but they sit on mostly-[[unlikely]]
  cold branches, so the incremental win over the already-shipped #768 hoists is
  likely <1 %, against a 15-40-site error-prone edit surface.  (b) The code-ptr
  dispatch local (`cache cu->code.data()`) is DELICATE: `cu` is reassigned at
  7+ inline tail-call/return/thunk-force sites within one dispatchLoop, so a
  cached pointer needs refreshing at every one (a single miss = wrong-bytecode
  corruption) — not the "cheap win" the audit framed.  Given the WS-3 batch
  measured CPU-neutral (dispatch micro-opts are noise at firefox scale), the
  effort/risk is not justified now.  Revisit only if a darwin-4 dispatch
  profile shows guard-variable / code-reload cost material.
- **P3.5 remainder (nursery-bounds cache + post-construct short-circuit +
  inverted dispatch hints vm.cc:3941/5254/5279) — remaining, GC-adjacent, low
  EV** (same neutral-batch reasoning; the constexpr `phaseDActive` half, the
  main win, shipped).  Bounded follow-up if a profile motivates it.
- **P3.7 (reuseScope for arity-1 strict primop callers) — DEFERRED**; the
  autoresearch L3 idea, plausible but D-scale and the batch-neutral result
  lowers its prior.
- **P3.8 single-pass concat / context id-array** = the W-scale part of P4.4
  (string-context), whose CPU half is now FALSIFIED (§5.6 P4.4 note) — only the
  RSS/correctness halves survive, W-scale.

---
---

# Handback — WS-2/WS-4/WS-5/WS-6 + cross-cutting dispositions, 2026-07-02

Honest accounting of every remaining §9 item.  Two classes: (A)
**already measure-informed → documented-defer** (prior beat-tw campaign +
this session's batch-neutral CPU result are the evidence), and (B)
**not-yet-measured → measure-first-pending next-step** (open, scoped, NOT
claimed closed).

## WS-2

- **P2.1 build (design a, `OP_BIND_FORMALS`)** — the correct path (design b
  falsified); W-scale + BI-critical; the highest-value THUNK lever (12.79 % of
  firefox thunk allocs measured).  **Next-step (B): build it** — new opcode +
  emit + a force-free raw `Bindings::lookup` at OP_CALL, default-thunk only for
  the missing subset.  Kept the `isFormalWrapper` instrument to verify the
  alloc drop.  (task #16)
- **P2.3 (or-default + inherit-in-rec)** — **measure-first-pending (B)**; tag
  the two thunk classes, ≥2 % each ⇒ build.  Prior is a CLOSE (both are rarer
  than the 12.79 % formal wrappers), but must be measured. (task #28)

## WS-4 (RSS/CU)

- **P4.4 lever 1.1 (string-context)** — CPU half **FALSIFIED** this session
  (§5.6 note; −0.4 % firefox).  RSS/span-sharing + §2.6-correctness halves
  survive, W-scale, BI-critical on drvPaths — **(A) defer**: the beat-tw
  campaign already graded the MALLOC_SMALL/context-copy bucket
  modest/foundational, and RSS is "shrink the live representation," a broad
  program with no single lever.
- **P4.1 (LambdaDescriptor diet) / P4.2 (DAG double-lowering) / P4.3
  (literal-pool + PosSnapshot interning, symbol-mirror delete) / P4.7 (Bindings
  Kind-split, drop `Closure::cu`)** — the arena/MALLOC_SMALL RSS-reduction
  program.  **(A) defer with evidence:** the beat-tw campaign (see project
  memory `project_beat_tw_v2_2026-06-23`, `project_bibop_campaign_2026-06-27`)
  established that ALL reclaim-based RSS levers are dead and the gap is the LIVE
  representation (v3 cells + CU structs + Boehm heavier than TW's 16 B
  niche-tagged Value) — a broad foundational program, NOT a single lever.  P4.2
  (fix the DAG double-lowering, so an expression isn't lowered twice into two
  descriptors) + P4.3 (delete the diagnostics-only `Module::internSymbol`
  mirror, §5.5) are the two SMALL, self-contained, low-risk wins here worth a
  D-scale pass if RSS becomes the priority; the rest is the foundational
  program.
- **P4.5 (derivationStrict env-building as one C leaf)** — **(B)
  measure-first-pending**, BI-CRITICAL (drv hashes); D-scale; needs the
  mergeBindings-by-site table first.
- **P4.6 (import single read + key memo, AOT string_view, SQLITE_STATIC)** —
  **(B) measure-first-pending**; warm-CPU lever, independent ∥.  The warm gap
  is ~2.4× (firefox); import I/O + deserialize is a plausible warm slice.  Next
  step: the warm-run import timing buckets (the audit's own falsifier), then
  the D-scale build if a bucket is material.

## WS-5 (GC/policy)

- **P5.1 (default gen-major: stop paying vs enable metadata)** — **(B)
  measure-first-pending**; §5.1 says the default config cannot reclaim dead
  tenured cells yet pays full GC cost.  This is a POLICY decision to co-own with
  the bounded-memory M2 owner (A.3 note); measure the per-fire cost + no-op
  rate first.  NB the M-3 trap: `alloc.hh g_majorGcEnabled` must stay hard-false
  (re-enabling the legacy per-op major alongside the always-on nursery is a UAF).
- **P5.2 (nursery permanently-full latch) / P5.3 (dirtyContainers dedup, drop
  liveTenuredRanges when un-brute'd)** — **(A) low-EV defer**: bounded by GC's
  ≤7 % CPU share; the batch-neutral result lowers the prior further.
- **§2.5 (deepForceList writeback stale-across-scavenge)** — latent; overlaps
  the task #14 missed-root investigation (see WS-1 handback + task #14).

## WS-6 (compile-time)

- **P6 (occur-DCE default-on decision, scope-map interning, freeVars worklist)**
  — **(B) measure-first-pending**; PARSE+LOWER is 20-29 % of COLD CPU (a real
  cold/CI slice, amortized warm).  Next step: the PARSE+LOWER share on cold +
  CI, then the occur-DCE default-on A/B (the decisive sub-lever).

## Cross-cutting honest verdict

This session shipped the cheap/safe WS-3 items, decisively FALSIFIED two audit
proposals (P3.3 emit-sort, P4.4-CPU) with durable artifacts, and validated the
batch as CPU-neutral on darwin-4.  The audit's own §9 honest expectation holds:
there is no single remaining CHEAP lever that materially moves the gap.  The
real remaining levers are (1) **P3.1 chain-aware IC** — the one scoped D-W CPU
lever with a headline mechanism; (2) **P2.1-design-a** — the one scoped THUNK
lever with a measured 12.79 % target; (3) the **foundational RSS program**
(leaner live representation) the beat-tw campaign already characterized as
multi-week; (4) **JIT** (removes dispatch entirely) — the campaign's deferred
multi-week play.  Everything else is measure-first-pending (P2.3, P4.5, P4.6,
P5.1, P6) or low-EV-deferred.
