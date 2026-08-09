# Tier-D Hardening Plan — Differential Fuzzer + Live Production Parity Monitor

**Scope:** a written design plan (NOT an implementation) for closing two open
items in the v3 debug story (`src/libexpr-v3/CLAUDE.md` §"The debug story",
LESSONS §4.9): item #6 *differential fuzzing* ("random Nix expressions, parity
assert — not yet built") and item #2's gap ("arbitrary-input front door"). Adds
a live nix-eval-jobs parity monitor that turns v3's production deployment into a
continuously self-auditing net.

**Author:** Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.
SPDX: Apache-2.0.

**Grounding — files read for this plan:**
- `src/libexpr-v3/CLAUDE.md` (debug story, Rule 0, operational limits, brute gate)
- `src/libexpr-v3/test/run-arith-compare-parity-tests.sh` (the 3-way P/O/T oracle, `@@@` row format)
- `src/libexpr-v3/test/run-primop-edge-parity-tests.sh` (dual-path P/O/T positives+negatives)
- `src/libexpr-v3/test/all-v3-tests.sh` (the 22-suite `--brute` battery + suite table)
- `src/libexpr-v3/test/property/property_tests.py` (seeded generator, `error_class()`, `compare()`)
- `src/libexpr-v3/test/property/run-property-tests.sh`, `.../README.md`
- `src/libexpr-v3/test/REPROS.md` (fixture-manifest convention)
- `src/libexpr-v3/include/v3/primop.hh` (`PrimOp{name,arity,lazyArgs,flags}`, `allRegisteredPrimOps`)
- `src/libexpr-v3/primops.cc` (`registry()`, `findPrimOp`, `allRegisteredPrimOps` @9389)
- `src/libexpr-v3/vm.cc` (`builtins` attrset materialized from `allRegisteredPrimOps()` @11270)
- `nix-eval-jobs/src/worker.cc` (`processJobRequest`, TW-retry net, `ensureTwRoot`, `rebuildV3`, `envFlagEnabled`, `NIX_V3_REQUIRE`)
- `nix-eval-jobs/src/drv.hh` (`Drv{...}` + `operator==` default; `fromV3` vs `fromPackageInfo`)
- `nix-eval-jobs/src/nix-eval-jobs.cc` (collector reads `Response::Error{fatal}`)

---

## 1. Problem and the coverage gap it closes

v3 is IN PRODUCTION on Hydra via nix-eval-jobs, and correctness bugs keep
surfacing **empirically**, not by code review:
- `div INT64_MIN / -1` → silent wrap on aarch64, **SIGFPE worker-crash on x86_64**;
- `readFile`/`hashFile` of an unbuilt derivation → **divergent drvPath**, both
  engines "succeed" but disagree.

Both are **dual-path** bugs: every operation with a hot `OP_*` opcode AND a
`builtins.*` primop path (div, head/tail, readFile/hashFile, select, `++`, `//`,
comparison, bit ops…) can diverge on *one* path while the other stays correct.
The curated 3-way harness (`run-arith-compare-parity-tests.sh`,
`run-primop-edge-parity-tests.sh`) is exactly the right oracle for this — P
(primop) vs O (opcode) vs T (tree-walker) — but it only fires on the
**~30 hand-authored inputs** someone already thought to write.

Two structural blind spots remain:

1. **The 215-primop surface.** `builtins` is materialized in v3 from
   `allRegisteredPrimOps()` (`vm.cc:11270`); the registry (`primops.cc:119`)
   holds ~215 entries. Only ~30 have curated parity rows. The other ~185
   (obscure: `genericClosure`, `splitVersion`, `parseDrvName`, `zipAttrsWith`,
   `foldl'` corner arities, `convertHash`, `groupBy`, `replaceStrings` overlap,
   `match`/`split` group semantics, `fromJSON`/`toJSON` round-trips, `deepSeq`
   on cyclic structure…) are essentially unaudited for byte-parity on boundary
   inputs.
