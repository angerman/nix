# Formal Verification — Cost/Benefit Analysis for v3 — 2026-05-22

Analysis of whether TLA+ or related formal verification tools would
benefit the v3 VM. Triggered by the question "is this sensible?" during
the Stage 2 progress review. Conclusion: **yes, sensibly used for
three narrow bounded protocols, after cheaper formal-ish techniques
ship first.** Full evaluator verification (CompCert-style) is not a
realistic investment for this project's resource envelope.

This document is captured for the lode/ archive so the cost/benefit
table can be revisited when the team's situation changes (e.g., after
Stage 2 closes, after a concurrent-eval candidate stage is committed,
or if a sponsored verification effort becomes available).

## 1. The landscape (which tools, what they do)

| Tool | Class | Best at | v3 fit |
|---|---|---|---|
| **TLA+ / TLC / TLAPS** | Specification + model-check + proof | Concurrent protocols, state machines, invariants. Used by AWS (Cosmos DB, S3, DynamoDB), MongoDB, Azure. | ★★★ for protocols (cell-update, write barriers, fiber switch). ✗ for full VM. |
| **Alloy** | Relational specification | Counterexample-finding on small instances. Lighter than TLA+ but less proof depth. | ★★ — overlap with TLA+ targets but less stable for runtime CI. |
| **Coq / Lean / Isabelle** | Interactive theorem prover | Deep semantic proofs (CompCert, sel4, CakeML). | ✗ — 5-20 person-year commitment; not realistic. |
| **F* / Dafny** | Verified programming languages | Programs whose proofs are written alongside the code. | ✗ — would require rewriting the VM; not realistic. |
| **CBMC / KLEE / SMACK** | Bounded model checkers for C/C++ | Specific bug classes on real code (UB, races, OOB). | ★★ — bounded scope only; v3 too large for full coverage. |
| **Cryptol / SAW** | Crypto-focused | Crypto algorithm correctness. | ✗ — wrong domain. |
| **PROMELA / SPIN** | Explicit-state model checker | Concurrent protocols (similar to TLC). | ★★ — overlaps TLA+; less ergonomic for v3. |
| **Frama-C / VeriFast / VST** | C verification | Heavy C/C++ verification. | ✗ — research-grade investment. |

Prior art for VM/interpreter verification:
- **CompCert** — verified C compiler (Coq). ~20 person-years.
- **CakeML** — verified ML compiler (HOL4). Multiple person-years.
- **sel4** — verified microkernel. ~25 person-years on the kernel
  itself plus ongoing.
- **CertiKOS** — verified concurrent kernel.
- **Vellvm** — verified LLVM IR semantics (Coq).
- **Doligez & Leroy** — formalised an OCaml GC in Coq.
- **Yuasa snapshot collector / Dijkstra tri-colour** — standard
  TLA+ / Promela exercises in any GC-verification class.
- **Pizlo et al. — Riptide collector** (PLDI) — used Promela.

No production-shipping formal verification work for any Nix
evaluator (cppnix, Tvix, Lix). cppnix has no formal spec; Tvix has
property testing but no TLA+ / proof-tool work I can verify.

## 2. The v3 bug surface — what would formal methods catch?

Auditing recent bugs against formal-method fit:

| Bug class | Example | Formal-verifiable? | Cheapest tool |
|---|---|---|---|
| Memory corruption at FFI seams | #670/#671 dangling `string_view` | Yes, but bounded | **MSAN/ASAN** (sanitizer CI) |
| State-machine corruption | Closure-pool tail-call aliasing (`1708d31bd`) | Yes — classic TLA+ target | TLA+ cell-update spec |
| Write-barrier invariant violation | Phase D barrier missed sites | Yes — classic GC verification | TLA+ write-barrier spec |
| Concurrent protocol bug at FFI | bridge_yield mailbox state | Yes — small state space | TLA+ fiber spec |
| Silent type permissiveness | #680 `"x"+1`, `null+1` accepted | No (parity bug; no formal TW spec) | Differential fuzzing |
| Context propagation drop | #682 toFile string-context | No (parity bug, data-flow) | Differential fuzzing |
| "Mirror TW" comment wrong | #683 const-fold 1.0/0.0 | No (parity bug; no TW spec) | Differential fuzzing |
| Eager-on-lazy-binding | #686 lowerWith eager force | No (semantic regression; AST-shape) | Property test |
| Exponential lowering | #756 M^N emitSelectChain | No (complexity bound) | RSS watchdog (works) |
| Slot-chain depth surprise | #757 4096-deep `composeExtensions` | No (input-driven) | Lint rule V0002 |
| Memoization loop | A12 `parse.nix:61` 11.5M forces | Partial — invariant "force count ≤ alloc count + 1" | Property test |
| Stack overflow in recursive force | A12b `forceValue` C-recursion | Partial — invariant "depth ≤ N" | Property test |

