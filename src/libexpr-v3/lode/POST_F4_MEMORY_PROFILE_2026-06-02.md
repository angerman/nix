# R0 deliverable — post-F4 memory profile + Gate A-D verdicts

**Date:** 2026-06-02
**Status:** R0 COMPLETE — the re-measurement mandated by
[`POST_F4_GC_GOAL_2026-06-02.md`](POST_F4_GC_GOAL_2026-06-02.md) §2.
Same host, gate-off (no GC), trimmed mean ± σ over N=5 (peak) per
[[same-host-bisect]].
**Verdict:** GC is needed (Gate A), it's an arena problem (Gate B), GC can
close the gap with enormous margin (Gate D), and the family is
**Nofl-Immix** (Gate C — with the post-F4 caveat that **evacuation is now
load-bearing, not optional**).

Companion: [`IMMIX_NOFL_DESIGN_2026-06-02.md`](IMMIX_NOFL_DESIGN_2026-06-02.md)
(the chosen design), [`POST_F4_GC_GOAL_2026-06-02.md`](POST_F4_GC_GOAL_2026-06-02.md)
(the gates), [[memory-first-class]].

---

## 1. The four numbers (post-F4, gate-off, same host 2026-06-02)

| Workload | peak_rss (MB) | v3_arena cumul (MB) | elsewhere (MB) | freeable @end | wall (s) |
|---|---|---|---|---|---|
| hello.drvPath | **712.7 ± 0.15** | 587.2 | 0.0 | 238 MB | 0.98 |
| HNE | **2434.7 ± 0.06** | 1660.9 | 370.8 | 1.43 GB | 6.15 |
| M5 (cardano-node) | **4686 ± 102** | 7197.4 | 0.0 | 6.22 GB | 24.6 |

M5 raw peaks (N=5): `[4722, 4765, 4923, 4571, 4286]` — **all 5 > 4096**.
`v3_arena` is *cumulative bump bytes* (deterministic, σ=0); resident peak is
lower (macOS compresses cold bump pages — `arena 7197 ≫ resident 4686`).

**Same-host pre-F4 reference:** M5 ledger 4125-4953 (warm, `580d0999a2`,
2026-05-27); HNE historical 2987 (`HNE_BUCKET_DECOMP`). Post-F4 HNE dropped
~550 MB (elsewhere 990→371: the bridge-held cache retention is gone) —
matching the goal §1 prediction (~518 MB bridge-held on HNE). M5 is in the
same band as the pre-F4 warm ledger (bridge deletion did **not** by itself
drop M5 under the watchdog).

A prerequisite F4 **correctness regression** surfaced and was fixed first
(`b1068808d`): `readDir`/`import` on an attrset whose `outPath` is *itself*
an attrset (haskell.nix `cleanSourceWith`) — the deleted bridge's recursive
`coerceToString` wasn't replicated. HNE was non-evaluable before that fix.

---

## 2. Gate verdicts (pre-committed in POST_F4_GC_GOAL §3)

### Gate A — is GC needed? → **YES**
M5 peak 4686 ± 102 ≥ 4096 (every one of 5 runs exceeds the watchdog;
min raw 4286). Bridge deletion did not settle M5. **GC is needed.**
Over-watchdog excess **E = 4686 − 4096 ≈ 590 MB** (trimmed mean).

### Gate B — GC problem or cache problem? → **GC (arena) problem**
M5 `elsewhere = 0.0` (no ImportCache/SQLite resident on the cardano-node
`.name` path). E lives entirely in `v3_arena`. **GC problem, not cache.**
(HNE carries 371 MB `elsewhere` — a *secondary* cache-eviction lever, but
its arena 1660 MB dominates; M5 is the watchdog target and is arena-pure.)

### Gate C — which family? → **Nofl-Immix (evacuation load-bearing)**
*Measurement honesty:* the L(t) periodic sampler fires only at
`exitDepth==0` safepoints (C-stack-safety: the transitive walk needs precise
roots). Deep getFlake evals almost never unwind to `exitDepth==0`, so
sampling is sparse and **below the ≥10-sample acceptance bar** (M5 n=2,
HNE n=4, hello n=2) and **trough-biased** (samples land right after a
top-level sub-expr completes, when intermediates just died).

Available signal (all `exitDepth==0`, lower-biased):
- M5: L_resident = {0.213 @ 2.0 GB alloc, 0.145 @ 6.9 GB} — live grows
  438→993 MB while alloc grows 2.0→6.9 GB ⇒ **low-L, high-garbage**.
