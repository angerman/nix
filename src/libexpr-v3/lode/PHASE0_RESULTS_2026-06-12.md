# Phase 0 results — PLAN_BEAT_TW falsifiers (2026-06-12)

Host: **Moritzs-MacBook-Air.local** (8-core, laptop). Numbers are DIRECTIONAL:
the *sign* of every effect below is host-robust; *magnitudes* (especially RSS)
carry ±10–15 % laptop noise — firefox RSS measured 574 / 623 / 668 MB across
three default runs. **RSS bars must be confirmed on darwin-4.** CPU (min-of-N
user-CPU) is the lower-noise axis and is what the conclusions below lean on.

Harness: `bench/phase0-falsifiers.sh`. Binary rebuilt at HEAD + the two
small-code probes (0.4 memo gated off, 0.5 freeListBins gate).

## The seven-row context (default arm, this host)

| Row | CPU ratio | RSS ratio | notes |
|---|---|---|---|
| hello.drvPath | 1.42–1.53× | 1.99× | TW 0.78s/136MB; v3 1.1s/269MB |
| git.drvPath | 1.74× | 1.99× | TW 1.01s/177MB; v3 1.76s/352MB |
| firefox.drvPath | 2.73× | 2.0× (noisy) | TW 1.84s/333MB; v3 5.0s/~620MB |

firefox RSS is already far below the plan's stated 1294 MB — the 46-commit
review (Chain default-on etc.) cut it to ~620 MB. The RSS *ratio* is ~2.0×,
not 2.70×.

## 0.1 — wrapper share (NIX_V3_NO_BC_DERIVATION_HYBRID=1) → **FALSIFIED "retire the wrapper"**

The all-C `primDerivationStrictNative` path **does not produce a result** — it
throws `error: infinite recursion encountered` on hello / git / firefox alike
(bails in ~0.16 s, hence the deceptive "7–30× faster"; the arms did different
work — the measurement-gate trap). The bytecode hybrid wrapper exists precisely
to break that recursion and **cannot be retired**. Lever 1.2 therefore narrows
to *optimising* the wrapper (drop the `attrNames`+`args.${k}` string allocs +
3× key hashing; iterate the chain cursor directly), not flipping its default.

## 0.2 — RSS decomposition at HEAD (NIX_V3_MEM_BUCKETS=1)

hello.drvPath (peak 282 MB):
- arena RESERVED (bump) 160 MB, **arena LIVE 30 MB → 130 MB dead-but-resident**
- run-phase alloc volume: **bindings 96.7 MB**, thunks 26.9, pairs 12.7, chars 7.6
- `mergeBindings`: 23 874 calls, 41.9 MB, **100 % from OP_ATTRS_UPDATE (`//`)**
- boehm_heap 402 MB (≈all free, gc_count=1); CU cache 30 MB graph + 22 MB bytecode

firefox.drvPath (peak ~660 MB):
- arena RESERVED 480 MB, **arena LIVE 30 MB → ~450 MB dead-but-resident**
- run-phase alloc volume: **bindings 264.5 MB**, thunks 111.9, pairs 45.8, chars 26.7
- `mergeBindings`: 88 835 calls, 136.9 MB, **100 % OP_ATTRS_UPDATE (`//`)**

**Conclusion**: allocation volume is dominated by `//` (attrset-update)
materialisation on both rows; the resident excess is "dead arena cells the
collector never reclaimed." The volume driver is the same on both axes →
allocate-less (cut `//` copies) is the highest-leverage RSS lever and is CPU-positive.

## 0.3 — GC-threshold curve → **FALSIFIED "lower threshold lowers RSS"** (and found the real blocker)

| | hello | git | firefox |
|---|---|---|---|
| default RSS / CPU | 269 MB / 1.07s | 350 MB / 1.71s | ~620 MB / 5.0s |
| lowGC (32–64 MB) RSS / CPU | 278 MB / 1.46s | 358 MB / 2.33s | ~620 MB / 4.8s |

Lowering the threshold **raised** RSS on the light rows (+9/+8 MB) and cost
+33–36 % CPU; firefox was within noise. Re-checked after the 0.5 freeListBins
fix — **the anomaly persisted**, so it is NOT freeListBins growth.