**Headline pattern**: of recent bugs, ~3-4 classes (memory-corruption,
state-machine, write-barrier, concurrent-protocol) map cleanly to
formal verification. ~8 classes are parity / semantic / complexity
bugs that don't have a natural formal-method fit because TW itself
isn't formalised.

This is the key tension: **most v3 bugs come from "v3 disagrees with
TW"** and formal verification of v3 against a non-formalised TW
catches nothing. Differential testing IS the spec here.

## 3. Where TLA+ specifically pays — three bounded targets

### 3.1 Cell-update protocol — Tag::Slot pointer stability

**Existing prose spec**: `lode/CELL_INVARIANTS.md`. Promoting prose to
TLA+ is a small conceptual step; the invariants are already named.

**State**:
- Cell states: `Uninitialised` / `InProgress` / `Completed`
- Transitions: bytecode opcodes (`OP_ATTRS_REC_INIT` /
  `OP_ATTRS_REC_SET` / `OP_RETURN` / `OP_FORCE`)
- Side-table: `Thunk::cell`, `Thunk::shapeCell`, `Tag::Slot` payloads

**Safety property**:
- No observer reads a stale snapshot
- Every cell-binding writes back exactly once
- Pointer-stability across nursery scavenges

**Concrete bugs this would have caught**:
- Closure-pool tail-call aliasing (`1708d31bd`, Phase A6) — state-
  machine confusion between real and fake closures.
