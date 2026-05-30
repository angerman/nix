# EXIT Phase C #5 — investigation results

**Date:** 2026-05-30
**Per:** user direction "Continue with the Phase C prereq"
**Status:** Investigative.  Threshold sweep + diagnostic + one-site fix attempt all REVERTED.  Phase C remains genuinely multi-session-prereq-blocked.

---

## 1. What this commit attempted

Per user direction "Continue Phase C prereq", attempted:

1. **Threshold sweep** to find safe chain shapes.  Result: nb=1 (single-key patches) PASSED on hello.name/.pname/firefox.name but FAILED on hello.drvPath/.outPath with a DIFFERENT error (`attribute 'shell' missing`).  nb≥2 fails uniformly with `buildPythonApplication missing` per #4.

2. **OP_ATTRS_SELECT chain-aware fast path** (vm.cc:8240+).  Identified the manual binary search on `b->entries[]` that misses parent keys for Chain bindings.  Added a chain-aware branch that calls `b->lookup()` (which IS chain-aware) and skips the IC cache for Chain bindings.  Distinguished error message (`[CHAIN-MISS depth=...]`) to detect when the fix path fires.

3. **Validation**: rebuilt + re-tested chain-enabled hello.name.  The error STILL said `attribute 'buildPythonApplication' missing` WITHOUT the `[CHAIN-MISS depth=...]` suffix — meaning the failure is NOT at OP_ATTRS_SELECT but at one of the other ~30 direct `entries[]`-access sites in primops.cc / vm.cc.

---

## 2. Key empirical findings

### 2.1 Chain construct WORKS

`Alloc::allocChainBindings(parent, overlaySize)` + the construct path in mergeBindings produce structurally-correct Chain bindings.  Confirmed by:
* hello.name with nb=1 PASSES → chain construct fires + downstream lookup walks chain correctly via `b->lookup()`
* The error site is NOT at chain construction; it's downstream

### 2.2 Failure is downstream consumption

The `buildPythonApplication missing` / `shell missing` errors come from DIRECT `entries[]` access (NOT through `lookup()`/`forEach()`) that sees overlay-only and misses parent.

30+ sites in primops.cc + several more in vm.cc each have this pattern.  Fixing ONE site (OP_ATTRS_SELECT in this commit) made `hello.name nb=2-4` cases still fail with the same error — confirming the failure has SHIFTED to a different site (because chain construct is creating more chains via nb=2-4).

### 2.3 The 190-site audit IS irreducible

Each direct `entries[]` access on a Chain bindings is a potential failure point.  Even if I fix the few sites that fire on hello.name, the next nixpkgs path will fire on different sites.  Until ALL ~190 sites are audited + converted (or proven safe), the failure mode shifts but doesn't disappear.

The threshold-narrowing approach (nb=1 only) doesn't avoid the audit — it just narrows the failure-trigger frequency, not the underlying bug count.

---

## 3. Reverted in this commit

* mergeBindings chain-construct path (`vm.cc` near line 1170)
* OP_ATTRS_SELECT chain-aware fast path (`vm.cc` near line 8240)
* `NIX_V3_CHAIN_NB_MAX` / `NIX_V3_CHAIN_NA_MIN` env-var thresholds

Post-revert: 3/3 nixpkgs paths byte-equal; no behavior change.

---

## 4. What stays

`Alloc::allocChainBindings(parent, overlaySize)` helper stays in `alloc.hh`.  Unchanged from commit `8d8314bea`'s introduction.  Staging point for the eventual multi-session attempt #N+1 that completes the 190-site audit first.

---

## 5. Conclusion

Phase C revival genuinely requires the multi-session audit work.  No single-session attempt CAN ship without it because:

* Construction site is 1 location (mergeBindings)
* Consumption sites are 190+ locations
* Fixing 1 consumption site exposes the next 189
* The 4-pivot rule on construction-shape variations confirms the audit can't be sidestepped by clever construction gating

The session's mergeBindings-class contribution remains:
* **Tag::App3** (commit `3d64028e8`): -28.9 MB peak_rss on HNE (delivered)
* **EXIT_MERGEBINDINGS_AUDIT** (commit `a4e5dff6c`): showed 2-pass already captured headline
* **Phase C #4 / #5** (commits `8d8314bea` + this): falsification confirmed; audit prereq locked in as multi-session

## 6. Cross-references

* `EXIT_PHASE_C_4_FALSIFIED_2026-05-30.md` — the 4-pivot doc
* `EXIT_MERGEBINDINGS_AUDIT_2026-05-30.md` — the parent audit
* `EXIT_DAY4_TAG_APP3_LANDED_2026-05-30.md` — the realistic memory win
* `[[falsification-rule]]` — Rule 0
* `[[measure-twice-cut-once]]` — pre-committed thresholds + 3-pivot rule

---

*Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.  SPDX-License-Identifier: Apache-2.0.*
