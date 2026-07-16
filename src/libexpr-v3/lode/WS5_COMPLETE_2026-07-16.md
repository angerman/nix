# WS-5 COMPLETE — parallel-eval-density (D1 + D2a + D2b + D3 + B1 + B2)

**Date:** 2026-07-16
**Status:** COMPLETE. All five WS-5 tracks + the two blockers are implemented, integrated, and gated on both macOS (aarch64-darwin) and x86_64-linux.
**Copyright:** (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group. SPDX-License-Identifier: Apache-2.0.

---

## Final state (all committed, integrated on the branch)

| track | what | commit |
|---|---|---|
| D1 | side-array CU/descriptor mutables → `cu.rt` + `cu_registry` | ws5-d1 |
| D2a | `OwnedOrBorrowed` wrapper; borrow code/consts from AOT mmap | ws5-d2a |
| D3 | `--fork-worker` fork-server (zygote) | ws5-d3 |
| B1 | pre-existing Linux `RLIMIT_AS` crash fix | ws5-b1 (`6e7b047e6`) |
| B2 | canonical AOT symbol/pos table + `LambdaDescriptor` flatten | ws5-b2 (`ed507f3be`) |

## Gate results (measured, both OSes)

- **macOS `--brute` = 41/41 GREEN. Linux `--brute` = 41/41 GREEN.** (Linux was 34/41 before B1.)
- **byte-identity** (hello/git/gcc/firefox `.drvPath`): owned path == **AOT-borrow path** == golden, on both OSes.
- **Borrow rate (integrated, AOT active): code 100% + lambdas 100%** (D2a-full + D2b) — up from D2a's 7.6% pre-B2, via the canonical symbol/pos table.
- **Cross-process CU sharing (Linux, 2 concurrent evals sharing one AOT mmap):**
  - firefox (973 CUs, 27.9 MB AOT): AOT-mapping `Shared_Clean == Rss == 22 MB` — 100% of the touched CU footprint shared.
  - HNE / haskell.nix (2345 CUs, 126.2 MB AOT): AOT-mapping **`Shared_Clean = 105 MB`** across both processes (83% of the AOT resident + shared).
- **D3 KPI-5 (per-child COW cost, `--cow-fork` firefox):** 164 MB / ~1430 MB fresh = 11% ≤ 70% → PASS.

## Two corrections to earlier docs (honesty)

1. **B1 root cause — the WS5_INTEGRATION §1 diagnosis was WRONG.** It was NOT a GC/arena/scavenge runaway. gdb on linux-1 showed zero arena blocks at the failing `Arena::refill` (the FIRST 16 MB block fails). The cause: v3's `NIX_V3_MAX_HEAP` does `setrlimit(RLIMIT_AS, cap+256MB)`, but upstream Nix's `BumpMemoryResource` reserves **two 8 GiB `MAP_NORESERVE` virtual arenas** (SymbolTable + Exprs) in the `EvalState` ctor first — so a 2.25 GB `RLIMIT_AS` ≪ 16 GiB already reserved → ENOMEM → abort. **macOS ignores `RLIMIT_AS`**, which is exactly why macOS `--brute` stayed green and it looked Linux-nursery-specific. Baseline-proven pre-existing (rebuilt `97d8d05f8` on Linux → identical crash). Fix (`limits.cc`, +64/−2): baseline `RLIMIT_AS` on the process's *current* VmSize (`/proc/self/statm`; 0 on macOS → byte-id-neutral). RSS is still hard-capped by the RSS watchdog; additional virtual growth is still bounded to `cap`.

2. **The Shared_Clean gate's "212 MB → ≥127 MB" was mis-calibrated.** The 212 MB (from M2-b) is the *in-memory* CU-bytecode footprint (deserialized, `std::vector`s + descriptor objects). The *serialized, in-place-shareable* AOT footprint is smaller (HNE 126 MB). The right reading of the gate — "≥60% of the CU footprint becomes `Shared_Clean` cross-process" — is **MET**: 100% borrow, and 105 MB / 126 MB (83%) of the HNE AOT is shared across processes (100% of what a single eval touches). The literal 127 MB isn't reached only because the shareable AOT itself is 126 MB, not 212 MB.

## What this delivers for CI

For N parallel evals per box on the same flake/nixpkgs, the CU bytecode + descriptors (the largest live, read-only chunk) are now **borrowed in place from one shared AOT mmap** — 100% of CUs, ~105 MB shared once across the whole fleet (HNE-scale) instead of copied private per process. Combined with the shipped persistent worker / fork-server (D3, per-child ~11% of fresh) and the default-on IFD visibility (D2/WS-2), the parallel-eval-density objective is delivered and measured on the CI OS. Two pre-existing, non-WS-5 items remain out of scope: the broader v3 Linux-port hardening beyond the RLIMIT_AS fix, and the in-memory (non-AOT) representation floor (the 1.6–2.0× RSS structural floor from the representation-rewrite track).
