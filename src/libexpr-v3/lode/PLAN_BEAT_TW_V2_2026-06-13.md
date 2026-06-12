# PLAN BEAT-TW v2 — profile-anchored plan with quality gates (2026-06-13)

Supersedes the open items of `PLAN_BEAT_TW_2026-06-12.md` + `PLAN_BEAT_TW_EXECUTION_2026-06-12.md`.
Anchored in NEW measurements taken 2026-06-13 (laptop, `build/src/nix/nix` @ HEAD 777d286c7,
every run byte-identical to TW; CPU = min user over 3; profiles via `/usr/bin/sample` 1 ms).
Laptop numbers are used for ATTRIBUTION (shares are host-robust); all keep/revert BARS are
darwin-4 + `bench/v3-vs-tw-gate.sh` per the measurement gate.

---

## 0. New measurements that rewrite the priorities

### 0.1 The firefox CPU anomaly is the major GC — measured end to end

| arm (laptop, min of 3) | CPU | RSS | vs TW CPU |
|---|---|---|---|
| firefox v3 default | 4.65 s | 623 MB | 2.61× |
| firefox v3 `NIX_V3_NO_MAJOR_GC=1` | **3.46 s** | **600 MB** | **1.94×** |
| firefox TW | 1.78 s | 333 MB | — |
| hello v3 default → no-GC | 1.12 → 1.10 s | 270 → 264 MB | 1.56× → 1.53× |
| git v3 default → no-GC | 1.80 → 1.74 s | 349 → 345 MB | 1.80× → 1.74× |

`NIX_VM_STATS` on the firefox default arm pins it exactly:

```
gc_count=1  markMs=969.29  sweepMs=177.24      ← 1.15 s = the whole A/B delta
markedCells=3,936,343  liveBytes=262.2MB  deadBytes=224.3MB  reclaim%=46.1%
blocksFreed=0  bytesFreed=0.0MB                ← the GC reclaimed NOTHING
deadLines=42.72% (Immix acceptance ≥30%)
```

**Today's major GC on drvPath workloads is pure CPU waste**: it marks 3.9 M cells at
~246 ns/cell (≈10× slower than a good mark), finds 224 MB dead — all *scattered*
(zero fully-dead blocks) — and frees 0 bytes. RSS is *unchanged-to-better without it*.
Three structural facts follow: (a) the GC trigger/benefit policy is wrong; (b) the mark
itself is ~10× optimizable (per-edge block lookup); (c) the 42.7 % dead lines are exactly
what line-granular reuse would capture *if the GC ran mid-eval with reuse behind it*
(workstream E). Note: **mid-eval live = 262 MB** on firefox — the "live ≈ 30 MB" figure
is end-of-eval only; all GC yield models must use 262 MB.

### 0.2 CPU profile attribution (self-time shares, I/O excluded)

| firefox v3 (~3180 samples) | share | | hello v3 (~1720) | share |
|---|---|---|---|---|
| GC mark/sweep bucket | **34 %** | | dispatchLoop self | 26 % |
| dispatchLoop self | 19 % | | **primIntersectAttrs** | **10 %** |
| **primIntersectAttrs** | **11 %** | | **materialize + its introsort** | **9 %** |
| malloc/free family | 6 % | | **lookupStringContextEntries** | **6 %** |
| mergeBindings (+lambdas) | 3 % | | forceValue | 4 % |
| context side-table probe | 2 % | | malloc/free family | 7 % |
| forceValue/callClosure | 3 % | | _tlv_get_addr | 1.5 % |

