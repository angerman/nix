# Stage 9 Killed — 2026-05-22

Stage 9 (Module Linking — content-addressed module cache at thunk-body
granularity) was killed by its own kill criterion. The Phase L0 dedup-
survey spike (#772, commit `37616ecc6`) measured the falsifying data
in ~1 day. This memo captures the falsifier so the decision is
auditable later if anyone proposes resurrecting cell-level dedup.

This is a clean example of Rule 0 working as designed: a pre-committed
kill criterion existed in the roadmap, a cheap measurement spike
falsified it, the stage was abandoned before sinking the ~5 weeks of
L1-L4 work. **No code was written that needs to be unwound.**

## 1. What Stage 9 was

Per `lode/LINKING_DESIGN_2026-05-17.md` and `ROADMAP_TO_VISION` Stage 9:

- Content-addressed cell store at **thunk-body** granularity
- Each `ir::MkThunk` / `ir::MkClosure` body would be hash-identified
- A `Map<CellHash, CompiledBody>` would deduplicate across the entire
  Nix expression universe
- Cited motivation: `LESSONS_LEARNED §4.1` named "5 000+ replicated
  callPackage closures" as a target for collapse
- Phased over L0 (survey) → L1 (ABT identity refactor, #773) → L2
  (cell schema) → L3 (manifest split) → L4 (disk-cache integration)
- Total scope: ~5 weeks, ~1 500 LoC across `serialize.cc`,
  `disk_cache.cc`, the ABT refactor in `ir.cc`/`ir.hh`, manifest
  format, and CLI plumbing

## 2. The kill criterion (committed in advance)

ROADMAP Stage 9, copied verbatim from the doc:

> "If L0's nixpkgs dedup survey shows <2× collapse, the per-thunk-
> body granularity hypothesis is wrong; abandon Stage 9 and revisit at
> the whole-`ExprAttrs`-or-`ExprLet`-bindings level."

Note what this commits to:

- A **specific numerical threshold** (2×)
- A **specific measurement** (nixpkgs dedup survey)
- A **specific decision rule** (abandon at the chosen granularity;
  the broader content-addressing idea may be revisited at coarser
  granularity)
- It does NOT commit to "we keep doing Stage 9 if 2× is met" — that's
  just the kill threshold, not a green-light threshold. A 2× win
  might still not be worth the engineering investment.

## 3. The cheap-proxy mechanism (saved ~1 week vs full ABT)

The full Stage 9 design called for `structuralHash()` on IR nodes
modulo alpha-equivalent VarId renumbering. That's the L1 refactor
(#773) — ~1 week of careful recursive walks across 40+ Expr variants
and threading the ABT-style binding scope through every IR-walker.

The L0 spike intentionally takes a **cheaper proxy**: hash each
Function's bytecode slice (post-compile, post-symbol-remap-to-global)
and survey duplicate ratios across all compiled CUs in a run.

**Bytecode-level hashing is a LOWER BOUND on cell dedup.** Two
alpha-equivalent functions WILL hash differently if their VarId
numbering differs (the compiler's local-slot assignment is
order-sensitive). The bytecode-survey number is the floor; the real
IR-level dedup ratio (post-ABT) can only be HIGHER.

This proxy choice is critical: it means the falsifier is **valid
without paying the L1 cost**. If even the lower bound fails the
threshold, there's no possible upper-bound result that would change
the decision.

Implementation: `include/v3/dedup_survey.hh` + `dedup_survey.cc` —
two files, ~180 LoC, gate `NIX_V3_DEDUP_SURVEY=1`, zero overhead
when unset. FNV-1a hash over the bytecode word stream.
`surveyCUBytecodeDedup(cu)` called from `primImport` after `compile()`
and from `runRootExpr` for the outer CU.

## 4. The measurements

Two independent workloads, both consistent:

### hello.drvPath (full eval, ~10 K LoC nixpkgs)

```
totalFunctions = 78 421
uniqueHashes   = 66 731     →  fn_dedup_lb   = 1.18×
totalBytes     = 5 057 KB
uniqueBytes    = 4 913 KB   →  byte_dedup_lb = 1.03×
```

### cardano-node M5 (~10× heavier, haskell.nix world)

```
totalFunctions = 528 813
uniqueHashes   = 450 181    →  fn_dedup_lb   = 1.17×
totalBytes     = 34 055 KB
uniqueBytes    = 31 934 KB  →  byte_dedup_lb = 1.07×
```

**Function-level lower bound: 1.17-1.18×.**
**Byte-level lower bound: 1.03-1.07×.**

The kill criterion was <2×. We are decisively below. The two
workloads agree, which rules out workload-specific quirks.

The byte-level number is the most important: even if 17% of
functions are duplicates, only 3-7% of *bytes* are duplicates. The
storage savings of a content-addressed cell store would be ~3-7%
cache footprint — a microoptimization at best, and one that comes
with substantial architectural complexity.

## 5. What this falsifies

**The per-thunk-body content-addressed cell store hypothesis is
falsified.** Whatever the "5 000+ replicated callPackage closures"
mentioned in `LESSONS_LEARNED §4.1` are, they do NOT manifest as
identical bytecode-bodies at the `ir::MkThunk` / `ir::MkClosure`
granularity.

Three possible explanations for the surface-level "5 000+ replication"
not translating to dedup:

1. **Replication differs at the bytecode-emit level.** Local-slot
   allocation order, embedded `SymbolId`s for the specific package
   name, captured-upvalue ordering — each unique even when the
   semantic shape is identical.
2. **The 5 000+ is at a different granularity than thunk-body.**
   Perhaps whole `ExprAttrs` or whole-file. Coarser-grained dedup
   might still work; finer-grained does not.
3. **Real dedup IS there at the alpha-equivalent IR level but the
   bytecode encoding loses it.** This is the case the cheap proxy
   conservatively under-measures. But even if true, the byte-level
   dedup is what counts for storage; alpha-equivalent IR-level
   hashing would only collapse function COUNT (the 1.17× → maybe
   higher), not the 1.03-1.07× byte storage win.

## 6. Consequences (work cancelled / demoted)

- **#773 (ABT alpha-equivalent identity refactor)** was the Stage 9
  Phase L1 prerequisite. **No longer Stage-9-justified.** The ABT
  refactor *is* still a Stage 5 prereq (hidden classes need stable
  identity for shape interning per `UNISON_IDEAS_2026-05-07.md` §2),
  so if Stage 5 happens later, that's the venue for the refactor.
  It does NOT need to land standalone.
- **L1, L2, L3, L4 cancelled.** ~4 weeks of calendar reclaimed.
- **Per-file disk cache (existing infra) is the architectural
  choice.** Stage 9's cell-level refactor would have been a deeper
  pivot; the existing `disk_cache.cc` per-CU model is sufficient.
- **The compile-vs-eval lane (#769)** — per-file compile/eval timing
  breakdown — is now the *only* caching strategy active. Per
  `#770-#770c` the disk cache is currently wall-clock neutral on
  hello.drvPath due to 1.78 ms/file deserialize cost. Either
  optimise deserialize (the probable next focus) or accept compile
  as a real share of eval cost.
- **#774 (Stage 4 v4.4 cross-fn strictness through higher-order
  callees)** is INDEPENDENT of Stage 9 and remains a viable
  next-week candidate; multi-day implementation.
- **ROADMAP_TO_VISION needs updating** — Stage 9 row removed from
  the stage list and Effort Summary; cross-references pruned;
  Candidate stages don't gain it back (it was committed, not
  candidate).
- **`LINKING_DESIGN_2026-05-17.md` should be marked SUPERSEDED**
  with a pointer to this memo.

## 7. The caveat (single sentence)

The measurement is a lower bound. If a future investigation wants to
falsify both bytecode-level (this measurement) AND IR-level (after
ABT refactor), it could pay the ABT cost and re-run. But given
byte-level dedup is 1.03-1.07× (essentially nothing to deduplicate),
even alpha-equivalent IR-level hashing would only collapse function
COUNT, not byte storage — and the storage win is what makes Stage 9
viable, so the IR-level recheck wouldn't change the decision.

## 7.5 Revival conditions (added 2026-05-23)

The kill criterion itself left a door open: "abandon Stage 9 and
**revisit at the whole-`ExprAttrs`-or-`ExprLet`-bindings level**."
That language is the revival condition for one of the three paths
below. Specific re-measurement triggers:

### 7.5.1 Trigger A — coarser-granularity dedup measurement

**Hypothesis**: per-thunk-body granularity didn't dedup; per-
`ExprAttrs` or per-`ExprLet`-bindings granularity might. The L0
spike was at the wrong level; the same workloads might show
substantially higher dedup at a coarser semantic boundary.

**Re-measurement**: extend `dedup_survey.cc` to hash whole
`ir::ExprAttrs` / `ir::ExprLet` body (not individual MkThunk slices).
Re-run on hello.drvPath + cardano-node M5.

**Trigger**: any future investigation into "cache more eval state
across runs" — specifically the materialization-retirement Phase 2
described in `IFD_DEEP_DIVE_2026-05-21.md` §11 includes a
content-addressed eval cache; coarser-granularity Stage 9 is
adjacent.

**Effort**: 2 days (modify dedup_survey.cc to traverse at ExprAttrs
level; re-run on the same two workloads).

**Decision rule**:
- If byte-dedup ≥ 2 × at coarser granularity: revive Stage 9 at
  ExprAttrs/ExprLet granularity. Cell schema design proceeds.
- If 1.5-2 ×: marginal; do a single-day implementation feasibility
  spike before committing.
- If < 1.5 ×: kill stands at all granularities measured.

### 7.5.2 Trigger B — post-ABT IR-level measurement

**Hypothesis**: bytecode-level was a lower bound because of compile-
order-sensitive VarId numbering. Alpha-equivalent IR-level hashing
(post-ABT refactor) might show substantially higher dedup, because
two semantically-identical functions that bytecode-hash differently
might IR-hash identically.

**Re-measurement**: after the ABT refactor lands (for any reason —
hash-keyed eval cache, effect propagation, or v3-lint Mode 2),
extend `dedup_survey.cc` to use the IR-level `structuralHash()` and
re-run.

**Trigger**: ABT refactor lands. As of 2026-05-23, ABT is dormant
(see §6); it might land later as a prereq for one of the live
non-perf consumers.

**Effort**: 1 day on top of the existing ABT refactor (just modify
the hash function in dedup_survey.cc to use the IR-level hash).

**Decision rule**:
- If IR-level byte-dedup ≥ 2 ×: same as Trigger A (revive Stage 9).
- If IR-level byte-dedup still < 2 ×: kill confirmed at both
  granularities; close this revival path permanently.

### 7.5.3 Trigger C — cross-process / cross-machine cache sharing

**Hypothesis**: the L0 spike measured WITHIN a single eval. A user
running many separate evals (CI fleet) might share substantial
bytecode across runs. Cross-process / cross-machine dedup is a
different architectural question and was not measured.

**Re-measurement**: this is actually orthogonal to Stage 9 as
designed (cell-level content-addressing within a single eval). It's
a closer fit to "Nix binary cache for eval results" — which is
Stage 7 from `IFD_DEEP_DIVE_2026-05-21.md` §11 (cache substituter
protocol).

**Trigger**: when materialization-retirement program (or any
similar "share eval state across processes") commits to substituter
work. **This isn't Stage 9 revival; it's a different stage. Listed
here for cross-reference only.**

**Effort**: not applicable to Stage 9 revival.

**Decision rule**: handle in the materialization-retirement /
substituter work, not here.

### 7.5.4 Probability assessment

| Trigger | Probability of firing | Probability of revival after firing |
|---|---|---|
| A (coarser-granularity) | Moderate (tied to materialization-retirement Phase 2) | Moderate — coarser granularity could plausibly show > 2 × dedup |
| B (post-ABT IR-level) | Low-Moderate (ABT lands only if non-perf consumers prioritise) | Low — byte-dedup at 1.03-1.07 × is so low that even structural alpha-equivalence is unlikely to push it past 2 × |
| C (cross-process) | Independent of Stage 9 | n/a (different stage entirely) |

**Honest read**: Stage 9 revival at thunk-body granularity is
permanently dead. Stage 9 revival at coarser granularity has a
plausible path through Trigger A if the materialization-retirement
program proceeds. The two are distinct enough that the coarser-
granularity work would arguably be a NEW stage, not Stage 9
revived.

## 8. Lessons (Rule 0 working as designed)

This is the cleanest example to date of the project's falsification
discipline producing the right outcome:

1. **The kill criterion was committed in advance** in the roadmap
   (line 431-433). Not added post-hoc to justify abandoning Stage 9
   — it existed before the spike was written.
2. **A cheap proxy made the spike affordable.** Bytecode-level
   hashing is ~1 day of work vs. ~1 week for full ABT-aware IR
   hashing. The proxy was chosen specifically because it gives a
   one-directional bound (lower) that's sufficient to kill the
   stage without needing the more expensive measurement.
3. **The data was measured on two independent workloads.** A single
   workload measurement could have been workload-specific; two
   independent workloads agreeing at 1.17× / 1.03-1.07× rules out
   noise.
4. **The decision was binary and immediate.** No "let's gate Stage 9
   behind an env var for now" — the stage is cancelled, the L1-L4
   work is taken off the calendar, the ABT refactor is demoted to
   Stage 5 prereq only.
5. **No code was written that needs to be unwound.** The L0 spike
   produced two small files (`dedup_survey.{hh,cc}`) plus three
   call-site additions. These stay as runtime instrumentation
   should anyone want to re-measure; they're zero-cost when the
   gate is off.

This is what the falsification rule (`feedback_falsification_rule.md`)
is *for*. Compare against a hypothetical where Stage 9 had been
attempted without an L0 spike — the team would have paid ~5 weeks
+ ~1 500 LoC + maintenance debt before discovering the underlying
hypothesis was wrong.

## 9. References

- Falsification rule: `feedback_falsification_rule.md`
  ("Every commit must answer: what hypothesis does this kill?")
- Original Stage 9 design: `LINKING_DESIGN_2026-05-17.md`
  (should be marked SUPERSEDED post this memo)
- Stage 9 kill criterion: `ROADMAP_TO_VISION_2026-05-15.md`
  Stage 9 §Kill criterion (lines 431-433)
- Triggering "5 000+ replicated closures" claim: `LESSONS_LEARNED_2026-05-15.md`
  §4.1
- ABT refactor (#773 — demoted to Stage 5 prereq): see
  `UNISON_IDEAS_2026-05-07.md` §2 (Unison Item 2)
- Sibling cache work (now sole caching strategy): `#769` per-import
  timing, `#770/770b/770c` disk-cache stats + skip-parse + falsifier
- L0 spike implementation: `src/libexpr-v3/dedup_survey.cc`,
  `include/v3/dedup_survey.hh`
- Commit hash: `37616ecc6` (2026-05-22 22:37)

---

Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
Input Output Group. SPDX-License-Identifier: Apache-2.0.
