# Nix memory profiler — user-facing retention attribution design

**Date:** 2026-05-27
**Author:** session synthesis (2-agent research: v3 code inventory + prior-art survey)
**Status:** strategic design — Stage 15 memory-profiler sibling; Phase 1 foundation is team-internal (no-regret); Phases 2+ are UX pillar (deferred per ROADMAP "post-perf + post-IFD")
**Triggering question:** "How could we build tracing infrastructure that helps a Nix author understand why something holds on to a lot of live data, so they can better architect their code and let the GC reclaim memory more / faster?"

Companion docs:
- [`NIX_PROFILER_DESIGN_2026-05-21.md`](NIX_PROFILER_DESIGN_2026-05-21.md) — Stage 15 WALL profiler design (this is the MEMORY sibling)
- [`IDEAL_GC_DESIGN_2026-05-26.md`](IDEAL_GC_DESIGN_2026-05-26.md) §1 — GC investments are RSS-primary (rule this doc operates under)
- `live_trace.cc` (commit `f3491859f` / `5d2b194cb`) — existing per-type retained-bytes tracer; foundation for Phase 1
- [`MEMORY_REDUCTION_AVENUES_2026-05-26.md`](MEMORY_REDUCTION_AVENUES_2026-05-26.md) §Category 1 — measurement-first framing this doc extends to user-facing

---

## 1. Position (TL;DR)

**The user problem is real and underserved.** No Nix evaluator has user-facing retention attribution. The v3 team has built ~80 % of the infrastructure needed (live-trace + precise-roots + per-object PosIdx on the dominant types). **Phase 1 — source-attributed live-trace with PosIdx aggregation — is 3-5 days of work and would directly help the v3 team's current cardano-node M5 memory investigation.** Phases 2-7 are UX pillar (Stage 15-sibling) deferred per ROADMAP.

