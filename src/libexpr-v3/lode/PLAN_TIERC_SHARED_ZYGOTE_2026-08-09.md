# Tier-C design — shared-memory "zygote" / shared-bytecode architecture for the nej worker pool

**Date:** 2026-08-08
**Author:** Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group. SPDX-License-Identifier: Apache-2.0.
**Status:** DESIGN ONLY (no source change, no push, no heavy eval). Grounded in the source read below.
**Repos:** `/Users/angerman/Projects/iohk/nix` (`src/libexpr-v3/`, branch `angerman/2.35-eval-profiling-v2`) + `/Users/angerman/Projects/iohk/nix-eval-jobs` (branch `2.34.1-v3`).

---

## 0. Executive framing — what is already built, what this plan adds

Two composable levers were requested. Source review shows **the hard parts of both already exist and are measured**; this plan is mostly *deploy + one topology change + a measurement gate*, not green-field.

- **Lever 1 (shared read-only bytecode segment) is essentially DONE** as the WS-5 AOT mmap path. `aot_cache.cc` maps one file `MAP_PRIVATE PROT_READ` (`aot_cache.cc:128`); `CompilationUnit` already split its read-only sections (`code`, POD constants, `lambdas`, formals) into borrowable views (`OwnedOrBorrowed`/`FlatArray`/`FlatStr`, `bytecode.hh:661-701`) with **all runtime-mutable state side-arrayed into `CompilationUnit::Runtime rt`** (`bytecode.hh:757-782`, WS5-D1). `WS5_COMPLETE_2026-07-16.md` measures **100 % CU borrow, 105 MB `Shared_Clean` across concurrent processes on HNE/haskell.nix**. What remains for Lever 1 is *deployment wiring* (build the AOT for the base pin, point workers at it) — already sketched in `CI_INTEGRATION_DESIGN_2026-07-17.md §3.2-3.3`.
- **Lever 2 (zygote) has a working prototype** (`v3-eval --fork-worker` / `--cow-fork`, `cli/v3-eval.cc:606-731`) with measured CoW density (`WS5_COW_BASELINE_2026-07-16.md`: 27 MB/child same-expr, 190 MB/child distinct-package). What remains is **porting the warm-parent-fork topology into nej's worker pool** so the *evaluated* base graph (not just bytecode) is shared — the strictly-bigger prize Lever 1 cannot reach.

The honest new content of this plan: (a) **Lever 1 shares bytecode only, never the ~700 MB evaluated import-cache value graph** — an mmap read-only segment *cannot* soundly hold the moving-GC value graph; (b) **only the zygote (CoW fork of a warm parent) shares that evaluated graph**; (c) a rigorous GC-safety argument for why the warm-parent fork is safe under the always-on moving nursery + Phase-D barriers + Boehm, and (d) a measure-first gate to size the zygote prize on *real haskell.nix* before building the nej topology change.

---

## 1. The problem — what each worker duplicates today

### 1.1 nej process model (as read)

`nix-eval-jobs.cc:main` sets `GC_DONT_GC=1` (`:592` — **GC is done by killing forks; Boehm never collects**), then spawns `nrWorkers` collector **threads** (`:662-665`). Each collector (`:449-561`) owns one worker **process** created by `Proc` via `startProcess` (a `fork()`, `:120-156`). A worker (`worker.cc:593-741`) builds **its own `EvalState`** (`:599`) and, under `NIX_V3_DIRECT_EVAL`, evaluates the flake root **once** via `evaluateFlakeV3`/`evalExprRoot` (`:655-683`) then loops `processJobRequest` (`:733`), descending one attrPath per job (`descendAttrPath`, `:494`). A shared `Sync<State>` todo/active queue (`nix-eval-jobs.cc:233-239`) feeds all collectors.

The fork that creates each worker happens **before any evaluation** (cold fork at collector startup). So each of the ~20 workers independently:

1. **re-parses + re-lowers** the ~13 haskell.nix/nixpkgs `source` imports (the machinery), unless the CU disk/AOT cache serves them;
2. **re-evaluates the base** — `builtins.getFlake` → native `call-flake.nix` (`v3_call_flake.cc:329`) → the top-level `hydraJobs`/`checks` attrset — into **its own tenured arena**;
3. **re-builds its own in-memory `ImportCache`** — `primops.cc:6271-6283`: a `std::deque<CompilationUnit> cus` (the imported base CUs) **plus** `std::unordered_map<std::string, ImportCacheEntry> results` where each entry holds a GC-managed `Value` (`primops.cc:6260-6262`). On HNE this is the ~700 MB `ImportCache` bucket (`HNE_BUCKET_DECOMP_2026-05-27.md`);
4. **re-primes its own applied-import cache** (`vm_applied_cache.cc`), keyed by `LambdaDescriptor* + canonical-args digest` (`:187-199`).

### 1.2 What is shareable / identical across workers

For a fixed jobset (fixed nixpkgs + haskell.nix pin), items 1-4 are **byte-identical across all workers** up to the point each worker's attrPath descent diverges. The base machinery is the same locked flake for every job. Quantified from existing measurements:

| duplicated per-worker cost | today | shareable by |
|---|---|---|
| CU bytecode + descriptors (read-only) | ~105-126 MB private ×20 (~2.5 GB) | **Lever 1 (AOT mmap)** → 105 MB shared once (`WS5_COMPLETE`) |
| parse + lower CPU of the machinery | full re-parse ×20 | **Lever 1** (deserialize-borrow instead of parse) + disk cache |
| evaluated base value graph (`ImportCache.results` + arena) | ~700 MB private ×20 | **Lever 2 only (zygote CoW)** — not mmap-able |
| applied-import cache (warm memo) | re-primed ×20 | **Lever 2 (zygote CoW)** |

**Target of this plan:** the third and fourth rows (the evaluated graph + warm memo), which Lever 1 structurally cannot reach, plus finishing the deploy of Lever 1 for the first two rows.

---

## 2. Design — both levers with concrete touchpoints

### 2.1 Lever 1 — shared read-only bytecode + import-cache **bytecode** segment (mmap)

**Mechanism (built).** One AOT flat file per base pin, mmap'd `MAP_PRIVATE PROT_READ`, process-lifetime, never unmapped (`aot_cache.cc:128-129`, `:52` lifetime note). CU read-only sections borrow spans into the mmap (`bytecode.hh:670-701`); the WS5-B2 canonical symbol/pos seeding (`aot_cache.cc:210-275`) makes the per-process id remap the identity so borrowed pages stay `Shared_Clean`. A CU not in the file falls back to the SQLite disk cache (`disk_cache.hh:53-65`, copied+owned) or fresh compile — **a stale/missing AOT is never a correctness risk, only a sharing-effectiveness one** (`CI_INTEGRATION_DESIGN §3.3`).

**What is shareable (from `WS5_D2_INPLACE_AOT_DESIGN` table):** `code`, `intConstants`, `floatConstants`, `lambdaCodeOffsets`, `lambdas` (post-D1), formals — all POD/read-only. NOT shareable in the mmap: `stringConstants` (per-process intern pointers), `primops` (re-resolved), and the entire `Runtime rt` side-object (per-process, heap). Correct by construction: the mmap holds **no GC pointer**.

**Deploy touchpoints (the only remaining work for Lever 1):**
- `bench/build-aot-cache.py` / `build-aot-cache-ci.sh` — build the AOT keyed to the jobset's nixpkgs/haskell.nix rev (derive from `flake.lock`, mirror `test/nixpkgs-pin.sh`).
- nej worker environment: `NIX_V3_AOT_CACHE_FILE=/var/cache/hydra/v3-aot/current.aot` (`CI_INTEGRATION_DESIGN §3.2`). `aot_cache::init()` runs eagerly at the top of `runRootExprFromString` (`aot_cache.cc:221`), which the nej Model-B path reaches via `evalFlakeRoot`/`evalExprRoot` → `runRootExprFromString` (verify — see open question Q3).
- systemd oneshot `v3-aot-cache` to rebuild-on-pin-change with atomic `ln -sfn current.aot` (`CI_INTEGRATION_DESIGN §3.3`).

