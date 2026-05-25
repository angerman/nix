# Memory Reduction Opportunities — 2026-05-23

Five-agent codebase review of opportunities to reduce v3 VM live memory
usage, prompted by the codification of memory-as-first-class-target
(`feedback_memory_first_class.md`, `ROADMAP_TO_VISION` §"Optimization
targets"). Companion to `PERF_AUDIT_2026-05-23.md` (which covered both
wall + memory; this doc is memory-focused).

**Headline:** the team has demonstrated memory wins compound faster
than wall wins (75 % peak-RSS reduction on hello.drvPath in 4 days).
This audit identifies ~150-300 MB of additional reachable reduction on
hello.drvPath, split into three tiers by risk. **Tier A is 3-4 days of
measurement spikes** that gate everything else. The team has been
disciplined about measure-twice-cut-once; memory work should follow
the same protocol.

## §1 — Current state recap

```
hello.drvPath peak RSS trajectory:
  Pre-2026-05-19    ~4.3 GB     per #702
  After #748+#750   ~1.9 GB     -386 MB primIntersect/Remove two-pass
  After #752        ~1.08 GB    -849 MB PosIdx32 inline (-44 % peak)
  Today (post-#783) ~1.08 GB    (no published RSS spike post-#783;
                                 wall ratio v3:TW = 1.41×)

cardano-node M5 peak RSS:
  v3 + TW-callFlake bridge   919 MB
  TW reference                898 MB
  ratio                       1.02× (essentially at parity)
  v3-native callFlake (post #758): NOT YET MEASURED — gap

Per-category bytes on hello.drvPath (per #702 / #719 three-way RSS):
  bytesBindings:  ~52 % of v3 arena (dominant)
  bytesThunks:    ~31 %
  bytesClosures:  smaller; ~11 MB header at 350 K closures
  "elsewhere":    significant; CompilationUnit + capacity slack +
                  fiber stack + SQLite + Boehm metadata
```

## §2 — Confirmed bugs (land today, no measurement needed)

### §2.1 `shapeCell` 8 B field unconditionally occupied (Thunk audit C3)

`Thunk::shapeCell` (`closure.hh:148`) is 8 bytes per Thunk that holds a
pointer ONLY when `NIX_V3_CELL_EVERYWHERE=1`. The default is OFF, so
on every Thunk this field is dead weight 8 B.

- **File:line:** `closure.hh:148` (field), `alloc.hh:558` (gated alloc)
- **Cost when gate off:** 8 B × ~1.6 M Thunks = **~12.8 MB unconditional**
- **Fix:** `#ifdef NIX_V3_CELL_EVERYWHERE` around the field declaration
  + a conditional accessor. Or move it to a sparse side-table keyed by
  Thunk* (rare-path access).
- **Risk:** Low. The gate already exists; only the struct layout is
  affected.
- **Falsifier:** `sizeof(Thunk)` drops 8 B when gate compiled-out;
  bytesThunks counter drops proportionally on hello.drvPath.
- **Effort:** 0.5 day.

### §2.2 Phase D `cellContainer` field overhead validation

The Thunk audit identified `Thunk::cellContainer` (8 B, Phase D
metadata) as part of the per-Thunk overhead. This is **load-bearing
for Phase D barriers**, NOT a bug. Documented for completeness; no
action.

## §3 — Tier A: Measurement spikes (3-4 days, GATES everything else)

Per measure-twice-cut-once: most of the optimisations below depend on
data the team doesn't have yet. These spikes produce the data; without
them, choices are speculative.

The 5 agents proposed ~10 distinct spikes. They collapse to 4
non-overlapping measurement passes:

### §3.1 Per-category byte breakdown (1 day)

**What:** Run `NIX_VM_STATS=1` on hello.drvPath + cardano-node M5 and
publish the per-allocator byte breakdown that already exists at
`alloc.hh:238-245` (`bytesValues`, `bytesClosures`, `bytesThunks`,
`bytesEnvs`, `bytesLists`, `bytesBindings`, `bytesPairs`,
`bytesChars`).

**Plus:** dump `attrsetSizeBuckets[10]` histogram per
`alloc.hh:191`.

**Output:** authoritative table of "which allocator owns the 1.08 GB."
Subsumes PERF_AUDIT T3.2 + most agents' opening question.

### §3.2 Null-fraction counters for Closure / Thunk pointers (1 day)

**What:** Add `V3_DBG_CU_NULL` and `V3_DBG_WITHS_NULL` counters at
OP_MAKE_CLOSURE / OP_MAKE_THUNK sites. Increment based on whether
`cu` / `capturedWiths` are null.

