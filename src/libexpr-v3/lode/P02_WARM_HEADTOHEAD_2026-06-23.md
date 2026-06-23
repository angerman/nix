# P0.2 (#132) — WARM (cache-on) TW-vs-v3 head-to-head — 2026-06-23

Task #132.  The cache-off gap (3×) includes v3's one-time parse+lower+optimize,
which is amortized in production (warm disk-bytecode cache).  This measures the
REAL production steady-state gap.  darwin-4 (quiet host, authoritative CPU),
median-of-N, back-to-back, binary built from HEAD 048ecf425.

## Results (darwin-4, tight spreads)

| workload | mode | TW | v3 default | gap |
|---|---|---|---|---|
| firefox | cold | 0.74 s / 358 MB | 2.69 s / 677 MB | 3.64× CPU, 1.89× RSS |
| firefox | **WARM** | 0.73 s / 358 MB | **1.82 s / 586 MB** | **2.49× CPU, 1.64× RSS** |
| M5 | cold | 3.60 s / 982 MB | 11.02 s / 2983 MB | 3.06× CPU, 3.04× RSS |
| M5 | **WARM** | 3.59 s / 982 MB | **6.57 s / 2218 MB** | **1.83× CPU, 2.26× RSS** |

(mideval is opt-in and CPU-costly — firefox warm 2.86 s/525 MB 3.92×; M5 warm
12.20 s/1803 MB 3.40× — it lowers RSS but the lever is killed for peak-RSS, see
#136; shown for completeness.)

## What warm reveals

1. **Parse+lower is a LARGE fraction of v3's cold CPU** — firefox −0.87 s (−32%),
   M5 −4.45 s (−40%).  It is amortized in production, so the real production CPU
   gap is **1.83× (M5) – 2.49× (firefox)**, not the 3× the cache-off number
   suggests.  The cache-off gap OVERSTATES the production gap.
2. **Warm also cuts RSS substantially** — firefox −91 MB, **M5 −765 MB (−26%)**.
   Cold mode builds + retains the IR + parse structures for thousands of files
   (MALLOC_SMALL churn + fragmentation); warm loads bytecode straight from SQLite
   without those transients.  So a big slice of the cold RSS (and of the #139
   ~360 MB "fragmentation") is parse/lower TRANSIENT, not steady-state.  Production
   RSS gap is **1.64× (firefox) – 2.26× (M5)**.
3. **TW is warm-insensitive** (0.73–0.74 s / 358 MB; 3.59–3.60 s / 982 MB) — it has
   no persistent cache and re-parses every process, but its CPU is eval-dominated
   so warm≈cold.  Confirms TW as the stable anchor.

## Conclusion — the production gap to close

| axis | production gap | nature | lever |
|---|---|---|---|
| CPU | 1.83–2.49× | per-op interpreter overhead (parse already amortized) | JIT (#137, structural, multi-week) |
| RSS | 1.64–2.26× | live representation heavier than TW's 16 B niche Value | broad foundational (all reclaim levers killed: #134/#136) |

The warm CPU gap is **structural per-op dispatch** — the profile (PROFILE_AT_SCALE)
already showed forceValue/callClosure are NOT the bottleneck; it's uniform per-op
cost, which only native codegen (JIT) closes.  The warm RSS gap is the live cell
representation (v3 cells + CU structures + Boehm FFI > TW's 16 B niche-tagged
Value) — confirmed structural by the four RSS-lever KILLs this session.

This confirms the prior-session warm reframe (profile_at_scale: ff 2.44 / M5 1.79)
on fresh darwin-4 data with tight spreads.  P0.2 COMPLETE.

*Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output
Group. SPDX-License-Identifier: Apache-2.0*
