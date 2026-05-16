# v3 Architectural Alignment Scorecard — 2026-05-15

This is a vision-vs-reality scorecard. Re-score quarterly to detect drift between stated architectural intent and what actually shipped. Companion to `ACTION_PLAN_2026-05-15.md` (immediate corrections) and `ROADMAP_TO_VISION_2026-05-15.md` (long-horizon path).

## Stated vision (one paragraph)

The v3 architecture target is an **STG-inspired, V8-influenced bytecode VM** for the Nix language. It reuses the cppnix parser. It has a custom generational GC (Cheney nursery with write barriers). It talks to nix-store and other external systems through a thin, well-defined FFI. The majority of evaluation lives in the pure bytecode VM — neither delegating to a tree-walker as a safety net nor reimplementing what cppnix already provides via its FFI surface.

Legend: ✅ delivered · ⚠️ partial · ❌ missing · 🔻 drifted away from vision

## Component scorecard

| # | Component                       | Vision                                                         | Current state                                                                                                                  | Status | Plan stage         |
|---|---------------------------------|----------------------------------------------------------------|--------------------------------------------------------------------------------------------------------------------------------|--------|--------------------|
| 1 | Parser reuse                    | Reuse cppnix AST verbatim                                      | `lower.cc` consumes cppnix's AST; no parser in v3                                                                              | ✅      | n/a (done)         |
| 2 | Bytecode VM core                | Clean opcode dispatch + IR                                     | Dispatch loop exists; embeds primops, bridge plumbing, diagnostics in one TU (vm.cc 9 755 LoC)                                 | ⚠️     | Action P4          |
| 3 | Optimizer pipeline              | Multi-pass IR optimizer                                        | 7 passes; `opt_occur.cc` exists but **not wired into `optimise()`**                                                            | ⚠️     | Action P0          |
| 4 | STG thunk states                | Black / Suspended / Evaluated                                  | Present in `closure.hh`; blackhole detection works                                                                              | ✅      | n/a (done)         |
| 5 | STG-shape uniformity            | All bindings lazy by default; strictness pass un-thunkifies    | Per-construct emit-time decisions in `lower.cc`; recurring eager/lazy asymmetry bugs (#496-#498-#516-#546-#548-#577)         | ⚠️     | Roadmap Stage 4    |
| 6 | V8 hidden classes / shapes      | Shape-tagged attrsets; same source → same shape                | ❌ not started                                                                                                                  | ❌      | Roadmap Stage 5    |
| 7 | Polymorphic Inline Caches       | Site-local shape/type cache at SELECT and CALL                 | ❌ not started                                                                                                                  | ❌      | Roadmap Stage 6    |
| 8 | Selector thunks                 | `inherit (a) b, c` shares the force of `a`                     | ❌ not started                                                                                                                  | ❌      | Roadmap Stage 7    |
| 9 | Generational GC                 | Cheney nursery, write-barrier, default-on                      | Phase A (allocator) + Phase C (scavenge) landed; **Phase D (write barriers) deferred**; default-OFF; closure-pool fills gap | ⚠️     | Roadmap Stage 3    |
| 10| Thin FFI to nix-store           | Narrow FFI surface for **system boundaries** (store, paths, IFD, file I/O, eval-state parse, shared symbol interning). Pure data ops (list/attrset/string/arith) stay **v3-native primops** by design — marshalling v3 Values ↔ Boehm-managed TW Values is too expensive and crosses GC ownership. | `ffi.cc` is 385 LoC ✓. Bridge plumbing for system boundaries (target keep + thin). Bridge plumbing for v3-can't-do-it escape hatches: `bridge_yield.cc` + ~370 LoC in vm.cc (target retire). primops.cc 8 336 LoC: a mix of correct v3-native + some duplication; needs audit, not blanket shrinking. | ⚠️     | Roadmap Stage 2+8  |
| 11| Pure bytecode evaluation        | v3-direct evaluates real workloads end-to-end                  | TW pre-eval default-ON (`NIX_V3_SKIP_INSTALLABLE_PREEVAL` is opt-out); v3-direct fails hello.name                              | 🔻     | Action P1-2 + R-S2 |
| 12| Bytecode disk cache             | Lowered bytecode cached on disk                                | `disk_cache.cc` + `serialize.cc`; works                                                                                        | ✅      | n/a (done)         |

## Drift items (changes away from vision)

Things that exist in the current codebase but are not in the stated architectural target. These need to be retired or absorbed back into a principled mechanism.

| #  | Drift                                            | Severity | Why it happened                                                          | Plan stage             |
|----|--------------------------------------------------|----------|--------------------------------------------------------------------------|------------------------|
| D1 | 169 env-var gates                                | High     | Bug investigations leave bisect/escape gates behind                      | Action P0 + P4         |
| D2 | vm.cc at 9 755 LoC, embedded subsystems          | High     | Dispatch + primops + bridge + diagnostics in one TU                      | Action P4              |
| D3 | Bridge layer load-bearing                        | Critical | v3-direct cannot complete real workloads without TW round-trips          | Roadmap Stage 2        |
| D4 | TW-bridge escape hatches in vm.cc + bridge_yield | Medium   | v3-can't-do-it fall-backs to TW for cycle handling, formals refusal, blackhole-as-value, etc. — these violate the V3-NATIVE constraint (`cf12c1880` retracted TW-routing as architectural mistake) | Roadmap Stage 2 |
| D4b| Opcode-body primop duplication                   | Low      | `primHead/Tail/Length/ElemAt` re-inlined in vm.cc OP_HEAD/TAIL/LENGTH/ELEM_AT — same logic in two places. NOT to be confused with v3-native primops in general, which are architecturally correct. | Roadmap Stage 8 (consolidation) |
| D5 | Closure-pool sentinel infrastructure (fakeClo)   | Medium   | Hand-rolled recycle pool exists because nursery is default-OFF; A5/A6 corruption proves the protocol is ill-defined | Roadmap Stage 3 |
| D6 | lode/ accreting 54 design docs                   | Low      | Investigation discipline — but pattern signals "understanding-through-archaeology" rather than maintained design | Action P4 |

## Orphan gaps (no current plan stage closes them)

Listed explicitly so they're not silently forgotten across quarterly re-scorings.

- **Direct threading / computed-goto opcode dispatch**. Optimizer doc estimates 10-15% on hot loops. Not on action plan or roadmap; would be Stage 6.5 if PIC perf measurements demand it.
- **Tagged pointers** (small-int unboxing, immediate booleans/null). Memory roadmap mentioned in May 2026; no current task. Defer to post-Roadmap-Stage-7 if needed for the final perf push.
- **Property-test framework for force/black/closure-update invariants.** Today's tests are unit + golden + shell scripts. A1-A12 / STG-cascade bugs likely would have been caught by randomised stress on the invariants. Not in any plan stage.
- **A formal specification document for the v3 bytecode** — file format, opcode semantics, IR invariants. Currently the spec is the implementation, which is fragile.
- **Cross-process or content-addressed bytecode cache sharing.** disk_cache.cc is per-user; nothing in the vision precludes sharing across users/CI/builders, but it's not on the plan.

## Quarterly re-evaluation protocol

Every 90 days, walk this scorecard top to bottom. For each row:
- Did its status (✅/⚠️/❌/🔻) change?
- Did the delta narrow, stay, or widen?
- Is the assigned plan stage still right? Slipped? Cancelled?
- For drift rows: did severity change?

Trigger a **full alignment review** (not just a phase review) if ANY of:
- ≥2 components moved to a worse status.
- ≥1 critical drift item got worse (severity grew).
- Plan-stage assignment slipped by >1 quarter for ≥2 components.
- Any orphan gap above remained orphaned for ≥2 consecutive quarters.

An alignment review re-asks: is the vision still right? Are stages still in the right order? Should a stage be cut or added?

## Honest verdict at this snapshot (2026-05-15)

- **Delivered**: 4 of 12 components (1, 4, 12, and most of 2's skeleton).
- **Partial**: 4 components (2, 3, 5, 9).
- **Missing**: 3 components (6, 7, 8).
- **Drifted**: 1 component in serious drift (11 — pure-bytecode eval), plus D-items D1, D2, D3, D4, D5, D6.

**Summary**: roughly **30-35% of the architectural vision is shipped**. The rest is either staged-in-design (nursery), explicitly-not-started (V8-style shapes/PICs/selectors), or actively drifting away (TW pre-eval dependency, TW-bridge escape hatches in vm.cc, env-var sprawl). Note: large primops.cc is **not drift** — v3-native primops are architecturally correct because marshalling v3 Values to/from Boehm-managed TW Values would be both expensive (per-call copy/wrap) and unsafe (crossing GC ownership). The FFI exists for system boundaries, not for replacing pure data primops.

The action plan addresses immediate correctness and ~half the drift (D1, D2, partial D3). The roadmap addresses the missing components (6, 7, 8) and the rest of the drift (D3 finish, D4, D5). Orphan gaps remain orphaned by design — re-evaluate at quarterly re-scoring.

## Cross-references

- Strategic principles + 5 immediate phases: `ACTION_PLAN_2026-05-15.md`
- Long-horizon stages 1-8: `ROADMAP_TO_VISION_2026-05-15.md`
- What worked / what didn't: `LESSONS_LEARNED_2026-05-15.md`
- Cleanup inventory inputs: `CLEANUP_AUDIT_2026-05-09.md`
- Cycle-handling architecture: `CELL_UPDATE_EVERYWHERE_2026-05-12.md`
- V3-NATIVE constraint origin: commit `cf12c1880` (Phase 3 memo retracting TW-routing as architectural mistake)
- Current bench floor: `bench/baselines/2026-05-11-post-phase4.json`
