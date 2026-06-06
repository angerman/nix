# v3 memory attack plan — critically reviewed, measure-first (2026-06-06)

> ## ⚠️ LOAD-BEARING CAVEAT: most prior memory work is PRE-REGISTER-VM
> The register-VM rework (Phase 5, 2026-06-06: R_PRIMOP2/R_CALL/register-mode If/
> R_STR_CONCAT2/R_MOVE/GET_UPVALUE_REC_BINDING_SLOT — fib now runs with the
> operand stack fully dropped) **significantly changed the call / formals /
> arg-passing / Bindings-flow paths**.  Nearly every memory finding this plan
> cites pre-dates it and **must be treated as STALE until re-measured on the
> current tree**, specifically:
> - the **5× Chain Phase C falsifications** (EXIT_PHASE_C_4_FALSIFIED, vm.cc
>   ledger) — all on the OLD VM; the unidentified `f origArgs → {}` collapse
>   lived in exactly the call/arg path that R_CALL reworked.  **RE-TEST FIRST.**
> - the **GC falsifications + Cheney/Immix SHIP-gates** ([[GC_PAUSE_2026-05-29]],
>   STAGE_6_FALSIFIERS) — pre-VM-rework; arena/Bindings churn may differ now.
> - the **L(t) live-fraction data** ([[L_TIME_SERIES_DATA_2026-05-29]]) — pre-rework.
> - the **per-tag / mergeBindings attribution** (#821, HNE_MEMORY_ATTRIBUTION) —
>   pre-rework (though S1 below RE-MEASURED `//`-dominance on the current tree).
> Rule for this plan: a prior falsification only stands if it is RE-CONFIRMED on
> the register-VM tree.  The register VM is a genuine new-insight per the
> falsification rule for re-opening these.

> ## ✅ FINAL VERDICT (2026-06-06): structural Chain lever — BUILT, byte-identical, NEUTRAL → DO NOT SHIP
> The structural lever was carried all the way to a CORRECT, byte-identical
> implementation (something the 5 prior attempts never reached) and then MEASURED
> on the gate workload:
> - **Unblocked** the "5×-falsified" Chain by isolating the real root cause —
>   chain-unsafe `entries[]` iteration sites (mapAttrs was the first miss), not an
>   architectural wall.  Fixed 8 sites; lang 142/143 with aggressive chains;
>   hello.drvPath/outPath + firefox.drvPath **byte-identical** with chains ON.
> - **Measured (true, byte-identical):** firefox.drvPath ON **972.9 MB** vs OFF
>   976.2; hello.drvPath 701.6 vs 705.7 — **NEUTRAL** (the prior −65% was a
>   confound: the bug made the eval do less work).
> - **Why neutral:** the chain avoids the `//` copy (mergeBindings 364→62.6 MB on
>   hello) but the huge `//` results are ITERATED (mapAttrs over package sets, the
>   derivation env build), and a chain-aware iteration must `materialize()` →
>   re-pays the copy.  The bytes are concentrated in `//` (S1 correct) but they
>   are NOT lookup-only — so shared-base/COW gives no net peak reduction.  To
>   realise a saving would require STREAMING (non-materialising) chain iteration
>   in every hot consumer (mapAttrs/env/foldl-over-attrs/…) — a much larger
>   rewrite than the lever, and the un-memoised materialise even REGRESSED +195 MB
>   before the memo fix brought it back to neutral.
> - **Decision (per §4 pre-committed gate "build ONLY if it clears ≤1.6×
>   firefox"):** it clears nothing (neutral) → **DO NOT SHIP.**  Chains stay gated
>   OFF (`NIX_V3_CHAIN_BINDINGS`, default off); the 8 iteration-site fixes are kept
>   (genuine chain-safety, inert when off).  Kills the hypothesis "the concentrated
>   `//` bytes admit a cheap structural COW fix" — they don't, because they're
>   iterated, not shared-read.
> - **Indicated remaining lever:** since the bytes are real + concentrated but NOT
>   cheaply shareable, the only way left to reduce them is to RECLAIM them → the
>   GC (Immix) path, re-judged on heavy workloads per S2 (§2 ceiling ~1.2–1.9×
>   firefox).  That is the next investment, NOT more structural-COW work.

> ## ✅✅ S2 DONE + OVERALL VERDICT (2026-06-07): GC also fails the gate → peak is LIVE-dominated
> S2 (end-of-eval precise live-walk on firefox.drvPath — the gate workload, on the
> register-VM tree, NOT the stale hello/HNE):
> - **Sweep at peak: 550.7 MB LIVE / 204.2 MB dead = 73% live, only 27%/204 MB
>   reclaimable.**  (Precise from-roots best case ~52% freeable → still only.)
> - **Immix line-occupancy: deadPct = 25.85% — BELOW the ≥30% acceptance
>   threshold** (Immix F1 FAILS on firefox, register-VM tree; the prior F1 PASS at
>   46–50% was hello/HNE, stale).  evac-opportunity: only 2 sparse blocks,
>   evacuable_RSS 33.6 MB.
> - **GC reclaim ceiling: 204 MB → peak 976→772 MB → ~2.6× TW** (best-case precise
>   ~1.9×).  **FAILS the §4 ≤1.6× (≤480 MB) gate.**  The §2 ceiling (1.2–1.9×) was
>   from stale hello/HNE L=0.41–0.69; firefox on the register-VM tree is L=0.73.
>
> **OVERALL MEMORY-PLAN VERDICT (S1+S2+S3 all measured):** the heavy-workload peak
> **cannot be halved by either available lever.**  firefox.drvPath's 772 MB arena
> is ~550 MB genuinely-LIVE working set (the nixpkgs eval graph; `elsewhere`=0, so
> no import-cache lever here) + only ~204 MB dead.  Structural COW is neutral (the
> `//` bytes are iterated, not shareable); GC reclaims only ~27% (live-dominated);
> Immix F1 fails (25.85%<30%).  The plan's premise — large reclaimable `//` churn —
> was a pre-register-VM artifact + a confounded reclaim estimate; the register VM
> (fewer transient thunks/closures) RAISED the live fraction.  **Reaching ≤1.6×
> would require shrinking the LIVE representation itself (HAMT-class — 4×-falsified,
> a representation rewrite), not reclamation or sharing.**  Per measure-twice, this
> is a NO-GO on all three measured levers; the measure-first plan's value here is
> preventing a multi-week Immix build (4–6 wk) that the S2 data shows would miss
> the gate.  Memory remains a known gap; no cheap lever exists on the current tree.


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

## 5b. S1 RESULT + S3 re-check (2026-06-06) — measured, decisive

**S1 (DONE, gate workload firefox.drvPath):** Bindings = 534 MB (69% of the
771.8 MB arena, exact match to §1). `mergeBindings by site` is **100%
concentrated in site [0] `OP_ATTRS_UPDATE` (`//`): 44,271 calls, 470.2 MB = 88%
of all Bindings bytes.** hello.drvPath confirms: `//` = 364 MB / 401.8 MB
Bindings (90.6%), peak 703.7 MB. ⇒ **CONCENTRATED** (not diffuse) ⇒ S3 routes to
the **STRUCTURAL lever**, not the GC. The huge tail (firefox 129+ = 5,798
attrsets) is the `//` chain-copy accumulation in the bump arena (no reclaim).

**S3 re-check (MANDATED by §5/§6) — the load-bearing correction:** the structural
"COW/shared-base `//`" lever **IS** the Chain Phase C path, which
`EXIT_MERGEBINDINGS_AUDIT_2026-05-30` records as **FALSIFIED 3×** (v1 missed a
Phase-D barrier; v2/v3 failed nixpkgs `hello.name` with `buildPythonApplication
missing` on a 2-entry Bindings). Root cause: ~**342 `entries[]` direct-access
sites** bypass the chain — an *ad-hoc whitelist* of "safe" iteration sites kept
missing one. The scaffolding ALREADY EXISTS in alloc.hh (`Kind::{Sorted,Chain}`,
chain-aware `lookup`, `forEach`, `materialize`, `entryCount`); only Chain
*creation* in mergeBindings was reverted.

**The deeper reading (vm.cc:1186-1248 ledger + EXIT_PHASE_C_4_FALSIFIED) makes
this NO-GO for a blind build.** It is **5 attempts (#1-#5), not 3**, all failing
identically (`buildPythonApplication missing`).  Critically, the v3 diagnostic
(`V3_DBG_CHAIN_SELECT=1`) confirmed **`materialize()` fires correctly** (chain
size 1-3 → materialised 41-494), AND the prime iteration suspect — the formals
destructure (vm.cc:5350) — **already materialises Chains**.  So the failure is
**NOT a missed iteration site**: the failing Bindings is a *Sorted* size-2
`{override, overrideDerivation}`, i.e. some Nix-level `f origArgs` silently
returns `{}` (→ `{} // overlay` short-circuits to the size-2 overlay) ONLY when
chains are live upstream.  **That root cause was never isolated across 5
attempts.**  Per [[measure-twice-cut-once]] §3.8 (3 strikes = falsified; this is
5) and the project's core lesson (do not circle on falsified levers), a 6th
*blind* attempt — even the compiler-enforced 342-site conversion — is **not
justified**, because the prior diagnostics show the conversion is *not confirmed
to be the fix* (materialise already fires correctly and still fails).

**The only justified next step is prereq #1: a minimal Nix-level repro that
isolates the `f origArgs → {}` collapse under chain interaction** — a focused
diagnostic effort (its own session), not part of this measurement spike, and the
genuine unblock.  Until that root cause is identified, building the structural
lever is forbidden by the plan's own §4 gate (can't credibly clear ≤1.6× if it
doesn't eval correctly) and §6.

**Net go/no-go (this spike):** S1 decisively says CONCENTRATED → structural lever
(Chain).  The structural lever was 5×-falsified — BUT all on the pre-register-VM
tree, and the prior attempts never ISOLATED the root cause.

### 5c. BREAKTHROUGH (2026-06-06) — root cause isolated; Chain UNBLOCKED

Acting on the user's insight (the VM was reworked) I re-enabled Chain
construction (`NIX_V3_CHAIN_BINDINGS=1`, nb≤4 && na≥16 → `Chain{parent=a,
overlay=b}`) on the CURRENT tree.  hello.name STILL failed identically
(`buildPythonApplication missing`) — so the rework didn't incidentally fix it,
but it gave a **reproducible failure on a well-understood VM**.  A minimal chain
repro (prereq #1 — which 5 prior attempts never built) tested each attrset op
nixpkgs uses: lookup ✓, attrNames ✓, intersectAttrs ✓, formals-destructure ✓,
removeAttrs ✓, hasAttr ✓ — **but `mapAttrs` on a chain FAILED** (returned a
parent-less attrset).  Root cause: **`primMapAttrs` iterates `src->entries[]`
directly over `src->size`, which for a Chain is the OVERLAY ONLY** — silently
dropping the parent.  nixpkgs maps over chained package sets pervasively ⇒ the
`f origArgs → {}` collapse.  This is the iteration-site class the ad-hoc audits
kept missing.

**Fix** (primops.cc `primMapAttrs`): `if (src->isChain()) src = materialize()`.
⇒ hello.name / hello.pname / firefox.name now PASS with chains ON (byte-identical
to OFF).  **Measured benefit (hello.drvPath, chains ON vs OFF):** peak RSS
**692.6 → 245.0 MB (−65%)**, arena 520 → 151, Bindings 401.8 → 102.0 (−75%),
mergeBindings 364 → 49.9 MB (−86%).  (Confounded: hello.drvPath ON still diverges
to a fake-store path — MORE chain-unsafe iteration sites remain in the derivation
path — so the −65% overstates the pure-chain win until those are fixed; but the
magnitude decisively clears the §4 ≤1.6× gate and justifies completing the
~20-30-site iteration audit.)

**Status:** Chain lever GO.  Remaining: fix the rest of the chain-unsafe
iteration sites (drvPath path) via the same minimal-repro method, get
hello/firefox.drvPath byte-identical, measure the TRUE benefit, `--core` green,
then decide default-on. The 5× "falsification" is RETIRED — it was an
unidentified iteration site (mapAttrs ++), not an architectural wall.

### 5d. AUDIT PROGRESS (2026-06-06) — 7 sites fixed; lang clean; derivation site remains

Method: env-tunable thresholds (`NIX_V3_CHAIN_MIN_NA` / `MAX_NB`) → run the lang
suite (143 minimal tests) with AGGRESSIVE chains (na≥2) against the TW oracle;
each failure pinpoints a chain-unsafe iteration site.  Sites fixed (all
`isChain()`-guarded → default chains-OFF behaviour provably unchanged: smoke PASS,
hello.{name,drvPath} byte-identical to pre-change):
1. `primMapAttrs` — the root-cause miss (overlay-only map).
2. `primDerivationStrictNative` — materialise `src` (drvPath built by iterating it).
3. `forceDeepRec` (deepSeq) — forEach so parent values are forced.
4-6. `print.cc` — plain printer (×2) + JSON `toJsonValue` + the `--strict`
     deep-force worklist (enqueue the chain parent) → full output.
7. `primScopedImport` — materialise the scope (else lower-time `unbound 'range'`).

Result: **lang 142/143 with aggressive chains** (only pre-existing
`eval-okay-types`, identical OFF); hello.{name,pname} + firefox.name
byte-identical to OFF.

**Open (narrowly isolated):** hello.drvPath / raw-`builtins.derivation`-with-big-
`//`-env still diverge on `eval .drvPath` (real path, phantom hash).  Confirmed
chain-induced (`NIX_V3_CHAIN_MIN_NA=99999` ⇒ byte-identical; ≤16 ⇒ diverges).
KEY finding: **`nix derivation show` of the SAME expr is byte-identical ON/OFF** —
i.e. the CORE drv computes correctly with chains (full env, right hash); only the
v3-stored **`.drvPath` ATTRIBUTE** read returns a phantom.  So the residual site
is in the derivationStrict-result / Phase-5 `.drvPath` cache path (native is the
active path and materialises `src`; the chain mishandle is narrower — likely the
drvHash cache KEY or the result-attrset `.drvPath` exposure), NOT the env build.
Next: localise that one path (drv-content diff already shows env is identical, so
it's the hashed-key or the stored-attr), fix, then `--core`-with-chains across
the 19 workloads to flush any remaining nixpkgs-graph sites.  Robust closure: the
compiler-enforced `entries[]` accessor conversion (guarantees no missed site —
the exact thing that defeated the 5 ad-hoc attempts).

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
