# IR-CHECK infrastructure — implementation plan

**Status**: CANDIDATE plan, breakdown for review. Recommends an MVP path (5 days) before committing to full plan (10-12 days). NO CODE in this doc.

This doc breaks down the 5-step plan from `LODE_REVIEW_2026-05-18.md` §6 into implementation-ready sub-tasks, identifies design decisions per step, surfaces risks, and proposes an adjusted timeline. The plan leverages the existing `checkIr` library in `ir_dump.cc` (lines 420-510) which already implements LLVM-FileCheck-compatible directive matching.

## 0. Goal restated

End-to-end flow: `input.nix → lower → optimize (selected passes) → dump IR → assert against CHECK directives in sidecar .expected`.

Today: only the dump and the directive-checker exist; the driver, per-pass selection, runner, fixtures, and convention are missing.

## Step 1 — Driver: `v3-eval --emit-ir`

**Original estimate**: 3 days. **Revised**: 2-3 days, but with a critical sub-task (determinism audit) that may add 1-2 days.

### 1.1 Decisions

**D1.1**: Separate binary or extend `v3-eval`?
- LLVM: `opt`, `llc`, `clang` are separate.
- GHC: `-ddump-stg`, `-ddump-simpl` flags on the main compiler.
- v3: extend `v3-eval`. Lowest cost, no duplication. Later a `v3-opt` binary can be added if needed.

**D1.2**: What gets dumped?
- Stage 1 ship: raw lowering (pre-opt) AND post-opt (after the full pipeline). Two flags: `--emit-ir-raw` and `--emit-ir` (post-opt). Default `--emit-ir` is post-opt.
- Stage 2 ship (step 2): per-pass entry points (`--opt-passes=A,B`).

**D1.3**: Output destination?
- stdout by default
- `-o FILE` for explicit file output

