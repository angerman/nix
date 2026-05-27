# Test infrastructure audit (2026-05-08)

> **ARCHIVED-IN-PLACE 2026-05-27**: This doc is historical (pre-strategic-doc-set or one-off RCA). Preserved here because it is still referenced from one or more KEEP docs. See [`LODE_CLEANUP_REVIEW_2026-05-27.md`](LODE_CLEANUP_REVIEW_2026-05-27.md) for the archival rationale.

---


A walk-through of the v3 test suite in `src/libexpr-v3/test/`, focused
on the question: **do we use the regular nix evaluator (tree-walker)
as oracle?**  Short answer: yes, in three modes, layered.  The cutover-
parity and drv-parity scripts are the strict floors; the standalone
`v3-eval` path is mostly tested against frozen goldens.

This audit catalogues each test runner, classifies its oracle, lists
the gaps, and ranks the cheapest improvements.

---

## 1. Oracle modes

| Mode | Where | Strength |
|---|---|---|
| **Recorded golden** (`tests/functional/lang/eval-okay-*.exp`) | `run-lang-tests.sh` | Frozen TW output.  Stable spec; drifts from live TW silently if upstream changes. |
| **Live TW process** (`nix-instantiate` or `nix eval`) | `run-cutover-parity-tests.sh`, `run-drv-parity.sh`, `run-v3-tests.sh`, ~10 bridge/laziness scripts | Strongest gate.  Asserts byte-equal output against the engine v3 is replacing. |
| **Hand-authored expected literal** | `smoke.cc`, `run-wc-laziness-tests.sh`, most targeted-bug scripts | Author-derived from TW behaviour but not re-run.  Strong as regression pin; weak as oracle (frozen at script-write time). |

The lang-test runner (`run-lang-tests.sh:88–94`) prefers the recorded
`.exp` golden and falls back to live TW only when no `.exp` exists.
That's the right hierarchy for spec-stability but means the lang
suite is not a *live* TW comparison.

---

## 2. Test layers

| Tier | Runner | Oracle | Surface | Count |
|---|---|---|---|---|
| Unit (C++) | `smoke.cc`, `evalscope-handles.cc`, `drv-preflight.cc` | hardcoded literals | hand-built IR; bypasses parser/lower | ~35 + 6 + 1 |
| Lang via standalone CLI | `run-lang-tests.sh` | `.exp` (preferred) → live TW (fallback) | `v3-eval` (pure v3, no TW backstop) | ~146 |
| Lang via cutover (smoke) | `run-cutover-tests.sh` | exit code only | `NIX_USE_V3=1 nix-instantiate` | ~146 |
| **Lang via cutover (parity)** | `run-cutover-parity-tests.sh` | **live TW byte-compare stdout** | `NIX_USE_V3=1 nix-instantiate` | ~146 |
| Eval-fail | `run-fail-tests.sh` | regex `error\|aborted\|throw\|assert\|fail` on stderr | `v3-eval` | varies |
| **Drv hash parity** | `run-drv-parity.sh` | **live TW byte-equal `.drvPath`** | both via `nix-instantiate` | 25 |
| Cross-engine smoke | `run-v3-tests.sh` | live TW | `v3-eval` vs `nix eval --impure` | 75 |
| WC laziness regression | `run-wc-laziness-tests.sh` | hardcoded literals | `v3-eval` | ~75 |
| Targeted-bug regression (~14 scripts) | `run-456-...`, `run-458-...`, `run-bridge-*`, `run-broader-thunkify-*`, `run-fix-inherit-from-self-*`, `run-gate-removal-*`, `run-inherit-from-laziness-*`, `run-intrinsic-recognition-*`, `run-lazy-bridge-arg-*`, `run-mutual-circular-formals-*`, `run-on-demand-root-*`, `run-tw-lambda-bridge-*` | mix: hardcoded + live TW | mostly cutover (`NIX_USE_V3=1 nix`) | varies |
| Disk-cache E2E | `run-disk-cache-tests.sh` | hardcoded literals | `v3-eval` (cold + warm processes) | 5 |
| Perf bench | `bench-v3-vs-tw.sh` | not correctness | both | 9 |
| Bisect | `wc38-bisect-harness.sh` | not a test | — | — |

