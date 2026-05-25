# v3-lint — Pathological-Pattern Detection Infrastructure — 2026-05-22

Four parallel agents researched: (a) the Nix linter ecosystem + non-Nix
prior art (hlint, shellcheck, clippy, ruff), (b) v3's existing
diagnostic hooks, (c) the team's accumulated pattern catalog from ~60
days of commits, and (d) whether haskell.nix's `composeExtensions`
chain can be refactored away. Synthesis below is organised around the
user's three explicit asks:

> (a) users know they happen
> (b) more importantly get enough context and information, how to fix this
> (c) is there any way we could provide something like hlint, or shellcheck

Motivating case: the #757 commit comment explains the team raised the
slot-chain chase limit 4096 → 100 000 after cardano-node M5 under
v3-native callFlake hit a legitimate 4096-deep `prev` slot chain
through `lib.composeExtensions`'s `final: prev:` formals stacking
thousands of layers in haskell-nix's overlay composition. The comment
notes "slot-chain compression" as a future VM-side optimisation, but
also surfaces a question: **the user could have written this
differently and didn't know.**

## 1. Three timing points, three distinct mechanisms

The user's three asks correspond to three distinct timing points in
the eval pipeline. Confusing them is the most common failure mode of
existing Nix linters; statix and friends only cover the first.

```
   PARSE → LOWER (IR) → BYTECODE → DISPATCH → POST-EVAL
     │         │            │           │            │
     │         │            │           │            └── (c) trace-driven
     │         │            │           │                  post-mortem
     │         │            │           └── (a) runtime emission
     │         │            │                  (warn-when-it-fires)
     │         │            └────────────── compile-time decision
     │         │                              (lower-time check)
     │         └─────────────────────────── IR-level static
     │                                       analysis
     └──────────────────────────────────── AST-only syntactic
                                            (statix territory)
```

**Ask (a) — "users know they happen":** *runtime emission.* When a
pathology fires during evaluation, emit a warning to the user. v3 has
12+ existing hooks observing this kind of event today (the diagnostic
hooks inventory, §3); they're just stderr-only, atexit-only, and
behind opt-in env-vars.

**Ask (b) — "get enough context and information, how to fix":**
*diagnostic enrichment.* Each warning needs a numbered code, a
position the user can navigate to, a one-line explanation, and a
concrete suggested rewrite — the ShellCheck SC1234 / hlint
"Found:…/Why not:…" pattern.

**Ask (c) — "something like hlint or shellcheck":** *static analysis.*
A tool that runs without evaluating (syntactic) or with light
evaluation (IR-level) and catches patterns *before they fire*. This is
where existing Nix tools (statix, deadnix, nil, nixd, nixf-tidy)
operate — but only at the syntactic layer. v3 can do better because
it has the IR.

The three are complementary, not alternatives. A complete v3-lint
ships all three; they share a single rule catalog and a single
suppression mechanism.

## 2. What no Nix tool does today (the gap)

Per the ecosystem survey:

| Tool | What it catches | Architecture | Trace-driven? |
|---|---|---|---|
| statix | 15 syntactic anti-patterns (bool-cmp, eta-reduce, etc.) | rnix AST only | No |
| deadnix | Unused let / lambda / pattern / inherit | rnix AST only | No |
| nil | Syntax + dup keys + undefined names + unused | LSP, incremental | No |
| nixd | sema-extra-with + libnixf rules (catalog opaque) | LSP, shares C++ eval cache | No |
| nixf-tidy | libnixf static + semantic | CLI, JSON | No |
| vulnix | CVE matches against `.drv`s in store | post-build, store walk | No |
| nix-linter | ~20 rules, abandoned 2018 | Haskell, parser-based | No |

**No Nix linter is trace-driven. No Nix linter sees lowered IR. No
Nix linter is IFD-aware.** These three are exactly where v3 is unique
in the ecosystem — and where the team's pattern catalog (§4) lives.

