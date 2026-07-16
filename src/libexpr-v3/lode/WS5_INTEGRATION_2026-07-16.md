# WS-5 FULL — integration outcome (D1 + D2a + D2b + D3)

**Date:** 2026-07-16
**Scope:** honest integration report for the parallel-eval-density goal. Three tracks built by parallel subagents (isolated worktrees), integrated + verified here.
**Copyright:** (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group. SPDX-License-Identifier: Apache-2.0.

---

## What landed (committed, integrated on the branch)

Three subagents implemented D1/D2a/D3 concurrently in isolated worktrees; I merged all three (only one textual conflict, in primops.cc — resolved by composing A's `cu.rt.fromImportCU` with B's AOT-borrow branch):

- **D1 (`ws5-d1`)** — side-arrayed the runtime-mutable CU/descriptor fields (`attrSelectCache`/`recSlotCache` → `CompilationUnit::rt`; descriptor counters/`cachedSingletonClosure`/`cu`-backptr/`fromImportCU` → `rt` + a new `cu_registry` desc→CU map). Format-benign (no schema bump).
- **D2a (`ws5-d2a`)** — `OwnedOrBorrowed<T>` vector-API wrapper; `code`/int/float-consts/`lambdaCodeOffsets` borrow in place from a page-aligned AOT-mmap section; schema **20→21**; AOT-path deserialize borrows, SQLite/fresh path owns. Solved the remap-vs-borrow SIGSEGV via id-preferring seeding.
- **D3 (`ws5-d3`)** — `--fork-worker` fork-server (warm parent → fork per request → result via pipe), `GC_atfork` hardening.

### Integrated verification (macOS)
- **`--brute` 41/41 ALL GREEN** (incl. the new fork-worker suite, brute-audit golden byte-id, applied-cache).
- **byte-identity** (hello/git/gcc/firefox `.drvPath`): owned path == golden **and** AOT-borrow path == golden.
- v3-smoke ALL PASSED (incl. B's `testSerializeBorrowRoundTrip`).
- Builds + runs on x86_64-linux (v3-eval + full nix CLI, 0 additional port errors).

## The two blockers to "GOAL MET", precisely isolated

### 1. Linux `--brute` green is blocked by a PRE-EXISTING Linux-port bug (NOT WS-5)

First-ever full `--brute` on x86_64-linux: 34/41. Classified:
- 3 = unbuilt test binaries (I'd only built v3-eval/nix/v3-smoke; built the rest → resolved).
- 4 (brute-audit, nonmoving-tenured, 815-cache, r1-verify) = all crash with `v3 fatal: arena block allocation failed (16 MB request) — address space exhausted` (`alloc.hh:2857`, `Arena::refill`) — a **runaway arena-block allocation on a trivial eval under the 1 MB-nursery brute config** (`NIX_V3_NURSERY_SIZE=1`), i.e. the moving-GC/arena under aggressive scavenge.

**DECISIVE classification (baseline-proven):** rebuilt the **pre-WS-5 baseline** (`97d8d05f8`) v3-eval on Linux (shipped baseline source, `meson --reconfigure`, fresh link) and ran the same repro → `TRUE_BASELINE_RC=134`, **identical crash**. So this is a **pre-existing Linux-port bug in the v3 moving-GC/arena under aggressive scavenge** — the nursery/scavenge code was developed + tested only on aarch64-darwin and had never run the `--brute` 1 MB-nursery config on Linux. **WS-5 introduced ZERO Linux regressions.** macOS `--brute` (the established gate) is 41/41 both baseline and integrated.

→ Linux-`--brute`-green requires fixing this pre-existing GC/arena port bug (out of WS-5 scope): a size/pointer/block-management assumption in the Cheney scavenge / `Arena::refill` that diverges on x86_64/glibc under a tiny nursery. It is the top pre-existing-Linux-port item, independent of this goal.

### 2. D2a full borrow-rate + D2b both need a canonical symbol/pos table (one shared build)

- **D2a borrow-rate regressed under integration**: POD constants 100%, but **code 7.6%** (integrated) vs B's 66.7% (isolated). Root cause: D1 interns more low-id base symbols at startup, which collide with the writer's baked low ids; the reader-side reservation (`aot_cache.cc:209-234`) only pushes NEW interns above the writer range — it cannot realign low ids. So most CUs need code remapping → owned copy, not borrow. So D2a's cross-process `Shared_Clean` gate (≥60% of the POD slice) is **not met** on the integrated tree (POD shares; code mostly doesn't).
- **D2b (borrow the `lambdas` array) was not built**: `LambdaDescriptor` still holds `std::string name/contextualName` + `std::vector<Formal> formals` (non-POD, heap-owning), so it is not borrowable in place without flattening those into the blob.
- **Both** are unblocked by the same follow-up B flagged: a **canonical symbol/pos table in the AOT file** (writer + reader agree on ALL ids, incl. low ones) + flattening `LambdaDescriptor`'s variable-length parts into the blob. That is the next foundational build.

## Measurements (Linux, integrated tree, default config)

- **D3 KPI-5 (per-child COW cost, `--cow-fork` firefox):** per-child Private_Dirty **164 MB** vs fresh ~1430 MB = **11% ≤ 70% gate → PASS.** (Note: same-expr per-child was 27 MB pre-integration in WS5.0; the rise to 164 MB — under a different nixpkgs input, URL vs pin — is a sharing-effectiveness item to verify, likely D1's ICs living in `cu.rt` *inline* in the CU still dirtying CU pages on attr-select in the child; it does NOT fail KPI-5.)
- **D2a cross-process sharing:** POD sections 100% borrowed (Shared_Clean across processes via the mmap); code 7.6% (per above) → below the ≥60% gate pending the canonical-table build.

## Honest goal status

| deliverable | correctness | macOS brute | Linux | measurement/gate |
|---|---|---|---|---|
| **D1** | ✓ byte-id | ✓ 41/41 | builds+runs | DONE |
| **D2a** | ✓ byte-id (owned+borrow) | ✓ 41/41 | builds+runs | correct; Shared_Clean gate BLOCKED (canonical table) |
| **D3** | ✓ byte-id vs fresh/worker | ✓ 41/41 | fork-worker suite ✓ | KPI-5 PASS (11%) |
| **D2b** | — | — | — | NOT BUILT (needs canonical table + descriptor flatten) |

**GOAL NOT fully met.** D1 + D3 are DONE (correct + macOS-brute-green + measured). D2a is correct + shipped but its Shared_Clean gate needs the canonical-symbol-table follow-up. D2b is not built (same prerequisite + descriptor flattening). Linux-`--brute`-green is blocked by a **pre-existing** Linux GC/arena port bug (baseline-proven, out of WS-5 scope). No fake greens: every number above is measured, and the two blockers are isolated to two concrete follow-up builds (canonical AOT symbol/pos table; the pre-existing Linux moving-GC arena fix).
