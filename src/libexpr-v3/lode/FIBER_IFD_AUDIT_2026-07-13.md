# F0 — Fiber-based IFD-overlap prerequisite audit (WS-4)

**Date:** 2026-07-13
**Deliverable:** D5 of the CI-throughput goal. Report only — no F1 mechanism built.
**Question it answers → DECISION INPUT #3:** fund F1 (the 2–4-week suspend-on-realise mechanism) or delete the dormant fiber substrate (H1)?
**Copyright:** (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group. SPDX-License-Identifier: Apache-2.0.

---

## 0. Context & the substrate under audit

The CI pain is that IFD builds block evaluation **sequentially**: every IFD funnels through one synchronous choke point — `ffi::realisePath` (ffi.cc:172) or a direct `EvalState::realisePath` call → TW `EvalState::realiseContext` → **`buildStore->buildPaths(...)` (libexpr/primops.cc:258)**, which returns only when the build finishes. The whole process waits.

v3's eval state is heap data (`VMState{valueStack, frames, withStack}`), and the tree contains a **complete, GC-integrated, zero-call-site coroutine substrate** — `fiber.cc` / `include/v3/fiber.hh` (ucontext, 16 MiB mmap'd guarded stacks, `fiberCreate`/`fiberResume`/`fiberYield`/`fiberDestroy`, `currentFiber` TLS, exception capture). This is the *only* viable suspension shape: the interpreter C-recurses through `forceValue`/`callClosure`/nested `run()` towers (the `kMaxCallDepth=5000` guard exists because those towers are real), so a CPS/frame-only suspension is impossible — whole-stack suspension is required, and that is exactly what a fiber is.

F1 would: at a realise call, register `(DerivedPath → fiber)`, submit the build to a worker pool, `fiberYield`, and resume the fiber when the build completes — while OTHER fibers run. This audit checks the three things that must hold before that is sound and useful.

---

## 1. Parked-fiber vs the moving nursery — the load-bearing invariant

### Finding (verified): the scavenger does NOT scan parked fibers; only the major mark does.

The default-on generational GC has two collectors:
- **Cheney nursery scavenge** (`gc.cc`) — moving; copies survivors to the tenured arena and **rewrites pointers**. `grep -c fiber gc.cc` = **0**: the scavenger has no knowledge of fibers.
- **Major mark-sweep** (`mark_sweep.cc`) — its conservative phase calls `walkLiveFiberStacks(...)` (mark_sweep.cc:917), the ONLY caller in the tree, conservatively pinning every *yielded* fiber's stack region.

`fiber.hh:26-41` already documents this as latent hazard **N2**: a scavenge fired from a re-entry on a fresh `VMState` (the driver-callback path) cannot see a yielded fiber's `fiberVm`; it is in neither the handed-in `VMState` nor `activeVMStack`. The two shipped pre-flips (M-5) are:
- (1) `GC_add_roots(fiber->stack, …)` in `fiberCreate` (fiber.cc:221) — makes **Boehm** scan the fiber stack, covering **TW `nix::Value*`** held there.
- (2) `walkLiveFiberStacks` wired into the **major** mark (mark_sweep.cc:917).

**Neither pre-flip protects a v3 nursery cell referenced only from a parked fiber against a nursery SCAVENGE.** Boehm roots (pre-flip 1) do not stop the v3 moving nursery from relocating a v3 cell, and the scavenger never consults the parked stack. So: fiber A holds a freshly-allocated (nursery) v3 `Value` in a local, yields across a build; fiber B (or the driver) allocates enough to trip a scavenge; A's cell is moved (or, if unreferenced from any scanned root, reclaimed); A resumes holding a **dangling / stale** pointer. This is the PhD-6 missed-root UAF class, and it is the single biggest correctness risk in F1.

### Recommendation: force-scavenge-before-yield (option a), NOT conservative-pin (option b).

