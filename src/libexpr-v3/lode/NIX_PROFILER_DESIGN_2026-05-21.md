# Per-line CPU + Memory Profiler for Nix Evaluation — MVP Design

**Date**: 2026-05-21.
**Goal**: A tool that gives Xcode/Instruments-style **per-line / per-token source
attribution** of CPU time and memory allocation during Nix evaluation, suitable
for large flakes like `github:IntersectMBO/cardano-node`. Output is visually
consumable in a browser — flame graph + source-annotated heatmap — with no
external server, share-by-file, and a path to share-by-URL.
**Method**: Synthesis of four parallel research turns: existing v3 infra
inventory, output-format/viewer survey, sampling-strategy design, visualization
UX design.

## 0. Headline

The infrastructure already in v3 covers ~70% of the data layer. The missing
~30% is a small, well-scoped addition (~5 days of focused work). The
*visualization layer* is best handled by piggy-backing on two existing
off-the-shelf web viewers — `go tool pprof -http` for the source-annotated
heatmap and `profiler.firefox.com` for the polished flame-graph + memory
track — and adding a thin custom HTML wrapper later if a unified single-file
report becomes valuable.

**MVP scope**: emit one JSONL trace from v3, two file-format exporters
(pprof + Firefox Profiler JSON), one Python post-processor. ~8 days total.

**Phase 2 (optional)**: custom self-contained HTML report (canvas flame + dual-
gutter source view, Instruments-style). +~2 weeks.

## 1. What we already have (and what's missing)

Confirmed by inventory of `src/libexpr-v3/` + `src/libexpr/`:

### Already in v3 (data layer, ~70% done)

| Component | File:line | What it captures |
|---|---|---|
| `posSnapshotPool` | `alloc.hh:1162` | Process-wide `(file, line, col)` table; 1-based handle |
| `LambdaDescriptor::posHandle` | `closure.hh:242` | Body position per lambda; populated by lower.cc |
| `LambdaDescriptor::Formal::pos` | `closure.hh:218-223` | Per-formal position |
| `attrPosTable` | `alloc.hh:847` | `(Bindings*, SymbolId) → pos` (per-attr) |
| `LambdaDescriptor::allocCount` / `forceCount` / `callCount` | `closure.hh:249/266/273` | Per-descriptor counts |
| `Thunk::forces` | `closure.hh:112` | Per-instance force count |
| `AllocStats.bytes*` | `alloc.hh:218-225` | Global byte totals by category |
| `V3_DBG_ALLOC_DUMP` | `vm.cc:191-262, 3228-3306` | Top-N descriptors atexit + every 2M MAKE_THUNK |
| `dumpHotDescriptors` | `primops.cc:8564-8609` | Top-N by forceCount, all CUs |
| `NIX_TRACE_EVAL` | `libutil/include/nix/util/eval-trace.hh` | F/W/B/M event stream with positions, time-ordered |
| `V3_DBG_HOT_FORCE` | `vm.cc:1923-1989` | Suffix-match `file:line:col` total + unique-thunks |
| `bench/perf-trace.py` | `bench/perf-trace.py` | External CPU%/RSS/Boehm-heap time-series sampler |
| TW `SampleStack` | `src/libexpr/eval-profiler.cc:121-181` | Pre/postFunctionCall hooks → folded-stack format |
| TW `Statistics::functionAllocs/Calls/attrSelects` + `ifdEvents` | branch `angerman/2.34-ifd-profiling` | Per-ExprLambda alloc breakdown + IFD events |

### Missing (the ~30% we need to add)

