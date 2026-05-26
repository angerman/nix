# Cardano-node M5 ritual measurement — 2026-05-26

**Date:** 2026-05-26 (evening)
**Author:** session synthesis
**Status:** measurement-only — surfaces a major memory + wall regression vs 2026-05-21 baseline
**Triggering question:** DIRECTION_NOTE_2026-05-26 §3.3 / §6 action #3 — cardano-node M5 strategic-workload drift check
**Companion docs:** [`CARDANO_NODE_M5_2026-05-21.md`](CARDANO_NODE_M5_2026-05-21.md) (baseline)

## TL;DR

**~6× memory regression and ~3× wall regression on cardano-node M5 vs 2026-05-21 baseline.**

Both v3-native (default) and v3 + TW-callFlake (workaround) paths regressed substantially. Output remains byte-identical to TW. Cause not yet localised — multiple defaults have flipped since 2026-05-21 (Phase 4b IFD cache, formals-bridge, V3_RELEASE available, Schema 14, disk-cache default-on, Phase D/E barriers, A1a ChainBindings scaffold).

The strategic-workload drift this measurement guards against is real. The 919 MB headroom against the 4 GB watchdog is gone — v3 now requires ≥ 6 GB heap headroom to evaluate the M5 target.

## Workload

Same expression as 2026-05-21 baseline:

```
(builtins.getFlake "path:/Users/angerman/Projects/iohk/cardano-node")
  .outputs.packages.aarch64-darwin.cardano-node.name
```

Host: macOS darwin 25.4.0, M-series, same machine as 2026-05-21.

## Numbers

**Wall (hyperfine, 5 runs after 1 warmup):**

| variant       | mean ± σ        | min    | max    |
|---------------|-----------------|--------|--------|
| tw            | 10.519 ± 1.305s | 9.21s  | 12.54s |
| v3-default    | 41.358 ± 14.9s  | 29.97s | 67.17s |
| v3-no-native  | 30.364 ± 4.09s  | 25.49s | 35.27s |

**Peak RSS (3 independent runs via /usr/bin/time -l):**

| variant       | run 1   | run 2   | run 3   | mean   |
|---------------|---------|---------|---------|--------|
| tw            | 856 MB  | 856 MB  | 856 MB  | 856 MB |
| v3-default    | 6895 MB | 4947 MB | 5946 MB | 5929 MB |
| v3-no-native  | 5298 MB | 5421 MB | 6561 MB | 5760 MB |

**Output:** byte-identical to TW (`"cardano-node-exe-cardano-node-10.6.1"`).

## Regression vs 2026-05-21 baseline

| metric                          | baseline 2026-05-21 | now 2026-05-26 | delta |
|---------------------------------|---------------------|----------------|-------|
| v3 (no-native callFlake) wall   | 10.2s               | 30.4 ± 4.1s    | **3.0× slower** |
| v3 (no-native callFlake) peak_rss | 919 MB            | 5760 MB        | **6.3× larger** |
| TW wall                         | 7.97s               | 10.5 ± 1.3s    | +32% (host noise; same machine, different day) |
| TW peak_rss                     | 898 MB              | 856 MB         | -5% (within noise) |
| v3/TW wall ratio                | 1.28×               | 2.89× (no-native) / 3.93× (default) | substantially worse |
| v3/TW memory ratio              | 1.02×               | 6.7× (no-native) / 6.9× (default)   | substantially worse |

TW remained stable on this host (within noise) → the regression is in v3, not the environment.

## Variant comparison (v3-default vs v3-no-native)

The 2026-05-21 baseline documented v3-native callFlake as the M5 blocker (4 GB OOM). With NIX_V3_NO_NATIVE_CALL_FLAKE=1 + 16 GB cap, both variants now complete. Differences:

- **Wall:** v3-no-native is faster (30.4s vs 41.4s; -27%). Native callFlake takes the slower path even though it now completes within 16 GB.
- **Peak RSS:** comparable (5760 MB vs 5929 MB). Variance dominates the difference.
- **Variance:** v3-default has σ=14.9s (36% of mean), much noisier than v3-no-native σ=4.1s (13% of mean). The native path is more sensitive to disk-cache state.

Neither variant is acceptable — both are 5-7× over the prior 919 MB target.

## Candidate causes (ordered by hypothesis priority)

Possible regression sources since 2026-05-21:

1. **Phase 4b IFD cache default-on** (`d22e1bfd3`, ~2026-05-23): EvalResults table accumulates IFD results in-process; cardano-node hits many IFDs through haskell.nix. Could be live retention growth.
2. **Formals-bridge default-on** (`d22e1bfd3`): if formals are now always synthesised, attrset population on the v3→TW boundary may grow.
3. **Schema 14 PosIdx remap** (`a7b41ddce`): adds per-CU sparse posTable; if cardano-node compiles many CUs, the table grows.
4. **Disk-cache default-on** (`#771`): in-memory CompilationUnits accumulate; large flake → large CU set.
5. **A1a ChainBindings scaffold** (`98ca953bb` + `6f8095cd5` + `2cf14fdce`): +8B per Bindings header. If M5 allocates many Bindings, accumulates.
6. ~~**cardano-node tree drift**~~ — **FALSIFIED** via cheap check (2026-05-26). cardano-node flake.lock last git-modified `d29c61807` on 2025-11-25; on-disk mtime predates the 2026-05-21 baseline. The workload itself is unchanged. **The regression is in v3, not the workload.**

