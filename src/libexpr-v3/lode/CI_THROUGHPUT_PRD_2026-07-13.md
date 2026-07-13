# PRD: v3 → CI-useful — correctness, IFD throughput, parallel-eval density

**Date:** 2026-07-13
**Status:** ACTIVE — supersedes the single-eval-parity objective
**Source:** 4-axis senior code review 2026-07-13 (IFD path / warm CPU / memory / cache layers), branch `angerman/2.35-eval-profiling-v2`
**Owner:** v3 team
**Copyright:** (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group. SPDX-License-Identifier: Apache-2.0.

---

## 1. Problem statement & reframe

The product goal is **CI throughput**, not single-eval benchmark parity:

1. CI re-runs the same or near-identical evals many times per day → **warm repeated eval** is the unit of value. Cold parse/lower/compile time is explicitly OUT of the comparison.
2. **IFDs are invisible and block sequentially** → the dominant wall-clock component of IFD-heavy CI evals (M5/HNE-class).
3. **Per-process RSS is too high to run N evals concurrently per box** → CI fleet under-utilized.

### 1.1 The closed question (do not reopen)

Single-eval CPU/RSS parity with the tree-walker is **out of reach in this architecture** and is no longer a goal. This is triply established: (a) the falsification ledger (env-pointer capture Gate-C KILL; barrier-removal Phase-S KILL; JIT Amdahl floor 1.42–1.69×; representation-rewrite structural floor 1.6–2.0× RSS), (b) the 2026-07-13 code review confirming each mechanism at line level, and (c) the workload profile itself: ~37M opcodes over ~150K lambda sites on firefox (~hundreds of executions/site, no hot loops), 52% of dynamic ops are stack movement, 65.7% of thunks never forced. The V8 playbook (per-site investment amortized by re-execution) does not match; v3 pays GHC's runtime taxes (blackholing, cell-update, barriers, safepoints) without GHC's strictness compiler.

### 1.2 Where v3 CAN win (the new thesis)

The tree-walker structurally **cannot**:
- **Suspend evaluation across a store build.** Its eval state is C++ stack recursion. v3's eval state is heap data (`VMState{valueStack, frames, withStack}`) plus a complete, dormant fiber substrate (`fiber.cc`) → v3 can overlap IFD builds. This is v3's one unique structural advantage. (Caveat: forceValue/callClosure C-recursion towers mean *whole-stack* fiber suspension is the only viable shape — see WS-4.)
- **Reuse warm eval state across CI runs.** v3 has a designed-in cache stack (CU disk cache, IFD import cache, applied-import cache, AOT mmap) that the TW lacks. Today only 2 of 7 layers fire across processes; a persistent-worker model + AOT file converts the rest (WS-3).
- **Share pages across N parallel evals.** v3's bytecode/CU/descriptors are read-only-in-principle after load; the TW re-parses per process. Today mutable fields interleaved into those pages defeat sharing; fixable (WS-5).

**Success = v3 is the evaluator you *want* in CI even at 1.8× single-eval CPU**, because: IFD walls overlap, warm re-evals hit caches the TW doesn't have, and N evals fit where the TW fits fewer.

---

## 2. Success metrics (measure these, not proxies)

| # | Metric | Baseline (2026-07-13) | Target | Where measured |
|---|---|---|---|---|
| KPI-1 | Per-eval IFD report (count, per-IFD ms, built/substituted) visible in CI logs | invisible | on by default in CI | any host |
| KPI-2 | Warm IFD-heavy eval wall time (HNE hello.drvPath, IFDs realised, fresh process) | ~7.6s v3 / ~1.5s TW | v3 ≤ TW via WS-2+WS-3 skip/overlap | darwin-4 + Linux CI host |
| KPI-3 | Multi-root IFD overlap factor (2 independent-IFD roots) | 1.0 (fully serial) | ≥ 1.6× wall reduction | Linux CI host |
| KPI-4 | Eval #2..N wall time in persistent-worker mode vs fresh-process | N/A (no worker mode) | eval#2 ≤ 0.5× fresh-process warm | darwin-4 |
| KPI-5 | Incremental RSS per additional concurrent eval (M5-class) | ~2.2 GB (all private) | ≤ 1.5 GB (COW/AOT sharing), stretch ≤ 1.0 GB (+Linux reclaim) | **Linux** host, smaps |
| KPI-6 | Correctness: full `--brute` ALL GREEN + parity tests for WS-1 fixes | 3 known divergences | 0 known divergences | any host |

**Measurement discipline (unchanged, load-bearing):** CPU/RSS numbers only from the quiet host darwin-4; NEW: RSS-reclaim and COW/fork measurements need a **quiet Linux host** (the macOS reclaim falsifications do NOT transfer — see WS-6/M1). Every benchmark stamped with commit + git-noted. Full `--brute` before every merge. Rule 0 applies to every commit.

---

## 3. Non-goals / killed levers (do not re-fund without new data + pre-committed threshold)

- TW-style env-pointer capture (Gate-C KILL: widens gap at 1.9–2.1 avg upvalues).
- Barrier removal via non-moving tenured (Phase-S KILL: reclaim cost eats the win).
- Standalone JIT as a beat-TW play (floor 1.42–1.69×; only paired with allocate-less).
- Whole-eval top-level result cache on real workloads (taint-wall: `primImport` itself taints; sound scope = pure-value evals only).
- Active IFD provenance v2 serving (post-eval key cannot skip the body; measured net-negative).
- String dedup, value hash-consing, RC/Perceus, flake-arg caching (all measured KILL).
- **macOS** page-release levers (dead on darwin; UNTESTED verdict on Linux — that re-test is WS-6/M1, not a re-fund of the macOS work).

---

## 4. Workstreams

Priority order: **WS-1 (correctness) and WS-2 (visibility) start immediately and in parallel. WS-3 (worker+AOT) is the cheapest big win. WS-4/WS-5/WS-6 are the strategic tracks. WS-7 is opportunistic. WS-8 is hygiene, folded into the others' commits.**

Every task below lists: rationale → exact entry points → deliverable → acceptance gate → kill criterion (Rule 0). Estimates are engineering-days for someone familiar with the tree.

---

### WS-1 — Correctness bugs (P0, ~4–6 days total, no dependencies)

Repo rule applies to each: **positive + negative + regression tests**, minimal repro kept forever under `test/`, full `--brute` green before merge.

- [ ] **C1. `primHashFile` must realise its argument** — `primops.cc:3847` calls raw `nix::hashFile` on the unrealised path; TW realises (`libexpr/primops.cc:2474`). `hashFile "${drv}/file"` on an unbuilt drv throws "does not exist" or hashes stale content.
  - Fix: route through `ns.realisePath` like `primReadFile` (`primops.cc:4039`) before hashing.
  - Tests: parity test TW-vs-v3 on a string-with-context arg (built + unbuilt drv); regression: plain-path arg unaffected.
  - Gate: parity suite green; `IfdProbeKind` taxonomy now truthful for this op.

- [ ] **C2. `primReadFileType` must realise its argument** — `primops.cc:4285` raw `symlink_status`; TW realises (`libexpr/primops.cc:2526`). Same fix/test shape as C1.

- [ ] **C3. `primFindFile` skips per-element realisation** — `primops.cc:3147` vs TW's per-element `realiseContext` (`libexpr/primops.cc:2309`). Search-path elements that are drv outputs silently miss.
  - Fix: realise each element's context before stat. Tests: search path containing a drv-output element, built + unbuilt.

- [ ] **C4. `primPathExists` swallows failed IFD builds into `false`** — trailing `catch (...)` at `primops.cc:3529-3533` falls through to raw lstat. TW only swallows `RestrictedPathError`; a *build failure* must propagate, not read as "path absent".
  - Fix: narrow the catch to the TW-equivalent error set; add a parity test asserting a failing-drv `pathExists "${drv}/x"` throws in both evaluators.
  - Kill criterion for the old behavior: the new failing-first test.

- [ ] **C5. Doc rot: `CallFrame` size comment** — `vm.hh:63` says 40 bytes; struct is 64 (grew via `forceWriteTarget`/`deepForceCursor`/`defEnv`/`memoKeyIdx`). Fix comment; add `static_assert(sizeof(CallFrame) == 64)` so it can't rot silently. (Fold into any WS commit.)

---

### WS-2 — IFD visibility (P0, ~2–4 days, no dependencies)

"IFDs are invisible" is a **defaults problem** — the machinery exists on both sides, all opt-in.

- [ ] **V1. Turn on per-IFD reporting in CI.** TW-side `profile-import-from-derivation` (`eval-settings.hh:220-235`) already logs `IFD #n: <drv> [built|substituted|already-valid] <N>ms at <pos>` + eval trace, and exports `nrIFDs/nrIFDsCached/totalIFDTimeUs/ifdEvents` into `NIX_SHOW_STATS` JSON (`libexpr/eval.cc:3372-3378`). Deliverable: CI pipeline config + one page in USAGE.md ("reading the IFD report"). Zero code.

- [ ] **V2. Default-on end-of-eval IFD summary line in v3.** One line at eval end when ≥1 IFD occurred: `v3: N IFDs, M built / K substituted / J cached, total blocked X.Xs (=Y% of wall)`. Data already flows through `ifdProbeCount`/`ifdProbeWithCtx` (`alloc.hh:832/855`) + the Phase-4b cache counters; today it hides behind `NIX_VM_STATS` (`run.cc:1398-1441`).
  - No new env gate (Rule: no gate without retirement criterion — this is unconditional output on stderr, like GC warnings).
  - Acceptance: HNE run shows the line; silent when 0 IFDs.

- [ ] **V3. Wall-time attribution.** Extend the summary to split eval-wall = compute + IFD-blocked + store-RPC, using the existing `import_timing.hh` phases. This is the number that justifies (or kills) WS-4 funding — **do V3 before starting WS-4** (Rule 0: measure before build).
  - Deliverable: `bench/ifd-decomp.sh` emitting the split for firefox/M5/HNE, git-noted.

---

### WS-3 — Persistent worker + AOT: make the existing cache stack actually fire in CI (P1, ~1.5–3 weeks)

Cross-process reality today (2026-07-13 review): a fresh CI process gets ONLY (1) the CU disk cache and (2) the IFD-import v1 cache. The **applied-import "moat" cache — the thing that makes eval #2 fast — is keyed on a raw `LambdaDescriptor*` (`vm.cc:4116`) and holds live GC graphs: it contributes ZERO across processes.** Fastest route to KPI-4 is not new caching — it's keeping the process alive.

- [ ] **W1. `v3-eval --worker` mode (or `nix eval --v3-worker`).** Single process, accepts sequential eval requests (stdin JSONL or unix socket; pick simplest), runs each through `runRootExprFromString`, streams results.
  - Must set keep-alive semantics for global roots: `clearPostEvalGlobalRoots` (`primops.cc:6749-6767`) currently drops the import cache at end of eval; worker mode keeps it (`NIX_V3_KEEP_GLOBAL_ROOTS=1` behavior becomes implicit in worker mode — fold the env var into the mode; retire the var per gate-hygiene).
  - Define the between-evals reset list explicitly (this is the correctness core of the task): `g_topLevelTaintMask` reset, limits re-arm, `tlActiveVMStack` empty assertion, stats epoch. Import cache stays (validated by mtime+size stat, `primops.cc:7135-7146`); applied cache stays (sound per-process by construction — descriptor identity is stable within the process).
  - Acceptance gate (pre-committed): eval#2 of the SAME expr byte-identical to eval#1 AND to fresh-process result (35/35 brute-style triple); KPI-4: eval#2 wall ≤ 0.5× fresh-process warm on firefox + M5.
  - Kill criterion: if between-evals state bleed cannot be bounded (byte-divergence in the triple test that survives one week of fixing), stop and document — the daemon route is then unsound and WS-3 reduces to W3.

- [ ] **W2. Memory bound for worker mode.** Unbounded worker = slow leak by design: ImportCache default eviction is 0=never (`primops.cc:6520-6529`) AND it is a GC *root* (`primops.cc:6773`); the arena never shrinks (see WS-6). Deliverables: (a) default LRU bound for ImportCache in worker mode (pick by measurement: bound that keeps M5 eval#2 within 5% of unbounded), (b) worker self-recycle policy (restart after N evals or RSS > threshold — zygote-style respawn is cheap once W1 exists).
  - Gate: 100 sequential mixed evals, RSS plateau (< +5%/10-evals after eval 10).

- [ ] **W3. Ship the AOT cache file in the CI image.** Infrastructure complete + tested (`aot_cache.cc`, `bench/build-aot-cache.py`, insert-manifest mode `NIX_V3_AOT_BUILD_MODE`, consulted before SQLite at `disk_cache.cc:382/477`).
  - Deliverable: Makefile target `make aot-cache-ci` (canary run → build file → verify lookup hit-rate ≥ 95% on second run); CI image recipe doc.
  - Gate: fresh-process warm eval with AOT file ≥ parity with SQLite-warm (it's an mmap binary search vs SQLite point lookup — expect small win + immutability/distribution benefit).

- [ ] **W4. Disk-cache growth hygiene** (fold-in): `EvalResults`/`CompilationUnits` LRU column exists but eviction was deliberately removed (`disk_cache.cc:409-414`) → DB grows unboundedly in a CI image. Either implement `PRAGMA`-cheap eviction at open (drop rows > N days / > size cap) or document "wipe per image build" as the policy. Rule 0: pick one, kill the other.

---

### WS-4 — IFD overlap: fibers + a work source (P1-P2, ~4–7 weeks, gated on WS-2/V3 measurement)

The choke point: EVERY IFD funnels through `ffi::realisePath` (`ffi.cc:172-182`) → `EvalState::realiseContext` → **synchronous `buildStore->buildPaths` (`libexpr/primops.cc:258`)**, one at a time, with the whole process blocked. No batching exists (`OP_IFD_PROBE` is a placeholder, `vm.cc:13057-13075`; the S2 pre-walk was never built; `BlockingFFI<T>` async store API is declarations-only — `ffi.hh:746-756`, never defined). The in-tree `fiber.cc` (295 lines, ucontext, 16MiB guarded stacks, GC pre-flips landed at `mark_sweep.cc:916`) is a **complete dormant substrate with zero call sites**.

**Sequencing rule: A before B before C. Each phase has its own kill gate; do not start the next on a red gate.**

- [ ] **F0. Prerequisite audit (3–5 days, written report as the deliverable):**
  1. *Moving-nursery vs parked fibers*: major mark scans yielded fiber stacks (`mark_sweep.cc:916-917`) but the Cheney **scavenge does not** — a parked fiber holding raw nursery `Value`s across a yield is a missed-root UAF (PhD-6 class). Decide: (a) force-scavenge before every yield so parked stacks hold only tenured refs, or (b) conservative-pin nursery cells referenced from parked stacks. Prototype the chosen invariant + a `--brute`-style stress (yield inside every realise site under 1MB nursery + audit).
  2. *Blackhole protocol*: a parked strand owns blackholed thunks; a second strand touching one currently throws "infinite recursion". Design wait-queues (GHC-style) or strand-abort-and-retry. The par-trace ~50% memo-sharing measurement bounds how disjoint real strands are — re-run `par_trace` on the actual CI jobset shape to size the collision rate BEFORE building wait-queues.
  3. *Store threading*: N overlapped builds need N daemon connections on worker threads; eval fibers stay on one pthread. Verify `EvalState`/store client thread-safety assumptions at the ~6 realise call sites (`primops.cc:7290/7335/4039/4120/3516`, `ffi.cc:557/664`).

- [ ] **F1. Mechanism (2–4 weeks): suspend-on-realise.** Wrap the realise funnel: when a fiber hits `buildPaths`, register `(DerivedPath → fiber)` in a pending-build table, submit to a worker-thread build pool, `fiberYield`; scheduler resumes fibers as builds complete.
  - Entry points: the single funnel `ffi::realisePath` + `ensurePath`/fetch sites; driver in `run.cc`.
  - Acceptance gate (pre-committed): 2-root harness with 2 independent 10s-build IFDs completes in ≤ 12s (vs 20s serial); full `--brute` green under fiber mode; byte-identity on nixpkgs golden.
  - Kill criterion: if F0.1's invariant can't survive brute stress in 2 weeks of fixing → park the mechanism, document, and delete or re-gate fiber.cc per Rule 0 (its current zero-callsite state is already a Rule-0 violation — this workstream is its fund-or-delete decision).

- [ ] **F2. Work source (the real design problem — suspension alone buys nothing; laziness discovers ONE pending IFD at a time):**
  - **F2a (do first, cheapest): multi-root eval.** CI evals a *jobset* — many attr roots. `v3-eval --roots a b c …` (or worker-mode parallel requests) = one fiber per root, naturally independent IFD strands. No speculation, no new soundness surface.
    - Gate: KPI-3 (≥1.6× on 2-root independent-IFD harness), then a real jobset slice.
  - **F2b (only if F2a's overlap is memo-collision-bound): S2 bytecode pre-walk** (`lode/IFD_DEEP_DIVE_2026-05-21.md` §S2) — walk reachable bytecode from the root to surface IFD-class call sites early, submit as a batch. Speculative; needs a false-positive budget.
  - **F2c (research, do not fund yet): sibling-attr speculation.** Highest ceiling, highest waste; requires cancellation + heap isolation between speculative strands.

- [ ] **F3. Integration with WS-3 worker:** overlapped IFDs inside worker mode; report overlap factor in the V2 summary line.

---

### WS-5 — Parallel-eval density: COW sharing (P2, ~2–4 weeks, independent of WS-4)

Target: KPI-5. N×M5-class concurrent evals per box. The dominant blockers are self-inflicted page-dirtying, not representation size.

- [ ] **D1. Side-array the mutable fields out of cold pages (~1–2 weeks).**
  - Move OUT of `CompilationUnit`: `attrSelectCache` + `recSlotCache` (`bytecode.hh:654/669`) — ICs are written on ~every attr access and bulk-nulled at every gen-major (`vm.cc:4773-4778`); they guarantee CU pages never stay COW-clean.
  - Move OUT of `LambdaDescriptor` (~170B each, 25.5MB/65% of firefox CU footprint): `forceCount/allocCount/callCount`, `cachedSingletonClosure` (`closure.hh:386-439`), and the post-load re-stamped `cu` backpointer (`closure.hh:565-581`) + `fromImportCU` stamping (`bytecode.hh:594-600`) — one 8B write dirties a 16KB page holding ~90 descriptors.
  - New home: parallel side arrays keyed `(cu, funcId)`, allocated per process.
  - Acceptance: after load + one warm eval, bytecode/constants/descriptor pages measured CLEAN in a forked child (Linux `/proc/self/smaps` `Private_Dirty` on those mappings ≈ 0); byte-identity; `--brute` green; CPU regression ≤ 1% on darwin-4 (indirection cost).
  - This also shrinks the descriptor (diagnostic bloat ~38%) — take the free RSS win.

- [ ] **D2. Consume the AOT mmap in place (~1 week, after D1).** Today every AOT hit copies the blob and `deserializeCU` materializes private vectors (`serialize.cc:922-956`) — the resident CU is 100% private even with the shared file. Deliverable: flat CU format readable in place (offset-based views over the mmap for `code`/constants/symbols), fall back to deserialize for mutably-needed sections.
  - Gate: N processes share CU pages (smaps `Shared_Clean` accounting); per-process CU-resident slice reduction ≥ 60% on firefox.

- [ ] **D3. Zygote spike (~1 week, after D1; Linux):** warm parent (AOT loaded, prelude evaluated), `fork()` per eval request.
  - Measure: incremental `Private_Dirty` per child on M5-class eval vs fresh process (KPI-5).
  - Pre-committed gate: incremental RSS ≤ 70% of fresh-process RSS, else document which pages dirty (expect: lazy forcing dirties the shared warm graph by design — `Thunk::cell` writeback + `ValuePair::evaluated` memo; only fully-forced data stays clean) and keep the zygote for CU/cache sharing only.
  - Note: Boehm side must be audited for fork-safety (GC locks/threads at fork time); the nursery is thread-local and re-init-able.

---

### WS-6 — Memory reclaim on the ACTUAL CI OS (P2, ~1–2 weeks measurement-first)

The entire "page-release levers are dead" chain was measured on **macOS** (`std::free` of 16MB calloc'd blocks returns 0% RSS — libmalloc magazines; `madvise` no-op; R1 note `alloc.hh:2758-2762`). Linux glibc `free()` munmaps large chunks and `MADV_DONTNEED` works. **Every reclaim verdict must be re-run on Linux before being treated as structural there.** (Rule 0: this kills — or transfers — the macOS falsifications.)

- [ ] **M1. Linux reclaim falsifier (measurement only, ~3 days).** On a quiet Linux host: (a) does the DEFAULT ungated huge-block sweep (`mark_sweep.cc:2493-2505` → `std::free`, `alloc.hh:2544-2547`) return RSS? (b) mmap'd blocks (`mapArenaBlock`, `alloc.hh:2767-2772`, currently dead-gated) + `MADV_DONTNEED` on dead 16MB blocks? Pre-committed thresholds: ≥150MB peak-RSS reduction on M5 = fund M2; < 50MB = the macOS verdict transfers, close.
- [ ] **M2. (Only on M1 green) Wire whole-block-free into the default gen-major on Linux.** `freeWholeBlock` (`alloc.hh:2433-2514`) is currently reachable only under hard-false `g_majorGcEnabled` (`mark_sweep.cc:2475-2477`, `alloc.hh:1242` — the M-3 UAF trap is the *legacy per-op* major GC, NOT the safepoint gen-major). Retarget the gate to the safepoint gen-major context (`vm.cc:4726-4799`, exitDepth==0, post-forceScavenge — no live nursery pointers into candidate blocks) + enable `cellStarts` metadata by default IF its cost gates pass (the default sweep is currently mark-without-sweep: `mark_sweep.cc:2438` breaks on the first regular block because `cellMetaEnabled()` is false — today we pay seconds of mark on M5 for near-zero reclaim).
  - Gates: `--brute` under 1MB-nursery stress (UAF class); CPU ≤ +3% darwin-4; Linux M5 peak RSS −≥150MB.
  - Kill criterion: cellStarts maintenance cost > 3% CPU → keep Linux huge-block + MADV path only.
- [ ] **M3. (Optional, after M1/M2) Default-sweep honesty.** If M2 is killed, stop paying the blind mark in default builds: raise the gen-major threshold or disable marking where the sweep can't act. A GC that marks and cannot reclaim is pure tax.

---

### WS-7 — IFD phase-3: pre-eval build-output-narHash fragment cache (P2, ~2–3 weeks, spec exists)

The one sound-AND-fast cache shape not yet built (spec: `lode/IFD_PROVENANCE_CACHE_SPEC_2026-07-07.md`, phase 3; all three 2026-07-07 Phase-0 investigations converged here). The shipped v1 IFD cache (default-on, keys `path+narHash`, skips fragment parse+lower+eval — `primops.cc:7578-7607`) has a demonstrated under-capture hole (transitive reads, test N1); the sound v2 key is post-eval so it can't skip (Phase-2 KILL). Phase 3 = compute the sound key **at the realise boundary, before fragment eval** (Bazel build-then-key): key on the realised build-output narHashes so a hit soundly skips the fragment body cross-process.

- [ ] **P1. Re-read the spec + write the phase-3 key derivation** (what exactly is available pre-eval at `realisePath` return: output path set + narHashes; how nested IFDs compose; poison rules inherited from v2's `ProvenanceFrame`).
- [ ] **P2. Shadow mode first** (v2 discipline): compute phase-3 key, compare served-would-be vs actual, count mismatches. Gate: mismatches == 0 on M5+HNE darwin-4, wouldHits ≥ 90% of v1 hits.
- [ ] **P3. Active mode** behind the existing gate family with retirement criterion. Pre-committed SHIP gate: HNE warm wall −≥25% vs v1-only; byte-identity; brute green. Kill: T_hit/T_eval ≥ 0.5 (the v2 lesson — if a hit can't skip real work, it's dead).
- [ ] **P4. Retire v1's unsound key** on P3 ship (Rule 0: confirming phase-3 kills v1's under-capturing key — don't leave both).

---

### WS-8 — In-architecture CPU cleanups (P3 — opportunistic, ONLY after WS-1..4 are staffed)

Bounded wins; none inverts the TW sign. Each needs a darwin-4 before/after + git note. Listed in expected-yield order:

- [ ] **K1. Kill formal-wrapper thunks via supplied-arg direct binding** — 12.79% of all runtime thunks (`cli/lower_v3.hh:599-635`, flag `closure.hh:483`). Design-a (OP_BIND_FORMALS raw-lookup) was deferred, not killed; design-b (eager entry-select) is FALSIFIED (recurses on module-system fix-point) — do not retry it.
- [ ] **K2. Chain-aware SELECT IC** — chain bindings (default-on `//` path) bypass the IC entirely (`vm.cc:10760-10838`): cache `(chainLeaf, sym) → (layer, slot)`. Measure IC hit rates per site FIRST (the audit's open ask — one afternoon with the existing counters).
- [ ] **K3. Dispatch mechanics**: computed-goto + cache `code.data()` in a local; replace the per-op `shouldScavenge()` recomputation (`vm.cc:4601-4617`) with an allocator-set "GC requested" word; fix the inverted `__builtin_expect(nursery != nullptr, 0)` hint (always true in the outer loop).
- [ ] **K4. Formals validation merge-scan** — `OP_CALL` re-validates formals per call via `forEach`×binary-search (`vm.cc:7596-7737`); both sides are sorted → O(n+m) merge, or memoize validation per `(descriptor, caller-shape)`.
- [ ] **K5. Extend `reuseScope`** (measured −5.7% on foldl) from `callClosure2` to all arity-1 callback callers (`vm.cc:15868` family).
- [ ] **K6. Blackhole O(frames) scan** → maintain a per-VMState side map or frame back-pointer (`vm.cc:9662-9667`; frame stacks run thousands deep on nixpkgs).

---

## 5. Rule-0 hygiene items (fold into the workstreams' first commits)

- [ ] **H1. fiber.cc fund-or-delete** — resolved by WS-4/F1's gate. Until then it is the forbidden "gated experiment without a gate". The WS-4 F0 report is its kill criterion.
- [ ] **H2. `BlockingFFI<T>` declarations** (`ffi.hh:746-756`, never defined) — delete now; WS-4 will introduce its own real API. Aspirational dead API is documentation debt.
- [ ] **H3. Disk-cache LRU column** — resolved by WS-3/W4 (implement or drop; don't keep the ambiguous middle).
- [ ] **H4. `IfdProbeKind` taxonomy vs implementation drift** — resolved by WS-1 C1–C3 (after which the emitted probe kinds are truthful; add a lint/test asserting every IfdProbeKind op actually realises).

---

## 6. Suggested sequencing (2 juniors, ~10 weeks)

```
Week  1-2 : A: WS-1 C1-C5 (correctness)         B: WS-2 V1-V3 (visibility + IFD decomp measurement)
Week  3-5 : A: WS-3 W1-W2 (worker mode)         B: WS-6 M1 (Linux falsifier) then WS-3 W3-W4 (AOT+hygiene)
Week  4-5 :                                     B: WS-4 F0 (audit report) once M1 is measured
Week  6-9 : A: WS-4 F1 (fiber mechanism)        B: WS-5 D1-D2 (COW side-arrays + in-place AOT)
Week 10   : A: WS-4 F2a (multi-root overlap)    B: WS-5 D3 (zygote spike) / WS-6 M2 if M1 green
Then      : WS-7 (phase-3 cache) as the next funded block; WS-8 opportunistic.
```

Decision points for the senior/owner: end of Week 2 (does the V3 IFD decomposition confirm IFD-blocked ≥ 40% of CI wall? if not, promote WS-5/WS-6 above WS-4), end of Week 5 (F0 audit → fund or kill fibers; M1 → fund or close Linux reclaim), end of Week 9 (F1 gate → F2a or park).

## 7. Standing rules (unchanged, restated because every task above touches them)

1. Rule 0: every commit kills a hypothesis; no exit-by-gate.
2. No new env gate without an inline retirement criterion at the first getenv site.
3. Full `--brute` ALL GREEN before every merge; subsets are inner-loop only.
4. Perf numbers: darwin-4 only (CPU), quiet Linux host (RSS/COW — NEW), commit-stamped + git-noted.
5. Every bug fix: positive + negative + regression tests; repros kept forever.
6. Layout changes (CallFrame/Thunk/Closure/AllocStats) → rebuild v3-smoke before brute, or false failures.
