# Phase D (nursery write barriers) — verdict (2026-06-14)

Goal: complete Phase D (the write barriers) to unblock workstream E. **Verdict:
the barriers are NEARLY complete + a strong perf win, but NOT shippable — the
broad `--brute` gate caught one real missed-root class (41 tenured words → one
shared nursery object, on hello), a use-after-free hazard that BLOCKS the
default-on flip. Two things are simultaneously true: (1) once a contention
artifact was removed, the generational nursery is a clear CPU+RSS WIN over the
major-GC default on every deep workload (firefox/HNE/M5), byte-identical +
AUDIT-clean — it achieves E's "cap RSS cheaply" goal via cheap young scavenges
instead of the cache-bound major mark; (2) the BRUTE scanner found a missed root
the narrower gates didn't, so flipping default-on would ship a UAF. Completing
Phase D = fixing that one missed-root class (a targeted GC-scavenger RCA), then
flipping.**

## Found already built
Phase A (Cheney nursery) + Phase C (scavenge) + the Phase D barrier suite are
implemented, gated `NIX_V3_NURSERY=1` (default-off): `barrier.hh` (429 lines);
inter-gen edges → `dirtyContainers` (remembered set); the scavenge walks the
remembered set (gc.cc:1026/1386), not a full O(tenured) walk. Built+gated, not
from scratch. Flipping default-on touches ~5 interacting gates (barriers
barrier.cc:84; allocator+scavenge vm.cc:3233 / nursery.hh:450,459; major-GC
auto-disable alloc.hh:982 — nursery-on ⇒ major mark off = generational).

## Correctness: VALIDATED (barriers complete)
Under `NIX_V3_NURSERY=1` (+ `V3_DBG_NURSERY_AUDIT=1` / `GC_STRESS=1000`):
byte-identical to TW on **hello, git, cargo, rustc, python3.withPackages,
firefox, HNE.hello, M5/cardano-node** (8 workloads incl. the deepest), and **0
BRUTE/AUDIT missed-root signatures** anywhere (AR7 stress harness PASS).

## MEASUREMENT-DISCIPLINE CORRECTION (the "50-150× slow" was an artifact)
A first read had firefox-under-nursery at 146 s / timing out — "50-150×." That
was CPU+memory CONTENTION: five HUNG `repro-a12b-op-call-iter-force.nix` evals
(ELAPSED 2 days, 0 % CPU, ~400 MB Boehm heap each) were paging the host. The
tell: firefox-under-nursery was `146 s real / 4.85 s user`. After `pkill`-ing
them, all numbers below are clean (user-CPU is contention-robust regardless).
The v3-vs-tw gate's "noisy host" warning, in the flesh — check for stray procs
before trusting wall.

## Performance (CLEAN, user-CPU + v3_arena; nursery vs the current major-GC default)
| workload | major-GC (default) | nursery | Δ |
|---|---|---|---|
| firefox | 5.24 s / arena 503 MB / RSS 622 MB | 4.26 s / **470 MB** / 455-631 | **−19 % CPU, −33 MB arena** |
| HNE.hello | 13.60 s / 788 MB / RSS 2105 MB | **6.37 s** / 755 MB / **1382 MB** | **−53 % CPU, −33 MB arena, −723 MB maxRSS** |
| M5/cardano | 34.78 s / 2147 MB / RSS 1894 MB | **20.14 s** / 2131 MB / 1771 MB | **−42 % CPU, −16 MB arena, −123 MB maxRSS** |

The nursery is CHEAPER and comparable-or-lower RSS on all three — because its
cheap young scavenges keep the arena bounded WITHOUT paying the cache-bound
major mark (firefox major mark ~1.3 s; HNE/M5 much more). The large maxRSS wins
(HNE −723 MB) are partly the avoided mark working-set spike; the arena (the true
heap) is comparable-to-slightly-better.