| Gap | Why it matters | Effort |
|---|---|---|
| Per-IP → source-position map (`cu.codePosMap`) | Without this, samples can only resolve to lambda-body granularity. We want per-line. | 1.5 days |
| Per-IR-binding position propagation (`ir::Binding::pos`) | Lower.cc only stores positions on function entries; need per-expression to feed `codePosMap`. | 1 day |
| Safe-point CPU sampler (counter-based, hybrid with 50 ms wall-clock thread) | Currently no time-series CPU attribution to Nix source. | 1 day |
| Per-position allocation byte counters (aggregated per-poll, not per-alloc) | Currently bytes are global only; we need bytes-by-source-position. | 1 day |
| Per-frame call-site resolution (resolve caller `ip - 2` via `codePosMap`) | For inverse flame graph (who calls X). | 0.5 day |
| Trace output (JSONL with samples + per-position cumulative counters) | Intermediate format that exports cleanly to pprof/Firefox/Speedscope. | 0.5 day |
| pprof exporter | Primary source-heatmap UI via `go tool pprof -http`. | 1 day |
| Firefox Profiler JSON exporter | Primary flame-graph + memory-track UX via `profiler.firefox.com`. | 1 day |
| Documentation | `USAGE.md` entry + a `bench/prof.py` driver script. | 0.5 day |
| **Total MVP** | | **~8 days** |

## 2. Architecture

```
  +----------------------------+
  | v3 dispatch loop (vm.cc)   |
  |  - safe-point poll at      |
  |    every 10k opcodes       |
  |  - calls profile::maybeRecordSample()
  |  - calls profile::attributeAllocPoll() |
  +-------------+--------------+
                |
                v
  +----------------------------+      +-----------------------+
  | profile.cc                 |  <-- | sampler std::thread   |
  |  - per-position counters   |      | 50 ms wall-clock      |
  |  - sample ring buffer      |      | bumps samplesPending  |
  |  - codePosMap lookup       |      +-----------------------+
  |  - emit JSONL on stopProfile() |
  +-------------+--------------+
                |
                v
        profile.jsonl  (intermediate format)
                |
        +-------+--------+--------+
        v       v        v        v
     pprof   FFprof   Speedscope  CSV
      .pb    .json    .json      .csv
        |       |        |         |
        v       v        v         v
   go tool   profiler  speedscope  pandas/
   pprof -http  .firefox  .app    duckdb
                .com
```

### 2.1 Data layer

**`CompilationUnit::codePosMap`** (`bytecode.hh`, new field):
```cpp
struct CompilationUnit {
    ...
    // Sorted ascending by .first; binary-search for any ip → position.
    // Append-only at emit time; one entry per change-of-position.
    std::vector<std::pair<uint32_t /*startIp*/, uint32_t /*posHandle*/>> codePosMap;
};
```
~5× compression vs full per-IP array; ~10 MB on cardano-node-scale CUs.
Built in `emit.cc::emitOne()` by calling `recordPos(currentPosHandle)` —
mirrors the existing `recordForceSite` pattern at `emit.cc:339`.

Lookup at sample time: `std::lower_bound` over `codePosMap` for the given `ip`,
return the largest `startIp <= ip`. O(log N), ~50 ns per lookup.

**Per-IR-Binding position** (`ir.hh`, new field):
```cpp
struct Binding { ... uint32_t pos = 0; };  // posSnapshotPool handle
```
Populated in `lower.cc::addBinding()`-style central helpers via the existing
`posIdxToHandle(e->getPos())` call. Already done for function entries; just
need to extend the same mechanism to every IR binding.

### 2.2 Sampler

**Safe-point sampler** triggered at the existing `vm.cc:2307` poll point
(every `kPollInterval == 10000` opcodes). The poll already runs; we add one
branch:

```cpp
if (__builtin_expect(nix::v3::profileActive(), 0)) [[unlikely]] {
    if (nix::v3::profileSamplesPending() > 0)
        nix::v3::profileRecordSample(vm, cu, ip);
    nix::v3::profileAttributeAllocPoll(currentPosHandle(*cu, ip));
}
```

**Wall-clock sampler thread** (modelled on `heap_trace.cc`): a detached
`std::thread` ticks every 50 ms (gives 20 Hz, configurable up to 1 kHz via
`NIX_V3_PROFILE_RATE_HZ`); each tick atomically increments
`samplesPending`. The thread itself never reads `vm.frames` — it just signals.
This decouples timing precision (sampler thread) from snapshot safety (poll).

