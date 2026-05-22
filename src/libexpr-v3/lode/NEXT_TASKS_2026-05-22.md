# Next Tasks — Spec (2026-05-22)

## Premise

The architectural goal is a **pure bytecode VM that is faster and
leaner than TW**.  Today (post-#768) hello.drvPath runs at
**2.43× TW wall-clock**.  The gap is real and the lane to close
it lives entirely on the v3 side.

The user prompt that triggered this spec called out a key
measurement gap: "Do we even measure what our eval performance
is, and how much of it is compilation (which we can cache)?"

#769 just landed the measurement.  See findings + decision below.

## Headline finding (from #769)

On hello.drvPath, 1.25 s wall clock breaks down as:

  * Outer expression compile (V3_TIMING outer dump):  ~0.1 ms (~0 %)
  * **Compile inside primImport (sum across 269 imports): ~540 ms (43 %)**
    * parse:    94 ms
    * lower:    97 ms
    * optimise: 266 ms   ← single biggest sub-phase
    * compile:  82 ms
  * Inner-CU run (from primImport's `run(cu)`):  ~6 ms
  * Outer eval (dispatch + primops not via import): ~700 ms (56 %)
  * Result-cache hits (in-memory, same path):   682 / 951 imports (71 %)
  * Disk cache:                                  hits 269 / 269 when
                                                 enabled but lookup
                                                 overhead is currently
                                                 within-invocation
                                                 negative

**Compile is 43 % of v3 wall on hello.drvPath.**  TW's 535 ms
wall has its own parse + (no bytecode lower / optimise / emit)
+ eval — i.e. TW's compile is ~95 ms (parse only).  v3's compile
costs roughly **445 ms more than TW** because of the IR pipeline.

If compile were free (Stage 9 / disk cache), v3-direct hello.drvPath
would drop to ~0.71 s = **1.33× TW**.  That's the architectural
goal range.  Stage 9 IS the path to "pure VM faster than TW."

## Next tasks (priority order)

### #770 — Disk cache cross-invocation benchmark + stats

The existing disk cache (gated `NIX_V3_DISK_CACHE=1`) was designed
for cross-invocation re-use; we benchmarked it within a single
invocation, which is the wrong use case.  The right measurement:
100 sequential `nix eval` runs over the same expression; the
first run primes the cache, the next 99 should be faster than
no-cache.

Deliverables:

  * Wire `disk_cache::stats()` (hits/misses/inserts) into
    `NIX_VM_STATS` output.
  * Multi-run hyperfine: cold run vs steady-state (n=20).
  * If steady-state shows net win, document and proceed to #771.
  * If still net loss, identify the overhead source (likely
    `serialize::deserializeCU`, currently untimed).

Effort: 1 day.  Falsifier: "the disk cache pays off
cross-invocation."

### #770b — Skip parse on disk-cache hit

The current primImport flow is:
1. `parseExprFromFile` (94 ms)
2. Read file content for cache key hash
3. Disk-cache lookup
4. If hit: deserialize + run (skip lower/optimise/compile)

On a disk-cache hit the parse output is unused — we deserialize
the cached CU instead.  Refactor: compute the cache key BEFORE
parsing (one file read for the hash), lookup, ONLY parse on
miss.  Saves the full 94 ms per import on cache hit.

Effort: 0.5 day.  Combined with #770: hello.drvPath warm-cache
wall could drop to ~700 ms (vs TW 535 ms = 1.31× TW).

Skip-parse + skip-compile is **the closest concrete path to
beating TW on a warm cache**.

### #771 — Promote disk cache to default (gated on #770 + #770b)

Promote `NIX_V3_DISK_CACHE` to default-on if #770 + #770b
confirm net wall-clock win on the cross-invocation benchmark.
Opt-out: `NIX_V3_NO_DISK_CACHE=1`.

Effort: 1 day after #770/#770b land.

### #772 — Stage 9 Phase L0: structural hash on IR

The per-file disk cache (#770/#771) caches whole-file CUs.  It
misses the nixpkgs callPackage-replication structure (5 000+
near-identical closures per LESSONS_LEARNED §4.1).  Stage 9's
content-addressed cell store would catch those.

Phase L0 is the falsifier: implement `structuralHash()` on IR
nodes (BLAKE3 over ir::MkThunk / ir::MkClosure modulo VarId
renaming).  Survey nixpkgs eval: do we see 5×+ cell-level dedup?

  * If yes: Stage 9 has measurable lever, proceed to L1-L4
    (~4 more weeks).
  * If <2×: kill Stage 9, document why per-file disk cache
    is sufficient.

Effort: 1 week.  This is also the prerequisite for Stage 5
(hidden classes — shape interning needs alpha-equivalent
identity).

### #773 — Stage 9 Phase L1: ABT alpha-equivalent identity (de Bruijn)

After L0 confirms cell-level dedup is real, refactor IR to use
de Bruijn `(depth, index)` references instead of named VarIds
for thunk-body identity.  Two thunk bodies that differ only in
VarId numbering hash to the same content.

Effort: 1 week.  Also unblocks Stage 5.

### #774 — Stage 4 v4.4+: cross-fn strictness through higher-order callees

Independent of the compile-cache work.  Current v4.3 hit ~17%
resolveCalleeLambda coverage with 0 elisions on real workloads
because beta-reduce eats simple wrappers.  Extension: propagate
through higher-order callees (mapAttrs, foldl', etc.).  Goal:
reduce v3's 661 K thunk allocations on hello.drvPath (half
wasted per the existing data).

Effort: 1-2 weeks.

## Strategic alignment with ROADMAP_TO_VISION

Looking at the Stage 1-9 sequence:

  * Stage 1: ✅ Action plan complete.
  * Stage 2: ✅ Pure-bytecode eval (NIX_V3_SKIP_INSTALLABLE_PREEVAL
    retired #760-764).
  * Stage 3: 🟡 Phase D landed (#731); Phase E v0.3 (9958eb4a5)
    opt-in; nursery default-OFF.  The roadmap kill criterion
    ("perf < 1.2× TW on canonical") not yet met.  But #767-#768
    show op-count / alloc-traffic reductions don't move wall
    clock; the lever is COMPILE caching (#770-#772), NOT more
    nursery work.
  * Stage 4: ✅ v1-v4.3 landed.  v4.4 (this spec's #774) extends.
  * Stage 5-7: blocked on Stage 9 Phase L1 (ABT identity).
  * **Stage 9: not started — and the data says it's where the
    next big lever lives.**

The 43 % compile share on hello.drvPath says: **Stage 9 (compile
caching) is the highest-impact remaining lever for closing the
TW gap, larger than continuing on Stage 3 / Stage 4 polish.**

### Architectural consistency check

The user flagged a concern in the previous turn when I suggested
moving bytecode-wrapper work into a C++ FFI.  That contradicted
"more compute in the v3 VM."

This spec is architecturally CONSISTENT with "more compute in
the VM":

  * **Stage 9 caches the OUTPUT of the v3 IR pipeline** (bytecode +
    metadata).  It doesn't move compute out of the VM; it ensures
    we don't repeat the work across processes.  The compiled
    bytecode that the VM runs is unchanged.
  * **Skip-parse on cache hit** eliminates work entirely — both
    in v3 and (parse is shared with TW via cppnix parser); pure
    win.
  * **Stage 4 v4.4** reduces allocations BY ANALYSIS in the IR;
    no FFI surface change; pure v3 work.

None of #770-#774 shrinks the v3 VM's compute responsibility.
They make it RUN LESS, but the WORK it does is unchanged.

## Open question for the user

Pick one of:

A. **Start with #770 (disk cache audit + cross-invocation
   benchmark)** — fastest path to a concrete wall-clock
   improvement on hello.drvPath if cache works as designed.

B. **Start with #774 (Stage 4 v4.4 cross-fn strictness)** —
   independent of the compile-cache work; reduces allocations
   directly.  Could compound with #770's compile-cache savings.

C. **Start with #772 (Stage 9 L0 structural hash spike)** —
   foundation move; longer term but unblocks both finer-grained
   compile caching AND Stage 5+.

Default recommendation: **A first** (#770 + #770b: ~1.5 days),
since it directly tests whether the existing infrastructure
already gives us a wall-clock win.  If yes, that's a fast
2-week path to v3-direct beating TW on warm cache.  If no, the
data steers us to #772 or alternative.

## Cross-references

  * `bench/samples/2026-05-22/*` — measurement infra.
  * Commits: 3bbb401ef (#769), 5bbd49628 (#767a), 16b72604a
    (#766a), 2970dbd04 (#765).
  * `ROADMAP_TO_VISION_2026-05-15.md` Stage 9 (~Weeks +n).
  * `LINKING_DESIGN_2026-05-17.md` — full Stage 9 design.
  * `PERF_STRATEGY_2026-05-17.md` — Stages 10-12 candidates.

Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
Input Output Group.  SPDX-License-Identifier: Apache-2.0
