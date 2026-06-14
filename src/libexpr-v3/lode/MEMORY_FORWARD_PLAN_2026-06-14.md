# v3 memory — integrated forward plan (2026-06-14)

Synthesises the reviewer's root analysis (`WHY_V3_USES_MORE_MEMORY_2026-06-14.md`,
968d5fd0e) with this session's measured findings into a single actionable plan.

## The reframe (why the per-lever hunt failed)
v3's memory excess is **not a single removable structure** — it is a DISTRIBUTED
per-object constant tax + unreclaimed churn + stranded dead, in three layers:
- **A — per-object live tax (~1.4× TW):** thunk 40 B header + 8 B/upvalue (vs TW's
  16 B `{Env*,Expr*}` with a shared Env); `ValuePair` 32 B (vs ~16 B). Bindings are
  actually *leaner* than TW. This tax buys v3's compute win (fib RSS 0.27× TW).
- **B — neither engine reclaims → peak ≈ cumulative bytes:** v3 allocates more
  garbage (bigger objects + 68.7 % of thunks never forced = churn TW never creates).
- **C — 224 MB stranded dead (44 % of firefox peak):** freed=0 blocks; the place v3
  *could* beat TW (TW's conservative GC can never reclaim mid-eval).

Consequence: a distributed tax yields **15–40 MB levers by construction** — the
−80 MB single-lever bar was mis-shaped and shelved a correct win (L1). **Switch to a
cumulative/stacking bar.**

## Concrete steps (ordered; each a todo FP-0..4)

### FP-0 — re-shape the bar (process; do first)
Replace the per-lever −80 MB threshold with a **cumulative RSS ratchet**: a
correct, byte-identical, CPU-neutral lever counts toward a per-release cumulative
target even if <40 MB. Add a `cumulative_rss` column to `bench/baselines/seven-rows.tsv`
+ a `make ratchet-check` note. Unblocks FP-1/2/3.

### FP-1 — un-gate L1 (now; cheap; already validated)
Flip `NIX_V3_CHAIN_LOOKUP_SELECT` default-on. **−33.6 MB firefox arena** (darwin-4
confirmed), byte-identical, CPU-neutral; validated (06-07 canary 5/5, lang 143, core
21/21 gate-on, provenance 0, aggressive-GC clean). Re-confirm byte-identity + core
after the default flip. ½ day. → banks −33 MB.

### FP-2 — thunk-header shrink 40 → 24 B (the M5 lever; ⚠ drv-hash + GC-critical)
Two stages; each removes 8 B from the suspended union. **Arena rounds allocs to
16 B** (`(bytes+15)&~15`, cell-bitmap depends on it), so removing 8 B changes the
*allocated* size only when it crosses a 16 B boundary: the full 16 B (40→24) gives
null-withs thunks a clean 16 B, but split into 8 B stages each helps half the
population by `nUp` parity.

**FP-2a — drop `suspended.cu` — SHIPPED (56968f897).** A suspended thunk's CU is
its descriptor's owning CU: OP_MAKE_THUNK is the SOLE creator (vm.cc:4773/4777 =
only `allocThunkSuspended` caller + only `suspended.desc=` writer) and sets
`desc=&cu->lambdas[i]`, so `desc->cu` is well-defined and equals the stored `cu`.
Added a transient (NOT-serialized) `mutable cu` backpointer to `LambdaDescriptor`,
set idempotently at MAKE; read via `thunkCU(t)`. Byte-identical by construction;
canary 5/5 + core 21/21 + `static_assert(sizeof(Thunk)==32)`. Header 40→32. NB:
ABI change → rebuild ALL v3 test binaries (a stale v3-smoke gave a false 20/21).
Arena unchanged on firefox (469.8) — banked the even-`nUp` half, masked by peak.