**Rationale**: SIGPROF + signal handler would race with `vm.frames` reallocation
and require async-signal-safe code (no `std::vector::back()`). Counter-only
opcode-stride sampling would underweight slow primops. The hybrid is both safe
and unbiased.

### 2.3 Per-sample snapshot

```cpp
struct Sample {
    uint64_t  t_us;            // microseconds since profile start
    uint64_t  alloc_bytes_delta;   // bytes allocated since last sample
    uint64_t  boehm_heap;          // GC_get_heap_size()
    uint64_t  rss;                 // currentRssBytes() (limits.cc:85)
    uint8_t   kind;                // 0 = wall-clock; 1 = mem-pressure; 2 = forced
    std::vector<uint32_t> stack;   // bottom-up posHandles (length = vm.frames.size())
};
```

Stack construction:
- For each `f` in `vm.frames` bottom-up:
  - Body position: `f.closure->desc->posHandle` (or `f.thunk->suspended.desc->posHandle`).
  - Call-site position: caller's `ip - 2` → `codePosMap` lookup.
- Walk `tlActiveVMStack` (vm.cc:2050) for cross-`runRootExpr` re-entries
  (e.g., `import`) — concatenate stacks.

Average ~80–200 bytes per sample. 30-second eval at 20 Hz = 600 samples ≈
~100 KB. Trivial.

### 2.4 Per-position allocation counters

Every `Alloc::*` call (alloc.hh:404+) bumps a thread-local `bytesAllocSincePoll`
counter (one add — 5 ns, vs. 30 ns for a direct per-position map insert).
At each safe-point poll, the accumulated bytes get attributed to the
**current source position** via a single `unordered_map<uint32_t, AllocBucket>`
insert (~50 ns per 10k opcodes).

This trades attribution precision for hot-path speed: allocations done within
a 10k-opcode window all get attributed to the position at the *end* of the
window. In practice positions usually stay on-line for many opcodes, so the
bias is negligible for steady-state workloads. The same trade-off `tcmalloc`,
`heaptrack`, and Go's heap profiler make.

**Lazy semantics fall out automatically**:
- Memory profile: counter bumped at `Alloc::*` (creation site of the thunk).
- CPU profile: sample taken at force time (current `ip` is inside the forced thunk).
- Both views show in the same JSONL output via separate columns.

### 2.5 Special cases

| Case | Decision |
|---|---|
| Bridge to TW | Time during the bridge attributed to the v3 caller's position (right semantics: "v3 waiting on this primop"). TW-internal time can be sub-profiled by enabling TW's own `SampleStack` separately. |
| `import` re-entry | Walk `tlActiveVMStack`; concatenate inner frames after a synthetic `import` boundary marker. |
| fakeClo recycling | Read `closure->desc->posHandle` at sample time, not at cache; recycled closures naturally pick up the current binding. |
| Cycles / blackhole catches | The sampler reads `frames` after dispatch processes the next opcode, so the snapshot is always coherent. `clearBlackMarksOnException` runs inside the catch (no scavenge/sample fires). |
| Native intrinsics | Same as TW primops: time attributed to the OP_CALL site. Intrinsics can opt-in to sub-step attribution by calling `profileWithCurrentPos(handle)` if desired. |

## 3. Output formats

### 3.1 JSONL intermediate (source of truth)

```jsonl
{"hdr":"v3-profile/1","evaluator":"v3","t_start":"2026-05-21T08:50:11Z","cmd":"nix eval github:IntersectMBO/cardano-node#hydraJobs..."}
{"posTable":[{"h":1,"file":"flake.nix","line":42,"col":5},{"h":2,"file":"/nix/store/.../lib/attrsets.nix","line":1696,"col":7},...]}
{"sample":{"t":12345,"stack":[1,2,7,15],"alloc":1024,"heap":536870912,"rss":1073741824,"kind":0}}
...
{"pos":2,"alloc_closures":12345678,"alloc_thunks":987654321,"alloc_lists":...,"forces":1248712,"calls":3,"opcodes":98765}
{"pos":7,"alloc_closures":...,"forces":...}
...
```

