# RCA: the rare `brute-audit` scavenger "BRUTE hit" flake (#34, 2026-07-06)

## Symptom
Under the full `--brute` battery (1 MB nursery + `V3_DBG_NURSERY_BRUTE=1
V3_DBG_NURSERY_AUDIT=1`), the `brute-audit` suite INTERMITTENTLY fails with
`v3 SCAVENGE BRUTE: N tenured words point into nursery` (N>0) on a nixpkgs
workload — observed on `hello-outPath` (2026-07-06) and `firefox-name` (prior).
It passes on isolated re-run and is not reproducible on demand (~1-in-several
full-brute runs). Classified as a possible missed-root UAF (the PhD-6 class).

## Investigation (measured, not speculated)
`postScavengeBruteScan` (gc.cc) is a CONSERVATIVE raw-word scan: after each
scavenge it walks every 8-byte word of every LIVE tenured object and flags any
word whose value satisfies `nursery.contains(word)`. It is meant as a coarse
tripwire; the precise check is `postScavengeAudit` (a deep reachability walk
that reports `reachable via <field> [root=<root>]`).

**Reproduction attempts:** 40 firefox-name + 12 hello-outPath + 4 gcc/git runs
= 56 fresh processes, **0 hits** → the event is rarer than ~1/56 per eval and
is NOT per-scavenge (all these workloads do only 2-4 scavenges — Bindings
allocate straight to the arena, bypassing the 1 MB nursery). So it is a
per-PROCESS, ASLR-dependent all-or-nothing event.

**Near-miss instrumentation** (`V3_DBG_NURSERY_BRUTE_NEARMISS=1`,
`Nursery::minDistanceToNursery`) — for the SCALAR `{SymbolId,PosIdx32}` word of
every live Bindings entry, measure the closest approach to the nursery. On
hello-outPath (clean runs):
```
nursery young=[0xc4d800000,0xc4d900000)  scalarWords=115583
  within[4G=58 256M=0 1M=0]   closest={word=0xc0000009e dist=1.3GB}
```
Across 4 ASLR variations: **58 scalar words in the nursery's 4 GB region EVERY
run** (nursery high-32 = 0xc, matching PosIdx=0xc entries); nursery low-32
ASLR-varied (closest approach 470 MB – 1.8 GB). **AUDIT-reachable = 0 on every
run.**

## Root cause — a TOOLING FALSE-POSITIVE, not a GC bug
A `Bindings::Entry` is `{ SymbolId(4B), PosIdx32(4B), Value(8B) }`. The raw-word
scan reads the packed `{SymbolId,PosIdx32}` word (a NON-pointer) at each entry's
`+0` offset as `word = SymbolId | (PosIdx32 << 32)`. That word lands in the
nursery's 1 MB range exactly when ASLR places the nursery such that:
- `PosIdx32 == nurseryBase >> 32` (a matching PosIdx exists — 58 do on hello),
  AND
- `SymbolId ∈ [nurseryBase & 0xFFFFFFFF, +1MB)` — i.e. nursery low-32 falls in
  the SymbolId range (`< ~0x180000`, since SymbolIds are 0..~500K).

The nursery low-32 is uniform-ish over ~4 GB, so P(hit) ≈ `~1.5MB / 4GB` ≈
**~0.04-0.07% per eval** — matching "seen ~once across many brute runs."

It is NOT a real missed root: the scalar word is not a pointer, the scavenger
correctly ignores it, and the precise AUDIT deep-walk (which tag-decodes Values,
never raw metadata words) is CLEAN on every run. Confirmed independently for
`firefox-name` (prior flake) — same nixpkgs Bindings-entry mechanism.

**Corroboration from the original symptom.** The failing run reported `60
tenured words` then `80 tenured words` (two consecutive scavenges) — NOT 1. A
single random collision would give ~1 hit; 60/80 is the WHOLE clustered set:
the 58 in-region scalar words all sit at `0xc_000XXXXX` (SymbolIds 0..~500K =
a ~500 KB span), so when ASLR drops the nursery's 1 MB window over that cluster,
~all of them collide at once (and the count grows 60→80 as more entries tenure
between scavenges). This all-or-nothing burst is exactly what the mechanism
predicts and what "rare, non-deterministic, ~60-80 at a time" looks like.

## Fix (gc.cc, `bruteScanSlotIsScalar`)
Make the BRUTE scan TYPE-AWARE: skip provably-NON-POINTER scalar/metadata slots
(a Bindings entry's `{SymbolId,PosIdx32}`, header kind/size/count, closure
`{nUpvalues,_pad}`, env `{isWithEnv,nValues}`, list `{size,_pad}`, thunk header
word). A pointer never lives in these slots, so skipping them CANNOT hide a real
missed root; the AUDIT deep-walk remains the precise reachability check, and
pointer-capable slots (Bindings.value/parent, Closure.upvalues/desc/upvalEnv,
etc.) are still scanned. Filtered hits are reported separately
(`... scalar-slot words coincided ... FILTERED`).

The classifier's per-type offsets encode the CURRENT cell layouts (Bindings
header 16 B post-P1a; Closure header 32 B post-P1b) and are unit-tested in
`test/smoke.cc::testBruteScanScalarClassifier` to guard against layout drift —
a future header change that shifted these offsets would either re-expose the
flake (scalar slot not skipped) or hide real misses (pointer slot wrongly
skipped), and the test would catch it.

## Diagnostics added (permanent tooling)
- `postScavengeBruteScan` now decodes each remaining (pointer-capable) hit as a
  Value and prints its tag / ptr-tagged status.
- `V3_DBG_NURSERY_BRUTE_NEARMISS=1` reports, per scavenge, how close scalar
  words come to the nursery (the evidence tool for this RCA).
- `Nursery::minDistanceToNursery` / `youngLo` / `youngHi`.

Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
Input Output Group. SPDX-License-Identifier: Apache-2.0.