**Note:** because nej already sets `GC_DONT_GC=1` and mmap is `MAP_PRIVATE`, Lever 1 sharing works **without any fork inheritance** — each worker independently `mmap`s the file and the OS page cache backs the shared read-only pages. It is the model-agnostic lever (works for process-per-job CI too).

### 2.2 Lever 2 — the zygote (warm-parent CoW fork), pushed into nej

**Prototype (built): `v3-eval --fork-worker`** (`cli/v3-eval.cc:606-731`). It warms ONE parent by evaluating `--expr` (`:613`), then per stdin request `fork()`s a child (`:679`) that evaluates against the parent's warm caches inherited CoW, writes the result down a pipe, and `_exit()`s without returning into the parent loop (`:703` — so it cannot corrupt the warm image). Boehm fork-safety is bracketed manually: `GC_set_handle_fork(-1)` before `GC_INIT` (`:350`) + `GC_atfork_prepare/child/parent` around every fork (`:677-708`). `--fork-jobs N` caps resident children (the density knob, `:714`).

**The nej integration — two candidate topologies:**

**Topology A — per-worker fork-server (low blast radius).** Change nej's `worker()` (`worker.cc:593-741`) so that, after `evaluateFlakeV3`/`evalExprRoot` warms the base (`:655-683`), instead of looping `processJobRequest` in-process it becomes a fork-server: for each job pulled from the parent pipe, `fork()` a child that runs `descendAttrPath` + `processDerivationV3` (`:494-504`), serialises the `Drv` JSON to the result pipe, and `_exit()`s. This is `--fork-worker` applied to nej's per-job descent.
- *Pro:* the worker is single-threaded (nej gives workers a 64 MB-stack `Thread` only in the parent; the worker process itself is single-threaded), so the fork is safe with the existing `GC_atfork` brackets. Minimal change to the collector/queue.
- *Con:* **it does not remove the cross-worker base duplication** — 20 workers still each evaluate the base once. It only shares the base across the *jobs within one worker*, which the in-process loop already does. **So Topology A buys almost nothing over the status quo for CPU, and nothing for the cross-worker RAM prize.** Its only marginal use is isolating a crashy job into a child (a correctness/robustness nicety). **Recommend NOT pursuing A for the sharing goal.**

**Topology B — single global zygote (the real win).** Evaluate the base **once** in a single-threaded warm parent, then fork the worker pool from that warm image so every worker inherits the *evaluated* base (arena + `ImportCache.results` + applied cache) CoW.
- Concretely in `nix-eval-jobs.cc:main`: after `initNix`/`initGC` and store pre-open (`:610-656`) but **before spawning the collector threads** (`:659-665`), add a single-threaded warm-up phase that engages v3 (`setFlakeSettings`; `evaluateFlakeV3`/`evalExprRoot` as in `worker.cc:655-683`) and **forces the top-level jobs attrset to WHNF** (not descending — leave the per-job thunks unforced so each child forces only its own). Then `threadNursery().forceScavenge()` to drain the nursery into tenured (stable addresses; see §3). Then fork the N workers; each inherits the warm base CoW and runs the existing collector/descent loop against it.
- The collector/queue currently lives in the parent's threads (`nix-eval-jobs.cc:449-561`); in Topology B the queue coordination must move to the forked workers talking to a coordinator over pipes (nej *already* has this exact pipe protocol — `getNextJob`/`processWorkerResponse`, `:347-423`). The restructure is "fork the pool from a warm parent and keep the existing pipe protocol" rather than "fork cold workers then eval".
- *Pro:* shares the ~700 MB evaluated graph + warm memo across all 20 workers → the RAM prize + the base-eval CPU done once, not 20×.
- *Con:* larger blast radius; the warm-up must be single-threaded (fork-before-threads); needs the `GC_set_handle_fork(-1)` + `GC_atfork_*` discipline nej does not yet have (nej relies on default atfork today).

