# Bounded-memory v3 eval — engineering plan + scope (2026-06-29)

GOAL: make v3's RSS = `live-set + CAPPED overhead`, workload-independent (a hard ceiling the
eval stays under), accepting modestly higher CPU. NOT "beat TW's peak" (the failed campaign) —
"bound the overhead the campaign proved we hold needlessly." Pre-commit a per-workload RSS
ceiling; every step has a byte-id + `--brute` + measured-bound gate.

## Investigated foundations (this is what the plan is built on)

- `desc->cu` (closure.hh:554, read by thunkCU closure.hh:561) is LOAD-BEARING — the thunk→CU
  link; the LambdaDescriptor lives *inside* `cu.lambdas[]`, so a live thunk physically pins its
  CU. Closures pin via `c->cu` (closure.hh:67); frames via `fr.cu` (vm.hh:68). Not removable.
- CUs are a REFERENCE GRAPH: a closure compiled in CU-A can be retained in CU-B's import
  result / the final value (primops.cc:7018 caches arbitrary Values incl. closures). ⇒ CU
  eviction = mark reachable thunks/closures (the GC walk gc.cc:981–994 already does this) →
  free unreferenced CUs. Not a per-file refcount.
- Storage: `importCache().cus` is `std::deque<CompilationUnit>` (primops.cc:6420), raw
  pointers, APPEND-ONLY, never evicted. Middle-erase invalidates other CUs' addresses ⇒
  freeing a CU needs `deque<unique_ptr<CU>>` (reset the ptr; slot+addresses stable) or per-CU
  mmap (munmap on evict → guaranteed RSS return).
- TWO independent "no return to OS" facts: (1) the ARENA never munmaps — kills arena reclaim
  (the wall); (2) BOEHM's ~403MB is reserved-free, pinned ONLY because the arena is a Boehm
  root (GC_add_roots, alloc.hh) → conservative scan pins free pages → fixable by arena
  DEREGISTRATION, independent of (1). CU bytecode is plain malloc — neither wall applies.

## Bucket map (M5, fresh decomp) + boundability

| bucket | M5 | wall? | lever |
|---|---|---|---|
| arena live | ~592MB | floor | leaner cells (foundational, out of scope) |
| arena dead | ~970MB | YES (no munmap) | hard-cap free-list / file-backed arena (M3) |
| CU bytecode (malloc) | large (IFD) | no | evict cold CUs + reload (M2) |
| SQLite page cache | ~100–270MB | no | cache_size pragma (M1.B) |
| Boehm reservation | ~403MB | no | arena deregistration (M1.A) |
| LambdaDescriptor diag | ~75MB | no | drop name/counters (M1.C) |
| posSnapshot/symtab/etc | ~70MB | — | correctness-needed, keep |

## Phases (sequenced by value/risk; measure-first gates kill weak levers early)

### M0 — Measurement foundation (no behavior change)
- **M0.1** Unified RSS-decomposition report under NIX_VM_STATS: one table — maxRSS = arena
  (total/live/dead) + CU-bytecode (importCacheBytecodeBytes) + SQLite (approxResidentBytes) +
  Boehm (heap/free) + elsewhere-probe + residual. GATE: instrument-only byte-id; sums
  reconcile to maxRSS within stated fragmentation; hello/git/firefox.
- **M0.2** Baseline + ceilings: run M0.1 on firefox/M5/HNE/simplex (darwin-4); record
  per-bucket baseline; COMMIT a pre-committed RSS ceiling per workload. GATE: table git-noted.

### M1 — Non-arena bounding (un-walled; low–med risk; recovers ~750MB; every workload)
- **M1.A.1** Audit Boehm ownership (Tag::External/String/Path FFI Values) vs arena-as-root
  false pin (reuse ARENA_DEREGISTRATION_DESIGN). GATE: audit doc; FFI-owned set small +
  separately scannable.
- **M1.A.2** Replace arena `GC_add_roots` with a bridge-source side-table (register only real
  nix::Value* bridges). GATE: byte-id + `--brute`; Boehm no longer scans the arena.
- **M1.A.3** Measure Boehm reservation drop (darwin-4). GATE: boehm resident ↓ toward FFI
  working set (target: recover most of 403MB); auditor zero missed-root; CPU neutral-or-better.
- **M1.B.1** Expose `PRAGMA cache_size`(+mmap_size) on the disk_cache connection (NIX_V3_DISK_CACHE_MB).
  GATE: byte-id; pragma verified via sqlite3_db_status.
