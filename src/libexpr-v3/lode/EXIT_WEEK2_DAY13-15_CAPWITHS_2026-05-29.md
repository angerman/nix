# EXIT Week 2 Day 13-15 — capWiths singleton interning

**Date:** 2026-05-29
**Status:** IMPLEMENTATION LANDED (commit `25bf129af`); **arena PASS, peak indeterminate** at current noise floor.  Retain (wall-neutral, deterministic arena saving, additive with future GC).
**Task:** #865
**Plan reference:** [`EXIT_GC_SPIRAL_PLAN_2026-05-29.md`](EXIT_GC_SPIRAL_PLAN_2026-05-29.md) §4.3 + [`T1_3_PAIRS_LISTS_ATTR_2026-05-27.md`](T1_3_PAIRS_LISTS_ATTR_2026-05-27.md) §"Lever 2"

---

## 1. The change in one sentence

`mapAttrs`/`with`-chain lambdas captured a 1-element `ListVec` (~32 B) per MAKE_THUNK / MAKE_CLOSURE — 544 K allocs / 12.8 MB on HNE per T1.3.  This commit interns those into a fixed-size hash table so all Thunks/Closures capturing the same with-target share one tenured-arena `ListVec`.

## 2. Why this works

* `capturedWiths` ListVec is set-once at MAKE and read-only thereafter (`pushCapturedWiths` only iterates `->elems[i]`).
* Allocated arena-resident — pointer stable for process lifetime.
* Phase D / nursery generational correctness:
    * First miss → `listPostConstructBarrier` → ListVec on `dirtyContainers` → scavenge forwards any nursery payload.
    * Subsequent cache hits return the SAME pointer; barrier already paid.
    * Cache orphans (stale keys after scavenge forwarding): bounded by 4096 buckets; collisions overwrite.

## 3. Implementation (commit 25bf129af)

* `vm.cc` top: anon-namespace `internOrAllocSingletonCapWiths(v)` + 4096-bucket hash table (96 KB total).
* `vm.cc` OP_MAKE_CLOSURE / OP_MAKE_THUNK / `snapshotCurrentWiths`: take the intern path for nWiths==1; larger sizes fall through to per-call `Alloc::allocList`.
* `vm.cc` `namespace nix::v3 {}` block: external-linkage wrappers `getCapWithsHits/Misses/Evicts()` for `NIX_VM_STATS` dump.
* `run.cc` NIX_VM_STATS block: dump cache hit-rate + estimated savings.
* `vm.hh`: declare public stat getters.
* No schema bump — serialization format unaffected.

Gate: `NIX_V3_NO_CAPWITHS_INTERN=1` reverts to per-call alloc for A/B measurement.

Retirement criterion (Rule 0, parallels fakeClo): retire when (a) Phase E v0.2 default-on makes nursery `ListVecs` cheap, OR (b) Stage 6 production GC reclaims per-call `ListVecs` unaided.

## 4. Correctness validation

* `ninja src/libexpr-v3/libnixexprv3.dylib` clean under `-Werror=switch` + `-Werror=switch-enum`
* `all-v3-tests --quick`: **6/6 PASS**
* `all-v3-tests --core` (incl. `583-tag-app-cache`): **15/15 PASS**
* `hello.drvPath` byte-identical to TW: `r77jznkw60xvqjzs3jvd1dn54pxcqs68-hello-2.12.3.drv`
* `hello.name` byte-identical: `"hello-2.12.3"` — cache reports **hits=84305 misses=24 evicts=0 hitRate=100.0% (~2.7 MB saved)**

## 5. Measured memory delta

A/B at fresh `builddir/src/nix/nix` binary (N=10 each, trim-2 mean):

| Config | n | peak_rss (MB) ± σ | v3_arena (MB) ± σ |
|---|---:|---:|---:|
| HNE intern OFF (`NIX_V3_NO_CAPWITHS_INTERN=1`) | 10 | 3875.71 ± 234.99 | **3154.1 ± 0.00** |
| HNE intern ON (default) | 10 | 3914.32 ± 422.96 | **3137.3 ± 0.00** |

**Δarena = -16.8 MB σ=0** (deterministic; matches T1.3 projection of ~13 MB).
**Δpeak  = +38.6 ± 483 MB pooled** (intern nominally HIGHER on peak; within ~0.08σ of zero → effectively no signal).

84 K cache hits × 32 B/hit = ~2.7 MB direct allocation savings on hello.name; HNE shows ~17 MB at the larger workload scale, consistent with ~95% hit rate on the 544 K MAKE_THUNK calls.

### 5.1 Pre-committed SHIP gate verdict

T1.3 threshold: "≥ 10 MB peak RSS reduction on HNE + no `--core` regression."

