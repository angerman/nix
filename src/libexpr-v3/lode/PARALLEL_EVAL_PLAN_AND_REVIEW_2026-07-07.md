# Parallel eval for v3 — GC verdict, phased plan, hard-parts review, go/no-go (2026-07-07)

**Status: DESIGN / CRITICAL REVIEW. No code, no build.** Supersedes the pessimism
of `PARALLEL_EVAL_CAPABILITIES_2026-05-18.md` §5 (per-workload table) and closes
its §8 measurement-spike loop with the now-shipped trace data. Grounds every claim
in file:line + the `NIX_V3_PAR_TRACE` numbers (git-noted at `aac10b894`) + the
Determinate/Lix prior art.

Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output
Group. SPDX-License-Identifier: Apache-2.0.

---

## 0. TL;DR

- **GC verdict (headline): Option A is viable — and it is the RIGHT answer.** v3 can
  run parallel *mutators* with a **stop-the-world moving collector at a global
  safepoint** and per-thread nurseries, WITHOUT a concurrent/parallel moving GC.
  This is exactly how default-parallel GHC and JVM young-gen work: parallel
  mutators + STW copying collector. **Effort ≈ 4-7 months** (Determinate-shaped,
  our moving GC *parked* at the safepoint), **not** the 9-15 months a concurrent
  moving GC (Option B) would cost. Option B is a latency technique v3 does not need.
- **Why Option A works for v3 specifically:** the collector already fires ONLY at
  `exitDepth == 0` (vm.cc:4482-4560) — a *natural* safepoint — and the nursery,
  arena, dirty-list, and active-VMState registry are ALREADY `thread_local`
  (nursery.hh:686, alloc.hh:2871, barrier.cc:39, vm.cc:3777). The single-threaded
  design is, by accident of the moving-GC discipline, already shaped for the
  parallel extension. The hard new work is a *global* rendezvous + walking N
  C-stacks + N active-VMState registries at the STW point.
