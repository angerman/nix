# RCA — `ValuePair::evaluated` missed scavenger root (#705)

**Date**: 2026-05-21
**Issue**: Recurring `hello.drvPath` SIGSEGV under `NIX_V3_NURSERY=1
NIX_V3_NURSERY_SCAVENGE=1` despite four `#705` missed-root fixes already
landed. Direct-root audit (`V3_DBG_NURSERY_AUDIT=1`) reports clean.
Brute-scan (`V3_DBG_NURSERY_BRUTE=1`) reports ~502K stale nursery
pointers in tenured arena memory post-scavenge.
**Status**: ROOT CAUSE IDENTIFIED. Fix not yet landed.

## TL;DR

`ValuePair::evaluated` (third field, added 2026-05-18 commit `d3e41c13d`
for App-result memoization) is a missed scavenger root.

`walkPair` (`gc.cc:402-406`), `fwdPair` fast-path leaf gate
(`gc.cc:289`), and `Auditor::visitPair` (`gc.cc:651-655`) all walk only
`left` and `right`. The third field, written by every App memoization
(`vm.cc:5565` in OP_FORCE; `vm.cc:10329` in forceValue), holds a Value
whose payload (`Closure*` / `Thunk*` / `ListVec*`) can be in the nursery.
ValuePair itself is always tenured (`Alloc::allocPair` uses
`threadArena()` directly; `fwdPair` aborts on nursery). After scavenge
the tenured `ValuePair::evaluated.payload` is a stale nursery pointer.

The next force on the same App reads `outerPair->evaluated` (memo-hit
fast path at `vm.cc:5518` and `vm.cc:10302`), sees `tag !=
Uninitialized` so takes the cached path, returns the stale Value to the
caller, which forceValues it. `forceValue` reads `t->state` from
memset-zero memory (`state=0=Suspended`), then reads `t->suspended.desc
= nullptr`, then `desc->nLocals` → SIGSEGV.

This matches the session-plan signature byte-for-byte:
> "the stale Thunk's chain length (compressChain) is 0 — first
> iteration's t is stale … all-zero state confirms memset-after-scavenge
> survivor."

`NURSERY_PHASE_D_DESIGN_2026-05-18.md` §3.1 explicitly predicted this
hazard on the day Tag::App memoization landed, classifying it as a
"structural issue, not an edge case." The Phase D fix was deferred to
the eventual write-barrier landing; the scavenger's walker was never
updated to compensate in the meantime.

## Evidence

| Line of evidence | Strength |
|---|---|
| `NURSERY_PHASE_D_DESIGN_2026-05-18.md` §3.1 predicts the exact hazard on the same day App-memo landed | ★★★★★ |
| `walkPair` (`gc.cc:402-406`) visits `left` + `right` only; no third visit | ★★★★★ |
| `Auditor::visitPair` (`gc.cc:651-655`) has the same omission — explains why audit reports clean | ★★★★★ |
| `fwdPair` fast-path gate (`gc.cc:289`) checks `left.tag()`/`right.tag()` only — pairs whose `evaluated` is non-leaf nursery-payloaded are short-circuited entirely | ★★★★ |
| `git log -G "Value evaluated;" src/libexpr-v3/include/v3/value.hh` → `d3e41c13d` (2026-05-18) adds the field; `gc.cc` walkers predate it | ★★★★★ |
| App-memo write sites: `vm.cc:5565` + `vm.cc:10329` confirmed to assign `v` with potentially nursery payload | ★★★★★ |
| `Alloc::allocPair` (`alloc.hh:518-523`) uses `threadArena()` — ValuePair always tenured; the inter-generational hazard is structural | ★★★★★ |
| Crash signature (`compressChain=0` + `state=0` + `desc=nullptr`) matches a forceValue receiving a Value with stale Thunk payload sourced via the memo-hit fast path | ★★★★★ |
| Brute-scan 502K stale pointers — consistent with each tenured ValuePair carrying one stale pointer at offset 32 (`left`=0, `right`=16, `evaluated`=32) × App-memo-active pair count | ★★★★ |

## Critical review — what could disqualify this hypothesis

Each falsification attempt failed:

1. **"Could ValuePair be nursery?"** No. `Alloc::allocPair` uses
   `threadArena().alloc` (always tenured). `fwdPair` aborts on nursery
   detection (`gc.cc:281`). Confirmed by design and by abort guard.

2. **"Is App memoization actually active?"** Yes by default. Gated by
   `NIX_V3_NO_APP_MEMO` (env-var; off-by-default). Hello.drvPath runs
   with App-memo on.