- **M1.B.2** Measure SQLite resident vs cap + CPU. GATE: SQLite resident capped; CPU ≤5%.
- **M1.C.1** Move LambdaDescriptor `name`/`contextualName` to a side-table; gate the 3 mutable
  counters off-by-default (#139: ~38% diagnostic). GATE: byte-id + `--brute`; diagnostics
  reachable under V3_DBG.
- **M1.C.2** Measure descriptor/CU footprint drop. GATE: descriptor bytes ↓ (~38% of bucket).

### M2 — CU bytecode eviction (headline, un-walled; HIGH risk; gated on M2.1)
- **M2.1** CU-liveness INSTRUMENT (no eviction): at a mid-eval safepoint, mark reachable
  thunks/closures/frames (reuse GC mark) → referenced-CU set → report "cold CU bytes"
  (unreferenced CUs whose results are data-only). SIZES the realizable win.
  GATE (GO/NO-GO): cold-CU MB per workload; proceed only if ≥150MB cold on M5/HNE, else STOP.
- **M2.2** Evictable CU storage: `deque<CompilationUnit>` → `deque<unique_ptr<CompilationUnit>>`
  (stable .get() addresses; free = reset). GATE: byte-id + `--brute` (transparent; no eviction yet).
- **M2.3** CU mark-sweep + evict + AUDITOR: at the safepoint, free unreferenced cold CUs +
  drop their data-only importCache.results entries (keep entries with live closures). AUDITOR
  (PhD-6-class): brute-scan all live objects for any desc/cu into a to-be-freed CU → must be 0.
  GATE: auditor zero missed-CU-ref across `--brute`; byte-id with eviction ON; repro test
  (evict + re-import same file → byte-id).
- **M2.4** Reload-on-demand + trigger/cap: re-import re-deserializes from the disk cache
  (existing fallback); trigger eviction when CU-bytecode resident > cap. GATE: CU-bytecode
  resident stays under cap; byte-id; CPU (re-deserialize) ≤10%; ACTUAL maxRSS drop measured
  (confirms malloc returns pages — else escalate M2.2 to per-CU mmap+munmap).

### M3 — Arena dead-slack cap (the wall; HIGHEST risk; last)
- **M3.1** Arena hard-cap free-list: refuse to map a new block while a reusable dead cell fits
  (force in-place reuse) + aggressive mid-eval sweep (CPU-for-memory trade). MEASURE whether
  the mapped-block high-water stays bounded to live+slack. GATE: arena resident bounded;
  byte-id + `--brute`; CPU ≤ pre-committed %. HONEST RISK: the high-water is reached at the
  live-set peak regardless of reuse — this may cap only DEAD slack, not the live peak.
- **M3.2** (conditional, radical) IF M3.1 can't bound: file-backed (mmap'd) arena spike — OS
  evicts cold live cells to a backing file; resident bounded to working set at page-fault IO
  cost. GATE: design doc + spike → explicit GO/NO-GO before any production commit (major
  redesign; touches every cell access + the moving GC).

### M4 — Integration + ship
- **M4.1** Unified mode `NIX_V3_RSS_CAP=<MB>` enabling M1+M2(+M3) with the cap. GATE: byte-id + `--brute`.
- **M4.2** darwin-4 head-to-head firefox/M5/HNE/simplex: RSS under cap? CPU cost? vs TW?
  GATE: pre-committed ceiling met at ≤X% CPU across all four.
- **M4.3** nixpkgs byte-id soak + default-flip decision (or document + leave gated).
  GATE: full soak diverge=0; decision recorded.

## RESULT UPDATE (2026-07-02, after M0+M1 execution) — M1 EVAPORATED

Measure-first killed the entire "safe ~750MB M1 tier":
- M0.1/M0.2 DONE: unified RSS-decomp reconciles exactly; baseline + ceilings set (git-noted).
- M1.A (Boehm dereg) MOOT: arena-noroot is ALREADY the default (NIX_V3_ARENA_ROOT inverted);
  darwin-4 A/B shows Boehm 402.9MB identical root-on/off → the 403MB is Boehm-native, unmap
  FALSIFIED on macOS. Nothing to recover via dereg.
- M1.B (SQLite cap) MOOT: disk cache REDUCES peak (firefox cache-ON 615 vs OFF 710); the 326M
  DB is mmap'd/OS-evictable already; in-process SQLite=2MB. Capping would hurt.
- M1.C (descriptor diag) DEFERRED: 87-site refactor for ~10MB, M2-subsumed.
- M1.D (malloc-frag reclaim) FALSIFIED: pressure_relief returned 0MB (system libmalloc, no
  reclaimable frag; nothing frees mid-eval). #139's 360MB was jemalloc-specific.

⇒ The ONLY non-arena lever standing is M2 (CU-bytecode eviction, 149-158MB on M5/HNE) — and
M1.D implies freed malloc may not return to OS, so M2 must use per-CU mmap+munmap. The
dominant memory (arena 1544 M5 + Boehm-reserved ~200) is WALLED/unrecoverable. So bounded
memory BELOW TW requires cracking the arena (M3, the wall); M2 is a modest ~5% side-lever.

REVISED ceilings (M2-only, arena unbounded): firefox ≤600 · M5 ≤2200 · HNE ≤1150 · simplex
≤560. Sub-TW needs M3. Next: M2.1 (measure cold-CU — may itself kill M2 if few CUs are cold).

## Expected outcome (honest)
- M1 alone: ~750MB recovered on every workload (Boehm 403 + SQLite ~270 + diag ~75), low–med
  risk, shippable independently. This is the safe near-term win.
- M1+M2: M5 ~3.1GB → ~2GB (non-arena bounded), IFD workloads benefit most.
- < TW (982MB) additionally requires M3 (arena, the wall) + eventually leaner cells.
- The "fixed memory" property (a hard, workload-independent ceiling) needs M2's cap + M3.

START: M0.1 → M0.2 → M1.A (Boehm dereg = biggest single fixed recovery, every workload) in
parallel with M1.B (SQLite). M2.1 (measure cold-CU) gates the headline lever before any risk.

Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.
SPDX-License-Identifier: Apache-2.0