**Recommendation:** design for **Topology B**, gated by the Phase-1 measurement (§4). Lever 1 (AOT) ships first regardless (it is deploy-only and already measured GO), and it *also* backstops B: even without B, B's children still borrow the AOT.

**Composition of the two levers.** They stack cleanly: the zygote parent maps the AOT (Lever 1) → its CU pages are `Shared_Clean` and file-backed; the fork (Lever 2) then shares the *evaluated* arena/import-cache/applied-cache on top via CoW. The AOT (read-only, file-backed) never dirties on fork; the evaluated graph shares until a child memoises a base thunk (the per-child private cost).

---

## 3. The GC / memory-safety analysis (the crux)

The load-bearing constraint (`src/libexpr-v3/CLAUDE.md §0`): **the generational nursery + Phase-D write barriers + gen-major collection are always-on; Boehm is the underlying page allocator.** Any shared-across-processes scheme must be sound against all four. The verdict below is **SAFE for both levers**, and the reasons are structural, not incidental.

### 3.1 The memory-model facts that make it safe

1. **`GC_DONT_GC=1` in nej (`nix-eval-jobs.cc:592`).** Boehm **never runs a collection** in the whole nej process tree. No mark threads, no sweeping, no freeing, no Boehm-side moving of any page. This eliminates the single scariest fork hazard (a collector thread mid-mark at `fork()`, or Boehm reclaiming a page a child still needs). Boehm degenerates to a bump allocator for TW `nix::Value`s at FFI leaves.
2. **The tenured arena is v3-owned and NON-MOVING.** `Arena` blocks are `calloc`'d (default) or `mmap`'d (`alloc.hh:2640-2748`), per-thread (`threadArena()`, `:2744`), and the mark-sweep of tenured (`mark_sweep.cc`) does **not move** survivors — only the nursery→tenured scavenge copies. So **once an object is tenured its address is stable for the rest of the process**. With `NIX_V3_ARENA_NOROOT=1` default-on (`alloc.hh:2738`, `:1469`) the arena is not even a Boehm root.
3. **The nursery is per-thread and moving** (`nursery.hh:70`, `threadNursery()` `:427`, `calloc`'d + `GC_add_roots` at `:386`). It is the *only* moving region.
4. **CUs are not GC cells.** `gc_layout.hh` (the layout manifest) scopes the GC walk to Closure/Thunk/Bindings/ListVec/ValuePair only; the CU / inline-cache walk is "keyed off a descriptor pointer, not an object child slot" (`gc_layout.hh:28-32`). WS5-D1 moved every runtime-mutable CU field (`forceCount`/`allocCount`/`callCount`/`cachedSingletonClosure`, ICs, `fromImportCU`) into the per-process `Runtime rt` heap side-array (`bytecode.hh:757-782`, `closure.hh:410-421`). The one GC pointer among them — `cachedSingletonClosure` (a `Closure*`) — lives at an address-stable side-array slot registered via `singletonClosureRegistry()` (`barrier.cc:52,77`) so the moving nursery forwards it. **The mmap'd read-only CU sections therefore contain zero GC pointers.**
5. **All GC roots are per-process after fork.** `dirtyContainers()` (the Phase-D remembered set), `standaloneCellRoots()`, `singletonClosureRegistry()` are `thread_local` (`barrier.cc:39-52,67-80`); the import-cache roots are walked from a process-global static (`walkImportCacheRoots`, `primops.cc:4634`). A `fork()` gives the child its own CoW copies of all of them.

### 3.2 Lever 2 (zygote CoW fork) — safety

The "shared segment" here is the parent's **entire warm heap** inherited CoW: the tenured arena, the nursery buffer, the CU deque (`ImportCache.cus`), the `ImportCache.results` value map, the applied cache, and the thread_local root vectors.

- **Moving nursery stays private.** After fork the child owns a CoW copy of the nursery buffer + bump pointer. Every child allocation and every scavenge (copy nursery→tenured, `memset` reset, `nursery.hh:268-291`) writes only the child's CoW pages. The parent's nursery is untouched. **Mitigation, required:** `forceScavenge` in the parent immediately before forking the pool so the inherited nursery is *empty* and every base survivor is already tenured (stable address). This (a) maximises `Shared_Clean` (no pending pointer moves in shared pages) and (b) removes the only scenario where parent and child could both scavenge the *same* young object into divergent tenured copies.
- **Phase-D barriers stay private and correct.** When a child forces a job-specific thunk and writes a nursery pointer into a CoW-shared tenured base `Bindings`/`Thunk`, the barrier (compile-time-active, `barrier.cc:102-110`) records the dirty slot in the child's *own* `tl_dirty` and the OS CoW-copies that arena page private to the child. The parent's remembered set and pages are unaffected. No cross-process barrier is needed because no child ever needs the parent (or a sibling) to observe its write.
- **Boehm is inert.** `GC_DONT_GC=1` → Boehm never collects in parent or child, so it can neither move nor free a shared page. Fork-time lock state is the only residual Boehm concern → mitigated by adopting `--fork-worker`'s discipline (`GC_set_handle_fork(-1)` + `GC_atfork_prepare/child/parent`, `cli/v3-eval.cc:350,677-708`) in the nej fork path.
- **Pointer stability of the shared segment.** Tenured is non-moving (§3.1.2) → the base graph never moves → shared base pages stay `Shared_Clean` until a child *writes* one. The only writes into shared base objects are **thunk memoisation** (`Blackhole→Evaluated`, `barrier.cc thunkSetEvaluated`) and IC updates — each is a CoW page copy, private, correct. This is exactly the `WS5_COW_BASELINE` per-child `Private_Dirty` (27-190 MB).
- **First-write semantics.** CoW makes the first write to a shared page transparently private — **no manual barrier or copy needed**; OS semantics guarantee isolation. This is why the WS5.0 prototype "held in practice" (`WS5_COW_BASELINE §Verdict`).

**What must never happen (invariants):** (i) the parent must not resume *evaluating* after forking children that share its live nursery — in Topology B the parent only forks/reaps/relays (it never forces a job); (ii) no child may assume another child or the parent sees its writes (guaranteed by independent-per-job descent); (iii) the nursery must be drained before the fork (mitigation above).

### 3.3 Lever 1 (mmap read-only segment) — safety

- The AOT is `MAP_PRIVATE PROT_READ`, process-lifetime (`aot_cache.cc:128,52`). Borrowed spans are read-only (`code[ip]` only). WS5-D1 guarantees **no code path writes a borrowed CU section** (mutables are in `rt`). So the pages are never dirtied → stay `Shared_Clean` cross-process and cross-fork.
- The moving GC never touches CU bytes (§3.1.4). The mmap holds no GC pointer, so the scavenger/mark-sweep never walk into it.
- A *buggy* write to a `PROT_READ` page would `SIGSEGV` — **contained** (see §6): nej's `handleBrokenWorkerPipe`→`WorkerDied` respawns and retries the attrPath on a tree-walker-only worker (`nix-eval-jobs.cc:251-315,515-552`). It cannot corrupt other processes (private mapping) and cannot silently produce wrong results.

**Verdict:** both levers are GC-safe. Lever 1 is safe because the shared segment is provably pointer-free and read-only. Lever 2 is safe because CoW privatises every mutation, the nursery is per-process, tenured is non-moving, and `GC_DONT_GC` neutralises Boehm — provided (a) the nursery is drained before the pool fork and (b) the fork adopts the existing `GC_atfork` brackets.

---

## 4. Phased plan — measurement FIRST (Rule 0)

### Phase 0 — Lever 1 deploy (already GO; do in parallel, no research)
Build the AOT for the base pin; set `NIX_V3_AOT_CACHE_FILE` on the nej workers; rebuild-on-pin timer (§2.1). **Gate:** fleet CU `Shared_Clean` ≥ 60 % of AOT (met at 83 % in lab, `WS5_COMPLETE`); drvPath byte-identity vs TW on one real jobset; `--brute` 41/41 both OSes. This ships the bytecode-sharing win independently of the zygote.

### Phase 1 — SIZE THE ZYGOTE PRIZE ON REAL haskell.nix (the gate that decides Topology B)
The existing `WS5_COW_BASELINE` numbers are from `v3-eval --cow-fork` re-evaluating the *same* firefox expr — **not** a warm haskell.nix base with children descending *distinct* jobs. Before touching nej, measure the actual shape:

1. **Base fraction of per-worker wall.** Instrument a single nej worker on a real haskell.nix jobset (e.g. cardano-node) to split wall into (a) flake-lock + getFlake + base-machinery eval (parse/lower/import-cache-build + `call-flake.nix`) vs (b) per-job attrPath descent. Use `V3_DBG_CALLFLAKE_TIMING` (`v3_call_flake.cc:335`) + `import_timing.hh` + `NIX_VM_STATS`. This is the CPU prize the zygote captures once instead of 20×.
2. **Per-child private RAM from a warm base with distinct-job descent.** Extend the `--cow-fork` harness (`cli/v3-eval.cc:519-569`, which already reads `/proc/self/smaps_rollup`) to: warm the parent by forcing the jobs attrset to WHNF, `forceScavenge`, fork, and in the child descend a *distinct* attrPath (a real job). Report child `Private_Dirty` vs `Shared_Clean`. This is the true per-job densification number (WS5.0 case B's 190 MB is an upper-ish proxy).
3. **Base evaluated-graph size.** Confirm the `ImportCache.results` + arena base footprint (the ~700 MB HNE bucket) via `NIX_V3_MEM_BUCKETS` (`cli/v3-eval.cc:929`) / `importCacheBytecodeBytes`/`ResultCount` (`primops.cc:6368-6410`).

### Phase 2 — build Topology B (only if Phase 1 clears the gate)
Warm-parent-fork in `nix-eval-jobs.cc:main` before the thread spawn; adopt `GC_set_handle_fork(-1)` + `GC_atfork_*`; drain nursery pre-fork; keep the existing pipe protocol for the forked pool. **Gate:** drvPath byte-identity vs Phase-0 TW on the same jobset; `--brute` 41/41; fleet `Private_Dirty` per worker measurably below a cold worker's; no worker OOM-restart storm.

### Phase 3 — retune the fleet
From Phase-2 smaps: raise `evaluator_workers`, lower `evaluator_max_memory_size` (`CI_INTEGRATION_DESIGN §3.4`) — measured, not guessed.

---

## 5. Pre-committed KILL criteria (Rule 0)

- **K1 (zygote CPU).** If Phase-1(1) shows the shareable base is **< 25 %** of per-worker wall on real haskell.nix → the zygote's CPU rationale is dead (Lever 1 AOT already covers bytecode); **KILL Topology B on CPU grounds** and ship Lever 1 only. (Split verdict: RAM may still justify B — see K2.)
- **K2 (zygote RAM density).** If Phase-1(2) shows per-child `Private_Dirty` from a warm base with distinct-job descent is **> 70 %** of a fresh worker's RSS (i.e. descending a job cascades memoisation writes across most of the shared base) → the zygote does not densify → **KILL Topology B**. (KPI-5 in `WS5_COMPLETE` passed at 11 % for same-expr; the distinct-job number is the unknown this kills on.)
- **K3 (GC-safety).** If a warm-parent fork with a drained nursery + distinct-attrPath children **fails `--brute` or diverges drvPath byte-identity** and the divergence is not a fixable missed-root/barrier bug → **KILL Topology B** (fall back to Lever 1 + the robustness-only per-worker fork-server if desired).
- **K4 (Lever 1 deploy cost).** If maintaining the per-pin AOT (rebuild cadence, cache-dir growth) costs more ops than the sharing saves → keep AOT opt-in per box; do not KILL the code (it's already shipped and correctness-neutral on miss).

A clean KILL here is a deliverable: it would confirm "Lever 1 (bytecode mmap) is the whole shippable win; the evaluated-graph zygote does not pay for real haskell.nix" — which is directly actionable for the deploy.

---

## 6. Failure modes + containment

| failure | trigger | containment (mostly already in nej) |
|---|---|---|
| worker writes into the shared **AOT** segment (`PROT_READ`) | a stale/buggy write to a borrowed CU section (should be impossible post-D1) | `SIGSEGV`/`SIGBUS` → `handleBrokenWorkerPipe`→`WorkerDied` (`nix-eval-jobs.cc:251-315`) → respawn + tree-walker retry of that attrPath (`:515-552`). Private mapping ⇒ no other process affected. |
| moving GC (scavenge) in a child touches shared **arena** pages | normal job descent memoising base thunks | CoW copies the page private → correct; cost = per-child `Private_Dirty` (the K2 number). Not corruption. |
| gen-major mark-sweep in a child dirties arena metadata | heap-growth trigger during a heavy job | CoW cost only; non-moving so addresses stable; correct in the private view. |
| child inherits a **half-full nursery** with pending moves | forking without draining | **Mitigation (required):** `forceScavenge` before the pool fork (§3.2). Without it, risk of parent+child scavenging the same young object divergently. |
| Boehm atfork lock state | fork while a Boehm lock is held | `GC_DONT_GC` means no marker threads; adopt `GC_set_handle_fork(-1)` + `GC_atfork_*` (as `cli/v3-eval.cc:350,677-708`). |
| **stale AOT** after a pin bump | jobset pin moves, AOT not rebuilt | not a correctness risk — v3 falls back to owned CU (`disk_cache.hh:53-65`); only sharing drops → rebuild-on-pin timer (§2.1). |
| parent resumes evaluating after forking | Topology B parent forces a job | forbidden invariant (§3.2 i); parent only forks/reaps/relays — enforce in code + review. |
| worker crash mid-job loses the attrPath | any SIGSEGV in a child | already handled: collector retries the single attrPath on a tree-walker-only worker (`nix-eval-jobs.cc:437-447,515-552`); never fails the whole eval (RR1-F2). |

---

## 7. Top-3 open questions

1. **Which topology does the ROI justify — A (per-worker fork-server) or B (global zygote)?** A is low-risk but buys ~nothing for the cross-worker sharing prize (the base is already amortised within a worker's in-process loop). B captures the real prize but restructures nej's fork-before-threads ordering and moves queue coordination into forked children. **The Phase-1 base-fraction + distinct-job `Private_Dirty` measurements decide this** (K1/K2).
2. **Does distinct-job attrPath descent keep the shared base `Shared_Clean`, or does memoisation cascade writes across most of the ~700 MB?** WS5.0 case B dirtied 190 MB re-evaluating a *distinct package* with no shared base; the warm-base + attrPath-descent number is unmeasured and is exactly what K2 kills on.
3. **Does the nej Model-B flake path actually engage the AOT + disk caches?** `eval_jobs_api.hh` (the `evalFlakeRoot`/`evalExprRoot`/`descendAttrPath` surface `worker.cc` links) is **not in this 2.35 worktree** — it lives on the published `input-output-hk/nix@angerman/2.34-v3` branch (Model-B accessor commits `dbbb0abd8`, `847123ca6`, `7e5325fe4`, `4ae029f1e`). Verify that `evalFlakeRoot`/`evalExprRoot` route through `runRootExprFromString` so `aot_cache::init()` (`aot_cache.cc:221`) + the SQLite CU cache fire on the nej path — Lever 1's entire premise for nej depends on it.

---

## Files actually read (for grounding)

**nix-eval-jobs:** `src/nix-eval-jobs.cc` (main/collector/Proc/queue/GC_DONT_GC), `src/worker.cc` (per-worker EvalState, v3 engage, processJobRequest, descendAttrPath).
**v3 (`src/libexpr-v3/`):** `CLAUDE.md` §0 (nursery/Phase-D/Boehm constraint), `cli/v3-eval.cc` (`--fork-worker`/`--cow-fork`/`GC_atfork`), `aot_cache.cc` (mmap reader + WS5-B2 seeding), `include/v3/bytecode.hh` (CompilationUnit + `Runtime rt`), `include/v3/closure.hh` (LambdaDescriptor + WS5-D1 note), `include/v3/gc_layout.hh` (layout manifest / GC-walk scope), `include/v3/nursery.hh` (per-thread moving nursery + Boehm root), `include/v3/alloc.hh` (Arena calloc/mmap + NOROOT + thread_local), `barrier.cc` (thread_local roots + compile-time Phase-D), `include/v3/serialize.hh` (schema 23 + the two caches), `include/v3/disk_cache.hh` (SQLite CU + EvalResults + borrow), `primops.cc` (ImportCache struct + root walk), `vm_applied_cache.cc` (desc+args memo key), `lode/WS5_COW_BASELINE_2026-07-16.md`, `lode/WS5_D2_INPLACE_AOT_DESIGN_2026-07-16.md`, `lode/WS5_COMPLETE_2026-07-16.md`, `lode/CI_INTEGRATION_DESIGN_2026-07-17.md`.

---

## WS-C BUILD SPEC + status (2026-08-09, post-K2 GO)

**Gates passed:** K1 (CPU) base = 34–46% of warm per-worker wall; K2 (RAM) per-child
private = job-incremental closure, base CoW-shared, <70% of fresh (coreutils ~1.6%, git
~51%; the memoization-re-dirty hypothesis was REFUTED by the coreutils control). git-noted
on `0ee094bf9` (nixpkgs proxy — getFlake not wired in v3-eval; ratio is architecture-driven
so it transfers to haskell.nix: sibling components ≈ coreutils tiny-private, cross-project ≈
git larger-private).

**SCOPE CORRECTION:** Lever-2 (zygote) is NOT "mostly wiring" — it is a nej worker-lifecycle
restructure. nej today: `main` spawns N collector *threads* (nix-eval-jobs.cc:658-664); each
thread lazily forks a worker *process* (`Proc`/`startProcess`, :120-151) that builds its OWN
EvalState and evaluates the flake from scratch. `GC_DONT_GC=1` (:592) — the zygote's GC-safety
key (Boehm never moves/frees, so CoW pages are stable).

**Topology B build steps (resumable):**
1. In nej `main`, AFTER flake setup but BEFORE spawning collector threads (:658): eval the
   common base (flake outputs + haskell.nix machinery) once in the single-threaded parent to
   warm the EvalState (CU cache, descriptors, import-cache value graph).
2. `nix::v3::forceScavenge()` to drain the nursery into the (non-moving, stable-address)
   tenured arena — so nothing young/movable crosses the fork.
3. Adopt v3-eval's fork hardening (cli/v3-eval.cc:376 `GC_set_handle_fork(-1)` +
   GC_atfork_prepare/parent/child brackets around the fork).
4. **Pre-fork the N worker processes here, in the single-threaded parent** (NOT lazily inside
   the collector threads — fork-from-multithreaded is unsafe). Each worker inherits the warm
   EvalState CoW and REUSES it (skip the per-worker base re-eval + re-parse/re-lower).
5. Then spawn the parent's N collector threads to pipe attrPaths to the pre-forked workers
   (the existing collector/queue protocol is unchanged downstream).
6. Wire Lever-1 (AOT deploy): build the AOT segment per pin, set `NIX_V3_AOT_CACHE_FILE` in
   the deploy so workers mmap the shared read-only bytecode (WS-5, 105 MB Shared_Clean).

**GATE (mandatory):** full `--brute` + firefox drvPath byte-identical UNDER the forked pool.
KILL: fork drvPath divergence unfixable (K3). **Faithful gate = LINUX** (the deploy platform;
smaps + fork semantics). Touches the nej repo + deploy infra → per discipline needs OK before
any push; the fork-pool gate wants the farm.

**STATUS: specified + de-risked; implementation NOT started** (fork-safety-critical nej
surgery + Linux-farm gate). This is the remaining WS-C deliverable.