3. **"Could importCache or bridgeThunkCache be the real culprit
   instead?"** Both are real missed roots (independently surfaced by
   parallel audits) but neither matches the crash signature as
   directly. importCache only dangles between repeated imports of the
   same path; bridgeThunkCache only matters when a Bridge thunk's
   `t->evaluated` is re-read post-resolution. ValuePair::evaluated
   fires on every App force, every memo write, every scavenge — vastly
   higher contact frequency, and the read path lands on
   `forceValue → SEGV` directly. These remain real bugs (see "Other
   missed roots" below) but are unlikely to be the proximate cause of
   the *first* SEGV in a hello.drvPath run.

4. **"Could the scavenger walk it via another path?"** No. Both the
   scavenger's `drain()` and the audit's deep walk go through
   `walkPair`/`Auditor::visitPair`. Neither visits the third field.
   Only the brute scanner could detect the dangling pointer; the brute
   scanner is opt-in (`V3_DBG_NURSERY_BRUTE=1`) and isn't enabled in
   `run-nursery-tests.sh`.

5. **"Could the App fast-path leaf gate save us?"** No. `fwdPair`'s
   gate at `gc.cc:289` only checks `left` and `right`. A pair with
   `evaluated.tag() == Tag::Closure/Thunk/List` (non-leaf) and
   leaf-tagged `left`/`right` is silently returned without queuing the
   pair — meaning the walk doesn't fire at all. So the gate makes the
   bug WORSE for the most common App-memo pattern (`primopApp` with
   leaf primop on the left, leaf arg on the right, non-leaf evaluated).

The hypothesis survives all five attempts.

## Other missed roots discovered during the investigation

Surfaced by parallel audit; should be closed before Stage 3 default-on
even though none of them are the proximate cause of *this* SIGSEGV.

| # | Site | File:Line | Status |
|---|---|---|---|
| A | `importCache().results[].result` singleton not walked. `import <nixpkgs>` and lib imports stash Closures/Attrs with nursery payloads. On subsequent imports of the same path, the cached Value can dangle. | `primops.cc:6970` | Real missed root; add `walkImportCacheRoots(visit)` |
| B | `bridgeThunkCache` (thread_local map of `nix::Value *` → `Thunk *`). Bridge thunks themselves are tenured, but their `t->evaluated` field (set on Bridge → Evaluated transition at `vm.cc:5736`, `vm.cc:10727`) can hold nursery payload. The cache is the only path that keeps the Bridge thunk reachable between forces. | `primops.cc:8434` | Real missed root; add `walkBridgeThunkCache(visit)` |
| C | `g_cachedCallFlake.closureValue` singleton not walked. Currently latent — protected only by an incidental lambda-lift singleton optimization that routes the callFlake closure through `allocClosureTenured`. Disable that optimization and the singleton routes via nursery. | `v3_call_flake.cc:151` | Add explicit walk; do not rely on incidental protection |
| D | `Thunk::shapeCell` not walked by `walkThunk` or `Auditor::visitThunk`. `*shapeCell` may carry nursery payloads. | `gc.cc:351-385`, `closure.hh:156` | Gated `NIX_V3_CELL_EVERYWHERE=1` (default-off). Walk for correctness; required before flipping the gate on |
| E | `CallFrame::forceWriteTarget` pointer is **assumed tenured** by the scavenger comment (`gc.cc:448`) but `vm.cc:8863` deepForceList sets it to `&list->elems[i]` where `list` can be nursery. After scavenge the pointer is into the freed nursery slot, not the new tenured list. Self-recovering via `ip = ip - 1` rewind and re-iteration, so silent perf loss rather than direct SEGV — but still incorrect. | `vm.cc:8863` ↔ `gc.cc:448` | Either forward the pointer (treat as a v3-internal cell that can be in nursery) OR change the protocol to use a slot index + base pointer that the scavenger does update |
| F | `forceDeep` / `printNixValueRich` hold nursery `ListVec*` C-locals across recursive `forceValue` calls when invoked AFTER the outer dispatch has returned (`vm.frames.empty()` → inner dispatch enters at `exitDepth==0` → scavenge enabled). | `print.cc:33-49`, `print.cc:530-542` | Refactor to re-read the list payload via a re-rooted Value after each inner force; OR push the list back onto valueStack as a synthetic root during the loop |
| G | `Auditor::postScavengeAudit` (`gc.cc:684-705`) does NOT mirror the scavenger's added root walks (`forceWriteTarget`, bridge tables, primop replacements, vBuiltins, AttrSelectIC). This is the hygiene gap that makes "audit clean" co-exist with SEGVs. | `gc.cc:684-705` | Make the Auditor walk identical to the Scavenger root set, automatically |

## Recommended fix order

1. **`ValuePair::evaluated` walks** (this RCA's primary fix; ~6 lines).
   Predicted outcome: hello.drvPath SIGSEGV resolves; brute-scan stale-
   pointer count collapses from ~502K to <1K residual (from the
   other missed roots listed above).

   ```cpp
   // gc.cc walkPair
   void Scavenger::walkPair(ValuePair * p) {
       visitValue(p->left);
       visitValue(p->right);
       visitValue(p->evaluated);          // ← MISSING
   }

   // gc.cc fwdPair fast-path gate
   if (isLeafTag(p->left.tag())
    && isLeafTag(p->right.tag())
    && isLeafTag(p->evaluated.tag())) return p;   // ← evaluated added

   // gc.cc Auditor::visitPair — mirror the addition
   visitValue(p->evaluated, "ValuePair.evaluated");
   ```

2. **`walkImportCacheRoots`** — closes the next-highest-frequency missed
   root. Add the walker in `primops.cc` next to `walkV3BridgeRoots`,
   call from `Scavenger::run`.

3. **`walkBridgeThunkCache`** — same shape, in `primops.cc`.

4. **`walkBuiltinsRoot` already exists**; **explicit walk of
   `g_cachedCallFlake.closureValue`** — same shape, in
   `v3_call_flake.cc`.

5. **`walkThunk` walks `t->shapeCell`** — guards against the
   `NIX_V3_CELL_EVERYWHERE=1` flip without revisiting this RCA.

6. **`forceWriteTarget` protocol refactor** — switch from raw pointer
   to (slot index, base pointer-walked-by-scavenger) so the deepForceList
   path doesn't silently corrupt.

7. **`forceDeep` / `printNixValueRich` C-stack hardening** — restructure
   the loop to re-read the list payload via a re-rooted Value after each
   inner force, OR temporarily push the list onto valueStack so the
   scavenger sees it.

8. **Auditor parity** — automate the audit's walk to be exactly the
   scavenger's walk (single source of truth). Without this, future
   regressions of this class will surface the same way: brute-scan
   shows hits, audit shows clean.

9. **(Defer, but plan now)** Phase D write barrier per
   `NURSERY_PHASE_D_DESIGN_2026-05-18.md` §4. The walks added above
   patch the *correctness* hole; Phase D recovers the generational-GC
   *perf benefit* by tracking inter-gen pointers in a remembered set
   rather than walking every reached tenured object every scavenge.
   For Stage 3 default-on the patches suffice; the perf factor (the
   ~5-10× GC scan factor of the 200× force-rate gap) requires Phase D.

## Lesson — process gap that allowed this to land

`d3e41c13d` (2026-05-18) added `ValuePair::evaluated` (a new
pointer-bearing field). The commit landed the producer side (write
sites in `vm.cc`) and the consumer side (read sites at `vm.cc:5518`,
`vm.cc:10302`) but did not update the scavenger's walker. The reviewers
of that commit (and the Phase D design doc landing on the same day —
which explicitly named this as a future hazard) did not block the field
addition on a same-commit scavenger update.

**Rule for future field additions** to any walked struct (Closure,
Thunk, Bindings, ListVec, ValuePair, CallFrame):

1. The field MUST be added to the scavenger's `walkX` function in the
   same commit.
2. The field MUST be added to the `Auditor::visitX` function in the
   same commit (or the Auditor needs to be refactored to share the
   scavenger's walker — see fix #8 above).
3. The field's gating env-var (if any) is irrelevant — the scavenger
   walks regardless. Walking a zeroed field is a no-op; walking a stale
   field is a SEGV.

A property test that asserts *"every pointer-bearing field of every
walked struct appears in the matching walkX function"* would catch this
class entirely. This is item #6 in `LESSONS_LEARNED_2026-05-15.md` §4.9
("Property tests for VM invariants") — currently listed as "not yet
built." This RCA strongly motivates building it before any further field
additions to walked structs.

## Cross-references

- `lode/NURSERY_PHASE_D_DESIGN_2026-05-18.md` §3.1 — predicted this hazard
- `lode/CHENEY_NURSERY_DESIGN.md` — Phase A/C design context
- `lode/SESSION_PLAN_2026-05-20_BINDINGS_STG3_STG4.md` "Track C" —
  documents the four already-fixed missed roots; lists the open one
- `lode/GC-REVIEW.md` §7 — Pattern B "interior pointer held across
  allocation" — partly related (C-stack hazard class) but not this bug
- `lode/LESSONS_LEARNED_2026-05-15.md` §4.9 item 7 — "Property tests for
  VM invariants" — what would have caught this

## Copyright

Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
Input Output Group. SPDX-License-Identifier: Apache-2.0
