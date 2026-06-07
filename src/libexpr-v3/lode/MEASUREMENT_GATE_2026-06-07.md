# Why v3-vs-TW numbers keep flipping — and the gate that stops it (2026-06-07)

**Status:** DIAGNOSIS + SHIPPED TOOL. Built after the M5/cardano "near-parity"
claim turned out to be a measurement artifact (v3 never engaged). Codifies the
recurring failure mode and ships an executable gate (`bench/v3-vs-tw-gate.sh` +
`bench/Makefile`) that makes the error class impossible-by-accident.

Operationalizes the prose rules ([[feedback_measure_twice_cut_once]],
[[feedback_same_host_bisect]], [[feedback_head_5_counter_trap]], the #700
hyperfine rule) — because **prose discipline doesn't execute.** It fails exactly
when someone is excited about a result, under time pressure, on a noisy host —
i.e. every time it flipped.

---

## 1. The pattern — ~7 flips this session, ONE failure mode

Not one was an arithmetic error or a noise error. **Every one was "we measured
something other than what we claimed."** The numbers were usually correct *for
what they actually measured*; the error was binding the number to a claim.

| flip | what was *actually* measured | the assertion that kills it |
|---|---|---|
| M5 "8% / near-parity" | **TW-2.35 vs TW-2.34.7** — v3 never engaged (`flake#attr` → TW) | **ENGAGED**: abort unless v3 emitted `v3-direct` stats |
| firefox "parity" | v3 **faked the store path** — arms did different work | **IDENTICAL**: refuse parity unless v3 out == TW out byte-for-byte |
| register-VM "1.5× / 2× never a ceiling" | **intra-v3** (register vs stack), not v3-vs-TW | **COMPARAND**: one arm v3-engaged, one arm TW |
| TW ref = fork-2.35 | **unpinned** baseline binary | **PINNED-REF**: record nix version + path |
| fib "635K thunks" | `--expr` **without optimise** (non-production path) | **PROD-PATH**: `nix eval` (optimise+strictness on), not bare `v3-eval` |
| Chain "−65%" | **divergent eval** (did less work) | **IDENTICAL** (again) — different result ⇒ different work |
| wall "6–24 s" | **host noise** | **METRIC**: user-CPU + peak-RSS only; no bare wall |

## 2. Two systemic causes

1. **You cannot tell which engine ran from the command.** The CLI has three
   eval paths (TW, v3-direct-`--expr`, flake-installable) and v3 *silently
   falls back to TW* (the V3-NATIVE bridge, fake-store, the flake path). M5 is
   the pure form: `NIX_V3_DIRECT_EVAL=1 nix eval cardano#…` *looks* like a v3
   measurement and *is* a TW measurement. The engine actually used is invisible.