This format is the contract. All downstream consumers (pprof exporter, Firefox
exporter, Speedscope exporter, CSV exporter, custom HTML report) read it.
JSONL chosen over a single JSON blob so it streams cleanly during long evals
and partial output survives a crash.

Compressed size on cardano-node-scale: ~5–50 MB raw, ~1–10 MB gzip.

### 3.2 pprof protobuf (primary for source heatmap)

Why: `go tool pprof -http=:8080 profile.pb` produces the **annotated-source
view** that is exactly the "per-line heatmap" the user asked for. Plus a
Graphviz call-graph, flat/cum table, and flame graph. Multi-metric native
(`sample_type = [cpu/nanoseconds, alloc_space/bytes, alloc_objects/count]`).

Mapping:
- `Profile.sample_type` = `[cpu/nanoseconds, alloc_space/bytes, alloc_objects/count]`.
- `Profile.sample[]` = our `Sample` records.
- `Sample.location_id[]` = ordered list of `Location` IDs (one per stack frame).
- `Location.line[].function_id` → `Function.filename` + `Function.start_line`.
- `Location.line[].line` = exact line within `Function.filename`.

Implementation: a ~300 LoC C++ emitter using the generated `profile.pb.cc`
stub, OR a ~150 LoC text-pprof emitter (`go tool pprof` accepts both).

Viewer: `go tool pprof -http=:8080` (web UI, free, polished). Also accepted
by Speedscope, pprof.me, Grafana Pyroscope.

### 3.3 Firefox Profiler JSON (primary for flame graph + sharing)

Why: `profiler.firefox.com` is the closest off-the-shelf experience to Xcode
Instruments for an interpreter. Native first-class `FuncTable.lineNumber` +
`FrameTable.line` + `nativeAllocations` table. Diff-view between two profiles
(TW vs v3 — perfect for our comparison workflow). Web-based, free, polished,
share-by-URL after upload.

This is also the format `samply` emits, so a user who wants a complementary
external sampler view can run `samply load profile.json` and load both
profiles into the same UI.

Implementation: column-oriented tables (`stringTable`, `funcTable`, `frameTable`,
`stackTable`, `samples`, `markers`). ~500 LoC TypeScript-style schema → C++ emitter.

### 3.4 Speedscope (free upgrade via pprof reader)

Speedscope reads pprof natively. By emitting pprof we get Speedscope's
flame-graph + sandwich + time-ordered views for free at `speedscope.app`.

### 3.5 CSV (ad-hoc analysis)

A flat `pos, file, line, col, cpu_self, cpu_incl, alloc_self, alloc_incl,
force_count, called_from_user_code` table for `pandas` / `duckdb` exploration.
~50 LoC.

## 4. Visualization UX

Two paths, sharing the same JSONL contract.

### 4.1 Off-the-shelf (MVP)

User invokes:
```bash
NIX_V3_PROFILE=/tmp/profile.jsonl nix eval github:IntersectMBO/cardano-node#... 
bench/prof.py export-pprof   /tmp/profile.jsonl /tmp/profile.pb
bench/prof.py export-firefox /tmp/profile.jsonl /tmp/profile.json

# CPU + memory annotated source view
go tool pprof -http=:8080 /tmp/profile.pb

# Polished flame graph + memory track + share-by-URL
open https://profiler.firefox.com/from-file
# (drag /tmp/profile.json into the page)
```

This is the *real* MVP — no custom rendering code. The "visual UX" the user
asked for is delivered by two mature, free, polished web viewers that
already know how to display per-line CPU and memory attribution. Total
custom code: ~150 LoC C++ emitters + ~200 LoC Python post-processor + ~5
days of integration.

### 4.2 Custom HTML report (Phase 2, optional)