- **Phased MVP:** (P0 refactor thread-through) → **P1 atomic thunk state**
  (Suspended/**Pending**/**Awaited**/Blackhole/Evaluated/**Failed** + CAS + waiter
  list, à la Determinate) → **P2 lock-free symbol intern** → **P3 per-thread
  nurseries + global STW safepoint** (the moving-GC crux) → **P4 thread-safe
  caches** (ImportCache/AppliedCache/disk) + **FFI serialization** → **P5
  `parallelMap`/spark on the 1.7k-16.8k independent root forces** → **P6
  work-stealing**. Each phase has a pre-committed speedup gate.
- **Honest go/no-go: CONDITIONAL / DEFER.** Parallelism buys ~**3-4× wall-clock**
  (matching the ecosystem's measured ceiling — Determinate 4.1×/12t flake-show,
  3.0×/16t search) but is **ORTHOGONAL** to the two beat-TW walls (per-op 1.5-2.2×,
  RSS 1.6-2×) and to the moat. The DAG is wide+shallow (NOT Amdahl-dead: op-weighted
  W/S = 17.9× hello / 24.5× git / 46.6× M5) but **~50% of forces are memo-hits**
  (serialized-by-reuse) → the same sub-linear ceiling Determinate hit. And the
  **1.7k-16.8k independent root forces mean process-level parallelism
  (nix-eval-jobs / Hydra / `xargs -P`) already captures the throughput for free**.
  **Recommend: do NOT commit the full stack now.** Parallel eval is worth it ONLY if
  **single-eval LATENCY** (one big `nix eval`/`nixos-rebuild`/repl, un-splittable
  into processes) or a **long-lived daemon** becomes the pinned objective. If it
  does, do P1+P2 first (they are independently useful and low-risk), gate on P5.
  Otherwise the cheaper `PARALLEL_EVAL_CAPABILITIES` §6 alternatives (I/O
  concurrency ~4-8wk; speculative pre-forcing ~2-4wk) capture the realistic
  single-eval latency win at a fraction of the risk.

---

## 1. THE GC VERDICT (headline) — Option A vs Option B

### 1.1 The question, precisely

Determinate and Lix get 3-4× on cppnix WITHOUT touching the GC because Boehm is
already thread-aware (mark-sweep, non-moving, stop-the-world on heap pressure —
nesting-independent). v3's blocker is that its collector **MOVES** cells (Cheney
scavenge nursery→tenured with a forwarding map, gc.cc:85 `unordered_map<void*,
void*> forward`; Immix evacuation in the tenured region). You cannot relocate a
cell while another thread holds a raw `nix::Value*`/`Thunk*`/`Closure*` to it —
that is the PhD-6 root class, now multi-threaded.

Two ways out:

- **Option A — moving GC stays, STW at a global safepoint + per-thread nurseries.**
  All mutator threads run in parallel; when GC is needed they *all* rendezvous at a
  safepoint, one (or several cooperating) thread does the existing moving
  scavenge/major collection while the others are *parked*, then everyone resumes.
  Mutators are parallel; the collector is stop-the-world (it may itself be
  parallel-but-still-STW later). This is default GHC (`-threaded` copying GC: "when
  a GC is required, all the cores stop running Haskell code and report for GC
  duties") and JVM young-gen.
- **Option B — concurrent/parallel *moving* GC.** Collector relocates cells
  *concurrently* with mutation. Needs read/write barriers that cooperate with
  moving (Brooks/Baker forwarding, or a SATB/load-barrier à la ZGC/Shenandoah).
  This is the 9-15mo, Layer-5, very-high-risk path.

### 1.2 Verdict: **Option A. Option B is not required.**

**Option B is a LATENCY technique, and v3 does not have a latency problem it
solves.** Concurrent moving GC exists to shrink *pause times* for interactive/
real-time workloads (GHC's `--nonmoving-gc` is explicitly "low-latency" and costs
~10% throughput — LITERATURE_SWEEP §"myth-busts"). Nix eval is a *batch* workload:
we care about total wall-clock, not p99 pause. A STW pause that parks 8 threads for
the duration of a scavenge is *throughput-neutral* as long as GC is a small % of
runtime — and the profile says it is: **GC ≤ 7% on-CPU** across firefox/HNE/M5
(project memory PROFILE_AT_SCALE 2026-06-21). Paying 9-15mo + the historically-
worst class of VM bugs (concurrent heap walks) to shrink a ≤7% STW slice is
disproportionate. **Option A captures essentially all of the parallel-mutator win.**

**Option A is viable for v3 because the architecture is already 80% shaped for it:**

| Requirement for Option A | v3 today | Citation |
|---|---|---|
| A place where GC can safely fire (no raw ptrs mid-relocation) | GC fires ONLY at `exitDepth == 0` — the outermost dispatch boundary; inner dispatch loops are explicitly forbidden from scavenging | vm.cc:4482-4493 (nursery gate), 4514-4530 (scavenge), 4545-4560 (major) |
| Per-thread bump allocation (no alloc contention) | Nursery is `thread_local Nursery` already | nursery.hh:686 |
| Per-thread tenured arena | `thread_local Arena` already | alloc.hh:2871 |
| Per-thread remembered set / dirty list | `thread_local tl_dirty`, `tl_standaloneCells` | barrier.cc:39, 41 |
| A per-thread registry of live VMStates the collector must walk | `thread_local tlActiveVMStack` — added in #705 to solve the *nested-VMState* missed-root problem WITHIN a thread | vm.cc:3777 |
| Conservative C-stack scan to find raw ptrs in C locals | `walkCStackConservative()` via setjmp + platform stack-base | mark_sweep.cc:848-952 |
| Precise root walk of VM stacks | `walkAllV3Roots()` enumerates `activeVMStack()` | precise_root.cc:100; vm.cc:4636 |

The `exitDepth == 0` gate is the single most important fact: **v3 already has a
safepoint.** The single-thread rationale (vm.cc:4483-4495) is *exactly* the
multi-thread rationale — "inner dispatch loops hold nursery pointers in C-stack
locals; scavenging there would relocate objects leaving callers' locals stale." In
the parallel world this generalizes to: **each worker parks at its own
`exitDepth==0`; the collector runs when ALL workers are parked.** And
`tlActiveVMStack` (vm.cc:3777, "the scavenger walks ALL entries here so every
VMState on the call chain has its roots forwarded") is *precisely* the mechanism
that generalizes to "the STW collector walks every worker's active-VMState stack."
The nested-force problem #705 already solved single-threaded is the same shape as
the multi-thread root-enumeration problem.

### 1.3 What Option A still costs (the honest delta)

Option A is NOT free — it is "months, Determinate-shaped, GC parked," which is the
whole point of getting the verdict right. The genuinely-new work vs
single-threaded:

1. **Global rendezvous ("safepoint request").** Today `exitDepth==0` is a
   *local* condition per dispatch loop. Parallel needs a global "GC pending" flag
   that each worker polls at its `exitDepth==0` boundary (cheap: fold into the
   existing slow-gate mask the dispatch loop already checks — the same mask
   `opTick` folds into, par_trace.cc). A worker that reaches the boundary with the
   flag set blocks on a barrier; the last arriver runs GC; a generation counter
   releases the rest. **This is the classic "safepoint poll" and is well-trodden.**
2. **Walk N C-stacks + N active-VMState registries at the STW point.** Today the
   collector scans one thread's C-stack (mark_sweep.cc:848) and one thread's
   `tlActiveVMStack`. Parallel needs a *global registry of worker
   {C-stack-bounds, active-VMState-stack, nursery, dirty-list}* that the STW
   collector iterates. Each worker registers on spawn; the collector, once all are
   parked, scans each. This is mechanical but touches the root-enumeration core.
3. **Per-thread forwarding maps → one moving cell has one forwarder.** Cheney
   forwarding (gc.cc:85) is per-thread today (`threadScavengeBuffers()`, gc.cc:97).
   A cell tenured by thread A but referenced by thread B must be forwarded *once*
   and both see the new address. Under STW this is fine IF the collector treats the
   union of all thread heaps as one graph (single forwarding map for the STW
   cycle). This is the main *design* subtlety, not a barrier: STW means no mutator
   races the forward, so a single collector-owned forwarding map suffices.
4. **`g_singletonCapturedWithsRegistry` is process-global and UNGUARDED**
   (barrier.cc:56). Under threads this is a data race. Fix: make it thread-local
   like its siblings, or lock it. Small but must-fix (a UAF/corruption hazard).
5. **Parked-worker C-stack liveness.** A parked worker's C-stack still holds live
   roots (it's mid-eval). The barrier must guarantee a parked worker's stack is
   *quiescent* (not being written) when scanned — which parking at a safepoint
   gives you by construction. This is the JVM "all threads at safepoint" invariant.

**Effort estimate, Option A:** P0-P4 below ≈ **4-7 months** including race-bug
debugging (the perennial multiplier). This is *materially cheaper* than the prior
doc's 9-15mo because that estimate carried Layer-5 parallel GC (6-8wk) AND the
implicit assumption of concurrent collection. Parking the *existing* moving
collector at a *global* version of the *existing* `exitDepth==0` safepoint removes
the single largest and riskiest line item.

**Effort delta A→B:** +8-12mo and the highest-risk code in any runtime (concurrent
moving heap walk with mutator barriers). **Not justified** for a ≤7%-GC batch
evaluator. Option B is bookmarked, not planned.

---

## 2. MINIMUM-VIABLE-PARALLELISM — phased plan (files + gates)

Dependency-ordered. Each phase is independently shippable behind
`NIX_V3_EVAL_CORES` (default 1 = today's exact single-threaded path; N>1 opts in),
independently measurable, and carries a pre-committed speedup/regression gate.
Rule-0: a phase that fails its gate is reverted or its hypothesis killed, not left
as a coexisting opt-in.

**Correctness spine for every phase:** full `--brute` (22 suites) MUST stay green
at `EVAL_CORES=1` (byte-identical to today) AND at `EVAL_CORES=N` (drvPath
byte-identical to the N=1 golden). Multi-threading determinism is the gate that
catches races; drv-parity is the gate that catches divergence.

### P0 — Thread-through refactor (prereq; no behavior change)

Make the single global evaluator state explicitly per-worker. `VMState`
(vm.hh:127-170) is *already* a self-contained struct (valueStack/frames/withStack +
the LEVER-1 applied-cache fields) — the good news is this is mostly done. The work
is auditing the remaining `g_*` process-globals and static locals the dispatch/
alloc/GC path touches (the GC agent enumerated them) and confirming each is either
(a) already `thread_local`, (b) read-only-after-init (fine to share), or (c) needs
guarding. **Files:** vm.cc (dispatch), vm.hh (VMState), barrier.cc (the one
unguarded `g_singletonCapturedWithsRegistry`:56 → thread-local). **Gate:** N=1
byte-identical, `--brute` 22/22, zero perf delta. **Risk:** Low. **~3-4 wk.**

### P1 — Atomic thunk state machine (the Determinate core)

This is the heart, and v3's starting point is *cleaner* than the prior doc feared.

- **Today:** `enum class ThunkState : uint8_t { Suspended, Blackhole, Evaluated,
  Native }` (closure.hh:126-132) — a **plain byte**, no atomics. Transition sites:
  Suspended→Blackhole at vm.cc:9860 (before pushing the body frame); Blackhole→
  Evaluated at vm.cc:9055-9056 (`thunkSetEvaluated` writes the 8-byte result
  `evaluated` field FIRST, then the state). **The payload-before-tag discipline
  Determinate requires ALREADY HOLDS by luck** (barrier.hh:308-316). Blackhole
  cycle detection throws `BlackholeError` at vm.cc:15235.
- **Change (à la Determinate `parallel-nix-eval`):** make `state`
  `std::atomic<uint8_t>`; add **Pending** (being evaluated, no waiters), **Awaited**
  (being evaluated, ≥1 waiter), **Failed** (threw; payload = `std::exception_ptr`).
  Force protocol:
  - `Suspended --CAS--> Pending` (winner evaluates; only one thread wins).
  - Forcing a `Pending`: `Pending --CAS--> Awaited`, register self in a waiter list
    *stored separately from the cell* (Determinate keeps waiters off the value so
    the value stays 8 bytes and unmoved-by-waiters), then block.
  - Winner on completion: write `evaluated` payload, then atomic store `Evaluated`
    with release ordering (the discipline v3 already follows), then wake waiters.
  - Winner on exception: store `Failed` + `exception_ptr`, wake waiters (they
    rethrow).
- **Result fits one atomic word:** v3's `evaluated` is a single 8-byte NaN-boxed
  `Value` (closure.hh:214, value.hh:120-122 `sizeof(Value)==8`) — so, unlike the
  prior doc's worry about 16-byte results needing cmpxchg16b, **v3 needs only an
  8-byte payload store + a 1-byte atomic state swap.** The tag also lives *inside*
  the NaN-box word (value.hh:124-131), so the "make the type field atomic" move maps
  onto v3 as "make the `ThunkState state` byte atomic" — even simpler than making
  the whole Value atomic.
- **Cross-thread cycle detection:** the single-thread Blackhole==self-cycle
  invariant breaks. A `Pending`/`Awaited` owned by *another* thread is NOT a cycle
  — you wait. A cycle is now a *cycle in the waiter graph* (A waits on B waits on
  A). Need deadlock detection over the waiter edges (Determinate has "a couple of
  concurrency bugs to sort out" here — this is a known-hard corner). v3's existing
  cross-VM `Value::vBlackhole` sentinel path (vm.cc:15163) is the seed of the
  waiter-propagation mechanism.
- **Files:** closure.hh (state field + states), barrier.hh (thunkSetEvaluated
  ordering — already right), vm.cc (9860 force entry → CAS; 9055 completion → store
  + wake; 15163/15235 blackhole → waiter-graph). New: a waiter-list side table.
- **Gate:** `--brute` 22/22 at N=1 AND N=8 (races surface here first); drvPath
  byte-id N=8==N=1. No speedup gate yet (single-threaded scheduling in P1 — just
  correctness of the atomic protocol). **Risk: HIGH** (memory ordering + waiter
  deadlock). **~4-6 wk.**

### P2 — Lock-free (or sharded) symbol / string interning

- **Today:** `GlobalSymTab { std::vector<std::string> table;
  std::unordered_map<std::string,SymbolId> index; }` (ir.cc:48-82), accessed via
  `globalInternSymbol` (ir.cc:88-100) — **zero synchronization, append-only.**
  Exactly Determinate's pre-lock-free starting point ("the symbol table was
  rewritten to make symbol access lock-free").
- **Change:** append-only + CAS on the index (a symbol, once interned, never
  changes id → readers are lock-free; only insert contends). A sharded mutex or a
  concurrent hash map (the codebase already links a concurrent-map dependency —
  primops.cc:103) is the low-risk first cut; lock-free is the optimization.
- **Files:** ir.cc (GlobalSymTab). **Gate:** N=8 `--brute` green + no measurable
  intern contention in a symbol-heavy eval (flake-show). **Risk: Medium.** **~2 wk.**

### P3 — Per-thread nurseries + global STW safepoint (the moving-GC crux)

The Option-A payload. Nursery/arena/dirty-list are already `thread_local` (P-nothing
needed there). The new work is §1.3 items 1-3+5:
- Global "GC-pending" flag polled at each worker's `exitDepth==0` (fold into the
  existing dispatch slow-gate mask, vm.cc:4482 region).
- Global worker registry {C-stack bounds, `tlActiveVMStack`, nursery, dirty-list}.
- STW barrier: last-arriver runs the *existing* moving scavenge/major (gc.cc,
  mark_sweep.cc) over the union of worker heaps with one collector-owned forwarding
  map; generation counter releases parked workers.
- Fix `g_singletonCapturedWithsRegistry` (barrier.cc:56) race (if not already in
  P0).
- **Files:** vm.cc (safepoint poll + barrier + worker registry), gc.cc (multi-root
  scavenge over registry), mark_sweep.cc (multi-C-stack scan — generalize
  `walkCStackConservative` to iterate the registry), barrier.cc, nursery.hh.
- **Gate:** N=8 `--brute` 22/22 under the aggressive 1MB-nursery brute stress (this
  is where a missed cross-thread root shows up as a UAF); drvPath byte-id; GC STW
  pause ≤ (single-thread GC time × small constant) so throughput stays ≥ linear-ish.
  **Risk: VERY HIGH** (the moving-GC-under-threads correctness core — but bounded by
  STW, which removes the concurrent-heap-walk class). **~6-10 wk.**

### P4 — Thread-safe caches + FFI serialization

- **Caches (unguarded plain maps today):** ImportCache (primops.cc:6463-6540, ~700
  MB burden, `unordered_map` + `deque<CompilationUnit>` with LRU eviction),
  AppliedCache (primops.cc:6765-6789 — **the LEVER-1 moat**), disk cache
  (disk_cache.cc; SQLite already has an init mutex, aot_cache.cc:38-45). Need
  thread-safe insert + lookup. **Design subtlety:** LRU eviction mutates on *read*
  (`bumpImportEntry`, accessCounter++) → a naive shared-map + eviction is a
  write-on-read contention hotspot. Options: sharded caches, or read-mostly with
  epoch-based reclamation, or per-worker cache with a merge at safepoint. The
  AppliedCache moat is small (8 entries default) → a single mutex is fine; the
  ImportCache is the contention risk.
- **FFI (the serialization point, and the M5 question):** ~1015 LoC FFI infra, 104
  static TW-cross sites, 59 ffi.hh wrappers (FFI_AUDIT_2026-05-20; ffi.cc). cppnix's
  `EvalState`/libstore/Boehm are NOT thread-safe. Start with a **global FFI mutex**
  (prior doc §6, Determinate-agnostic): any worker entering TW (import/IFD via
  `realisePath` ffi.cc:172, `derivationStrict`, fetchers ffi.cc:629-693,
  `addPathFull`/`filterSource` ffi.cc:518-620, readFile/readDir, `getFlake`
  ffi.cc:868, `outputOf` ffi.cc:828) takes the lock. Migrate to per-API locks or a
  designated-TW-thread only if contention shows.
- **HOW MUCH OF M5 IS FFI-BOUND (must measure before trusting M5 speedup):** M5 is
  IFD-heavy (project memory: "M5 IS IFD, x86_64 plan-to-nix ~33s one-time"). If the
  serialized IFD/store fraction is large, the FFI mutex caps M5's parallel win hard
  — this is the Amdahl term that isn't in the pure-eval trace (the trace is
  idealized, ignores FFI serialization, par_trace.hh explicitly). **A per-workload
  "FFI on-CPU + wall-in-TW" measurement is the P4 pre-gate.**
- **Files:** primops.cc (caches), ffi.cc/ffi.hh (mutex), disk_cache.cc. **Gate:** N=8
  green + cache/FFI contention not eating the P5 speedup. **Risk: HIGH** (FFI
  thread-safety of cppnix "in unexpected ways" — prior doc §6). **~3-4 wk + debug.**

### P5 — `parallelMap` / spark on the independent root forces (the payoff phase)

The fan-out unit is *measured*: **1.7k-16.8k mutually-independent top-level root
forces** per eval (par_trace.cc:73-79 `g_rootForces`, "top-level, independent";
git-noted `aac10b894`). A drvPath eval is driven by top-level bytecode launching
many independent root forces — the obvious embarrassingly-parallel fan-out. Also the
canonical `mkDerivation { buildInputs = [a b c]; }` shape (prior doc §3 option c).
- **Design:** a `parallelForce`/spark that hands independent root forces (or
  buildInputs lists) to a worker pool; sparks are hints (idle worker steals, else
  dropped — GHC model). Strictness-analysis-driven sparking (prior doc §3 option b)
  is the principled version but needs Stage 4; the *pragmatic first cut* is
  pattern-matching the top-level root fan-out + `buildInputs`, which needs no
  strictness pass.
- **Files:** vm.cc (dispatch — spark creation at root-force sites + a worker pool),
  a new scheduler module. **Gate (THE speedup gate): ≥ 2.5× wall-clock on 8 cores**
  on flake-show / flake-check (the wide workloads) at byte-identical output. If <2×,
  the memo-sharing serialization (§3) has won and the whole stack is refuted →
  revert. **Risk: Medium** (scheduling correctness on top of P1-P4). **~3-4 wk.**

### P6 — Work-stealing deque (scale-out)

Chase-Lev lock-free deque (prior doc §12 refs). Only after P5 proves the ceiling is
worth chasing to higher core counts. **Files:** the P5 scheduler. **Gate:** improves
16-core scaling over P5's simpler pool. **Risk: Low** (well-documented). **~2 wk.**

**Cumulative Option-A MVP (P0-P5): ~4-7 months. P6 optional.**

---

## 3. RANKED HARD-PARTS / UAF REVIEW

Ranked by risk × likelihood-of-sinking-the-project.

### HP-1 [CRITICAL] Moving GC under threads — raw `Value*`/`Thunk*` in C locals across the safepoint (the PhD-6 class, now × N)
The whole reason Option A is non-trivial. A raw cell pointer in a *parked* worker's
C local must be found (conservative C-stack scan, mark_sweep.cc:848) and, because the
collector MOVES, *updated* via the forwarding map. Single-threaded v3 already lives
this (the `exitDepth==0` gate + `tlActiveVMStack` #705 exist *because* of it,
vm.cc:3777/4483). Multi-thread mitigation: STW parking guarantees each worker's stack
is quiescent when scanned (JVM safepoint invariant); one collector-owned forwarding
map per STW cycle removes mutator-vs-forward races. **The residual risk is the
worker registry being incomplete** — a live worker whose C-stack/VMState the
collector forgot to scan = the exact `tlActiveVMStack` missed-root UAF #705 fixed,
generalized cross-thread. Brute-stress (1MB nursery, N=8) is the detector.

### HP-2 [CRITICAL] FFI serialization + the M5 Amdahl term
cppnix TW/libstore/Boehm are not thread-safe. Every store-path/`derivationStrict`/
IFD/fetch call (104 cross-sites) serializes on the FFI lock. For IFD-bound M5 this
may cap the parallel win far below the idealized 46.6× W/S (the trace explicitly
ignores FFI serialization, par_trace.hh). **Must measure FFI wall-fraction per
workload before promising M5 numbers.** The V3-NATIVE constraint (fewer FFI
primops) directly reduces this contention — another reason V3-NATIVE matters.

### HP-3 [HIGH] The ~50% memo-sharing contention — shared thunks are CAS hotspots (this IS stdenv)
**~48-53% of all force-requests hit already-evaluated/shared thunks** (git-note
`aac10b894`; par_trace.cc:70 `g_memoHits`). Under parallel eval those shared thunks
(stdenv, lib, the fix-point) are exactly where every worker's CAS collides —
`Suspended→Pending` contention + waiter-list pile-ups on the hottest ~50% of forces.
This is *precisely* Determinate's measured sub-linearity: "most threads cannot make
progress until the `stdenv` value has been computed" → 3.0× on 16 threads for
search. **The memo-sharing that makes v3 fast single-threaded is the same thing that
caps its parallel scaling.** No fix — it's structural (the shared subgraph is shared
work). It sets the realistic ceiling at ~3-4×, not the idealized W/S.

### HP-4 [HIGH] Cross-thread blackhole / cycle detection & waiter-graph deadlock
Single-thread: Blackhole-on-self = cycle → `BlackholeError` (vm.cc:15235).
Multi-thread: a Pending/Awaited owned by *another* thread is a *wait*, not a cycle;
a real cycle is a cycle in the *waiter graph*. Need deadlock detection over waiter
edges without false-positiving on legitimate cross-thread waits. Determinate's own
"couple of concurrency bugs to sort out" live here. v3's cross-VM `Value::vBlackhole`
sentinel (vm.cc:15163) is the propagation seed. **Property test needed** (cycle
detection totality across threads — debug-story item #7, not yet built).

### HP-5 [MEDIUM] Thread-safe cache inserts with LRU-on-read
ImportCache/AppliedCache mutate on *read* (LRU bump: primops.cc:6500
`accessCounter++`, `bumpImportEntry`). A shared map with eviction is a
write-on-read contention point AND an eviction-vs-live-reference race (evicting an
entry a worker is mid-read = UAF). AppliedCache moat is tiny (8 entries) → one mutex.
ImportCache is the real one → sharding or per-worker + safepoint merge.

### HP-6 [MEDIUM] `g_singletonCapturedWithsRegistry` unguarded process-global
barrier.cc:56 — the one non-thread-local mutable GC registry. Straight data race
under threads. Must go thread-local or locked in P0. Small but a corruption class.

### HP-7 [LOW-MED] Determinism of drvPath under nondeterministic scheduling
The output must be byte-identical regardless of which worker forces what when. Nix
eval is pure, so results are scheduling-independent BY SEMANTICS — but any residual
order-dependence (error message ordering, trace interleaving, first-writer-wins in a
cache) is a divergence. The N=8==N=1 drvPath byte-id gate is the detector; it must
run in `--brute` at every phase.

---

## 4. HONEST GO / NO-GO

### 4.1 What parallelism is and isn't
- **Delivers:** ~**3-4× wall-clock** on wide workloads at 8-16 cores (matching the
  ecosystem: Determinate 4.1×/12t flake-show, 3.0×/16t search, 3.7× flake-check,
  5× the IFD test project; sub-linear by HP-3). The DAG is **NOT Amdahl-dead** (the
  prior doc's 1.2-2× table was too pessimistic — op-weighted W/S 17.9-46.6×; the
  width is there).
- **Does NOT touch the beat-TW walls.** Parallelism is orthogonal to per-op
  efficiency (1.5-2.2× wall, needs JIT) and RSS (1.6-2× — and parallel makes RSS
  *worse*: N nurseries + N worker heaps). It does not narrow the single-core gap to
  the tree-walker at all.
- **Does NOT extend the moat.** The repeated-eval result cache (LEVER-1, the actual
  strategic win) is a *cross-invocation* lever; parallelism is *intra-invocation*.
  Complementary at best, and parallel eval *complicates* the moat (thread-safe cache
  inserts, HP-5).

### 4.2 The process-parallelism problem (the decisive counter-argument)
The **1.7k-16.8k independent root forces** are also the reason intra-process
parallelism is *often unnecessary*: that same independence is what
**nix-eval-jobs / Hydra / `xargs -P` already exploit at the process level, at zero
v3 engineering cost and zero race risk.** For batch throughput (evaluate many
derivations/attrs/systems — the flake-show/flake-check/Hydra shapes where parallel
eval shines *most*), process-level parallelism captures the win *for free*. The
narrow residual case for *shared-memory* parallelism is:
- a **single big `nix eval`** that is one un-splittable expression with internal
  parallelism (a single `nixos-rebuild`/`nix build` eval, a repl), where **latency**
  (not throughput) is the objective and you cannot fork it into processes; OR
- a **long-lived daemon** amortizing warm state where duplicating that state across
  processes is wasteful (and even then, the moat + a shared cache may serve better).

### 4.3 Comparison to the cheaper §6 alternatives
For the single-eval-latency case, the prior doc's §6 alternatives are much cheaper
and race-free:
- **I/O concurrency (async, no parallel eval):** overlap readFile/readDir/fetch/
  store-query/IFD while eval continues. ~4-8wk, no atomic thunks, no parallel GC, no
  race class. Same FFI-thread-safety question (HP-2) but bounded. Captures a real
  slice of latency on I/O-heavy evals.
- **Speculative pre-forcing (single-threaded, scheduled):** eagerly force
  provably-strict positions (buildInputs) for cache locality. ~2-4wk post-Stage-4.
  Cap ~1.2-1.5×. No threads.

### 4.4 Recommendation
**DEFER the full stack. Do not commit P0-P6 now.** Rationale, in one line: parallel
eval buys ~3-4× wall-clock that is (a) orthogonal to both beat-TW walls and the
moat, (b) already free at the process level for the wide/batch workloads it helps
most, and (c) costs 4-7 months + the multi-threaded-race bug class, gated behind the
~50% memo-sharing ceiling and the FFI/M5 Amdahl term.

**Commit ONLY IF** a pinned objective appears that process-parallelism cannot serve:
**single-eval LATENCY** (one un-splittable big eval / interactive repl) or a
**shared-state daemon**. If it does:
1. **First, and independently useful even standalone:** P1 (atomic thunk state) +
   P2 (lock-free symbols). These are the Determinate core, they harden the thunk
   machine, and P2 removes a real single-thread hot-path allocation. Low-to-medium
   risk, ~6-8wk combined.
2. **Then measure the FFI/M5 Amdahl term (HP-2 pre-gate)** and the memo-contention
   (HP-3) on the *target* workload before P3-P5. If FFI wall-fraction is high on the
   target, stop — the ceiling is below 2×.
3. **P3-P5 only if the pre-gate clears.** P5's ≥2.5×/8-core gate is the final
   falsifier.

**Else (no latency/daemon objective): pursue the cheaper alternatives** — I/O
concurrency (§4.3) for I/O-bound latency, speculative pre-forcing for locality — and
keep steering throughput to process-level parallelism. **Retire `par_trace.cc` per
its own retirement criterion** (par_trace.hh §"Retirement criterion") — the
parallel-potential question is now decided by this doc.

---

## 5. Cross-references & provenance
- Prior candidate design: `PARALLEL_EVAL_CAPABILITIES_2026-05-18.md` (this doc
  supersedes its §5 pessimism + closes its §8 spike; §6 alternatives + §12 reading
  list still valid).
- Measured parallel potential: `LITERATURE_SWEEP_2026-07-07.md` §6 + git note on
  `aac10b894` (`NIX_V3_PAR_TRACE`: op-weighted W/S 17.9× hello / 24.5× git / 46.6×
  M5; deepest chain 129-191 vs 0.77M-11M forces; **48-53% memo-hit**).
- Determinate `parallel-nix-eval` (atomic `type` field, Pending/Awaited/Failed, CAS,
  waiter list stored off-value, payload-before-type, lock-free symbol table; 4.1×/12t
  flake-show, 3.0×/16t search) + `changelog-determinate-nix-3111` (2.2-5× real cases,
  `eval-cores=0`). Lix adopting.
- GHC default parallel GC = STW copying, per-capability ~1MB nurseries, cores
  rendezvous for GC ("all cores stop and report for GC duties"); `--nonmoving-gc` is
  the latency alternative at ~10% throughput cost (= Option B analog).
- Code anchors: safepoint `exitDepth==0` vm.cc:4482-4560; conservative C-stack scan
  mark_sweep.cc:848-952; moving forwarding map gc.cc:85; thread-local nursery
  nursery.hh:686 / arena alloc.hh:2871 / dirty-list barrier.cc:39 / active-VMState
  vm.cc:3777; ThunkState closure.hh:126-132 (uint8_t) + transitions vm.cc:9860/9055 +
  BlackholeError vm.cc:15235 + cross-VM sentinel vm.cc:15163; Value 8B NaN-box
  value.hh:120-131; VMState vm.hh:127-170; symbol table ir.cc:48-100; caches
  primops.cc:6463 (Import) / 6765 (Applied); FFI ffi.cc/ffi.hh + FFI_AUDIT
  (~1015 LoC / 104 cross-sites / 59 wrappers); unguarded global barrier.cc:56.
