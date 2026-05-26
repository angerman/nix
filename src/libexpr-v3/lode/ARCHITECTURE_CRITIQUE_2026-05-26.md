# Architecture critique 2026-05-26 — cross-cutting critical review

**Date:** 2026-05-26
**Author:** session synthesis (4-agent parallel critical review)
**Status:** strategic — surfaces cross-cutting risks not captured by per-subsystem docs
**Triggering context:** user-requested ultrathink architectural review post-#815 closure / A1a+A2 landing. Four critical-lens agents dispatched in parallel (Explore mode): VM core, optimizer pipeline, cache stack, test coverage. This doc synthesises their findings against the existing strategic doc set (NEXT_STEPS §8.5, PROFILING_AUDIT, EVAL_CACHE_ARCHITECTURE, MEASURE_TWICE_CUT_ONCE).

Companion docs that this critique builds on:
- [`NEXT_STEPS_2026-05-25.md`](NEXT_STEPS_2026-05-25.md) §8.5 (architectural risks AR1-AR15) — extended here
- [`PROFILING_AUDIT_2026-05-24.md`](PROFILING_AUDIT_2026-05-24.md) (methodology blind-spot pattern)
- [`EVAL_CACHE_ARCHITECTURE_2026-05-23.md`](EVAL_CACHE_ARCHITECTURE_2026-05-23.md) §13.3 (Phase 4b RCA pattern)
- [`MEASURE_TWICE_CUT_ONCE_2026-05-23.md`](MEASURE_TWICE_CUT_ONCE_2026-05-23.md) §5.7 (methodology audit before structural conclusion)
- [`RCA_815_CROSS_WORKLOAD_2026-05-25.md`](RCA_815_CROSS_WORKLOAD_2026-05-25.md) (root-cause investigation cited as `~140/162 files still diverge` benchmark)

---

## 1. Position (TL;DR)

Four agents reviewed in parallel; **four convergent themes emerged that no single subsystem doc captures**:

1. **The non-determinism class is OPEN, not closed.** The #815 Light variant fixed 2 specific `lower.cc` sites. Per Agent 2, 7 `unordered_map<VarId, ...>` sites in `opt_*.cc` remain in DECISION-BEARING positions. Per Agent 3 / #815 RCA Light Phase 3 diagnostic, ~140/162 files still diverge across cross-process bytecode. **Each unfixed site is a #815-class reproducer waiting to fire.**

2. **Convention-encoded invariants without mechanical enforcement are the cross-subsystem failure mode.** Agent 1 lists 5 VM-core invariants; Agent 2 lists missing opt-pass pre/post conditions; Agent 3 lists 5 cross-layer cache dependencies. **The 4-instance "RESOLVED → reopened" methodology pattern of this week is a symptom of this structural cause**, not a separate issue.

3. **Test coverage is concentrated on OUTPUT parity; BEHAVIORAL correctness is weakly tested.** Per Agent 4: 143/143 lang PASS proves output equivalence, NOT force-order. 10/10 core PASS proves smoke, NOT GC soundness under stress. 580 property tests are primop-only; VM invariants (idempotence, sharing, cycle detection, post-scavenge equivalence) untested. Five LESSONS §4.9 mechanisms NOT built.

4. **R1 (Full de Bruijn IR) is more urgent than NEXT_STEPS reflects.** Three of four agents independently surfaced reasons for R1 sooner. The trigger-gated framing assumes a future signal; the agent findings suggest the signal is already present in the codebase, just unaudited.

**The single biggest recommendation:** **schedule the AR5 audit with a 1-week cap, allow it to evolve into R1 if scope warrants.** Cost: 1-2 days for audit; potentially +1 week for R1 if triggered. Expected outcome: closes the non-determinism class structurally, not site-by-site.

**Three immediate actions (≤ 1 day each):**
- **CR1:** Audit `lower.cc` for remaining iteration-order leaks beyond `lowerLetRec` / `lowerAttrs` (Agent 3 estimate: 3-4 hours).
- **CR2:** Cross-check `collectReferencedSymbols` ↔ `remapSymbolsInBytecode` for opcode-dispatch completeness (Agent 3 estimate: 2 hours).
- **CR3:** Add `OP_GET_LOCAL` `stackBase + nLocals` debug-mode assertion at frame setup (Agent 1 estimate: 2-3 days; the assertion itself is hours, the verification across edge cases is days).

---

## 2. Convergent themes (where ≥ 2 agents agreed)

### 2.1 Non-determinism class is OPEN

| Source | Evidence | Specificity |
|---|---|---|
| Agent 2 | 7 `unordered_map<VarId, ...>` sites in `opt_*.cc` decision-bearing positions | `opt_inline.cc:205` (alias map), `opt_cse.cc:132` (seen map), `opt_beta_reduce.cc:210-212` (mapBlockDefs), `opt_strictness.cc:320, 323`, `opt_func_strictness.cc:241, 354, 359`, `opt_strict_call_unthunk.cc:644, 782` |
| Agent 3 | `lower.cc` has ~140/162 files still showing name-diff after Light Phase 1+2 fix per #815 RCA diagnostic | "The third leak is still in the tree" |
| #815 RCA itself | Light variant fixed `lowerLetRec` + `lowerAttrs`; `thunkify` called from ~6 other sites untouched | `ExprWith` body iteration, inherit-from cache resolution, callPackages nesting |