A self-contained HTML report that combines:
- Canvas-virtualized flame graph (top-left).
- Source-annotated heatmap with **dual-gutter per line** (Instruments-style:
  thick bar = CPU, thin bar = Alloc) (top-right).
- Top-bar summary: `Eval: 42.3s · Alloc: 8.7 GB · 12 472 source positions ·
  47 flake inputs · 16 IFDs (4 cached)`.
- Lineage row: `evalModules 14.2s · callPackage 5.7s · foldl' 4.1s · mapAttrs
  2.8s · derivationStrict 2.1s · …` — clickable, each focuses sandwich view.
- Three lineage modes via toolbar toggle: **Force-site** (default), **Alloc-site**,
  **Retention**. The mode applies uniformly across both panes; switch mode →
  both panes recolor with the same camera.
- Framework blackboxing keyed by **flake input identity** (not directory) —
  more useful than Chrome's path-based heuristic because we know precisely
  which positions belong to `nixpkgs` vs the user's flake vs IFD-imported
  transitive flakes via store-path provenance.

Effort: ~3 weeks (JSON contract → 1 day Speedscope export → 1 week HTML v1
→ 1 week canvas flame → 3 days lineage modes).

This is *only* worth building if the off-the-shelf MVP proves insufficient —
likely cases: (a) we want share-by-attachment without uploading to a third-
party server; (b) we want the Nix-specific innovations (flake-input blackboxing,
alloc-site vs force-site toggle) that no off-the-shelf viewer supports.

### 4.3 What we explicitly do not build

- A custom Instruments package (`.instrpkg`). Apple's `os_signpost` is
  rate-limited; v3 emits ~1M force events per second on cardano-node and
  signposts will silently drop. Even at coarse granularity (per-derivation
  only), `.instrpkg` is ~2 weeks of XML/CLIPS work, macOS-only, and not
  share-by-URL. The Firefox Profiler is the cross-platform, web-shareable
  equivalent — and `samply load` on macOS gives a native-feeling view via the
  same format. Skip Instruments unless someone specifically asks for it.
- A VSCode extension. v1.2 once the JSON contract is stable.
- Chrome trace event format. Samples are bolted on, not native; source
  attribution is `args`-stringly-typed; cardano-node-scale JSON files blow
  up the viewer. pprof + Firefox Profiler are strictly better fits.

## 5. Implementation order

### Phase 0 (~0.5 day): contract

1. Document the JSONL intermediate format in `bench/profile-format.md`.
2. Stub `nix::v3::profile` API in `include/v3/profile.hh`:
   `startProfile()`, `stopProfile()`, `recordSample(vm, cu, ip)`,
   `attributeAllocPoll(posHandle)`, `bumpAllocBytes(kind, n)`, `profileActive()`.

### Phase 1 (~5 days): data layer

3. Add `Binding::pos` to `ir.hh`; propagate via `lower.cc` central addBinding helper. (1 day)
4. Add `CompilationUnit::codePosMap`; populate from `emit.cc::emitOne()`. (1.5 days)
5. Implement `profile.cc`: per-position counter map, sample buffer, sampler thread, JSONL writer. (2 days, modelled on `heap_trace.cc`)
6. Wire `vm.cc:2307` safe-point poll branch. (0.5 day)

### Phase 2 (~2 days): exporters

7. Python post-processor `bench/prof.py`:
   - `export-pprof` (~150 LoC text-format pprof, then optionally protobuf).
   - `export-firefox` (~200 LoC Firefox Profiler JSON).
   - `export-speedscope` (~100 LoC) — bonus; trivial.
   - `export-csv` (~50 LoC).

### Phase 3 (~0.5 day): docs

8. `USAGE.md` entry: `NIX_V3_PROFILE=path.jsonl` + the export commands + viewer URLs.
9. Add `bench/prof.py --help` walkthrough.

### Phase 4 (optional, ~2-3 weeks): custom HTML report