**Why:** Per DATA_STRUCTURE_AUDIT_2026-05-21 B1/B2: claim is ~90 %
null. **Unverified.** Side-table extraction depends on this number.

**Decision rule:** if null fraction ≥ 80 % on both workloads, the
side-table win is real (~10-20 MB combined for Closure + Thunk). If <
60 %, kill the side-table proposal.

### §3.3 "Elsewhere" memory contributor census (1 day)

**What:** Extend the `run.cc:665-751` elsewhere-probe with explicit
sub-categories:
- CompilationUnit bytecode storage (sum across all CUs of `code` +
  `lambdas` + literal pools)
- std::vector capacity slack (capacity − size, for
  `posSnapshotPool` / `globalSymbolTable` / `dirtyContainers` /
  `cuRegistry`)
- Fiber stack count + max depth (`fiber.hh:87` kDefaultFiberStack
  is 16 MiB; if only 1 fiber, this is a sizing decision)
- SQLite cache footprint (`sqlite3_status`)
- Boehm internal metadata estimate (`GC_get_stats` cross-checked
  against heap-size)

**Why:** Per the "elsewhere" agent: this is the unaccounted-for ~600-
800 MB of v3's hello.drvPath RSS that's neither in the per-category
counters NOR in Boehm. Without this breakdown, "elsewhere" is a black
box that compounds invisibly.

**Decision rule:** the breakdown drives the next 3-4 Tier B items;
"elsewhere" remains the unknown floor on memory until measured.

### §3.4 Common-value frequency tracer (1 day)

**What:** A single instrumentation pass that emits:
- Top-10 int values by `mkInt()` count
- Top-20 string values by `allocChars()` count + total bytes
- Top-N path-prefix patterns by `Tag::Path` allocation
- Top-N CU source-SHAs by `primImport` count (informs CU dedup)
- Top-N attrset shapes (sorted-symbol vectors) by Bindings
  allocation count

**Why:** Multiple agents proposed sharing/interning optimisations
(small-int pool, common-string pool, path-prefix pool, CU dedup,
shape interning) — all gated on whether the top-K is concentrated
enough to pay back.

**Decision rule:** each sharing candidate has a "top-K accounts for ≥
N %" threshold pre-committed. Below threshold = kill.

### §3.5 ExprAttrs / ExprLet dedup re-measurement (2 days)