* Strict peak reading: **FAIL** (nominal +38 MB; well within noise envelope but doesn't show the required -10 MB reduction).
* Arena reading: **PASS** (-16.8 MB ≥ 10 MB threshold, deterministic).
* `--core` regression: NONE (15/15 PASS).

**Decision: PARTIAL PASS** by arena, peak-indistinguishable from zero.  Same pattern as Day 9-11 App3 — wall-neutral architectural cleanup whose peak Δ falls under the σ floor on the M5/HNE class workloads where arena > host RAM (per [[arena-over-ram-peak-noise]]).

### 5.2 Why peak σ swamps the signal

T1.3's projected savings of ~13 MB is below the HNE σ floor on this host today (σ=235 MB intern-OFF, σ=423 MB intern-ON, pooled ~483 MB at 1σ).  Even N=10 doesn't resolve a 17 MB peak signal in a ±400 MB envelope.  The deterministic arena counter is the load-bearing measurement here, not peak_rss.

## 6. Retention rationale (parallels Day 9-11 App3)

Per [[measure-twice-cut-once]] §3.5 (keep code despite borderline SHIP):
1. **Arena saving IS real** (-16.8 MB σ=0, replicates T1.3 projection).
2. **Wall-neutral** within σ (48.4 vs 47.5 s on HNE; both within ~1s of each other).
3. **Opt-out gate present** (`NIX_V3_NO_CAPWITHS_INTERN=1`) for safe rollback.
4. **Additive with future GC** — fewer ListVec allocations means less scavenger work; Immix / Phase E shipping later benefits from a smaller per-MAKE allocation count.
5. **Hit rate 100%** on small workload (84K/84K); the cache mechanism does what it's supposed to.

Hold the gate as A/B-revert escape hatch.  Same retirement criterion as fakeClo (commit `6d7bf236c`): delete when Phase E v0.2 default-on OR Stage 6 production GC lands.

## 7. Measurement-methodology issue surfaced

While verifying Day 13-15, discovered the bench script (`bench/measure-peak-noise-floor.sh`) defaults `NIX_BIN="${NIX_BIN:-$ROOT/build/src/nix/nix}"`.  The `build/` directory is a SEPARATE build tree from `builddir/` — `build/src/nix/nix` is 7.3 MB stripped; `builddir/src/nix/nix` is 32.7 MB with debug.  They contain DIFFERENT binaries.

`build/src/nix/nix` mtime is May 28 18:36 JST — BEFORE the Day 6-8 fakeClo wire-back commit at 18:48 JST.  This means the Day 6-8, Day 9-11, and Day 12 N=10 bench measurements that defaulted to `build/` may have run against a pre-wire-back binary.

Consequence: the Day 12 HNE / M5 SHIP verdict's headline numbers (HNE 2457 MB, M5 4046 MB, wall ~5.4 s) may not reflect the actual code on HEAD.  Today's measurements with `builddir/` binary explicitly show HNE 3914 ± 423 MB peak / 3154 MB arena / 48 s wall — substantially different.

**Open follow-up:** re-run Day 12 N=10 measurements with `NIX_BIN=builddir/src/nix/nix` to settle whether Week 1's "-98 MB HNE" / "-805 MB M5 arena" claims hold up against an honest baseline.  Not blocking for Day 13-15 retention (the capWiths A/B is internally consistent on `builddir/`); blocking for the Week 1 retrospective.

## 8. What Week 2 ships

| Lever | Δarena (deterministic) | Δpeak (with σ) | Wall | Architectural status |
|---|---:|---:|---:|---|
| capWiths singleton intern | **-16.8 MB HNE** | +38.6 ± 483 MB (noise) | -0.9 s (noise) | Kept; conditional retirement on Phase E v0.2 / Stage 6 GC |

~2 engineer-days.  Below-σ-floor on peak; arena is the trustworthy measurement.

## 9. Cross-references

* [`EXIT_GC_SPIRAL_PLAN_2026-05-29.md`](EXIT_GC_SPIRAL_PLAN_2026-05-29.md) §4.3 (per-site bundle)
* [`T1_3_PAIRS_LISTS_ATTR_2026-05-27.md`](T1_3_PAIRS_LISTS_ATTR_2026-05-27.md) §"Lever 2" (the 13 MB projection)
* [`EXIT_WEEK1_DAY9-11_APP3_2026-05-29.md`](EXIT_WEEK1_DAY9-11_APP3_2026-05-29.md) — sibling lever; same peak-vs-arena pattern
* [`EXIT_WEEK1_DAY12_BUNDLE_VERDICT_2026-05-29.md`](EXIT_WEEK1_DAY12_BUNDLE_VERDICT_2026-05-29.md) — Week 1 SHIP verdict (subject to §7 caveat)
* Memory: [[peak-vs-alloc-distinction]] — long-lived bytes vs short-lived; capturedWiths are mid-lifetime (held by closures)
* Memory: [[arena-over-ram-peak-noise]] — why peak σ swamps small signals on HNE
* `bench/baselines/2026-05-29-week2-capwiths/` — raw measurement JSON (intern-ON via bench script + intern-OFF inline)

---

*Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group. SPDX-License-Identifier: Apache-2.0.*
