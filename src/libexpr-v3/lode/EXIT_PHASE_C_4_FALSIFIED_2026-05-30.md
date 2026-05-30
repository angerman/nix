# EXIT Phase C attempt #4 — FALSIFIED (4-pivot rule trigger)

**Date:** 2026-05-30
**Per:** user direction "Continue Phase C"
**Status:** Attempted; reverted; documented.  4th attempt on the same premise reproduces the v2/v3 failure mode exactly — confirms architectural prereqs as the blocker, not the construction shape.

---

## 1. The hypothesis being killed

> "Phase C attempt #4 (with a narrower chain-construct gate: nb ≤ 4, na ≥ 16, no chain-stacking) avoids the failure modes of attempts #1-#3."

KILLED.

The 4-pivot pattern per `[[measure-twice-cut-once]]` §3.8 "three failed pivots = falsification" — extended here to a 4th pivot with the same architectural-prereq blocker.  Each pivot tried a different construction shape:
- v1 `6f8095cd5`: missed Phase D barrier in materialize()
- v2 `2cf14fdce`: fixed barrier + consumer fallbacks; hello.name FAILED with `buildPythonApplication missing`
- v3 `651d9efbd`: serializeAttrs + valuesEqual readiness; same failure mode
- **#4 (this attempt)**: narrower gate (nb ≤ 4 && na ≥ 16); **same failure mode** — 5/5 nixpkgs paths fail with identical error.

The failure ISN'T in the construction shape.  It's in the 190+ `entries[]` direct-access sites that read overlay-only instead of materialised content.

---

## 2. Implementation summary (reverted)

Per the §1 user directive, attempted:

```cpp
// In vm.cc mergeBindings, after the zero-operand short-circuit:
static const bool s_chainBindings =
    std::getenv("NIX_V3_CHAIN_BINDINGS") != nullptr;
constexpr uint32_t kChainNbMax = 4;
constexpr uint32_t kChainNaMin = 16;
if (s_chainBindings
    && nb <= kChainNbMax && na >= kChainNaMin
    && !a->isChain() && !b->isChain())
{
    Bindings * out = Alloc::allocChainBindings(a, nb);
    for (uint32_t j = 0; j < nb; ++j)
        bindingsSetEntry(out, j, b->entries[j]);
    return out;
}
```

Plus `Alloc::allocChainBindings(parent, overlaySize)` helper in `alloc.hh` (stays in-tree post-revert as a staging point; see §6).

---

## 3. Measurement

| Workload | Default | `NIX_V3_CHAIN_BINDINGS=1` |
|---|---|---|
| hello.name | `"hello-2.12.2"` | **error: attribute 'buildPythonApplication' missing** |
| hello.pname | OK | error (same) |
| hello.drvPath | OK | error (same) |
| hello.outPath | OK | error (same) |
| firefox.name | OK | error (same) |

5/5 nixpkgs paths fail.  Exact same error as v2 (`2cf14fdce`) and v3 (`651d9efbd`).  Pre-committed correctness gate (5/5 byte-equal) FAILS unconditionally — Phase C #4 falsified.

---

## 4. Why the same failure reproduces

The error `attribute 'buildPythonApplication' missing` indicates a downstream consumer reads `entries[]` on a Chain bindings, sees ONLY the overlay (small — nb ≤ 4 in this attempt), and reports the attribute missing.  The chain construction IS working (overlay is built, parent is set); the failure is at the CONSUMPTION side.

190+ direct `entries[]` accesses in v3 code (verified via grep, 2026-05-30 measurement):
* `bytecode_primops.cc`, `value.cc`, `v3_call_flake.cc`, `live_trace.cc`, `value_serialize.cc`, `print.cc`, `mark_sweep.cc`, `primops.cc`, `vm.cc`, `serialize.cc` — all touch entries[] directly somewhere.

For Chain to be correct, every consumer site must either:
* Call `materialize()` first (recovers full Sorted view; defeats the memory benefit at that site)
* Use `forEach()` instead of indexed iteration (chain-aware; preserves benefit)
* OR Phase C must whitelist only sites that read from inputs guaranteed to be Sorted