**D1.4**: Output format?
- Use the existing `ir_dump.cc` format (canonical for `checkIr`'s expected text). Don't invent a new format.

### 1.2 Sub-tasks

| # | Sub-task | Time | Risk |
|---|---|---|---|
| 1.1 | Add `--emit-ir` and `--emit-ir-raw` flags to `v3-eval` CLI argument parser | 0.5 day | Low |
| 1.2 | Wire the lowering pipeline so flags emit IR at the right point | 0.5 day | Low |
| 1.3 | **Determinism audit of `ir_dump.cc` output** (see §1.3) | 0.5-2 days | **HIGH** |
| 1.4 | Stable VarId assignment ordering (deterministic per source position) | 0.25 day | Medium |
| 1.5 | Symbol table iteration ordering audit (intern order, hash map keys) | 0.25 day | Medium |
| 1.6 | Block ID assignment audit | 0.25 day | Low |
| 1.7 | Smoke test: same input twice → byte-identical output | 0.25 day | Low |
| 1.8 | Document the new flag in USAGE.md | 0.25 day | Low |

### 1.3 The determinism subtask is on the critical path

**This is the single biggest risk for Step 1.** The `checkIr` library was built for smoke.cc, which uses hand-built IR (controlled). Real lowered IR may have non-deterministic ordering in:
- Symbol table iteration (if it uses `std::unordered_map` or `folly::F14`)
- Block bindings vector (should be insertion-ordered, but verify)
- Lambda freeVars (set iteration; needs sort)
- Block successor edges from terminals (if there's branching)
- Module's functions list (should be insertion-ordered)

**If the dump isn't deterministic**: every fixture is flaky. Tests pass once, fail next run. Wastes huge amounts of debugging time.

**Mitigation strategy**:
1. Dump the same module 10 times; diff. Expect zero output.
2. Dump under `--shuffle-allocations` (if it exists; if not, add it for testing) to force order variations.
3. For any nondeterministic source, sort before dumping (small CPU cost; large stability gain).
4. Lock down with a unit test: "dump determinism over 100 iterations."

### 1.4 What could go wrong

- **(R1.1)** Dump is not byte-deterministic across runs. Mitigation: §1.3.
- **(R1.2)** `--emit-ir` succeeds for trivial expressions but fails on complex ones (e.g., recursive let, deep lambdas). Mitigation: test corpus covers shape diversity.
- **(R1.3)** Source positions leak into output unpredictably (different machines, different paths). Mitigation: strip or canonicalize file paths in dump (`--ir-canonical-positions`).
- **(R1.4)** Error during lowering produces partial output that confuses `checkIr`. Mitigation: on error, emit a marker line `; LOWERING ERROR: <msg>` and exit non-zero; runner can detect this.

### 1.5 Revised estimate for Step 1: **2-3 days, with 1-2 days padding for determinism**

Total: **3-5 days** in the worst case. Originally 3.

## Step 2 — Per-pass entry points + sidecar `.expected` convention

**Original estimate**: 2 days. **Revised**: 2-3 days.

### 2.1 Decisions

**D2.1**: Pass identification — names or letters?
- Both. Internal name (`betaReduce`) is canonical; short alias (`beta` or `A`) for CLI ergonomics.

**D2.2**: Pipeline order vs request order?
- Default: pipeline order, regardless of request order in CLI.
- Override: `--opt-passes-in-order=A,B` (rare; explicit opt-in).
- Rationale: passes have order dependencies; default to safety.

**D2.3**: Default pass set?
- `--opt-passes=all`: run all passes in pipeline order.
- `--opt-passes=` (empty): equivalent to `--no-opt`.
- `--opt-passes=A,B`: run only A and B, in pipeline order.
- `--opt-passes=!CSE`: run all except CSE.

**D2.4**: Fix-point iteration control?
- `--opt-fixpoint`: enable (default once Phase C+ lands).
- `--opt-no-fixpoint`: disable (for testing single-pass behavior).
- `--opt-max-iterations=N`: safety cap.

**D2.5**: Where do CHECK directives live? — **RESOLVED 2026-05-18 by user**: in the `.nix` source file itself, as Nix line-comments with `#` prefix. Matches LLVM's `.ll`-file pattern exactly. No sidecar.

**D2.6**: In-file format? — **REVISED 2026-05-18 to match LLVM's actual model** (RUN: is a shell command, not a directive name).

LLVM's actual pattern:
```
; RUN: opt -S -O3 %s | FileCheck %s

define i32 @foo(i32 %x) { ret i32 %x }

; CHECK-LABEL: define i32 @foo
; CHECK: ret i32 %x
```

The `; RUN:` line is **a shell command** that the test driver (`lit`) executes. It typically:
1. Runs the compiler/optimizer on the source file (`%s`).
2. Pipes the output to `FileCheck %s`.
3. FileCheck reads `; CHECK:` directives from `%s` (the same file) and compares against stdin.

`%s` is `lit`'s substitution token for the test file path. Other common substitutions: `%t` (temp file), `%S` (test directory), `%T` (temp directory).

**For v3**, the same pattern using Nix `#`-comments:

```nix
# RUN: v3-eval --emit-ir --opt-passes=betaReduce,constantFold %s | FileCheck %s
# Tests beta reduction + constant folding composition.

(x: x + 1) 5

# CHECK: ; module n_funcs=1
# CHECK-NOT: Lambda
# CHECK-LABEL: B1:
# CHECK: v1 = LitInt 6
# CHECK: return v1
```

The `.nix` parser sees only `(x: x + 1) 5` (the rest are `#` comments). The runner extracts `# RUN:` lines, substitutes `%s`, and executes them as shell commands. FileCheck (a real LLVM binary) reads `# CHECK:` directives from the same file and compares against stdin. **Identical mental model to LLVM**; zero new conventions to learn for developers transferring from LLVM/Rust/Clang projects.

Multiple `# RUN:` lines per fixture are allowed — useful for testing the same source under different pass combinations or comparing pre- vs post-opt:

```nix
# RUN: v3-eval --emit-ir %s | FileCheck %s --check-prefix=OPT
# RUN: v3-eval --emit-ir --no-opt %s | FileCheck %s --check-prefix=RAW

(x: x + 1) 5

# RAW: Lambda
# RAW: App
# OPT-NOT: Lambda
# OPT: LitInt 6
```

**D2.7**: Check tool — **REVISED 2026-05-18 (user pushback ×3)**: ship our own ~300-line `v3-check` binary; do NOT pull in the LLVM toolchain.

**Sizing comparison** (verified 2026-05-18):

| What | Lines | Dep weight |
|---|---|---|
| v3's existing `checkIr` + `parseChecks` (`ir_dump.cc`) | **86** | zero (in-tree) |
| LLVM `FileCheck.cpp` driver | 1 037 | LLVM toolchain (~GB) |
| LLVM `lib/FileCheck/FileCheck.cpp` engine | 2 771 | same |
| **Subset we actually need** | **~250-350 estimated** | zero |

Pulling in `pkgs.llvm` (or `pkgs.llvmPackages_*.llvm`) drags in the full LLVM toolchain — many MB to GB. Disproportionate for one binary. **Build our own instead.**

**Design**: `v3-check FILE` — small CLI binary; reads stdin as actual IR, `FILE` as the match-file (the `.nix` fixture), invokes the existing `checkIr` library with extensions.

**Extensions needed beyond today's 86 lines**:
- `CHECK-LABEL:` — strong anchor; resets cursor position. ~30 lines.
- `CHECK-NEXT:` — adjacent-line constraint. ~30 lines.
- `{{regex}}` patterns — basic embedded regex via `std::regex`. ~50 lines.
- `#` prefix support (in addition to `;`). ~1 line (already mentioned).
- CLI wrapper (argv parsing, file I/O, exit code). ~50 lines.
- **Total new code**: ~150-200 lines on top of existing 86 = ~250-300 lines.

**Deferred until needed** (each maybe +50-100 lines):
- `CHECK-DAG:` (order-independent)
- `CHECK-COUNT-N:` (N occurrences)
- `[[var]]` variable capture
- `CHECK-EMPTY:`, `CHECK-SAME:`
- Multi-prefix (`--check-prefix=FOO`) — useful for multi-RUN fixtures with different prefixes; add when first multi-RUN fixture needs it.

**LLVM-syntax compatibility**: maintain exactly LLVM's `CHECK:`, `CHECK-NOT:`, `CHECK-LABEL:`, etc. so a developer who has real LLVM `FileCheck` installed can substitute it in for our fixtures with no changes. Developers transferring from LLVM/Rust/Clang have immediate fluency; we just don't *require* the full LLVM toolchain.

**Naming**: `v3-check` (honest: it's a subset, not the real thing). Fixture RUN lines reference it: `# RUN: v3-eval --emit-ir %s | v3-check %s`.

**For `smoke.cc` in-process tests**: the existing `checkIr` library stays. The new features (CHECK-LABEL, CHECK-NEXT, regex) extend the library too, so smoke.cc benefits without separate work.

**What about `# RUN:` accidentally matching as a CHECK?** Implement LLVM's behavior: if `RUN:` or `COM:` appears on a line, ignore any CHECK-prefix on that line. ~5 lines in the parser. Prevents `# RUN: ... | v3-check %s` from being seen as a match candidate.

### 2.2 Sub-tasks

| # | Sub-task | Time | Risk |
|---|---|---|---|
| 2.1 | Refactor `optimise(Module&)` to drive a pass list (`std::vector<Pass>`) | 0.5-1 day | Medium |
| 2.2 | Per-pass enablement: `NIX_V3_OPT_PASSES=...` env var + `--opt-passes` flag | 0.5 day | Low |
| 2.3 | Pass-name table (full name + alias + Phase letter) | 0.25 day | Low |
| 2.4 | Document sidecar `.expected` format in IR_CHECK_INFRASTRUCTURE_PLAN.md (this doc) | 0.25 day | Low |
| 2.5 | Extend `checkIr` with `CHECK-LABEL:` and basic `{{regex}}` patterns | 0.5 day | Medium |
| 2.6 | `--opt-fixpoint` flag and behavior | 0.5 day | Low (no fix-point passes yet) |
| 2.7 | Smoke test: per-pass flag behaves correctly | 0.5 day | Low |

### 2.3 The optimise() refactor (sub-task 2.1)

Today:
```cpp
void optimise(Module & m) {
    if (disabled) return;
    constantFold(m);
    betaReduce(m);
    // ...
}
```

Refactor to:
```cpp
struct Pass {
    const char * name;
    const char * alias;       // short name; nullable
    void (*fn)(Module &);
    bool defaultEnabled;
};
static const Pass kPipeline[] = {
    { "constantFold", "cf", constantFold, true },
    { "betaReduce", "beta", betaReduce, true },
    { "constantFold-2", "cf2", constantFold, true },  // re-run
    // ...
};

void optimise(Module & m, PassSet enabled) {
    for (const Pass & p : kPipeline) {
        if (enabled.contains(p.name)) p.fn(m);
    }
}
```

**Risk**: refactor introduces a behavior change. Mitigation: run lang tests + cutover-parity + property tests before AND after; expect zero behavior change.

### 2.4 What could go wrong

- **(R2.1)** Refactor changes behavior subtly. Mitigation: regression suite green before merging.
- **(R2.2)** `CHECK-LABEL` / `{{regex}}` extension to checkIr introduces parsing bugs. Mitigation: unit tests for the directive parser.
- **(R2.3)** Sidecar format bikeshed eats time. Mitigation: decide D2.5/D2.6 now (in this doc); document; move on.
- **(R2.4)** Pass dependencies make per-pass runs produce surprising output. E.g., requesting Phase C alone (without Phase A) emits unfused IR because A didn't surface the patterns C looks for. Mitigation: document dependencies; CLI warns "Phase C without Phase A is unusual."

### 2.5 Revised estimate for Step 2: **2-3 days**

Total: **2-3 days**. Originally 2.

## Step 3 — `test/run-ir-checks.sh` runner

**Original estimate**: 1 day. **Revised**: 1.5-2 days.

### 3.1 Decisions

**D3.1**: Runner language?
- Bash (consistent with existing `test/run-*.sh`).

**D3.2**: How does the runner work? — **REVISED 2026-05-18 to match `lit` model**.

The runner is `lit`-like: a minimal shell-or-Python tool that walks fixtures, extracts `# RUN:` lines, performs substitutions (`%s`, `%t`), and executes the shell commands. Test passes iff all RUN commands return exit code 0.

Pseudocode (`test/run-ir-checks.sh`):
```sh
for fixture in test/ir-fixtures/*.nix; do
    ok=true
    while read -r runline; do
        # Substitute %s -> fixture path, %t -> per-test temp file.
        cmd=$(echo "$runline" | sed "s|%s|$fixture|g; s|%t|/tmp/$(basename $fixture).t|g")
        if ! eval "$cmd"; then
            echo "FAIL: $fixture: $cmd"
            ok=false
            break
        fi
    done < <(grep -E '^# RUN:' "$fixture" | sed 's/^# RUN: //')
    $ok && echo "PASS: $fixture"
done
```

Real implementation: ~100 lines with reporting, summary, exit code aggregation. Possible upgrade later: use LLVM's actual `lit` (Python). For MVP, the bash runner is sufficient.

**No `v3-eval --ir-check` mode needed.** The runner executes the RUN: shell command which is typically `v3-eval --emit-ir ... | FileCheck %s`. v3-eval only needs the `--emit-ir` flag (and pass-control flags).

**D3.3**: Test discovery? — **REVISED**.
- Walk `test/ir-fixtures/` for `*.nix`. No sidecar required.
- A `.nix` without any `# RUN:` line is skipped (treat as documentation example or non-test file).
- A `.nix` with malformed `# RUN:` (e.g. unresolvable substitution) is reported as a fixture error, not silently skipped.

**D3.4**: Dependencies? — **REVISED 2026-05-18**.
- **No new external dependencies.** `v3-check` is in-tree (~250-300 lines new code; see D2.7).
- Existing toolchain only: C++ compiler, std::regex (already used in v3).
- Bash + grep + sed for the test runner (standard POSIX).
- Avoids LLVM toolchain entirely.

**D3.4**: Reporting?
- Per-fixture: PASS / FAIL with clear summary.
- On FAIL: show the `checkIr` diagnostic (context lines, pattern, etc.).
- Aggregate: `[N/M] fixtures passed`.

**D3.5**: Output and exit code?
- Exit 0 if all pass; non-zero if any fail.
- Verbose mode prints PASS lines too; default prints only failures.

**D3.6**: Meson integration?
- Add as a meson test target: `v3-ir-checks`.
- Runs under `ninja test`.

### 3.2 Sub-tasks

| # | Sub-task | Time | Risk |
|---|---|---|---|
| 3.1 | Extend `checkIr` in `ir_dump.cc`: add `#` prefix support, `CHECK-LABEL:`, `CHECK-NEXT:`, `{{regex}}`, `RUN:`/`COM:` line-skip | 1 day | Low (in-tree work; existing structure) |
| 3.2 | Add `v3-check` binary: tiny CLI around extended `checkIr` (reads stdin actual, takes match-file argv, exits 0/1) | 0.25 day | Low |
| 3.3 | Write `test/run-ir-checks.sh` runner: walk fixtures, extract `# RUN:` lines, substitute `%s`/`%t`, execute as shell commands, aggregate exit codes | 0.5 day | Low |
| 3.4 | Substitution support: `%s`, `%t` minimum; document additional substitutions | 0.25 day | Low |
| 3.5 | Reporting + summary (PASS/FAIL per fixture, aggregate count) | 0.25 day | Low |
| 3.6 | Meson integration; GitHub Actions hook | 0.25 day | Low |
| 3.7 | Smoke test with 2 hand-written fixtures + 1 multi-RUN fixture | 0.25 day | Low |

### 3.3 GitHub Actions + Nix CI integration (user answer to Q6, 2026-05-18)

CI target: GitHub Actions + Nix where applicable. Standard pattern from CLAUDE.md global instructions: `nix develop -c make ...`. The hook:

- `.github/workflows/v3-ir-checks.yml` runs on every PR (and main pushes).
- Job: `nix develop -c bash src/libexpr-v3/test/run-ir-checks.sh`.
- Failure annotates the PR with the failing fixture path + checkIr diagnostic.
- Fast: each fixture is <100ms; 30 fixtures = <5s of test time + meson overhead.

Integration with existing GH Actions: add to the existing v3 test workflow (presuming one exists) as a new test step, or as a separate workflow file. Run alongside lang tests, property tests, etc.

### 3.4 Single-invocation simplifies pipeline-composition concerns

The previous version of this plan worried about `--emit-ir | --check-ir` pipeline error handling (`pipefail` etc.). With the revised D3.2 (single `--ir-check FILE` invocation), the pipeline composition is internal to `v3-eval`. No shell-pipe error handling needed; the binary either returns 0 or non-zero with diagnostic.

### 3.4 What could go wrong

- **(R3.1)** Subtle interactions between `--emit-ir` and `--check-ir` invocations (e.g., shared option parsing state). Mitigation: integration smoke test.
- **(R3.2)** Bash portability (macOS vs Linux `bash` differences). Mitigation: use POSIX shell where possible; CI runs both.
- **(R3.3)** Meson `test()` integration has gotchas around relative paths. Mitigation: use `meson.current_source_dir()` consistently.

### 3.5 Revised estimate for Step 3: **1.5-2 days**

Original 1 day was too aggressive. Pipeline-composition + reporting + meson integration is 1.5-2 days realistically.

## Step 4 — Initial 10 fixtures for already-landed passes

**Original estimate**: 3 days. **Revised**: 4-5 days.

### 4.1 Decisions

**D4.1**: Which 10 passes to cover?
- Currently landed: `constantFold`, `betaReduce`, `commonSubexprElim`, `elimRedundantForce`, `inlineTrivialBindings`, `fusePrimOpApps`, `deadBindingElim`, `opt_occur` (gated), `opt_strictness`.
- One positive + one negative fixture each = ~18 fixtures.
- Compromise for the "initial 10": one positive per pass (8-9 fixtures) + one composition fixture (1) = 10.

**D4.2**: Positive fixture design — what does a CHECK actually assert?
- Specific output shape (e.g., `; CHECK: LitInt 6` after const-fold).
- Specific absence (e.g., `; CHECK-NOT: Lambda` after beta-reduce).
- Specific binding count (later, with `CHECK-COUNT`).

**D4.3**: Negative fixture design?
- Cases where the pass SHOULD NOT fire (e.g., `(throw "boom") 5` should not beta-reduce; the lambda body is impure).
- Catches false-positive optimizations.

**D4.4**: Naming convention?
- `test/ir-fixtures/{pass}-{feature}-{pos|neg}.nix`
- E.g., `test/ir-fixtures/beta-reduce-basic-pos.nix`.

**D4.5**: What about composition fixtures?
- Test: do passes in sequence interact correctly?
- E.g., `betaReduce + constantFold`: `(x: x + 1) 5` should collapse to `LitInt 6` (beta inlines, then constantFold collapses).
- One per phase-pair plausibly relevant.

### 4.2 The 10 fixtures (proposed)

| # | Fixture | Pass | Type |
|---|---|---|---|
| 1 | `constantFold-arith-pos.nix` | constantFold | pos: `1+2 → 3` |
| 2 | `constantFold-string-concat-pos.nix` | constantFold | pos: `"a"+"b" → "ab"` |
| 3 | `betaReduce-basic-pos.nix` | betaReduce | pos: `(x: x+1) 5 → 5+1` |
| 4 | `betaReduce-impure-neg.nix` | betaReduce | neg: `(x: throw "x") 5` NOT inlined |
| 5 | `CSE-shared-expr-pos.nix` | CSE | pos: `let a = e; b = e; in a+b` → single binding |
| 6 | `elimRedundantForce-chain-pos.nix` | elimRedundantForce | pos: `Force(Force(x))` → `Force(x)` |
| 7 | `inlineTrivialBindings-alias-pos.nix` | inlineTrivialBindings | pos: `let a = b; in a+1` → `b+1` |
| 8 | `fusePrimOpApps-multi-arg-pos.nix` | fusePrimOpApps | pos: `App(App(LitPrimOp(add), a), b)` → `PrimOpCall(add, [a,b])` |
| 9 | `deadBindingElim-unused-pos.nix` | deadBindingElim | pos: `let unused = e; in 0` → `0` (binding removed) |
| 10 | `composition-beta-then-fold.nix` | betaReduce + constantFold | composition: `(x: x+1) 5 → 6` |

### 4.3 Sub-tasks

| # | Sub-task | Time | Risk |
|---|---|---|---|
| 4.1 | Write 10 .nix fixtures (15-30 min each) | 0.5 day | Low |
| 4.2 | Write 10 .expected sidecars with CHECK directives | 0.5 day | **MEDIUM (writing the right directives is iterative)** |
| 4.3 | Iterate: run runner; fix CHECK pattern mismatches with actual output | **1-2 days** | **MEDIUM** |
| 4.4 | Document fixture-authoring guide in test/ir-fixtures/README.md | 0.25 day | Low |
| 4.5 | Discover and fix any IR-dump determinism bugs surfaced by fixtures | 0.5-1 day | **HIGH** |
| 4.6 | Cross-validate: each fixture's eval result via `v3-eval` matches expected behavior | 0.25 day | Low |

### 4.4 The iteration loop (sub-task 4.3) is the real cost

Writing fixtures is fast (15-30 min each). Getting the CHECK directives RIGHT is iterative:
- Run runner; see actual output; adjust CHECK directives to match.
- Some CHECK directives need refinement (too specific → brittle; too loose → no value).
- Some fixtures reveal unexpected IR shapes (passes do more or less than expected).
- Some fixtures reveal bugs in passes themselves (this is GOOD — that's why we're building the infrastructure).

Realistic: 30-45 min per fixture for the first pass; 1-2 hours per fixture if surprises come up. 10 fixtures × ~1 hour mean = 10 hours = 1.5 days. Plus 0.5-1 day for the determinism surprises.

### 4.5 What could go wrong

- **(R4.1)** Determinism issues surface only with real fixtures (not smoke.cc). High probability. Mitigation: §1.3 audit MUST happen first.
- **(R4.2)** Fixtures reveal bugs in existing passes. Probability medium-high. This is GOOD — it's what we want. But it ADDS to the timeline (bug investigation + fix).
- **(R4.3)** Fixture CHECK directives are too brittle (break on benign changes). Probability medium over time. Mitigation: balance specificity vs robustness. Use `CHECK-LABEL` to anchor; use wildcards for stable-but-irrelevant content.
- **(R4.4)** Writing the right CHECK is hard for complex IR. Probability medium. Mitigation: convention — "test the SPECIFIC thing the pass does, not the entire output."
- **(R4.5)** Negative fixtures (CHECK-NOT) miss subtle ways the pass fires. Probability low.

### 4.6 Revised estimate for Step 4: **4-5 days**

Original 3 days was optimistic. Iteration loop + determinism surprises pushes it to 4-5 days realistically.

## Step 5 — Continuous fixtures alongside IR Phases B-H

**Original estimate**: continuous. **Refinement**: ~30% overhead per phase.

### 5.1 The continuous-fixture rule

**Proposal**: enforce as a Rule-0-equivalent — *every IR pass land must include at least one positive + one negative fixture*. Reviewer checks that the fixture's CHECK directives correspond to the pass's stated exit criterion.

This makes the per-pass exit criteria (from IR_OPTIMIZATION_PLAN §2.5) verifiable in CI:

| Phase | IR_OPTIMIZATION_PLAN exit criterion | Fixture form |
|---|---|---|
| B (primop fold) | `length [1 2 3 4 5]` lowers to LIT_INT 5; no OP_CALL_PRIMOP | Positive: assert `LitInt 5`; Negative: assert no `PrimOpCall length` |
| C (stream fusion) | `attrset-build-1k ≤30% over TW` | Positive: fused form emitted; Negative: no intermediate-list allocation pattern |
| D (lambda lift) | smaller alloc counts on benchmarks | Positive: capture-free Lambda → singleton ref |
| E (selector recog) | fewer "generic OP_CALL" dispatches | Positive: `x: x.foo` lambda has `selectorSym = foo` set |
| F (App spine fold) | PartialApp counters drop | Positive: fully-static spine fully folded |
| G (If fold) | fewer OP_BRANCH_FALSE in compiled bytecode | Positive: `if true then a else b` → `a` |
| H (genList unroll) | `genList id 4` → 4-element ListExpr | Positive: ListExpr with 4 LitInt elements |

### 5.2 Estimated overhead per phase

| Phase | Implementation time (per IR plan) | Fixture time (proposed) | Total |
|---|---|---|---|
| B | 2-3 days | +1 day | 3-4 days |
| C | 4-5 days | +1-2 days | 5-7 days |
| D | 1-2 days | +0.5 day | 1.5-2.5 days |
| E | 0.5 day | +0.5 day | 1 day |
| F | 1-2 days | +0.5 day | 1.5-2.5 days |
| G | 0.5 day | +0.25 day | 0.75 day |
| H | 1 day | +0.5 day | 1.5 days |

Sum of fixture time: ~4-5 days. Distributed across the IR Phase B-H work; not concentrated.

### 5.3 Update tooling (`update_test_checks.py` equivalent)

When a pass's CORRECT behavior changes (e.g., a new optimization fires earlier), existing fixtures' CHECK directives need updating. LLVM has `utils/update_test_checks.py` to regenerate `; CHECK:` lines from actual output.

For v3: build a similar Python or shell script `test/update-ir-checks.sh`. Takes a fixture; runs `--emit-ir`; replaces the CHECK directives in the .expected file. Reviewer verifies the regenerated CHECK is "morally right" (what the pass should produce).

Estimated effort: 1 day. Land when first fixture-regeneration is needed.

### 5.4 What could go wrong

- **(R5.1)** Phase-land PRs ship without fixtures because "we're in a hurry." Mitigation: PR template + reviewer checklist.
- **(R5.2)** Fixtures fall out of sync with pass behavior (e.g., a pass becomes more aggressive; old fixtures still pass but no longer cover the new behavior). Mitigation: periodic fixture audit; update tool helps.
- **(R5.3)** Fixture count balloons (50+ per pass); CI runtime suffers. Mitigation: monitor; set a per-pass fixture-count budget (e.g., max 10 per pass; consolidate redundant ones).

## 7. Total revised timeline

| Step | Original | Revised |
|---|---|---|
| 1 — Driver | 3 days | 3-5 days (with determinism) |
| 2 — Per-pass + sidecar | 2 days | 2-3 days |
| 3 — Runner | 1 day | 1.5-2 days |
| 4 — 10 fixtures | 3 days | 4-5 days |
| **Subtotal (1-4)** | **9 days** | **10-15 days (~2-3 weeks)** |
| 5 — Continuous | continuous, +30% per pass | ~4-5 days distributed across Phases B-H |

**The original 9-day estimate was optimistic by ~30-50%.** Realistic: **2-3 weeks of focused work for steps 1-4**, plus ~4-5 days distributed across Phases B-H for step 5.

## 8. MVP path — 5 days to validate value (revised 2026-05-18 after user answers)

Before committing to the full 2-3 weeks, the team could ship a much smaller MVP that proves the flow works end-to-end, then expand.

| Day | MVP sub-task |
|---|---|
| 1 | **Determinism audit + fix** (user-confirmed part of MVP scope). Includes any sorting, hash-map iteration removal, symbol-table audit |
| 2 | Add `v3-eval --emit-ir` flag (post-opt only; no per-pass selection yet) |
| 3 | Extend `checkIr` in `ir_dump.cc` (~150-200 new lines): `#` prefix, `CHECK-LABEL:`, `CHECK-NEXT:`, `{{regex}}`, `RUN:`/`COM:` line-skip. Add `v3-check` CLI binary (~50 lines) |
| 4 | Write `test/run-ir-checks.sh` runner (~100 lines bash); create `test/ir-fixtures/`; write 3 fixtures (positive, negative, multi-RUN composition) using LLVM-style `# RUN: ... | v3-check %s` |
| 5 | Meson-integrate; GitHub Actions hook; document fixture-authoring guide; produce sample-failure output for the team to see what a CHECK miss looks like |

**End of MVP**: the team can author new fixtures and run them; the infrastructure is real, LLVM-syntax-compatible, in-tree (zero new external deps). Developers familiar with LLVM-style testing have immediate fluency. Total new code: ~450-550 lines across `ir_dump.cc` extensions + `v3-check.cc` + `run-ir-checks.sh`.

If value is clear, proceed to the per-pass entry points (step 2 from the original plan) and 10-fixture corpus (step 4). If value is unclear, the MVP is still useful for ad-hoc IR inspection.

**MVP cost**: 5 days (Q5 confirmed: determinism inside MVP scope). **Decision point**: at end of MVP, do we continue?

**Canonical RUN: line convention** (added 2026-05-18 from team feedback during fixture authoring):

The standard single-RUN form for `test/ir-fixtures/*.nix`:

```
# RUN: v3-eval --file %s --emit-ir | v3-check %s
```

For testing a specific pass (or pass set):

```
# RUN: v3-eval --file %s --emit-ir --opt-passes=streamFusion | v3-check %s
# RUN: v3-eval --file %s --emit-ir --opt-passes=betaReduce,constantFold,streamFusion | v3-check %s
```

For pre/post-opt comparison (multi-RUN with `--check-prefix`):

```
# RUN: v3-eval --file %s --emit-ir-raw | v3-check %s --check-prefix=RAW
# RUN: v3-eval --file %s --emit-ir     | v3-check %s --check-prefix=OPT
```

**Why `--file %s` (and not `< %s` or positional)?** Matches Nix's existing CLI convention (`nix eval --file FILE`, `nix-instantiate --file FILE`). Today's `v3-eval` accepts only a positional expression argument; adding `--file FILE` is a small addition to the MVP Day 2 work. Positional expression and `--file` are mutually exclusive (error if both given).

**Subtle**: the Nix parser silently ignores the `#`-comment directives because `#` starts a Nix line comment. The Nix file is fully valid — the parser sees only the expression. The `# RUN:` and `# CHECK:` lines are pure metadata for the test runner (`run-ir-checks.sh`) and `v3-check`. Fixture authors should NOT put Nix code in `#` lines expecting it to be evaluated.

**MVP fixture example** (Day 4 deliverable — single self-contained `.nix` file):

```nix
# RUN: v3-eval --emit-ir --opt-passes=betaReduce,constantFold %s | v3-check %s
# Tests 1-shot beta reduction + constant folding composition.
# (x: x + 1) 5 should inline to (5 + 1), then fold to 6.

(x: x + 1) 5

# CHECK: ; module n_funcs=1
# CHECK-NOT: Lambda
# CHECK-NOT: App
# CHECK-LABEL: B1:
# CHECK: v1 = LitInt 6
# CHECK: return v1
```

One file. Nix parser sees only `(x: x + 1) 5`. Runner extracts `# RUN:` line, substitutes `%s` → fixture path, executes the shell command. `v3-check` reads `# CHECK:` directives from the same file, compares stdin (the IR dump). LLVM-syntax-compatible; real LLVM `FileCheck` would also work on this fixture if a developer happens to have it installed.

**Multi-RUN fixture example** (test same source under multiple opt levels):

```nix
# RUN: v3-eval --emit-ir --no-opt %s | v3-check %s --check-prefix=RAW
# RUN: v3-eval --emit-ir %s | v3-check %s --check-prefix=OPT

(x: x + 1) 5

# RAW: Lambda
# RAW: App
# OPT-NOT: Lambda
# OPT-NOT: App
# OPT: LitInt 6
```

Both RUN lines must pass; if either fails the fixture fails. (`--check-prefix` support adds maybe +30 lines; defer to when first multi-RUN fixture lands.)

## 9. Critical self-review — what might be wrong with this plan

1. **Determinism is the critical-path risk and may be underestimated.** I budgeted 0.5-2 days for the audit. If the IR data structures use `std::unordered_map` extensively or rely on pointer-order-dependent iteration anywhere in the dump path, fixing this could take 3-5 days. Worth investigating BEFORE committing to the timeline.

2. **The `checkIr` extension (CHECK-LABEL, regex) may be more involved than budgeted.** Today's checkIr is ~90 lines. Adding `CHECK-LABEL` is straightforward; adding regex `{{...}}` requires a regex engine choice (`std::regex` is slow; `re2` is a dependency; custom is risky). Budget 1 day for regex, not 0.5.

3. **Negative fixtures are harder than positive ones.** Asserting "X does NOT happen" requires anticipating all the ways X COULD happen. Per-pass purity analysis is the example: `(throw "x") 5` should not beta-reduce — but is `throw` the only impure primop the pass should refuse? What about `abort`, `currentTime`, `getEnv`, `import`? The negative fixture's coverage requires careful thought.

4. **The "test the SPECIFIC thing" convention is harder in practice.** What about IR shape that's incidental but visible? E.g., `betaReduce` test asserts the LitInt 5 appears but the surrounding scaffolding (block ID, terminal) is not relevant. CHECK directives must be careful to skip the irrelevant parts. This is a real source of brittleness.

5. **Fixture maintenance grows superlinearly with passes.** LLVM has 50 000+ tests after 25 years. v3 will have hundreds, but each requires authoring + maintenance. The update tool (`update_test_checks.py` equivalent) is essential, not optional, after ~30 fixtures. Should be on the roadmap, not deferred.

6. **CI integration is missing from the plan.** I focused on the local runner. For sustained value, fixtures should run on every PR. This is its own work item: meson test integration is partial; full CI hook depends on the team's CI setup (GitHub Actions? Hydra?). Add as a step 4.5: ~1 day.

7. **The 10-fixture target is sparse.** Each pass deserves ~3-5 fixtures (positive, negative, composition, edge case). 10 fixtures across 8+ passes is ~1.25 per pass on average. Real coverage needs 30-50. Budget more time for fixture authoring or accept sparse initial coverage.

8. **The plan assumes the team will write fixtures.** If the team treats fixtures as test-writer's-burden rather than every-engineer's-responsibility, fixture corpus will stagnate. Need a cultural commitment: "no pass land without fixtures."

9. **The sidecar convention may collide with existing `.expected` semantics elsewhere.** Some test runners already use `.expected` files. Verify no clash before standardizing. Possible alternative: `.ir-expected` suffix.

10. **The driver `v3-eval --emit-ir` shares CLI argument space with existing v3-eval modes.** Flag collisions (e.g., `-e` for expression vs `-e` for something else) need audit. Budget for CLI cleanup if collisions exist.

## 10. Recommendation

**Ship the MVP first (5 days).** Validates value, surfaces unexpected complications (especially determinism), and provides immediate utility for IR inspection.

After MVP:
- Decision point: continue to per-pass entry points + 10 fixtures (steps 2 + 4)?
- If yes: ~1-1.5 weeks more. Total ≈ 2 weeks.
- If no: MVP is still valuable as an inspection tool.

Continuous (step 5): make fixture authoring part of every IR Phase B-H PR. Rule 0 equivalent: "no IR pass land without ≥1 positive + ≥1 negative fixture demonstrating the pass's exit criterion."

Build the update tool when the first regeneration cycle hits (~30 fixtures in).

CI integration: separate ~1-day work item; defer to after MVP value is confirmed.

## 11. Open questions for the team

**Q1 — RESOLVED 2026-05-18**: `test/ir-fixtures/` is fine; consistent with existing `test/` pattern.

**Q2 — RESOLVED 2026-05-18 (user pushback)**: NO sidecar. CHECK directives in `.nix` source file as `#` comments. Matches LLVM `.ll`-file pattern exactly.

**Q2b — RESOLVED 2026-05-18 (user pushback ×2)**: RUN: is a SHELL COMMAND (LLVM-faithful), not a custom directive. Runner is `lit`-like: extracts `# RUN:` lines, substitutes `%s`/`%t`, executes shell commands.

**Q2c — RESOLVED 2026-05-18 (user pushback ×3)**: Don't pull in the LLVM toolchain just for FileCheck. Build our own `v3-check` (~250-300 lines total, including ~86 already existing) — LLVM-syntax-compatible subset, in-tree, zero new external deps. See D2.7 (revised). Update D3.4 accordingly (done above).

**Q3 — REMAINS OPEN**: Does meson currently support `pipefail`-correct test runners? With the revised single-invocation `--ir-check FILE` model (D3.2), pipeline-composition risk is gone — but if the runner script itself pipes (e.g., for debugging), `pipefail` still matters. Verify in meson docs.

**Q4 — RESOLVED 2026-05-18**: extend `v3-eval` with ONLY `--emit-ir` (and pass-control flags). Ship a separate small `v3-check` binary (in-tree, ~250-300 lines total) — not a flag on v3-eval, not LLVM FileCheck.

**Q5 — RESOLVED 2026-05-18**: Determinism inside MVP scope. If audit reveals 3-5 days of work, MVP slips from 5 to 7-8 days. Acceptable cost; the alternative (build fixtures on flaky determinism) is much worse.

**Q6 — RESOLVED 2026-05-18**: GitHub Actions + Nix where applicable. Pattern: `.github/workflows/v3-ir-checks.yml` runs `nix develop -c bash src/libexpr-v3/test/run-ir-checks.sh` on every PR. Annotates failing fixtures inline.

## 12. Cross-references

- `LODE_REVIEW_2026-05-18.md` §6 — origin of the 5-step plan.
- `IR_OPTIMIZATION_PLAN_2026-05-18.md` — IR Phases A-H whose fixtures step 5 ships.
- `ir_dump.cc` lines 420-510 — existing `checkIr` library (the substrate this plan builds on).
- `test/smoke.cc` — existing CHECK usage on hand-built IR (the 25 directives that prove the library works).
- Action plan Phase 4 — `OPTIMIZER_PIPELINE.md` (separate work; consumes the per-pass enable matrix).

Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group. SPDX-License-Identifier: Apache-2.0.