The two strictest TW-oracle tests are **cutover-parity**
(`run-cutover-parity-tests.sh`) and **drv-parity**
(`run-drv-parity.sh`).  Drv-parity is the strictest: byte-equal
drvPath transitively asserts every attr coercion, hash-input
ordering, and string-context bit matches TW.

---

## 3. What's working

- **Cutover-parity gate** runs every lang-suite case under both
  engines and byte-compares stdout.  Any silent miscompile in the
  cutover path lights up here.
- **Drv-parity** covers 25 representative shapes including
  `__structuredAttrs`, fixed-output, `__contentAddressed`,
  `__impure`, and `builtins.path` with NAR hashing
  (`run-drv-parity.sh:38–124`).  Phase A/B/C/D coverage is built
  into the test layout, so unfinished phases self-document.
- **Two evaluator surfaces tested separately**: `v3-eval` (pure v3,
  no TW backstop, harshest) and `NIX_USE_V3=1` (cutover, with TW
  fallback for unsupported AST).  Both surfaces matter for
  different reasons — the first proves v3 standalone, the second
  proves the production cutover doesn't regress.
- **Disk-cache E2E** specifically engineers symbol-id divergence
  between cold and warm processes to surface remap-pass holes
  (`run-disk-cache-tests.sh:1–31`).  CRIT-1 (the
  `OP_REC_BINDING_SLOT_REF` missing remap arm) was caught by
  exactly this design.  `V3_STRICT_DISK_CACHE=1` re-throws
  cached-CU errors instead of silently falling back to fresh
  compile.
- **Targeted-bug regression scripts** — every closed bug (#455,
  #458, #495, WC-37/38, the bridge family, inherit-from laziness,
  TW-lambda bridge, on-demand-root, intrinsic recognition) has a
  co-located shell script.  A future revert lights up by name.
- **WC-laziness suite** (1051 lines, ~75 cases) is a structural
  strong point: each case is annotated with the commit hash that
  made it pass and the pre-fix symptom.  Regression bisection
  becomes a one-line grep.
- **Optimizer pass coverage** in `smoke.cc` exists for all six
  passes — const-fold (6 cases), DCE (2), CSE (2), inline (1),
  primop-fuse (2), strictness (5).  REVIEW_2026-05-06 §B6's "zero
  unit tests" claim is stale.

---

## 4. Gaps and caveats

### 4.1 Lang-suite oracle is *frozen* TW, not live TW

`run-lang-tests.sh:88–94` prefers the `.exp` golden over live TW.
Frozen goldens are the right oracle for *spec stability* but drift
silently from upstream TW if the live engine's behaviour changes.
For "v3 implements TW's actual semantics correctly" questions, the
cutover-parity script is the better gate.

### 4.2 Optimizer pass coverage is end-to-end, not IR-in/IR-out

`smoke.cc` runs each test by hand-building IR, compiling, executing,
and checking the resulting `Value`.  A pass that *should* fire but
doesn't isn't caught — only a pass that produces a *wrong* value is.
The recommended fix (REVIEW_2026-05-06 §B6 / §7) is per-pass IR-in /
IR-out fixtures; that work hasn't landed.

### 4.3 Eval-fail does not verify error-message text

`run-fail-tests.sh:73` accepts any stderr matching
`error|aborted|throw|assert|fail` as pass.  TW's own eval-fail
suite compares the full message via `.err.exp` files.  v3 catches
"did it raise an error" but not "did it raise the *right* error"
— a wrong-message regression is silent.

### 4.4 Display-format divergence is masked