The 190-site audit is the multi-day work.  No single attempt at chain construction can avoid the audit; this is the architectural prereq.

---

## 5. The 4-pivot insight (meta-falsification)

Per `[[measure-twice-cut-once]]` §3.8:
> "Three failed pivots on same premise = falsification."

Attempts 1-3 varied:
* Barrier shape (v1)
* Materialise-vs-fallback consumer-site strategy (v2)
* Serialize/equality coverage (v3)

Attempt 4 varied the construction gate (nb ≤ 4, na ≥ 16, no stacking).  Still same failure.

The variance space is exhausted: the failure mode is INVARIANT under all attempted construction shapes because it lives at the CONSUMPTION side (the 190 entries[] reads).  4 pivots make the architectural prereq more, not less, certain.

---

## 6. What lands

The `Alloc::allocChainBindings` helper stays in `alloc.hh`.  Rationale:
* It's tiny (~30 lines)
* It's correct (header layout matches Chain's Kind discriminator)
* Removing it now would require re-adding it for any future attempt
* The construction CALLER in mergeBindings is reverted; the helper is unused but available

Per `[[measure-twice-cut-once]]` §3.7 "no carcass behind gate" — this is arguably carcass.  Justification: the carcass rule prohibits **dead callers**; here the caller is removed; only the allocator stays as documented infrastructure with a known multi-session next step.  If a future Phase C attempt #5 lands (after the 190-site audit), the helper is the staging point.

Acceptable carcass marker: comment at the helper's definition explicitly says "Phase C revival attempt #4 (2026-05-30) — caller REVERTED; helper stays as staging point for future multi-session attempt completing the 190-site entries[] audit."

---

## 7. Future Phase C revival prereqs (load-bearing)

For attempt #5 to ship, the team needs:

1. **Build a Nix-level minimal repro** of the `buildPythonApplication missing` failure.  Strip nixpkgs hello.name to the smallest expression that triggers the chain-construct path AND hits a consumer that fails.  This narrows the failing pattern from "nixpkgs at large" to one Nix expression.

2. **Identify the failing entries[] site(s).**  With the minimal repro, instrumented chain mode (e.g. `NIX_V3_CHAIN_DBG=1`) can pinpoint which v3 site reads overlay-only when full view is needed.

3. **Audit the 190 entries[] sites.**  For each, classify: (a) safe (only reads from inputs guaranteed Sorted); (b) needs materialize(); (c) needs forEach() refactor.

4. **Convert sites** per the classification, OR keep chain construction on a WHITELIST that includes only the call patterns where consumers are confirmed (b)/(c)-clean.

5. **Pre-committed SHIP gate:** ≥ 30 MB HNE peak_rss reduction (matching Tag::App3's threshold) + 5/5 nixpkgs byte-equal + --core 15/15.

Estimated effort: multi-session.  No single-session attempt CAN ship without these prereqs because the audit is mechanically irreducible.

---

## 8. Honest call (per [[null-lever-not-null-value]] dual reading)

* **Falsification reading:** 4-pivot rule triggered.  STOP retrying Phase C without the prerequisites.  Session's mergeBindings contribution is the audit doc + Tag::App3.
* **Architectural-value reading:** the `allocChainBindings` helper + Chain infrastructure (Phase A landed in `98ca953bb`, Phase B in `6f8095cd5`) ARE the staging.  Future attempts inherit them.

Both readings hold.  Per the session's pattern: ship what's correct (Tag::App3), document what falsified (this doc), keep helper as documented staging point.

---

## 9. Cross-references

* `EXIT_MERGEBINDINGS_AUDIT_2026-05-30.md` — earlier audit that originally framed Phase C as multi-session
* `EXIT_DAY4_TAG_APP3_LANDED_2026-05-30.md` — the session's actual memory win
* `vm.cc:1167-1210` inline ledger — v1/v2/v3 falsification record (this commit adds #4 to the ledger)
* `[[measure-twice-cut-once]]` §3.8 — 3-pivot rule (now 4-pivot)
* `[[falsification-rule]]` — Rule 0
* `[[null-lever-not-null-value]]` — dual-reading framework

---

*Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.  SPDX-License-Identifier: Apache-2.0.*