**The architecture: GHC + V8 hybrid.** Import GHC's biographical profiling (LAG/DRAG/VOID thunk classification — directly diagnoses lazy-language memory leaks) and V8's dominator-tree retention analysis (handles Nix's heavy sharing through `lib.fix` / `callPackage`). Reuse v3's existing PosIdx infrastructure (Bindings::Entry::pos + LambdaDescriptor::posHandle) + precise-root walker. **Don't build parallel infrastructure.**

**Phase 1 deliverable (~3-5 days):** `NIX_V3_LIVE_TRACE_BY_POS=1` env var; per-PosIdx retained-bytes report at end of eval. Identifies top-N source positions retaining memory; immediately useful for team's M5 investigation.

**The full user-facing tool** (Phases 2-7, ~3-4 months) is Stage-15-class work that should live with the UX pillar sequencing.

---

## 2. The user problem

A Nix author runs `nix build .#my-package` and gets OOM-killed. Or sees 4 GB RSS in `nix eval`. Or wants to optimize their flake's CI memory footprint. They have specific questions today's tooling cannot answer:

| Question | What today's tools say | What the user actually wants |
|---|---|---|
| "Why does my flake eval take 3 GB?" | `time -v` shows peak RSS | "Top 10 source positions retaining memory; click for retention path" |
| "Why is `lib.fix` overlay expensive?" | nothing | "Your overlay at `flake.nix:42` is captured by N callPackage chains; retains 80 MB" |
| "Did adding this dependency cost me memory?" | impossible to measure delta | "Snapshot-before / snapshot-after diff shows this added 250 MB retained" |
| "Is this `with` capturing too much scope?" | static guesswork | "This `with` retains 47 unused upvalues totaling 45 MB" |
| "Why isn't this thunk being collected?" | nothing | "Thunk at `lists.nix:1213` retained but never forced (LAG)" |

These are real, recurring, and unaddressed by any current Nix evaluator. **A user-facing memory profiler is a genuine differentiator** — and the v3 team already has most of the infrastructure built.

### 2.1 Production scenarios driving demand

- **Cardano-node CI** at IOG: 4.5-6.5 GB RSS per evaluation; exceeds 4 GB watchdog
- **haskell.nix users**: deep callPackage chains + lib.fix overlays create multi-GB retention; manual `materialization/` workaround exists precisely because retention is opaque
- **NixOS module-system stress**: fix-point evaluation can balloon; "which option is the worst retainer?" is unanswerable today
- **Library maintainers** (nixpkgs reviewers): "does this PR add memory?" requires bisecting commits manually

---

## 3. What infrastructure exists in v3 today (Agent A inventory)

### 3.1 Per-object PosIdx tracking — already present on dominant types

| Object type | Has PosIdx? | Where stored | Header cost |
|---|---|---|---|
| **Bindings::Entry** | ✓ YES | `Entry::pos` (uint32_t) at offset 4-7 (inlined pad slot, #752) | 0 B added |
| **Thunk** | ✓ YES | `Thunk::suspended.desc` → `LambdaDescriptor::posHandle` | 0 B added (via shared descriptor) |
| **LambdaDescriptor** | ✓ YES | `LambdaDescriptor::posHandle` (uint32_t) | 4 B (existing field) |
| Closure | ✗ NO | — | +4 B if reuse `_pad`; else +8 B |
| ListVec | ✗ NO | — | +4 B if reuse `pad` |
| ValuePair | ✗ NO | — | +4 B (breaks 16-B alignment; needs care) |
| Env | ✗ NO | — | +4 B if reuse pad |
| String (`Tag::String`) | ✗ NO | — | non-trivial (no object header today; stored as `const char*`) |

**Implication for Phase 1**: Bindings + Thunk + LambdaDescriptor cover the dominant retained bytes (per `5d2b194cb` HNE measurement: Bindings 72.5 % live = the lion's share; Closures/Lists/Pairs all <10 % live). Phase 1 can ship without Closure/ListVec/Pair PosIdx and still attribute >80 % of retained memory.

### 3.2 Live-trace walker — already exists and is cleanly extensible

`live_trace.cc` (commit `f3491859f`) walks live graph from precise roots via BFS; reports per-type live/allocated bytes. Extension points identified:

1. **Per-object PosIdx extraction during walk** — each `walkX` function knows the type:
   - `walkThunk` (line 137): `posHandle = t->suspended.desc->posHandle` when state == Suspended
   - `walkBindings` (line 185): `posHandle = b->entries[i].pos` per entry
   - Bridge thunks have no `suspended.desc`; need union-case guard

2. **Aggregation** (line 69): replace flat `LiveCounters counts` with `std::unordered_map<uint32_t, LivePosEntry> posData`

3. **Predecessor tracking** (line 113-118): add optional `std::unordered_map<void*, void*> predecessors` populated in `enqueue` when seen-set insert succeeds

4. **Output** (line 244-371): call `resolvePosSnapshot(posHandle)` → `{file, line, column}` for user-facing rows

### 3.3 PosIdx granularity

**AST-node-level.** Each attribute definition, formal parameter, and dynamic binding gets its own PosIdx. Dedup pool collapses repeated positions (per `posSnapshotIndex()` in alloc.hh:1841-1875).

**Spread:** typical Nix files have 10²-10⁴ distinct source positions; hello.drvPath likely 10²-10³ — small enough for a hash-map aggregation, large enough to be meaningful.

### 3.4 What's missing for the full user-facing tool

| Capability | Status | Effort to add |
|---|---|---|
| Per-PosIdx retained-bytes aggregation | NOT BUILT | 3-5 days (Phase 1) |
| Retention-path sampling (one predecessor per object) | NOT BUILT | 1-2 weeks (Phase 2) |
| Biographical profiling (LAG/DRAG/VOID per thunk) | NOT BUILT | 2-3 weeks (Phase 3) |
| Dominator-tree analysis (shared-object attribution) | NOT BUILT | 2-3 weeks (Phase 4) |
| Snapshot diff (delta-cost-attribution) | NOT BUILT | 2-3 weeks (Phase 5) |
| User-facing builtins (`builtins.profileMemory`) | NOT BUILT | 1 week (Phase 6) |
| Output formatting (flame-graph SVG / HTML report) | NOT BUILT | 2-3 weeks (Phase 7) |
| Closure PosIdx tracking | NOT BUILT | 2-3 days (deferred follow-on) |
| ListVec/Pair/Env PosIdx tracking | NOT BUILT | deferred follow-on |

---

## 4. Prior art (Agent B distilled)

Six profiler families surveyed. Three are directly applicable to Nix:

### 4.1 GHC heap profilers (the strongest analog)

Lazy + functional + sharing → maps cleanly to Nix. Key concepts to import:

**Biographical profiling (`+RTS -hb`)** — THE headline lesson. Classifies every closure into one of four states:

| State | Meaning | Nix-relevance |
|---|---|---|
| **LAG** | Allocated, but not yet used (forced) | **Lazy-leak diagnostic.** "Thunk at lib/lists.nix:1213 allocated 5 GC cycles ago but never forced — is it actually used?" |
| **USE** | Currently active (just forced; will be referenced again) | Steady-state |
| **DRAG** | Used once, but still retained | **Lib-fix overlay leak diagnostic.** "Bindings forced at start of eval, retained until end despite no further reads" |
| **VOID** | Never used; about to be collected | Profitable to surface; usually an architectural anti-pattern |

For Nix specifically: VOID thunks identify dead overlays; DRAG thunks identify the `lib.fix` / `callPackage` retention pattern; LAG thunks identify lazy-leak anti-patterns. **This is directly the model Nix authors need.**

**Cost-centre stacks (`+RTS -hc`)** — every closure tagged with its lexical call-stack (top N frames). Maps to Nix as `{file:line:col} + {top-N-caller-PosIdx-chain}`. Critical for the "library-vs-user-code" attribution problem (per §5.7 below).

**hp2ps output** — time-series flame-graph SVG. Visually shows retention growing over time, decomposed by category. Nix equivalent would be RSS-over-eval-step decomposed by source-position class.

References: Sansom & Peyton Jones "Time and Space Profiling for Non-Strict, Higher-Order Functional Languages" (POPL '95); Runciman & Wakeling "Heap Profiling of Lazy Functional Programs" (JFP '93).

### 4.2 V8 heap snapshots (retainer + dominator)

**Dominator tree analysis** — THE second headline lesson. Each object O's "retained size" = sum of bytes O exclusively dominates (would be freed if O were freed). Distinguishes:
- **Shallow size**: O's own bytes
- **Retained size**: O's bytes + everything only O dominates

For Nix: lib.fix overlays create heavy multi-path sharing. The same `pkgs.stdenv` is reachable from thousands of callPackage chains. Naive attribution charges stdenv to every retainer; dominator-tree attribution charges it once, to its dominator (whichever ancestor uniquely retains it).

**Retainer view**: back-walk from any object to GC root, showing all retention paths. For each retainer, the snapshot diff distinguishes "exclusive retention" from "shared retention." Critical for the user question "why is THIS thing retained?"

**Snapshot diff** (V8 Comparison view): "what grew between snapshot A and snapshot B" — the natural API for "did my flake change add memory?"

### 4.3 Other models — selective import

- **Python tracemalloc**: per-allocation traceback + `snapshot.compare_to()`. Allocation-rate not retention focus. APPLIES: sampling at 1/Nth allocation pattern. DOESN'T APPLY: tracks allocation not retention.
- **Ruby ObjectSpace + memory_profiler**: full enumeration of live objects. APPLIES: per-Tag summary table (already half-built via NIX_V3_BINDINGS_ATTR #746). DOESN'T APPLY: full enumeration too expensive at 6.5M-alloc scale.
- **Erlang :recon**: per-process heap attribution. DOESN'T APPLY: Nix has no process model. BUT analog applies: per-EvalScope or per-flake-module attribution.
- **Java JFR**: sampling-based + flame graphs. APPLIES: sampling overhead profile (every Nth alloc). DOESN'T APPLY: JVM monomorphic class metadata; Nix is dynamically typed at the Value level.

### 4.4 Lessons NOT to import

| Anti-pattern | Why it doesn't fit Nix |
|---|---|
| Erlang per-process accounting | No process model |
| JFR allocation flame graphs as PRIMARY view | Flame graphs answer "where did time happen" not "what is retained"; Nix users care about retention. Use only as Tier-2 view |
| Ruby full ObjectSpace enumeration as default | 6.5M allocs × 6 GB RSS = 10+ s and 500+ MB profiler-own state. Must be opt-in / sampled |
| Continuous tracemalloc-style tracking | High allocation rate (6.5M/eval) makes overhead unacceptable; sampling required |

---

## 5. Design proposal

The v3 memory profiler is a hybrid: **GHC biographical model + V8 dominator/retainer model + Nix-specific PosIdx tracking**.

### 5.1 Core capability — source-attributed retained bytes (Phase 1, ~3-5 days)

**End of eval, walk live graph, aggregate retained bytes by source PosIdx, output top-N.**

```
$ NIX_V3_LIVE_TRACE_BY_POS=1 nix eval --impure --expr '(import <nixpkgs> {}).hello.drvPath'

V3 live-trace-by-position (hello.drvPath end of eval):

Total live:   285.67 MB / 525.07 MB allocated (54 %)
Total freeable lower bound: 239.40 MB

Top 20 source positions by retained bytes:

  bytes    objects  source position
  -----    -------  ---------------
  62 MB    142      nixpkgs/pkgs/build-support/stdenv.nix:42    (mkDerivation)
  35 MB     89      nixpkgs/lib/lists.nix:1213                  (foldl' accumulator)
  28 MB     1247    nixpkgs/lib/fixed-points.nix:95             (lib.fix overlay)
  19 MB     56      nixpkgs/pkgs/build-support/call-package.nix:24
  14 MB     203     nixpkgs/lib/attrsets.nix:127                (mergeAttrs)
  ...      ...      ...
```

This is the foundation. All later phases extend it. **Builds on already-landed live-trace; no architectural risk.**

Concrete implementation (per Agent A §3.2):

```cpp
// live_trace.cc additions
struct LivePosEntry { size_t count; size_t bytes; };
std::unordered_map<uint32_t, LivePosEntry> posData;

// In walkThunk (live_trace.cc:137):
if (t->state == ThunkState::Suspended) {
    uint32_t pos = t->suspended.desc->posHandle;
    if (pos) {
        auto & e = posData[pos];
        e.count++; e.bytes += sizeof(Thunk);
    }
}

// In walkBindings (live_trace.cc:185):
for (uint32_t i = 0; i < b->size; ++i) {
    uint32_t pos = b->entries[i].pos;
    if (pos) {
        auto & e = posData[pos];
        e.count++; e.bytes += sizeof(Bindings::Entry);
    }
    visitValue(b->entries[i].value);
}

// At dump time (live_trace.cc:296+):
sort posData by bytes desc;
for each top-N: resolvePosSnapshot(pos) → file:line:col;
print table.
```

**Pre-committed acceptance criteria (per measure-twice §3):**
- Output shows ≥3 distinct PosIdx buckets summing to ≥80 % of Bindings+Thunk bytes
- Runs to completion in <1 sec on hello.drvPath
- Top-5 buckets visually meaningful (correspond to identifiable nixpkgs functions)

### 5.2 Biographical profiling — LAG/DRAG/VOID for thunks (Phase 3, 2-3 weeks)

Per GHC `+RTS -hb`. Add to each Thunk:
- `allocTick` — eval-step number when allocated
- `firstForceTick` — eval-step number when first forced (0 if never)
- `lastTouchTick` — eval-step number last reached during walk

At snapshot time, classify:
- **VOID:** `firstForceTick == 0` (never forced; allocated and retained)
- **LAG:** `firstForceTick == 0` AND held by live retainer (allocated, retained, not used)
- **USE:** `firstForceTick > 0` AND `lastTouchTick == current`
- **DRAG:** `firstForceTick > 0` AND `lastTouchTick < current - N` (forced, then dragged)

Output:

```
Thunk biographical classification (hello.drvPath end of eval):

  Class    Count   Bytes      Top source position
  -----    -----   -----      -------------------
  VOID    8,442   18 MB      nixpkgs/lib/attrsets.nix:127 (mapAttrs branches never taken)
  LAG     3,116    7 MB      nixpkgs/lib/lists.nix:1213 (foldl' intermediate)
  DRAG   12,341   24 MB      nixpkgs/lib/fixed-points.nix:95 (overlay fix-point cells)
  USE       213   <1 MB      various (current-frame locals)
```

**VOID + LAG + DRAG identify the actionable refactoring targets.** USE is steady-state. This is the model that diagnoses haskell.nix `materialization` retention.

Cost: 4 B per Thunk × 1.6M Thunks on hello.drvPath = ~6 MB per-thunk tick overhead. Acceptable; ships under `V3_RELEASE` strip if not needed.

### 5.3 Dominator-tree retention attribution (Phase 4, 2-3 weeks)

Per V8 retained-size model. Compute Lengauer-Tarjan dominator tree over the live graph; for each object O, "retained size" = sum of bytes O exclusively dominates.

**Critical for Nix** because `lib.fix` / `callPackage` create heavy multi-path sharing. Without dominators:
- `pkgs.stdenv` is reachable from 1247 callPackage chains
- Naive attribution charges stdenv to every retainer
- User sees "stdenv retains 200 MB" attributed 1247 times = misleading

With dominators:
- stdenv has ONE dominator (typically the flake-output build path or `nixpkgs.lib`)
- Retained-size charged once to the dominator
- User sees "if this flake-output were freed, 200 MB would be reclaimed"

**Don't do dominator computation in-process** (Lengauer-Tarjan is O(V·E)). Snapshot the graph; compute dominators offline:

```bash
NIX_V3_HEAP_SNAPSHOT=snap.json nix eval ...
nix v3 memprof --dominators snap.json --output dom.json
nix v3 memprof --report dom.json --top 20
```

### 5.4 Retainer paths (Phase 2, 1-2 weeks)

For any selected object/PosIdx, show the back-walk to root. V8-style "why is this retained?"

Implementation: during BFS walk, record one predecessor per object (cheap; ~8 B per object snapshot). At query time, walk from chosen object back through predecessors to root.

```
Retention path for foldl' accumulator at lib/lists.nix:1213:

  v3 root: g_currentVMState
    → flake output 'packages.x86_64-linux.cardano-node'
      → Closure mkDerivation at pkgs/build-support/stdenv.nix:42
        → Bindings 'inputs' at flake.nix:18
          → Thunk lib.unique at lib/lists.nix:1187
            → ListVec result at lib/lists.nix:1213  ←  THIS retains 35 MB

Refactoring suggestion:
  The foldl' accumulator at lib/lists.nix:1213 is held by lib.unique
  at lib/lists.nix:1187, called from your flake.nix:18 'inputs' attribute.
  Restructure your 'inputs' to not depend on this list, OR avoid lib.unique
  on the large list, to free 35 MB.
```

Per Agent A §3.2 step 4: ~8 B per object header for predecessor pointer (or side-table; can avoid header cost if side-table is acceptable).

### 5.5 Snapshot diff (Phase 5, 2-3 weeks)

`builtins.memSnapshot` + diff command:

```nix
let
  snapshot1 = builtins.memSnapshot "before-hello";
  result   = pkgs.hello.outPath;
  snapshot2 = builtins.memSnapshot "after-hello";
in {
  inherit snapshot1 snapshot2;
  diff = builtins.memSnapshotDiff snapshot1 snapshot2;
}
```

```
$ nix v3 memprof --diff before-hello after-hello

Added between snapshots:  250 MB retained
Top added retainers:
  +82 MB   nixpkgs/pkgs/applications/networking/browsers/firefox/...
  +35 MB   nixpkgs/lib/lists.nix:1213 (lib.unique called more)
  +28 MB   nixpkgs/pkgs/build-support/stdenv.nix:42 (mkDerivation closures)
```

The natural API for "did my change add memory?"

### 5.6 Per-Tag + per-source-position rollup (Phase 1.5, ~1 week)

Already half-built via `NIX_V3_BINDINGS_ATTR=1` (commit `66b1061cd` #746). Extend to all object types and to PosIdx breakdown:

```
$ NIX_V3_LIVE_TRACE_BY_POS=1 NIX_V3_LIVE_TRACE_TAG_DETAIL=1 nix eval ...

Per-Tag rollup with PosIdx detail (hello.drvPath end of eval):

  Tag        Live  Allocated  Live%   Top source positions
  ----        ----  ---------  -----  --------------------
  Bindings   281 MB  388 MB    72.5%  
                                      62 MB stdenv.nix:42
                                      35 MB lists.nix:1213
                                      28 MB fixed-points.nix:95
                                      ... (top 10 per Tag)
  Thunks      1.6 MB  52 MB    3.0%   
                                      0.4 MB attrsets.nix:127 (LAG)
                                      0.3 MB lists.nix:1187 (DRAG)
                                      ...
  Closures   89 KB   44 MB    0.2%   (mostly VOID — barely retained)
  Lists      24 KB   12 MB    0.2%   (mostly VOID)
  ...
```

The 0.2-3 % live fraction on Closures/Lists/Pairs identifies the massive concentration opportunity.

### 5.7 The library-vs-user-code attribution problem

A subtle but critical issue: a Bindings allocated by `lib.fix` is technically at `lib/fixed-points.nix:95`. But the USER doesn't write `lib.fix` directly — they write `(self: super: { ... })` in their own flake.

**Two-axis attribution** (per GHC cost-centre stacks):

- **Allocation site**: the AST node that emitted the Bindings (`lib/fixed-points.nix:95`)
- **Caller chain**: top N frames of the eval stack at allocation time (e.g., `flake.nix:42` → `lib.fix` → `nixpkgs-overlay.nix:7`)

The report shows both:

```
35 MB   lib/lists.nix:1213    foldl' accumulator
        Called from your code at:
          flake.nix:42        builtins.foldl'
          flake.nix:18        'inputs' attribute
        (top 3 caller frames; deepest first)
```

This answers the user's actual question: "what in MY code is causing the library to retain memory?"

Implementation cost: tag each allocation with a caller-chain fingerprint (4-frame hash). At report time, group by caller-chain. Cheap if sampled (every Nth allocation); expensive if continuous.

### 5.8 User-facing API (Phase 6, ~1 week)

Three integration points:

#### CLI / env var (lowest friction)

```bash
# End-of-eval report
NIX_V3_LIVE_TRACE_BY_POS=1 nix eval ...

# Snapshot mode (for diff)
NIX_V3_HEAP_SNAPSHOT=snap.json nix eval ...

# CLI report tool
nix v3 memprof --report snap.json --top 20
nix v3 memprof --diff before.json after.json
nix v3 memprof --retainer-path snap.json --site lib/lists.nix:1213
```

#### Builtin

```nix
let
  snapshot = builtins.memSnapshot "label";
  report   = builtins.memProfile pkgs.hello.outPath;
in report.topRetainers
```

#### REPL integration

```
nix repl> :profile pkgs.hello.outPath
[shows top retainers by source position]

nix repl> :retainer-path lib/lists.nix:1213
[shows path back to root]
```

---

## 6. Phased implementation plan

| Phase | Scope | Effort | Status |
|---|---|---|---|
| **Phase 1 — Foundation** | Source-attributed live-trace (PosIdx aggregation) | **3-5 days** | **No-regret; benefits team's CURRENT M5 work** |
| Phase 1.5 | Per-Tag rollup with PosIdx detail | 1 week | |
| Phase 2 | Retention path sampling (one predecessor per object) | 1-2 weeks | |
| Phase 3 | Biographical profiling (LAG/DRAG/VOID) | 2-3 weeks | Critical for lazy-leak diagnosis |
| Phase 4 | Dominator-tree retention attribution | 2-3 weeks | Handles `lib.fix` / `callPackage` sharing |
| Phase 5 | Snapshot diff | 2-3 weeks | "Did my change add memory?" API |
| Phase 6 | User-facing builtins + REPL + CLI | 1-2 weeks | |
| Phase 7 | Output formatting (HTML report, flame graph SVG) | 2-3 weeks | UX polish |
| Phase 8+ | Closure / ListVec / Pair PosIdx tracking | 1-2 weeks each | Deferred follow-ons |

**Total Phase 1-7: ~3-4 months.**

### 6.1 Phase 1 stands alone

Phase 1 (3-5 days) is **team-internal diagnostic** that helps cardano-node M5 memory work TODAY. It doesn't require committing to Phases 2-7. Specifically:

- Phase 1 reuses already-landed live-trace + precise-roots
- Adds PosIdx aggregation only (no new headers; no new memory cost)
- Output is CLI text (no UI work)
- Acceptance criteria are bounded and measurable

**Recommend: Phase 1 NOW (this week), independent of UX-pillar sequencing.**

### 6.2 Phases 2-7 are UX pillar

Per ROADMAP §Stages 14/15/17, UX pillar is deferred "post-perf + post-IFD." The full user-facing memory profiler belongs there, alongside Stage 15 (per-line wall profiler) and Stage 14 (error UX) + Stage 17 (pattern lint).

**Defer Phases 2-7 to the UX pillar arc.** Don't pick them up until perf phase exits.

---

## 7. Specific design lessons + recommendations

From the prior-art survey + v3 inventory, the load-bearing recommendations:

1. **Biographical profiling is the headline.** GHC's LAG/DRAG/VOID classification directly diagnoses lazy-language memory leaks. The `materialization/` workaround in haskell.nix is a community response to opaque retention; surfacing DRAG would obviate it.

2. **Dominator-tree is the right model for shared retention.** Nix's `lib.fix` / `callPackage` produce multi-path sharing that breaks naive per-allocation attribution. V8's dominator tree solves this. Compute offline from snapshot; don't do it in-process.

3. **Cost-centre stacks → source-position stacks.** Two-axis attribution (alloc-site + caller-chain) distinguishes library code from user code. 4-frame caller-chain hash is cheap.

4. **Snapshot diff is the natural API for delta-cost.** Author-facing: "did my change add memory?" is the most common question. Diff snapshots; aggregate added retainers.

5. **Sampling, not continuous tracking.** 6.5M+ allocs per eval forbid per-alloc overhead. Sample at 1/1024 or 1/Nth; opt-in continuous for debug mode only.

6. **Reuse existing infrastructure.** PosIdx already on Bindings + Thunk + Lambda. Live-trace walker exists. Precise-root framework exists. **Don't build parallel infrastructure.**

7. **Per-Tag rollup already half-built.** `NIX_V3_BINDINGS_ATTR` (#746) is the per-Bindings attribution. Generalize to all Tags + PosIdx breakdown for free.

8. **CLI text first; UI later.** GHC's hp2ps stayed text-based for decades; pretty UI is a Phase 7 polish, not foundation.

---

## 8. Hard problems + mitigations

### 8.1 Lazy retention semantics

A Thunk's "retained memory" includes:
- (a) Its captured upvalues (definite)
- (b) What the thunk's body will allocate when forced (latent)
- (c) What the thunk's evaluation will retain transitively (latent + transitive)

**Mitigation:** report (a) only by default ("conservative"); opt-in mode forces the thunk transiently and measures (b)+(c) ("empirical"). Document the distinction in user output.

### 8.2 Pattern aggregation (not just line aggregation)

Per-line PosIdx aggregation gives FINE granularity but a user often wants PATTERNS:
- "All overlays using `lib.recursiveUpdate` create deep chains"
- "All `callPackage` calls in this flake capture full `pkgs`"

**Mitigation:** add caller-chain fingerprint (Phase 3+). Group by `(functionName)` aggregated across all call sites:

```
Retained by pattern (top 5):
  lib.fix overlays       350 MB across 47 fix-points
  lib.recursiveUpdate    120 MB across 89 calls
  callPackage closures    95 MB across 1234 instances
```

### 8.3 Source positions are coarse

Library functions in `lib.fix` / `callPackage` are SHARED across many call sites. "lib/fixed-points.nix:95" is technically correct but doesn't tell the user which OF THEIR overlays caused the bloat.

**Mitigation:** §5.7 two-axis attribution. Show alloc-site + top-3 caller-frame PosIdx.

### 8.4 Performance during profiling

Tracing has overhead. Continuous tracking is impractical (6.5M+ allocs).

**Mitigation:** sample at 1/Nth (default 1/1024); snapshot-at-end-of-eval (no per-step overhead); opt-in continuous for debug mode.

### 8.5 Output for non-expert authors

Nix authors aren't v3-team. Output must be self-explanatory + actionable.

**Mitigation:** glossary in output ("a Thunk is a deferred computation; high LAG count means many computations are allocated but never used"); concrete refactoring suggestions when patterns are recognizable; cross-link to common-pattern documentation.

### 8.6 Sharing graph complexity

cardano-node retention graph is millions of objects; even sampled retainer paths can be deep.

**Mitigation:** SUMMARISATION — group by source-position class; show top-N levels of retention path; collapse repeating patterns. Output is a tree summary, not a flat list.

---

## 9. Strategic positioning

### 9.1 This is a real differentiator

No other Nix evaluator has user-facing memory profiling:
- **TW**: nothing
- **Tvix**: nothing
- **Lix**: nothing
- **Snix**: nothing
- **Determinate**: nothing

Even simple Phase 1 (source-attributed live-trace) puts v3 ahead. The full Phase 1-7 system is a publishable, citable feature.

### 9.2 Aligns with memory-first-class framing

Per `IDEAL_GC_DESIGN_2026-05-26.md` §1 (codified 2026-05-27): GC investments are RSS-primary, not wall-primary. A memory profiler is the user-facing surface of that framing.

### 9.3 Aligns with measure-twice methodology

The whole tool is "measure-before-you-cut" applied to USER code. Authors get the same discipline the v3 team has. The internal `live_trace.cc` becomes a stepping stone to the external `nix v3 memprof`.

### 9.4 Foundation work helps the team's current memory work

Phase 1 (~3-5 days) gives the v3 team PosIdx breakdown on M5 — directly informs:
- Which source positions in nixpkgs are the largest retainers
- Whether memory is concentrated in user code or library code
- What to fix first

This is concrete, useful, and benefits TODAY's investigation. **Phase 1 should land regardless of broader UX pillar decisions.**

---

## 10. Honest limits

- **Phase 1 effort estimate (3-5 days)** is based on Agent A's detailed inventory; could be 1-2 days more if `posSnapshotIndex` resolution path turns out to need refactoring.
- **Biographical profiling (Phase 3) overhead** is estimated at 12 B per Thunk × 1.6M = 20 MB on hello.drvPath. Acceptable but non-trivial; `V3_RELEASE` strip should be considered.
- **Dominator-tree computation (Phase 4)** is offline; doesn't impact eval performance but adds tool-chain complexity (separate `nix v3 memprof` binary).
- **The "GHC + V8 hybrid" framing** is genuinely aspirational; both projects have decades of profiler refinement. v3 won't match them in v1.
- **User-facing output for non-experts (§8.5)** is the hardest UX problem; this design doesn't solve it, just acknowledges it.
- **The "publishable, citable feature" claim (§9.1)** assumes v3 reaches operating-state where the profiler is usable. Until then, it's internal infrastructure.
- **Phase 1's "no-regret" framing** assumes the team validates the value proposition in <1 week. If Phase 1 doesn't surface useful patterns within that window, the broader phases should also be reassessed.
- **The two-axis attribution (§5.7)** is straightforward conceptually but expensive in implementation (caller-chain fingerprinting). Phase 1 ships without it; Phases 3+ add it.
- **`builtins.memSnapshot` etc.** are user-facing builtins; adding them is a Nix-language-level decision that may require broader coordination.
- **Closure/ListVec/Pair PosIdx tracking deferred** — these objects have low live-fraction (0.2-3 %) so contribute little to retained bytes; Phase 1's exclusion is defensible but if a future workload exhibits Closure-dominated retention, Phase 1 misses it.

---

## 11. Recommended immediate action

**Phase 1 (3-5 days) starting this week.** Specifically:

1. Day 1-2: extend `live_trace.cc` with per-PosIdx aggregation (Agent A §3.2 steps 1-3)
2. Day 3: add PosIdx resolution for user-facing output (Agent A §3.2 step 4)
3. Day 4: test on hello.drvPath + cardano-node M5; verify ≥80 % attribution
4. Day 5: documentation + landing commit + measurement-data writeup

Pre-committed acceptance per measure-twice §3:
- Output shows ≥3 distinct PosIdx buckets summing to ≥80 % of Bindings+Thunk bytes
- Runs to completion in <1 sec on hello.drvPath
- Top-5 buckets visually meaningful (nixpkgs function names visible)
- Surfaces ≥1 actionable refactoring target on cardano-node M5

If Phase 1 ships positive, Phases 2-7 belong in the UX pillar arc (Stages 14/15/17 alongside).

---

## 12. Cross-references

- [`NIX_PROFILER_DESIGN_2026-05-21.md`](NIX_PROFILER_DESIGN_2026-05-21.md) — Stage 15 wall profiler design; this is the memory sibling
- [`IDEAL_GC_DESIGN_2026-05-26.md`](IDEAL_GC_DESIGN_2026-05-26.md) §1 — RSS-primary framing rule
- [`MEMORY_REDUCTION_AVENUES_2026-05-26.md`](MEMORY_REDUCTION_AVENUES_2026-05-26.md) — measurement-first; this doc extends Category 1 to user-facing
- [`live_trace.cc`](../live_trace.cc) (commit `f3491859f`) — Phase 1 foundation
- [`precise_root.hh`](../include/v3/precise_root.hh) — root walker
- `NIX_V3_BINDINGS_ATTR` (commit `66b1061cd` #746) — half-built per-Tag rollup
- [`ROADMAP_TO_VISION_2026-05-15.md`](ROADMAP_TO_VISION_2026-05-15.md) Stages 14/15/17 — UX pillar
- [`MEASURE_TWICE_CUT_ONCE_2026-05-23.md`](MEASURE_TWICE_CUT_ONCE_2026-05-23.md) — Phase 1 acceptance criteria

**Prior-art references:**
- Sansom & Peyton Jones, "Time and Space Profiling for Non-Strict, Higher-Order Functional Languages" (POPL '95) — GHC biographical profiler foundation
- Runciman & Wakeling, "Heap Profiling of Lazy Functional Programs" (JFP '93) — LAG/DRAG/VOID model
- GHC User's Guide §6 "Profiling" — `+RTS -h{c,d,b,T,m}` documentation
- V8 Heap Profiler internals (web.dev "Memory terminology"; Erik Corry blog 2013) — dominator tree + retainer view
- Python tracemalloc (PEP 454) — per-allocation traceback model
- Java JFR `jdk.ObjectAllocationSample` — sampling-based allocation profiling

---

*Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group. SPDX-License-Identifier: Apache-2.0.*