- **(a) Force a full nursery scavenge immediately before every `fiberYield`.** After a full scavenge, every live nursery cell has been promoted to the **tenured arena**, which is bump-allocated and **non-moving** in the default config (`g_majorGcEnabled` is hard-false; whole-block-free is gated off). So a parked fiber's stack then holds only **tenured (stable-address) pointers** — invisible-but-safe to any later scavenge (which only moves nursery cells). Cost: one scavenge per yield = one per IFD. IFDs are rare and already dominated by seconds-scale builds, so this is negligible. This reuses the existing safepoint-scavenge machinery (a scavenge is already fired at `exitDepth==0` safepoints).
- **(b) Make the scavenger walk + conservatively PIN parked-fiber stacks** (mirror `walkLiveFiberStacks` into gc.cc). Rejected: conservative pinning inside a *moving* collector defeats compaction (pinned cells fragment the to-space), reintroduces the exact "can't move ⇒ can't compact" tension the Immix/Cheney design avoided, and is far more code. Only revisit if force-scavenge-before-yield shows unacceptable cost on a real jobset (it won't at IFD cadence).

### Pre-committed stress test for F1 (before any perf work)

`fiberYield` at **every** realise site under `V3_DBG_NURSERY_AUDIT=1 V3_DBG_NURSERY_BRUTE=1` + 1 MB nursery (the `--brute` config), with ≥2 concurrent fibers each holding v3 Values across the yield, forcing a scavenge during the yield window. Gate: full `--brute` green + nixpkgs golden byte-identity. If force-scavenge-before-yield can't hold this within two weeks of fixing → **F1 KILL**, and fiber.cc should be deleted per H1 (its zero-call-site state is already a Rule-0 violation).

---

## 2. Blackhole collisions — how disjoint are overlapping strands really?

### Finding: overlap is bounded by shared memoisation, and the instrument to size it already exists.

A parked strand leaves `ThunkState::Blackhole` on every thunk it is forcing (vm.cc blackhole protocol). If a second runnable strand touches a thunk the first has blackholed, today it throws "infinite recursion (blackhole)". Safe overlap therefore requires EITHER provably-disjoint strands OR GHC-style **blackhole wait-queues** (a strand blocking on a foreign blackhole parks until the owner completes, then reads the memoised result).

How often would strands collide? That is exactly what `par_trace.hh` measures — the **work/span Amdahl ceiling** for intra-eval parallelism (par_trace.hh:5-21): `work/span ≈ 1` ⇒ fully serial (heavy shared memoisation, collisions everywhere); larger ⇒ more independent work. Prior runs put memo-sharing at **~50%** (0.48–0.53, dead-stable — project memory [[project_fund_134_phase0_2026-07-07]]), i.e. strands are far from disjoint on nixpkgs-shaped evals.

### Recommendation

- **Re-run `par_trace` on the REAL CI jobset shape BEFORE building wait-queues.** The ~50% figure is from single-root evals (firefox/M5). A CI *jobset* (many independent attr roots — see §4/F2a below) may be far more disjoint at the ROOT level even if each root's internals share heavily. The relevant number for F1 is cross-**root** collision rate, which has not been measured. This is a cheap measurement (par_trace already exists); it decides whether wait-queues are even needed for the F2a multi-root work source.
- If cross-root collision is low (likely): F2a multi-root overlap needs **no** wait-queues — distinct roots blackhole distinct thunks. Wait-queues become necessary only for the aggressive intra-root speculation (F2c), which this audit does not recommend funding.

---

## 3. Store / EvalState threading at the realise sites

### Finding: `EvalState` is single-threaded; only the build may leave the eval thread.

The realise entry points (post-WS-1): `v3RealisePathArg` (the 5 read primops), `ffi::realisePath` (import-attrs/readDir-attrs), direct `ns.realisePath` in readFile / readDir-str / import-str, and `ensurePath`/fetch for `storePath`/fetchers. All bottom out in one `EvalState`'s `realiseContext` → `buildPaths`. `EvalState` carries mutable shared state (symbol table, position table, the `realisedDerivations` memo this branch adds at eval.hh:1187, string/store caches) and is **not** designed for concurrent access; `import_timing.hh:64-66` states the VM is single-threaded outright.

### Recommendation: one eval pthread, builds on a worker pool.

- Keep **all eval fibers on a single pthread** (cooperative scheduling — fibers are cooperative by construction). No `EvalState` field is touched concurrently, so no locking of the evaluator is needed.
- Offload only `buildPaths` to **N worker threads**, each holding its **own store/daemon connection** (the store client is the concurrency unit; a daemon connection is not shareable across threads). The eval fiber yields; a worker issues the blocking `buildPaths` on its connection; on completion the scheduler marks the fiber runnable and the (single) eval thread resumes it and reads the realised outputs via the normal `resolveDerivedPath` path.
- `realisedDerivations` (the in-eval memo, primops.cc:121-163) must be consulted on the eval thread before dispatching a build, and updated on the eval thread after — never from workers (keeps it lock-free).

This is a standard "single mutator, parallel I/O" split; the only shared object the workers touch is the store, and store connections are per-worker. No `EvalState` re-entrancy is introduced.

---

## 4. The work-source problem (why suspension alone buys nothing)

Even with perfect, sound suspend/resume, **laziness discovers one pending IFD at a time** — a lone suspended fiber just waits. Overlap needs a second source of runnable work. Ranked (matches PRD WS-4 F2):

- **F2a multi-root eval (fund first, cheapest, no new soundness surface).** A CI jobset is many attr roots; evaluate them as one fiber per root. Independent roots ⇒ naturally independent IFD strands, and (per §2) likely low cross-root blackhole collision ⇒ **no wait-queues needed**. This is where the overlap actually comes from.
- **F2b S2 bytecode pre-walk** (speculative; needs a false-positive budget) — only if F2a's overlap is collision-bound.
- **F2c sibling-attr speculation** — highest ceiling, highest waste, needs cancellation + heap isolation + wait-queues. **Do not fund now.**

---

## 5. DECISION INPUT #3 — recommendation

**Conditional GO on F1, gated on DECISION INPUT #1 (D2/V3).**

The mechanism is de-risked at the substrate level: the coroutine exists and is GC-integrated for the major collector; the one hard correctness gap (scavenger vs parked fibers, §1) has a concrete, cheap, low-risk fix (force-scavenge-before-yield) with a pre-committed brute-stress gate; the threading model is a standard single-mutator/parallel-I/O split (§3); and the overlap source is F2a multi-root, which likely needs no blackhole wait-queues (§2, pending the cheap par_trace-on-jobset re-measure).

**But F1 is only worth funding if IFD-blocked is a large fraction of warm CI wall** — that is DECISION INPUT #1 from `bench/ifd-decomp.sh` (D2/V3). If the IFD-heavy workloads show IFD-blocked ≥ ~40% of warm wall, **fund F1 + F2a** (est. 2–4 weeks mechanism + the multi-root driver). If IFD-blocked is small even on IFD-heavy workloads (e.g. IFDs are pre-built and warm realise is cheap), **do NOT fund F1**; instead **delete fiber.cc under H1** (resolving its Rule-0 violation) and redirect to WS-5/WS-6 (parallel-eval density), which then dominate the CI win.

**Cheap pre-work regardless of the GO/NO-GO** (do these before committing to the 2–4-week mechanism): (i) the par_trace-on-real-jobset re-measure (§2), and (ii) a throwaway spike of force-scavenge-before-yield at a single realise site under brute stress (§1) to confirm the invariant holds. Both are days, not weeks, and either one falsifying kills F1 early.