**FP-2b — relocate `capturedWiths` to the FAM tail — SHIPPED (8cd42314d).**
The reviewer's "gate on `nWithTargets>0`" was imprecise — the snapshot fallback
makes `capturedWiths` non-null whenever a `with` is active.  Measured the real
gate (V3_DBG_THUNK_WITHS, = `capturedWiths==null`): firefox 74.1 % of 2.05 M; M5
97.1 % of 17.1 M.  Design: removed `capturedWiths` from the union (→ `{desc}` =
8 B → header **24 B**, `static_assert==24`); store the `ListVec*` RAW at
`tail[nUpvalues]` iff `willHaveWiths = (nWiths>0) || (withStack > frame.base)`
(pre-alloc; EXACTLY predicts `capturedWiths!=null` since `snapshotCurrentWiths`
is null iff `top<=base`); bit in the spare `_pad0` → `hasWithsSlot`; accessors
`thunkCapturedWiths`/`thunkSetCapturedWiths` (getter gated on Suspended/Blackhole
so a stale bit on an evac-shrunk Evaluated thunk can't read OOB).  CRITICAL: one
source-of-truth `thunkScanSize()` replaces all ~15 size sites so the evac COPY
never drops the slot (= UAF); all reads/forwards updated (gc.cc:667/717 evac
forward = the UAF surface; mark_sweep, live_trace, barrier.hh; fakeClo ×2; MAKE ×3).

**Realized win (the M5 lever): M5 arena 2013.3 → 1862.3 MB (−151 MB, beats the
141 MB projection); COMBINED FP-1+FP-2a+FP-2b M5 = 2147.5 → 1862.3 = −285 MB.**
firefox 469.8 → 453.0 (−16.8; the `nUp=1` bucket crossed the 16 B boundary so its
peak moved after all).  Byte-identical: canary 5/5 + core 21/21 + firefox/M5
drvPath/name under major-GC AND hello/firefox/git.drvPath under `NIX_V3_NURSERY=1`
(validates the withs-slot scavenge forward).  `--brute` 21/22 — UNCHANGED vs the
pre-FP-2b baseline: BRUTE hits are the pre-existing PhD-6 family (hello-drvPath/
outPath, gcc-name) + a stale firefox golden (150.0.3 vs 151.0.4), none
withs-related → FP-2b added NO new missed root.  V3_DBG_THUNK_WITHS probe retired.

### FP-3 — pair tax (ValuePair 32 B) — investigate, reconcile first
~16 MB firefox. BUT "ValuePair 24/32 split" is on the plan's do-not-repropose list.
Re-examine *why* it was retired: if the killer was the mis-shaped single-lever bar,
it now stacks (split App=24 B `{left,right,evaluated}` / App3=32 B). If it was a real
aliasing/`s_matMemo` hazard, leave retired. Reconcile before any code.

### FP-4 — generational nursery (the strategic Layer-C lever; multi-week; ⚠)
The ONLY path to beating TW on derivations (reclaim the 224 MB dead mid-eval — the
one capability TW's conservative GC structurally forbids). The barriers are already
correct (8 workloads byte-identical + AR7 stress) and the nursery is a measured
**−19/−53/−42 % CPU win** on firefox/HNE/M5. Blocked on:
- **PhD-6** — RCA+fix the one missed root (`--brute`: 41 tenured words → 1 shared
  nursery obj on hello). Careful GC-scavenger work (a wrong walker = UAF). Teach the
  brute scanner to type holders without the major-GC cell-start bitmap (off under the
  nursery), find the unbarriered write / missing per-type walk, fix, re-run → 0 hits.
- then full byte-identity → flip nursery default-on (5-gate) → **completes Phase D
  AND workstream E**.

## Stacking projection (the point of the new bar)
firefox: L1 (−33) + header (−16 live) + pair (−16) ≈ **−60–90 MB** of the live tax;
nursery then attacks the **224 MB dead**. M5: header alone ~**−78 MB live / −272 MB
churn**. Each is byte-identical + (mostly) CPU-neutral-or-positive.

## Honest stakeholder framing (the reviewer's, endorsed)
v3 pays memory to buy CPU and is already far ahead on compute (fib 0.27× RSS). On
derivations the live tax stacks down to ~1.5×; *beating* TW there requires turning
v3's GC weakness (no mid-eval reclaim) into the advantage TW can never have — the
generational nursery (FP-4). Stop hunting for a fourth cheap firefox lever; the
measurements say there isn't one.

## Sequencing
FP-0 → FP-1 (this session, safe) → FP-2 (1 wk, the M5 win) → FP-3 (investigate) →
FP-4 (multi-week, the firefox endgame; completes Phase D + E). Standing gates:
cumulative ratchet, full byte-identity for ⚠ levers, the brute scanner for any GC
change, Rule 0.

*Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output
Group. SPDX-License-Identifier: Apache-2.0.*