- The partial-Bindings registry retirement (`vm.cc:6254`'s comment)
  was prose-reasoned. A TLA+ spec would have proven the retirement
  safe before the commit, not after.

**Effort**: ~2 weeks (spec authoring ~3 days, TLC model ~1 week,
documentation + CI integration ~3 days). ~150 lines of TLA+. Spec
lives alongside `CELL_INVARIANTS.md`; TLC runs in CI as `make
verify-cell-protocol`.

**Maintenance**: spec must update when new opcodes are added that
touch cells. Each opcode addition is a small refinement check.

### 3.2 Nursery write-barrier protocol — post-Phase D

**Background**: Phase D landed 2026-05-21 (#720 milestone). Write
barriers ensure no tenured object holds a stale nursery pointer
after scavenge. This is THE canonical GC-verification target —
every textbook GC chapter formalises it.

**State**:
- Object generation: `Nursery` / `Tenured`
- Pointer-set per object: pointers OUT
- Dirty-list: tenured objects with at least one nursery pointer
- Scavenger: walks dirty-list as additional roots; updates pointers

**Safety property**:
- Closed invariant: after scavenge, no `Tenured → Nursery` pointer
  exists EXCEPT through a dirty-list entry that was processed.
- Forward progress: bounded scavenge time.

**Concrete bugs this would have caught / would catch next**:
- The "1 MB stress missed-root" in Phase E v0.2 (`9958eb4a5`) — a
  missed barrier site. Formal coverage check would have caught it
  pre-merge.
- Future barrier evolution: as `Closure::cu` / `capturedWiths`
  side-tables land per `DATA_STRUCTURE_AUDIT_2026-05-21.md`, each
  new write site must be audited. TLA+ spec parameterises the
  audit — add the new site to the spec, model-check; either it's
  consistent or TLC produces a counterexample.

**Effort**: ~2-3 weeks. The first GC barrier spec is the largest
investment (the abstraction layer between "Nix Value graph" and
"two-generation heap" must be carefully designed). Subsequent
variants (e.g., card-marking, region-based) reuse the same skeleton.

**Maintenance**: HIGH initially (every barrier-site change touches
the spec); decreases once the team gets fluent. Pays off as the
nursery design evolves toward Phase E (two-region with age-based
promotion) and beyond.

**Prior art reuse**: Doligez & Leroy's OCaml GC formalisation is
the closest existing precedent. The Phase D dirty-bit design (Path α
per the decision memo) is structurally similar to OCaml's minor-heap
mutator-recorded set; the OCaml proof can be ported substantially.

### 3.3 bridge_yield fiber-switch protocol

**Background**: `include/v3/fiber.hh` + `bridge_yield.hh` +
`bridge_yield.cc`. Two-fiber concurrent protocol with ucontext
switch and mailbox-based communication.

**State**:
- Mailbox: `kind` (Idle / ForceTW / Resume) + `twValueToForce` +
  `resultV3` + `exc`
- Active fiber: Main / Bridge
- Pending exception across switch boundary

**Safety property**:
- No mailbox slot is read before it's been written by the other
  fiber.
- Exception flow crosses fiber boundary cleanly (no swallowed
  exceptions, no double-throw).
- Resource ownership: each Value pointer in the mailbox has a
  single owner at any time.

**Concrete bugs this would have caught**:
- #670/#671 dangling `string_view` lived NEAR the bridge code but
  the bug itself was at a different FFI seam (symbol-table interning).
  A sister "FFI Value lifetime" spec, separate from the fiber
  protocol, is what would have caught it.
- The bridge fiber stack-size investigation (#668-related) was
  partly motivated by uncertainty about exception flow. A spec
  would document the contract.

**Effort**: ~2 weeks. Smallest of the three targets because the
protocol itself is genuinely small (one mailbox, two fibers, four
states).

**Maintenance**: LOW — the fiber protocol changes infrequently.
Spec serves as documentation as much as verification.

**Honest caveat**: bridge_yield is fading in importance as v3
becomes more self-sufficient. If Stage 2 closes (gate deletable),
the bridge surface shrinks and this spec's value drops. Defer
until Stage 2's bridge architecture stabilises (post-#755 fix).

## 4. Where formal verification does NOT pay

### 4.1 Full evaluator semantics (Coq / Lean / Isabelle)

**Verdict**: Not realistic. CompCert took 20 person-years. The
project's resource envelope (1-2 engineers, 13-week Stage 2
runway) is off by an order of magnitude.

Even a partial verification ("v3 lowering preserves Nix
semantics") would require a formal definition of "Nix semantics"
that doesn't exist. Building one is its own multi-year project
(Tvix has approached this informally and not converged).

### 4.2 v3-vs-TW differential correctness

**Verdict**: Already covered by `bench/diff-tw-v3.sh` + the lang
test suite + the regression repros. The "spec" is operational
(TW's behaviour); formal verification of v3 against a non-formal
spec catches nothing additional.

Differential fuzzing (§5 below) extends the coverage of this
operational spec.

### 4.3 Performance / complexity bounds

**Verdict**: Not naturally formal-verifiable. TLA+ doesn't express
"this lowering is O(n) not O(n²)." Practical complexity verification
tools (RAML, Resource Aware ML; AARA papers) exist but are
research-grade for non-functional languages.

#756's M^N exponential bug was caught by the RSS watchdog (#753).
The right detection tool for complexity bugs is **memory + wall-time
ceilings + alerting**, not formal complexity analysis. v3 already
has this.

### 4.4 Parity bugs

**Verdict**: ~70% of recent bugs (#680, #682, #683, #687, #688,
#690, #691) are "v3 silently disagrees with TW." Formal verification
catches none of these without a TW spec.

These are differential-testing-shaped, not formal-verification-shaped.

## 5. Higher-ROI techniques to ship FIRST

`LESSONS_LEARNED_2026-05-15.md §4.9` lists these as "not yet built"
in the team's debug-story:

### 5.1 Sanitizer-assisted CI (★★★★★ — cheapest, highest yield)

- **Tools**: ASAN, MSAN, UBSAN, TSAN
- **Catches**: Memory corruption, use-after-free, uninitialised
  reads, signed overflow, data races. The exact class of
  #670/#671.
- **Cost**: ~2 days to wire into the test runner. Builds run
  ~2-4× slower; acceptable for CI.
- **Recommendation**: ship Monday. If MSAN had been on, #670/#671
  would have been caught the day the bug landed, not after multiple
  sessions of differential investigation.
- **Compatibility**: Boehm GC is the only friction (false positives
  on conservative pointer scanning). Solutions: build-mode without
  Boehm, or whitelist Boehm's memory regions in MSAN.

### 5.2 Differential fuzzing (★★★★★ — highest ROI for v3-specific bugs)

- **Shape**: random Nix expression generator + parity assertion
  vs TW.
- **Catches**: Systematically the #680/#682/#683/#687 class. Plus
  unknown bugs in unexplored expression shapes.
- **Cost**: ~2 weeks to ship a robust harness. Inspired by
  `csmith` (C compilers), `jsfunfuzz` (JavaScript engines).
- **Architecture**: small, well-typed Nix-expression grammar
  (avoid grammar-bombs); seed corpus from `tests/lang/` +
  failed-bisection repros; coverage-guided via libFuzzer or
  honggfuzz; both evaluators run in-process for speed.
- **Recommendation**: ship after sanitizer CI (week 1-2 of any
  hardening sprint).

### 5.3 Property-based testing for VM invariants (★★★★)

- **Properties named in `LESSONS_LEARNED §4.9`**:
  - Force idempotence: `force(force(v)) == force(v)` for all `v`
  - Sharing equivalence: forcing a value via two paths produces
    consistent results
  - Cycle-detection totality: every cycle is either reported or
    terminates
- **Catches**: Memoization bugs (A12 class), shared-state bugs,
  cycle-detection regressions.
- **Cost**: ~1 week per property. Test generator infrastructure
  amortises.
- **Recommendation**: ship after differential fuzzing. The same
  expression generator powers both; property tests check VM-internal
  invariants, fuzzing checks v3-vs-TW.

### 5.4 Issue → fixture manifest (★★★ — cheap regression discipline)

- **Shape**: `test/REPROS.md` mapping every closed bug to a
  regression fixture under `test/`.
- **Cost**: ~3 days to backfill from commit history; ongoing
  ~30 sec per new bug fix.
- **Catches**: Regressions on previously-fixed bugs. The team has
  already been hit by the #660 lazy-bridge revert and the
  realisedDerivations cache stability question. Mechanical
  prevention.
- **Recommendation**: ship anytime; doesn't compete for engineering
  time with the deeper techniques.

## 6. Cost matrix

```
Technique                  Effort     Catches                       ROI
──────────────────────────────────────────────────────────────────────────
Sanitizer-assisted CI      2 days     memory-corruption             ★★★★★
                                       class (#670/#671)
Differential fuzzing       2 wk       parity bug class              ★★★★★
                                       (#680/#682/#683/#687)
Property tests for VM      1 wk       force-idempotence,            ★★★★
                                       sharing-equiv class (A12)
Issue → fixture manifest   3 d        regression class              ★★★
TLA+ cell-update protocol  2 wk       state-machine bugs in         ★★★
                                       cell lifecycle
TLA+ write-barrier         2-3 wk     GC barrier bugs (regression    ★★★
                                       as barrier design evolves)
TLA+ fiber / bridge_yield  2 wk       FFI-protocol bugs             ★★
                                       (lower value post-Stage 2)
Coq / Lean / Isabelle      5-20 yr    everything                    ✗
```

## 7. Recommended sequence

Phased over weeks, not committed to a stage in the roadmap.

| # | Item | When | Effort |
|---|---|---|---|
| 1 | Sanitizer CI | Anytime (2 days) | 2 days |
| 2 | Differential fuzzing harness | After (1) | 2 weeks |
| 3 | Property tests for force-idempotence / sharing-equiv | After (2) | 2-3 weeks |
| 4 | Issue → fixture manifest | Parallel to all above | ongoing |
| 5 | TLA+ cell-update protocol spec | After Stage 2 closes (when cell protocol stabilises) | 2 weeks |
| 6 | TLA+ write-barrier spec | After Stage 3 close-out (Phase E productionised) | 2-3 weeks |
| 7 | TLA+ fiber / bridge_yield spec | Defer indefinitely (bridge surface shrinking) | n/a |

Items 1-4 should ship in a 4-6 week hardening sprint. Items 5-6 are
**post-stabilisation** investments — verify what's stable, not what's
in flux. Item 7 is contingent on bridge_yield remaining load-bearing
after Stage 2; if Stage 2 closes cleanly, bridge_yield's surface
shrinks and the spec value drops.

## 8. Decision criteria for revisiting

This document should be re-read when any of these triggers fire:

- **Stage 2 closes**: bridge surface shrinks; TLA+ fiber spec
  reconsidered (probably dropped).
- **Stage 13 (multi-core capabilities) is committed**: concurrent
  evaluator opens a much larger formal-verification target. TLA+
  on the work-stealing protocol becomes attractive.
- **A "wrong-hash drvPath" class bug surfaces in production**:
  store-path-affecting bugs (#682 class) escalate to formal-
  verification candidates if they recur.
- **A sponsored verification effort becomes available**: budget
  changes the calculation. Coq/Lean for partial VM semantics
  becomes realistic with a dedicated researcher.

## 9. Honest summary

Three pieces of v3 *would* benefit from TLA+ (cell-update protocol,
write barriers, fiber switch). Each is 2-3 weeks of work, well-
scoped, high enough value to justify the calendar cost — but only
after the cheaper techniques close their bug-class coverage.

**The trap to avoid is doing TLA+ first** because it's
intellectually attractive. The team's existing detection
infrastructure (STRESS=1000 tests, 143/143 lang suite, RSS watchdog,
NIX_TRACE_EVAL, differential bench) is genuinely strong; the gaps
are the four "not yet built" items in the debug story. Closing
those is engineering work the team is good at. TLA+ is a
research-shaped commitment that should follow the engineering
wins, not lead them.

The cell-update protocol TLA+ spec is the natural first move when
formal methods do become attractive — small enough to fit in
calendar, valuable enough to maintain, and aligned with the
existing `CELL_INVARIANTS.md` prose. But it's item 5 on the
priority list, not item 1.

## 10. References

- `LESSONS_LEARNED_2026-05-15.md §4.9` — the 10-item debug story;
  items 6 (differential fuzzing), 7 (property tests), 10 (issue
  manifest) are the "not yet built" gaps cited above.
- `CELL_INVARIANTS.md` — prose specification of the cell-update
  protocol; natural seed for the TLA+ spec.
- `NURSERY_PHASE_D_DECISION_2026-05-21.md` — Phase D barrier
  design (Path α per-container dirty bits); structural reference
  for the write-barrier spec.
- `DATA_STRUCTURE_AUDIT_2026-05-21.md` — flagged the Closure::cu
  side-table follow-on which is a future write-barrier site that
  would test the spec's parameterisation.
- `FORK_REVIEW_2026-05-21.md` §A4 — exception-recovery gap
  (clearBlackMarksOnException + primops.cc callClosure) is a
  state-machine candidate similar in shape to the cell-update
  protocol.
- `IFD_DEEP_DIVE_2026-05-21.md` §6 — the FFI bridge surface; if
  bridge_yield stays load-bearing, this defines the surface a
  fiber-protocol spec would cover.
- AWS use of TLA+: <https://lamport.azurewebsites.net/tla/amazon.html>
- Doligez & Leroy OCaml GC formalisation: *A concurrent, generational
  garbage collector for a multithreaded implementation of ML*, POPL 1993.
- CompCert: <https://compcert.org/>
- CakeML: <https://cakeml.org/>
- sel4: <https://sel4.systems/Info/Docs/seL4-1.4.0/sel4_OS.pdf>
- TLA+ Specifying Systems (Lamport): <https://lamport.azurewebsites.net/tla/book.html>
- `csmith`: <https://embed.cs.utah.edu/csmith/> (compiler differential
  fuzzing prior art)

---

Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
Input Output Group. SPDX-License-Identifier: Apache-2.0.