**What:** Extend `dedup_survey.cc` (the Stage 9 #772 spike file) to
hash at whole-ExprAttrs / ExprLet body granularity instead of per-
MkThunk. Re-run on hello.drvPath + cardano-node M5.

**Why:** Stage 9 was killed at thunk-body granularity (1.17× function
/ 1.03× byte dedup, both below 2× threshold). The kill criterion
explicitly leaves the coarser-granularity revival path open (per
`STAGE_9_KILLED_2026-05-22.md` §7.5.1).

**Decision rule:**
- Byte-dedup ≥ 2× at coarser granularity → revive Stage 9 at
  ExprAttrs/ExprLet granularity (200-300 MB potential).
- 1.5-2× → marginal; single-day feasibility spike before commit.
- < 1.5× → kill confirmed at all granularities.

## §4 — Tier B: Cheap wins post-measurement (1-3 days each)

These items have small enough scope to qualify for the
measure-twice-cut-once §3.8 variant (implement, measure, revert if
threshold not met).

### §4.1 Fiber stack size reduction (1 hour, GATED on §3.3)

**Current:** `kDefaultFiberStack = 16 MiB` per `fiber.hh:87`.

**If §3.3 census shows hello.drvPath uses 1 fiber and max stack depth
< 2 MiB:** reduce to 4 MiB. **−12 MiB baseline** per fiber.

**Falsifier:** if any benchmark workload max stack depth ≥ 4 MiB,
revert.

### §4.2 SQLite cache_size tuning (30 min, GATED on §3.3)

**Current:** SQLite default `cache_size = 2000 pages` (~8 MB).

**If §3.3 census shows SQLite holds 5-20 MB:** add `PRAGMA cache_size
= 500` (~2 MB). **−6 MB**.

**Falsifier:** A/B wall-clock; if cache_size=500 causes ≥ 2 % wall
slowdown on hello.drvPath, revert.

### §4.3 `shrink_to_fit` on scavenge boundaries (1 day, GATED on §3.3)

**If §3.3 census shows ≥ 50 MB cumulative vector/map capacity slack:**
call `shrink_to_fit()` on `dirtyContainers`, `globalSymbolTable`,
`posSnapshotPool`, `cuRegistry` at end of each scavenge or at process
mid-eval checkpoints.

**Estimated savings:** **10-30 MB** on hello.drvPath.

**Falsifier:** A/B wall-clock; if shrink-to-fit causes regrowth that
nets > 0 cycles, revert.

### §4.4 `snapshotCurrentWiths` elision via lowerer (2-3 days)

**Per PERF_AUDIT_2026-05-23 T2.3:** lowerer emits with-targets
explicitly so the runtime snapshot is rare. **~30-50 MB saved** on
hello.drvPath (500 K-1 M ListVec allocations per eval, 48-64 B each).

**Implementation:** `lower.cc` tracks active with-scopes at OP_MAKE_*
emission sites; populates `nWithTargets > 0` with explicit values
instead of letting runtime call `snapshotCurrentWiths`.

**Falsifier:** `V3_DBG_ALLOC_DUMP` `allocList` count drops ≥ 50 %
on hello.drvPath. Wall delta ≥ 3 ms (per PERF_AUDIT T2.3 projection).

### §4.5 ValuePair PrimOpApp split (32 B vs 48 B) (1-2 days)

**Per DATA_STRUCTURE_AUDIT C1:** PrimOpApp instances don't use the
`evaluated` field (16 B wasted). Split into `AppPair {48 B}` and
`PrimOpPair {32 B}` variants.

**Estimated savings:** **~12.8 MB** if ~50 % of ValuePair instances
are PrimOpApp (frequency from §3.1 byte breakdown).

**Falsifier:** `bytesPairs` drops proportionally to PrimOpApp share
on hello.drvPath.

### §4.6 Single-KV Bindings sentinel (3-5 days, GATED on §3.1)

**Per `attrsetSizeBuckets[1]` data:** singletons are ~12 % of attrset
allocations. If §3.1 confirms a high fraction of singletons have
identical (symbol, value-type) combinations (e.g.,
`nameValuePair`-style results), a per-symbol-name sentinel pool
becomes attractive.

**Estimated savings:** **~5-15 MB** on hello.drvPath (per Tvix-style
Empty/KV/Map specialisation cited in OPTIMIZATION_STRATEGIES §4.4).

**Constraint:** must respect Tag::Slot pointer-stability — singletons
must be arena-tenured (not pool-recycled).

**Falsifier:** sentinel hit rate ≥ 50 % on hello.drvPath; bytesBindings
drops by ≥ 5 MB.

## §5 — Tier C: Medium wins (1-2 weeks each, larger payoff)

These items have higher cost but larger potential. **Each is gated on
Tier A measurement** showing the underlying assumption holds.

### §5.1 Closure::cu + capturedWiths side-table (2-3 days, GATED on §3.2)

**Per DATA_STRUCTURE_AUDIT B1/B2:** if §3.2 confirms ≥ 80 % null
fraction, move these 8 B pointers to a sparse side-table keyed by
Closure* / Thunk*.

**Estimated savings:** **~10-20 MB combined** at 350 K Closures + 1.6
M Thunks.

**Falsifier:** struct sizes drop 16 B each; dispatch loop still hot
(side-table lookup is rare path).

### §5.2 In-memory CompilationUnit dedup (2-3 days, GATED on §3.4)

**Per Sharing audit A7:** if §3.4 shows < 50 unique source-SHAs across
269 imports on hello.drvPath (i.e., 5×+ reuse), an in-memory `CU*`
cache keyed by source SHA saves redundant deserialization +
allocation.

**Estimated savings:** **5-50 MB** depending on overlap.

**Falsifier:** unique-SHA count gates this; below 50 = ship; above
200 = kill.

### §5.3 Bindings append-overlay structural sharing (2 weeks, MULTI-GATED)

**Per DATA_STRUCTURE_AUDIT B4 + Bindings audit B1:** the biggest
potential single lever — **100-300 MB on OP_UPDATE-heavy workloads**.

**Gates:**
1. Phase D pointer-stability audit (currently default-on, but the
   append-overlay shape needs explicit validation that Tag::Slot
   pointers remain valid across overlays).
2. §3.5 ExprAttrs dedup measurement — if coarse-granularity dedup
   shows ≥ 2× ratio, append-overlay is doubly justified.

**Implementation:** `MergedBindings { Bindings * base; Bindings *
overlay; }` with two-array lookup; flatten only when iterated.

**Falsifier:** OP_UPDATE chain depth histogram on cardano-node M5; if
average depth < 4, the benefit doesn't justify the complexity.

### §5.4 Small-int / common-string pools (3-7 days combined, GATED on §3.4)

**Per Sharing audit C1+C2:** if §3.4 shows top-10 ints account for ≥
10 % of int allocs, OR top-20 strings account for ≥ 5 % of string
bytes, pools become attractive.

**Estimated savings:**
- Small-int pool (0..255): ~4 KB pool size, ~1-5 MB saved.
- Common-string pool (license names, "out"/"dev"/"lib"/etc.):
  ~5-20 MB saved.

**Constraint:** pooled objects must NOT be reused/recycled (Tag::Slot
pointer-stability); pool entries are immortal.

**Falsifier:** allocCount drops proportionally; bytesValues +
bytesChars drop.

### §5.5 LambdaCore / LambdaMeta split (1 week, post-Stage-9 reconsideration)

**Per DATA_STRUCTURE_AUDIT B6:** ~152 B per LambdaDescriptor, spanning
2-3 cache lines. Cold fields (name, contextualName, instrumentation
counters, astLambda) split into a side-table.

**Estimated savings:** **~5 MB at 50 K descriptors** AND cache-line
density improvement on the hot OP_CALL path.

**Status:** PARTLY a cache optimisation, not just memory. Defer
until Tier A spike confirms LambdaDescriptor descriptor count and
whether the hot path is actually cache-bound.

## §6 — Tier D: Architectural items (deferred)

These require larger commitments and don't fit the cheap-spike model.

- **Lazy-list / lazy-attrset Tag variants** — per Lazy materialization
  agent C: pointer-stability risk is high; speculative benefit
  depends on consumer access patterns. **Gate on Phase D feasibility
  decision; defer indefinitely.**
- **Cross-process bytecode mmap sharing** — per "elsewhere" agent E:
  multi-week effort; benefits only multi-process deployments (CI
  farms). Defer to post-perf-closure work.
- **Stage 9 revival at coarser granularity** — IF §3.5 measurement
  shows ≥ 2× byte-dedup at ExprAttrs/ExprLet level, this becomes a
  serious item (2-4 weeks). Otherwise stay dead.

## §7 — Negative findings (do not pursue)

| Direction | Verdict | Source |
|---|---|---|
| Per-thunk-body content dedup | KILLED Stage 9 (#772) | dedup_survey 1.17×/1.03× < 2× threshold |
| Lazy-list Tag variant for `primMap` results | NEGATIVE | Lazy agent T5: ≥ 80 % consumer access via elemAt-within-N-ops; lazy is anti-optimisation |
| Path-prefix pointer pool | BLOCKED | Sharing audit A10: breaks Tag::Slot pointer-stability invariant; paths used as dict keys |
| Closure hash-cons by (desc, upvalues) tuple | LOW EXPECTED WIN | Sharing audit D1: `cachedSingletonClosure` already covers no-upvalue case; non-trivial extension adds ~5 MB at best |
| Bindings HAMT migration | BLOCKED + LOW HOT-PATH | Per OPTIMIZATION_STRATEGIES §5.3 + Bindings audit: iteration is not v3's hot path, HAMT insert is up to 28 % slower |
| Skip-language / Adapton-style whole-graph memoisation | UNTRIED → KILL | Per OPTIMIZATION_STRATEGIES §6: family-wide kill from prior research |
| Lazy frame-locals (no zero-fill + bitmap) | TOO RISKY | Lazy agent T3: GC-walk cost likely exceeds allocation savings; needs careful study |
| Lazy `OP_ATTRS_REC_INIT` uninit entries | TOO RISKY | Lazy agent T2: consumption rate ~95 %, lazy is anti-optimisation; Tag::Slot pointer-stability risk |

## §8 — Recommended sequence (Rule 0 disciplined)

```
DAY 1-4  ──── Tier A measurement spikes
  Day 1   §3.1 per-category byte breakdown    (1 day)
  Day 2   §3.2 null-fraction counters         (1 day)
  Day 3   §3.3 elsewhere census              (1 day)
  Day 4   §3.4 common-value tracer           (1 day)

  GATE: publish measurement table. Tier B items below
        are now data-driven, not speculative.

DAY 5    ──── Tier 2 bugs (immediate)
  Day 5   §2.1 shapeCell #ifdef               (0.5 day)
                                                ~12.8 MB
                                              
DAY 6-12 ──── Tier B cheap wins (data-driven)
  Day 6   §4.1 fiber stack reduction          (1 h)
  Day 6   §4.2 SQLite cache_size              (30 min)
  Day 6   §4.3 shrink_to_fit boundaries       (1 day)
                                                ~30-50 MB combined
  Day 7-9 §4.4 snapshotCurrentWiths elision   (2-3 days)
                                                ~30-50 MB
  Day 10  §4.5 ValuePair split                (1-2 days)
                                                ~12.8 MB
  Day 11-12 §4.6 Single-KV sentinel
                  (only if §3.1 supports)      (3-5 days)
                                                ~5-15 MB

  CUMULATIVE TIER B: ~90-140 MB reduction expected

DAY 13+  ──── Tier C medium wins (data-driven)
  Run §3.5 ExprAttrs dedup spike              (2 days)
  Decide on §5.3 append-overlay vs §5.2 CU dedup
  §5.1 cu/capturedWiths side-tables based on §3.2

  CUMULATIVE TIER C: ~100-300 MB additional reduction
                     (heavily dependent on dedup measurement)

LATER    ──── Tier D architectural items
  Only after Tier A+B+C land and the team has new
  measurements that justify the larger commitments.
```

**Total realistic 2-3 week target:** ~150-300 MB additional reduction
on hello.drvPath, bringing peak RSS toward 0.8-0.9 GB territory —
within ~2× of TW.

Combined with the 4-day work the team already shipped (4.3 GB →
1.08 GB), the post-Tier-B/C state would be ~0.8 GB vs TW's ~0.4-0.5
GB. **The wall-clock target of ~1.4× TW combined with ~1.5-2× memory
becomes a credible end-state** even without the architectural
register-VM rewrite that #780 falsified.

## §9 — Honest limits

1. **Tier A spike data may falsify Tier B/C estimates.** The 90-140
   MB Tier B projection assumes the underlying assumptions hold (fiber
   count = 1, ≥ 50 MB vector slack, ~50 % PrimOpApp share, etc.).
   Honest: if §3.1 shows the breakdown is totally different from
   estimates, Tier B shrinks.

2. **The 2-3× TW memory ratio on hello.drvPath has a structural
   floor.** v3's arena allocations are real work (Bindings entries,
   Thunks, Closures). TW doesn't allocate the same shape because
   it's a tree-walker. Some of the gap is genuine architectural
   cost of bytecode-VM-with-FFI. The reachable target is probably
   ~1.5× TW memory, not 1.0×.

3. **Stage 9 §7.5.1 revival is real but speculative.** The coarser-
   granularity dedup MIGHT show 2×+ — Stage 9's original kill
   criterion explicitly leaves this door open. But the bytecode-
   level result was 1.03-1.07× byte dedup; alpha-equivalent
   IR-level hashing only changes function COUNT, not byte
   storage. Don't pre-commit to Stage 9 revival until §3.5
   measurement is in hand.

4. **Cross-eval matrix is missing** — there's no recently-published
   side-by-side hello.drvPath RSS table for v3 vs TW. PERF_AUDIT
   T3.2 + §3.1 should produce this; treat any specific "X MB
   saved" claim before then as estimate, not measurement.

5. **The `OPTIMIZATION_STRATEGIES_2026-05-23.md` headline strategy
   (broaden ICs)** is wall-focused, not memory. Tier 1 of that doc
   composes with Tier B+C of THIS doc — they target different
   factors. Don't double-count.

## §10 — Cross-references

- `feedback_memory_first_class.md` — the codified rule this doc operationalises
- `ROADMAP_TO_VISION_2026-05-15.md` §"Optimization targets" — the rule's roadmap entry
- `PERF_AUDIT_2026-05-23.md` — sibling audit; T3.2 measurement spike subsumed by §3.1+§3.3 here
- `DATA_STRUCTURE_AUDIT_2026-05-21.md` — the substrate for many findings; B1/B2/B4/B6 are referenced
- `OPTIMIZATION_STRATEGIES_2026-05-23.md` — wall-focused; this doc is memory-focused; both compose
- `STAGE_9_KILLED_2026-05-22.md` §7.5.1 — Stage 9 coarser-granularity revival path
- `MEASURE_TWICE_CUT_ONCE_2026-05-23.md` — every recommendation here has a falsifier
- `NURSERY_PHASE_D_DESIGN_2026-05-18.md` — Tag::Slot pointer-stability constraint dominates §5.3
- Recent measurement landings: #748 (-14.6 MB), #750 (-386 MB), #752 (-849 MB), #781b (sparse symbolTable)

---

Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
Input Output Group. SPDX-License-Identifier: Apache-2.0.