2. **Error-message parity.** v3 ships its OWN error UX (see
   `lode/ERROR_UX_DESIGN_2026-05-20.md`: Diagnostic struct, two-span errors,
   Levenshtein "did you mean", error codes, trace summarisation). So a naive
   whole-message compare **false-positives constantly**. The arith parity script
   itself documents this (lines 128-139): the incomparable-types error TEXT
   *legitimately* diverges (v3 terse "expected comparable types" vs TW "cannot
   compare a set with a set…"), so that test deliberately asserts only "neither
   engine returns a bool." The **high-value** error signal is not message text —
   it is **throw-vs-succeed class parity** (the div-SIGFPE and catAttrs-fail-open
   bugs were both class mismatches: v3 succeeded-with-junk where TW threw).

The fuzzer attacks blind spot #1+#2 offline; the monitor attacks the
*both-succeed-but-differ* drvPath class that even the fuzzer can miss because it
only manifests on real store-backed workloads (readFile-of-derivation needs a
real store path).

---

## 2. Differential fuzzer design

### 2.1 Generator — type-directed, grammar-based, well-formed by construction

**Goal:** most generated expressions must *evaluate* (produce a value or a typed
eval-error), not parse-fail. A parse-fail teaches nothing about semantic parity.

**Approach:** extend the existing generator, do not start fresh. The seed is
`src/libexpr-v3/test/property/property_tests.py` — reuse its typed value
generators (`gen_int`, `gen_string`, `gen_list_int`, `gen_attrs_int`), its
`run_eval`, and `filter_warnings`. Add a recursive **type-directed** expression
generator `gen_expr(t, depth, rng)` producing an expression of a target Nix type
`t ∈ {Int, Float, Bool, String, Path, List<τ>, Attrs, Lambda, Null}`:

- **Leaves** (depth 0 or by probability): typed literals + **boundary values**:
  `INT64_MIN` (`-9223372036854775807 - 1`), `INT64_MAX`, `0`, `-1`, `1`;
  `""`, empty list `[ ]`, empty attrs `{ }`; UTF-8 strings (`"café"`, emoji,
  combining marks — the `stringLength "café" == 5` byte-vs-codepoint trap);
  out-of-range indices; large/deeply-nested structures (depth-controlled).
- **Internal nodes** are chosen **by return type** from a type→producers table:
  - operators: `+ - * / < <= > >= == != ++ // && || ->` and unary `- !`;
  - primop applications drawn from the live `builtins` surface, keyed by the
    return type the caller needs and the arities from
    `allRegisteredPrimOps()`/`builtins`;
  - binders/control: `let … in`, `with … ;`, `rec { … }`, `if…then…else`,
    `assert c; e`, string interpolation `"…${e}…"`, lambda + application.
- **Recursion**: arguments are `gen_expr(argType, depth-1, rng)`, so the whole
  tree is well-typed at the Nix level by construction. It can still THROW at
  eval (div-by-zero, oob, missing attr) — which is exactly the error-parity
  surface we want.

**Split-risk bias (the core of the design).** Because the div/head/tail bugs are
opcode-vs-primop splits, the generator must (a) over-weight the dual-path op
families, and (b) emit **both syntactic forms** so the O and P paths are both
exercised against T:
- `a + b` **and** `builtins.add a b`; `a / b` **and** `builtins.div a b`;
- `xs.name` / `xs.name or d` **and** `builtins.getAttr "name" xs` /
  `builtins.getAttr`+`hasAttr`;
- `a ++ b` **and** `builtins.concatLists [a b]`; `a // b` **and**
  `builtins.mapAttrs`/`intersectAttrs` compositions;
- `builtins.head`/`tail`/`elemAt`/`length`, comparison operators, `bitAnd/Or/Xor`.
The 3-way oracle then catches P≠O (v3-internal) and (P==O)≠T (v3-vs-TW) for free.

**Coverage-driving toward the 215 surface.** At startup enumerate the surface on
BOTH engines: `nix eval --expr 'builtins.attrNames builtins'` (TW) and the same
under `NIX_V3_DIRECT_EVAL=1` (v3, materialized from `allRegisteredPrimOps`). This
gives two free wins: (1) a **surface diff** — any primop present on one engine
and missing on the other is a divergence reported immediately; (2) a
**coverage-guided bias** — track per-primop emission counts and steer the
generator toward under-covered names (name-coverage, not branch-coverage; no
instrumentation needed). This directly attacks the "215 registered, ~30 covered"
gap and makes progress *measurable* (coverage % is a first-class metric).

**Impurity blocklist (determinism).** The generator NEVER emits impure/
non-reproducible primops by default: `currentTime`, `currentSystem`, `getEnv`,
`readFile`/`readDir`/`pathExists` of non-fixture paths, `fetchGit`/`fetchTree`/
`fetchurl`, `path`, `storePath`, `exec`, `trace`. Run under `--pure-eval` where
the grammar allows. (A separate, explicitly-store-backed generator mode is a
later phase — see the monitor, which covers store-backed drvPath divergence on
real workloads instead.)

**Seeding (project rule — no `Date.now`/unseeded random).** One `--seed N` drives
a `random.Random(seed)`; every expr is reproducible from `(seed, index)`. The
existing `property_tests.py` already models this (`--seed`, default
`20260517`); the fuzzer inherits it. Wall-clock is NEVER an input — timestamps in
ledgers are the seed+index, not `time.time()`.

**Resource discipline (CLAUDE.md operational rule).** Every probe carries
`NIX_V3_MAX_WALL_TIME` / `NIX_V3_MAX_HEAP` / `NIX_V3_MAX_CPU_TIME` plus a
generator depth cap, so `let f = x: f x; in f 0`-shaped exprs get a typed
`WallTimeExceededError` instead of hanging the sweep. A v3-limit-throw vs
TW-no-limit is a KNOWN asymmetry → allowlisted (see §2.5), not a finding.

### 2.2 The oracle — three-way P / O / T (extend the shell idiom)

Reuse the exact engine-invocation idiom from the two parity shell scripts:
- **P** = `NIX_V3_DIRECT_EVAL=1 v3-eval --expr E` (primop path), value = `tail -1`;
- **O** = `NIX_V3_DIRECT_EVAL=1 nix eval --expr E` (OP_* opcode path — the split-catcher);
- **T** = `env -u NIX_V3_DIRECT_EVAL nix eval --expr E` (tree-walker oracle).

Verdict:
- **Success:** stdout byte-identical across P == O == T → PASS.
- **Any engine throws:** all three must throw (class parity, §2.3) and their
  normalized error signatures must match (kind parity, §2.3).
- Diagnostic decomposition of a FAIL:
  - `P ≠ O` → **v3-internal opcode/primop split** (bug even if both differ from T);
  - `(P == O) ≠ T` → **v3-vs-TW divergence**;
  - `throw-vs-succeed` on any pair → **HARD** (the div-SIGFPE / catAttrs class).

### 2.3 Fair error comparison — three tiers (default = class + kind, never raw text)

Naive whole-message compare is wrong (v3's ERROR_UX diverges by design). Verdict
tiers, cheapest+highest-value first:

1. **Class parity (HARD, always).** Did each engine *throw or succeed*? A
   throw↔succeed mismatch is the killer signal (div-SIGFPE, catAttrs-fail-open,
   floor/ceil-clamp were all this). Cheap (exit code), zero false-positives.
2. **Kind parity (normalized signature, SOFT).** Normalize each stderr, then
   compare a canonical signature:
   - strip ANSI (`filterANSIEscapes` analogue), drop leading `error:` / `v3
     error:` prefix;
   - drop source positions `at «string»:L:C` / `at /path:L:C`;
   - drop `…while evaluating…` / `…while calling…` trace frames — keep only the
     **terminal cause line**;
   - replace `/nix/store/<32-hash>-` with `/nix/store/<HASH>-` (store-hash
     placeholder), collapse whitespace, lowercase;
   - extract `(error-noun-phrase, offending primop/type token)` as the signature
     (e.g. `("expected a list but found", "an integer")`, `("division by
     zero",)`, `("attribute missing", "b")`).
   Signature mismatch = SOFT flag → a *lower-severity* fixture (message drift,
   which v3 may intend). This catches real drift (e.g. an opcode path that says
   "empty list" where TW says "expected a list") without drowning in cosmetic
   noise.
3. **Substring anchor (curated, opt-in, STRONG).** For triaged regression
   fixtures only, keep the existing `*"$frag"*` hand-authored fragment assertion
   from the shell scripts. This is the permanent guard once a human confirms the
   expected message.

The fuzzer runs at tier 1+2 by default. Tier 3 fragments are authored only when a
finding graduates into `REPROS.md`.

### 2.4 Minimization (grammar-aware shrink to a small repro)

On a stable divergence (see stability gate below), shrink greedily to a fixpoint,
accepting a candidate reduction iff it **preserves the same divergence
signature** (same P/O/T verdict class + same tier-1/2 signature):
- replace any sub-expression with the simplest same-type leaf (`Int→0`,
  `List→[ ]`, `Attrs→{ }`, `String→""`);
- delete list/attrs elements one at a time (ddmin-style);
- reduce nesting depth; shrink integer magnitudes toward 0 / the nearest boundary
  (`INT64_MIN`, `-1`, `0`, `1`);
- unwrap redundant `let`/`with`/parens.
Shrinking is pure re-evaluation — deterministic, no RNG. Result: a minimal
`.nix` one-liner a human can read, like the curated rows.

**Stability gate (guards against flaky exprs).** Before shrinking OR filing,
re-run the original expr K times (default 5). If the P/O/T verdict is not stable,
**quarantine** it (log, do not shrink, do not file) — a flake must never wedge
the suite or produce a non-reproducing fixture.

### 2.5 Fixture emission (into the test/ convention) + allowlist

Greenfield dir `src/libexpr-v3/test/fuzz-findings/` (confirmed absent today):
- **`FINDINGS.tsv`** — machine-readable ledger: `seed, index, verdict-class,
  tier1/2 signature, original-expr, minimized-expr, P, O, T`. Seed+index stamp
  (never wall-clock).
- **`run-fuzz-found-parity-tests.sh`** — generated in the SAME `@@@`-delimited
  row format the two parity scripts use (`expr@@@expected` for POS,
  `expr@@@fragment` for NEG), so a graduated finding is a drop-in curated row.
- **Quarantine by default.** Auto-emitted findings are informational — wired into
  `all-v3-tests.sh --full` only, behind a FIXED seed, and NOT into `--brute` (the
  merge gate). Rationale: a fresh fuzzer finding is an *un-triaged hypothesis*,
  and Rule 0 forbids letting an untriaged gate block merges.
- **Graduation (human triage).** A confirmed bug → fixed → gets a permanent
  `test/repro-<topic>.nix` + a `REPROS.md` row (the existing bisect→fixture→guard
  workflow) + a tier-3 fragment in the relevant `run-*-parity` script.
- **Known-drift allowlist** (`fuzz-findings/ALLOWLIST.tsv`): signatures a human
  has ruled acceptable (the incomparable-types message drift; v3-only
  resource-limit throws). Allowlisted signatures are never re-filed — prevents
  the fuzzer re-reporting the same known drift forever. Each allowlist entry
  carries a one-line rationale (Rule 4-style retirement note).

### 2.6 Concrete source touchpoints
- **Generator:** new `src/libexpr-v3/test/property/fuzz_parity.py`, importing the
  `gen_*` value generators + `run_eval` + `filter_warnings` from
  `property_tests.py`; adds `gen_expr`, three-way `run_pot()`,
  `normalize_error()`, `shrink()`, `emit_fixture()`, `builtins`-surface
  enumeration/coverage.
- **Runner:** `src/libexpr-v3/test/run-fuzz-parity.sh` (thin wrapper, mirrors
  `run-property-tests.sh`), plus a nightly larger-N mode.
- **3-way engine idiom:** copy the `v3eval`/`nixv3val`/`tweval` +
  `v3err`/`nixv3err`/`twerr` functions verbatim from
  `run-primop-edge-parity-tests.sh`.
- **Wiring:** add a `fuzz-parity|fixed-seed regression sweep|…` suite row to
  `all-v3-tests.sh` under the `--full` block (NOT `--brute`).

---

## 3. Live production parity monitor (nix-eval-jobs)

### 3.1 The gap it closes vs the existing net
`worker.cc processJobRequest()` already has a **TW-retry safety net** — but it
fires **only on an exception** from the v3 section (comment lines 472-491). The
dangerous class is **both v3 and TW succeed, but the drvPaths differ**
(readFile/hashFile-of-derivation): no exception, so today it is **invisible in
production**. The monitor closes exactly this: a sampled, non-fatal
*both-succeed* drvPath comparison.

### 3.2 Hook
In `processJobRequest`, in the v3 success branch — right after
`processDerivationV3(...)` returns a `Response::Job{drv}` — on a sampled subset,
ALSO evaluate the SAME attrPath on the tree-walker and compare. All the machinery
already exists and is reused verbatim:
- TW root: `ensureTwRoot()` (already lazily built for parity-fallback jobs);
- TW descent: `findAlongAttrPath(state, attrPathS, autoArgs, *ensureTwRoot())` +
  `autoCallFunction` (identical to the existing fallback block, lines 526-535);
- TW Drv: `processDerivation(...)` → `Drv::fromPackageInfo`;
- **Comparison:** `Drv` already derives `bool operator==` (drv.hh:98). Primary
  cheap key = `drv.drvPath` (a single hash string — the load-bearing invariant,
  cf. the repo's "drvPath parity proof"); deep key = full `operator==`
  (name/system/outputs/inputDrvs/requiredSystemFeatures/cacheStatus/meta), since
  the store-side fields derive from drvPath + store.

### 3.3 Sampling and overhead budget
Running TW for every job **doubles** eval cost — unacceptable on the hot fleet
(v3 exists to be the fast path; the deploy is AOT-warm, plan-IFD-dominated). So:
- Gate `NIX_V3_PARITY_SHADOW=<rate>` parsed via the existing `envFlagEnabled`/
  value idiom (so `=0` genuinely disables — same footgun-proofing as
  `NIX_V3_DIRECT_EVAL`), with an inline **retirement criterion** at the first
  `getenv` (Rule 4). Default **off** in prod.
- **Two-key strategy to fit budget:** the **primary** sampled check compares only
  `drv.drvPath` — but note that still requires a TW eval of the job to obtain the
  TW drvPath, so the cost floor is `rate × (TW single-job eval)`. The **deep**
  `operator==` check runs on a smaller sub-sample of the already-sampled jobs (no
  extra eval — the TW Drv is already built).
- **Pre-committed budget:** monitored workers only; `rate ∈ [0.01, 0.05]`;
  aggregate added eval-wall on a monitored worker **≤ 5%** at `rate = 0.05`
  (since a single TW job eval ≈ one v3 job eval, `5%` sample ≈ `5%` wall). The
  hot fleet runs `rate = 0` (monitor disabled); parity coverage comes from a
  dedicated **canary worker/jobset** or a small worker fraction.
- CI shadow mode: under a `NIX_V3_REQUIRE`-style flag, a divergence **escalates to
  a hard fail** (mirrors the existing `v3Require` rethrow) — "prove parity," not
  "observe parity."

### 3.4 Divergence reporting — never fail the eval
- Emit ONE structured, greppable, **non-fatal** stderr sentinel (the collector
  already reads worker stderr for the Hydra UI — `nix-eval-jobs.cc:416` handles
  `Response::Error{fatal}`, and non-error stderr is surfaced):
  `nix-eval-jobs: PARITY-DIVERGENCE attr=<attrPath> key=drvPath v3=<…> tw=<…>`.
- The eval still returns a result. Policy knob: default **emit-and-keep-v3**;
  optional **prefer-oracle-on-divergence** (return the TW Drv when they differ) —
  this is strictly safer for Hydra and reuses the exact "TW is the correctness
  oracle" framing already in the retry-net comment.
- Optionally append a JSONL record (seed/attr-stamped, not wall-clock) under a
  configured dir so a downstream job auto-files fuzz-findings fixtures from real
  workloads — the production monitor *feeds the offline fuzzer's corpus*.
- No new eval infrastructure, no second live heap beyond the TW root the worker
  can already build; the added surface is one sampled comparison call.

### 3.5 Concrete source touchpoints
- `nix-eval-jobs/src/worker.cc`: the v3 success branch inside
  `processJobRequest` (after `processDerivationV3`); reuse `ensureTwRoot`,
  `findAlongAttrPath`, `processDerivation`, `Drv::operator==`; new gate via
  `envFlagEnabled("NIX_V3_PARITY_SHADOW")`.
- `nix-eval-jobs/src/drv.hh`: the `Drv::operator==` and `drvPath` field are the
  comparison surface (no change needed).

---

## 4. Phased plan (measure yield BEFORE building the framework)

- **Phase 0 — spike, ~1 day, NO framework (Rule-0 gate).** Extend the existing
  3-way harness minimally: a ~150-line `fuzz_parity.py` with the recursive
  `gen_expr` + the P/O/T oracle at **tier-1 (class) + byte-value** only. No
  minimization, no fixture emission, no error normalization. Run a fixed-seed
  sweep of N = 1,000 then 10,000 exprs; **measure the divergence yield**
  (de-duplicated by verdict + a coarse expr shape). Also report `builtins`
  surface-coverage %. This is the falsifier for "arbitrary-input fuzzing finds
  what the curated net misses."
- **Phase 1 — full offline fuzzer (only if Phase 0 clears its gate).** Add
  tier-2 error normalization, grammar-aware minimization + stability gate,
  quarantine fixture emission (`fuzz-findings/`), coverage-driven bias, and the
  surface-diff check. Wire the fixed-seed subset into `all-v3-tests.sh --full`;
  add a nightly rotating-seed larger sweep.
- **Phase 2 — production monitor (only if Phase 1 surfaces real bugs, OR
  independently to catch store-backed drvPath drift).** Implement the sampled
  drvPath shadow in `worker.cc` behind `NIX_V3_PARITY_SHADOW`, canary-only, with
  the §3.3 budget; deep `operator==` sub-sample; JSONL corpus feed into
  `fuzz-findings/`.
- **Phase 3 — property invariants (LESSONS §4.9 #7).** Fold in VM-invariant
  properties the generator can assert without an oracle: force idempotence
  (`E == builtins.seq E E`), P==O internal consistency as a standalone assert,
  cycle-detection totality (every self-referential shape terminates with a typed
  error on both engines).

---

## 5. Pre-committed KILL criteria (Rule 0)

- **Phase-0 KILL.** If a fixed-seed **10,000-expr** type-directed sweep surfaces
  **zero** divergences that are not already covered by the curated suites (after
  de-dup by signature), AND generator `builtins`-coverage is **≥ 60%** of the
  live surface, then "arbitrary-input fuzzing finds dual-path/obscure-primop bugs
  the curated net misses" is **FALSIFIED for this grammar** → stop; do NOT build
  minimization/monitor. One escape: broaden the grammar **once** (add
  context-strings + realise/IFD shapes) and re-measure; a second zero-yield is a
  clean kill (delete the spike, note the falsification).
- **Monitor KILL.** If the sampled shadow finds **zero drvPath divergences** over
  a pre-committed real-jobset volume (one full cardano-node/nixpkgs eval,
  ~N jobs) at the budgeted rate, "silent both-succeed drvPath divergence occurs
  on real workloads" is unsupported at that rate → raise the rate **once**
  (budget permitting) or retire the monitor to on-demand CI shadow only.
- **Allowlist discipline.** Any finding a human triages as known-acceptable
  (message drift, v3-only resource-limit throws) goes to `ALLOWLIST.tsv` with a
  rationale and is never re-filed. A commit that adds the fuzzer must state which
  hypothesis it kills or confirms (Rule 0); a `*_findings.md` with no code change
  does not merge.

---

## 6. Risks and mitigations

1. **Flaky / non-deterministic exprs.** Impurity blocklist (§2.1); `--pure-eval`;
   pin `<nixpkgs>` via `test/nixpkgs-pin.sh` if ever in scope; the K-run
   stability gate quarantines anything non-reproducing before shrink/file.
2. **Error-format false-positives (v3's intentional ERROR_UX).** Default to
   tier-1 (class) + tier-2 (normalized kind), NEVER raw message; known-drift
   allowlist; tier-3 exact fragments only for human-triaged guards. Grounded in
   the arith script's own documented incomparable-types drift.
3. **Monitor perf overhead.** Sampling + drvPath-only primary key + canary-only +
   hard ≤5% budget + `NIX_V3_PARITY_SHADOW=0` kill-switch reusing `envFlagEnabled`
   (so `=0` truly disables — the RR1-F6 footgun already fixed for
   `NIX_V3_DIRECT_EVAL`).
4. **Fuzzer wedging the merge gate.** Quarantine dir; `--full` fixed-seed subset
   only; never `--brute`. A finding gates nothing until a human graduates it.
5. **Low-signal generation (parse-fails / trivial throws).** Type-direction makes
   exprs well-formed by construction; track a "useful-yield" metric (fraction
   reaching a value or a *typed* eval-error, not a parse error) as a
   generator-quality gate; coverage % ensures breadth over the 215 surface.
6. **Resource blowup / non-termination.** Generator depth cap + mandatory
   `NIX_V3_MAX_WALL_TIME`/`MAX_HEAP`/`MAX_CPU_TIME` on every probe (CLAUDE.md
   rule); v3-limit-throw vs TW-no-limit is an allowlisted known asymmetry, not a
   finding.