2. **The flips cluster optimistic** (M5 near-parity, register-VM 1.5×, Chain
   −65%, #700 "2× faster"). A surprising-good result — or a fix — triggers
   *reporting before verifying the setup*. The fix must be direction-neutral:
   a gate that refuses the number regardless of which way it points.

## 3. The gate — abort-on-unmet-precondition, not best-effort-report

`bench/v3-vs-tw-gate.sh` runs both arms and **refuses to emit a number until
the preconditions binding it to a "v3 vs TW" claim hold.** Every assertion is
printed so a human can sanity-check (the tool is not a black box). Honest
decision tree:

```
 run TW arm (no DIRECT)  +  run v3 arm (DIRECT=1, NIX_VM_STATS=1, clean caps)
        │
        ├─ v3 NOT engaged (no `v3-direct` stats)  ─────────►  ABORT  (TW-vs-TW; refuse)
        ├─ v3 engaged but exit≠0 (timeout/error)  ─────────►  "v3 INCOMPLETE"  (the cardano result)
        ├─ engaged, completed, result DIFFERS     ─────────►  "DIVERGENT"  (refuse parity; metrics withheld)
        └─ engaged, completed, result IDENTICAL   ─────────►  clean row, all gates green
```

- **ENGAGED** = ≥1 `v3-direct` line on stderr — the empirically-verified
  discriminator (TW emits none; selftest §4 proves it).
- **COMPARAND** is *structural*: the TW arm has no `DIRECT`, the v3 arm does —
  you cannot accidentally compare v3-to-v3.
- **PROD-PATH**: uses `nix eval --expr` (optimise+strictness on), not the bare
  `v3-eval` binary (which skips them — the fib-635K trap).
- **METRIC**: min user-CPU over `RUNS` + max peak-RSS — both load-insensitive;
  no bare wall (the 6–24 s noise). instructions-retired is a Linux-builder
  add-on (darwin has no `perf`); the gate is fully useful without it.
- v3 arm runs under `NIX_V3_MAX_WALL_TIME`/`MAX_HEAP` so a pathological eval
  (cardano/haskell.nix) **times out cleanly and is reported as INCOMPLETE** —
  honest data, not a hang or a bogus ratio.

## 4. Dogfood — proof it works (run 2026-06-07, this host)

The measurement tool gets its own test (it can be buggy too):

- **`--selftest` → PASS.** `DIRECT=1` run detected ENGAGED (insns=32); plain run
  detected TW (no false positive). The detector distinguishes the engines.
- **fold workload → all gates green, v3 1.82× slower / 1.39× mem** (user-CPU
  0.11→0.20 s; peak 41→57 MB). The honest number *caught my own mislabeling*: I
  called it "pure eval" but it is genList/`listToAttrs`/`foldl'` =
  primop-iteration (the v3-weak fold-add-17× family), not the attrset-*forcing*
  the team measured as 1.87× faster. **"Pure eval" is not a monolith** — split
  lookup/forcing (v3-strong) from construction/iteration (v3-weak).
- **firefox.drvPath → DIVERGENT, parity REFUSED.** ENGAGED ✓ (insns=18.2M, v3
  ran the full eval), but TW `/nix/store/xxd13j…` ≠ v3 `/v3-fake-store/45c208b5…`
  → metrics withheld. **The exact firefox error, now impossible to report by
  accident.**

## 5. How to run

```bash
cd src/libexpr-v3/bench
make selftest                          # validate the detector FIRST
make measure-lookup                    # v3-strong regime
make measure-fold                      # v3-weak regime (primop iteration)
make measure-firefox                   # demonstrates the DIVERGENT catch
make measure WORKLOAD='1 + 1'          # arbitrary expr, gated
# knobs: RUNS=5 V3_WALL=300s V3_HEAP=8G NIX_BIN=/path/to/nix ALLOW_DIVERGENT=1
```

The cardano/M5 case routes through `--expr '(builtins.getFlake "…").…drvPath'`
→ v3 engages and (per the team) times out → the gate reports **v3 INCOMPLETE**,
never the false "8% slower."

## 6. Operationalization (the rule)

**Every v3-vs-TW perf/memory claim — in a commit body, a lode doc, or a status
report — must come from this gate, or state explicitly why not.** This is the
executable form of the #700 hyperfine rule. A "v3 is X× faster/slower" sentence
without a gate row (or an honest INCOMPLETE/DIVERGENT verdict) is unsubstantiated
and should be treated as such in review.

**Complementary in-product fix (higher leverage, team-owned):** make v3 *itself*
warn when `NIX_V3_DIRECT_EVAL=1` is set but v3 is bypassed (flake path → TW).
That catches the M5 class *everywhere* — including ad-hoc eval — not just in the
benchmark. The gate protects measurements; the warning protects everyone.

## 7. Limitations / honest caveats

- The gate is darwin-first (user-CPU + peak-RSS); instructions-retired needs the
  Linux builders or `xctrace`. The two **gates** (ENGAGED, IDENTICAL) — which is
  where every flip happened — are host-independent.
- IDENTICAL is byte-equality of the eval *result*. For drvPath under the
  fake-store, it will always flag DIVERGENT until v3 produces real store paths —
  which is correct: we genuinely cannot do a clean v3-vs-TW drvPath comparison
  until then (only `ALLOW_DIVERGENT=1` for a deliberately-scoped memory-only
  reading, e.g. [[MEMORY_REPRESENTATION_2026-06-07]] §4b).
- New files only — `bench/v3-vs-tw-gate.sh`, `bench/Makefile`. No engine source
  touched; safe to run alongside in-flight memory work.

---

Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.
SPDX-License-Identifier: Apache-2.0