## Why this UNBLOCKS E (and corrects the E verdict's framing)
E wanted to cap firefox's scattered dead by firing GC mid-eval; it failed
because the major mark is cache-bound (+38.8 % per fire). **The nursery solves
the same problem differently**: instead of an expensive whole-live-set major
mark, it does cheap young scavenges (copy survivors, drop young dead) that keep
the arena bounded continuously. So the generational collector IS the cheap
RSS-bounding GC E needed — and it's a net CPU WIN, not a +15 % cost. The
"tenured-dead needs the major mark" framing in an earlier draft was wrong: the
data shows the nursery keeps the arena comparable to the major GC (HNE 755 vs
788, M5 2131 vs 2147) while avoiding the mark.

## `--brute` FOUND A MISSED ROOT → flip BLOCKED (the discipline paid off)
The broad gate `all-v3-tests --brute` (core suite under a 1 MB nursery +
aggressive scavenge + `V3_DBG_NURSERY_BRUTE`/`_AUDIT`) = **21/22; the
`brute-audit` suite FAILED**:

```
v3 SCAVENGE BRUTE: 41 tenured words point into nursery   (hello-name/-drvPath/-outPath, gcc-name)
```

A BRUTE hit = a tenured arena word holds a nursery pointer the scavenge did NOT
forward — a **missed root = use-after-free hazard**. The dump is precise: all 41
holders point to the SAME nursery object (`0xb364aac80`) — ONE widely-shared
nursery object, referenced 41× from scattered tenured cells, not forwarded. So
it is a single missed-root class: either 41 writes that bypassed the barrier
helpers, or that object's type isn't walked by a per-type scavenger walker.