10. JSON contract validation harness (`bench/test_profile_format.py`).
11. Speedscope export (already done in Phase 2; covers flame-graph view).
12. HTML report skeleton: top bar + file picker + source heatmap pane (HTMl + CSS, no JS yet). (1 week)
13. Canvas flame graph with virtualization, sandwich view, framework blackboxing. (1 week)
14. Three lineage modes (Force-site / Alloc-site / Retention) + bidirectional drill-down. (3 days)
15. Differential view (compare two profiles). (3 days)

### Phase 5 (longer-term, deferred)

16. VSCode extension consuming the same JSONL (CodeLens overlays per line).
17. CI integration: run the profiler on a fixed nixpkgs revision per release; auto-detect regressions.

## 6. Overhead budget

Target: **< 2 % wall-time with profiler ON**; **< 0.1 % profiler OFF**.

| Component | Cost when ON | Cost when OFF |
|---|---|---|
| Per-opcode safe-point branch | ~0.5 ns / poll = 1/10k opcodes | same (predicted not-taken) |
| Per-allocation TLS counter bump | ~5 ns / alloc | 0 (gated branch) |
| Per-poll position attribution | ~50 ns / poll | 0 |
| Sample snapshot (when fired) | ~2 µs (steady_clock + getrusage + GC_get_heap + walk N frames + lookup + map insert) | 0 |

At 20 Hz × 30 s = 600 samples × 2 µs = ~1.2 ms total sample cost.
Per-alloc cost dominates: 100 M allocs × 5 ns = 500 ms over a 30 s eval = ~1.7 %.

**Rule 0 falsifier**: profiler-ON adds ≤ 2 % on `hello.name` and ≤ 5 % on
cardano-node-scale; profiler-OFF adds ≤ 0.1 %. First commit measures on
`fib33` + `lib-evalModules-100` benchmarks. If overhead exceeds budget, the
per-poll aggregation window is the first tunable; if that doesn't close the
gap, the design is wrong.

## 7. How this fits the ROADMAP

This is **enabling infrastructure** for several stages, not a roadmap stage of
its own:

- **ACTION_PLAN Phase 1.5** (workload measurement spike) — the profiler is
  exactly the "drvPath force-rate decomposition" instrumentation that
  Phase 1.5's TODO lists as needed. The four factors of the 200× per-force
  gap can be observed directly: (a) per-opcode dispatch cost via existing
  `NIX_VM_OPCOUNTS`, (b) GC-scan time via existing `NIX_V3_HEAP_TRACE`,
  (c) intermediate allocation count per logical op via the per-position
  byte counters this design adds, (d) Value-identity-sharing via per-attr
  force counters (also added here).
- **Stage 3 (nursery default-on)** — the profile output makes the "before vs
  after Phase D" wall-time comparison legible at the source-position level.
  Profile diffs ("which lib/ position got 3× faster after Phase D landed") are
  the kind of evidence needed to justify default-on.
- **Stage 4 (uniform STG-shape / strictness analysis)** — the alloc-site view
  surfaces which positions allocate the most intermediate thunks; that's the
  candidate list for the strictness pass.
- **Stage 6 (PICs)** — call-site profile data identifies the OP_CALL sites
  worth specializing.

Where to insert it: **ACTION_PLAN Phase 1.5** is the natural home. The
existing PERF_TRACE_TOOL design (`PERF_TRACE_TOOL_DESIGN_2026-05-20.md`)
covers external CPU%/RSS/heap time-series; this design covers per-source
attribution. The two are complementary — perf-trace tells you *when* time
was spent; this profiler tells you *where in source* it was spent. Both
should land before Phase 1.5 closes.

## 8. Things deliberately deferred (with reasons)

- **Full per-IP map (option b from the sampling agent)**. 50 MB on cardano-
  node-scale CU vs 10 MB compressed; binary search is fast enough that the
  memory saving is more valuable than the constant-time lookup.
- **Per-allocation event log** (vs counter). 100 M alloc events × 16 B/event =
  1.6 GB; even ring-buffered at 16 MB this hides which positions dominate
  under steady state. The counter has the same end-state information.
