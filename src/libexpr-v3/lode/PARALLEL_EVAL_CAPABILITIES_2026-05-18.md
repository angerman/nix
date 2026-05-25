# Parallel Eval — multi-core capabilities (CANDIDATE)

**Status: CANDIDATE, NOT COMMITTED. Includes self-correction of previous-turn overstatements.**

This document explores whether v3 could be extended to a multi-core capability-based parallel evaluator in the style of GHC's parallel runtime. It is the result of an analysis turn followed by a critical-review turn that found several material overstatements; both the proposal and the corrections are recorded here.

Like `PERF_STRATEGY_2026-05-17.md` (salsa/HAMT) and `LINKING_DESIGN_2026-05-17.md` (content-addressed cells), this is candidate work that would only commit to the roadmap after a measurement spike validates the load-bearing hypotheses.

## 1. The question

Could we extend v3 to something like multi-core GHC with capabilities — multiple OS threads cooperating on a shared heap, sparks for deferred parallel work, work-stealing scheduler, parallel GC?

Short answer: **yes, technically feasible; engineering cost substantial; empirical justification not yet established; many cheaper alternatives unexplored.** Do not commit. Measurement spike (§8) proposed instead.

## 2. GHC's capability model (reference)

Per Marlow, Peyton-Jones, Singh, *Runtime Support for Multicore Haskell* (ICFP 2009) and subsequent papers:

- A **capability** is "an OS thread permitted to execute Haskell code." A Haskell program runs with N capabilities, typically N = number of cores.
- Each capability owns: a **run queue** of Haskell threads (TSOs), a **spark pool** of deferred parallel work, a **thread-local nursery** for allocation, and the currently-executing TSO.
- **Sparks**: `par a b` creates a spark for `a`, returns `b`. Sparks are hints, not threads — an idle capability may steal a spark from a busy capability and convert it to real evaluation. If no one steals, the spark is silently dropped.
- **Work-stealing scheduler**: idle capabilities steal sparks (or whole TSOs) from busy ones. Standard Cilk-style lock-free deque (Blumofe & Leiserson, FOCS 1994).
- **Sharing**: shared heap across capabilities. Atomic operations on thunk headers. When capability A is forcing thunk T, T's header atomically transitions to a `BlackHole` referencing A's TSO. Capability B that needs T sees the BlackHole and blocks (parking the TSO on the BlackHole's blocking queue). When A finishes, the BlackHole becomes an Indirection pointing to the result; blocked TSOs wake up.
- **Parallel GC**: stop-the-world coordinated mark-and-sweep, or local-heap collection (Marlow et al., ISMM 2011).

Total RTS work: 5+ years of focused engineering by Simon Marlow and collaborators, with multiple academic papers documenting each layer.

## 3. How this would map to v3 — component sketch

Six layers. Estimates are MY revised estimates (see §5 for prior-turn underestimation acknowledgment).

### Layer 1 — Atomic thunk state machine

Today v3's thunk-state transitions (`Suspended → Black → Evaluated`) are non-atomic memory writes. Multi-core needs atomic CAS-based transitions with appropriate memory ordering:

```cpp
struct Thunk {
  std::atomic<HeaderBits> header;  // tag + payload pointer
  ...
};

// Capability A trying to force thunk T:
auto expected = packSuspended(body);
auto desired = packBlack(myCapability);
if (T.header.compare_exchange_strong(expected, desired,
                                     std::memory_order_acq_rel,
                                     std::memory_order_acquire)) {
  // Won the race — this capability owns the force.
  Value result = evaluate(body);
  T.header.store(packEvaluated(result), std::memory_order_release);
  wakeBlockedCapabilities(T);
} else {
  // Lost the race.
  switch (decodeState(expected)) {
    case BLACK:
      if (decodeOwner(expected) == myCapability) {
        // Real cycle within our own thread chain.
        throw BlackholeError(T);
      } else {
        // Different capability owns the force; block.
        blockOnThunk(T);
      }
      break;
    case EVALUATED:
      return decodeResult(expected);
  }
}
```

The hard parts:
- v3's thunk result is a 16-byte tagged Value, larger than a single atomic word. Either use `cmpxchg16b` (x86) / DCAS / LL/SC (ARM), or use an indirection: header points to either body or result block.
- Memory ordering: which loads need `acquire`? Which stores need `release`? The wrong choice causes either correctness bugs (race conditions) or performance regressions (over-strict barriers).
- Block/wake protocol: blocked TSOs need a queue per blackholed thunk; the queue itself needs concurrent-safe access.

Realistic estimate: **3-4 weeks**. The previous turn said 1-2 weeks, which understated the design work.

### Layer 2 — Per-capability VM state

Today v3 has a single `VMState`: frame stack, value stack, with-stack, exception state, current TSO equivalent.

Multi-core needs N of these, encapsulated in a `Capability` structure that also owns the spark pool, run queue, nursery, and current TSO.

```cpp
struct Capability {
  VMState vm;                            // private to this capability
  WorkStealingDeque<Spark> sparkPool;    // can be stolen from
  std::deque<TSO*> runQueue;              // private
  Nursery nursery;                       // thread-local allocator
  TSO* currentTSO;
  uint32_t capId;
  ...
};
```

Most existing single-threaded code becomes "operates on a Capability passed by reference." This is mechanical refactor but invasive: every function that touched `g_vm` or implicit state needs a Capability parameter.

Shared data structures (Stage 9 cell store, Stage 5 shape table, string-intern table) need concurrent-safe access. Read-mostly with occasional inserts → `folly::ConcurrentHashMap` or similar pattern.

Realistic estimate: **3-4 weeks**.

### Layer 3 — Sparks and a `par`-like primitive

The semantically interesting design choice: how do parallel opportunities get expressed?

**Option (a) — explicit `builtins.par`**: programmer-controlled. Simple. Requires nixpkgs annotations to exploit; nobody will add them.

**Option (b) — strictness-analysis-driven**: Roadmap Stage 4's strictness pass identifies positions where a value WILL be deep-forced. The lowerer emits sparks for the strict positions at construction time. Most leverage; depends on Stage 4 quality.

**Option (c) — pattern-matched on `mkDerivation`**: shape-match the canonical `mkDerivation { buildInputs = [a b c]; }` and spark `a, b, c`. Nix-specific; targets the highest-frequency pattern.

Right design is probably (b) layered on Stage 4 (strictness analysis as the prerequisite), with (c) as an opportunistic special case for hot patterns.

Realistic estimate: **3-4 weeks** post-Stage-4. Cannot start without strictness analysis in place.

### Layer 4 — Work-stealing scheduler

Standard Cilk-style work-stealing deque. Each capability pushes new sparks to the bottom of its own deque (LIFO for itself); steals from the top of another capability's deque (FIFO for them). Lock-free implementation is non-trivial but well-documented (Blumofe & Leiserson 1994, Chase & Lev 2005 *Dynamic Circular Work-Stealing Deque*).

Realistic estimate: **2 weeks** once Layers 1-2 are stable.

### Layer 5 — Parallel GC

**This is the hard part.** Two real choices:

**5a — Stop-the-world parallel mark-sweep**: all capabilities synchronize at GC points; multiple threads cooperate on the shared heap. Simpler. Builds on Stage 3's nursery (`CHENEY_NURSERY_DESIGN.md`).

**5b — Local-heap collection**: each capability minor-collects its nursery independently; major collections coordinate. Better scalability (Marlow et al. ISMM 2011); much harder.

5a estimate: **6-8 weeks** on top of Stage 3. 5b estimate: **add 8-12 weeks** for the local-heap protocol.

Risk: concurrent GC is the historical home of the hardest VM bugs. Race conditions during heap walks have hidden for years in mature systems.

### Layer 6 — FFI / cppnix bridge serialization

**Critical and underrated.** cppnix's Boehm GC is not designed for arbitrary multi-threaded access. Every v3 → TW FFI call (store ops, derivations, file I/O, IFD) must serialize.

Patterns:
- **Designated TW thread**: one capability is permitted to call TW; others post requests via a queue. Scales better.
- **Global FFI mutex**: any capability acquires a lock to enter TW. Simpler; risk: contention dominates.
- **Per-API locks**: separate locks for store-read, store-write, file-IO, etc. Middle ground.

Probably start with the global mutex and migrate to per-API as bottlenecks emerge. The V3-NATIVE constraint (LESSONS_LEARNED §1.1) matters more than ever: the more v3-native primops, the less FFI serialization.

Estimate: **2-3 weeks** for the lock infrastructure; **months of debugging** if it turns out cppnix's primops are not thread-safe in unexpected ways.

## 4. Cumulative cost estimate (revised)

| Layer | Effort | Risk |
|---|---|---|
| 1 — Atomic thunk state | 3-4 weeks | High (memory ordering, blocked-TSO queue) |
| 2 — Per-capability VM state | 3-4 weeks | Medium (mechanical refactor; surface area large) |
| 3 — Sparks + primitive | 3-4 weeks (post Stage 4) | Medium |
| 4 — Work-stealing | 2 weeks | Low |
| 5a — Stop-the-world parallel GC | 6-8 weeks | Very high (concurrent heap walk) |
| 6 — FFI serialization | 2-3 weeks + debugging | High (cppnix not designed for it) |
| **Subtotal (without 5b)** | **19-25 weeks focused effort** | **Cumulative: very high** |
| Testing & race-condition debugging | **+ ongoing** | Multi-threading bugs hide for years |
| 5b — Local-heap GC (optional) | + 8-12 weeks | Very high |

Realistic total with testing and validation against real workloads: **9-15 months**.

The previous turn estimated 6-12 months; revising upward. The previous-turn's Layer 1 estimate of 1-2 weeks was the most egregious underestimate.

For reference: Simon Marlow's parallel-GHC work took 5+ years of focused effort with multiple academic papers documenting each layer. We are not Marlow, but we can read his papers. The 9-15 month estimate is for engineering that imports the well-documented patterns — not for inventing them.

## 5. What it would buy (revised down from previous turn)

Honest workload-by-workload table:

| Workload | Parallel potential | Realistic 8-core speedup | Confidence |
|---|---|---|---|
| `pkgs.hello.name` (deep dep chain) | Low — Amdahl-bound by sequential stdenv | 1.2-1.5× | Medium-high |
| `pkgs.haskellPackages.X.outPath` (deep + many siblings) | Medium | 2-3× | Medium |
| `nix flake show nixpkgs` (25k independent attrs) | High but limited by FFI | 3-5× | Medium |
| `nix flake check` (many independent checks) | High | 3-5× | Medium |
| Hydra eval (jobsets × systems) | High at jobset level (already process-parallel) | 2-4× on eval phase only | Low |
| Interactive `nix run pkg` first time | Low — single chain | 1.1-1.3× | High |
| `nixos-rebuild switch` eval | Medium for eval; build phase already parallel | 1.5-2× eval; smaller wall-time | Medium |

**Why these are revised down from the previous turn:**

1. **Amdahl's law on the stdenv bootstrap**: every nixpkgs eval threads through stdenv-stage0 → stage1 → stage2 → final stdenv. This is a sequential dependent chain. If 20-30% of total eval is in this chain, 8-core speedup is capped at ~3-4× regardless of how much parallelism we extract elsewhere.

2. **FFI serialization caps Hydra wins**: thousands of store-path calls per Hydra eval, each contending on the FFI lock.

3. **Hydra is already process-parallel at jobset level**. Within-process multi-core helps individual jobset evals but not the aggregate.

4. **I/O bottleneck**: many real Hydra workloads are dominated by tarball fetches and store-path lookups, not pure eval.

## 6. Alternatives I missed in the previous turn

The previous turn implicitly assumed shared-memory multi-core was the path. Three alternatives that should be considered first because they're cheaper and may capture most of the win:

### 6.1 Process-level parallelism

The simplest "parallelism for Nix" is N independent `nix` processes, one per core. Hydra already does this at the jobset level.

For many workloads — eval many flakes, check many systems, batch-evaluate independent attrs — process-level parallelism gives essentially the same benefit with **essentially zero engineering cost** in v3. Use `xargs -P` or a job scheduler.

The case for **shared-memory** parallelism (which is what capabilities buy) is much narrower than the previous turn implied:

- Workload must not be splittable into independent processes — i.e. a single `nix eval` of a single expression that has internal parallelism.
- Shared state (cell store, shape table, parsed/lowered modules) must be large enough that duplicating across processes is wasteful.
- The expression must have enough intra-eval parallel structure to exceed Amdahl bounds.

If most "parallelism for Nix" cases can be solved with `xargs -P` and existing process-level patterns, the multi-core capability work isn't justified. **Measurement should distinguish these cases.**

### 6.2 I/O concurrency without full parallelism

Many Nix primops do I/O. We could overlap I/O — read N files concurrently while eval continues — **without committing to full shared-memory parallel eval**. This is the `async/await` model (Rust's tokio, JavaScript Promises, GHC's `forkIO`).

Specifically:
- `builtins.readFile`, `readDir`, `pathExists` — file I/O
- `builtins.fetchurl`, `fetchTarball`, `fetchGit` — network I/O
- Store-path queries — IPC to nix-daemon
- IFD — disk + build

A single-threaded v3 with async I/O could keep the eval core busy while I/O is in flight. Concurrency, not parallelism. **Much cheaper** to implement (no atomic thunk state, no parallel GC, no race condition risk), and captures a real fraction of the "real" parallel opportunity.

Estimate: **4-8 weeks**, compared to 9-15 months for full multi-core. Same Layer-6 FFI question (cppnix has to permit concurrent calls), but no Layer 1-5 needed.

### 6.3 Speculative pre-forcing (single-threaded, but scheduled)

Even without multi-threading, we can be smarter about eval order. When v3 sees `mkDerivation { buildInputs = [a b c]; ... }`, the lowerer knows `a, b, c` will be deep-forced. Today's lazy eval forces them on demand. A speculative variant forces them eagerly during a "warm-up" phase, exploiting better cache locality.

Single-threaded; no race conditions; no parallel GC; no FFI lock. Net benefit: better instruction cache, reduced backtracking, possibly some prefetch wins. Cap probably 1.2-1.5×.

Estimate: **2-4 weeks** post-Stage-4 (strictness analysis identifies the safe positions).

### 6.4 SIMD or batch-evaluator for primop-heavy paths

Some primops are arithmetic / string ops on large lists. A SIMD-optimized inner loop for `genList`, `foldl'`, string concat could give within-thread speedup on those specific paths. Niche; only relevant if profile shows these primops dominate.

Estimate: **case-by-case**, probably 1-2 weeks per primop family.

## 7. Prerequisites in the current roadmap

For full multi-core capability work to be sensible, v3 needs to mature significantly first:

1. **Phase 1-4 (action plan)**: must close. vm.cc decomp (Phase 4) is essentially required; a 9755-line vm.cc with embedded primops and bridge plumbing is not refactorable into per-capability structure.
2. **Stage 3 (nursery default-on)**: thread-local nursery is a Layer-5 prerequisite. Phase D write-barriers must work first.
3. **Stage 4 (uniform STG + strictness analysis)**: identifies where sparks are safe (Layer 3). Without strictness analysis, sparking is either programmer-annotated (won't happen) or guesswork (wasteful).
4. **Stage 8 (thin FFI)**: every FFI call becomes a serialization point in Layer 6. Thinner FFI = less contention.
5. **Stage 9 (linking)**: cell store needs thread-safe inserts.
6. **Phase 1.5 measurement spike (PERF_STRATEGY)**: should be extended to ALSO measure parallel potential (see §8).

That's a large prerequisite stack. Multi-core capabilities is plausibly **Stage 13** in the roadmap — post-Stage 9, post-Stages 10-11 if they commit, post any salsa caching work.

## 8. Proposed measurement spike (extension of Phase 1.5)

The previous turn proposed a 6-week speculative-pre-forcing "starter" as the way to test the multi-core hypothesis. That's still substantial engineering with race-condition risk. A **cheaper** spike — pure measurement, no multi-threading code introduced:

### Goal

Determine the realistic upper bound on multi-core speedup for typical Nix workloads, BEFORE committing engineering. If the spike shows <2× theoretical maximum on 8 cores, the case for full capability work collapses.

### Method

1. **Trace v3 evals to capture dependency DAG.** During eval, record for each forced thunk T: (a) thunk identity (cell hash from Stage 9), (b) parent thunk that triggered forcing, (c) wall-clock duration of the force itself (excluding nested forces). Output: a (parent, child, duration) trace.

2. **Offline analysis.** Build the dependency DAG; compute:
   - **Critical path length**: longest sequential chain of forces. Lower bound on single-thread runtime.
   - **Parallel work** total: sum of all force durations.
   - **Theoretical speedup**: parallel work / critical path. This is the absolute maximum on infinite cores.
   - **N-core speedup**: simulate scheduling on N cores (greedy list scheduling); measure how close to theoretical we get with realistic N=4, 8, 16.
   - **Sequential fraction**: what % of work is on the critical path?

3. **Workloads to measure**:
   - `(import <nixpkgs> {}).hello.name` — small.
   - `(import <nixpkgs> {}).haskellPackages.aeson.outPath` — medium with siblings.
   - `nix flake show nixpkgs` — large independent batch.
   - `nixos-rebuild dry-build` — module-eval heavy.
   - cardano-node flake — real-world heavy overlay.

4. **Output**: one-page report `MEASUREMENT_SPIKE_PARALLEL_*.md`.

### Why this is the right Rule 0 move

The hypothesis "multi-core capabilities are worth building" rests on "v3 evals have substantial parallel structure." We can test this with trace analysis — **no multi-threaded code needed, no race conditions, no GC redesign**. If the spike shows critical paths are 60%+ of total work, the multi-core ceiling is below 2× and the engineering is not justified. If the spike shows <30% critical path (i.e. most work is parallel-eligible), the ceiling is above 3× and the engineering may be justified.

The total spike cost is probably **1-2 weeks** of instrumentation + analysis. The total avoided cost on a wrong-direction commitment is **9-15 months**. Asymmetric leverage; should obviously do the cheap measurement first.

### Estimate

1 week instrumentation + 1 week analysis + writeup = **2 weeks**. Can run alongside Phase 1.5 salsa measurement (some shared instrumentation infrastructure).

## 9. Falsification criteria for the candidate (when/if it advances)

Should the measurement spike clear and the candidate become committed work, each layer has a kill criterion:

| Stage | Falsifies on | Action if falsified |
|---|---|---|
| Trace measurement | Critical path >60% of total work | Don't commit multi-core; pursue process-level + I/O concurrency instead |
| Layer 1 (atomic thunk) | Race-condition tests find correctness bugs that resist 4 weeks of debugging | Pause; reconsider whether v3's thunk shape is right for shared-memory parallelism |
| Layer 3 (sparks) | Strictness analysis identifies <20% of positions as safe-to-spark | Sparks won't pay off; pursue I/O concurrency only |
| Layer 5 (parallel GC) | Stress test shows heap corruption that resists 2 weeks of debugging | Defer to local-heap (5b) or abandon shared heap |
| Final integration | Real-workload bench shows <2× on 8 cores | The multi-core hypothesis is refuted; revert |

## 10. Honest assessment of what this would and wouldn't fix

Even if we built it perfectly:

**Would help:**
- Hydra-style batch eval (single process, many derivations)
- Large `nix flake show / nix flake check` runs
- Workloads with substantial intra-eval parallelism that don't fit process-level decomposition

**Wouldn't help much:**
- Single deep-chain eval (`pkgs.hello.name`)
- Interactive `nix repl` (single eval at a time)
- Wall-clock of `nixos-rebuild switch` (build phase dominates)
- Workloads where I/O dominates (already addressable by §6.2 async I/O)

**Could regress:**
- Small workloads (`nix eval --expr '1 + 2'`) — multi-threading overhead can make them slower
- Memory-constrained environments — per-capability nurseries × N
- Latency-sensitive primops if FFI lock contention is high

## 11. Cross-references

- `LESSONS_LEARNED_2026-05-15.md` §1.1 — V3-NATIVE constraint; relevant because FFI thinness directly impacts §6 contention.
- `PERF_STRATEGY_2026-05-17.md` — salsa hypothesis; complementary to (not substitutive of) parallel eval.
- `ROADMAP_TO_VISION_2026-05-15.md` — current stage structure; multi-core would be Stage 13+.
- `CHENEY_NURSERY_DESIGN.md` — Stage 3 nursery design; Layer 5a builds on it.
- `LINKING_DESIGN_2026-05-17.md` — Stage 9 cell store; Layer 2 shared state.
- `ACTION_PLAN_2026-05-15.md` Phase 1.5 — measurement spike; this proposes an extension.

## 12. Reading list

Required if the team seriously considers this direction:

1. **Marlow, Peyton-Jones, Singh, *Runtime Support for Multicore Haskell* (ICFP 2009)** — the canonical GHC RTS reference. Covers capabilities, sparks, work-stealing, BlackHoles. https://www.microsoft.com/en-us/research/publication/runtime-support-for-multicore-haskell/

2. **Marlow et al., *Multicore Garbage Collection with Local Heaps* (ISMM 2011)** — Layer 5b reference.

3. **Blumofe & Leiserson, *Scheduling Multithreaded Computations by Work Stealing* (FOCS 1994 / JACM 1999)** — work-stealing scheduler used by Cilk, GHC, Go.

4. **Chase & Lev, *Dynamic Circular Work-Stealing Deque* (SPAA 2005)** — modern lock-free work-stealing deque.

5. **Marlow, *Parallel and Concurrent Programming in Haskell* (O'Reilly, 2013)** — practitioner's reference; chapters on `par`, `seq`, Strategies translate directly to design choices for a Nix `par` primitive.

6. **Herlihy & Shavit, *The Art of Multiprocessor Programming* (2nd ed., 2020)** — for the concurrent data structure choices in Layers 1-2.

## 13. Critical-review summary (self-correction record)

Items where this document corrects the previous-turn analysis:

| Previous claim | Correction |
|---|---|
| "Nix is better fit than Haskell for parallel pure eval" | Roughly equivalent at runtime semantics; weaker at static guarantees. Overstated. |
| "Atomic thunk state in 1-2 weeks" | Realistic 3-4 weeks; memory-ordering design + blocked-TSO queue are the load-bearing work. |
| "Speculative pre-forcing starter in 3-4 weeks" | Realistic 6-10 weeks. Still expensive for a hypothesis test. |
| "6-12 months total" | Revised to 9-15 months including testing and real-workload validation. |
| "Hydra 6-8× on 8 cores" | Realistic 2-4× on the eval phase; eval is often not the dominant cost. |
| "Salsa would absorb the warm wins so multi-core becomes optional" | Wrong framing. Salsa is cross-invocation; multi-core is intra-invocation. Complementary, not substitutive. |
| "Process-level parallelism" | Not even mentioned. Probably captures most of the win at zero v3 cost; should be the first thing measured. |
| "I/O concurrency without parallelism" | Not even mentioned. Much cheaper, captures a real fraction of the gain. |
| "Amdahl bound from stdenv bootstrap" | Not even mentioned. Caps 8-core speedup at ~3-4× regardless. |
| "Memory ballooning concern" | Not even mentioned. Per-capability nurseries × N is real. |
| "Backwards-compat risk" | Not even mentioned. 25 years of single-threaded cppnix assumptions. |
| "Speculative pre-forcing as the spike" | Wrong choice. Trace-analysis (§8) is the cheaper, race-condition-free spike that actually answers the load-bearing question. |

The pattern in these corrections is that the previous-turn analysis was **too implementation-eager and not measurement-eager enough**. The right Rule 0 move is the §8 measurement spike. The full multi-core capability work is only justified if measurement returns results consistent with the hypothesis.
