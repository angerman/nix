# `lode/` -- engineering logbook (historical)

Files in this directory are **point-in-time artifacts** -- review snapshots,
benchmark logs, fix-plan postmortems.  They were accurate when written but the
codebase has moved past them.  Don't trust them for current state.

For current state:

- `../USAGE.md` -- living usage guide (still at top level on purpose).
- `../test/wc38-bisect-README.md` -- co-located with the harness it documents.

For history:

- `REVIEW_2026-05-0{3,4,5}.md` -- multi-agent review snapshots.
- `BENCH-2026-05-04-*.md` -- benchmarks under specific commits / configurations.
- `OPTIMIZATION_PLAN.md` -- chronological engineering log; forward-looking parts
  are superseded.
- `TRAFFIC-OWNERSHIP-REVIEW.md` -- pre-#426; central thesis ("call-hook never
  wired") was contradicted by the actual code at the time of writing.
- `WC38_FIX_PLAN.md` -- "what we predicted vs. what landed"; useful as a
  cautionary case study.
- `GC-REVIEW.md` -- GC roadmap; Phase 0 fixed, Phase 1+ still useful.

Drop new artifacts here as they age.

Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.
SPDX-License-Identifier: Apache-2.0