## Surface-area notes

The "v3_total + boehm_heap + elsewhere" breakdown from V3_VM_STATS at OOM (with native callFlake at 4 GB cap) showed:
- v3_total: 2.9 GB
- boehm: 384 MB (99.9% free in earlier hello.drvPath profile; likely similar)
- elsewhere: 724 MB

At the (real, completing) 6 GB peak with 16 GB cap, scaling suggests ~5 GB v3_arena. The 8× growth in `bytesBindings` (595 MB peak) vs prior workloads suggests **mergeBindings call frequency on cardano-node may be the dominant lever** — A1a Phase C target territory.

## Strategic implications

1. **The DIRECTION_NOTE #3 ritual was load-bearing.** Without this re-measurement, the 919 MB capability target would remain a stale claim in `CARDANO_NODE_FEASIBILITY_2026-05-18.md` and `CARDANO_NODE_M5_2026-05-21.md`. Today's data falsifies that as a current state.
2. **The 4 GB watchdog default no longer protects M5.** Users hitting M5 with NIX_V3_MAX_HEAP=4G (default of the safety system) will see OOM. The watchdog cap should likely default higher OR M5 cannot be considered "v3-supported" at current defaults.
3. **A1a Phase C scheduling becomes more urgent.** Memory-first-class framing per `[[memory-first-class]]` requires Phase C to recover the 5+ GB regression. Until Phase C (or an alternative path) lands, M5 is not back at the prior capability level.
4. **R8a Phase 1 spike (AOT distribution) target should be HNE first, M5 second.** HNE has a 5.3× RSS gap; M5 has a 6.3× gap. Phase 1's pre-committed threshold (≥30% warm-eval improvement on HNE) remains the right gate before extending to M5.
5. **Per-default-flip ritual measurement is needed.** This 5+ GB regression accumulated silently across multiple commits because no single commit re-measured M5. The DIRECTION_NOTE §3.3 cron-cadence proposal is essential infrastructure.

## What this doc is NOT

- **Not a root cause.** Hypotheses listed in §"Candidate causes" are unranked beyond intuition. Bisect needed.
- **Not a fix.** Captures state; does not propose remediation.
- **Not a falsifier for any specific commit.** Multiple commits could have contributed.
- **Not a recommendation to revert.** Reverting blindly destroys correctness wins (Phase 4b parity, formals-bridge, Schema 14 R1-trigger closure). Bisect first.

## Recommended next steps

1. ~~**Cheap check first**~~ — **DONE 2026-05-26.** Cardano-node tree is unchanged since 2025-11-25; v3 is the regression source. Hypothesis #6 falsified.
2. **Bisect default-flips** (1-2 days): turn off each recently-flipped default one at a time, re-measure peak_rss. Identify the dominant lever.
3. **Per-origin Bindings attribution on M5** (1 day): run with NIX_V3_BINDINGS_ATTR=1 — same instrumentation used for hello.drvPath in `#746`. The 595 MB bytesBindings figure suggests this will be high-signal.
4. **Wire nightly cron** (1 day): `bench/cardano-node-m5-cron.sh` recording peak_rss + wall per build. Guard against silent drift accumulation.
5. **Tier elevation**: this regression is significant enough that an active investigation track (Tier R or Tier A) should be opened to recover the 919 MB target.

## Cross-references

- [`CARDANO_NODE_M5_2026-05-21.md`](CARDANO_NODE_M5_2026-05-21.md) — baseline (now stale)
- [`CARDANO_NODE_FEASIBILITY_2026-05-18.md`](CARDANO_NODE_FEASIBILITY_2026-05-18.md) — strategic workload definition; 919 MB capability claim
- [`DIRECTION_NOTE_2026-05-26.md`](DIRECTION_NOTE_2026-05-26.md) §3.3 / §6 action #3 — the ritual that surfaced this
- [`MEMORY_REDUCTION_AVENUES_2026-05-26.md`](MEMORY_REDUCTION_AVENUES_2026-05-26.md) — A1a Phase C target work
- [`AOT_DISTRIBUTION_2026-05-26.md`](AOT_DISTRIBUTION_2026-05-26.md) — R8a Phase 1 HNE-first ordering still correct
- `[[memory-first-class]]` (memory file) — operating rule cross-checked here

**Commits referenced (candidate cause hypotheses):**
- `d22e1bfd3` Phase 4b + formals-bridge default-on
- `#771` (id TBD) NIX_V3_DISK_CACHE default-on
- `a7b41ddce` Schema 14 PosIdx remap
- `b17ab3359` R1-trigger AttrSet REC_SET canonical emit (today; could not contribute to pre-today regression but applies to v3-default now)
- `98ca953bb` + `6f8095cd5` + `2cf14fdce` A1a Phase A/B scaffold

---

*Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group. SPDX-License-Identifier: Apache-2.0.*
