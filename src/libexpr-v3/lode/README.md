# `lode/` -- engineering logbook (historical)

Files in this directory are **point-in-time artifacts** -- review snapshots,
benchmark logs, fix-plan postmortems.  They were accurate when written but the
codebase has moved past them.  Don't trust them for current state.

For current state:

- `../USAGE.md` -- living usage guide (still at top level on purpose).
- `../test/wc38-bisect-README.md` -- co-located with the harness it documents.

## Reviews & audits (point-in-time)

- `REVIEW_2026-05-0{3,4,5,6,6b,7,8}.md` -- multi-agent review snapshots, in
  date order.  `*-06b.md` is a same-day adjustment doc to `*-06.md` after
  critical re-review; `*-08.md` is the latest (six-agent pure-VM-goal audit,
  with a same-day update block at the top reframing recommendations + verdict
  around the v3-direct inversion that landed hours after publication).
- `INVERSION_PLAN_2026-05-08.md` -- forward-looking plan: v3 as host,
  TW as leaf.  Phase 1 (`NIX_V3_DIRECT_EVAL` for `nix eval --expr/--file`)
  landed same-day; Phases 2–4 enumerated.
- `TRAFFIC-OWNERSHIP-REVIEW.md` -- pre-#426; central thesis ("call-hook never
  wired") was contradicted by the actual code at the time of writing.
- `GC-REVIEW.md` -- GC roadmap; Phase 0 (CRIT-2/3/4 arena correctness) closed
  via #432.  Phase 1+ (runtime GC redesign) still future.
- `V3VALUE_CANON_AUDIT.md` -- audit of `v3::Value` canonicalisation migration
  cost.  Research; layout claims still match `value.hh`.
- `TEST_INFRA_AUDIT_2026-05-08.md` -- audit of the v3 test suite: oracle
  modes (recorded golden / live TW / hardcoded literal), per-runner
  classification, gaps, ranked recommendations.

## Plans (forward-looking; check status)

- `OPTIMIZATION_PLAN.md` -- chronological engineering log; Phases 1-4 landed
  pre-2026-05-05.  Phase 5 status: see `USAGE.md` (default-on).  Forward-
  looking sections superseded.
- `FFI_PLAN_2026-05-06.md` -- header-pack design for the v3 FFI surface
  (~59 functions, ~20 types in 13 categories).
- `FFI_PLAN_2026-05-06b.md` -- same-day critical-review adjustment doc to
  `*-06.md` (13 corrections; EvalScope, fiber re-entrancy, expanded
  DerivationDescriptor, 4-variant string context).
- `WC38_FIX_PLAN.md` -- resolved by c95be6461 + 685262a6f.  Useful as a
  "what we predicted vs. what landed" cautionary case study.
- `OPT_OCCUR_PLAN_2026-05-08.md` -- step-by-step plan for the foundational
  occurrence-analysis pass (`opt_occur.cc`).  Phases A (analysis), B (DCE
  migration), C (future consumers); authorisation table; double-count
  regression contract.
- `VM_TW_MEASUREMENT_2026-05-09.md` -- progress digest (#529, #530 closed)
  + audit of v3↔TW time-share telemetry (V3_TIMING, NIX_VM_STATS,
  NIX_V3_BRIDGE_TIMING).  Identifies three gaps for "what % of eval was
  in v3 vs TW?"; proposes four ranked improvements (A: per-primop FFI
  time, B: nixpkgs bench targets, C: one-line summary, D: sampling).
- `CLEANUP_AUDIT_2026-05-09.md` -- six-agent codebase cleanup audit
  (opcodes / hook-mode / flags / dead code / tests / docs).  ~110 LoC
  deletable today; bulk milestone-gated (#498 closure, INVERSION
  Phases 2/3/4, v5 disk-cache schema bump).  Tiered action list +
  KEEP-forever inventory.

## Reports & ideas

- `OPTIMIZER_REPORT_2026-05-07.md` -- comprehensive review of the IR
  optimizer pipeline + literature survey + ranked recommendations.
- `UNISON_IDEAS_2026-05-07.md` -- five techniques to import from Unison
  (content-addressed IR, ABT identity, hash-keyed eval cache, ability
  propagation, queryable cache CLI) with prior-art viability section.
- `WITH.md` -- prior-art survey for the `with E;` feature: static
  injection (Pascal/OCaml), principled dynamic scope (Common Lisp), and
  the JavaScript-class worst case.  Concrete moves ordered by leverage.
- `TEAM_A_B.md` -- pipeline-design assessment + per-pass testability
  audit + two-team split feasibility (FFI/runtime vs optimizer/IR).
  2-week prep sprint required before split; ownership table appended.

## Benchmarks (point-in-time)

- `BENCH-2026-05-04-*.md` -- perf under specific commits / configurations.
  Superseded by `../USAGE.md` "Real-world behaviour" section (updated
  2026-05-05).

Drop new artifacts here as they age.

Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.
SPDX-License-Identifier: Apache-2.0