TW reference (firefox, ~1570 CPU samples): interpreter dispatch spread ~16 %, **parser
~6 % every run** (v3's disk cache skips this — a banked v3 advantage), layered-Bindings
iteration heap-merge ~8 %, malloc ~10 %, the rest long-tail. The drv-localized levers
(wrapper skeleton, key round-trips, mergeBindings micro-cost) are arithmetically ≤0.5 %
each — **the 2026-06-12 "wrapper-skeleton-bound" diagnosis is retired** (Rule 0: killed
by this profile + the design-pack arithmetic).

### 0.3 Two concrete defects found by the profile (both session-sized)

1. **`primIntersectAttrs` iterates the wrong side** (`primops.cc:1969-1989`): both passes
   walk the FULL `src` — in the dominant callPackage pattern
   (`intersectAttrs (functionArgs f) pkgs`) src ≈ 20 k entries, keep ≈ 10. ~1.6 k+ calls
   × ~40 k entry-visits each. TW iterates the smaller set. Fix: iterate `keep`,
   binary-search `src` (O(|keep|·log|src|) ≈ 100× less work; output = keep-order = sorted;
   entries copied from src — byte-identical by construction).
2. **`materialize()` full-sorts already-sorted layers** (`std::introsort` over LevelEntry —
   9 % of hello CPU): chain layers are individually sorted; a k-way merge + dedup
   (layers ≤ 8) replaces the O(n log n) sort. (Workstream A may remove most materialize
   calls anyway; this is the cheap interim + covers the residual consumers.)

### 0.4 Premise corrections from the design agents (Rule 0 bookkeeping)

- **"GET_LOCAL_FORCE fusion miss under `let`" is STALE** — at HEAD all four shapes emit
  the same fused 7-dispatch body. The real residual is the **cross-Force deferral flush**
  (`emitOne(Force)` unconditionally `flushAllDeferred()` at `emit.cc:1148`) → 7→4 possible.
- **"The C-stack scanner exists only in the evac path" is WRONG at HEAD** — the non-moving
  mark already has the transitive conservative scanner (`walkCStackConservative`,
  mark_sweep.cc:659-753, wired at :1907-1918). The depth>0-GC blocker is the
  `exitDepth==0` trigger gate + one real hazard class (H-B below), not the mark.
- **"Immix reuse isn't on the hot path" is imprecise** — it IS in `Arena::alloc`
  (alloc.hh:1267-1307); it engaged ~14 allocs because spans are born at the single
  end-of-eval GC. Starved by trigger placement, not unwired.
- **Chain-SELECT corruption (2026-06-07): the C-1 hypothesis is SUPPORTED.** The spike
  was never committed; the surviving design comment (vm.cc:8682-8703) describes exactly
  the stale-KEEP signature; sibling contamination semantically requires a wrong-value
  writer (closed thunks; TW's own layered Bindings memoize through shared layers across
  all of nixpkgs with no materialize, src/libexpr attr-set.hh:353-373); and — decisive —
  **materialize never actually isolated anything**: the python3/firefox corruptions went
  through the SHARED `s_matMemo` flat copy with materialize ON. All four known
  wrong-value writers are fixed at HEAD (arm-on-PAP, C-1, C-3, C-4).

---

## 1. Standing quality gates (apply to every item below)

| Gate | Requirement |
|---|---|
| **QG-1 Correctness** | lang 143/143 + core 20/20 + run-pap*/chain-parity suites + byte-identical drvPath on the 7 gate rows. **Drv-hash-critical levers** (marked ⚠) additionally: `bench/chain-nixpkgs-fullsweep.sh` on darwin-4 + the 2026-06-07 failure set pinned as tests (git, cargo, rustc, cargo-auditable.cargoDeps, python3.withPackages — real store, disk cache off). |
| **QG-2 Performance** | `bench/v3-vs-tw-gate.sh` ENGAGED + IDENTICAL; min-user-CPU; **RSS bars on darwin-4 only** (laptop RSS noise ≈ the bars); every item ships with BOTH CPU and RSS deltas (memory-first-class). |
| **QG-3 Pre-commit** | keep/revert bars written in the work item BEFORE measuring (below); no post-hoc adjustment except via the threshold-recalibration rule. |
| **QG-4 Ratchet** | `bench/baselines/seven-rows.tsv` checked in after each landed item; a `make ratchet-check` target fails if any row regresses >3 % CPU or >5 % RSS vs the file without a justification line in the commit. |
| **QG-5 Rule 0** | every commit names the hypothesis it kills; falsifications get a lode line + memory entry. Stale-binary check (explicit ninja rebuild) + `tail -1` on stats dumps + no OPCYCLES across nested loops + verify the main-eval stats dump actually fires when using `v3-eval` directly (a probe this session saw `total=6` = install-CU-only counting). |

---

## 2. Wave 1 — the measured quick wins (each session-sized; ~1 week total)

| # | Item | Mechanism | Bar (keep / revert, darwin-4) |
|---|---|---|---|
| 1.1 | **GC policy: stop paying for a GC that frees nothing** | (a) default initial threshold 256 MB → 1 GB (growth 2.0 unchanged); (b) adaptive backoff: after any major GC with `bytesFreed < 5 %` of heap, threshold ×4. Guard A/B first: HNE + M5 on darwin-4 with the new policy — pre-committed: if either regresses peak RSS >5 %, keep 256 MB for them via the backoff only. | firefox CPU −≥20 % @ RSS Δ ≤ +2 % / revert if any row's RSS +>5 % |
| 1.2 | **primIntersectAttrs: iterate the smaller side** (§0.3.1) | two passes over `keep` with binary search in `src`; byte-identical by construction | drv rows CPU −≥5 % / <2 % |
| 1.3 | **materialize k-way merge** (§0.3.2) | replace introsort with ≤8-way sorted-layer merge + dedup | hello CPU −≥3 % / <1 % |
| 1.4 | **has-context bit on string cells** | flag co-located with the Chars allocation (writers already funnel through `setStringContextEntries`, readers through `lookupStringContextEntries` — alloc.hh:3928/3934); context-less strings (the vast majority) skip the unordered_map probe entirely | hello CPU −≥3 %, firefox −≥1.5 % / <1 % |
| 1.5 | **Cross-Force deferral** (`emit.cc:1148`) | flush only when `e.thunk` itself is pending; fold body 7→4 dispatches; IR-CHECK fixtures incl. Many-occurrence negative + branch-flush guard (#668 class) | foldl CPU −≥2 % / <1 % |
| 1.6 | **OP_RETURN diet** | fold ~8 per-return debug-gate branches into one loop-invariant `kRetSlowGate` + `noinline` cold extraction; guard the no-op `withStack.resize` | fib CPU −≥3 % / <1.5 % |
| 1.7 | **`OP_APPLY_OVERRIDES` chain guard** (correctness, from the RCA): `dst->lookup(k)` is chain-aware and `const_cast` writes in place (vm.cc:8440-8444) — a genuinely context-dependent write into a shared parent if dst is ever a Chain | regression test + guard; no perf bar |
| 1.8 | **Mark speedup** (pays off 1.1's backoff GC and workstream E): block-aligned mmap (align 16 MB) → block lookup = pointer mask + header, kills the per-edge `inActive`/index walk in `tryMark` (488 self samples) | mark ns/cell −≥60 % on the stats line / <30 % |

Wave-1 projection (laptop arithmetic, to be re-pinned on darwin-4): firefox ≈ 2.7-2.9 s
(~1.6×), hello ≈ 0.92-0.98 s (~1.3×), git ≈ ~1.55 s (~1.55×), foldl ≈ 1.40×, fib ≈ 1.30×.

**Decision point DP-1 (after Wave 1): re-profile firefox + hello v3 (same sample method),
re-pin the 7-row table on darwin-4, update the ratchet file.**

---

## 3. Wave 2 — the two structural levers (run in parallel, ~1-2 weeks)

### Workstream A — chain-SELECT lookup-without-materialize (⚠ drv-hash-critical)
The unified lever: removes most `materialize()` calls + their memo copies (CPU) and the
dominant share of Bindings allocation volume (RSS: merge+materialize ≈ 96/264 MB
hello/firefox). The 06-07 blocker is, with high confidence, the since-fixed C-1 family
(§0.4) — and the retry is **strictly safer than the original spike**:

1. **Tests first** (0.5 d): pin the 06-07 failure set (QG-1 list); synthetic shared-base
   sibling-chain suite incl. the C-1 shape (entry = `App(thunk,arg)` → under-applied PAP
   with interleaved sibling forces).
2. **Detector** (0.5-1 d): "is-chain-parent"/"multi-child" bits in `Bindings::_pad8`
   (alloc.hh:176), gated `V3_DBG_SHARED_WB=1`: log/abort on any writeback into a
   multi-child parent + provenance assert (pre-arm payload pointer must still be in the
   target at fire time — catches any C-1-class stale write at the write).
3. **L1 implementation** (1 d, gate `NIX_V3_CHAIN_LOOKUP_SELECT` + inline retirement
   criterion): chain SELECT walks layers; **leaf hit → existing KEEP protocol; parent hit
   → force with NO writeback armed** (thunk still self-memoizes; only slot-flattening is
   lost). IC: skip install/hit on chain operands (v1). Same at SELECT_DYN.
4. **Validation** (1-2 d wall): QG-1 full set + aggressive-GC byte-identity
   (`NIX_V3_MAJOR_GC_THRESHOLD_MB=8/16/64`) + fullsweep lookup-on vs off vs TW.
5. **L2** (parent-hit writeback, the TW-equivalent max-sharing form) only if L1 misses
   the bar, only behind the detector.

**Bar: firefox peak RSS −≥80 MB byte-identical, 0 sweep divergences (keep) /
corruption or −<40 MB (revert with data).** CPU expected positive (materialize removal);
report both. Rule-0 framing: a clean sweep kills "shared-parent memoization is inherently
unsafe in v3"; a detector-silent recurrence kills the C-1-explains-it hypothesis.

### Workstream B — resident primFoldl + tail-position OP_CALL_PRIMOP (foldl → ≤1.15×)
(b)-lite from the design analysis: convert `primFoldl` to a cursor state machine re-armed
by `OP_CALL_PRIMOP` (the `deepForceCursor` ip-rewind precedent, vm.cc:10913-11053) — kills
the per-element dispatchLoop re-entry (~45-65 ns of the ~80-120 ns gap). Prereq emit fix:
saturated statically-known PrimOpCall in tail/entry position must emit `OP_CALL_PRIMOP`
(today the gate shape goes through `callClosure`'s saturation path — `LIT_PRIMOP; OP_CALL_N`).
Cursor in a NEW CallFrame field (do NOT alias `deepForceCursor`). 2-4 days.
**Bar: foldl CPU −≥8 % / revert <4 %.** Risks: GC-stress soak (more mid-fold safepoints),
strict-arg-scan re-arm assert, run-pap suites.
**Escalation (pre-committed): if foldl still >1.15× after B + Wave 1, implement OP_FOLD
(2-3 d, bar ≥10 % / <5 %), reusing B's body-frame-push helper.**

**Decision point DP-2: darwin-4 re-pin. Expected: firefox ~1.5×, hello ~1.2-1.3×,
foldl ~1.0-1.15×, RSS firefox ≤ ~520 MB.**

---

## 4. Wave 3 — dispatch ceiling + RSS endgame (~2-3 weeks, gated by DP-2 data)

### Workstream C — computed-goto dispatch (78 cases, dual-mode macro)
After Waves 1-2 shrink per-element dispatch counts, dispatch share rises. Dual-mode
`CASE()/DISPATCH()` macros (`#if V3_COMPUTED_GOTO`) so the A/B is a compile flag.
2-4 days; honest expectation 5-15 % on fib/foldl (Apple-silicon indirect prediction
damps the classic win). **Bar: ≥5 % fib OR ≥4 % foldl / revert <3 % (delete the scaffold
on revert unless zero-cost — no carcasses).**

### Workstream D — context span-sharing (⚠) — fund per DP-1 profile
Immutable arena `ContextSpan` (u32 token-ids, process-wide intern with cached parsed
`NixStringContextElem`); copy = pointer share; merge = int-merge; absorb at drv = cached
elems. Subsumes 1.4. ~61 accessor refs / ~30 functional sites; 4-6 d.
**Fund only if the DP-1 profile still shows ≥3 % in context symbols** (post-1.4 the probe
share is gone; the copy share was ~2-4 % on firefox). **Bar: firefox CPU −≥2 %
byte-identical / revert <1 % or any divergence.**

### Workstream E — depth>0 GC + span reuse (the RSS structural backstop)
Composes with A (A cuts volume; E caps the high-water of what remains). The six prior GC
falsifications all ran at the single late safepoint — trigger placement is the new
variable. Staged, each with a kill bar:

- **Stage 0 (run NOW, ~1 d, 0 LoC)**: `NIX_V3_LIVE_TRACE_PERIODIC` at HEAD on hello +
  firefox → today's mid-eval L(t) + fully-dead-line fraction per sample; re-take the
  "+33 % at low threshold" anomaly with `NIX_VM_STATS` mark-split + defer counters.
  **Kill bar: mid-eval fully-dead lines <20 % at the trough, or live_mid >0.8×arena on
  firefox → the lever dies for ~1 day's cost** (and the same data tells workstream A
  which bytes are dead-churn vs live-retained — it pays for itself regardless).
- **Stage 1 (4-7 d)**: `gcPending` flag set in `refill()` (no per-opcode TLS), checked in
  ALL dispatch loops; drop the nested-VMState defer for the non-moving path; hard-disable
  evac/nursery at depth>0; **H-B audit: ~22 primop sites holding arena pointers in libc
  containers** (`genericClosure`'s `std::set<Value>` explicitly relies on the exitDepth
  gate — primops.cc:3210 comment) → GcRoot container registration; mark-based selective
  IC/matMemo eviction (evict only entries whose Bindings died — the mark knows);
  poison-on-sweep debug mode. **Kill bars: mid-eval dead lines <20 %; projected GC CPU
  >+15 % at a 64 MB window; any unexplained divergence post-audit.**
- **Stage 2 (3-5 d)**: production span reuse — unify `active_.cur/end` WITH the span
  cursor (refill pops spans before mmapping; fast path byte-identical to today), zeroing
  at rebuild time not per-alloc, stats gated. **Bar: firefox peak RSS −≥100 MB @ ≤+10 %
  CPU (≥50 MB wall-neutral also ships) / revert −<40 MB or >+15 % CPU.**
- **Stage 3 (1-2 d)**: default-on after fullsweep; retire `freeListBins_`,
  `V3_DBG_IMMIX_ALLOC`, recycle-pct gates per GC_DECISION's retirement clause.

Yield model (honest): firefox peak arena 480 → 430 (conservative) … 180 MB (optimistic);
hello possibly ~0 (window ≥ heap) — hello's RSS path is A + F + G, not E.

### Workstream F — `OP_ATTRS_UPDATE_N` n-ary merge (2-3 d)
Emit-time left-spine `(a // b) // c` collection → ONE merge pass/allocation (mirror of
TW's UpdateQueue, eval.cc:2180-2211). No aliasing risk (the refcount/in-place variant
stays rejected — `s_matMemo` staleness is the killer). **Bar: hello bindings-alloc volume
−≥15 % (NIX_V3_MEM_BUCKETS) at CPU ≤ +1 % / revert <8 %.**

### Workstream G — cache diet (the hello/attrNames RSS closer)
darwin-4 `NIX_V3_MEM_BUCKETS` decomposition FIRST (the darwin-4 firefox 1228 MB vs laptop
620 MB spread is unexplained — possibly cold caches/larger closure; decompose before
funding). Then: SQLite `PRAGMA cache_size` cap; `NIX_V3_IMPORT_CACHE_MAX_ENTRIES` sane
default (LRU exists, default 0 = never); per-CU IC vector trim (AttrSelectIC 64 B/site).
**Bar: hello RSS −≥30 MB, attrNames RSS ratio ≤1.1× / revert <15 MB.**

---

## 5. Honest end-state projection + the risk register

| Row | now (darwin-4) | after W1 | after W2 | after W3 | ≤1.0× confidence |
|---|---|---|---|---|---|
| attrNames CPU/RSS | 0.70× / 1.30× | 0.70 / 1.30 | 0.70 / 1.25 | 0.70 / **≤1.0** (G) | won / medium |
| fib CPU | 1.36× | ~1.30 | ~1.30 | **~1.05-1.15** (C) | medium — may need a follow-up call-path profile |
| foldl CPU/RSS | 1.45× / 0.5× | ~1.40 | **~1.0-1.15** (B) | ≤1.0 (C or OP_FOLD) | high |
| hello CPU/RSS | 1.65× / 1.90× | ~1.3 / 1.9 | ~1.2 / ~1.5 (A) | **~1.0-1.1 / ~1.1-1.3** (C+F+G) | medium / **lowest** — RSS floor = caches + arena base; itemize at DP-2 |
| git CPU/RSS | 1.88× / 1.95× | ~1.55 / 1.9 | ~1.35 / ~1.5 | ~1.1-1.2 / ~1.1 | medium |
| firefox CPU/RSS | 2.78× / 2.56× | **~1.6** / ~2.4 | ~1.4-1.5 / **~2.0** (A) | **~1.15-1.3 / ~1.0-1.4** (C+D+E) | medium / medium |

Residual risks, named: (i) firefox CPU below ~1.3× depends on dispatch-ceiling work (C)
plus whatever DP-1's re-profile surfaces next — the long tail (malloc churn 6 %, force
machinery, chain-lookup amplification) has no single big lever left; (ii) hello RSS ≤1.0×
is the hardest cell (TW's 126/136 MB floor is small; v3 carries SQLite + CU cache + arena
granularity) — if DP-2 shows the floor >136 MB after A+F+G, the honest options are a
`NIX_V3_NO_DISK_CACHE` eval-mode default for small evals or accepting >1.0× on that cell
and saying so; (iii) workstream A could still corrupt — that is what the detector +
tests-first ordering is FOR (one-run falsification instead of a drvPath-diff hunt);
(iv) E could die at stage 0 — by design, for one day's cost.

**Do-not-repropose list (cumulative, with falsification evidence):** wrapper-retire
(infinite-recurses), GC-threshold-lowering-alone (gc_count=1; this plan's 1.1 RAISES it),
Immix-flip-as-gated (starved, now resolved via E-stage-2 properly), OPCYCLES-TLS gating
(0 %), key de-stringify (0 %), context-parse memo alone (2.4 %), context sort-skip (wash),
GET+FORCE fusion-miss (stale — already fused), wrapper-skeleton-bound diagnosis (profile:
≤0.5 %), single-pass drv hashing as a RATIO lever (shared with TW — ratio-negative;
absolute-CPU lever only), `//`-refcount in-place mutation (s_matMemo staleness),
arity-cache byte, OP_LESS, reuseScope-TLS-skip, ValuePair 24/32 split, string-value dedup,
Boehm tuning.

---

## 6. Execution order (one line)

Wave 1 (1.1-1.8 + E-stage-0, ~1 wk) → DP-1 re-profile/re-pin → Wave 2 (A ∥ B, 1-2 wk)
→ DP-2 re-pin → Wave 3 (C, then D-if-funded, E-stages-1-3, F, G, 2-3 wk) → final 7-row
re-pin + ratchet freeze. Every item: own commit, A/B in the body, hypothesis named,
bars pre-committed above.

---

## 7. Addendum (2026-06-13, post-guard-A/B): WHY M5 occupies ~2.2-2.7 GB — measured decomposition

Laptop run, byte-identical (`cardano-node-exe-cardano-node-10.6.1`): v3 27.17 s / 2218 MB
(default 256 MB policy, 2 GC fires) vs TW 8.67 s / 858 MB. Final stats dump (tail -1):

```
v3_arena=2147.5MB  elsewhere=0.0MB  boehm_free=402.7/402.9MB   ← it is ALL eval heap
run-phase alloc: thunks=928.0MB  bindings=469.9MB  pairs=253.5MB  closures=183.8MB
                 chars=137.5MB  lists=72.4MB        (total 2130.7MB)
thunks allocated=17,140,931  forced=6,302,774  → 63.2% NEVER FORCED (avg 56.8B/thunk)
mergeBindings: 766,742 calls but only 172.1MB     ← `//` materialization is NOT the M5 story
bridge=0                                          ← the old "731MB bridge retention" is obsolete (F4)
GC#1: live=313.5MB  freed=16.8MB (1 block)        GC#2: live=1049.7MB  freed=0.0MB
GC total: 11.8s of 27.2s CPU (~43%) for 16.8MB reclaimed
```

**Answer**: M5 is a *genuinely live* giant, not churn and not caches. `getFlake` retains
the whole flake-outputs tree while haskell.nix's cabalProject fixpoint builds enormous
package/component attrsets whose values are **~10.8 M never-forced suspended thunks** —
the GC correctly keeps them (live 1.05 GB mid-eval and growing). The v3-vs-TW RSS gap
(2147 vs ~858 MB) is dominated by the **representation tax on suspended computation**:
v3 flat-captures upvalues per thunk (40 B header + 8 B/upvalue ≈ 57 B avg × 17.1 M =
928 MB), where TW's equivalent thunk is a 16 B Value pointing at a shared AST + a
**shared Env chain** (sibling thunks in one binding group share one Env spine, ~16-24 B
amortized). Pairs (253 MB of App partial-applications) add the same class of ballast.

**Consequences for the plan**:
- The team's adaptive backoff (freed<5% → throttle) is exactly right for M5; HNE's
  useful GC (−624 MB, whole blocks die) is preserved by the yield key. 1.8 (mark
  speedup) turns M5's residual mandatory mark from ~5 s toward ~0.5-1 s.
- Workstream A (chain-SELECT) helps M5 only marginally (merge volume is 172 MB / 8%
  of arena). Workstream E helps M5 little (live-dominated heap).
- **NEW candidate workstream H — shared capture frames for sibling thunks** (the M5/HNE-
  class lever): one upvalue tuple per binding group, sibling thunks reference it (8 B)
  instead of flat-copying captures — re-introduces TW's env sharing exactly where it
  wins (huge never-forced thunk populations) while keeping flat capture on the hot
  forced path. Pre-design measurement first: histogram captured-upvalue duplication
  across sibling thunks (cheap NIX_V3_*_ATTR-style probe). Potential ≈ 300-500 MB on
  M5-class; needs-measurement. Sequence after A/B/E per measure-twice.
- v3 M5 CPU without the GC tax ≈ 15.4 s ≈ 1.78× TW — the same base gap as the other
  drv rows; the Wave-1 CPU items apply to M5 unchanged.

*Measurements 2026-06-13 by the orchestrating session (profiles in /tmp are transient;
all load-bearing numbers are reproduced inline above). Agent design analyses: chain-RCA,
resident-iteration, depth>0-GC, drv-CPU-pack (4 parallel deep reads at HEAD 777d286c7).*
*Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.
SPDX-License-Identifier: Apache-2.0.*
