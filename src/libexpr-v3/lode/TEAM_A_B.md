# Team A / Team B split — feasibility (2026-05-07)

> **ARCHIVED-IN-PLACE 2026-05-27**: This doc is historical (pre-strategic-doc-set or one-off RCA). Preserved here because it is still referenced from one or more KEEP docs. See [`LODE_CLEANUP_REVIEW_2026-05-27.md`](LODE_CLEANUP_REVIEW_2026-05-27.md) for the archival rationale.

---


Can two teams work in parallel on v3 — one finishing the FFI /
runtime / "move everything into the VM" track, the other on the
optimizer / IR / shape / hidden-class track? This report assesses
the pipeline's modularity, the existing test infrastructure, and
the coupling points that determine whether parallel operation is
safe.

**Bottom line:** yes, viable, after a ~2-week prep sprint. The
architecture is genuinely modular at the IR/bytecode boundary; the
prep work that's required to make a split safe is *independently*
worth doing.

---

## 1. Is the pipeline well-designed?

The shape:

```
Expr* (libnixexpr-parser) → lower.cc → IR → opt_*.cc → emit.cc → bytecode → vm.cc
                            ────────────────────────────────────  ─────────
                                       compile-time                 runtime
```

### Strengths

- **Clean phase separation.** Each stage has a single job. The IR
  is the universal hand-off; nothing reaches into adjacent stages.
- **A-normal form IR.** All operands are `VarId`s, no nested
  expressions. Exactly the right substrate for optimization passes
  — every pass is a flat block walk.
- **Plug-in optimizer.** Each pass is its own `opt_*.cc` file with
  an entry function (`constantFold(Module&)`, etc.). The pipeline
  driver in `opt_const_fold.cc:285–298` is a 12-line list. Adding,
  removing, or reordering passes is mechanical.
- **Disk cache downstream of optimization** (`disk_cache.cc` keys
  on the optimized bytecode + opcode-table fingerprint).
  Optimization changes invalidate cache cleanly.
- **Stable contract.** The opcode set + IR node kinds are the only
  two things both teams need to agree on. Both are explicit in
  headers (`bytecode.hh`, `ir.hh`).

### The accreting weakness

