# Bindings allocation attribution — Phase 1 spike (#746)

**Date**: 2026-05-21
**Author**: Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group
**Status**: SPIKE LANDED — data captured; lever decision queued

## Goal

After Stage 4 v4.2 (13a434ea0) shipped the recursive sub-block
cloning machinery for caller-side strictness elision, hello.drvPath
still showed 0 real-world elisions.  The strategic question moved
back to the dominant gap:

  * **#702 RSS decomp** measured 1895 MB peak RSS on hello.drvPath
    (vs TW's 145 MB), with **956 MB / 84 %** of the v3 arena owned by
    Bindings.
  * The data-structure audit (2026-05-21) named "Bindings append-
    overlay" as the multi-GB story but did not pinpoint the call
    site.

Phase 1 (3-5 day spike) was: instrument per-allocation-site
attribution so the next lever (persistent-map overlay, construction-
site inlining, or shape polymorphism) is picked on data, not
intuition.

## What landed

`dumpBindingsAttribution(FILE*, topN)` inline in
`include/v3/alloc.hh`.  Walks the existing `bindingsOriginTable`
(already captures every non-empty `allocBindings` call's file:line
via `__builtin_FILE`/`__builtin_LINE`), aggregates by `source`
label, computes per-origin total bytes + per-size-bucket counts,
sorts by total bytes descending, prints top-N.

Wired into `run.cc`'s `NIX_VM_STATS` block.  Single env-var gate
`NIX_V3_BINDINGS_ATTR=1` (also auto-enables recording via
`bindingsOriginEnabled()`).  Zero cost when off; ~hundreds-of-ns
+ ~100 MB side-table memory when on.

Retirement criterion: when the lever decision has landed and the
size-impact measurement is committed, this spike comes out.

## Headline finding

### hello.drvPath

```
v3-direct bytes (in arena/nursery): ... bindings=807.1MB ...
v3-direct bindings-attr: 19 distinct origins, 199149 total allocs,
                          420.2 MB total tracked
  alloc@vm.cc:959                 25504 allocs  395.5MB  94.1%
                                  [0/519/1131/1161/4850/2368/2252/6133/1775/5315]
  alloc@vm.cc:6708               106231 allocs    7.5MB   1.8%
  primMapAttrs                     2066 allocs    6.9MB   1.6%
  OP_ATTRS_REC_INIT_TAIL          35793 allocs    5.4MB   1.3%
  OP_ATTRS_REC_INIT               10629 allocs    1.3MB   0.3%
  OP_ATTRS_INIT_DYN                8457 allocs    1.2MB   0.3%
  ... long tail ...
```

**vm.cc:959 is `mergeBindings`** — v3's implementation of the
Nix `a // b` overlay operator.  On hello.drvPath:

  * **25,504 mergeBindings calls produced 395.5 MB of Bindings —
    94.1 % of all attributed Bindings bytes.**
  * Size histogram is spread wide: 5315 huge (≥129-entry) merges,
    6133 in the 33-64 bucket, 4850 in 5-8.
  * Combined with the slack analysis below, mergeBindings owns
    ~80-95 % of all 807 MB of `bytesBindings`.

The runners-up combined are noise:

  * vm.cc:6708 = an OP_ATTRS_REC_INIT path (7.5 MB — small but
    very many small allocs, 106k×).
  * primMapAttrs (6.9 MB) — `builtins.mapAttrs` constructions.
  * OP_ATTRS_REC_INIT_TAIL (5.4 MB), OP_ATTRS_REC_INIT (1.3 MB),
    OP_ATTRS_INIT_DYN (1.2 MB) — direct attrset literals
    discovered to be recursive (let-rec sets).

### hello.name (triangulation)

```
v3-direct bytes: ... bindings=266.9MB ...
v3-direct bindings-attr: 16 distinct origins, 33422 total allocs,
                          85.5 MB total tracked
  alloc@vm.cc:959                  6310 allocs   74.2MB   86.7%
  primMapAttrs                     1173 allocs    5.8MB    6.8%
  OP_ATTRS_REC_INIT_TAIL           5962 allocs    2.5MB    2.9%
  alloc@vm.cc:6708                12770 allocs    1.4MB    1.7%
  ...
```

Same shape on a different workload — mergeBindings dominance is
not a hello.drvPath artifact.

## Secondary finding — mergeBindings over-allocation slack

`bytesBindings` total = 807 MB.  My rollup tracked 420 MB.  The 387
MB gap is real and identifiable:

`mergeBindings` allocates `na + nb` entries up front
(`vm.cc:959`), then in the duplicate-key merge loop only fills `k`
of them (`k <= na + nb`), and writes `out->size = k` (`vm.cc:985`).
The remaining `(na+nb - k) * sizeof(Entry)` bytes are arena-pinned
but unused — wasted.

  * `bytesBindings` counter sees the original `na+nb` allocation
    bytes.
  * `dumpBindingsAttribution` reads `b->size` (now `k`) at dump
    time, so it computes the LIVE-entry bytes.
  * The difference is pure overhead from duplicate keys.

On hello.drvPath: **~387 MB of arena memory is duplicate-key
slack from mergeBindings**.  On hello.name: ~181 MB.  About half
of all v3-arena Bindings bytes on each workload.

This is independently actionable: a two-pass merge (count
distinct first, then alloc exact size) would reclaim it.  But it's
also evidence that the architectural fix (persistent maps with
spine sharing) would eliminate both the slack AND the per-merge
allocation entirely.

## Implication for the next lever

The data-structure audit's multi-GB hypothesis is confirmed AND
narrowed to a single call site.  The decision tree:

  * Option A — **Persistent/immutable map for overlay sharing**.
    `a // b` returns a logical overlay (the new entries with a
    pointer back to `a` for unchanged keys).  Lookup walks the
    chain; deep chains are common in nixpkgs (`prev // overlay
    final prev` from `lib.fixedPoints.extends`), so a chain depth
    budget + collapse heuristic is needed.  Pure win on memory;
    lookup overhead per chain depth.
  * Option B — **Two-pass mergeBindings to eliminate slack**.
    Cheap (~1 day), localized.  Recovers ~half the v3-arena
    Bindings bytes on hello.drvPath (~387 MB).  Does NOT change
    allocation count; does NOT help the per-mergeBindings cost.
  * Option C — **Detect-and-share unchanged spine**.  If
    `b->size << a->size` and all of `b`'s keys are in `a`, the
    output shares `a`'s entries[]; only modified slots are
    physically rewritten.  Subset of A's mechanism with a more
    specific signature.

Recommendation: **start with B as a lossy quick win** while
designing A.  B is ~1 day and frees ~387 MB on hello.drvPath
immediately with no API or invariant changes — a measurable Rule 0
falsifier ("does shrinking slack reduce peak_rss?").  Then A as
the multi-week architectural lever for the remaining 420 MB +
allocation-count reduction.

C is a fallback if A's chain-walk overhead turns out to be
prohibitive on lookup-heavy workloads.

## Rule 0 status

Hypothesis falsified: **"Bindings allocations are spread across
many sites"**.  Data shows ONE site is 94 % of bytes.

Hypothesis confirmed: **"`prev // overlay` overlay-style
construction dominates v3 arena usage"**.  vm.cc:959 IS exactly
the `a // b` evaluator.

Hypothesis introduced: **"mergeBindings over-allocation is itself
~half of all Bindings memory"**.  Falsifier: a two-pass
mergeBindings should reclaim ~387 MB on hello.drvPath and ~181 MB
on hello.name, and peak_rss should drop by approximately the same
amount.

## Validation

  * 9/10 v3 core suite PASS (only pre-existing `lint-no-inline-
    getenv` for vm.cc:92 V3_DBG_SIGTRAP unrelated to this work).
  * Default-off path unchanged (no behavioral or perf delta when
    NIX_V3_BINDINGS_ATTR is unset).
  * hello.drvPath byte-identical to TW under both gate states.
  * Cost when on: 199k extra unordered_map entries (~100 MB of
    tracking memory) + cached label intern table (~50 KB).
    Dump itself walks the map once; on hello.drvPath ~50 ms.

## Files changed

  * `src/libexpr-v3/include/v3/alloc.hh` — added
    `bindingsAttrDumpEnabled()`, `BindingsAttrRollupEntry`,
    `dumpBindingsAttribution()`; extended
    `bindingsOriginEnabled()` to also fire on
    NIX_V3_BINDINGS_ATTR=1.
  * `src/libexpr-v3/run.cc` — call `dumpBindingsAttribution(stderr)`
    from the NIX_VM_STATS exit block.

## Cross-references

  * `lode/DATA_STRUCTURE_AUDIT_2026-05-21.md` — predicted Bindings
    append-overlay as multi-GB; this commit pinpoints the call
    site.
  * `lode/EXTEND_DERIVATION_INVESTIGATION_2026-05-18.md` — context
    for hello.drvPath as the canonical workload.
  * #702 — RSS decomposition that named 956 MB / 84 % as the
    Bindings share.
  * #719 — earlier allocator-counter dump (per-category, not
    per-site).

## Memory references

  * `project_data_structure_audit_2026-05-21.md` — predicted this.
  * `project_702_rss_decomp_2026-05-21.md` — the 84 % share number.
