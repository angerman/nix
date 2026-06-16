# v3-autoresearch — prototype

Karpathy's [autoresearch](https://github.com/karpathy/autoresearch) loop (autonomous
keep-if-better-else-revert) adapted to the v3 VM. Full design + rationale:
[`../lode/AUTORESEARCH_V3_DESIGN_2026-06-16.md`](../lode/AUTORESEARCH_V3_DESIGN_2026-06-16.md).

The one-line thesis: v3 already has the safety substrate autoresearch deliberately omits
(the byte-identity **gate**, the **ratchet**, the win-banking **ledger**, the falsification
**journal**, parallel-agent analysis). This prototype adds the missing **automation layer**
on top of that substrate — which is what makes autonomous keep/revert *safe* on a
correctness-critical VM, where a bare `val_bpb`-style metric would reward-hack into wrong
drvPaths.

## Files

| file | role (autoresearch analog) |
|---|---|
| `program.md` | the research-org spec: objective, HARD constraints, keep/revert bars, fenced-off areas (`program.md`) |
| `do-not-repropose.tsv` | persistent memory of killed levers — read before proposing, append on every revert (autoresearch has none) |
| `autoresearch-cycle.sh` | ONE deterministic experiment: rebuild → measure v3 vs TW on a row → emit a VERDICT (the gate-wrapped `train 5 min → eval → keep/revert`) |
| `autoresearch-loop.workflow.js` | the agent-driven outer loop (scout → explore-in-worktrees → adversarial-verify → synthesize), run via the `Workflow` tool |
| `run-journal.tsv` | append-only verdict log written by the cycle harness |

## Quick start

```sh
# 1. Validate the harness (≈1s, no build, no engine mutation):
bash src/libexpr-v3/research/autoresearch-cycle.sh --selftest

# 2. Grade ONE candidate by hand (you've edited the engine; this rebuilds + gates it):
bash src/libexpr-v3/research/autoresearch-cycle.sh --row foldl --metric cpu
#    → VERDICT  KEEP-CANDIDATE | NEUTRAL | REVERT-* | ABORT-NOT-ENGAGED

# 3. Run the autonomous loop (ONLY on a clean tree — see safety rules):
#    via the Workflow tool:
#    Workflow({ scriptPath: "src/libexpr-v3/research/autoresearch-loop.workflow.js",
#               args: { row: "foldl", metric: "cpu", width: 4 } })
```

## Safety rules (non-negotiable)

1. **Correctness is a HARD gate.** `REVERT-DIVERGENT` (v3 result ≠ TW) is never a win. The
   harness aborts on `ABORT-NOT-ENGAGED` (no `v3-direct` stats line — you may have measured TW).
2. **Adversarial verify every keep.** The loop hands each `KEEP-CANDIDATE` to a verifier that
   tries to *refute* it (engaged? prod path? byte-identical on more rows? special-casing?
   error-masking?). This is the anti-reward-hacking layer — the engine is a gameable metric.
3. **Keeps are PROVISIONAL — no self-flip.** A change is committed but NOT default-flipped for
   any GC/representation change until a periodic full nixpkgs flip-soak + a human confirm it
   (the gen-major rollout discipline, `e863f127d`).
4. **Clean tree only** for the autonomous loop — it spawns worktree agents that rebuild; start
   from a committed state so experiments don't inherit uncommitted engine edits.
5. **Fenced off** (never edit autonomously): `derivationStrict`/store-path semantics, the FFI
   boundary, the writeback/cell machinery (the C-1 family). These need human RCA.

## Status (2026-06-16)

Scaffolding built; `autoresearch-cycle.sh --selftest` PASSES (v3 engaged, byte-identical,
verdict logic correct). No autonomous engine-mutating run has been started — the working tree
has uncommitted `primops.cc` work; the loop should start from a clean commit.

*Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.
SPDX-License-Identifier: Apache-2.0.*