`lower.cc` is doing too much. 2227 lines, growing. Originally just
`Expr* → IR`; has accreted slot-capture (#458), intrinsic
recognition (#495), self-dot heuristic (#495 follow-on), env-shape
gymnastics (#455). It is becoming the codebase's fault line — two
of the last three review cycles bisected bugs to a `lower.cc`
line. **A split into `lower_basic.cc` + `lower_intrinsics.cc` +
`lower_recattr.cc` is defensible regardless of team-split
decisions.**

---

## 2. Per-pass IR testability

The infrastructure exists. `test/smoke.cc` already does it
correctly:

- Build an `ir::Module` by hand (constructor calls; no parser
  required).
- Invoke a single pass directly: `commonSubexprElim(m)`,
  `elimRedundantForce(m)`, etc.
- Inspect the resulting IR with C++ assertions.

This is exactly the V8 / GHC unit-test pattern. Recent commit
47ecd4ba8 added 4 strictness tests using this harness, bringing
`opt_strictness` to 6 tests.

### What's missing

| Gap | Effort | What it gives you |
|---|---|---|
| **IR pretty-printer** | ~1 day, ~200 LOC | Snapshot tests: "expected IR after pass X" as a textual fixture you can diff. Today you have to assert structurally with C++. |
| **Per-pass disable harness** | ~0.5 day | `NIX_V3_NO_OPT_<PASS>=1` for each pass individually. Currently only `NIX_V3_NO_OPT_STRICT` exists. Lets you bisect optimizer regressions in production code, not just unit tests. |
| **Standardised input/output IR fixtures** | ~2 days | A `tests/ir/` directory with `.ir.in` / `.ir.out` text pairs per pass. The optimizer becomes regression-testable like a real compiler unit. |
| **Pass-level perf microbench** | ~2 days | Hyperfine-style timings per pass on standard inputs (fib35, hello.name, cardano-node IR). Detects "this pass got 3× slower" before it ships. |

None are research; all are mechanical. Total ~1 week and the
optimizer becomes a properly testable compiler subsystem.

---

## 3. Two-team viability

Yes, with three preconditions. The work splits naturally along
the bytecode/IR contract.

### Team A — "move everything into v3"

**Owns:**

- `vm.cc` (dispatch loop, opcode handlers)
- `ffi.cc` / `include/v3/ffi.hh` (FFI surface)
- `primops.cc` (primop implementations)
- `v3_hook.cc` (TW bridge during transition)
- `bridge_yield.cc` / `fiber.cc` (re-entrancy)
- `disk_cache.cc` (cache implementation)

**Goals:**

- `applyClosure` end-to-end + first production caller
- Category C/D/E (Filesystem / Fetchers / Store) end-to-end
- `EvaluatorSettings` snapshot wired through
- Plugin ABI (Option B compat shim)
- Retire `primV3CallBridge1` fallback path
- Eliminate libnixexpr link-time dependency

### Team B — "optimizer / IR"

**Owns:**

- `opt_*.cc` (all 6 passes + new ones)
- `ir.cc` / `include/v3/ir.hh` (IR data structures, free-vars)
- `lower_basic.cc` (after split) — AST → IR shape
- `emit.cc` lowering choices (which opcodes to emit, which fast
  paths)
- Future analyses: occurrence, demand, cardinality, escape
- Future passes: hidden-class shapes, selector thunks,
  CLOSUREREC, content-addressed IR

**Goals:**

- Foundational analyses (occurrence + use-def map)
- Hidden-class shapes for attrsets (S1 — biggest single win)
- Selector thunks (GHC-style projection short-circuit)
- Demand analysis + worker/wrapper
- Content-addressed IR fragments (Unison-inspired)
- Computed-goto dispatch (mechanical)
- Typed numeric opcodes (mechanical)

### Read-only / shared

Both teams **read** `bytecode.hh` and `ir.hh`. Both can **propose**
additions via shared review. Neither team alone should renumber
opcodes or change IR node kinds.

---

## 4. Coupling points

Three places where the teams will meet:

### Coupling 1: `lower.cc`

Both teams touch it. Team A adds AST patterns for new bridge
primops; Team B adds AST patterns for shape-aware or
intrinsic-aware lowering. Conflict-prone — ~40% of lowering
changes in the last three weeks were ambiguous.

**Mitigation:** split it (§1). After the split:
- `lower_basic.cc` — language-shape lowering (literals, lambdas,
  let, with, if, attrsets, lists). Team B owns.
- `lower_intrinsics.cc` — pattern-matching for `lib.fix`,
  `extends`, `mapAttrs`, etc. Team B owns; Team A consults.
- `lower_recattr.cc` — slot-capture, RecBuildSlot, env-shape
  invariants. Team A owns (it's where #455-class bugs live and
  the FFI work needs to keep it healthy).

### Coupling 2: `bytecode.hh` / new opcodes

When Team B adds `OP_ATTRS_SELECT_BY_SHAPE` and Team A adds
`OP_FFI_INVOKE`, opcode-number conflicts are a merge-resolve
nuisance. More importantly, **dispatcher shape changes**
(computed-goto rollout, opcode reorganisation) need cross-team
awareness.

**Mitigation:** reserve opcode ranges, with one shared
`bytecode.hh` PR window per week.

```
0x00–0x3F   core (literals, control flow, locals, frames)   shared
0x40–0x6F   IR-emit opcodes (force, attrs, list, primop)    Team B owns
0x70–0x7F   bridge & FFI                                    Team A owns
0x80–0x8F   FFI / closure callback (future)                 Team A reserved
0x90–0x9F   shapes / hidden-class IC (future)               Team B reserved
0xA0–0xBF   superinstructions / fast paths                  Team B owns
0xC0–0xDF   type-test / list-op fast paths (existing)       Team B owns
0xE0–0xEF   numeric typed opcodes (future)                  Team B reserved
0xF0–0xFE   reserved
0xFF        OP_HALT
```

This isn't binding; it's a coordination convention to surface
collisions at PR-review time, not at merge time.

### Coupling 3: `primops.cc` and `PrimOpFlags`

Team A wants to migrate primops out of TW; Team B wants to add
`lazyArgs` / purity flags so the optimizer can fuse and memoise
them.

**Mitigation:** Team A owns the primop registry; Team B proposes
flag additions via review. Flags are a small fixed enum
(`include/v3/primop.hh`); coordination is per-flag, not
per-primop.

---

## 5. Prep sprint — what must exist before splitting

A ~2-week sprint, doable by either team or jointly:

| Item | Cost | Owner | Why |
|---|---|---|---|
| **Per-pass test harness** (`NIX_V3_NO_OPT_<PASS>=1` + smoke fixtures) | ~1 week | Team B | Without it, optimizer regressions surface as "v3 is broken" with no clue who broke it |
| **CI bench attribution** | ~2 days | Either | Hyperfine on every PR, per-commit attribution; without it, perf regressions become whodunits |
| **Split `lower.cc`** | ~3 days | Joint | Single biggest collision target; reduces merge friction by an order of magnitude |
| **Reserved opcode ranges in `bytecode.hh`** | ~30 min | Joint | Avoids merge friction on opcode numbering |
| **Documented IR contract** | ~1 day, ~300 lines | Team B | "What does each IR node mean?" Currently lives partly in `ir.hh` comments, partly in oral tradition |
| **IR pretty-printer + dump-after-pass tooling** | ~1 day | Team B | Required for snapshot tests and bisecting optimizer regressions |

Total: 8–10 working days, independent value.

---

## 6. After the prep

### Team B's independent runway (6–12 weeks)

The optimizer team's wins **don't depend** on FFI completion:

1. Occurrence analysis — foundational, ~3 days
2. Computed-goto dispatch — mechanical, ~1 day
3. Typed numeric opcodes (`OP_ADD_II` etc.) — ~1 day
4. Selector thunks — ~3 days, depends on occurrence
5. Hidden-class shapes — ~1–2 weeks, blocked on C2
6. Demand analysis + worker/wrapper — ~2 weeks, depends on
   occurrence
7. Content-addressed IR fragments — ~1–2 weeks, depends on ABT
   refactor

Expected cumulative win: ~25–40 % on cardano-node-class workloads,
front-loaded by the mechanical wins (computed-goto, typed
numerics).

### Team A's independent runway (6–12 weeks)

The FFI team's wins **don't depend** on the optimizer:

1. `applyClosure` end-to-end with one production caller — half
   week
2. Category C (Filesystem I/O) — 2 days
3. `EvaluatorSettings` wiring — 2 days
4. C2 root-cause fix — ~1 week (also unblocks Team B's shapes)
5. Retire `primV3CallBridge1` — ~3 days (after applyClosure)
6. Plugin ABI Option B implementation — ~1 week
7. `libnixexpr` link-time dependency removal — ~2 weeks
   (long-tail cleanup after categories C/D/E land)

Expected outcome: v3 owns evaluation; TW becomes optional /
deletable; the FFI surface is real, not decorative.

### Where they meet

- **C2 fix.** Team A finishes it (it's a `lower_recattr.cc` /
  upvalue-capture bug); Team B's shapes work consumes the fix.
- **Opcode additions.** Coordinated weekly via the reserved-range
  convention.
- **Primop flag changes.** Reviewed jointly when added.

These are normal cross-team surfaces; nothing more aggressive
than what any two-team C++ project handles every week.

---

## 7. Risk assessment

### Likely failure modes

- **Test-harness gap masks silent breakage.** If Team B ships an
  optimizer change that emits subtly different bytecode, and
  Team A simultaneously refactors a dispatch handler, the
  resulting test failure could be misattributed for days.
  Mitigation: per-pass disable harness (in prep sprint); CI
  bench attribution (in prep sprint).
- **`lower.cc` merge conflicts.** Mitigation: split before the
  parallel work starts.
- **Drift on `PrimOpFlags` semantics.** Mitigation: written
  contract for what each flag means; one team owns the
  registry.

### Unlikely failure modes (don't worry about these)

- **Opcode set instability** — the disk cache fingerprint
  catches renumbering. Cache invalidates; nothing wedges.
- **IR shape changes mid-flight** — Team B controls IR; Team A
  doesn't read IR.
- **GC integration surprises** — current Boehm-based arena is
  stable; both teams use it the same way.

### When to abort the split

If after 4 weeks of parallel work either of these is true:

- Average PR review cycle for cross-team-touching files
  (`lower.cc`, `bytecode.hh`, `primops.cc`) exceeds 2 days.
- More than 1 perf or correctness regression per week is
  attributable to "the other team's change."

Then the modularity isn't yet what's claimed; merge teams back
and finish the prep work.

---

## 8. Recommendation

**Run the prep sprint now (2 weeks).** It serves both teams
whether the split happens or not. Every prep item is also on
the optimizer's "should do" list from prior reviews
(`OPTIMIZER_REPORT_2026-05-07.md` has 4 of 5 in its "concrete
next sprint").

**After prep, split for a 6-week trial.** Two parallel teams,
review cycles ≤2 days, weekly bytecode-and-flag sync. At
6 weeks, evaluate: are both teams shipping? Are regressions
attributable? Is `lower.cc` healthy?

If yes, continue. If no, merge back; prep work was still worth
doing.

**Don't split before prep.** Current `lower.cc` is a single
chokepoint and the test infrastructure is too thin to catch
silent breakage from a parallel-team mistake. The 2-week prep
is the cost of admission to safe parallel operation.

---

## Appendix: file ownership table

| File | Team | Read or write |
|---|---|---|
| `vm.cc` | A | write |
| `ffi.cc` / `ffi.hh` | A | write |
| `primops.cc` | A | write |
| `v3_hook.cc` | A | write |
| `bridge_yield.cc` / `fiber.cc` | A | write |
| `disk_cache.cc` | A | write |
| `serialize.cc` | A | write |
| `lower_recattr.cc` (post-split) | A | write |
| `opt_*.cc` (all 6 + new) | B | write |
| `ir.cc` / `ir.hh` | B | write |
| `lower_basic.cc` (post-split) | B | write |
| `lower_intrinsics.cc` (post-split) | B | write |
| `emit.cc` | B | write |
| `disasm.cc` | B | write |
| `bytecode.hh` | shared | weekly sync |
| `include/v3/primop.hh` | A primary | flag changes via review |
| `include/v3/value.hh` | shared | rare changes; both review |
| `include/v3/closure.hh` | A primary | rare changes |
| `test/smoke.cc` | B | write (IR/optimizer tests) |
| `test/run-*.sh` | shared | by topic |

Anything not listed is either niche or shared by default.