**Interpretation:** The team responded to #815 with a *targeted* fix (canonical alphabetical ordering at two specific sites). The agent findings indicate the *class* of bug — process-local iteration order leaking into bytecode — has many more instances. Each is reproducible by a different workload triggering a different code path.

**Defense status:**
- A4 lint catches schema bumps; does NOT catch non-canonical iteration
- `V3_DBG_DESERIALIZE_VERIFY` detects DIVERGENCE at runtime but is opt-in and post-hoc
- #815 regression test catches the SPECIFIC scenario; does NOT cover the class

**The Real Fix Set** (mutually exclusive options):
- **Option A** — Audit all 140+ sites + apply Light variant pattern. Multi-day, fragile, doesn't scale.
- **Option B** — R1 Full de Bruijn IR. 1 week / ~300 LoC. Structural fix; eliminates the class.
- **Option C** — Replace `unordered_map` with `std::map` (or sorted `std::vector`) in decision positions across lower.cc + opt_*.cc. ~2-3 days. Targeted; doesn't fix underlying SymbolId process-locality but defends the immediate class.

Cost-benefit decision now data-informed: Option A scales linearly with site count and is fragile to new additions; Option B has high one-time cost but closes the class; Option C is a stopgap.

### 2.2 Convention-encoded invariants without enforcement

| Subsystem | Invariants identified (by agent) |
|---|---|
| VM core (Agent 1) | Closure capture positional order; intrinsic lambdas structural-not-semantic; `OP_GET_LOCAL` frame-layout fragility; with-stack snapshot-not-live; forceValue/op_force_slow keep-in-sync |
| Optimizer pipeline (Agent 2) | No documented pass pre/post-conditions; no formal dataflow invariants; pass ordering rationale partial |
| Cache stack (Agent 3) | REC_SET count vs INIT count not validated; sparse symbol collection completeness not checked; LambdaDescriptor field-layout-vs-FuncId correspondence not enforced; opcode fingerprint vs semantic identity not validated; deserialize emit-order assumption not asserted |

**The pattern:** invariant exists, is documented in comments or implicit in correctness, but no runtime check / static analysis / property test catches violations.

**The 4-instance "RESOLVED → reopened" methodology pattern of this week is a downstream symptom:**
- Phase 4b cache scope bug (`35564703f`) — invariant: cache hook fires only on intended scope. Was violated; no enforcement.
- CU-disk-cache cold-tax artifact (`fe678273a`) — invariant: cold timing measures the system under test. Was violated by harness; no enforcement.
- disk_cache PK collision (`9e09a7e4c`) — invariant: cache entries persist under schema bumps. Was violated by single-column PK; no enforcement.
- #815 cross-workload regression — invariant: cached CU is process-independent. Was violated by iteration order; no enforcement.

Each was diagnosed via individual RCA. The structural answer is mechanical enforcement infrastructure, not case-by-case fixes.

### 2.3 Behavioral-correctness test coverage gap

| Domain | Output parity tests | Behavioral correctness tests |
|---|---|---|
| Lang fixtures | 143/143 (Agent 4) | None — output matched, force order untested |
| Primops | 580 (58 categories × 10) | None — no overflow boundaries, no edge cases |
| Nixpkgs sweep | 64-pkg drvPath byte-equal | None — single output measured |
| haskell-nix-example | byte-identical to TW | None — VM-state invariants not asserted |
| Phase 4b IFD synthetic | hit rate + correctness | None — no force-order trace comparison |