Root cause (confirmed from `vm.cc:3204` + gc_count): **the major-GC safepoint
fires only at `exitDepth == 0`** (the outermost dispatch loop). firefox/git/hello
evals spend almost the entire run in *nested* dispatch loops (callClosure2
re-entries, primop callbacks), so control returns to depth 0 essentially once —
**`gc_count = 1` regardless of threshold (tested 32/64/128 MB).** Lowering the
threshold cannot fire more GCs; it only adds mark cost at the one safepoint.
**Lever 2.1 (GC threshold) is INERT until the GC can fire mid-eval at depth>0.**
Because flat mark-sweep does NOT move cells (`vm.cc:3182-3184`), a non-moving
major GC at depth>0 is C-stack-safe in principle — that is the real (new) RSS lever.

## 0.4 — context-parse memo (V3_DBG_CTX_PARSE_MEMO=1) → **narrows Lever 1.1 to span-sharing**

firefox CPU: memo OFF 4.93 s, memo ON 4.81 s → **−2.4 %** (byte-identical). Below
the plan's ≥3 % "fund full interning" bar. Per the plan's own contingency, the
drv-CPU cost is in the per-edge *copies* (format → deep-copy-per-op), not the
parse-back. **Lever 1.1 narrows to span-sharing** (avoid the per-edge string
copies + the sort-over-strings), not parse memoisation.

## 0.5 — freeListBins push gate → **KEEP** (correctness-neutral; removes waste)

Gated the per-dead-cell `freeListAdd` (mark_sweep.cc:878) to the only condition
that ever pops it (`g_freeListReuseEnabled && !g_immixAllocEnabled`). On the
default path nothing consumes `freeListBins_`, so the push was pure RSS bloat
(8 B/cell + map<vector> growth) + sweep CPU. Default-path CPU/RSS neutral
(269 MB / 1.07 s, within noise); removes a confound from any future GC-heavy A/B.
`clearCellStartBitFor` left unconditional (bitmap-only, preserves sweep semantics).

## NEW falsification — Immix line reuse (V3_DBG_IMMIX_ALLOC=1) does ≈nothing

hello 269→270 MB, firefox 623→623 MB even with low threshold so GC fires + reuse
is active. Two reasons: (1) `immix-alloc: allocs=14` — **the Immix path is NOT on
the hot allocation path** (≈14 allocs total engage it); (2) reuse caps *virtual
growth*, but **peak RSS is a high-water mark** on a monotonic drvPath eval — by
the time GC frees lines, allocation is nearly done, so the bump high-water is
unchanged. **Lever 2.2 (flip Immix default-ON) is inert as gated** and would not
move peak RSS even if it engaged. The arena *does* munmap whole-dead blocks
(`freeWholeBlock`, RSS-returning on macOS) and has evacuation (`NIX_V3_EVAC`);
evac on firefox did not lower peak either (within noise, gc_count still 1 → it
runs at the one late safepoint).

## Net re-framing of Phases 1 & 2

- **Phase 1 (drv CPU)**: wrapper stays (optimise it: kill the per-attr string
  alloc + 3× key hash); 1.1 narrows to span-sharing (parse is only 2.4 %);
  1.3 (context fast paths) still worthwhile. The bulk of the firefox 2.73× is
  the wrapper's per-drv instruction+thunk overhead × #drvs + per-edge copies.
- **Phase 2 (RSS)**: the stated levers (2.1 threshold, 2.2 Immix-flip) are INERT
  for measured reasons. The real RSS path is **(a) allocate less** — cut the
  `//` (OP_ATTRS_UPDATE) materialisation volume that is 96 MB hello / 264 MB
  firefox of the arena and 100 % of mergeBindings (Levers 1.1/2.3 + a new
  "`//` writes in place / shares" lever), and **(b) fire the non-moving major GC
  at depth>0 safepoints** so the high-water mark is actually lowered mid-eval.
  Both need darwin-4 to measure RSS to the plan's bars.

*Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output
Group. SPDX-License-Identifier: Apache-2.0.*
