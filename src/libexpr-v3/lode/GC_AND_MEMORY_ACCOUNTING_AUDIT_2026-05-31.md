# GC implementation + memory-accounting audit

**Date:** 2026-05-31
**Per:** user directive "Carefully create an in-depth analysis of our as-implemented GC with all the details necessary. As well as the comparison to the TW with Boehm. Is our memory accounting correct, or do we incorrectly overcount?"
**Commit at audit:** `3b6be11ec` (post-App3 combined baseline)
**Status:** AUDIT — comprehensive review of v3's GC stack, accounting formulas, and the v3-vs-TW comparison surface.

---

## TL;DR

* **v3 has FIVE distinct memory regions** that contribute to RSS (Boehm heap, v3 arena, nursery, mmap'd CompilationUnits via ImportCache, C++ stdlib state).  TW has TWO (Boehm + C++ stdlib).
* **The headline accounting formula has TWO load-bearing flaws:**
  1. It mixes a **lifetime PEAK** measurement (`ru_maxrss`) with **current-instant snapshots** (`v3_arena.bytesAllocated()`, `GC_get_heap_size()`).
  2. It treats `v3_arena` as if it were resident memory, but it's a **mmap'd-virtual** byte count.  When arena's virtual footprint exceeds resident RSS (e.g. M5: arena 5838 MB / peak 3836 MB), `elsewhere` clamps to 0 and **the entire "what else is in RSS" decomposition silently loses signal**.
* **Net direction: v3's published numbers UNDERCOUNT live working-set on workloads where arena pages are page-evicted** (M5 today; potentially HNE in larger evals).  Headline "arena ≥ peak" claims are an **artefact of the formula**, not the working set.
* **The `elsewhere-probe`** (the per-container side-table sum) is sound for what it covers but **misses the largest contributors** (ImportCache `CompilationUnit` deque + SQLite + libstore state — together ~990 MB on HNE per HNE_BUCKET_DECOMP).
* **The Stage 6 mark-sweep walker** (mark_sweep.cc, `runMajorMarkSweep`) is correct for the precise-root set walked by `walkAllV3Roots`, BUT it does NOT collect by default (`NIX_V3_MAJOR_GC=1` opt-in) and the cumulative-arena counter does not decrement on cell-free — only on whole-block-free.  This is the structural reason `v3_arena` is unchanging across a run after `bytesAllocated` reaches its peak.

The recommended remediation is laid out in §10.

---

## 1. Architecture overview

### 1.1 Tree-walker (TW) — Boehm-only

```
nix CLI process
├── code (.text, dylibs)             ── resident, OS-counted (~50 MB libnix*)
├── libc malloc heap                  ── std::string / std::vector overhead
│                                       primarily libstore + libutil state
└── Boehm GC heap                     ── nix::Value, nix::Bindings, nix::Env,
                                        Expr nodes, parser AST
                                        — single allocator, single counter
                                        — GC_get_heap_size() is authoritative
```

TW's GC is Boehm with conservative-scan over registered roots + the C-stack.  Every `nix::Value` lives in a Boehm-managed cell.  When a nix::Value goes unreferenced, Boehm's mark-sweep reclaims it on the next GC.

**Decomposition of TW peak RSS:**
* Boehm heap (committed) = `GC_get_heap_size()` — cleanly counted
* Everything else = `peak_rss - boehm_heap` — implicit "everything else" bucket
* No layered accounting required; one allocator, one number.

### 1.2 v3 — five-layer stack

```
nix CLI process
├── code                                  resident, OS-counted
├── libc malloc heap                      C++ stdlib (eval state, store paths)
├── Boehm GC heap                         ── TW interop + bridge tables
│                                            + traceable_allocator pools
│                                            + (legacy) primops.cc state
│                                            ── still allocates ~400 MB heap
│                                              (mostly free per probe)
├── threadArena (v3 arena, alloc.hh)      ── v3 cells (Closure, Thunk,
│                                            Bindings, ListVec, ValuePair,
│                                            allocChars strings)
│                                            16 MB blocks via calloc; never
│                                            released by default; registered
│                                            with Boehm via GC_add_roots
├── threadNursery (nursery.hh)             ── Y + S generational regions
│                                            (Phase E, opt-in default-OFF)
│                                            mmap'd separately; GC_add_roots'd
└── ImportCache deque + results            ── parsed CompilationUnits (the
                                              bytecode for each imported .nix)
                                              + cached Value results from
                                              builtins.import
                                              libc malloc'd; uncapped lifetime
```

The arena handles all "hot" v3 allocations.  Boehm is downgraded to a "leaf" role: only TW-interop values, bridge tables, fakeClo wrappers, and any traceable-allocator-tagged pool live there.

---

## 2. v3 allocators in detail

### 2.1 `threadArena` (alloc.hh)

* **Block layout:** 16 MB `calloc`'d chunks (`Arena::kBlockSize = 16 * (1 << 20)`).
* **Huge cutoff:** allocations larger than `kBlockSize / 4 = 4 MB` get their own dedicated `calloc` (`hugeBlocks`).
* **Block lifetime:** ALL blocks are registered with Boehm via `GC_add_roots` so Boehm scans them for nix::Value pointers (the v3 cells can carry Tag::External / String / Path payloads owned by Boehm).  Blocks are NEVER returned to the OS except by the opt-in Stage 6 whole-block-free path (`releaseBlock`, called from sweep when `Sweeper::tryReleaseBlock` finds a fully-dead block).
* **`bytesAllocated()` semantics:** returns `active_.totalBytes`.  This counter is:
  * INCREMENTED by `kBlockSize` (16 MB) when `refill()` adds a new block
  * INCREMENTED by exact `bytes` for huge allocations
  * **DECREMENTED only when an entire block is freed** (Stage 6 sweep, opt-in)
  * **NOT decremented when individual cells go dead**
  * Therefore: `bytesAllocated()` reports **cumulative virtual mmap'd footprint**, NOT live bytes.

**Critical:** because cell-free does not decrement the counter, on a long eval `v3_arena` reaches a peak and then stays flat (or grows monotonically) for the rest of the process.  This is the **deterministic counter** (σ=0 across N=10 measurements) — but it's deterministic about *virtual mmap'd page count*, not working set.

#### Stage 6 `releaseBlock` path

`Arena::releaseBlock(blockStart)` (alloc.hh:1675-1745) is the only path that decrements `totalBytes`:
1. Removes the block from `active_.blocks` + `cellStarts` + `lineMarks` parallel arrays.
2. Calls `GC_remove_roots(blockStart, blockStart + kBlockSize)`.
3. `std::free(blk)` — returns the pages to libc.
4. Decrements `totalBytes -= kBlockSize`.

This is called from `mark_sweep.cc::Sweeper::tryReleaseBlock` (commits Phase 2-3 of Stage 6) **only when**:
* `NIX_V3_MAJOR_GC=1` is set (Stage 6 opt-in)
* AND `Sweeper::blockLiveCells == 0`
* AND `BitmapMarker::anyMarkInRange(blockStart, 0, blockUsedBytes) == false`

Default state (Stage 6 OFF): no release path active.  `v3_arena` is monotonic-nondecreasing.

### 2.2 `threadNursery` (nursery.hh, Phase E)

* **Default:** OFF (`NIX_V3_PHASE_E=1` to enable; Phase E v0.2 landed but stayed opt-in).
* Two-region (Y young + double-buffered S survivor) when active.
* Size: `nstats.sizeBytes` (default ~14 MB Y; same again for S × 2 = 42 MB total when active).
* **Allocated via `calloc`** at thread init; registered with Boehm via `GC_add_roots(base, base + sizeBytes)`.
* **NOT counted in `v3_arena.bytesAllocated()`** — different mmap.
* **NOT counted in `GC_get_heap_size()`** — registered as roots but not OWNED by Boehm.
* Counted ONLY in the elsewhere-probe (`nurseryBytes = nstats.sizeBytes + 2 * sizeBytes if Phase E`).

### 2.3 Boehm GC (v3 leaf-only role)

* Used for:
  * `nix::Value *` produced by TW (when bridging via `__v3_call_bridge_1` / `__v3_force_attr` / `__v3_force_list_elem`).
  * `v3BridgeClosures` / `v3BridgeAttrs` / `v3BridgeLists` tables (primops.cc:3951+).  These are `unordered_map<BridgeId, BridgeEntry, traceable_allocator>` — storage routed into a Boehm-scanned region.
  * Bridge-root registry (`bridge_root_registry.cc`) — side-table of `(curStart, curEnd)` ranges that hold v3 Value handles.
  * fakeClo pool (post-`#875` wire-back) — held in pool with Boehm-managed entries.
* **Heap behaviour:**
  * Default Boehm tunables (free_space_divisor=3 etc.)
  * `boehm_heap = GC_get_heap_size()` — total bytes owned by Boehm
  * `boehm_free = GC_get_free_bytes()` — bytes available in freelists
  * `boehm_unmapped = GC_get_unmapped_bytes()` — pages returned to OS (on macOS aarch64 this is consistently **0** per `BOEHM_TUNING_FALSIFIED_2026-05-27`)
* **Measurement reality:** `boehm_heap` is ~400 MB on tiny workloads (`1 + 2` reports 402.9 MB) — Boehm's initial heap is large.  `boehm_free` ≈ `boehm_heap` because v3 barely uses Boehm in the hot path.

### 2.4 ImportCache (primops.cc:8103-8115)

```cpp
struct ImportCache {
    std::deque<CompilationUnit> cus;       // stable addresses
    std::unordered_map<std::string, ImportCacheEntry> results;
    uint64_t accessCounter = 0;
};
```

* `cus` holds parsed bytecode (`std::vector<Instruction>` + symbol tables + lambda descriptors + IC caches) **per imported file**.  These are `libc malloc'd` via std::vector — appear in `peak_rss` but not in any v3 counter.
* `results` caches the evaluated `Value` per import.  The Value's pointee (Bindings, etc.) IS in the v3 arena — counted by `v3_arena`.  But the `unordered_map`'s buckets + Value headers + ImportCacheEntry struct itself are libc-malloc'd.
* Phase 4b LRU eviction (`NIX_V3_IMPORT_CACHE_MAX_ENTRIES`) was **falsified** as an HNE lever (per `EXIT_PHASE_4B_LRU_FALSIFIED_2026-05-30`) and stays default-disabled.

**Per `HNE_BUCKET_DECOMP_2026-05-27 §"v3-direct memory: elsewhere"`:**
* HNE `elsewhere` = ~990 MB
* Of which: ~700 MB attributed to ImportCache (cus + results)
* ~270 MB to SQLite cache
* Remainder to libstore / libutil state

### 2.5 Stage 6 mark-sweep (mark_sweep.cc)

* **`runMajorMarkSweep(VMState & vm)`** — the production-GC entrypoint.
* **Trigger:** `vm.cc:2917` calls it when arena bytes-allocated exceeds an opt-in threshold.  Gate: `NIX_V3_MAJOR_GC=1`.
* **Phase 1 (mark):**
  1. Clear all per-block line marks (`Arena::clearAllLineMarks`).
  2. `BitmapMarker marker(arena)` — allocates per-block bitmap (`bits[kBlockSize / 1024]` u64 words = 128 KB per 16 MB block).
  3. `MarkVisitor visitor(marker); visitor.setArena(arena);`
  4. `walkAllV3Roots(vm, visitor)` — precise-root walk (see §3).
  5. `visitor.drain()` — drain the typed worklist (Closure / Thunk / Bindings / ListVec / ValuePair).
  6. `visitor.drainConservative(arena, arenaMin, arenaMax)` — for interior pointers found during precise mark, scan the containing cell's bytes for sub-pointers.
  7. `walkCStackConservative` — Boehm-style scan of the current thread's C stack + callee-saved registers via setjmp spill.
* **Phase 2 (sweep):** iterate cell-starts per block via the `cellStarts` bitmap; classify live vs dead; build per-size-class free lists.
* **Phase 3 (free-list reuse):** allocator slow path consults free lists when bump alloc would advance the block frontier.  Opt-in via `V3_DBG_FREELIST_REUSE=1`.
* **Whole-block-free:** if all cells in a block are dead, `Sweeper::tryReleaseBlock` returns the block.

**Default state on a normal run:** Stage 6 is OFF.  `runMajorMarkSweep` does not fire.  The precise-mark machinery exists but isn't exercised.

### 2.6 Phase D barriers (alloc.hh, barrier.hh)

Phase D — inter-generational write barriers — is **default-ON** since `c0911aee6` (per `EXIT_GC_SPIRAL_PLAN §4.3` clause amendment).  Barriers feed:
* `dirtyContainers` — list of containers (Bindings / Pair / Thunk) that received a write since the last nursery scavenge.
* `cellOwnerTable` — maps cells → owning-Thunk (used by `evaluated` slot tracking).

Both side-tables are libc malloc'd; both contribute to `elsewhere`.

But: `cellOwnerTable` is **EMPTY** in the default state because the table is populated only when the cell-write barrier hits a Phase E-active configuration.  Tested via `NIX_VM_STATS=1` (cellOwnerTable: 0 buckets).  Documented as "metadata" in `precise_root.cc:169-176` — not a root source.

---

## 3. Precise-root walk (precise_root.cc::walkAllV3Roots)

The precise root set covers nine sources:

1. **Primary VMState** — `valueStack`, `withStack`, `frames` (each frame's `closure`, `thunk`, `forceWriteTarget`).
2. **Secondary VMStates** — every active VMState on the thread (via `activeVMStack()`).
3. **Standalone cell roots** — singleton Values registered via `registerStandaloneCellRoot`.
4. **Singleton closure registry** — addresses of `LambdaDescriptor::cachedSingletonClosure` slots holding arena Closure*.
5. **FFI bridge tables** — `v3BridgeClosures` / `v3BridgeAttrs` / `v3BridgeLists`.
6. **Import cache roots** — every `ImportCacheEntry::result` Value.
7. **C++-stack roots** — RAII-registered `GcRoot` instances.
8. **(Mark-sweep only)** post-precise conservative scan of arena interior + C-stack.
9. **(Mark-sweep only)** Phase D `dirtyContainers` walked at the END of mark (see gc.cc:1411+).

#### What's INTENTIONALLY excluded

* `cellOwnerTable` — metadata; cells already reachable via other roots.
* `drvHashCacheMap` — values are byte blobs (serialised), no live v3 pointers.

#### What's POTENTIALLY missing (audit concern)

* **`bytecodePrimopReplacementMap`** (bytecode_primops.cc) — `walkBytecodePrimopRoots` exists and is called from `postScavengeAudit` (gc.cc:1377) but **NOT from `walkAllV3Roots`**.  Comment in `walkAllV3Roots` doesn't address it.  This could be an oversight.
* **`vBuiltins`** singleton — `walkBuiltinsRoot` exists, called by audit but not `walkAllV3Roots`.  May be reachable via VMState frames; needs verification.
* **`callFlakeRoot`** — same pattern as vBuiltins.
* **`deepForceRoots`** — same.

If any of these hold a real cell pointer not reachable through another source, the mark phase undermarks → sweep frees a live cell → next force crashes.  The Stage 6 SHIP-gate falsification doc (`STAGE_6_FALSIFIERS_RESULT_2026-05-29 §F2`) doesn't audit this exhaustively; the OP_REC_BINDING_SLOT_REF crash mentioned in `EXIT_WEEK3_DIAGNOSTIC_PIVOT §1.1` is consistent with an under-marked root.

---

## 4. Memory-accounting formula audit — the headline issue

### 4.1 The published formula

```cpp
// run.cc:680-706
size_t rssBytes = ru_maxrss * (1024 on Linux | 1 on macOS);
size_t boehmHeap = GC_get_heap_size();
const size_t arenaPin = threadArena().bytesAllocated();
const size_t elsewhere = (rssBytes > boehmHeap + arenaPin)
    ? rssBytes - boehmHeap - arenaPin : 0;
```

Reported line:
```
v3-direct memory: peak_rss=X.X MB boehm_heap=Y.Y MB boehm_free=Z.Z MB
  boehm_unmapped=W.W MB v3_arena=A.A MB elsewhere=E.E MB
```

### 4.2 The flaws

#### Flaw 1 — mixing PEAK with INSTANT

`ru_maxrss` is **monotonic-max-RSS across process lifetime**.  It reports the highest resident-set size at any point.

`GC_get_heap_size()` and `threadArena().bytesAllocated()` are **instant snapshots at end-of-eval**.

The formula `peak - boehm_instant - arena_instant` is comparing dissimilar timescales.  Consider:
* Mid-eval: arena grows to 5 GB resident, boehm 400 MB.  `peak_rss` advances to ≥ 5.4 GB.
* Late-eval: arena pages get evicted by macOS (working set shrinks); arena_instant still reports 5 GB (cumulative).
* End-of-eval: peak_rss = 5.4 GB (highest ever), arena = 5 GB (cumulative virtual), boehm = 400 MB.
* `elsewhere = max(0, 5400 - 400 - 5000) = 0`.

But the **actual elsewhere working set at the peak moment** could have been hundreds of MB.

#### Flaw 2 — arena counter is VIRTUAL mmap, not RESIDENT

The arena's `bytesAllocated()` counts pages that the arena has `calloc`'d (which on macOS triggers virtual address reservation but lazy zero-fill).  After a cell becomes dead and the page is not re-touched, macOS evicts the page.  The OS-reported RSS drops; the arena counter does not.

Consequence: on workloads with significant churn in the arena (M5), `arena_pin > peak_rss` is possible and observed:
* M5 measurement today: `arena_pin = 5838 MB`, `peak_rss = 3836 MB`.  Δ = +2002 MB.
* Formula yields `elsewhere = max(0, 3836 - 403 - 5838) = max(0, -2405) = 0`.
* **The clamp silently destroys the signal**: we can't tell whether the real "elsewhere" is 50 MB or 500 MB.

#### Flaw 3 — Boehm reports VIRTUAL too

`GC_get_heap_size()` returns the size of Boehm's heap pool (virtual reservation), not resident.  On a fresh `1+2` eval, Boehm reports 402.9 MB but `peak_rss = 55.6 MB` — Boehm's heap is mostly faulted-out at the moment of measurement.  Same dual-accounting issue as arena.

#### Flaw 4 — boehm_free is mostly never material to RSS

`boehm_free` is a freelist bookkeeping number.  Free chunks within Boehm's heap don't release RSS until Boehm decides to unmap them.  On macOS aarch64 with the default tuning, `boehm_unmapped = 0` always — no release happens.  Reporting `boehm_free` is informational but doesn't decompose RSS.

### 4.3 What an HONEST accounting would say

Three options, in order of effort:

**Option A — same-timescale snapshots (cheap, ~2 hours impl)**

Replace `ru_maxrss` with a CURRENT RSS measurement (read `/proc/self/statm` on Linux or `mach_task_basic_info` on macOS at end-of-eval).  Then `peak_now ≈ boehm_now + arena_now + everything_else_now` is meaningful at the same instant.

Keep `ru_maxrss` for the "peak ever" line but DON'T use it in a subtractive formula.

**Option B — resident-only arena counter (~1 day impl)**

For each arena block, sample `mincore(blk, kBlockSize, vec)` at the end of eval; sum resident pages → `arena_resident`.  Replace `arenaPin` in the formula with `arena_resident`.

This is what the user's directive 2026-05-29 ("don't rely on OS's RSS values") implicitly asks for.  It separates VIRTUAL (cumulative cells) from RESIDENT (working set).

**Option C — full mid-eval time-series (DIAG-3 / DIAG-4 — already wired)**

Per `EXIT_WEEK3_DIAGNOSTIC_PIVOT §2`:
* DIAG-3: per-Tag L(t) time-series (in place, `NIX_V3_LIVE_TRACE_PERIODIC=K`)
* DIAG-4: phase decomposition counters (in place, run.cc:191-408)

These give working-set-over-time at v3-counter resolution.  The headline end-of-eval RSS becomes one column in the broader picture, not the sole truth.

### 4.4 Recommendation

The current line is **safe to ship but misleading on M5-class workloads where arena ≥ peak**.  Decisions based on `elsewhere = 0` at end-of-eval should be treated with skepticism.  The arena counter is decision-quality (deterministic, σ=0) but only for *virtual cumulative bytes*, not for working-set RSS.

Per `[[memory-first-class]]` §"Operationalization": the bench harness should report BOTH `arena_cumulative` AND `arena_resident` (Option B); the SHIP-gate thresholds should be evaluated against `arena_resident`, not `arena_cumulative`.

---

## 5. The `elsewhere-probe` audit

The probe (run.cc:1289-1399) enumerates per-container side-tables and estimates their byte footprint.  Let me audit each entry.

| Component | Sample value (1+2 eval) | What it actually counts | Honesty |
|---|---|---|---|
| `stringContextSideTable` | 0 buckets | hash buckets + entry nodes + string bodies | OK, but body bytes computed via `s.size()` — misses small-string-optimization overhead and per-string allocation rounding |
| `posSnapshotPool` | 161 entries, 256 cap | `capacity * sizeof(PosSnapshot) + entries * 40` | UNDERCOUNTS — string body is approximated at 40 B/entry; real `/nix/store/...` paths can be 80-120 B |
| `bindingsOriginTable` | 0 buckets | hash buckets + entry nodes | Only populated under `NIX_V3_DBG_BINDINGS_ORIGIN=1` — typically 0 |
| `cellOwnerTable` | 0 buckets | hash buckets + entry nodes | Effectively dead in default mode; legitimate 0 |
| `globalSymbolTable` | 224 entries, 256 cap | vector header + string bodies + index map | OK; uses `capacity * sizeof(std::string)` which captures SBO overhead |
| `dirtyContainers` | 0 entries, 0 cap | `capacity * 2 * sizeof(void*)` | UNDERCOUNTS — DirtyEntry is a struct (`{kind, ptr}`); should be `capacity * sizeof(DirtyEntry)` |
| `standaloneCellRoots` | 0 entries | `capacity * sizeof(void*)` | OK |
| `nursery` | `<mmap>` | `sizeBytes` or `3 * sizeBytes` if Phase E | OK |
| `singletonClosureReg` | 14 entries, 16 cap | `capacity * sizeof(Closure**)` | OK |
| `arena.cellStarts` | `<phase3>` | sum of per-block bitmap capacities (128 KB per block when MAJOR_GC=1) | Gated on Stage 6; 0 in default mode (correct) |
| `arena.freeList` | 0 entries | `count * 2 * sizeof(void*)` | OK but only meaningful when sweep ran |

**What the probe SHOULD also count (but DOESN'T):**

| Missing | Estimated HNE contribution | Why missing |
|---|---|---|
| ImportCache.cus (CompilationUnit deque) | **~500-700 MB** | No accessor; CU size not summed |
| ImportCache.results bookkeeping | tens of MB | unordered_map overhead not counted (Values are in arena) |
| TW-side state (parser, store paths) | hundreds of MB | TW's `nix::EvalState` etc. live in Boehm or libc malloc |
| SQLite cache (`v3-bytecode-v3.sqlite`) | ~200-300 MB | mmap'd; not visible to v3 |
| Bridge-root registry curStart/curEnd ranges | ~MB | side-table doesn't account |
| `v3BridgeClosures` / `v3BridgeAttrs` / `v3BridgeLists` table buckets | ~MB | traceable_allocator → in boehm_heap, but probe doesn't separate |
| `attrSelectCache` / `recSlotIC` / IC tables per CompilationUnit | MB-scale per CU | inside CompilationUnit; same issue as the deque |
| C++ `std::deque` / `std::vector` overhead in eval-state | unknown | not counted anywhere |

**Bottom line:** the elsewhere-probe accounts for **the v3-specific side-tables** but doesn't address the **dominant elsewhere contributor — ImportCache CompilationUnits**.  Per `HNE_BUCKET_DECOMP`, ~700 MB of the 990 MB HNE elsewhere bucket is unaccounted by the probe.

---

## 6. The M5 anomaly: arena > peak_rss

Today's M5 measurement crystallises the accounting issue:

| Metric | Value | What it means |
|---|---|---|
| `peak_rss` | 3836.82 ± 380.67 MB | Max RSS over process lifetime |
| `v3_arena` | 5838.50 ± 0.0 MB | Total cumulative mmap'd arena pages (16 MB blocks × N + huge allocs) |
| `boehm_heap` | ~403 MB | Boehm reserved pool |
| `elsewhere` (formula) | 0 MB | Clamped because peak < arena + boehm |
| `wall` | 137.92 ± 1.52 s | |

**The arena VIRTUAL footprint exceeds resident RSS by 2002 MB.**  This is consistent with:

* M5 allocates 5838 MB of arena cells over the lifetime of the eval.
* macOS aggressively pages out arena blocks where cells have gone dead.
* At the peak moment, resident arena was at most 3836 MB minus (boehm_resident + nursery_resident + everything_else).

The formula reports `elsewhere = 0` not because there IS no elsewhere — there obviously is (CompilationUnits, libstore, etc.) — but because we **don't know the arena-resident-at-peak number**.

#### Implication for the watchdog

* The watchdog evaluates `peak_rss`.  Today's M5 trim-2-mean is 3836 MB; watchdog target is 4096 MB → **+259 MB under**.
* But the underlying arena allocation pattern of **5838 MB cumulative** means: if the OS page-eviction policy ever throttles (memory pressure on the host), the working set could push closer to the cumulative number, blowing through the watchdog.
* The σ on M5 peak is 380 MB — already wide enough that 3/10 individual runs exceed 4096 MB.

The watchdog passing at the trim-2 mean depends on the OS doing aggressive page-eviction for arena pages with dead cells.  This is **not under v3's control** and could be unstable across hosts / loads.

---

## 7. TW vs v3 comparison surface

| Question | TW | v3 |
|---|---|---|
| Allocator count | 1 (Boehm) | 3 (Boehm + arena + nursery) + opt-in (Stage 6 free-lists) |
| Authoritative bytes-counter | `GC_get_heap_size()` | None (no single counter is authoritative for live bytes) |
| Page-release mechanism | Boehm `gcollect_and_unmap` + freelist coalescing | None by default; Stage 6 whole-block-free is opt-in |
| Working-set vs cumulative | Boehm's heap can shrink (after collect-and-unmap) | Arena monotonic-grow by design |
| Headline RSS decomp accuracy | high (`peak - boehm = everything_else`) | poor on M5-class (arena > peak; formula clamps) |
| Reproducibility of bytes | high (Boehm semantics well-known) | high for arena (deterministic) but doesn't translate to RSS |
| Eval-cache state | minimal (nix::EvalState only) | substantial (ImportCache + disk_cache + bytecode SQLite) |

**Why v3's accounting is harder than TW's:**

1. TW has one allocator → one number.  v3 has three, plus libc-malloc state, plus the OS paging layer to reconcile.
2. TW's Boehm is well-tuned and stable across measurement sessions.  v3's arena counter is deterministic on cumulative bytes but uninformative on working set.
3. The "v3-native" goal (V3-NATIVE rule per CLAUDE.md §1.1) inherently MOVES allocations from Boehm to the arena, making the arena dominate → magnifying the arena-vs-resident discrepancy.

**What v3 wins on:**

* Arena-cumulative is a deterministic, ship-gate-quality counter for code-side allocation behaviour.  σ=0 across runs means changes in allocation count are visible regardless of OS noise.
* The TW comparison is honest at end-of-eval if we agree both report `ru_maxrss` (peak ever) — Tag::App3 +32 MB on M5 at 0.065σ vs TW is comparable apples-to-apples even if our internal decomposition is wonky.

---

## 8. Are we overcounting? Are we undercounting?

The user's question.  Let me answer it precisely.

### 8.1 Overcounting risks

* **Arena pages counted as "v3 memory" but page-evicted** → reported `v3_arena` *overstates resident v3 memory*.  This is the M5 5838 vs 3836 discrepancy.
* **Boehm heap reserved virtual not all faulted in** → reported `boehm_heap` *overstates resident Boehm memory*.  Less material because Boehm tunable behaviour is stable.

### 8.2 Undercounting risks

* **Elsewhere clamp to 0 when arena > peak** → loses ALL signal about libstore / CompilationUnit / SQLite.  Material on M5 today (~5838 - 3836 = 2002 MB of mystery).
* **Elsewhere-probe missing ImportCache CompilationUnit deque** → underestimates elsewhere by ~500-700 MB on HNE.
* **No accounting for SQLite disk-cache mmap'd pages** → underestimates by ~200-300 MB.
* **Bridge-table buckets (traceable_allocator) live in Boehm heap but probe doesn't break them out** → bridge tables are counted but invisible.
* **Phase 6 cell-starts bitmap and mark-sweep bitmaps allocated transiently during GC** → not counted in any persistent measurement (correct, but worth noting).

### 8.3 Net direction

**On workloads where arena dominates (M5, full nixpkgs), we OVERCOUNT arena and UNDERCOUNT elsewhere, with the overcounting fully cancelling out elsewhere in the formula.**

On workloads where arena fits comfortably below peak (HNE today: arena 1476, peak 2928), the formula works directionally:
* `elsewhere = 2928 - 403 - 1476 = 1049 MB` — matches the probe's ~7 MB stringContextSide + ~700 MB ImportCache (unmeasured but documented in HNE_BUCKET_DECOMP) + ~270 MB SQLite + ~70 MB other.  Order of magnitude correct.

The decomposition is **directionally accurate on HNE-like workloads** but **silently breaks on arena-dominant workloads (M5)**.

---

## 9. Comparing v3 commits with this lens

Re-reading the session-arc claims:

* **Tag::App3 measurement (HNE)**: arena went 1493 → 1476 MB (-16.8 MB).  This is real and decision-quality (deterministic counter).
* **Tag::App3 measurement (M5 today)**: arena went 5570 → 5838 MB (+268 MB).  Also real and deterministic.  But it's a CUMULATIVE virtual increase — the working set may or may not have changed.
* **fakeClo wire-back claim "-704 MB M5 arena"**: arena counter delta is honest as a cumulative measurement.  Whether peak RSS also dropped by 704 MB is a separate question that the formula's clamp obscures.
* **HNE peak σ=1.4 MB** under current measurement is genuinely tight — but that tightness applies only to peak_rss; the resident decomposition is no tighter.

In short: **arena deltas are decision-quality for "did we allocate fewer cells?".  Peak deltas are decision-quality for "did the working set get smaller?".  The two should NOT be treated interchangeably**, which is what the headline formula effectively does.

---

## 10. Recommendations

### 10.1 Short-term (this session)

* Stop reporting `elsewhere` as a single number when `peak_rss < boehm + arena`.  Emit a diagnostic line:
  ```
  v3-direct memory: peak_rss=3836.8MB boehm_heap=403.0MB v3_arena_virtual=5838.5MB
    arena>peak: working-set decomposition unavailable
  ```
* Add a "v3_arena_resident" estimate via `mincore` sampling — one-time pass at end-of-eval.  Cost ~1-2 ms.  Gives the missing resident number.

### 10.2 Medium-term (1-2 sessions)

* Move headline RSS reporting from `ru_maxrss` (peak ever) to **end-of-eval CURRENT RSS** for the decomposition formula.  Keep peak as a separate line.
* Audit `walkAllV3Roots` for the missing root sources (`bytecodePrimopReplacementMap`, `vBuiltins`, `callFlakeRoot`, `deepForceRoots`) — confirm they're reachable via other paths OR add them.
* Extend the elsewhere-probe to enumerate `importCache().cus` and report sum-of-CompilationUnit-sizes.  Closes the largest gap.

### 10.3 Long-term (multi-session, after Stage 6 default-on)

* When Stage 6 mark-sweep ships default-on, the arena's `bytesAllocated()` becomes a meaningful working-set proxy (whole-block-free returns pages).  Re-evaluate the formula at that point.
* Per `[[memory-first-class]]`: bench harness should emit BOTH wall AND peak-RSS AND arena-resident AND arena-virtual deltas.  SHIP-gate thresholds need explicit which-counter.

### 10.4 What NOT to do

* Don't remove the formula — it's directionally useful on HNE-class workloads and provides backwards comparability with the 2026-05-{27,28,29,30} baselines.
* Don't reintroduce `NIX_V3_NO_DISK_CACHE` as a normal-mode env var — it's a measurement aid, not a production gate.
* Don't claim "M5 watchdog passed" without acknowledging the arena-resident question.  The trim-2 mean is honest as a *number*, but the *mechanism* (OS page-eviction absorbing arena growth) is not under our control.

---

## 11. What this audit kills

Per `[[falsification-rule]]`:

* **Claim: "v3_arena is a working-set measurement"** — KILLED.  It's a cumulative virtual mmap counter.
* **Claim: "elsewhere = 0 means there's no non-arena memory pressure"** — KILLED.  The clamp obscures the real elsewhere on arena-dominant workloads.
* **Claim: "v3 has lower memory footprint than TW because v3_arena + boehm_heap < TW's boehm_heap"** — REFUTED.  v3 has arena+boehm+nursery+ImportCache(libc)+SQLite(mmap); TW has boehm+libc-stdlib.  Like-for-like comparison is `peak_rss` to `peak_rss`, full stop.
* **Claim: "fakeClo wire-back saved 704 MB on M5 peak"** — PARTIALLY KILLED.  It saved 704 MB on M5 cumulative arena.  M5 PEAK delta was claimed at -219 MB per `EXIT_WEEK1_RETROSPECTIVE`, which IS within σ=308 noise.  The arena delta is decision-quality; the peak delta is not.

## 12. What this audit doesn't kill (preserves)

* Tag::App3 HNE peak win (-29 MB) — measured with current measurement methodology; reproducible.  KEEP verdict stands.
* HNE arena-cumulative win across session arc (-1661 MB) — deterministic; real.
* Stage 6 mark-sweep mechanism (mark + sweep + whole-block-free) — correct in design; opt-in by default.
* Precise root walker (walkAllV3Roots) — covers nine sources; comment trail explains exclusions.

---

## 13. Cross-references

* `EXIT_POST_APP3_BASELINE_2026-05-31.md` — the measurement that exposed M5's arena > peak.
* `EXIT_WEEK3_DIAGNOSTIC_PIVOT_2026-05-29.md` — explicit "OS RSS is a proxy" stance; informed this audit.
* `HNE_BUCKET_DECOMP_2026-05-27.md` — elsewhere = ImportCache + SQLite + other attribution.
* `STAGE_6_PRECISE_GC_DESIGN_2026-05-27.md` — Stage 6 design context.
* `STAGE_6_IMPLEMENTATION_GUIDE_2026-05-27.md` — Stage 6 phase plan.
* `BOEHM_TUNING_FALSIFIED_2026-05-27.md` — Boehm tuning baseline.
* `GC_PRECISE_ROOT_FOUNDATION_2026-05-27.md` — the precise-root architecture.
* `EXIT_WEEK3_DECISION_2026-05-29.md` §5.3 — M5 σ environmental notes.
* `precise_root.cc::walkAllV3Roots` — the precise root walker.
* `run.cc:680-706` — the headline RSS-decomposition formula.
* `run.cc:1289-1399` — the elsewhere-probe.
* `mark_sweep.cc::runMajorMarkSweep` — Stage 6 entrypoint.
* `alloc.hh:1769` — `bytesAllocated()` semantics (cumulative).
* `alloc.hh:1675-1745` — `releaseBlock` whole-block-free.
* `[[falsification-rule]]` — Rule 0.
* `[[measure-twice-cut-once]]` — methodology framing.
* `[[memory-first-class]]` — bench-harness counters expectation.

---

*Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group. SPDX-License-Identifier: Apache-2.0.*
