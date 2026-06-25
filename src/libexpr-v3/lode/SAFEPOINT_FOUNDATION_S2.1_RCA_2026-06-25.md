# S2.1 — mid-eval moving compaction: works, + the brute-audit RCA (complete) — 2026-06-25

Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output
Group. SPDX-License-Identifier: Apache-2.0.

## The win (validated)

Mid-eval moving compaction WORKS. Invoking the existing cell-pin Bartlett evacuator
(`runEvacuation`) in the mid-eval path (`NIX_V3_MIDEVAL_GC=1 NIX_V3_EVAC=1
NIX_V3_EVAC_CELLPIN=1 NIX_V3_EVAC_PCT=1.0`):
- hello/firefox.drvPath BYTE-IDENTICAL under full compaction;
- synthetic (foldl' over 2M genList, correct result 1e8): evac fires mid-eval
  (`exitDepth>0`), `movedCells`>0, `blocksFreed`>0, **`freedRSS` 33–67 MB munmap'd to the
  OS**, `typed/movable=100%`.
This demonstrates the RSS reclaim the campaign said was structurally blocked.

## The ship-gate bug (brute-audit) — RCA COMPLETE

Under the brute-audit (1 MB nursery + `V3_DBG_NURSERY_AUDIT`, max moving stress), evac
fails: `hello-drvPath`/`outPath` AUDIT hits — nursery `Closure`s reachable via tenured
`Bindings[(no-origin)].entries[].value`.

Decisive measurements (this session):
1. **Isolated to evac**, not GC frequency: low mid-eval threshold WITHOUT evac =
   brute-audit 17/17 clean; WITH evac = fail.
2. **Root-label instrument** (committed `1e1696c4d`): holders reachable via
   `[root=currentVm]` and `[root=importCache]` — i.e. from a SCAVENGER root.
3. **`scav=` instrument** (snapshot the scavenge's `walked` set): every hit is
   **`scav=0`** — the scavenge NEVER scavenged the holder.
4. **Not a relocated cell**: re-dirtying *all* relocated cells (fix-2, a superset) left
   the holders `scav=0` → they are absent from evac's forward map → they are **ORIGINAL,
   non-relocated tenured Bindings**.
5. **`lastWriter=(no-recorded-writer=raw/bulk-path)`**: the nursery edge was written by a
   path that BYPASSED the write barrier → the Bindings was never put in the remembered set.

Mechanism (code-confirmed): `fwdBindings` (gc.cc:538, phaseDStep7) RETURNS a root-reached
tenured Bindings **without scavenging it** — tenured→nursery edges are covered ONLY by the
remembered set. An original tenured Bindings with a raw/bulk un-barrier'd nursery edge is
therefore in neither the root-walk's scavenge nor the remembered set → never forwarded →
the audit's deep walk catches it.

## Conclusion: it's a PRE-EXISTING latent barrier gap, surfaced by evac — NOT an evac bug

The holder is original (not moved by evac) with an un-barrier'd edge. So the defect is a
**raw/bulk write path that stores a nursery `Closure` into a tenured `Bindings.entries`
without the write barrier** (so the cell never enters the remembered set). It is
latent-benign WITHOUT evac (timing/GC-pressure keeps the edge's pointee forwarded by
other means before the audit), but evac's aggressive mid-eval GC + the audit surface it.

THREE evac-side fixes were built + MEASURED INSUFFICIENT (all ruled out, not guessed):
- fix-1 forward remembered set through relocation (kept — mandatory hygiene anyway);
- fix-2 re-dirty all relocated cells (reverted — holders aren't relocated → `scav` stays 0);
- fix-3 clear attrSelect IC on mid-eval evac (kept — mandatory gen-major parity anyway).
None touch the actual defect, because the defect is upstream of evac.

## Next (the real fix)

Find the raw/bulk write site: which construction/mutation path writes a nursery `Closure`
into a tenured `Bindings.entries[].value` without `bindingsPostConstructBarrier` (or the
per-entry barrier)? Candidates: a bulk Bindings entry-fill that barriers once at
construct-time but is later MUTATED (post-construct entry overwrite — e.g. a memoizing/
mapAttrs writeback) without re-barriering. Instrument: extend the cell-write-site map to
record EVERY `Bindings.entries[].value` store (including raw ones) so the "no-recorded-
writer" entries name their writer; barrier that site; add a + / − regression (the
brute-audit IS the negative test; a targeted repro is the positive). Then the S2.1 ship
gate (full --brute 22/22 under evac + byte-id soak) should pass → S2.2/S2.3.

Note: this latent barrier gap likely affects the existing gen-major evac path too
(`NIX_V3_EVAC` was an "unrevived path"); fixing it hardens both.

Gate `NIX_V3_EVAC` default-OFF; dead by default (verified). Mover proven; ship gate
pending the upstream barrier fix.