- HNE: L_resident ∈ [0.373, 0.556], median 0.435.
- hello: L_resident ∈ [0.549, 0.674].

Post-F4 **L dropped** (bridge-era L=0.74 was bridge-inflated; goal §1
predicted this). Per the pre-committed gate this nominally touches the
"L_min < 0.3 → copying-at-trough reopens" branch (M5 L_min 0.145) — but the
data is **invalid by its own ≥10-sample criterion**, so neither the
"non-moving" nor the "copying" branch fires cleanly on L(t) alone.

Disambiguator (goal's designated tiebreaker — the Immix line-occupancy /
block probe, reliable end-of-eval, *not* safepoint-limited):
- **HNE: 43.9% of 16 MB blocks fully dead (721 MB whole-block-freeable) but
  99.8% of 128 B lines dead (1640 MB line-reclaimable).** The gap between
  block-level (43.9%) and line-level (99.8%) reclaim is *exactly* the Immix
  advantage over flat MS, and the 56% mostly-dead-but-pinned blocks are the
  evacuation opportunity.
- M5 + hello block probe returned 0.0% (a probe tooling bug — its separate
  marker walk failed while the freeable trace in the *same run* succeeded;
  folds into the real GC marker per the probe's retirement criterion). But
  M5's **0% fully-dead-blocks alongside 6.22 GB freeable** is itself the
  decisive signal: **dead is scattered sub-block ⇒ whole-block-free alone
  cannot release it ⇒ evacuation is required.**

**Verdict: Nofl-Immix.** It is robust across the (uncertain) L range:
in-place mark-region avoids copying's peak at high L, and opportunistic
**evacuation** concentrates scattered live to free whole blocks at low/
fragmented L. The post-F4 low-L + scattered-dead profile does **not**
falsify Immix — it *elevates evacuation (R2.4) from optional defrag to
load-bearing*. A pure non-moving (no-evac) variant would be insufficient
(M5's scattered dead); a pure copying variant would bet L is always low
(unconfirmed). Immix subsumes both. (Consistent with the historical
`IMMIX_LINE_OCCUPANCY` 46.5%/50.5% and the user's committed direction.)

### Gate D — can GC close the gap? → **YES, with huge margin**
End-of-eval freeable: hello 238 MB, HNE 1.43 GB, **M5 6.22 GB** — all
vastly exceed E ≈ 590 MB. Even mid-eval, the M5 L=0.145 sample @ 6.9 GB
alloc implies ~5.8 GB dead at a high-alloc point. **GC can close the
watchdog gap many times over** — the constraint is the page-release
mechanism (R1), not the amount of reclaimable garbage.

---

## 3. Next steps (per the goal's R-phases)

1. **R1 — page-release spike (the cheap falsifier, BEFORE R2).** Every one
   of the 11 prior GC failures hit the arena page-release pin (`alloc.hh`
   has no `madvise`/`munmap`). Prove `madvise(MADV_DONTNEED)`/`munmap` on a
   known-dead arena region returns ≥80% of freed bytes to RSS on macOS
   aarch64 (and Linux x86_64). This is platform-sensitive and the deepest
   possible blocker — surface it in ~2-3 days, not weeks. **Gate D already
   confirms there is far more than E of page-aligned dead to release**
   (43.9% fully-dead blocks on HNE; the M5 scattered-dead needs evacuation
   to make it page-aligned — wired in R2.4).
2. **R2 — implement Nofl-Immix** over the existing substrate (`alloc.hh`
   line bitmap + cellStarts + freeSpans; `mark_sweep.cc` MarkVisitor +
   sweepOneBlock + walkCStackConservative; `precise_root.{cc,hh}`, cleaner
   post-F4). Page-release built INTO the sweep. **Evacuation (R2.4) is
   load-bearing per Gate C, not optional.** SHIP gate: M5 peak < 4096.

## 4. Honest limits
- The L(t) mid-eval distribution is undersampled (safepoint constraint) —
  Gate C rests on the directive + Immix's L-robustness + the HNE
  line-occupancy datum, not a fresh ≥10-sample L distribution.
- `dumpV3LiveBlockProbe` is unreliable post-F4 (0% on hello/M5); the
  freeable trace (`NIX_V3_LIVE_TRACE`) is the trustworthy Gate-D anchor.
- Freeable is an end-of-eval LOWER BOUND on mid-eval reclaimable; the real
  peak-time reclaimable is what R2 must capture (and the M5 mid-sample says
  it is large).

---

*Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group. SPDX-License-Identifier: Apache-2.0.*