This is genuinely an unfilled gap, not just one the survey missed.
The closest concept is vulnix's `.drv`-parsing closure scan, which
is a different category (post-build security, not pre-build evaluation
hygiene).

## 3. What v3 already observes — diagnostic hook inventory

Per the hook inventory: **12+ distinct observation points exist
today.** The strong news: position attribution is mature
(`resolvePosSnapshot(handle) → {file, line, column}` in O(1), ~20 B
per handle, already used in `V3_DBG_HOT_FORCE` at `vm.cc:2029`).

Top-priority hooks (already firing; need only wiring):

| Hook | File:line | What it sees | Gap |
|---|---|---|---|
| Slot-chain chase limit (#757) | `vm.cc:170, 5811` | Ring buffer + frame stack at force-iteration > 100 000 | Stderr-only, `V3_DBG_CHASE=1` opt-in |
| Hot-loop re-forcing | `vm.cc:2011-2077, 6452` | Suffix-match on every Susp→Black, atexit ratio dump | Atexit-only |
| Per-descriptor alloc + force | `vm.cc:202-290, 6238-6240` | `allocCount` / `forceCount` per LambdaDescriptor, file:line via posSnapshot | Atexit-only; no real-time threshold |
| Attrset size histogram | `alloc.hh:174, 246` | 10-bucket count, "you allocated 1644 ≥129-item attrsets" | No callsite |
| Per-opcode counts | `alloc.hh:159-168, vm.cc:2472` | `NIX_VM_OPCOUNTS=1` top-N | No callsite |
| Resource limits (#753 watchdog) | `limits.cc:265-614` | Heap / CPU / wall / RSS at limit fire, top-16 frames | Limit-fire only |
| emitSelectChain (post-#756 fix) | `lower.cc:2954-2975` | Compile-time check on `or` chain blowup | No runtime telemetry |
| NIX_TRACE_EVAL force-order | `vm.cc:1942-2009, 6423-6438` | F/W event stream per thunk | TW-legacy format, hard to machine-parse |
| Bindings origin tracking | `alloc.hh:423, 617` | file:line at every allocBindings | Off by default |
| IFD profiling | `eval.hh:42-50, primops.cc:159-260` | IFDEvent struct with drvPath, duration, stackTrace | TW-side only, v3 not wired in |

The gaps cluster into four problems:

1. **No unified registry** — 70+ separate env-var gates; no central
   list of "patterns v3 detects".
2. **Atexit-only reporting** — most hooks dump at process exit; hard
   to surface in `nix eval --json` or in real time.
3. **No deduplication** — `V3_DBG_FORCE_SITE` emits one line per
   force, can be millions of lines.
4. **No soft thresholds** — only hard limits fire (chase ≥ 100 000,
   heap ≥ N GB). The interesting cases are *soft*: "you stacked 200
   composeExtensions — that's legal but expensive."

Closing these is mostly plumbing, not new observation logic.

## 4. The pattern catalog — 27 documented patterns

The pattern-catalog agent extracted 27 pathological patterns from
~60 days of commit history + RCA memos. Every entry has shape,
symptom, cause, fix-pattern, severity, and a commit/memo citation.

Category counts:

| Category | Count | Top severity |
|---|---|---|
| Composition stacks | 3 | HIGH |
| Lowering pathologies (exponential) | 2 | CRITICAL |
| Eager-vs-lazy traps | 4 | HIGH |
| Allocation explosions | 4 | HIGH |
| Silent permissiveness (semantic) | 8 | MEDIUM-HIGH |
| IFD smells | 3 | MEDIUM |
| Closure-pool corruption | 2 | CRITICAL |
| Deep recursion / stack overflow | 1 | HIGH |

The catalog is **derived from actual debugging**, not speculation —
every entry is a real bug or pathology the team already root-caused
once. The lint rules write themselves from this list:

| # | Rule slug | Severity | Detection | Reference |
|---|---|---|---|---|
| V0001 | `exponential-or-chain-lowering` | CRITICAL | IR-level: lowerExpr call count >> M×N at emitSelectChain | #756 (d9d3eed85) |
| V0002 | `deep-compose-extensions-chain` | INFO→WARN | IR-level: composeExtensions fold length > 8; warn > 32; error > 256 | #757 + this doc §6 |
| V0003 | `missing-force-no-ctx` | HIGH | Syntactic: primMatch/Split/HashString/HashFile/FromJSON/GetAttr/HasAttr/RemoveAttrs without requireNoStringContext | #734 (8ec4ca9d7) |
| V0004 | `arithmetic-uncatchable` | HIGH | Syntactic: primops throwing `std::runtime_error` not `EvalError` | FORK_REVIEW §B2 |
| V0005 | `eager-lower-on-lazy-binding` | HIGH | IR-level: lowerExpr(e->X) for lazy-semantic e->X without thunkify | #686 (65fbbc55e), #694 (067930b52) |
| V0006 | `callclosure-unguarded-throw` | HIGH | Syntactic: callClosure in primops.cc without VMExceptionGuard | FORK_REVIEW §A4 |
| V0007 | `quadratic-bindings-merge` | MEDIUM-HIGH | Trace: OP_UPDATE chain length > N at runtime | #747 (5e0c06e5d) |
| V0008 | `primop-set-ops-over-allocate` | HIGH | Trace: primIntersectAttrs/primRemoveAttrs alloc-time bytes >> use-time | #750 (96aa6331c) |
| V0009 | `attrpostable-bloat` | HIGH (resolved) | Trace: attrPosTable size > N MB | #752 (6a3df5b0a) |
| V0010 | `recursive-force-c-stack` | HIGH | Trace: forceValue depth > N (e.g. 100) | RCA_FAMILY_A7 |
| V0011 | `type-coercion-too-permissive` | MEDIUM | Differential: v3 returns value where TW throws | #680 (0bc3c0700) |
| V0012 | `ungated-instrumentation-counter` | MEDIUM | Syntactic: `++desc->X` outside `if(g_dbgAllocDump)` | #733 (f36c9e29c) |
| V0013 | `drv-string-coercion-missing-context` | CRITICAL | Data-flow: Tag::Derivation → string without context propagation | #682 (af4a7dd0b) |
| V0014 | `ifd-via-import-derivation` | MEDIUM | Syntactic: `import (derivation-producing-expr)`, `readFile (toString drv)` | IFD_DEEP_DIVE_2026-05-21.md |
| V0015 | `ifd-output-over-forcing` | HIGH | Trace: v3 forces attrs the consumer doesn't ask for | #755 (6cb4ecdb7) |

(Full 27-entry catalog in the agent report; only top-15 high-confidence
entries shown here.)

## 5. Architecture — three modes, one rule catalog

### 5.1 Mode 1 — Syntactic (AST-only)

What statix already does. v3-lint doesn't duplicate, but offers a
*v3-specific* rule subset that requires understanding the Nix
expression language semantics (not just syntax). Examples:
- V0003 (missing forceStringNoCtx checks at known primop call sites)
- V0012 (ungated instrumentation in C++ source — meta-rule)
- V0014 (IFD smell — `import (derivation)`)

Substrate: reuse `rnix-parser` (statix uses it) OR v3's own
`parser.cc` if a v3-native version is preferred for symmetry with
the IR pass.

### 5.2 Mode 2 — IR-level (post-lower)

**Unique to v3.** The rule sees the lowered bytecode — opcode-level
shape, freeVars analysis, intrinsic detection (lib.fix / extends /
composeExtensions per `Intrinsic` enum in `closure.hh`). Catches
patterns that the AST can't see because they emerge only after
substitution / inlining / fix-point analysis.

Examples:
- V0001 (M^N lowerExpr call count — same telemetry as #756 fix)
- V0002 (composeExtensions fold length post-resolve)
- V0005 (eager-lower-on-lazy-binding — requires understanding what
  `e->X` is)

Substrate: hook into the `IR_OPTIMIZATION_PLAN_2026-05-18.md`
optimizer pipeline. A new pass `opt_lint.cc` runs *after* the rest
of the optimizer (so it sees the final shape) and emits findings via
the unified registry.

### 5.3 Mode 3 — Trace-driven (post-eval)

**Unique to v3 and unique in the Nix ecosystem.** After (or during)
a real evaluation, surface findings from runtime observations. The
12+ existing hooks already collect the data; this mode formalises
the output.

Examples:
- V0010 (forceValue C-stack depth > N from a real run)
- V0007 (OP_UPDATE chain length at runtime)
- V0015 (IFD output over-forcing — needs IFD profile data)

Two sub-modes:
- **Inline** (`NIX_V3_LINT=warn`): emit warnings to stderr / a JSON
  sidecar as patterns fire during a normal `nix build`. Throttled
  via the existing per-pattern dedup logic (one fire per
  `(rule, file:line)` pair).
- **Post-mortem** (`v3-lint --replay <trace-file>`): run the eval
  with `NIX_TRACE_EVAL` to capture a trace, then analyse offline.

Substrate: the diagnostic hooks already exist (§3). The work is
plumbing — a single `lintEmit(ruleId, position, context, …)` function
called from each hook, backed by a rate-limited reporter that writes
to the chosen output sink.

### 5.4 The unified output format

All three modes emit findings in a single shape (SARIF 2.1.0 mappable):

```json
{
  "rule_id": "V0002",
  "rule_slug": "deep-compose-extensions-chain",
  "severity": "warning",
  "primary_location": {
    "file": "haskell.nix/overlays/haskell.nix",
    "line": 730,
    "column": 5
  },
  "related_locations": [
    {"file": "user-project/flake.nix", "line": 42, "note": "extend call here"}
  ],
  "message": "composeExtensions fold of 184 layers; chain depth is non-linear at force time",
  "explain": "v3 path-compresses repeated forces (O(1) after first touch), but cold-paths still walk the chain. See https://v3.nixos.org/lint/V0002",
  "suggested_fix": {
    "rewrite": "lib.composeManyExtensions [...]",
    "auto_fixable": "unsafe",
    "rationale": "Rewrites change which intermediate `final.X` slots exist; may affect evaluation order if any overlay depends on prior state."
  }
}
```

This format is directly machine-consumable (SARIF-uploadable to
GitHub Code Scanning, IDE-LSP-consumable via JSON-RPC) and
human-printable (a renderer can produce ShellCheck-style messages
from it).

## 6. The #757 rule — concrete first example

The motivating case, fully specified to illustrate the design:

**Rule V0002: `deep-compose-extensions-chain`**

**Shape (Mode 2 — IR-level):**

Trigger when any of these emit-time conditions hold:

1. A `let`-binding whose RHS is `lib.composeExtensions` /
   `lib.extends` with one operand being a let-bound name whose RHS
   also contains `lib.composeExtensions` / `lib.extends`
   (left-spine recursion ≥ 2).
2. A `lib.foldl'` / `builtins.foldl'` / `lib.foldr` with
   `lib.composeExtensions` (or `lib.flip lib.extends`) as the step
   function AND a list literal of length > 8.
3. A function whose body is `lib.fix (… extends … extends …)` with
   > 2 nested `extends`.

**Detection (Mode 3 — trace, supplementary):**

Bump a per-`composeExtensions` counter at the `Intrinsic::Compose`
match site in `closure.hh`. When the counter exceeds 32 *for the
same composition site*, emit V0002 with the position of the fold.

**Severity ladder:**

- `info` — fold length 8–32 (allowed; advisory)
- `warning` — 32–256 (rewrite recommended; soft cost)
- `error` — >256 (hard cost; v3 raises chase limit but cold-path
  re-eval still O(n))

**Suggested rewrite:**

```
- ((project.extend a).extend b).extend c
+ project.appendOverlays [a b c]
```

For library authors using `composeExtensions` directly:

```
- lib.foldr lib.composeExtensions base overlays
+ lib.composeManyExtensions overlays   # then apply once at the end
```

**False-positive guard:**

Skip when all elements in the chain are function literals that
statically don't bind `prev` (`final: _prev: …` or `final: …`). These
are already prev-free and fold trivially without the slot-chain cost.
For mixed chains where 60-70 % of elements reference `prev`, the
warning still fires.

**Auto-fix:**

Mark as `unsafe`. The rewrite is semantics-preserving for the
*final* attrset value but changes the intermediate `__overlay__`
shape that `appendOverlays` relies on (see haskell.nix line 730).
Apply only with `--unsafe-fixes`.

**Explain link target:**

`https://v3.nixos.org/lint/V0002` would document:
- Why deep composeExtensions chains are O(N) per cold-path attr
- The v3 slot-chain compression as the VM-side mitigation
- The trade-off between `extend` chains (more modular, slower) and
  `appendOverlays` (less modular, faster)
- The 60-70% `prev`-reference ratio finding from real haskell.nix usage

## 7. UX — ShellCheck + Ruff + Clippy synthesis

From the ecosystem survey, the architectural patterns that transfer
cleanly:

### 7.1 Numbered codes + per-code documentation

Adopt ShellCheck's `V<NNNN>` scheme. Every rule has a stable numeric
code that survives rule renames. Every code has a one-page
documentation entry at `https://v3.nixos.org/lint/V<NNNN>`
explaining the pattern + fix.

### 7.2 Severity tiers (4-level, ShellCheck-mapped, Clippy-categorised)

| Tier | Maps to | Default | Example |
|---|---|---|---|
| `error` | SARIF `error`; Clippy `correctness` | `--severity error` exits 1 | V0001 exponential lowering |
| `warning` | SARIF `warning`; Clippy `perf` / `suspicious` | shown by default | V0002 deep composeExtensions, V0007 quadratic merge |
| `info` | SARIF `note`; Clippy `style` | shown by default | V0014 IFD smell |
| `pedantic` | SARIF `note`; Clippy `pedantic` / `nursery` | off by default | over-thunkification hints |

### 7.3 Three-layer suppression

Following ShellCheck's pattern (the most-copied across linters):

1. **Inline pragma** — `# v3-lint: disable=V0002 reason="intentional layering"`
   on the preceding line. Reason required when noisy.
2. **Per-project config** — `.v3-lint.toml` with `disabled = [...]`
   and `severity_override = { V0002 = "info" }`.
3. **Environment** — `NIX_V3_LINT_OPTS="-e V0002"` for CI overrides.

### 7.4 Auto-fix discipline (Ruff-derived)

Each rule declares `fixable: safe | unsafe | none`.

- **safe**: textual rewrite with no laziness / evaluation-order
  change. Applied by `--fix` default. Estimated ~30 % of rules.
- **unsafe**: changes evaluation behaviour (laziness, eval order,
  intermediate shapes). Requires `--unsafe-fixes`. ~30 %.
- **none**: needs human judgement. ~40 %.

This split is critical for Nix because almost any rewrite can change
*when* something is evaluated, which is observable through `throw` /
`assert` / side-effecting `trace`.

### 7.5 CI integration

- `--format=sarif` produces SARIF 2.1.0 → uploadable to GitHub Code
  Scanning via `github/codeql-action/upload-sarif@v3`.
- `--format=json` for tooling.
- `--format=text` for humans (ShellCheck-style).
- Exit codes: 0 clean / 1 findings ≥ severity threshold / 2 tool error.

## 8. Phased rollout

| Phase | Mode | Effort | Deliverable | Prereq |
|---|---|---|---|---|
| **Phase 1 — Hook unification** | n/a | 1 wk | Replace 70+ scattered `getenv` calls with a single `LintRegistry` + `lintEmit(ruleId, pos, ctx)`. No new rules; just consolidate. | None |
| **Phase 2 — Trace-driven seed (Mode 3)** | trace | 1-2 wk | Wire 5 existing hooks (slot-chase, hot-force, IFD profile, primop-throw, alloc-explosion) to emit via the registry. SARIF / JSON / text emitters. | Phase 1 |
| **Phase 3 — Documentation + UX** | n/a | 1 wk | Per-rule docs site (markdown source in `docs/lint/`), explain links, severity config, suppression mechanism. | Phase 2 |
| **Phase 4 — Syntactic mode** | AST | 1-2 wk | Rules V0003 (NoCtx), V0004 (uncatchable arith), V0012 (ungated counter), V0014 (IFD smell). Reuses rnix-parser or v3 parser. | None (parallel to 1-3) |
| **Phase 5 — IR-level mode** | IR | 2-3 wk | Rules V0001 (M^N lowering), V0002 (deep composeExtensions), V0005 (eager lower) — runs in optimizer pipeline post-lower. | Phase 2 (registry) + `opt_lint.cc` skeleton |
| **Phase 6 — CI + integration** | n/a | 1 wk | GitHub Code Scanning workflow, LSP server, IDE hooks. | All prior |

Total ~7-10 weeks. **Phase 1+2 alone deliver (a) and (b)** — runtime
warnings with context that point to fixes. Phase 4+5 add (c) — the
hlint-style static analysis. The user's ask (c) is the most
ambitious; (a) and (b) ship sooner and benefit even users who don't
run the linter manually.

## 9. Can the #757 case be optimised away in haskell.nix?

Per the haskell.nix refactor agent:

**The root cause is NOT in haskell.nix's overlay assembly itself.**
`overlays/default.nix:102-141` does a `builtins.foldl' composeExtensions`
over ~20 static overlays — bounded.

**The chain-grower is the consumer-side `.extend` pattern.**
`overlays/haskell.nix:726-734` defines `makeExtensible` whose
`.extend` and `.appendOverlays` produce a fresh `composeExtensions
self.__overlay__ f` per call. Every user `project.extend (final:
prev: …)` adds one layer. Cardano-class projects hit 1-4 k frames
because ~100 Haskell packages × per-package overrides × cross-target
rebuilds × meta-overlays accumulate.

**Refactor options ranked:**

1. **Batched `appendOverlays` at consumer sites** (high feasibility,
   low-mid win). Encourage `project.appendOverlays [a b c]` over
   chained `.extend`. Only helps adjacent user calls; package-set
   internals unchanged.
2. **Stratified composition** (mid feasibility, high win). Group
   overlays into phases, batch each phase, chain only 3-5 phases.
   Breaks introspection of `__overlay__` used at lines 730-731.
3. **No-prev fast path** (mid feasibility, mid win). Partition
   `composeManyExtensions` args into prev-free (foldable trivially)
   and prev-using; only the latter goes through the slow chain.
   Bounded by the 60-70 % prev-reference ratio.
4. **Lazy attribute resolution** — infeasible in stock Nix.

**v3's slot-chain compression makes this survivable but not free.**
The opForceCompress pattern (write resolved Value back to each
visited slot on first force) turns the cost from per-access O(N) to
per-attribute O(N) — first-touch tax remains, but subsequent
accesses are O(1). Cold paths still walk; rebuilds still re-pay.

**The lint recommendation: keep V0002 even after VM compression.**
Users who write 200-layer chains should know they could batch for
~50× less heap. Lint at `info` → `warning` → `error` ladder above.

## 10. Comparison with existing tools — gap matrix

| Capability | statix | nil | nixd | nixf-tidy | vulnix | **v3-lint (this proposal)** |
|---|---|---|---|---|---|---|
| Syntactic AST rules | ✓ (15) | ✓ | ✓ | ✓ | ✗ | ✓ (Phase 4) |
| Inline pragma suppression | ✗ | ✗ | ✗ | ✗ | ✗ | ✓ (Phase 3) |
| Severity tiers | ✗ | ✗ | partial | partial | ✗ | ✓ (Phase 3) |
| Auto-fix (safe/unsafe) | ✓ (some) | ✓ (LSP) | ✓ (LSP) | ✗ | ✗ | ✓ (Phase 3-5) |
| **IR-level rules** | ✗ | ✗ | ✗ | ✗ | ✗ | **✓ (Phase 5)** |
| **Trace-driven rules** | ✗ | ✗ | ✗ | ✗ | partial (store) | **✓ (Phase 2)** |
| **IFD-aware** | ✗ | ✗ | ✗ | ✗ | ✗ | **✓ (Phase 2, V0014/V0015)** |
| SARIF output | ✗ | ✗ | ✗ | ✗ | ✗ | ✓ (Phase 3) |
| LSP | ✗ | ✓ | ✓ | partial | ✗ | ✓ (Phase 6) |

Three rows have NO existing solution: IR-level, trace-driven, and
IFD-aware. v3-lint occupies a genuinely new niche.

## 11. Open questions

1. **Should v3-lint live in this repo or a separate one?** Trace-driven
   mode requires being in-process with v3. Syntactic mode could be a
   separate Rust crate using rnix. Mixed model is most likely: a v3
   in-process emitter + an external CLI that consumes the JSON output
   plus runs syntactic rules independently.

2. **Position attribution on bridge thunks.** §3 noted that
   bridge thunks + native thunks have no meaningful position. Some
   trace-driven rules (V0010 deep recursion) need the position chain;
   may need synthetic positions or "near-X" attribution.

3. **Rule numbering policy.** ShellCheck's SC1234 codes are dense
   (sequential) and have never been renumbered. v3-lint should commit
   to the same policy from day one: a numeric code, once assigned, is
   permanent; rules can be deprecated but never recycled.

4. **Suppression scope.** Inline `# v3-lint: disable=V0002` — does it
   suppress only on the next expression, or the entire block, or the
   whole file? ShellCheck disambiguates with `disable=...` (next
   command) vs `disable=...` at top-of-file (whole file). v3-lint
   should mirror: inline = next expression, top-of-file or config-file
   = whole file.

5. **Trace-driven mode under `nix build` vs `nix eval`.** The trace
   hooks fire during evaluation; a `nix build` of a large flake might
   accumulate thousands of findings. Need throttling + summarisation
   ("V0002 fired 14× — see top 3") rather than line-per-fire dumps.

6. **Does v3-lint replace the need for `NIX_V3_MAX_HEAP` etc.?** No.
   Resource limits are runtime guards (kill the eval); lint is
   advisory (the eval continues). Both stay.

7. **Static effect inference (Unison Item 4 from
   `UNISON_IDEAS_2026-05-07.md`) — should it land before V0014/V0015
   (IFD-aware rules)?** §11 of `IFD_DEEP_DIVE_2026-05-21.md`
   proposes effect propagation as the policy-shield replacement for
   materialization. If it lands, V0014 graduates from syntactic
   (heuristic match on `import (drv)`) to IR-level (precise via
   `RequiresStore` ability). Order: lint Phase 4 (heuristic) → effect
   propagation → lint Phase 5 (precise upgrade).

## 12. Honest gaps

- **Rule count to ship Phase 4-5 is unknown.** The 27-pattern catalog
  is an upper bound; many entries are CRITICAL but already fixed
  (e.g. V0009 attrPosTable, fixed in #752) and would primarily serve
  as regression detectors rather than active linting. Realistic
  Phase 5 ship: 8-12 rules.
- **The trace-driven cost is non-zero.** Each hook adds a few ns to
  the dispatch loop. On hot opcodes (OP_FORCE, OP_CALL) this matters.
  Per-hook gating + dedup keep it bounded; measurement spike needed
  before any default-on mode.
- **statix has community traction; v3-lint starts at zero.** Adopting
  the statix rule format for the syntactic subset (Phase 4) reduces
  user friction — a v3-lint syntactic rule could be expressible as a
  statix rule too. Worth investigating.
- **Some agents in this audit projected rule severity from a single
  incident.** Severity assignments here are best-guesses from
  reading commit messages, not from production deployment. Expect
  re-tuning after first real CI use.
- **The 4096 number in #757 was a single-workload observation.**
  Real depth on different cardano-class projects may vary
  significantly. The V0002 threshold ladder (8 / 32 / 256) is
  illustrative; needs validation against a few real projects before
  ship.

## 13. Recommended order

| # | Action | Effort | Falsifier |
|---|---|---|---|
| 1 | **Phase 1** — Hook unification (LintRegistry + lintEmit) — converts 70+ env-var gates into one registry | 1 wk | env-var count drops; lint-no-inline-getenv.sh stays green |
| 2 | **Phase 2** — Trace-driven seed: 5 hooks wired → SARIF / JSON / text emitter | 1-2 wk | Run on cardano-node M5: V0010 / V0007 / V0014 / V0015 should fire; #757 case fires V0002 stub (at the 100k limit) |
| 3 | **Phase 3** — UX (codes, severity, suppression, docs site) | 1 wk | First per-rule doc lives at `docs/lint/V0001.md` |
| 4 | **Phase 4** — Syntactic mode with 4 starter rules (V0003 / V0004 / V0012 / V0014) | 1-2 wk | Lint passes on a clean nixpkgs check; finds 8 sites the team manually identified in #734 |
| 5 | **Phase 5** — IR-level mode with V0001 / V0002 / V0005 via `opt_lint.cc` | 2-3 wk | `v3-lint` on haskell.nix bootstrap.nix would have caught #756 / #757 ahead of time |
| 6 | **Phase 6** — CI workflow + LSP integration | 1 wk | GitHub Code Scanning shows v3-lint findings on PRs |

The whole arc is 7-10 weeks. Phases 1+2 alone (2-3 weeks) deliver
the (a) and (b) capabilities — runtime warnings with actionable
context. Phase 4+5 add (c) — the hlint-equivalent static analysis.

---

## Summary for the three user asks

**(a) Awareness:** Phase 2 (trace-driven) — 5 existing hooks emit
findings during real evaluations, throttled to one per `(rule,
file:line)` per session. Users see warnings during `nix build`
without changing anything.

**(b) Context to fix:** Phase 3 (UX) — every warning carries a
numbered code, primary + related positions, a one-line explanation,
a suggested rewrite, and a documentation link. Format is SARIF-
compatible for IDE/CI consumption.

**(c) hlint/shellcheck for Nix:** Phase 4 (syntactic) + Phase 5
(IR-level). Phase 4 reuses the rnix-parser ecosystem; Phase 5 is
unique to v3 (no existing Nix tool sees the lowered IR). Together
they catch ~27 pattern classes the team has already root-caused once.

The pattern catalog + diagnostic hook inventory + ecosystem survey
all converge: this is a buildable design, not a research project.
Every rule has a real-world incident behind it; every hook already
exists; every UX pattern is borrowed from a shipping tool.

The #757 case is the canonical first rule (V0002, see §6). The
combination of v3-side slot-chain compression (VM fix) + V0002
(user guidance) covers both halves: the VM survives the pathology,
and the user knows there's a faster shape available.

---

Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
Input Output Group. SPDX-License-Identifier: Apache-2.0.