- **Retention profile** (which allocations survived to end-of-eval). Requires
  a tagging pass over the cell arena at end of eval. Behind a `--retention`
  flag because of the cost; not on by default.
- **VSCode extension**. The JSONL contract is the prerequisite; build the
  extension once the format is stable. v1.2.
- **Custom Instruments package**. `os_signpost` rate-limit drops events at v3
  rates; macOS-only; ~2 weeks. The Firefox Profiler + samply path delivers
  the same UX cross-platform.
- **TW-side cross-attribution**. When v3 bridges to TW, TW's internal time
  goes unattributed to v3 positions. We attribute it to the v3 calling
  position (right semantics: "v3 is waiting on this primop"). If a workload
  is TW-dominant, run TW's existing `SampleStack` profiler separately and
  merge the views externally.

## 9. Falsifiable claims (Rule 0)

This design commits to:

1. Profiler-ON overhead ≤ 2 % on `hello.name`, ≤ 5 % on cardano-node-scale.
2. Profiler-OFF overhead ≤ 0.1 % (one predicted-not-taken branch per safe-point poll + one per alloc).
3. The JSONL → pprof → `go tool pprof -http` workflow renders an annotated-
   source view that correctly highlights the top 10 hot Nix source positions
   on cardano-node within ~10 seconds of post-processing.
4. The JSONL → Firefox Profiler workflow renders a flame graph that
   correctly attributes ≥ 90 % of cardano-node eval wall time to specific
   Nix source positions (the remaining ≤ 10 % is bridge-to-TW time attributed
   to the v3 calling position).
5. The custom HTML report (Phase 4, optional) loads a cardano-node-scale
   profile (~50–200 MB JSONL) in ≤ 5 seconds and renders ≤ 60 fps on a
   modern laptop browser.

Each falsifier is testable on the first commit. If any fails, the design is
wrong and we revise before the next commit.

## 10. Cross-references

- `PERF_TRACE_TOOL_DESIGN_2026-05-20.md` — complementary time-series external
  sampler (CPU% / RSS / Boehm heap). This design covers per-source attribution.
- `ACTION_PLAN_2026-05-15.md` Phase 1.5 — the "drvPath force-rate
  decomposition" TODO this profiler enables.
- `LESSONS_LEARNED_2026-05-15.md` §4.9 Item 5 — "documented CPU-profile
  workflow" listed as a gap; this design closes it.
- `EXTEND_DERIVATION_INVESTIGATION_2026-05-18.md` — the 200× per-force gap
  decomposition this profiler instruments.
- Branch `angerman/2.34-ifd-profiling` (TW side) — `Statistics::functionAllocs`,
  `functionCalls`, `attrSelects`, `ifdEvents`. The JSONL contract here is a
  superset; v3 emits the same kind of records plus per-IP attribution.
- `src/libexpr/eval-profiler.cc:121-181` — TW's `SampleStack` (folded-stack
  format). v3's pprof / Firefox emitters can sit alongside; their outputs can
  be loaded into the same viewer for cross-evaluator comparison.

## 11. Bottom line

**8 days of focused work** ships a working per-line CPU + memory profiler
that exports to two polished off-the-shelf web viewers. The substrate
(positions, descriptor counters, sampling poll point) is already in place;
the missing pieces are small, well-scoped, and orthogonal to v3's core
correctness work.

**3 weeks more (Phase 4)** ships a custom HTML report with Nix-specific
innovations (flake-input-aware framework blackboxing, alloc-site vs
force-site lineage modes) that no off-the-shelf tool does. Build this only
after the MVP proves the off-the-shelf viewers don't fit.

The Xcode/Instruments-like UX the user asked for is best delivered not by
building a custom Instruments package (rate-limited, macOS-only) but by the
Firefox Profiler running in any browser, on any OS, with the same flame
graph + memory track + source view + diff-view capabilities — plus
`go tool pprof -http` for the per-line source heatmap that is the most
direct answer to "where in the .nix did we spend time/memory."

## Copyright

Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
Input Output Group. SPDX-License-Identifier: Apache-2.0