**Per Agent 4:** the LESSONS §4.9 10-mechanism debug story has 5 BUILT, 2 PARTIAL, 3 NOT BUILT (#6 differential fuzzing missing grammar/shrinker; #8 deterministic stress not built; #9 crash artifacts not built).

**This compounds with theme 2.2:** even if invariants were documented, the test suite couldn't catch most violations because behavioral correctness is undertest. A grammar-based fuzzer + VM-invariant property tests would expose Agent 1's slot-chain growth, Agent 2's pass-interaction risks, Agent 3's cross-layer integrity gaps in unified way.

### 2.4 R1 (Full de Bruijn IR) urgency exceeds NEXT_STEPS framing

| Agent | Specific reason R1 should fire sooner |
|---|---|
| Agent 2 | "Recommendation 1 must complete before any new opt pass lands. The current pipeline has ~10 passes; each new pass multiplies interaction surface." |
| Agent 3 | "Schema-bump rate is unsustainable at AOT scale unless the underlying SymbolId process-locality is eliminated (R1 / Full de Bruijn IR)." |
| Agent 1 | 5× scale risks include patterns R1 partially addresses (ChainBindings discriminator validation under parallel; intrinsic-kind structural mismatch detection) |

The NEXT_STEPS Tier R1 trigger conditions:
- Second #815-class bug → **fires immediately if AR5 audit (CR1) finds 3+ sites**
- Stage 9 trigger B → not active
- Unison Item 3 active → not active
- Stage 13 active → not active
- AOT committed → not active
- AR5 audit ≥ 3 sites → **unscheduled, may fire**
- Symbol-table remap regresses → not active

The audit (AR5 / CR1 in this doc) is the load-bearing trigger. **If the audit fires AND finds 3+ sites, R1 fires.** Agent 2's 7 opt_*.cc sites + Agent 3's reference to "third leak" suggest the audit will trip.

**Recommendation:** schedule CR1 audit with a 1-week cap. Either:
- (a) Audit finds < 3 sites → AR5 trigger doesn't fire; R1 stays trigger-gated; we have hardened defenses anyway
- (b) Audit finds ≥ 3 sites → R1 fires; do it. The audit cost is sunk; R1 adds 1 week.

The framing change: from "wait for trigger" to "actively probe whether trigger should fire."

---

## 3. Per-subsystem findings (condensed)

### 3.1 VM core (Agent 1)

**5 architectural smells** (most concerning):
- `OP_GET_LOCAL` unchecked bounds (`vm.cc:2866-2870`) — comment explicitly accepts UB; silent corruption hazard
- Static `const bool` getenv caching trap in dispatch (`vm.cc:2897-2905, 4018-4023, 4057-4058, 6301-6302`) — test harnesses toggling env mid-session see stale cache; CLAUDE.md §4 forbids inline getenv but doesn't address this pattern
- Thunk cell-update invariant scattered across 3 write-sites with no central synchronizer (`closure.hh:114-148, vm.cc:10238-10270`) — Phase D barrier is convention, not enforced
- Slot indirection chase asymmetry — path compression fires only on Evaluated thunks, not Slot chains (`vm.cc:6294-6452`) — at 5× depth, unbounded chain growth
- Bridge thunk re-entrancy without TW-GC stability check (`vm.cc:4266-4305`) — `ForceChainGuard` defends cycles but not cross-GC stale pointers

**5 undocumented invariants:**
- Closure capture positional order — lowerer ordering of `freeVars` MUST match emit's `OP_GET_UPVALUE` indices
- Intrinsic lambdas are structural matches (Fix, Extends) — nixpkgs shape change silently disables intrinsic
- `OP_GET_LOCAL` indices frame-layout-dependent, off-by-one fragile (`vm.cc:2871-2873`)
- With-stack capture is snapshot-at-creation (`closure.hh:71-72`, `ir.hh:145-148`) — latent use-after-free if GC becomes precise
- `forceValue` (C-recursive) and `op_force_slow` (iterative) MUST be kept in sync — no automated equivalence check

**5× scale risks** (most concerning):
- Slot-chain growth without compression at scale — real nixpkgs cardano-node has 4096+ `lib.composeExtensions` layers; chain unbounded
- Write barrier cost with 5× mutation rate — Phase D dirty-marking list could outpace scavenge → GC thrashing
- ChainBindings single-bit `kind` discriminator (commit `98ca953bb`) — only 2 kinds; under parallel eval (R5/Stage 13) third kind needed → silent overflow
- `kMaxIndirectionChase=100000` hardcoded (`vm.cc:176`) — not adaptive; rejects valid deep overlay chains
- `OP_CALL` fun-force writeback vs `OP_RETURN` cell-write race window (`vm.cc:4128-4144, 6247-6270`) — serialized today, opens under Stage 13 work-stealing

**Specific recommendations from Agent 1:**
- Invariant-checking wrapper around Thunk cell-updates (1-2 days)
- Unify `forceValue` and `op_force_slow` path compression (3-5 days)
- Validate `OP_GET_LOCAL` `stackBase` at frame setup (2-3 days)
- SymbolTable-level intrinsic-kind validator (2 days)
- Slot-chain compression in `OP_FORCE` (2-3 days)

### 3.2 Optimizer pipeline (Agent 2)

**7 non-determinism sites in opt_*.cc decision-bearing positions** — see §2.1 table above for citations.

**4 pass-interaction risk scenarios:**
- `inlineTrivialBindings` AMPLIFIES upstream non-determinism (#815 root cause)
- `betaReduce` + `constantFold` interdependency — inlining choice cascades
- `inlineTrivialBindings` + `applyStrictnessAtCallSites` — alias rewrites invalidate single-use assumptions
- `opt_func_strictness` pre-conditions — strictness decision depending on unordered iteration can invalidate freeVars walk

**Missing principle:** no documented criterion for "should we add this pass." Visible heuristics (re-run after expose, alias-collapse after CSE, DCE last) exist but no formal pre/post-condition spec. The pipeline (per `opt_const_fold.cc::optimise()` lines 274-477) is 13 steps, conditional gates intermixed.

**Test coverage gaps:**
- No cross-process hash-order divergence test (implicit in disk-cache, not explicit)
- No pass-ordering stress (swap two passes; does final IR differ?)
- No hash-collision determinism test
- No pre/post-condition violation test
- DCE + later passes interaction untested

**Specific recommendations from Agent 2:**
- Determinism audit (3-4 days, CRITICAL) — replace `unordered_map<VarId, ...>` decision-bearing sites with sorted vectors or `std::map`
- Pass ordering specification (2 days) — pre/post-condition comments + rationale per pass
- Synthetic pass-interaction tests (5-7 days) — IR-CHECK fixtures stressing pass interactions
- Occurrence analysis adoption (1 day) — flip `deadBindingElimViaOccur` default
- Light canonical ordering in all opt passes (3-5 days, deferred after #1)

Agent 2's blocker statement: *"Recommendation 1 must complete before any new opt pass lands. The current pipeline has ~10 passes; each new pass multiplies interaction surface."*

### 3.3 Cache stack (Agent 3)

**5 cross-layer interaction risks:**
- Symbol-remap + REC_SET slot collision (`serialize.cc:462-468` × `lower.cc:2728-2790`) — count mismatch underflows silently
- Sparse symbol-table collection doesn't cover all operand sources (`serialize.cc:133-170`) — opcode added to lower.cc without updating walk → untracked symbol → garbage on load
- LambdaDescriptor field layout → FuncId/name correspondence (`serialize.cc:709-800` × `lower.cc:2707-2790`) — process-local FuncId allocation order divergence (root #815)
- Opcode fingerprint gates deserialization but not blob interpretation (`include/v3/serialize.hh:148-152`) — same fingerprint, different opcode semantics if enum shifts
- Deserialize → emit emit-order assumptions — three sites (lower iteration, emit sort, deserialize re-sort) must stay synchronized; no invariant-checker

**Schema-bump rate analysis:** v8 → v13 in 5 days. Driven by process-local SymbolId being baked into bytecode. **Not sustainable at AOT scale** until R1 eliminates the underlying cause. Agent 3: *"Each discovery [of non-derivable field] is a schema bump. A4 lint catches the bump discipline but doesn't prevent the discovery cadence."*

**A4 lint coverage gaps:**
- ✗ Bytecode shape mismatches (REC_SET count vs INIT count)
- ✗ Missing symbol-ID collection (collectReferencedSymbols misses an opcode)
- ✗ FuncId allocation order changes in lower.cc that don't add LambdaDescriptor fields
- ✗ Opcode enum value shifts
- ✗ Iteration-order dependencies across three subsystems

**Keep-up-at-night scenarios:**
- Iteration-order leaks in lower.cc that aren't FuncId allocation (the "third leak")
- Sparse symbol-table + opcode fingerprint can diverge (new opcode bug)
- Boehm conservative scan + cross-process SymbolId references (AR9 manifestation)

**Specific recommendations from Agent 3:**
- Audit `lower.cc` for remaining iteration-order leaks (3-4 hours, CRITICAL)
- Cross-check `collectReferencedSymbols` ↔ `remapSymbolsInBytecode` (2 hours)
- Document non-derivable LambdaDescriptor fields (1 hour)
- Bytecount validation for REC_SET/REC_INIT pairs (2 hours)
- Schema-stability gate for R8/AOT (3-4 days, long-term)

### 3.4 Test coverage (Agent 4)

**LESSONS §4.9 mechanism status:**
| # | Mechanism | Status |
|---|---|---|
| 1 | Bisect nixpkgs | ✓ BUILT |
| 2 | Differential TW-oracle testing | ✓ PARTIAL — front door missing |
| 3 | Trace evaluation semantics | ✓ PARTIAL — consolidate bitmask |
| 4 | Regression tests | ✓ BUILT |
| 5 | Profiling | ✓ PARTIAL — CI integration missing |
| 6 | Differential fuzzing | ⚠ PARTIAL — grammar generator absent |
| 7 | Property tests for VM invariants | ⚠ PARTIAL — primop-only |
| 8 | Deterministic stress | ✗ NOT BUILT |
| 9 | Crash artifacts on abort | ✗ NOT BUILT |
| 10 | Issue→fixture manifest | ✓ BUILT (REPROS.md) |

Three NOT BUILT, two PARTIAL with known gaps.

**5 classes of bug that would escape today's tests:**
- Force-order divergence (output matches, force events differ) — Agent 1's slot chain, Agent 2's pass interaction
- Cycle-detection regression under complex sharing — property tests primop-only
- Post-scavenge value corruption (Phase D/E missed barrier) — only fires under stress
- Laziness-preservation violation in new thunkify sites — no grammar fuzzing
- Primop semantic drift via inlined FFI — random 10 cases per primop misses edges

**4 false-confidence risks:**
- 143/143 lang PASS = output, not behavior
- 10/10 core PASS = smoke, not GC under stress
- 2 lint scripts PASS = specific anti-patterns, not perf regressions
- ~5 min suite = stateless, not interaction-coverage

**Specific recommendations from Agent 4:**
- Grammar-based Nix expression fuzzer (3-5 days)
- 4 VM-invariant property test suites (5-7 days)
- Wire `perf-trace.py` into nightly CI (1 day)
- Deterministic-stress gates GC_STRESS / RECYCLE_STRESS / ALLOC_SEED (2-3 days)
- `v3-diff-eval` front door (arbitrary-input differential testing) (2-3 days)

---

## 4. The R1 reconsideration

The original Tier R1 framing (NEXT_STEPS §6.5) put R1 behind 7 trigger conditions, none currently active. The agent findings shift this:

| Trigger condition | Current status | Agent-surfaced update |
|---|---|---|
| Second #815-class bug | Not fired | Agent 2: 7 opt_*.cc sites have the SAME pattern; Agent 3: "third leak is still in the tree" — class is open, bugs queued |
| Stage 9 trigger B | Not active | Unchanged |
| Unison Item 3 active | Not active | Unchanged |
| Stage 13 active | Not active | Agent 1: 5 sites would break under parallel — R1 is prereq when Stage 13 starts |
| AOT committed | Not active | Agent 3: schema-bump rate unsustainable at AOT scale; R1 reduces churn (also AR8) |
| AR5 audit ≥ 3 sites | **Unscheduled** | **Agent 3 directly recommends this audit; Agent 2's 7 opt sites alone meet the threshold** |
| Symbol-table remap regresses | Not active | Unchanged |

**Recommendation: re-classify R1 from `trigger-gated waiting` to `actively probing`.**

Concrete action:
- **CR1 (this doc)** = AR5 audit + Agent 3's lower.cc iteration-order audit, expanded to cover opt_*.cc per Agent 2 findings.
- Effort: 1-2 days total (Agent 3's 3-4h + opt_*.cc scan extension)
- Pre-committed decision: if total non-deterministic decision-bearing sites ≥ 5 across lower.cc + opt_*.cc → R1 fires within 2 weeks.

The trigger sharpening: **don't wait for the next #815-class bug. The pattern is already present in the code. Audit, then decide.**

### 4.1 Cost-benefit at current state

**Cost of R1 (Full de Bruijn IR):** 1 week / ~300 LoC per `LINKING_DESIGN_2026-05-17.md` Phase L1. May need re-estimation; estimate is over a week old.

**Cost of NOT doing R1, given agent findings:**
- Each #815-class regression: 12-30 hours of RCA (per the #815 timeline itself: 4 commits from first "RESOLVED" to truly resolved, ~24 hours)
- Number of latent reproducers: ≥ 7 from Agent 2 + unknown from "third leak" + opt-pass interaction combinatorics
- Best-case: 0 fire in next 30 days; worst-case: 3-5 fire, 60-150 hours of RCA
- Plus: each fire risks production cache corruption (#815 was caught by test harness; production user might not notice silently-wrong evaluation)

**Cost of partial fix (Option C from §2.1):** ~2-3 days to replace `unordered_map` with `std::map` everywhere. Closes the immediate class. Does NOT eliminate process-local SymbolIds. Does NOT enable Stage 13 / AOT.

**Recommendation:** Option C (2-3 days) is reasonable as a defensive intermediate IF R1 is genuinely > 2 weeks of work. CR1 audit will reveal whether R1's 1-week estimate is realistic or stale.

### 4.2 Alternative: schedule R1 directly without audit

If the team trusts the agent findings without an audit:
- R1 starts immediately
- 1 week dedicated work
- Net outcome: class closed structurally, plus Stage 9 trigger B becomes measurable, Stage 13 + AOT prerequisites partially satisfied

This is **more aggressive than the recommended approach** but defensible given:
- Three of four agents independently arrived at "R1 sooner"
- The 4-instance "RESOLVED → reopened" methodology pattern shows the cost of conventional discipline at this stage
- The team's velocity is high enough to absorb a 1-week refactor

The trade-off is between 1-2 days of risk-reducing audit vs 1 week of committed refactor. Either is defensible.

---

## 5. The non-determinism class — current attack surface (detailed)

For ease of future work, here's the consolidated attack surface:

### 5.1 lower.cc (per Agent 3)

| Site | Status | Action |
|---|---|---|
| `lowerLetRec` | ✓ FIXED Light variant (`93da764fd`) | None |
| `lowerAttrs` | ✓ FIXED Light variant (`93da764fd`) | None |
| `thunkify` from `ExprWith` body iteration | **OPEN** | Audit + apply Light pattern |
| `thunkify` from inherit-from cache resolution | **OPEN** | Audit + apply Light pattern |
| `thunkify` from `callPackages` nesting | **OPEN** | Audit + apply Light pattern |
| `thunkify` from other ~3 sites | **OPEN** | Audit + apply Light pattern |
| ~140/162 files showing name-diff per #815 RCA Light Phase 3 diagnostic | **OPEN** | Source identification needed |

### 5.2 opt_*.cc (per Agent 2)

| File:Line | Container | Decision-bearing? | Action |
|---|---|---|---|
| `opt_inline.cc:205` | `unordered_map<VarId, VarId> alias` | YES (path compression target) | Replace with `std::map` or sorted vector |
| `opt_cse.cc:132` | `unordered_map<Key, VarId, KeyHash> seen` | YES (first-insertion winner) | Replace |
| `opt_beta_reduce.cc:210-212` | `mapBlockDefs` returns `unordered_map` | YES (inlining candidate selection) | Replace |
| `opt_strictness.cc:320, 323` | `unordered_map<VarId/SymbolId, size_t>` | YES (formal classification) | Replace |
| `opt_func_strictness.cc:241, 354, 359` | `blockDefs`, `nameToIdx`, `forced` sets | YES (strictness signature union) | Replace |
| `opt_strict_call_unthunk.cc:644, 782` | `unordered_set<VarId>`, `unordered_map<VarId, VarId>` | YES (MkThunk elision selection) | Replace |
| `opt_const_fold.cc:397, 412, 423` | `unordered_set<uint64_t>` validation | NO (validation only) | Document, no action |

### 5.3 Defenses currently in place

| Defense | What it catches | Gap |
|---|---|---|
| Light variant ordering (lowerLetRec, lowerAttrs) | 2 specific sites | All others |
| A4 cache-coherence lint | Schema bumps on LambdaDescriptor + deserialiseCU | Iteration order, bytecode shape |
| `V3_DBG_DESERIALIZE_VERIFY` | Runtime cache divergence | Opt-in; post-hoc |
| #815 regression test | Specific 5-pkg sweep + HNE scenario | Other workload combinations |
| `V3_DBG_FUNCID_ALLOC` (per Agent 3 recommendation) | NOT YET BUILT | — |

---

## 6. The invariant-enforcement gap — cross-subsystem pattern

| Invariant | Source | Enforcement today | Gap |
|---|---|---|---|
| Cache fires only on intended scope | Phase 4b design | Manual code review | None — caught only by RCA |
| Cold timing measures system under test | Bench harness convention | None | Caught only when CU-disk-cache RCA fired |
| Cache entries persist across schema bumps | disk_cache design | A4 lint after #814 | Lint defends future; past bugs were silent |
| Cached CU is process-independent | Cache stack design | Light variant + #815 regression | 140+ sites open |
| Closure freeVars order matches OP_GET_UPVALUE | lowerer + emit convention | None | Silent wrong-value read on mismatch |
| Intrinsic lambdas remain structurally matched | LambdaDescriptor design | None | Silent perf cliff on nixpkgs shape change |
| OP_GET_LOCAL within frame bounds | Emit + dispatch convention | None | Silent memory corruption on lowerer bug |
| Thunk cell-update is write-once | Cell-update protocol | None | Silent corruption on race |
| forceValue and op_force_slow stay equivalent | Manual sync | None | Drift caught only by parity tests |
| With-stack snapshot semantics | Closure capture design | None | Latent use-after-free under precise GC |
| Opt pass pre/post-conditions | Pipeline design | None | Pass-interaction bugs latent |
| REC_SET count vs REC_INIT | Emit convention | None | Silent skip on underflow |
| Sparse symbol-table walks all SymbolId opcodes | serialize.cc convention | None | Garbage on missing opcode |
| LambdaDescriptor field layout vs FuncId | Cross-subsystem | A4 catches field add | Order changes not caught |

**14 invariants identified across 4 subsystems; 1 has CI enforcement (A4 lint for schema bumps), 2 have partial defenses (Light variant + #815 regression test). 11 have NO enforcement.**

This is the structural underlying cause of the 4-instance "RESOLVED → reopened" methodology pattern. The fix is comprehensive invariant-checking infrastructure, not case-by-case RCA.

---

## 7. Updated risk register — additions to NEXT_STEPS §8.5

Following the existing AR1-AR15 numbering:

| AR# | Risk | Source agent | Horizon | Mitigation priority |
|---|---|---|---|---|
| **AR16** | OP_GET_LOCAL unchecked bounds + silent corruption on lowerer bug | Agent 1 | Near-term | Add `stackBase + nLocals` assert at frame setup; ~2-3 days |
| **AR17** | Static `const bool` getenv caching trap in dispatch — test harness env-var toggling sees stale | Agent 1 | Near-term | Document or replace pattern; ~1 day |
| **AR18** | forceValue / op_force_slow keep-in-sync invariant has no enforcement | Agent 1 | Near-term | Equivalence test; ~2-3 days |
| **AR19** | Slot indirection chase asymmetry — chain growth unbounded at 5× depth | Agent 1 | Long-term | Slot-chain compression in OP_FORCE; ~2-3 days |
| **AR20** | ChainBindings 1-bit discriminator insufficient for parallel (3rd kind) | Agent 1 | R5 prereq | Widen discriminator + validation; ~1 day; part of R5 work |
| **AR21** | Optimizer pipeline has no documented pass pre/post-condition spec | Agent 2 | Near-term | Pass ordering spec; ~2 days |
| **AR22** | 7 unordered_map decision-bearing sites in opt_*.cc — same class as #815 | Agent 2 | Near-term | Determinism audit + replace; ~3-4 days (Option C of §2.1) |
| **AR23** | LambdaDescriptor non-derivable field set is undocumented | Agent 3 | Near-term | Document field-by-field; ~1 hour |
| **AR24** | Sparse symbol-table walk vs remap dispatch can drift on new opcode | Agent 3 | Near-term | Cross-check tool; ~2 hours |
| **AR25** | REC_SET count vs REC_INIT count not validated at emit | Agent 3 | Near-term | Bytecount validation; ~2 hours |
| **AR26** | Opcode fingerprint vs semantic identity not validated | Agent 3 | Long-term | Opcode-set hash in fingerprint; ~1 day |
| **AR27** | 3 of 10 LESSONS §4.9 debug mechanisms not built (#6 fuzzing, #8 stress, #9 crash artifacts) | Agent 4 | Near-term | Per Agent 4 recommendations; 2-3 days each |
| **AR28** | Lang test parity proves output, not force-order behavior | Agent 4 | Near-term | NIX_TRACE_EVAL diff harness; ~2 days |
| **AR29** | Property tests are primop-only; VM invariants (idempotence, sharing, cycle, post-scavenge) untested | Agent 4 | Near-term | 4 property test suites; ~5-7 days |
| **AR30** | Bench infrastructure exists (perf-trace.py) but NOT integrated into CI | Agent 4 | Near-term | CI wiring; ~1 day |

**15 new architectural risks identified; previous count was 15; total now 30.**

---

## 8. Prioritized recommendations (synthesized, not concatenated)

### 8.1 Immediate (≤ 1 day each, do this week)

| ID | Item | Effort | Agent | Why now |
|---|---|---|---|---|
| **CR1** | Audit lower.cc for iteration-order leaks beyond lowerLetRec/lowerAttrs | 3-4 hrs | Agent 3 | Trigger evaluation for R1 |
| **CR2** | Cross-check collectReferencedSymbols ↔ remapSymbolsInBytecode dispatch | 2 hrs | Agent 3 | Prevent scenario 2 |
| **CR3** | Document non-derivable LambdaDescriptor fields | 1 hr | Agent 3 | Informs next schema-bump decision |
| **CR4** | Wire perf-trace.py into nightly CI | 1 day | Agent 4 | Closes LESSONS §4.9 #5 partial |
| **CR5** | Add bytecount validation REC_SET vs REC_INIT pairs | 2 hrs | Agent 3 | Compile-time, zero runtime cost |

**Total: ~1.5 days of work, mostly parallel-able.**

### 8.2 Near-term (≤ 5 days, do next week if CR1 trips R1, otherwise this cycle)

| ID | Item | Effort | Agent | Outcome |
|---|---|---|---|---|
| **CR6** | Determinism audit + replace `unordered_map<VarId, ...>` in opt_*.cc decision positions | 3-4 days | Agent 2 | Closes 7 sites; defensive intermediate to R1 |
| **CR7** | Validate OP_GET_LOCAL stackBase + nLocals at frame setup | 2-3 days | Agent 1 | Catches lowerer alignment bugs |
| **CR8** | Pass ordering specification — pre/post-condition comment headers | 2 days | Agent 2 | Documentation + enables synthetic tests |
| **CR9** | Deterministic-stress gates (GC_STRESS, RECYCLE_STRESS, ALLOC_SEED) | 2-3 days | Agent 4 | Closes LESSONS §4.9 #8 |
| **CR10** | v3-diff-eval front door for arbitrary-input differential testing | 2-3 days | Agent 4 | Closes LESSONS §4.9 #2 partial |

**Total: ~12-15 days of work, parallel-able into ~5-7 calendar days with multi-person.**

### 8.3 Strategic (1-2 weeks each, plan over coming cycles)

| ID | Item | Effort | Agent | Strategic outcome |
|---|---|---|---|---|
| **CR11** | R1 Full de Bruijn IR (Phase L1) — IF CR1 audit trips trigger | 1 wk | Multiple | Closes non-determinism class structurally |
| **CR12** | Grammar-based Nix expression fuzzer + structural shrinker | 3-5 days | Agent 4 | Closes LESSONS §4.9 #6 |
| **CR13** | 4 VM-invariant property test suites (idempotence / sharing / cycle / post-scavenge) | 5-7 days | Agent 4 | Closes LESSONS §4.9 #7 |
| **CR14** | Unify forceValue + op_force_slow path compression + add equivalence test | 3-5 days | Agent 1 | Closes AR18; removes 5× scale risk |
| **CR15** | Synthetic opt-pass-interaction IR-CHECK tests | 5-7 days | Agent 2 | Defends against next pass-interaction RCA |
| **CR16** | Comprehensive invariant-enforcement infrastructure (extends A4 pattern) | 2-3 weeks | Cross-subsystem | Addresses the structural cause of the 4-instance methodology pattern |

### 8.4 Long-term (multi-week, defer until prereq state)

| ID | Item | Trigger |
|---|---|---|
| **CR17** | Schema-stability gate for R8 / AOT distribution | When R8 triggers fire (per existing) |
| **CR18** | Slot-chain compression in OP_FORCE (closes AR19) | When cardano-node-class workload becomes primary |
| **CR19** | Crash artifacts on abort (LESSONS §4.9 #9) | Operational signal: a crash that needed manual frame-stack recovery |
| **CR20** | ChainBindings discriminator widening (closes AR20) | R5 Stage 13 active |

---

## 9. What this changes in NEXT_STEPS

### 9.1 Sections to update

1. **§3 Tier A** — add CR1-CR5 as "Tier A-bis" or extend existing items:
   - CR1 + CR2 + CR3 + CR5 = AR5 audit expanded (was 1-2 hr task; now 1-1.5 day comprehensive)
   - CR4 (perf-trace CI integration) = standalone item, ~1 day, high ROI

2. **§4 Tier B** — add CR6-CR10:
   - CR6 (determinism audit + opt_*.cc fix) likely highest priority near-term
   - CR7-CR10 fill out the Tier B / B-bis space

3. **§6.5 Tier R** — R1 trigger sharpening:
   - Add CR1's outcome as a concrete sub-trigger (≥ 5 sites total = R1 fires)
   - Update R1 status from "trigger-gated" to "trigger-probing"

4. **§8.5 AR list** — add AR16-AR30 (15 new risks)

5. **§12 Operating rules** — add:
   - "Invariant-with-no-enforcement is a debt; track in AR list" (cross-cutting)
   - "Audit BEFORE waiting for a reproducer when class-of-bug evidence is present"
   - "unordered_map<K, V> in decision-bearing positions = non-determinism class; replace with std::map or sorted vector"

### 9.2 New section recommendation

Add **§15 Invariant register** to NEXT_STEPS, listing the 14 invariants from §6 of this doc with enforcement-status column. As enforcement infrastructure lands, mark items closed. Living doc; tracks debt over time.

### 9.3 Tier R1 status change

Current NEXT_STEPS R1 text: *"deferred per the RCA doc's 'Lessons' section"*.

Recommended update: *"trigger-PROBING (active audit CR1 in flight). Either confirms < 3 sites → stay deferred OR finds ≥ 5 sites → R1 fires within 2 weeks."*

---

## 10. Honest limits

- **Agent depth limits.** Each agent read ~5-10 files within a 2-hour budget. Some smells may not have been surfaced. Recommendations are starting points, not exhaustive.
- **File coverage limits.** None of the agents read `bench/` deeply (Agent 4 listed but didn't analyze). None read recent commit bodies for the #821-#823 series. There may be additional findings in files not sampled.
- **Subjectivity in "critical lens".** Each agent was prompted to find smells/risks. Confirmation bias toward concerns. Some flagged items may be acceptable trade-offs the team has already considered.
- **Effort estimates are agents' judgments**, not validated. CR1 at 3-4 hours might be 1-2 days in practice; CR11 (R1) at 1 week is the existing estimate but stale.
- **Convergence ≠ truth.** Four agents independently arriving at "R1 sooner" is a strong signal, but each had access to the same context about #815. The convergence partially reflects shared input, not just independent confirmation.
- **The 14 invariants list (§6) is from agent findings**; there may be more invariants the agents didn't surface. Treat as floor not ceiling.
- **Cost estimates for "next reproducer" use #815's 24-hour RCA as the basis**. Future RCAs may be faster (lessons learned) or slower (deeper issues). The 60-150 hour range for 3-5 fires is a rough guide.
- **CR6 (Option C) is presented as alternative to CR11 (R1)**. They're not strictly mutually exclusive; CR6 in 3-4 days followed by CR11 in 1 week is also viable. The choice depends on whether closing the immediate class quickly is worth 3-4 days of work that R1 would obsolete.
- **AR list growth from 15 to 30 risks** doesn't mean v3 is twice as risky. It means the systematic review surfaced existing risks that weren't yet documented. The risk landscape is unchanged; the visibility into it improved.

---

## 11. Cross-references

This doc operationalises agent findings against:

- [`NEXT_STEPS_2026-05-25.md`](NEXT_STEPS_2026-05-25.md) — §3 Tier A / §4 Tier B / §6.5 Tier R / §8.5 AR list / §12 operating rules — all proposed updates per §9 above
- [`PROFILING_AUDIT_2026-05-24.md`](PROFILING_AUDIT_2026-05-24.md) — confirms methodology blind-spot pattern is symptom of broader convention-without-enforcement (Theme 2.2)
- [`EVAL_CACHE_ARCHITECTURE_2026-05-23.md`](EVAL_CACHE_ARCHITECTURE_2026-05-23.md) §13 — Phase 4b RCA was one of 4 RESOLVED→reopened instances
- [`MEASURE_TWICE_CUT_ONCE_2026-05-23.md`](MEASURE_TWICE_CUT_ONCE_2026-05-23.md) §5.7 — methodology audit rule motivated by same pattern; this doc extends to structural cause
- [`RCA_815_CROSS_WORKLOAD_2026-05-25.md`](RCA_815_CROSS_WORKLOAD_2026-05-25.md) — "140/162 files still diverge" reference; "Lessons for the Full variant"
- [`PROFILING_IMPROVEMENTS_2026-05-24.md`](PROFILING_IMPROVEMENTS_2026-05-24.md) — T1.1 / T2.x / T3.x align with several CR items here
- [`MEMORY_REDUCTION_OPPORTUNITIES_2026-05-23.md`](MEMORY_REDUCTION_OPPORTUNITIES_2026-05-23.md) — A1 work in flight; AR16-AR20 (VM core risks) compose
- [`LESSONS_LEARNED_2026-05-15.md`](LESSONS_LEARNED_2026-05-15.md) §4.9 — 10 mechanisms; status updated in §3.4 + §8.2 here
- [`LINKING_DESIGN_2026-05-17.md`](LINKING_DESIGN_2026-05-17.md) Phase L1 — R1 source spec
- [`UNISON_IDEAS_2026-05-07.md`](UNISON_IDEAS_2026-05-07.md) Item 2 — R1 architectural motivation

**Commits referenced:**
- `35564703f` Phase 4b cache scope RCA (Theme 2.2 instance)
- `fe678273a` + `297f900971` CU-disk-cache cold-tax artifact RCA (Theme 2.2 instance)
- `9e09a7e4c` disk_cache PK collision (Theme 2.2 instance)
- `1b7496844` #815 truly resolved (Theme 2.2 instance)
- `93da764fd` Light Phase 1+2 canonical iteration fix (current defense, §5.1)
- `521277ac9` A4 cache-coherence lint LANDED (current defense, §5.3)
- `98ca953bb` ChainBindings discriminator scaffold (AR20 source)
- `46ce47c8a` A2 V3_RELEASE LANDED (-8.4 % wall)

---

*Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group. SPDX-License-Identifier: Apache-2.0.*
