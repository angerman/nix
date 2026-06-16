# Autoresearch for the v3 VM — design + prototype notes (2026-06-16)

Adapting Andrej Karpathy's **autoresearch** (the autonomous keep-if-better-else-revert
experiment loop) to the v3 bytecode VM. This doc is the detailed reference; the prototype
scaffolding lives in `src/libexpr-v3/research/`.

## 1. What autoresearch is (grounded in the primary source)

[`karpathy/autoresearch`](https://github.com/karpathy/autoresearch), released 2026-03-07,
~630 lines around a single-GPU nanochat. The loop:

1. An LLM coding agent (Claude/Codex) reads **`program.md`** — plain-English research
   directions + constraints. *You never touch the Python.*
2. The agent edits `train.py`.
3. Train for **exactly 5 minutes** wall-clock (the budget is the clock).
4. Evaluate **one metric: `val_bpb`** (validation bits-per-byte; lower is better,
   vocab-size-independent so architectural changes compare fairly).
5. **Keep if better, else revert.** Loop unattended overnight (50–100 experiments).

What it **deliberately omits** (verified against the repo): no correctness gate, no
anti-metric-gaming guard, no overfitting guard, no persistent experiment journal /
leaderboard, no sandbox, single-agent. Karpathy's stated future is "asynchronously
massively collaborative … SETI@home style — emulate a research *community*, not one PhD
student." The minimalism is fine for ML training (a slightly-wrong model is just a worse
model) and is exactly what makes *naive* autoresearch reckless for a correctness-critical
compiler/VM.

Sources: github.com/karpathy/autoresearch; datacamp.com/tutorial/guide-to-autoresearch;
x.com/karpathy/status/2030705271627284816 (the collaborative future).

## 2. The thesis

**The v3 team has already built the hard part autoresearch lacks — and is running the
loop by hand.** Months of work produced precisely the safety substrate that single-metric
keep/revert is missing. So "apply autoresearch" is NOT "adopt the harness wholesale"; it
is **add the automation loop on top of v3's existing gate/ratchet/ledger/falsification
discipline** — and that discipline is the prerequisite that makes autonomous keep/revert
*safe* here, where it would be dangerous on a bare metric.

