# Safepoint Foundation — full program scope — 2026-06-24

Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output
Group. SPDX-License-Identifier: Apache-2.0.

## The problem, precisely

v3 already has a generational moving GC (nursery + Cheney scavenge + gen-major,
default-on) AND a precise root walker (`walkAllV3Roots`, `GcRoot` RAII, the auditor,
Stages 1–5). The moving collector is **gated to `exitDepth==0`**: it only runs when the
dispatch loop is *not* nested inside a re-entrant primop callback.

WHY the gate exists: during deep nixpkgs eval, higher-order primops (`map`, `foldl'`,
`genericClosure`, `derivationStrict`, the bytecode-install path, …) call back INTO the
VM. Their C++ stack frames hold live v3 `Value`s in C++ locals. A *moving* collector
must update every reference to a relocated cell — but it cannot find/update a v3 pointer
sitting in an arbitrary C++ local, and a *conservative* scan can find a suspected
pointer but must NOT overwrite it (it might be a coincidental integer). So those cells
must be PINNED; the campaign chose the blunt instrument of gating ALL moving off mid-
eval. Consequence: the arena grows monotonically and never compacts during real evals →
the RSS wall (every reclamation lever this campaign died on this one root cause).

The **safepoint foundation** = make the live v3 pointers reachable through re-entrant
C++ frames *precise, enumerable, and updatable*, plus a safepoint protocol to run the
collector at well-defined points. This is the single shared prerequisite for the two
highest-ceiling levers (`SAFEPOINT_FOUNDATION_YIELD_2026-06-24.md`):
- **Compacting mid-eval GC** (RSS): firefox 1.64×→~1.34×, M5 2.26×→~1.5–1.7×.
- **Native codegen** (CPU): firefox 2.46×→~1.7–2.0×, M5 1.82×→~1.4–1.6×.
Necessary but not sufficient to *beat* TW (the two yields trade via GC frequency; the
heavier live representation + bytecode metadata is a residual). This program builds the
foundation and exploits it on both axes, each phase measure-gated.

## Architecture decision (resolved in S0.3)

Two roads for making re-entrant roots safe-to-move:
- **Precise handles** (V8/SpiderMonkey style): primops wrap cross-callback Values in
  RAII `Local<Value>` handles on a thread-local movable-root stack the GC walks +
  rewrites. Complete (move everything), but requires migrating every re-entrant site.
- **Bartlett mostly-copying**: conservatively scan the C-stack → PIN the blocks those
  suspected pointers land in; precisely compact everything else. Reuses the *existing*
  conservative C-stack scan (12–27% pinned, already measured), so it ships the RSS win
  with NO handle migration — but leaves the pinned fraction unreclaimed, and does not by
  itself give codegen its precise stack maps.
Likely split: **Bartlett for the RSS exploit (S2, early, cheap); precise handles +
stack maps for codegen (S1+S3, the full foundation).** S0.3 commits the choice.

## Phases, dependencies, gates

Critical path: **S0 → S1 → {S2 ∥ S3} → S4.** S2 (RSS) can start on the Bartlett path
after S0.3 without waiting for the full S1 handle migration; S3 (codegen) needs S1's
stack-map protocol. Every code phase: full 22-suite `--brute` is the correctness gate;
perf only on darwin-4; each phase has a pre-committed Rule-0 threshold.

### S0 — De-risk & decide (cheap; go/no-go)
- **S0.1** Warm peak-time reclaimable-ceiling: a flag that runs ONE full MARK (no move)
  at peak RSS in WARM mode and reports live/dead arena bytes for firefox+M5+HNE. Tightens
  the RSS ceiling from the estimate to a firm number. **GO threshold:** warm-peak dead
  fraction ≥ 30% AND projected M5 RSS ≤ 2.0× (else RSS half not worth the foundation).
- **S0.2** Re-entrant root census: extend `NIX_V3_MIDEVAL_AUDIT` to enumerate every
  primop site holding a live v3 Value across a re-entrant eval callback; count sites +
  cells; classify movable-needed vs pin-OK. Sizes S1.2 and the Bartlett pinned-fraction.
