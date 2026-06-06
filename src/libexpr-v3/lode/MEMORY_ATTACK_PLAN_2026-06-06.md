# v3 memory attack plan — critically reviewed, measure-first (2026-06-06)

**Status:** ATTACK PLAN. The register-VM wall arc is complete (v3 beats TW
1.54× on *compute*; real-nixpkgs drvPath is overhead/memory-bound and saw
little of it). The bigger remaining gap is **memory**. This doc reviews the
fresh peak-RSS/arena measurements, **corrects two errors in the first read**,
and sets a falsifiable goal + a measure-first plan. The GC track is paused
([[GC_PAUSE_2026-05-29]]) after real falsifications — so this is deliberately
**not** "restart the GC"; §3 says what is actually different now.

---

## 1. Measured facts (peak RSS is load-insensitive — solid even at load 40)

| workload | TW | v3 | ratio |
|---|---|---|---|
| hello.drvPath | 142 MB | 669 MB | **4.7×** |
| firefox.drvPath | 299 MB | 818 MB | **2.74×** |

- **The ratio COMPRESSES for heavier workloads** (4.7× → 2.74×): v3's overhead
  is more fixed (arena baseline + a Boehm reservation), TW scales with the
  eval. So the often-quoted "4.4–5.3× RSS" is **hello-specific**; the heavy
  workloads that matter (firefox, and presumably cardano/M5) are ~2.7× — and
  likely lower still as size grows. Memory ratio is **not** workload-invariant
  (unlike the opcode mix).
- **Arena per-tag (firefox, 772 MB arena):** Bindings **534 MB (69%)**, thunks
  107 (14%), pairs 49 (6%), lists 18, closures 16, chars 15. **Bindings is the
  consumer.**
- **Bindings size histogram (firefox, count):** mode is *small* (2-elem =
  104 419; 1-elem = 16 234) but there is a heavy *huge* tail — **129+ : 5 798**,
  65-128 : 2 801, 33-64 : 12 522.
- **Boehm heap 403 MB is 99.97% FREE** (`boehm_free=402.8`) — mostly reserved/
  non-resident address space, **not** real consumption. The resident peak is
  the arena.

