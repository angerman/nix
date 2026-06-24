# S0.3 — Safepoint Foundation architecture decision — 2026-06-24

Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output
Group. SPDX-License-Identifier: Apache-2.0.

Synthesizes S0.1 (#163, reclaimable ceiling) + S0.2 (#164, root census + pinned
fraction) into the committed architecture. Both measurements (git-noted d12d11e5e /
746fbc219) came back FAVOURABLE and CHEAPER than the program doc assumed.

## Inputs (measured, darwin-4)

- **Reclaimable ceiling (S0.1):** arena dead-at-warm-peak firefox 62% / M5 64% / HNE
  57%; projected perfect-compaction RSS firefox 1.08–1.22×, M5 1.30–1.39× TW. GO.
- **Bartlett pinned-fraction (S0.2):** cells reachable only via the conservative
  C-stack scan SHRINK with scale → at peak firefox **12.5%**, M5 **3.6%**.
- **Re-entrant root census (S0.2):** 3 re-entry primitives (`forceValue`,
  `callClosure`, `callClosure2`); ~7 hot primops hold loop-carried/container roots
  across a callback (~25 LoC to wrap); `GcRoot` RAII exists but is unused.

## DECISION

**Two collectors share one safepoint, split by axis:**

### RSS exploit (S2) = Bartlett mostly-copying — NO handle migration
At peak, only 3.6–12.5% of live cells are conservative-C-stack-only. A mostly-copying
collector that **conservatively pins the cells any C-stack slot references and compacts
the precisely-rooted rest** captures ~87–96% of the reclaim *without* the S1.1/S1.2
handle work. This is the cheap, early RSS win. Decision: **S2 = Bartlett**.

Crucial re-sequencing: the mid-eval mark-sweep **already runs at `exitDepth>0`** (the
`v3 evac-movability` line proves it) with the value-stack consistent + a conservative
C-stack scan (`drainConservative`) + the remembered set (`dirtyContainers`). The
infrastructure for *when/where* to collect mid-eval EXISTS; it is non-moving only
because moving was gated off. **S2.1's real work is the MOVER** (relocate + pointer-
fixup with pinning), wired into the existing trigger — not new safepoint plumbing.

### CPU exploit (S3) = precise handles + stack maps
Native codegen needs precise, updatable roots in JIT'd register/spill state — Bartlett's
conservative pinning is not enough (and you don't want to pin hot JIT frames). So the
**precise** path (S1.1 Handle API + S1.2 migrate the ~7 hot primops + S3.1 JIT stack
maps) serves codegen. It's ~25 LoC of primop wrapping + the JIT map emission — small,
because S0.2 showed the live-across-callback root set is tiny.

## Open refinements (resolve in S2.1, do NOT block S0.3)

1. **Pinning granularity.** Bartlett pins whole regions, not single cells. v3 has 128B
   Immix lines — pin at LINE granularity (not 16MB block) so scattered pinned cells cost
   little. The #136 "blocks 25–75% live" finding is about whole-block-free; line-pinned
   compaction is finer and unaffected.
2. **Both-reachable cells.** A cell reachable from BOTH a precise root and a C-stack slot
   must be PINNED (else the precise mover relocates it and the un-updatable C-stack
   pointer dangles). So the true pinned set ≥ the measured `conservativeOnly` (3.6–12.5%
   is a lower bound). Pin-wins-over-move is standard Bartlett; budget for a somewhat
   higher pinned fraction.
3. **Move via the remembered set.** S0.2 confirmed the dirty-list walk marks 61–181K
   cells the precise mark misses → those roots are real and must be RELOCATED+updated by
   the mover (not just marked). The existing `dirtyContainers` walk gives the mover its
   inter-generational roots.

## Safepoint poll + trigger

- **Poll point:** reuse the existing mid-eval heap-pressure check in `dispatchLoop`
  (between opcodes → value-stack consistent). S1.3 mainly *verifies* it is a valid MOVE
  safepoint (value-stack + withStack + remembered-set roots all enumerable there) rather
  than building new plumbing.
- **Trigger/cadence (S2.2):** reuse the heap-pressure threshold; expose a knob to balance
  RSS vs GC-CPU (the documented tension). Tune in S2.2/S4.1.

## Re-sequenced dependency graph (decouples RSS from codegen → they parallelize)

    S0.3 ─┬─ S1.3 (verify move-safepoint)            ── RSS PATH
          │     └─ S1.4 (validate roots complete, non-moving)
          │           └─ S2.1 (Bartlett mover) ─ S2.2 ─ S2.3
          └─ S1.1 (Handle API)                        ── CODEGEN PATH
                └─ S1.2 (migrate ~7 primops)
                      └─ S3.1 (JIT stack maps) ─ S3.2 ─ S3.3
    S4.1 ← S2.3 + S3.3

S1.3 is decoupled from S1.1 (the RSS path does not need handles); S1.4 (root-
completeness, non-moving) gates the Bartlett mover and is independent of the handle
migration; S3.1 needs both S1.2 (handles migrated) and S1.4 (roots validated). After
S0.3 the RSS and codegen paths run in PARALLEL.

## Net effect on the estimate

Bartlett at ~90% capture of the S0.1 ceiling → realized RSS ≈ firefox ~1.15–1.25×, M5
~1.4–1.5× (better than the original 1.34/1.5–1.7× envelope, because the pinned fraction
at peak is small). The handle/codegen path is unchanged in ceiling but cheaper to build
(~7 primops). Phase S0 (de-risk) COMPLETE → GO.