- **S0.3** Foundation architecture decision doc: Bartlett vs precise-handles (per axis);
  safepoint poll placement; stack-map representation; trigger/cadence policy. Pre-commits
  the architecture + the per-axis go/no-go.

### S1 — Precise roots + safepoint protocol (the foundation)
- **S1.1** Handle/HandleScope API: thread-local movable-root stack + RAII `Local<Value>`;
  the GC walks AND rewrites them on relocation. Confirm/extend `GcRoot` to be relocation-
  aware. Gate: smoke + --brute unchanged (no behavior change yet).
- **S1.2** Migrate re-entrant primop sites (S0.2 census) to handles — bulk mechanical,
  one site at a time, **auditor-gated** (zero missed/un-updated roots). (Skippable for the
  RSS-only Bartlett path; required for codegen.)
- **S1.3** Safepoint poll mechanism: poll points at the alloc slow-path + loop back-edges
  + call boundaries; a "GC-requested" flag; the collector runs ONLY at polls. Gate:
  --brute + byte-id; measure poll overhead (must be < ~2% wall).
- **S1.4** Validate the foundation with a NON-MOVING mid-eval mark-sweep at `exitDepth>0`
  (prove root completeness before risking moves): auditor ZERO missed roots mid-eval
  across full --brute + a nixpkgs soak. **The foundation's correctness gate.**

### S2 — Compacting mid-eval GC (RSS exploit)
- **S2.1** Mid-eval moving compactor: sliding mark-compact (or Bartlett mostly-copying
  per S0.3) — relocate live cells + fixup pointers via the precise walker, run at
  safepoints `exitDepth>0`. Gate: --brute under moving stress + byte-id.
- **S2.2** Whole-block-free + `munmap` + trigger/cadence policy: return emptied blocks to
  the OS (the actual RSS realization, impossible before because blocks were never fully
  dead) + a heap-pressure trigger balancing RSS vs GC-CPU (the tension).
- **S2.3** Validate + measure RSS on darwin-4 vs the S0.1 ceiling; --brute 22/22 +
  byte-id; ship gated. **SHIP gate:** realized warm RSS reduction ≥ 70% of the S0.1
  reclaimable ceiling on M5+firefox.

### S3 — Native codegen (CPU exploit; absorbs the old C4 JIT tasks)
- **S3.1** (was #152) Stack-map generation for JIT'd native code: at each safepoint in
  generated code, emit live-v3-ptr maps (registers/spill slots) so the GC finds+updates
  them — J3, built on S1's protocol. Gate: GC stress over JIT'd frames, auditor-clean.
- **S3.2** (was #153) Codegen backend integration: compile hot bytecode bodies (J1/J2
  NaN-box codegen already prototyped + byte-id on aarch64), value-stack ABI trampoline,
  hot-callCount compile trigger, deopt/bail. Gate: --brute + byte-id with JIT on.
- **S3.3** (was #154) Validate JIT under moving-GC stress + measure CPU on darwin-4 + ship
  gated. **SHIP gate:** realized warm CPU reduction ≥ 15% wall on firefox+M5, net of
  safepoint-poll overhead.

### S4 — Integration
- **S4.1** Combined warm head-to-head vs TW with compacting GC + codegen both on; tune the
  RSS/CPU Pareto point (GC frequency); measure the realized both-axes position against the
  estimate (RSS ~1.34–1.7× / CPU ~1.4–2.0×). Document beat-or-compete verdict.

## Honest expected outcome

Competitive (~1.3–2× both axes), a modern movable-GC + native-codegen substrate, and the
prerequisite for the long-horizon incremental/parallel-eval vision (both need precise
movable roots too). NOT a guaranteed <1× on RSS — the residual (heavier live
representation + bytecode metadata a tree-walker lacks) needs the container-shrink +
churn work on top. S0 is a real go/no-go: if the warm-peak reclaimable ceiling is below
threshold, the RSS half does not justify the foundation and only the CPU (codegen) half
proceeds.
