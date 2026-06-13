# Session index — BEAT_TW_V2 execution (2026-06-13 → 06-14)

Canonical entry point for the work implementing `PLAN_BEAT_TW_V2_2026-06-13.md`.
Branch `angerman/2.35-eval-profiling-v2`, from `c2a8f0bee`. Every row links to its
commit (the primary record, with A/B data + the hypothesis it kills — Rule 0).
Deeper write-ups: `PLAN_BEAT_TW_V2_EXECUTION_2026-06-13.md` (Wave-1 + WS-A table),
`BROADER_A_SCOPING_2026-06-14.md`. Cross-session memory:
`gc_trigger_tuning_falsified_2026-06-13`, `beat_tw_v2_wave1_results_2026-06-13`,
`broader_a_scoping_2026-06-14`.

## Headline
The "cheap wins" wave is tapped out: the §0.2 micro-shares don't reproduce as
wall wins (hello.drvPath is import/disk_cache-dominated), and the firefox-GC cost
is structurally hard (BOTH levers falsified). Confirmed wins come from the two
levers that survived measurement (§1.2 + §1.1b). The firefox RSS lever is NOT
materialize-removal (total materialize = 72MB) — it's the eval heap (→ workstream H).

## Ledger (item → verdict → commit → evidence)

| item | verdict | commit | key result |
|---|---|---|---|
| §1.2 intersectAttrs (iterate smaller side) | **SHIPPED** | `6df8d0ed6` | hello.drvPath 1.65→**1.28×**, firefox 2.78→**2.38×**, git→1.54×; byte-identical |
| lint cold-site marker (core→20/20) | **SHIPPED** | `7114d136c` | clean QG-1 baseline |
| §1.1a GC threshold 256MB→1GB | **FALSIFIED** | `0ddc9ad6a` (body) | HNE peak RSS +44.7% (its GC frees ~624MB whole blocks = useful) |
| §1.1b adaptive GC backoff (heap×8 on freed<5%) | **SHIPPED** | `0ddc9ad6a` | M5 CPU 26.4→**19.5s (−26%)**, arena-identical, HNE-safe |
| §1.7 OP_APPLY_OVERRIDES chain guard | **SHIPPED** | `40a64c2dd` | latent C-1-class correctness fix; run-1.7 8/8 |
| §1.3 materialize k-way merge | **FALSIFIED/reverted** | (exec-log) | noise: introsort ≈1% not the projected 9% |
| §1.4 has-context presence filter | **FALSIFIED/reverted** | (exec-log) | +10.5% SLOWER (find already cheap on tiny tables) |
| §1.5 cross-Force deferral / §1.6 OP_RETURN diet | **DEFERRED** | (exec-log) | sub-bar by analysis (~0.3% fib) |
| §1.8 mark speedup (block-aligned mmap) | **FALSIFIED** | `916ab91ea` | mark is **cache-bound** (225ns/cell, regionOf <10%) → can't hit −60% |
| E-stage-0 (mid-eval live trace) | **VIABLE** | (exec-log) | firefox L_resident 0.44 < 0.80, dead-lines 42.7% |
| DP-1 7-row re-pin + QG-4 ratchet | **SHIPPED** | `812912ed4` | `seven-rows.tsv` + `make ratchet-check`; attrNames **0.46×** (v3 wins), fib 1.37×, foldl 3.16×, hello 1.28×, git 1.54×, firefox 2.38×, M5 2.31× |
| WS-A.1 tests-first (06-07 canary) | **SHIPPED** | `a01bdd024` | git/cargo/rustc/cargo-auditable.cargoDeps/python3.withPackages byte-identical 5/5 |
| suite registration (§1.7 + canary) | **SHIPPED** | `f9ce54fb7` | core 21/21 |
| WS-A.2 shared-parent-writeback counter | **SHIPPED + FINDING** | `d6a3265eb` | writebacks PERVASIVE + **BENIGN** (firefox 18, byte-identical) = correct-WHNF memo like TW; "shared-parent write = C-1" is FALSE |
| WS-A.3 chain-SELECT **L1 + provenance assert** | **SHIPPED (gated)** | `e890e3502` | `NIX_V3_CHAIN_LOOKUP_SELECT`; 06-07 5/5, lang 143, core 21/21 gate-on, **provenance 0 violations** (C-1 refuted), aggressive-GC clean |
| L1 darwin-4 formal RSS bar (QG-2) | **MEASURED → gated** | `136b35883` | firefox −33.6MB arena / −29MB maxRSS, byte-identical, CPU-neutral — **below −40MB revert floor** → not default-on |
| broader lookup-without-materialize | **SCOPED → RETIRED** | `878d6a190` | total materialize 72MB firefox / 40MB hello < 80MB bar → unreachable; **pivot to eval heap** |

## Gated artifacts left in place (reproduce any number / guard regressions)
- Regression canaries: `test/run-chain-select-0607-failure-set.sh`, `test/run-1.7-apply-overrides-tests.sh` (in core suite)
- QG-4 ratchet: `bench/pin-seven-rows.sh`, `bench/ratchet-check.sh`, `bench/baselines/seven-rows.tsv`
- Measurement gates (default-off): `V3_DBG_SHARED_WB` (provenance + shared-WB counter), `V3_DBG_MAT_SITES` (per-caller materialize attribution), the `markMs`/`mark-split` stats, `NIX_V3_CHAIN_LOOKUP_SELECT` (L1)

## Open / next direction
**Pivot to workstream H — shared capture frames** (the representation tax; firefox
~1M thunks + ~1M pairs; M5 = 928MB thunks). Measure-twice first step: a gated
captured-upvalue-duplication histogram across sibling thunks to size the
recoverable fraction before the write-barrier/shared-frame work. Secondary:
workstream E (GC reclaim of the 224MB scattered dead — but mark cache-bound +
multi-week). L1 default-on remains pending a recalibrated bar / broader removal.

*Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output
Group. SPDX-License-Identifier: Apache-2.0.*