| autoresearch component | v3 equivalent that already exists |
|---|---|
| `train.py` (mutated artifact) | the engine: `emit.cc`, `vm.cc`, `opt_*.cc`, `alloc.hh` |
| `train 5 min` (the experiment) | `nix develop -c ninja` rebuild + `make measure WL=…` |
| `val_bpb` (single, lower-better) | the gate's (CPU ratio, arena MB) vs the pinned baseline |
| keep-if-better-else-revert | the **ratchet** (`bench/baselines/seven-rows.tsv`, don't-regress) + **cumulative-rss-ledger** (bank wins) + Rule-0 keep/revert with **pre-committed bars** |
| `program.md` (directions + constraints) | `lode/PLAN_BEAT_TW_V2`, the pre-committed bars, the **do-not-repropose** list |
| log of experiments | `lode/` docs + falsification-rule commit bodies ("what hypothesis does this kill") |
| (Karpathy's *future*) parallel agents / SETI@home | the team already fans out 4–6 analysis agents; `Agent isolation:worktree` + the `Workflow` tool are the substrate |

## 3. The three non-negotiable adaptations

These are what separates a safe v3-autoresearch from the reckless naive port.

**A. Correctness is a HARD gate, not part of the reward.** `val_bpb` is a soft number;
a v3 "win" that produces a wrong drvPath corrupts the store/cache. The keep predicate must
be `byte-identical-to-TW AND faster` — the gate's "**DIVERGENT verdict IS the result**"
rule (`bench/v3-vs-tw-gate.sh`, `lode/MEASUREMENT_GATE_2026-06-07.md`). v3 already has
this; autoresearch has nothing like it.

**B. An adversarial verifier, because reward-hacking is an active adversary here.** This
project's history IS a catalog of an (unintentional) optimizer gaming the metric:
fake-store masking (turned errors into wrong-but-passing values), `nix-instantiate`
routing to TW (measured the wrong engine), the CU disk-cache key that poisons A/Bs (warm
cache serves the wrong arm), benchmark special-casing. An autonomous agent *with an
objective and write access to the engine will find these within a few iterations.* Every
"win" must therefore pass a second agent whose job is to **refute** it: is v3 ENGAGED (≥1
`v3-direct` stats line; TW emits none)? is it the PROD path (not `nix-instantiate`)? is
the result byte-IDENTICAL to TW? is it COMPARING the right things (not a special-cased
benchmark expr)? did it remove a correctness check or mask an error to "win"? This is the
gate's ENGAGED+IDENTICAL+PINNED+COMPARAND verdict, automated, plus the session's
"adversarially verify findings" pattern.

**C. Persistent memory of dead branches.** autoresearch's single thread re-derives; v3 has
the **do-not-repropose** list (arity-cache byte 6b5284374, OP_LESS, ValuePair 24/32 split
38c263bdb, Boehm tuning, key de-stringify 7e32072f8, GET+FORCE fusion-as-miss, …). An
autonomous loop MUST read it first or it burns cycles re-falsifying killed levers. The
loop also WRITES to it on every revert (the revert is data — Rule 0).

## 4. The tiered loop (cycle-time engineering autoresearch gets for free at 5 min)

autoresearch's appeal is a tight 5-min clock. v3's clock is rebuild (minutes for `vm.cc`
or any header) + gate (N runs × rows) + (for keepers) the full nixpkgs flip-soak (~4.5 h
on darwin-4). So the loop is tiered, mirroring the measure-twice "cheap directional proxy
→ pre-committed threshold → run":

```
read program.md + do-not-repropose + current ratchet/ledger
  → propose ONE localized change (prefer small .cc TUs; headers rebuild the world)
  → git worktree + rebuild (stale-binary trap: rebuild the right targets)
  → FAST PROXY GATE  (1–2 relevant rows, laptop, autoresearch-cycle.sh):
       not ENGAGED          → ABORT  (measuring the wrong engine; distrust)
       DIVERGENT            → REVERT (correctness; non-negotiable) + journal
       worse than baseline  → REVERT + journal (→ do-not-repropose)
       not better by bar    → NEUTRAL → revert (avoid carcasses)
       better by bar        → KEEP-CANDIDATE
  → KEEP-CANDIDATE: full 7-row gate + lang --core + adversarial verify
  → survives: queue for darwin-4 full flip-soak (the broad net)
  → commit with the Rule-0 body (A/B data + hypothesis killed); bank in the ledger
```

**Provisional-keep rule:** the fast gate only covers measured workloads; a change can pass
7 rows + flip-soak and still be wrong on an unmeasured shape. So an autonomous keep is
PROVISIONAL until a periodic broad sweep + a human confirm a default-flip — exactly the
gen-major rollout pattern (gated → 24,882-attr soak → flip, `e863f127d`). **The agent
proposes; it must not self-flip a default.**

## 5. Parallelism = Karpathy's SETI@home vision, already feasible

Fan out K agents on distinct levers (emit-fusion, alloc-layout, a GC-cadence knob, an
opt-pass), each in its own `isolation:"worktree"`, each gated independently; a synthesis
step merges non-conflicting keepers and **re-gates the merge** (two individually
byte-identical changes can interact). The `Workflow` tool's `parallel()`/`pipeline()` is
the substrate. The read-only version (4–6 parallel analysis agents) has been used all
session; this closes the loop. Caveat: each worktree needs its own `build/` (disk + first
build cost), so parallel width trades disk/CPU for wall-clock.

## 6. Where to point it (and where NOT to)

**Best first targets** (high ROI, low risk, fast rebuild, clean metric — autoresearch's
sweet spot):
- **Optimizer passes + emit fusions** (`opt_*.cc`, `emit.cc`): small TUs → fast rebuild;
  CPU-measurable; byte-identity-gated. The manual versions already shipped (SET_LOCAL_KEEP,
  identityLambda fix 68e9d0cba, cross-Force deferral). Target the foldl / list-iter rows.
- **Continuous tuning knobs**: nursery size, gen-major trigger thresholds, IC sizes,
  growth factors — literal hyperparameters, the keep/revert sweet spot. **Arena MB is
  DETERMINISTIC** (cleaner than `val_bpb`'s noise; cleaner than maxRSS), so the memory
  decision is tighter than ML autoresearch gets on loss.

**Fence off in `program.md`** (needs human RCA; an optimizer here would re-introduce the
masking/cross-write bug family): drvPath semantics, the FFI boundary, the writeback/cell
machinery (the C-1 family), and `derivationStrict`/store paths.

## 7. Honest limits

1. **Build time dominates.** A `vm.cc` or header edit rebuilds the world (minutes), so the
   loop is 10–30 min/arm — 10–50× lower throughput than ML autoresearch. Mitigate with
   edit-locality (prefer `.cc` over headers), ccache/incremental, and parallel arms.
2. **Fast gate ≠ full coverage.** The 24,882-attr flip-soak is the broad net but too slow
   for the inner loop → provisional-keep + periodic broad sweep before any flip (§4).
3. **Greedy hill-climbing rots into carcasses.** Needs a periodic consolidate/simplify
   pass and the Rule-0 "no opt-in gate to let both coexist," or the engine accretes
   benchmark-specific hacks.
4. **Reward-hacking is the central risk, not a footnote** — §3B is mandatory, not optional.

## 8. Prototype (this session)

Scaffolding in `src/libexpr-v3/research/` (built; the deterministic harness validated on a
trivial workload — NO autonomous engine-mutating run started, because the tree has
uncommitted `primops.cc` work):

- **`program.md`** — the v3 research-org spec: objective, HARD constraints (byte-identity
  + lang + no-special-casing + no-ungated-env), pre-committed keep/revert bars, fenced-off
  areas, the metric definitions.
- **`do-not-repropose.tsv`** — seeded journal of session-falsified levers (the loop reads
  before proposing, appends on every revert).
- **`autoresearch-cycle.sh`** — the deterministic, gate-wrapped experiment runner: rebuild
  → measure v3+TW on a target row (reusing the `pin-seven-rows` measurement contract) →
  emit a machine-readable verdict (ABORT-NOT-ENGAGED / REVERT-DIVERGENT / REVERT-REGRESS /
  NEUTRAL / KEEP-CANDIDATE) → append to a run journal. `--selftest` validates the verdict
  logic on a sub-second workload against the current binary (no rebuild, no engine
  mutation). This is the safe, testable core that the agent loop calls.
- **`autoresearch-loop.workflow.js`** — the agent-driven outer loop, authored as a
  `Workflow` script file (run via the `Workflow` tool when the tree is clean): read spec +
  journal → propose → worktree → `autoresearch-cycle.sh` → adversarial-verify →
  keep/revert/journal; parallel arms.
- **`README.md`** — how to run it (and the do-not-self-flip / clean-tree rules).

## 9. Rule-0 framing

The prototype's standing hypothesis: **"the manual analyze→propose→measure→keep/revert
loop the team runs can be automated safely *because* the gate/ratchet/journal substrate
already enforces correctness + anti-gaming."** The first autonomous run on a low-risk
target (emit-fusion or a GC knob) either confirms it (a byte-identical CPU/arena win lands
unattended) or kills it (the adversarial verifier catches the loop gaming the metric — in
which case the gate predicates need hardening, which is itself the finding).

*Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.
SPDX-License-Identifier: Apache-2.0.*