**Bytes vs count (inference, flag for confirmation):** the non-huge buckets are
count-heavy but byte-light (~40 MB total, count-weighted). That leaves **~490
of the 534 MB Bindings in the ~5 800 huge (129+) attrsets** (≈85 KB / thousands
of entries each). For a *single* firefox eval, ~5 800 huge attrsets ⇒ the
**overlay/fixpoint copy problem**: `self // super`, `pkgs // overrides`, module
merges each materialise a fresh copy of a big binding array. This matches the
prior per-Tag decomp ("84% Bindings, ~6 000 huge 129+") and the
[[#455 family]] `prev // overlay` shape. **CONFIRM with a bytes-per-site
instrument (S1) before acting — this split is inferred from the count
histogram + entry-size estimate, not directly measured.**

---

## 2. Critical corrections to the first read (the load-bearing part)

**CORRECTION 1 — the "approach TW-parity" claim was WRONG.** The first read
said: "peak 818 MB ≫ live 287 MB ⇒ ~530 MB dead churn ⇒ a GC approaches TW-
parity." That conflated two different measurements. The **287 MB is the
`NIX_V3_MEM_BUCKETS` post-teardown RESIDUAL** (the CU/import-cache result graph,
measured *after* VMState is torn down — its own header says "RESIDUAL lower-
bound (VMState torn down)"). It is **not** the mid-eval reclaimable. Much of the
(818 − 287) gap was **live working set during eval**, which a mid-eval GC
cannot reclaim. Using the post-teardown residual as the reclaim target is the
same category error this project keeps catching ([[measure-twice-cut-once]]).

**CORRECTION 2 — the realistic GC reach, from the actual live-fraction data.**
The right number is the measured live fraction L(t) ([[L_TIME_SERIES_DATA_2026-05-29]]):
**L = 0.41–0.69** ⇒ dead fraction **31–59%**. A GC reclaims at most that
fraction of the arena (an **upper bound** — the prior Immix work showed
*realised* reclaim < projection: F3 post-GC-peak failed on hello):

| | arena | live (L×) | GC-reclaim ceiling | peak after | ratio vs TW |
|---|---|---|---|---|---|
| firefox | 772 MB | 317–532 | 240–455 MB | ~363–578 MB | **~1.2–1.9×** |
| hello | 520 MB | 213–360 | 161–307 MB | ~362–509 MB | ~2.5–3.6× |

So the GC's *ceiling* is **near-parity for HEAVY workloads** (firefox 1.2–1.9×)
and weaker for light (hello 2.5–3.6×). That's a meaningful win — but it is a
ceiling, not a promise, and it is bigger exactly where the prior hello-based
SHIP gates were measured weakest.

**Net:** memory is real and Bindings-dominated, the GC reach is genuinely good
for heavy workloads, but the parity framing was an over-claim. The honest
target is "roughly halve heavy-workload peak," not "match TW."

---

## 3. Why this is hard — and what is actually different now

Both obvious levers are already paused/falsified:
- **GC track PAUSED** ([[GC_PAUSE_2026-05-29]]): 6 falsifications + 0 MB
  shipped; the Immix SHIP-gates were **hello-based** (F1 line-occupancy PASSED
  at 46–50% dead, but F2 sweep-cost / F3 post-GC-peak failed on hello).
- **Persistent / HAMT attrsets: 4×-falsified** (the Chain/persistent-overlay
  path).

So this is **not** "build the GC" or "build HAMT." Two things genuinely change
the calculus, and only these justify re-opening:
1. **The bytes are concentrated, not diffuse** (inferred): ~85% of the Bindings
   bytes sit in ~5 800 huge attrset copies. A concentrated target admits a
   *structural* fix (a few hot copy-sites) that a diffuse one does not — this
   is a different lever than the falsified general GC.
2. **The reach is heavy-workload-favourable** (§2): the prior gates were judged
   on hello, where the GC is weakest (2.5–3.6×); on heavy workloads the ceiling
   is ~1.2–1.9×. The decision must be re-judged on heavy-workload data.

---

## 4. GOAL (falsifiable, pre-commit before building)

**Halve v3 peak RSS on the heavy production target.** Concrete pre-committed
gate: **firefox.drvPath from 2.74× → ≤ 1.6× TW** (≈ 818 → ≤ 480 MB), AND the
equivalent absolute reduction on M5/cardano keeping it well under the 4 GB
watchdog — by attacking the huge-attrset Bindings bytes. Measure peak RSS via
`/usr/bin/time -l` (fair metric); byte-identical eval results throughout. If
the chosen lever can't credibly clear ≤1.6× on firefox, do not build it.

---

## 5. NEXT STEPS — measure-first spike (do NOT build blind)

**S1 — bytes-per-allocation-site attribution of the huge (129+) Bindings.**
*The decisive measurement.* Where do the ~5 800 huge attrset copies come from —
`self // super` overlay chains, `// overrides`, module merges, `derivationStrict`,
repeated construction? Extend the per-Tag instrument with an allocation-site
tag (record the emitting CU/op or a coarse C++ call-site) and bucket the
huge-Binding **bytes** by site. ~1–2 days. **This determines the lever:**
concentrated (a few sites) ⇒ structural; diffuse ⇒ GC.

**S2 — L(t) + realised-reclaim on firefox + M5 (NOT hello).** The prior L(t)
was hello/HNE. Re-run `NIX_V3_LIVE_TRACE_PERIODIC` on the heavy workloads to get
their live-fraction curve and re-derive the GC SHIP-gate against *their*
reclaimable bytes (and confirm the §2 ceiling estimate is realistic for them).
~1 day; the instrument exists.

**S3 — decision fork (pre-committed on the S1/S2 results):**
- **Concentrated (S1: a few // / merge sites dominate the huge-Binding bytes)
  ⇒ STRUCTURAL lever.** Copy-on-write / shared-base `//` for large attrsets:
  keep the base binding array shared, record the overlay as a delta, materialise
  lazily / only on divergent read. This is the *narrow* cousin of the falsified
  full HAMT — it shares one hot structure rather than rewriting all attrsets —
  and it attacks the dominant bytes directly without a general GC. Re-check it
  is not subsumed by the HAMT falsification (HAMT replaced the representation;
  COW-on-// keeps it and shares the base).
- **Diffuse (S1: huge attrsets from many distinct, genuinely-transient sites)
  ⇒ revisit Immix** with the S2 heavy-workload SHIP-gate. F1 (line occupancy)
  already passed at 46–50% dead; the blockers (F2 sweep-cost, F3 post-GC-peak)
  were hello-measured — re-evaluate on heavy, where the reclaimable is larger.

---

## 6. What NOT to do (falsified / mistaken — keep dead)

- **Blind GC-variant sequencing** — killed ([[GC_PAUSE_2026-05-29]]); only
  re-open via S2's heavy-workload gate.
- **Full HAMT / persistent-attrset rewrite** — 4×-falsified.
- **Hello-only SHIP gates** — the ratio compresses and the reclaimable
  concentrates differently on heavy workloads; gate on firefox/M5.
- **Reading the 403 MB Boehm heap as live** — it's 99.97% free.
- **Treating the 287 MB MEM_BUCKETS residual as the reclaim target** — it's the
  post-teardown CU-cache, not the mid-eval reclaimable (Correction 1).
- **Promising TW-parity** — the L(t)-bounded ceiling is ~1.2–1.9× heavy /
  2.5–3.6× light, and realised < ceiling.

---

## 7. Reproduce / tooling

```
# peak RSS (fair metric) + v3 internal breakdown
NIX_V3_DIRECT_EVAL=1 NIX_VM_STATS=1 NIX_V3_MEM_BUCKETS=1 \
  /usr/bin/time -l ./build/src/nix/nix eval --impure \
  --expr '(import <nixpkgs> {}).<pkg>.drvPath'
#   -> "maximum resident set size"; "v3-direct memory:" (arena/boehm split);
#      "per-tag bytes"; "bindings size hist"; MEM_BUCKETS resident decomp
# live-fraction time series (S2)
NIX_V3_LIVE_TRACE_PERIODIC=<K> …
```
Instruments: `NIX_VM_STATS` (per-tag arena bytes + bindings/attrset
histograms + elsewhere-probe), `NIX_V3_MEM_BUCKETS` (post-teardown resident
decomp), `NIX_V3_LIVE_TRACE_PERIODIC` (L(t)). S1 needs a new per-site byte
tag on the huge-Binding path.

Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.
SPDX-License-Identifier: Apache-2.0
