/goal — WS-5 completion: the two isolated blockers (B1 pre-existing Linux GC/arena fix; B2 canonical-AOT-table → D2a-full + D2b).

LOAD FIRST: src/libexpr-v3/CLAUDE.md (Rule 0, --brute gate, darwin-4/byte-id). Then lode/WS5_INTEGRATION_2026-07-16.md (both blockers, root-caused) + lode/WS5_D2_INPLACE_AOT_DESIGN_2026-07-16.md. Memory: project_senior_review_ci_reframe_2026-07-13. STATE: D1/D2a/D3 MERGED — macOS --brute 41/41, byte-id owned+borrow==golden, builds on x86_64-linux. This goal finishes the 2 follow-ups.

GOAL MET = B1 + B2 each committed (Rule-0 body), macOS --brute 41/41 AND Linux --brute 40/40 GREEN, nixpkgs byte-id both OSes, per-deliverable measurement git-noted. A pre-committed falsification also = DONE.

USE SUBAGENTS: B1 and B2 are independent — spawn as parallel Agent tools in ONE message (isolated worktrees); integrate + gate each yourself. Also delegate an ADVERSARIAL review of the B1 GC fix (missed-root/UAF under the moving nursery) + the B2 canonical-table soundness before each merge.

B1 — PRE-EXISTING Linux moving-GC/arena crash (unblocks Linux --brute; helps ALL Linux work):
 Symptom (host linux-1, x86_64): `v3 fatal: arena block allocation failed (16 MB request) — address space exhausted` at alloc.hh:2857 (Arena::refill) — runaway tenured-block allocation on a TRIVIAL eval under NIX_V3_NURSERY_SIZE=1. Baseline-proven pre-existing (rebuilt 97d8d05f8 on Linux → TRUE_BASELINE_RC=134, identical) — NOT a WS-5 regression. Repro under the --brute env (NIX_V3_NURSERY_SIZE=1 + NIX_V3_DIRECT_EVAL=1) on `v3-eval --expr "1 + 1"`. Backtrace: Arena::refill ← allocClosureTenured ← dispatchLoop. Root-cause the scavenge/promote/reset under a 1MB nursery on x86_64/glibc (nursery never reclaims → unbounded tenured blocks); fix with gdb on linux-1.
 Gate: repro exits 0; Linux --brute 40/40 GREEN; macOS --brute stays 41/41 + byte-id unchanged (fix MUST be byte-id-neutral).

B2 — canonical AOT symbol/pos table + descriptor flatten (unblocks D2a-full-borrow + D2b together):
 (a) D2a code-borrow is 7.6% integrated (vs 66.7% isolated): the reader interns low-id base symbols that COLLIDE with the writer's baked low ids; the high-id reservation can't realign them. Add a CANONICAL symbol/pos table to the AOT so writer+reader agree on ALL ids (incl low) → borrowed `code` needs no remap → ~100% code-borrow.
 (b) D2b: `LambdaDescriptor` holds non-POD `std::string name/contextualName` + `std::vector<Formal> formals`; flatten them into the blob (offsets into a per-CU char/formal region) so the `lambdas` array borrows in place (extend D2a's OwnedOrBorrowed to descriptors).
 Gate: byte-id both OSes (owned + AOT-borrow == golden); macOS --brute 41/41; Linux 2-process smaps Shared_Clean ≥60% of FULL CU (212MB → ≥127MB) with code+lambdas borrowed; CPU ≤+1% darwin-4. Schema bump kSchemaVersion 21→22.

SEQUENCING: B1 + B2 in parallel (independent). B1 delivers the goal's Linux-brute bar; B2's smaps gate runs under DEFAULT config (unaffected by the B1 crash). Final: both merged, full --brute both OSes, re-measure D2a+D2b Shared_Clean + confirm D3 KPI-5 (~≤70% fresh) unchanged.

OPERATIONAL: linux-1 has NO .git → ship a rev's source via `git archive <rev> src/libexpr-v3 | tar` + rsync, then `meson setup build --reconfigure` (a source/meson.build swap needs reconfigure or ninja errors on stale rules — this invalidated 2 baseline attempts). rsync excludes ANCHORED (`/build`, not `build`). ssh `-o IdentityAgent=none -i ~/.ssh/id_rsa`; ninja incremental, re-invoke across ~10-min ssh timeouts; linux-1 has cache.iog.io (IFDs substitute). CPU ONLY darwin-4; smaps ONLY Linux (--cow-fork harness for per-proc numbers). Layout change → rebuild v3-smoke + bump kSchemaVersion. `#pragma GCC diagnostic` not `clang`. No new env gate w/o inline retirement criterion. Every number git-noted.

STOP when B1+B2 DONE; present Linux --brute 40/40 + macOS 41/41 + byte-id both OSes + D2a/D2b Shared_Clean (≥127MB).