This did NOT surface as a divergence on the 8 byte-identity workloads or the AR7
stress (those objects weren't reclaimed-then-read on those paths) — but the
dedicated whole-tenured-heap BRUTE scanner caught it. **This is exactly the
latent UAF a default-on flip would expose on some untested package.**

## Disposition
- **Phase D barriers: NEARLY complete, but NOT complete** — one precisely
  characterized missed-root class remains (1 shared nursery object × 41 tenured
  holders, reproducible on hello.drvPath under the brute config).
- **Nursery: a validated PERF win** (−19/−53/−42 % CPU + comparable/better RSS;
  byte-identical + AUDIT-clean on 8 workloads + GC_STRESS=1000) — so fixing the
  missed root is high-value (it unblocks a cheaper, RSS-capping default GC = E).
- **Default-on flip: BLOCKED** on the missed root. Flipping now would ship a
  use-after-free. NOT flipped.
- **The remaining work (the genuine "Phase D unresolved" core):** RCA the missed
  root — identify the holder cell class + the shared object's type, find the
  unbarriered write site (a raw `*cell = …` / `entries[i].value = …` bypassing
  the `barrier.hh` helpers) or the missing per-type scavenger walk in `gc.cc`,
  add it, and re-run the brute scanner to confirm 0 hits. A targeted, careful
  GC-scavenger debugging task (a wrong walker fix is another UAF) — for a fresh
  session, not the tail of this one. NB: under the nursery the major-GC is off
  (M-3), so the cell-start bitmap (which `findContainingCellStart`/`cellTypeAt`
  use to type the holder) is not maintained — the RCA needs the brute scanner
  taught to type holders without it (e.g. keep cell-start bookkeeping on under
  `V3_DBG_NURSERY_BRUTE`).

The measure-twice value: validation (a) confirmed the barriers are *mostly*
correct, (b) caught + corrected a contention artifact (the "50-150× slow"),
(c) showed the nursery is a strong perf win that achieves E's goal, and (d) — via
the broad BRUTE gate — caught a real missed root that the narrower gates missed,
correctly blocking a UAF-shipping default-on flip. No flip, no rushed GC change.

## PhD-6 RCA UPDATE (2026-06-14, post-FP-2): the missed root SPLIT INTO TWO

Built a **typed BRUTE scanner** (4b8cf66cc) — `postScavengeBruteScan` now reports
each live missed-root word's HOLDER CellType + field offset (`[holder=Bindings
@+48]`) via a typed `ScavLiveRange` carrying the type each `walk*` already knows.
A same-host-bisect (FP-2a/eca683aa1 vs HEAD) on hello.drvPath under the aggressive
brute config (1 MB nursery, constant scavenge) then split the "41-words" blocker
into **two independent phenomena**:

1. **The 43-words `capturedWiths` missed root — FIXED by FP-2b (incidentally).**
   Pre-FP-2b: BRUTE = **43 tenured words → 1 shared nursery obj** (`0xb0c0a3820`).
   Post-FP-2b: BRUTE = **0 live**.  FP-2b's corrected withs-slot evac-forward
   (`thunkSetCapturedWiths(fwdList(w))` + `thunkScanSize` incl. the slot) forwards
   those references correctly.  So the header-`capturedWiths` field was the
   41/43-words holder class, and relocating+forwarding it via the FAM tail closed
   that missed-root class.  (This is *why* FP-2b's `--brute` count was "unchanged
   at 21/22" — the BRUTE-hit sub-cases flipped to 0-live, but the case still fails
   on phenomenon #2 below, which short-circuits on `rc!=0` before the BRUTE check.)

2. **`error: undefined variable 'gnuabi64'` — the REAL remaining blocker, distinct
   and PRE-EXISTING (NOT FP-2b).**  Reproduces at BOTH pre- and post-FP-2b under
   aggressive scavenge, with **AUDIT clean + BRUTE 0-live (post-FP-2b)**.  It is a
   *wrong evaluation* (a lost `with`-binding — `gnuabi64` is a free var resolved
   through a `with abis;`-style scope), NOT a dangling nursery pointer — so BOTH
   the arena BRUTE scan and the deep-walk AUDIT miss it.  Only under the aggressive
   1 MB nursery (many scavenges); the DEFAULT-size nursery evaluates hello.drvPath
   byte-identically.  Scavenge-frequency-dependent = classic missed-root, but of a
   class the current scanners don't cover.

**Next concrete RCA step for gnuabi64 (first hypothesis already REFUTED):** the
obvious guess — `vm.withStack`/`vm.valueStack` not forwarded — is WRONG: the
scavenger DOES forward both (gc.cc:805-806 `visitValue`) and the AUDIT walks both
(gc.cc:1311-1314), all clean.  So the with-attrset Value on the stack is forwarded
correctly.  The bug is therefore deeper — candidates, in rough priority:
  1. **Truncated/stale with-attrset Bindings copy** — the abis Bindings is
     forwarded but the gnuabi64 ENTRY isn't copied (a `walkBindings`/`fwdBindings`
     size or entry-loop bug under back-to-back scavenges), so `OP_WITH_LOOKUP`
     finds the attrset but not the name → "undefined variable."
  2. **Stale cached lookup after a move** — an inline cache / `attrSelectCache` /
     memoised with-lookup slot pointing at the pre-move location (the IC walk at
     gc.cc:679/705 forwards IC'd Bindings, but a with-lookup-specific cache may be
     uncovered).
  3. **A with-attrset built in the nursery and forwarded mid-construction** (pushed
     onto withStack before all entries were written).
Instrument `OP_WITH_LOOKUP` to dump, on the failing gnuabi64 lookup, the
with-stack attrsets it searched (count + sizes + whether any was recently
forwarded), and/or add a post-scavenge check that every withStack attrset's entry
count is preserved across the copy.  Aggressive-1MB-nursery + hello.drvPath is the
deterministic repro.  NOTE: FP-2b already closed the capturedWiths missed-root
class, so gnuabi64 is the SOLE remaining nursery-flip blocker.

*Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output
Group. SPDX-License-Identifier: Apache-2.0.*