`run-v3-tests.sh:212` accepts `<list of N>` / `<attrs of N>` from
v3 as "display-only diff" against TW's `[ 1 2 3 ]`.  Defensible
for a values-regression test, but means the printer's output
format isn't itself tested against TW for `v3-eval`.  Cutover-
parity covers this (it uses `nix-instantiate`'s shared printer);
standalone `v3-eval` does not.

### 4.5 Mutually-circular formals: "observe", not "match"

`run-mutual-circular-formals-tests.sh:21–29` explicitly documents
known divergent behaviour: v3 reads sibling defaults eagerly, TW
thunks them.  The test asserts current divergence so a future
unification change must update the test.  Right design for a
documented limitation; flag here for completeness.

### 4.6 WC-laziness expected values are hand-typed

The header says "expected is what tree-walker / v3 should both
produce" but the script never invokes TW.  Frozen at script-write
time.  Strong regression pin, weak oracle — if TW changes, the
script keeps passing because the literal is frozen.

### 4.7 No fuzzing, no property-based testing, no mutation testing

The corpus is hand-curated.  Coverage is good for known shapes;
latent bugs in shapes nobody has written depend on cutover-parity
catching them via nixpkgs evaluation, which isn't currently in the
gate (see §4.8).

### 4.8 No real-world flake regression in CI

cardano-node is the canonical hard target across project memos and
reviews, but `bench-v3-vs-tw.sh` measures time, not correctness.
The closest is `run-wc38-nixpkgs-probe.sh` — a probe, not a gate.
A small nixpkgs evaluation in the test path (e.g.,
`(import <nixpkgs> { ... }).hello.name`) would catch a class of
regressions lang tests don't reach.  REVIEW_2026-05-06 §3 #6 makes
the same recommendation.

### 4.9 Cutover-parity has timeout=10s

`run-cutover-parity-tests.sh:54` discards slow-but-correct cases
as `<tw-error>` after 10 seconds.  Silent loss of coverage if the
test would have passed eventually.

---

## 5. Recommendations, ranked

| # | Action | Cost | Why |
|---|---|---|---|
| 1 | **Add a nixpkgs canary to the parity gate** | half a day | One synthetic that runs `(import <nixpkgs> { ... }).hello.name` under both engines and byte-compares.  Catches cardano-node-class identity bugs (#455, #498) without running the whole flake.  Same recommendation as REVIEW_2026-05-06 §3 #6. |
| 2 | **Tighten eval-fail with `.err.exp`** | half a day | Mirror the lang-test `.exp` design; compare error messages when an `.err.exp` is present.  Catches wrong-message regressions. |
| 3 | **Per-pass IR-in/IR-out fixtures for 6 optimizer passes** | 1–2 days | REVIEW_2026-05-06 §7 calls this the smallest useful next PR.  Silent-miscompile gate; pays back forever once aggressive optimizer work resumes. |
| 4 | **Promote a few WC-laziness tests to live-TW oracles** | a few hours | Cheap; if the literal and live TW disagree, *something* changed and we want to know which side.  Pick the most-load-bearing 5–10 cases. |
| 5 | **Bump cutover-parity timeout or split slow cases** | minor | 10 s is too tight for the larger lang tests under sanitizers; silent coverage loss today. |

---

## 6. Verdict

The oracle story is **soundly built but uneven**.  Cutover-parity
and drv-parity are the right floors and they catch divergence
where it matters for the production path.  Targeted-bug regression
is strong — every closed defect has a co-located script.  The
standalone `v3-eval` path is mostly tested against frozen
goldens, which is fine for spec-stability and weaker for tracking
live TW evolution.

The visible holes — error-message comparison, IR-fixture
optimizer coverage, and a nixpkgs canary in the gate — are all
cheap to close.  None require architectural changes.  Items #1
and #3 from §5 are the highest-leverage cheap wins; both have
been recommended in earlier reviews and remain outstanding.

---

Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.
SPDX-License-Identifier: Apache-2.0
