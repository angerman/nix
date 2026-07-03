# BEAT_TW v3 plan — 2026-07-03 — the capture-model trial

**Status: ACTIVE. Decision (user, 2026-07-03): take the big risk — trial the
capture-model change (env-pointer capture). This document is the execution
handoff for the agent/engineer running it.**

**Mission.** Produce RELIABLE DATA on whether inverting v3's capture polarity
(flat per-object capture copies → TW-style shared environment pointers) can
close the CPU+RSS gap to the tree-walker — and ship everything that must land
first for that data to be trustworthy. A decisive KILL with clean data is a
successful outcome of this plan; a silent maybe is not.

**Read first (process contract):** `src/libexpr-v3/CLAUDE.md` — Rule 0, the
full `--brute` pre-merge gate, darwin-4 measurement protocol, Phase-D/moving-GC
constraint #0, no-new-gate-without-retirement-criterion. This plan does not
repeat it, only cross-references. Companion analyses:
`lode/DEFECT_REVIEW_2026-07-03.md` (esp. §5 architecture spine, §2.1/§2.2,
§3.1/§3.2), `lode/DEFECT_AUDIT_2026-07-02.md` (+handbacks),
`lode/WHY_CPU_SLOWER_THAN_TW_2026-06-28.md` (opcode histogram).

---

## 1. Scoreboard and definitions of victory

Authoritative baseline (P0.4 re-baseline, darwin-4, commit-stamped in
`bench/baselines/darwin4-rows.tsv`):

| Workload | COLD CPU | COLD RSS | WARM CPU | WARM RSS |
|---|---|---|---|---|
| firefox.drvPath | 3.62× TW (2.68 s) | 1.89× (677 MB) | 2.43× (1.80 s) | 1.64× (586 MB) |
| cardano M5 | 3.04× (10.96 s) | 3.00× (2950 MB) | 1.81× (6.54 s) | 2.26× (2217 MB) |

Profile facts every claim must reconcile with: ~52 % of executed ops are
trivial stack ops; OP_GET_UPVALUE 13-17 % of ops; OP_MAKE_THUNK 7-8 %;
DISPATCH 7-23 % on-CPU; ALLOC ~20 %; thunks 62-67 % never forced; Bindings
≈84 % of arena; arena never returns pages.

**Pre-committed victory definition (program level):** warm firefox AND M5 on
darwin-4, byte-identical drvPaths, **CPU < 1.0× TW committed target; RSS
< 1.3× committed, < 1.0× stretch.** Every phase below re-measures against
this table; a track that doesn't move its number retires per Rule 0.

**Why the capture model is the bet** (DEFECT_REVIEW §5, both codebases read):
TW closures cost 0 bytes (`mkLambda` = two stores) and thunks 16 B, because
both store one pointer to a per-scope Env shared by every lambda/thunk in the
scope. v3 copies every capture into every object (24-40 B headers + 8 B/upval
+ one *dispatched push per capture per creation*), paying at creation in a
language where 62-67 % of created thunks are never touched again. This single
polarity inversion feeds: the 13-17 % GET_UPVALUE share (largely
capture-forwarding, not body reads), a large slice of the 52 % trivial ops,
the ALLOC ~20 % bucket, and the 2-3× per-object byte ratio (thunk 32-64 B vs
TW 16 B). No other unfalsified lever touches CPU and RSS at the root
simultaneously. It is also the riskiest change in the codebase — hence the
gated-trial structure below.

**Decision tree (pre-committed):**
- Gate A (counters, §3) fails → the polarity hypothesis dies for ~2 days'
  work; fall back to the capture-trailer (DEFECT_REVIEW Q2.1) as the cheap
  flat-capture optimization, and re-rank JIT as the primary CPU track.
- Gate A passes → build v1 (§5); Gate B = correctness; Gate C = perf with
  pre-committed ship/kill thresholds.
- Gate C SHIP → fund v2 (full conversion) + re-base the JIT plan on the new
  ISA. Gate C KILL → document, retire the gate, pivot to trailer + JIT.

---

## 2. Phase 0 — prerequisites (land BEFORE any capture work; ~1-2 weeks)

These are not busywork: each either touches machinery the trial edits, or is
required for the trial's data to be valid. All are specified in
DEFECT_REVIEW_2026-07-03 §1/§6 (Q-numbers below refer to its matrix). Every
item: failing-first test where applicable, full `--brute` before commit.

### P0.A — correctness fixes in the blast radius (order matters)
1. **Q1.4 cache-key fingerprint + real lint** (`primops.cc` kGates +=
   `NIX_V3_RAW_FORMALS`, `NIX_V3_NO_NONREC_ATTRS_INIT`; extend
   `test/lint-cache-coherence.sh` to diff all `getenv("NIX_V3_*")` reads in
   emit.cc/opt_*.cc/lower_v3.hh/ir.cc against kGates, allowlisting
   dump-only gates). **MANDATORY FIRST**: this trial adds a new
   codegen-changing gate (`NIX_V3_ENV_CAPTURE`), and without the lint the
   same stale-bytecode hole recurs and silently invalidates every A/B.
2. **Q1.1 withLookup dangling-reference fix** (write back by index,
   `vm.cc:2455-2456`; withStack reserve is 64 — the trial exercises with/env
   machinery under stress, so this latent UAF must die first). ASan + a
   >64-with fixture.
3. **Q1.2 valueLess `cellWrite` barrier** (`vm.cc:1295-1303`) and **Q1.3
   replaceStrings force-check-store order** (`primops.cc:2525-2528`) —
   PhD-6-class; the trial's brute runs must not chase pre-existing flakes.
4. **Env-walker completion (from DEFECT_REVIEW §1.9 — becomes LOAD-BEARING
   here):** `Env::parent` is currently walked by NO walker (scavenger
   `gc.cc:680-687` walkEnv, mark `mark_sweep.cc:637-649/690-702`, auditor
   `gc.cc:1264-1271` all walk `values[]` only) because nothing populates it
   yet. **This trial populates it.** Add parent-chain walking to all three +
   the auditor BEFORE any Env chain exists, with a synthetic-chain unit test
   (build a 3-deep parent chain holding nursery payloads under 1 MB nursery
   + AUDIT; must be clean). Also add `envPostConstructBarrier` at the
   env-intern creation site (`vm.cc:660-665`) — one line, closes the
   transitively-covered-only hazard.
5. **GcRoot-at-scavenge assert** (Q1.8): assert no live `GcRoot` registry
   entries when a moving scavenge fires. Cheap, definitive; the trial changes
   allocation patterns and must not silently open the empty-frame window.
6. **Q1.6 S1.2 rooting backlog** (primAll/primAny/primElem/primRemoveAttrs/
   primConcatStringsSep/primReplaceStrings/primListToAttrs-src/primSort
   mid-sort + primCatAttrs `kept` vector — sites listed in DEFECT_REVIEW
   §1.7). Not strictly blocking v1 (scavenge gating unchanged), but cheap,
   mechanical, and blocking for any later safepoint work — do them while the
   brute battery is hot.

### P0.B — quick CPU wins that touch the same code regions (bundle)
- **Delete the TEMP P2.1-a formals probe** (`vm.cc:6933-6943` + TAIL_CALL
  twin) — pure deletion on the OP_CALL region the trial edits.
- **Q2.5 attrsUsed-style O(1) extra-arg check** (replaces the per-call
  O(m·log f) `forEach` scan; the missing-arg loop already probes each formal
  — count presence there, compare sizes).
- **Q2.4 OP_CALL_N inline Slot→Closure hop** (`vm.cc:7819-7820`).
- **Q2.6 groupBy → C default** + NoCtx key fix (stale-justification flip,
  DEFECT_REVIEW §2.6) and **Q2.9 single path-coercion in STR_CONCAT**
  (`vm.cc:12251-12264`) — independent, safe, do opportunistically.

### P0.C — env-sharing disposition (decide, don't drift)
DEFECT_REVIEW §3.1: the interning heuristic (`vm.cc:598` `nUp <= 8 →
UINT32_MAX`) never fires; the feature shares ~nothing. **Decision for this
plan: RETIRE the interning heuristic, KEEP the plumbing.** The `Env` struct,
`upvalEnv` field, `closureUpvalue()` accessor, walkEnv, and the ENV_SHARED
thunk flag are exactly the infrastructure v1 reuses — do NOT delete them.
Remove only the `maybeInternUpvalueEnvFromStack` call sites + intern table
(or hard-gate them off) so the trial starts from a clean, single-mechanism
baseline. Run the nUp histogram (§3 counter 2) BEFORE retiring, since it
doubles as the sharing-rate falsifier the feature never got. `--brute` +
byte-id after.

**Phase 0 exit:** brute green (expect 28+/28+), new baseline row on darwin-4
(cold+warm, firefox+M5) — this is the row Gate C compares against, NOT the
2026-07-02 row (P0.B moves it slightly).

---

## 3. Phase 1 — step-0 instrumentation and Gate A (~2 days)

All counters are deterministic (host-independent); run cache-off (descriptor
flags are NOT serialized — warm runs zero flag-based instruments; DEFECT_REVIEW
§4.1). Add under `NIX_VM_STATS`, dump in the existing stats block. Workloads:
firefox.drvPath + M5 + git.drvPath.

1. **Capture-op share** (emit-time + runtime): count GET_LOCAL/GET_UPVALUE
   ops emitted inside capture sequences (the `emitVarRef` loops at
   `emit.cc:1091-1092/1106-1107`) vs body reads; runtime-weight by executing
   them under a per-op counter or by static count × descriptor allocCount.
   → sizes the dispatch win.
2. **nUp histogram + sibling density**: distribution of nUp at
   MAKE_THUNK/MAKE_CLOSURE; and per (function, frame-entry): how many
   MkThunk/Lambda creations execute per frame that has ≥1 escaping local
   (see §5.3 for "escaping"). → sizes the sharing win: the Env model wins
   bytes when siblings-per-frame ≥ 2 or nUp ≥ 2, loses when a lone thunk
   captures 1 var (Env 24 B vs inline 8 B).
3. **Forwarding-capture share**: fraction of captures whose source is itself
   an upvalue of the creating frame (pure forwarding). Cheap at emit time
   (capture's VarId resolves to upvalue, not local). → sizes the
   transitive-re-copy waste the Env chain eliminates entirely.
4. **Single-assignment check (static)**: % of escaping locals whose slot has
   exactly one reaching store before every capturing MAKE (see §5.6 trap 1).
   → sizes v1 eligibility coverage.

**GATE A (pre-committed):** BUILD v1 if
(counter 1 ≥ 8 % of executed ops) OR (counter 3 ≥ 30 % of captures AND
counter 2 sibling-density ≥ 1.5 avg). CLOSE the polarity bet if counter 1
< 4 % AND counter 3 < 15 % (fall back to the Q2.1 capture-trailer, which the
same counter 1 also gates: ≥ 8 % → build trailer instead). Between: build v1
anyway but downgrade Gate C expectations proportionally — record the
projection formula used.
Also compute and RECORD the **model projection**: predicted CPU win =
(counter-1 share × dispatch cost fraction) + alloc-bytes delta × ALLOC share;
predicted RSS win = (thunk/closure capture bytes − Env bytes) at measured
densities. Gate C judges realized-vs-projected, so write the projection down
BEFORE building.

---

## 4. Phase 2 — program overview (context for where the trial sits)

Four tracks; this plan executes Track E (the trial) and defers the rest:

- **Track A — JIT** (4-6 wk minimal viable; J0-J3 proven, J3 needs no stack
  maps — `reference_jit_and_workload_profiling_2026-06-29`). **Deliberately
  sequenced AFTER the capture decision**: v1 changes MAKE_THUNK/GET_UPVALUE —
  the exact opcodes a JIT templates. Do not start Track A until Gate C
  resolves.
- **Track B — S2.1 safepoint completion → mid-eval GC** (P0.A items 4-6 are
  its preconditions). Deferred; interacts with v1 only through the walkers
  Phase 0 fixed.
- **Track C — representation diets** (Closure 40→16 B Q3.2, Bindings 16 B
  header Q3.4, refill tail counter Q3.3, fused UPDATE_INIT Q3.5). Mechanical,
  parallelizable by a second agent, NO file overlap with v1 except
  closure.hh — coordinate: v1 owns closure.hh until Gate C.
- **Track E — THE CAPTURE-MODEL TRIAL** (§5). Owns: lower_v3.hh, ir.hh/ir.cc,
  emit.cc, vm.cc (frames/MAKE/GET/SET paths), closure.hh, gc.cc/mark_sweep.cc
  (Env walkers), serialize.cc.

---

## 5. Phase 2 Track E — the capture-model trial (v1), in full

### 5.1 The idea, stated precisely

Today (flat capture): `MkThunk`/`Lambda` IR nodes carry transitive `freeVars`;
emit pushes each capture (one dispatched GET per capture,
`emit.cc:1091-1092/1106-1107`); OP_MAKE_* pops them into a per-object FAM
(`vm.cc:5912`-region pop loop). A var used K levels deep is re-pushed and
re-copied at every intermediate level. Reads are O(1):
`upvalues[i]`/`env->values[i]`.

v1 (env-pointer capture, gated `NIX_V3_ENV_CAPTURE`, default-off): a function
whose locals are captured by inner objects allocates ONE heap `Env` per frame
entry holding exactly its *escaping* locals; inner MkThunk/Lambda capture a
single pointer to that Env (whose `parent` links to the creating frame's own
defEnv, mirroring lexical nesting). Reads become `OP_GET_ENV(depth, idx)` —
walk `parent` depth times, index once. Creation cost per object collapses
from (nUp pushes + nUp copies + FAM bytes) to (1 pointer store); per-frame
cost rises by one Env alloc (lazy, only in frames that create escaping
objects).

**Semantic argument (why byte-identity is expected):** captures are 8-byte
NaN-boxed `Value` words; copying the word vs reading it through an Env yields
the SAME word aliasing the same heap cells — sharing/memoization semantics
are unchanged at the cell level. Laziness is unchanged: v1 changes WHERE a
free-var word is read from, never WHEN anything is forced or selected. The
design-b/module-system falsification (a7948a2f3 / task #33: eager entry-time
`AttrSelect` through a mapAttrs param broke the config fix-point) does NOT
apply — no select or force moves. §5.6 trap 1 is the one real semantic
precondition (single-assignment), and it is checked statically per function.

### 5.2 v1 scope — deliberately partial (per-function opt-in, like OP_RAW_FORMAL)

IN: functions (lambda bodies and thunk bodies) where the emit-time analysis
proves eligibility (§5.3); their MkThunk/Lambda children capture
`(defEnv-pointer, [residual flat captures])` — a HYBRID: captures the analysis
can't route through the env chain stay flat in the existing FAM. This keeps
the fallback path byte-for-byte the current mechanism and makes eligibility a
per-capture decision, not per-function all-or-nothing.

OUT (v1 — do not scope-creep): migrating `with`-machinery into the env chain
(capturedWiths stays EXACTLY as-is — it is orthogonal: a thunk can hold
defEnv AND capturedWiths); letrec/rec-slot machinery (RecBindingSlotRef,
Slot tags) unchanged; formals wrappers unchanged; nursery-eligibility for
Envs (v1 Envs are tenured-direct like today's interned Envs — simpler GC
story; revisit in v2); any JIT work; any locals-representation change for
NON-escaping locals (they stay on the valueStack).

### 5.3 Emit-time analysis ("escape analysis lite", new pass or lower_v3 phase)

Per function F:
1. **Escaping-local set E(F)**: locals of F referenced in the `freeVars` of
   any transitive child MkThunk/Lambda.
2. **Single-assignment proof** per l ∈ E(F): every capturing MAKE site is
   dominated by exactly one store to l's slot, and no store to l follows any
   capturing MAKE on any path (the lowerer is one-slot-per-binding —
   `preassignSlotsInBlock`, no liveness reuse — so bindings pass trivially;
   emitter TEMP slots from the defer machinery FAIL and stay flat).
3. **Slot assignment**: eligible escaping locals get Env indices; F's header
   gets `envSlotCount` + a mapping; body references to them lower to
   OP_GET_ENV(0, idx) after the binding store (which becomes OP_SET_ENV).
4. **Child capture rewrite**: a child capture of an eligible escaping local →
   route via defEnv (depth d relative to child); a capture that is pure
   FORWARDING of an ancestor's env-routed var → also env-routed (depth+1) —
   this is where counter 3's waste dies; anything else stays in `freeVars`
   (flat FAM).
5. Function metadata: `usesDefEnv` bit; `envDepthMax` (for the reader);
   serialize additions (§5.5).

### 5.4 Runtime changes (vm.cc / closure.hh)

- **New opcodes** (operands are plain ints — NO SymbolId, avoiding the P3.3
  remap footgun by construction): `OP_MAKE_ENV n` (allocate frame Env, parent
  = current defEnv, store in frame register — emit LAZILY before the first
  escaping store, not at entry, so non-creating executions of the frame pay
  nothing); `OP_SET_ENV idx`; `OP_GET_ENV depth idx` (depth==0 fast case
  first); MAKE_THUNK/MAKE_CLOSURE gain a `usesDefEnv` flag path storing the
  frame's Env pointer (reuse the existing `upvalEnv` field on Closure and the
  ENV_SHARED tail-slot mechanism on Thunk — that is why P0.C keeps the
  plumbing).
- **Frame entry**: a called closure/forced thunk with `usesDefEnv` installs
  its stored Env as the frame's parent-env register (one store). fakeClo
  paths (`vm.cc:13330`-region) must propagate it — audit all three fakeClo
  fill sites.
- **Writebacks**: OP_SET_ENV and any in-place force writeback targeting an
  env slot MUST use `cellWrite` (tenured Env ← possibly-nursery payload;
  PhD-6). Count these — if writeback-into-env turns out hot, that is a
  Gate C data point, not a reason to skip the barrier.
- **GC**: walkEnv/mark/auditor already extended for `parent` in P0.A-4.
  `envPostConstructBarrier` after OP_MAKE_ENV. Envs are tenured: creating
  them does NOT need dirty-tracking beyond the post-construct scan + barriered
  writes.

### 5.5 Serialize / disk cache

New opcodes: add to the opcode-table FNV fingerprint (automatic invalidation),
teach the four serialize walkers + CU comparator + disasm the operand shapes
(the P3.3 adversarial lesson: EVERY walker that skips operands by count must
learn the new lengths — grep for the OP_MAKE_THUNK trailer handling and mirror
it). Descriptor additions (`envSlotCount`, `usesDefEnv`) are
LambdaDescriptor-schema changes → `kSchemaVersion` bump (the lint's Rule 1).
**Add `NIX_V3_ENV_CAPTURE` to kGates in the same commit that adds the gate**
(P0.A-1's lint now enforces this). Retirement criterion at the getenv site:
"retire (default-on or delete) at Gate C resolution, 2026-Q3".

### 5.6 Traps checklist (each has bitten this codebase before — verify explicitly)

1. **Mutation-after-capture** (§5.3-2): a captured env slot written after a
   MAKE changes what the child sees (flat capture snapshots; env capture
   doesn't). The static proof is the guard; add a debug-mode runtime check
   (store-after-capture poisons) under `V3_DBG_ENV_CAPTURE_AUDIT` for the
   brute battery.
2. **PhD-6 everywhere an Env is written** — barrier + walker coverage
   (P0.A-4 tests). Brute is the detector (1 MB nursery + AUDIT + BRUTE).
3. **Env lifetime vs frame lifetime**: the Env outlives the frame (that's the
   point) — nothing may store frame-relative pointers (valueStack addresses,
   `Value*` slots) INTO the Env; only 8 B Values. Audit OP_SET_ENV sources
   for Tag::Slot values pointing into the valueStack (rec-slot machinery) —
   those captures are INELIGIBLE in v1 (keep flat).
4. **module-system fix-point**: expected untouched (no force/select moves),
   but the falsifier is mandatory anyway: hello/git/firefox drvPath byte-id
   gate-on FIRST, before any perf run (design-b died here; 20 minutes of
   validation prevents a week of confusion).
5. **Instrument blindness on warm runs**: all new counters/flags are
   cache-off-only (flags not serialized). Never draw conclusions from a warm
   arm's counters.
6. **frames.reserve/withStack.reserve class**: OP_MAKE_ENV must not hold
   references into reallocatable vectors across the alloc (the withLookup
   lesson, Q1.1).
7. **Do not touch** `alloc.hh g_majorGcEnabled` (M-3 trap), Step-7 defaults,
   or scavenge gating — v1 changes none of the GC policy.
8. **Deserialized-CU parity**: a warm-loaded CU with `usesDefEnv` must behave
   identically — round-trip test (compile, serialize, deserialize in a fresh
   process, byte-compare eval) goes into the brute battery like
   `run-attrs-init-cache-roundtrip-tests.sh`.

### 5.7 Work packets (order; ~3-4 weeks total for v1)

- **W0 (0.5 d)**: gate + kGates + lint + retirement comment; opcode stubs +
  disasm + serialize walkers (no emission yet); brute green (no-op).
- **W1 (2-3 d)**: emit-time analysis (§5.3) + counters 1-4 wired to it;
  DUMP-ONLY mode (`NIX_V3_ENV_CAPTURE=dump`) that reports eligibility
  coverage per workload without changing codegen. Gate A numbers come from
  here if Phase 1 used estimates.
- **W2 (3-5 d)**: emission + runtime opcodes; lang suite (143) green gate-on;
  IR-CHECK fixtures for the new lowering shapes (`--file %s` convention,
  positive + negative: ineligible-mutation case stays flat).
- **W3 (2-3 d)**: GC integration tests (synthetic chains, 1 MB nursery brute,
  V3_DBG_ENV_CAPTURE_AUDIT); fakeClo path audit; deserialize round-trip test.
- **W4 (1-2 d)**: byte-identity ladder gate-on vs off: hello → git → firefox
  → python3 (the #696 PAP class) drvPaths; then full `--brute` gate-on AND
  gate-off. **GATE B: all byte-identical, brute 28+/28+ both settings.**
- **W5 (1-2 d)**: darwin-4 A/B, cache-off AND warm, firefox + M5 + git,
  median-of-5, both directions (on-vs-off), plus the deterministic counters
  (thunk bytes allocated, capture ops executed, Env allocs, env writeback
  barrier count). Git-note + darwin4-rows.tsv per protocol.
- **W6 (0.5 d)**: handback section appended to THIS file: realized vs the §3
  projection, per-counter attribution, Gate C verdict.

### 5.8 GATE C — pre-committed ship/kill (write the verdict, whatever it is)

Let P = the recorded Phase-1 projection for v1's eligible slice.

- **SHIP (fund v2)** if: realized CPU win ≥ max(3 %, 0.6·P_cpu) on firefox
  or M5 cache-off, AND RSS not worse by > 1 %, AND full-coverage model
  (v2: with-chain + rec + higher eligibility) projects ≥ 15 % CPU or ≥ 20 %
  RSS. → v2 = extend eligibility (defer-temp SSA repair, rec-slot handling,
  with-chain migration killing capturedWiths, Env nursery-eligibility), then
  re-base Track A (JIT) on the new ISA.
- **KILL** if realized < max(1.5 %, 0.3·P_cpu) AND RSS flat: the polarity
  mechanism does not deliver even where fully applicable → retire the gate
  (delete, preserving the branch point in git per P3.1 precedent), write the
  falsification, pivot to capture-trailer + Track A. This outcome is
  *reliable data* — the plan succeeded.
- **Between**: one bounded iteration on the top counter-identified residual
  (e.g. Env-alloc overhead → lazy-alloc refinement; writeback barriers →
  measure share), then re-run W5 once. No second iteration without new
  mechanism evidence (the anti-spiral rule from the GC campaign).

RSS note: v1 alone may show little RSS motion (Envs are tenured and the arena
never reclaims; the win is fewer/smaller thunk tails). Judge RSS primarily by
the deterministic byte counters (capture bytes eliminated vs Env bytes added)
and let the full-coverage model carry the peak-RSS projection — peak follows
only at v2 scale + Track B/C. Say so explicitly in the handback to prevent a
false RSS-kill.

---

## 6. Sequencing summary

| Step | What | Effort | Blocks |
|---|---|---|---|
| P0.A-1 | kGates + lint | H | everything gated |
| P0.A-2..6 | UAF/barrier fixes, Env walkers, assert, rooting backlog | ~1 wk | W2+, Track B later |
| P0.B | probe deletion, attrsUsed, CALL_N hop, groupBy, path-coerce | ~2-3 d | — |
| P0.C | retire env-share heuristic, keep plumbing | H-D | W2 |
| P0 exit | new darwin-4 baseline row | H | Gate C |
| Phase 1 | counters 1-4 + projection | ~2 d | Gate A |
| Gate A | build / close decision | — | W0 |
| W0-W6 | v1 build + validate + measure | ~3-4 wk | Gate C |
| Gate C | ship v2 / kill+pivot | — | Track A start |

Parallelizable by a second agent during W0-W6: Track C diets EXCEPT
closure.hh; P4.6 import micro-fixes; Q4.1 impurity taint. NOT parallel:
anything touching lower_v3.hh/emit.cc/vm.cc-MAKE/GET paths or gc walkers.

## 7. Command reference

```bash
# pre-merge gate (expect ALL GREEN, currently 28 suites):
nix develop -c bash src/libexpr-v3/test/all-v3-tests.sh --brute
# pinned nixpkgs for ANY <nixpkgs> eval:
source src/libexpr-v3/test/nixpkgs-pin.sh
# deterministic counters + phase breakdown:
bench/profile-at-scale.sh        # or: make profile / make profile-note
# baseline comparison (cold + WARM=1):
bench/beat-tw-compare.sh
# darwin-4 sync + build:
rsync -az --exclude='*.o' --exclude='*.dylib' src/libexpr-v3/ \
  aarch64-darwin-4.lan:Projects/iohk/nix/src/libexpr-v3/
ssh aarch64-darwin-4.lan 'cd ~/Projects/iohk/nix && nix develop -c ninja -C build src/libexpr-v3/v3-eval src/nix/nix'
# darwin-4 checkout lags the rsync'd binary — pass COMMIT=<laptop HEAD> to profile scripts.
# M5 on darwin-4: nix eval via `getFlake path:~/Projects/iohk/cardano-node`, attr .cardano-node.name
# NEVER draw counter conclusions from warm runs (descriptor flags not serialized).
```

## 8. Handback protocol

Append dated sections to THIS file (mirroring DEFECT_AUDIT's handbacks):
commits, each gate's measured result vs its pre-committed threshold,
corrections to any claim above found wrong, new darwin4-rows.tsv rows +
git notes. A kill is a deliverable — write it with the same care as a ship.
Update the project memory index on Gate A and Gate C resolutions.

*Plan authored 2026-07-03 from DEFECT_REVIEW_2026-07-03 (§2.1/2.2, §3.1/3.2,
§5, §6 matrix) + DEFECT_AUDIT_2026-07-02 handbacks (design-b RCA, P3.1/P3.3
falsifications) + JIT scoping 2026-06-29. Baseline: P0.4 rows at 2d7efdb97;
HEAD at authoring 0e32dbfb3.*

---

# Handback — Phase 0 execution, 2026-07-03 (in progress)

Executor session started from HEAD `0c7dfdcc3`. Full `--brute` = **29 suites**
(was 28; P0.A-1 added `cache-gate-coverage`). Then 32 suites after the Q1 batch
(3 more). All commits on branch `angerman/2.35-eval-profiling-v2`.

## P0.A-1 — kGates + REAL lint — DONE (`1f99afa30`)
Landed FIRST and ALONE per the binding rule. Full `--brute` 29/29 ALL GREEN.
- **Confirmed §1.4**: the comment claiming `lint-cache-coherence.sh` enforced the
  kGates↔codegen-gate sync was fictional (it only checked schema Rules 1/2).
- Added **Rule 3** to `lint-cache-coherence.sh`: a FULL-TREE invariant (not
  diff-scoped) — parse `kGates[]` from primops.cc, scan every `getenv("NIX_V3_*")`
  in emit.cc/opt_*.cc/cli/lower_v3.hh/ir.cc, fail unless each is in `kGates[]` OR
  an explicit non-codegen allowlist (`EMIT_BYTECODE`, `EMIT_BYTECODE_OUT`,
  `DBG_STRICTNESS_VERBOSE`). Overridable paths + `CACHE_LINT_RULE3_ONLY` for the
  self-test. Failing-first proof captured (flagged 3 gates pre-fix, clean after).
- **kGates += RAW_FORMALS, NO_NONREC_ATTRS_INIT** (the two §1.4 named) **AND
  `NIX_V3_OCCUR_DCE_VALIDATE`** — a hole §1.4 MISSED: it runs
  `deadBindingElimViaOccur` (the NEW DCE) as the kept result instead of the
  default `deadBindingElim`, so it changes emitted bytecode and belongs in the
  key. **Correction to §1.4: it named 2 missing gates; there were 3.**
- New self-test `run-cache-gate-coverage-tests.sh` (5 sub-tests, both directions,
  self-contained fixtures) registered in `--brute`.

## Q1 batch — Q1.1 + Q1.2 + Q1.3 latent-hardening — DONE (`0ed1eaa1c`)
Three force-path writeback fixes. Full `--brute` **32/32 ALL GREEN**; independent
adversarial review **NOT REFUTED** on all three (confirmed Q1.1 withStack is a
scanned root so the index store needs no barrier; Q1.2 mirrors valueEqual's
already-shipped interior-ListVec cellWrite pattern; Q1.3 memoization preserved;
one benign Phase-E-only nuance already tracked in §1.9). **Key finding: all three
are LATENT (not reproducible-today) fixes** —
real code-level asymmetries with their sibling barriers, but masked under current
gating. Verified empirically (0 audit flags pre-fix on constructed repros) —
consistent with the review's own §1.7 "safe today ONLY via the exitDepth==0 gate +
C-stack pin" classification. They become live under S2.1 safepoint work and are
landed now as trial preconditions.
- **Q1.1 (vm.cc withLookup)**: `w = forceValue(vm, w)` wrote through a `Value &`
  into `vm.withStack` across a re-entrant force that can realloc the vector.
  **Reachable-UAF PROVEN via instrumentation probe**: a chained-`with` fixture
  (>64 live with-scopes forcing a realloc) fires the realloc-while-holding-`w`
  window **8×** (`buf 0x…e00→0x…000 cap 64→256 i=0`) — a use-after-free WRITE to
  the freed buffer. Not observable in results (the dangling write+read are
  self-consistent) nor under macOS libgmalloc (doesn't guard this std::vector);
  the probe is the definitive proof. Fix: force into a local, memoize **by index**
  (`vm.withStack[i] = forced`), lookup through the local. Guardrail test
  `run-withlookup-dangling-tests.sh`.
- **Q1.2 (vm.cc valueLess)**: list-compare writeback missing the `cellWrite`
  barrier valueEqual (@~1086) and primSort already have. Latent (forced inner
  values bypass to tenured once the outer list has tenured under nursery
  pressure → rarely a real nursery→tenured edge today). Fix mirrors valueEqual.
  Guardrail `run-valueless-barrier-tests.sh`.
- **Q1.3 (primops.cc replaceStrings)**: **CORRECTION to §1.3** — its claimed
  reachable missed-root is NOT reachable: (1) `builtins.tryEval` does NOT catch
  the replaceStrings type error — verified in **v3 AND the tree-walker** (both
  throw "expected a string…" to top level), so the "list survives under tryEval"
  premise is false; (2) happy-path forced elements are STRINGS (tenured) → no
  nursery edge (40 000-element from-list under 1 MB-nursery audit = 0 flags pre-
  and post-fix). The fix stands as (a) a real premature-argument-mutation cleanup
  (force→check→store, never mutate a caller-visible arg with a value it rejects)
  plus (b) a defensive barrier consistent with sibling sites. Guardrail
  `run-replacestrings-barrier-tests.sh` (valid + type-error arms).

**Method note for future latent-barrier fixes**: the nursery AUDIT trips only when
the forced value is *already* a nursery cell being stored into a *tenured*
container (primSort's bulk-copy of early-built literal nested lists is the
canonical trip). Force-then-writeback sites (valueLess, replaceStrings) rarely
create that edge under current gating because late forces bypass to tenured — so
their tests are correctness+exercise guardrails, not pre-fix-tripping repros, and
that is expected, not a test defect.

## P0.A-4 — Env::parent walkers — DONE (`373ee07a5`)
All four GC walkers (scavenger walkEnv, mark-sweep marker+evac, auditor visitEnv)
now walk `Env::parent`; envPostConstructBarrier added at the intern site. NO-OP
today (nothing sets `parent`); full `--brute` 32/32. Adversarial review: scavenger
/auditor/barrier NOT REFUTED; surfaced a real LATENT marker/evac early-break
invariant hole (an Env can be mark-set WITHOUT a precise values-walk via the
interior Tag::Slot→Env conservative-mark path; gen-major byte-scan doesn't de-box;
evac walkFields(Env) is a no-op) → **W2 PRECONDITION** documented at both loop
sites + tracked. Inert today (parent null; NIX_V3_EVAC unrevived); scavenger is
immune. Must close before NIX_V3_ENV_CAPTURE populates `parent`.

## Phase 1 — counters + Gate A — DONE. **GATE A VERDICT: GO (BUILD v1).**
Counters wired (uncommitted at time of verdict; alloc.hh AllocStats +
vm.cc OP_MAKE_THUNK/CLOSURE + emit.cc capture loops + run.cc dump). Measured
LOCALLY (deterministic + host-independent per §3, so laptop numbers are
authoritative for Gate A), cache-off (`NIX_V3_NO_DISK_CACHE=1 NIX_VM_STATS=1
NIX_VM_OPCOUNTS=1`), all four workloads, byte-correct results:

| Workload | C1 capture-op share (RUNTIME) | C2 avg cap/MAKE | C2 nUp≥2 share | C3 fwd-capture (emit) |
|---|---|---|---|---|
| hello.drvPath   | 16.86% (1.29M/7.62M)  | 1.94 | ~50% | 64.91% |
| firefox.drvPath | 18.11% (6.68M/36.86M) | 2.10 | ~52% | 64.14% |
| git.drvPath     | 16.57% (2.09M/12.60M) | 1.97 | ~52% | 64.69% |
| M5 cardano.name | 17.88% (11.0M/61.55M) | 1.88 | ~54% | 53.76% |

**Gate A (pre-committed):** BUILD if (C1 ≥ 8%) OR (C3 ≥ 30% AND sibling-density
≥ 1.5); CLOSE if (C1 < 4% AND C3 < 15%). → **Every workload satisfies BOTH the
primary (C1 = 16.6–18.1% ≫ 8%) AND the secondary (C3 = 54–65% ≫ 30% with
sibling-density 1.88–2.10 ≥ 1.5) conditions.** This is a clear GO, not a
"between" case; no downgrade of Gate C expectations. The DEFECT_REVIEW §2.1
estimate ("capture pushes plausibly 10–16% of ALL ops") is CONFIRMED at the high
end (16.6–18.1%), and the forwarding-waste hypothesis (§2.1: "much of the
GET_UPVALUE share is capture-forwarding, not body reads") is confirmed strong:
53–65% of captures are pure upvalue-forwarding (the transitive re-copy the env
chain eliminates entirely).

**PRE-COMMITTED PROJECTION P (recorded BEFORE building v1, per §3; firefox is the
Gate C comparand):** v1 is per-function-opt-in / hybrid, so it captures only the
env-routable eligible slice (§5.3 single-assignment escaping locals; with-targets
stay flat in v1, so the withsTotal portion — 463 576 of firefox's 6.68M
capture-ops — is NOT eligible; nUp portion = 6.21M). Eligibility fraction is
counter-4 (single-assignment), estimated at Phase 1 and refined at W1 dump-mode;
conservative estimate ~50% of nUp capture-ops env-routable in v1 (single-assignment
bindings pass trivially; emitter TEMP-slots fail).
  - **Predicted CPU win P_cpu (firefox, v1 eligible slice) ≈ 5% (range 3–7%).**
    Formula (§3): dispatch term = capture-op-share(18.1%) × eligible-frac(~0.5) ×
    dispatch-cost-per-trivial-op(~0.5, trivial stack ops are dispatch-dominated)
    ≈ ~4.5%; + alloc term = per-capture FAM-copy elimination × ALLOC-share(20%)
    ≈ ~1%. ⇒ **P_cpu ≈ 5%.**
  - **Predicted RSS win P_rss (v1) ≈ ~0 to small (single-digit MB).** Per §5.8:
    Envs are tenured + arena never reclaims, so v1's RSS win is only fewer/smaller
    thunk tails (nUp≥2 objects, ~1.64M on firefox, drop from nUp×8B inline to one
    8B env-pointer, minus amortized shared-Env overhead). Judge RSS by the
    deterministic byte counters, NOT peak — peak follows at v2 scale.
  - **Full-coverage (v2) model:** extends eligibility + migrates the with-chain
    (killing capturedWiths, ~26% of firefox thunks per FP-2b + withsTotal 463K) +
    rec-slot + Env nursery-eligibility → projects **≥15% CPU or ≥20% RSS** (the v2
    SHIP gate). The 18% capture-op + 64% forwarding ceiling makes this plausible.
  - **Gate C thresholds derived from P (pre-committed):** SHIP if realized CPU
    ≥ max(3%, 0.6·P_cpu)=**3%** on firefox or M5 cache-off AND RSS not worse by
    >1% AND v2 model ≥15%CPU/≥20%RSS. KILL if realized < max(1.5%, 0.3·P_cpu)=
    **1.5%** AND RSS flat.

Next: build+brute+commit the counter instrumentation, then Track E v1 W0→W6.

## Track E v1 — W0 (opcode/gate/serialize foundation) — DONE (`f3acf9d14`)
Validated NO-OP foundation for the capture model:
- Opcodes OP_MAKE_ENV=0xE0, OP_SET_ENV=0xE1, OP_GET_ENV=0xE2 (plain-int operands,
  no SymbolId → no P3.3 remap footgun; OP_GET_ENV carries idx in operand + 1
  trailer word = depth).
- opcodeTableFingerprint += the 3 (auto-invalidates old CUs). All FOUR
  code-stream serialize walkers + disasm learn OP_GET_ENV's 1-word trailer.
- vm.cc dispatch stubs TRAP (reached = emitter/gate bug; nothing emits until W2).
- `NIX_V3_ENV_CAPTURE` gate (emit.cc `g_envCapture`, default-off, constraint-#4
  retirement comment) + added to kGates — **the P0.A-1 Rule-3 lint now ENFORCES
  this** (verified it flags the gate if absent from kGates).
- Validation: gate-off vs gate-ON hello.drvPath BYTE-IDENTICAL; zero env-opcodes
  emitted (no-op confirmed); full --brute 31/32 (sole failure = the
  let-chain-5000 fixed-15s-`timeout -s KILL` flake on this shared host at load
  avg 21 — measured 3.73s CPU/5.5s wall, correct result; NOT a W0 slowdown, W0
  adds no eval-time work; brute-audit + let-rec-publish both pass standalone +
  in the clean re-run).

## Track E v1 — W1..W6 roadmap (prepped; the multi-week remainder)
Prep done this session (structures located for a fast start):
- **W1 (escape analysis §5.3 + dump-mode)**: IR structs in `include/v3/ir.hh` —
  `Lambda`@100 / `MkThunk`@135 (freeVars + lexicalWiths), `Function`@473 (mirror
  the `rawFormalEligible`/`formalSym`@515 metadata pattern for the new
  `usesDefEnv`/`envSlotCount` + escaping-local→Env-index map). Dump-mode
  (`NIX_V3_ENV_CAPTURE=dump`) reports counter-4 eligibility (single-assignment
  escaping locals) per workload — refines the projection; NO codegen change, so
  byte-id is trivial and the brute is a formality.
- **W2 (emission + runtime)**: MUST first close the W2 PRECONDITION (marker/evac
  Env-parent early-break hole — task #8). Then OP_MAKE_ENV lazy-emit + OP_SET_ENV
  + child OP_GET_ENV; runtime handlers reusing Closure::upvalEnv + the ENV_SHARED
  thunk tail (P0.C keeps this plumbing). fakeClo audit (3 sites, vm.cc:~13330).
- **W3**: GC integration tests (synthetic Env chains + V3_DBG_ENV_CAPTURE_AUDIT —
  this is where the P0.A-4 walkers get their LIVE validation) + fakeClo +
  deserialize round-trip.
- **W4**: byte-id ladder gate-on/off (hello→git→firefox→python3) + Gate B (brute
  both settings).
- **W5**: darwin-4 A/B (cache-off + warm; firefox+M5+git; the P0.4 baseline row
  needs P0.B/P0.C landed first) + re-measure the Phase-1 counters for realized-
  vs-projected attribution.
- **W6**: Gate C verdict vs projection P (SHIP≥3%/KILL<1.5% + v2 ≥15%CPU/20%RSS).

Remaining Phase-0 hygiene (non-v1-blocking for W0/W1; needed before the W5
darwin-4 baseline): P0.B quick CPU wins, P0.C (retire env-share intern heuristic
— **the Phase-1 nUp histogram is its falsifier: avg nUp 1.88–2.10 ⇒ the nUp>8
intern threshold ~never fires, confirming §3.1**), Q1.6 rooting backlog, Q1.8
GcRoot-at-scavenge assert.

### W1 implementation spec (turnkey — all machinery located this session)
The escape analysis is the DUAL of `computeFreeVars(Module&)` in `ir.cc:312`,
which already: (a) builds `funcOfBlock[bid]` via per-function reachability from
`Function::entryBlock` (crossing sub-blocks via `collectExprSubBlocks`, NOT
crossing into nested-function entryBlocks); (b) after convergence, PROPAGATES
each function's `freeVars` onto the ir::Lambda/MkThunk **node**'s `freeVars`
field (`ir.cc:441-453`) — so a child's captures are readable directly off the
node. Reuse both.
- **E(F)** (per Function F): iterate F's blocks (`funcOfBlock[bid]==fid`), for
  each binding that is a `Lambda`/`MkThunk` read `e.freeVars` (the child's
  captures); classify each fv:
  - fv ∈ F's-own-bound set (block-binding `.var`s of F + `paramVar` +
    `extraParams`) → **escaping** (env-routable at depth 0 via F's frame Env);
  - fv ∈ `F.freeVars` → **forwarding** (env-routable at depth+1 via parent Env —
    this is the transitive re-copy the chain kills; should ≈ Phase-1 C3 54–65%);
  - (at the IR level ~all captures are one of these ⇒ ~100% env-routable; the
    real v1 INELIGIBILITY is EMIT-side — emitter TEMP defer-slots (§5.3-2) +
    with-targets stay flat — so counter-4 precision needs the emit-side slot
    map, computed at W2, per the plan's "Gate A numbers come from W1 if Phase 1
    used estimates").
- **W1 dump** (`NIX_V3_ENV_CAPTURE=dump`, analysis+stderr only, NO codegen ⇒
  byte-id trivial): report escaping/forwarding/total capture split per workload;
  cross-check forwarding ≈ Phase-1 C3.  Add `computeEnvCaptureStats(const
  Module&)` in ir.cc called after computeFreeVars; wire the report into the
  NIX_VM_STATS dump.  Then W2 adds `usesDefEnv`/`envSlotCount` to `Function`
  (mirror `rawFormalEligible`@ir.hh:515) + the emit-side slot-eligibility (SSA
  proof / TEMP-slot exclusion) + OP_MAKE_ENV/SET_ENV/GET_ENV emission.

### W1 — escape-analysis dump-mode — DONE (`e4d286166`)
`collectEnvCaptureStats(Module&)` in ir.cc (gate=dump/1, at computeFreeVars tail;
analysis-only ⇒ byte-identical). MEASURED on firefox.drvPath: total child
captures 92731 → escaping 37.9% / forwarding 62.1% / other **0.0%** ⇒ **100%
env-routable at the IR level**. Forwarding 62.1% CROSS-CHECKS Phase-1 C3 (64.14%)
— two independent measurements agree, validating the analysis. Projection
refinement: v1's ceiling is bounded by EMIT-side eligibility (W2), NOT IR
routability (100%) — the model applies to the full capture population. Full
`--brute` 32/32 (v3-smoke rebuilt: AllocStats layout changed — the recurring
stale-binary gotcha; ALWAYS rebuild v3-smoke on an AllocStats change).

### W2 — emission + runtime (NEXT; the risky core) — spec
1. **FIRST close the W2 PRECONDITION** (task #8: marker/evac Env-parent
   early-break hole) — it goes live the moment emission populates `parent`.
2. `Function` += `usesDefEnv` bit + `envSlotCount` + escaping-local→Env-idx map
   (mirror `rawFormalEligible`/`formalSym`@ir.hh:515); serialize (schema bump).
3. Emit-side eligibility = W1's escaping set MINUS emitter TEMP defer-slots
   (§5.3-2 single-assignment) MINUS with-targets (stay flat in v1). Lazy
   OP_MAKE_ENV before first escaping store; OP_SET_ENV on the binding store;
   child eligible captures → OP_GET_ENV(depth,idx), residual → flat FAM (hybrid).
4. Runtime handlers reuse `Closure::upvalEnv` + the ENV_SHARED thunk tail (P0.C
   keeps this plumbing); frame-entry installs the stored Env as defEnv register;
   OP_SET_ENV writeback uses `cellWrite` (tenured Env ← nursery payload; PhD-6).
   Audit all 3 fakeClo fill sites (vm.cc:~13330).
5. GATE: byte-id ladder gate-on/off (hello→git→firefox→python3) FIRST (design-b
   lesson), then brute both settings + adversarial review of the GC/emit changes.

#### W2a — frame-Env root-walking GC scaffold — DONE (`a431e5f4c`, no-op, brute 32/32, adversarial NOT REFUTED)
Adversarial review (2026-07-04) verified root-walk COMPLETENESS: all four
frame-root paths covered (scavenger main gc.cc:906 + nested :888 via GK_ENV;
auditor :1489 via visitEnv over primary+secondary VMStates; mark+evac via
walkAllV3Roots→walkOneVMState :94 for primary AND secondary), GK_ENV drains
through walkEnv (iterative parent chain), the no-dedup default visitEnv is
cycle-safe (parent chains provably acyclic — allocEnv inits parent=null,
OP_MAKE_ENV only ever points parent at an OLDER enclosing frame), MarkVisitor
marks the Env cell (else UAF), EvacVisitor shares the one walked_ set. No-op
claim airtight (defEnv only ever `=nullptr`; handlers are W0 traps).
NOTE (pre-existing, not worsened): the dormant NIX_V3_FIBER_BRIDGE yielded-fiber
conservative scan does not precisely walk a yielded VMState's frames — same
exposure as the existing closure/thunk frame fields; latent, gated off.
Landed the GC/root-walk HALF of W2 as a validated no-op (P0.A-4 pattern):
CallFrame.defEnv + `RootVisitor::visitEnv` (default walks values+parent;
MarkVisitor overrides to mark the Env cell; EvacVisitor overrides with walked_
dedup) + all four frame-root-walk sites (scavenger main+nested, auditor,
walkAllV3Roots for mark+evac).  So W2b's emission has the moving-GC integration
in place + independently adversarial-reviewed.  defEnv null today ⇒ inert.

#### W2b-prep — LambdaDescriptor metadata + serialize — DONE (`d7b17a614`, no-op, brute 32/32)
`usesDefEnv` + `envSlotCount` on LambdaDescriptor + serialize (schema 17→18);
unset ⇒ byte-identical + brute-green (cache round-trip suites confirm the
schema-18 round-trip).  Two W2b-runtime design findings from the build (record —
the plan's "reuse upvalEnv" note is incomplete):
- **upvalEnv repurposing**: P0.C retired interning ⇒ `Closure::upvalEnv` is always
  null and free to become the captured defEnv.  BUT `closureUpvalue()` branches
  `upvalEnv ? upvalEnv->values[i] : upvalues[i]` — once upvalEnv=defEnv it would
  misread the residual-flat FAM.  W2b-runtime must first make closureUpvalue read
  the FAM unconditionally (no-op today, upvalEnv null).
- **walkClosure XOR→both**: with a HYBRID closure (upvalEnv=defEnv AND a residual
  flat FAM), walkClosure/mark/evac (currently walk upvalEnv XOR the FAM) must walk
  BOTH — the defEnv as an Env (gray/mark/rewrite, already done for upvalEnv) AND
  the nUpvalues residual FAM.  Today XOR is a no-op (upvalEnv null → walks FAM);
  the both-walk must land before the emit sets upvalEnv=defEnv.
- **upvalEnv reuse is ENTANGLED (design decision for W2b-runtime)**: the plan
  §5.4 "reuse Closure::upvalEnv for defEnv" is broader than a one-liner —
  `closureUpvalue`/`closureUpvaluePtr` (closure.hh:89-102) and ~10 call sites
  read `upvalEnv ? upvalEnv->values[i] : upvalues[i]`, INCLUDING the extends/
  compose intrinsic paths (vm.cc:6702/6750/15407, `intrinsicVar0/1/2`) + the
  ENV_SHARED thunk mechanism.  Repurposing upvalEnv=defEnv requires all of them
  to stop treating upvalEnv as an upvalue-store (read the FAM instead) — a broad,
  subtle semantic change (design-b class).  CLEANER ALTERNATIVE (recommended for
  W2b): add a SEPARATE `Closure::capturedDefEnv` field (+8 B) — closureUpvalue
  unchanged, no XOR→both entanglement, walkClosure walks the FAM (as now) + the
  new field.  Decide reuse-vs-new-field in the coherent W2b-runtime build.

These, the handlers, MAKE/frame-entry install, the escape-analysis emit, and the
marker/evac precondition are the interdependent, GC-critical, exercised-together
core — the multi-day fresh-focus remainder.

#### W2b-runtime DESIGN DECISION (chosen 2026-07-04 after tracing all layers):
**Use a SEPARATE `Closure::capturedDefEnv` field (+8 B), NOT upvalEnv reuse.**
Rationale (six build-attempt findings): reusing upvalEnv would force
closureUpvalue→FAM (breaking the retained NIX_V3_ENV_SHARE_AFTER A/B override,
which can still set upvalEnv to an upvalue-Env), a walkClosure XOR→both change,
and interacts with the ~10 accessor callers + intrinsics + ENV_SHARED.  A
separate field avoids ALL of that: closureUpvalue/closureUpvaluePtr UNCHANGED,
no override conflict, no XOR→both.  It is the exact W2a pattern applied to
Closure — add the field, init it null at every alloc, GC-walk it ALONGSIDE
upvalEnv in walkClosure/mark/evac/auditor (gray/mark/rewrite as a tenured Env),
update allocClosure/closureAllocatedSize sizing (mind the `_pad`).  +8 B/closure
is a defined v1 cost (v2 reclaims it by fully retiring interning + reusing
upvalEnv once the override is dropped).  Then: real handlers; MAKE_CLOSURE/THUNK
usesDefEnv → `c->capturedDefEnv = frame.defEnv`; frame-entry → `newFrame.defEnv =
desc->usesDefEnv ? closure->capturedDefEnv : nullptr`; the escape-analysis EMIT
(FuncCtx.envSlot depth/idx, emitVarRef→GET_ENV, store→SET_ENV, child→
capturedDefEnv, hybrid residual FAM); close the marker/evac precondition.
Validate the minimal slice `let x=1+2; f=_: x; in f 0` → 3 gate-on==off, then the
byte-id ladder + brute → Gate B.  This design is directed + lower-risk, but the
runtime (field+GC+handlers+MAKE+frame-entry) is unexercisable without the emit,
and the emit is intricate — so runtime+emit land + byte-id-validate TOGETHER
(multi-day, fresh focus).

#### W2b — emission + runtime (the atomic, intricate remainder) — spec refined
With W2a done, W2b = descriptor + runtime handlers + MAKE/frame-entry + the
escape-analysis EMIT, all validated TOGETHER (byte-id ladder + brute + the
minimal slice), since gate-on codegen only works when emit+handlers+GC agree:
- **LambdaDescriptor** (closure.hh:324) += `bool usesDefEnv; uint16_t envSlotCount;`
  → serialize + `kSchemaVersion` 17→18 (lint Rule 1).
- **Handlers** (replace W0 trap-stubs; use `vm.frames.back().defEnv` for mutable
  access — frames is stable within a handler; stamp new Env slots with
  `mkUninitialized()`; OP_SET_ENV uses `cellWrite`).
- **MAKE_CLOSURE/THUNK** (vm.cc:5384/5779) usesDefEnv → store `frames.back().defEnv`
  into upvalEnv (reuse the shareUpvalues branch P0.C freed) + **frame-entry**
  install (`newFrame.defEnv = desc->usesDefEnv ? closure->upvalEnv : nullptr`) at
  every call/force path + the 3 fakeClo sites.
- **EMIT** (the hard part): extend W1's escape analysis to the per-Function
  eligible set (E(F) MINUS emitter TEMP defer-slots MINUS with-targets) + envIdx
  assignment + usesDefEnv; FuncCtx.envSlot; emitVarRef routes escaping locals →
  OP_GET_ENV(0,idx), env-routed ancestor upvalues → OP_GET_ENV(depth,idx);
  escaping-local store → OP_SET_ENV (after a lazy OP_MAKE_ENV); child capture of
  an escaping/forwarded local → route via defEnv (usesDefEnv), residual flat.
- **W2-PRECONDITION** (task #8): close the marker/evac/visitEnv early-break hole
  (now applies to visitEnv too) WITH W2b, validated by live chains under
  V3_DBG_ENV_CAPTURE_AUDIT.
- **Minimal validatable slice**: `let x=1+2; f=_: x; in f 0` → 3 gate-on==gate-off,
  then broaden + byte-id ladder (hello→git→firefox→python3) + brute both settings.

#### W2b-runtime EMIT — BUILT + PARTIALLY VALIDATED (2026-07-04). STATUS:
The full emit is implemented + landed (analysis passes + emitVarRef OP_GET_ENV
routing + escaping-local OP_SET_ENV binding-store + prologue OP_MAKE_ENV +
escaping-param copy + descriptor usesDefEnv/envSlotCount/residual-nUpvalues +
Lambda/MkThunk residual-push gating).  KEY BUGS FOUND + FIXED during bring-up:
1. **Lambda-lift singleton cache** (vm.cc:5417): a nUp==0 closure was interned as
   a context-free singleton + REUSED across calls — but env-capture closures are
   nUp==0 yet capture a per-call defEnv.  Fixed: exclude `usesDefEnv` closures from
   the singleton lift (they take the per-instantiation path that sets capturedDefEnv).
2. **Hybrid, not no-residual**: my "every freeVar env-routes" simplification was
   WRONG — recVars (let-rec / rec-attrset / formals-with-sibling-defaults, which
   lower to `ir::LetRec`) are captured via the rec-attrset machinery (OP_ATTRS_REC_
   INIT/SET + OP_GET_UPVALUE_REC_BINDING[_SLOT]), NOT plain upvalues.  Env-routing
   them corrupts that machinery.  Fix: exclude recVars (m.recVarIds +
   recVarToSlotVar + every LetRec entry/hiddenEntry outerUpvalues + hiddenVar) from
   E — they stay RESIDUAL flat upvalues.  So it IS the plan's hybrid.
VALIDATED gate-on==gate-off (hand-tests): pure-lambda capture, 3-level depth chain,
letrec recursion (fib), self-rec, multi-capture, formals (WITH sibling-default
`{a?1,b?a+1}`), rec-attrsets, currying/PAP, higher-order (map/foldl'/mapAttrs/
genList), nested-let, inherit, functionArgs, ellipsis.  GATE-OFF: byte-id
(hello==golden) + full --brute 32/32 (NO regression — every emit hook is
g_envCapture-gated).  GATE-ON --brute: **28/32** (broad PASS incl. lang 143, property, drv-parity,
fetcher, readdir, chain-bindings) with **4 FAIL suites** — ALL the same rec-binding
integration root (the earlier commit 73e4e35d5 body says "2 gaps"; the full run
shows 4 suites — CORRECTED HERE):
  (a) **iterative-force** app-spine (deep nested application) — rc=1.
  (b) **brute-audit** list-primop-barriers (7/17 sub-cases exit=1) + nixpkgs `lib`
      aborts with `OP_GET_UPVALUE_REC_BINDING_SLOT: upvalue index out of range`.
  (c) **583-tag-app-cache** (mapAttrs-style App/App3 capture).
  (d) **apply-overrides-1.7** (__overrides rec-attrset override chain).
Both point at the SAME root: a function that READS a rec-binding via
RecBindingSlotRef (OP_GET_UPVALUE_REC_BINDING[_SLOT], which uses the recVar's flat
upvalue index) while ALSO env-routing OTHER freeVars — the env-routed vars are
removed from the flat upvalue push list, and although emitOne push + fc.upvalue map
both skip env-routed in freeVars order (so they SHOULD agree), the LetRec ENTRY
emit (emitOne(LetRec)) has its OWN outerUp+recVar push (recVar = "implicit upvalue
0") that does NOT go through the residual-aware emitOne(MkThunk) path → index
disagreement.  **NEXT SESSION**: make the LetRec entry-thunk push residual-aware
(align emitOne(LetRec)'s per-entry push + the entry FUNCTION's fc.upvalue), OR (the
robust coarse fix) mark every LetRec-entry funcId + every RecBindingSlotRef-reading
funcId as `forceFlat` (usesDefEnv=false, push all freeVars, no env-routing) so the
rec machinery keeps its flat layout — but handle the knock-on (a forceFlat function
with its own escaping locals still needs createEnv for its children).  Also confirm
whether list-primop-barriers is a true GC missed-root (BRUTE hit) vs a divergence
(run the sub-test with V3_DBG_NURSERY_AUDIT).  The env-capture MECHANISM is PROVEN
VIABLE in the moving-GC VM (the core research question) — the remainder is
rec-binding-emit integration, not a viability question.  Gate C (perf) needs
nixpkgs to fully eval gate-on ⇒ blocked on the rec-binding integration.

#### W2 CODE-LEVEL DESIGN (worked out 2026-07-04; turnkey — build in the working
tree, validate byte-id + brute, commit only when GREEN; it is ATOMIC — emission +
runtime + GC must land together to be byte-id-validatable, so build it all then
validate; ~3-5 focused days):
- **CallFrame** (vm.hh:66) += `Env * defEnv = nullptr;` (+8 B). **GC-CRITICAL —
  and the frame-root walk is in TWO places with different mechanisms** (verified
  by a build attempt 2026-07-04; a correction to the earlier one-line note):
  1. **Scavenger** (gc.cc:902 main loop + gc.cc:888 nested-VMState loop) walks
     `vm.frames` directly — add `if (f.defEnv && walked.insert(f.defEnv).second)
     graylist.push_back({f.defEnv, GK_ENV});` (walkEnv + P0.A-4 handle the chain).
  2. **Auditor** (gc.cc:1479) — add `if (f.defEnv) a.visitEnv(f.defEnv, ...)`.
  3. **Mark + evac** do NOT walk frames directly — they use the SHARED
     `walkAllV3Roots` (precise_root.cc:52). **BLOCKER FOUND**: `RootVisitor`
     (precise_root.hh:85) has visitClosure/Thunk/Bindings/List/Pair/Slot but **NO
     `visitEnv`** — and a frame `defEnv` is a root reachable ONLY via the frame
     register until a child captures it, so it MUST be walked here.  visiting its
     values via `visitValue` is NOT enough: the MARK visitor must also set the
     Env cell's mark bit (else sweep reclaims a live Env → UAF), which only a
     visitEnv can do.  ⇒ **W2 must add `virtual void visitEnv(Env*&)` to
     RootVisitor + implement it in every subclass** (the mark + evac visitors in
     mark_sweep.cc route to their existing inline Env-walk — the same code P0.A-4
     edited; the NoOp/auditor-style visitors no-op or walk values).  Then
     walkAllV3Roots gains `if (f.defEnv) visitor.visitEnv(f.defEnv);`.  This is a
     broad GC-interface change best landed COHERENTLY WITH W2b's emission so live
     Env chains + V3_DBG_ENV_CAPTURE_AUDIT validate it (not as unexercised
     scaffold).  The plan's "reuse the upvalEnv walkers" note was incomplete: the
     frame defEnv is a NEW root class the upvalEnv walkers don't cover.
- **allocEnv(n)** (alloc.hh:2998) returns a TENURED Env, parent=null, nValues=n,
  values[] UNINITIALIZED → OP_MAKE_ENV MUST stamp the n slots (Uninitialized tag,
  not leave them — value.hh maps all-zero to Float 0.0; a mid-fill GC would
  misread them). Check the Value uninitialized-stamp helper.
- **Handlers** (replace the W0 trap-stubs; `cur` = current frame ref):
  - OP_MAKE_ENV: `Env* e=allocEnv(n); e->parent=cur.defEnv; stamp e->values[0..n);
    cur.defEnv=e; envPostConstructBarrier(e);` (lazy — emit before the first
    escaping store, so non-creating executions pay nothing).
  - OP_SET_ENV(idx): `cellWrite(&cur.defEnv->values[idx], pop(vm), nullptr);`
    (PhD-6: tenured Env ← possibly-nursery payload).
  - OP_GET_ENV(idx; +1 trailer=depth): `Env* e=cur.defEnv; for depth: e=e->parent;
    push(e->values[idx]);` (depth==0 fast case first).
- **MAKE_CLOSURE/THUNK** (vm.cc:5384/5779): when `desc->usesDefEnv`, store
  `cur.defEnv` into the closure's `upvalEnv` / the thunk ENV_SHARED tail — REUSE
  the `shareUpvalues` branch (vm.cc:5500 `c->upvalEnv = closureEnv`) that P0.C
  freed up; the child's flat nUp is only the RESIDUAL (hybrid). Then GC already
  walks upvalEnv (P0.A-4 + existing env-sharing walkers).
- **Frame entry** (OP_CALL / thunk-force new-frame setup): `newFrame.defEnv =
  desc->usesDefEnv ? closure->upvalEnv : nullptr;`. Audit the 3 fakeClo fill
  sites (vm.cc:~13330) to propagate it.
- **LambdaDescriptor** (closure.hh) += `bool usesDefEnv; uint16_t envSlotCount;`
  → serialize schema bump (lint Rule 1) + the 4 serialize walkers already handle
  OP_GET_ENV's trailer (W0). Emit reads these off ir::Function (like
  rawFormalEligible).
- **Emit** (emit.cc + a lower phase): the escape analysis (extend W1's
  collectEnvCaptureStats into a per-Function eligible set: E(F) MINUS emitter
  TEMP defer-slots MINUS with-targets, with slot→envIdx assignment) drives:
  add `FuncCtx.envSlot` (VarId→envIdx); in emitVarRef, an escaping-eligible
  local → OP_GET_ENV(0,idx), an env-routed ancestor upvalue → OP_GET_ENV(depth,
  idx); the escaping local's binding store → OP_SET_ENV(idx) (after OP_MAKE_ENV,
  emitted lazily at entry); a child capturing an escaping/forwarded local routes
  via defEnv (usesDefEnv) instead of pushing it flat; residual stays flat FAM.
- **W2 PRECONDITION FIRST** (task #8): close the marker/evac Env-parent
  early-break hole — it goes live the moment defEnv/parent chains materialize.
- **Minimal validatable slice** to prove the mechanism end-to-end before
  broadening: `let x = 1+2; f = _: x; in f 0` — x escapes into f; gate-on must
  emit OP_MAKE_ENV/SET_ENV + f-captures-defEnv + f-body OP_GET_ENV(0,0), and
  eval to 3 == gate-off. Then broaden eligibility + byte-id ladder + brute.

### P0.B (partial) + P0.C — DONE
- **P0.B item 1 (`6758acdac`)**: deleted the completed TEMP P2.1-a formals sizing
  probe (per-call formals walk + 2 V3_STATS bumps at both OP_CALL + OP_TAIL_CALL
  sites) + its counters + dump — instrumentation-creep off the OP_CALL hot path
  the trial edits (DEFECT_REVIEW §2.5). Byte-identical; brute 32/32. (Remaining
  P0.B — attrsUsed O(1) extra-arg check, OP_CALL_N Slot→Closure hop, groupBy→C,
  path-coerce — are CPU micro-opts, deferred: likely noise at firefox scale per
  the beat-TW campaign, and needed only before the W5 darwin-4 baseline.)
- **P0.C (`2dbb71da1`)**: RETIRED the env-share intern heuristic (Rule-0
  falsification — the Phase-1 nUp histogram is the falsifier: avg 1.88–2.10 ⇒
  nUp>8 threshold ~never fires; firefox envs=0.1 MB ⇒ shared ~nothing).
  `shareAfter`→UINT32_MAX (default never interns); KEPT the plumbing (Env/
  upvalEnv/closureUpvalue/walkEnv/ENV_SHARED) that W2 reuses + the
  NIX_V3_ENV_SHARE_AFTER A/B override. Gives the trial a clean single-mechanism
  baseline. Byte-id ladder hello/git/firefox IDENTICAL; brute 32/32.

**Remaining Phase-0 (non-v1-blocking; before the W5 darwin-4 baseline):** the 4
P0.B CPU micro-opts, Q1.6 rooting backlog (mechanical), Q1.8 GcRoot-at-scavenge
assert (needs empirical false-fire check).

**Session end-state (2026-07-03):** HEAD = `2dbb71da1`. 16 commits, each
full-`--brute`-gated (32 suites; recurring 31/32 = the let-chain-5000 15s-timeout
flake on this shared host at load avg ~21, verified 3.73s CPU/5.5s wall + passing
standalone — NOT a regression; and the AllocStats-change stale-v3-smoke gotcha,
fixed by rebuild each time). Every GC-critical commit independently
adversarial-reviewed (all NOT REFUTED). Delivered: Gate A = GO (plan's first
decision gate, reliable data) + Phase-0 prereqs + W0 (opcode foundation) + W1
(escape analysis, validated vs C3). W2→W6 is the multi-week remainder; W2
(emission, above) is the risky core — close the precondition first.

#### W2b-runtime EMIT DESIGN — KEY SIMPLIFICATION (2026-07-04): NO HYBRID RESIDUAL
Worked out the full emit and found the plan's "hybrid residual flat FAM" hedge is
UNNECESSARY and higher-risk than a clean single path. Proof: **every free var of a
function F is BY DEFINITION an escaping local of its owner** — v ∈ F.freeVars means
F (a nested function) uses v and v is bound in an ancestor A ⇒ v escaped A into F ⇒
v ∈ E(A). So the eligible-escape set = ALL freeVars; there is nothing left to route
flat. ⇒ For an env-capture function the flat FAM upvalue path (OP_GET_UPVALUE) is
simply UNUSED: the closure/thunk captures ONE `capturedDefEnv` pointer with
`nUpvalues=0` (the model's win, directly). with-targets (lexicalWiths) and
const-remat vars are NOT freeVars, so their existing paths are untouched — no
special exclusion needed. This kills the two-path hybrid + its index-shifting
bookkeeping. Analysis passes (module-level, once, before the emitFunction loop):
1. **ownLocals(F)** = paramVar + extraParams + every Binding.var in F's block-tree
   (entryBlock + If/With/Assert/And/Or sub-blocks, NOT descending into nested
   Lambda/MkThunk funcIdx bodies). Also build **varOwner[v] = F**.
2. **lexParent(F)** = the Function whose block-tree contains the Lambda/MkThunk node
   with funcIdx==F (scan all nodes).
3. **E(A)** (escape set): for every Function G, for every v ∈ G.freeVars, add v to
   E(varOwner[v]). (Propagation guarantees intermediates carry v too.)
4. **createsEnv(F)=|E(F)|>0; envSlotCount(F)=|E(F)|; envSlotOf[F][v]=idx** (0-based,
   stable order).
5. **usesDefEnv(F) = createsEnv(F) OR (∃ v∈F.freeVars : v∈E(varOwner[v]))** — the
   2nd clause (always true when freeVars non-empty, since all freeVars are env-
   routed) also covers FORWARDING: propagation puts an ancestor's escaping var in
   every intermediate F's freeVars, so intermediates get usesDefEnv=true and pass
   the incoming defEnv through (they create no env ⇒ defEnv passes through unchanged).
6. **depth(F,v)** for a ref to v owned by A (emit-time): `d=0; walk=F; while walk!=A:
   if createsEnv(walk) d++; walk=lexParent(walk); return d`. (Own escaping local:
   A==F ⇒ depth 0. Non-env-creating intermediates don't increment ⇒ pass-through.)
EMIT:
- **emitVarRef(v)** under g_envCapture: constRemat→LIT (unchanged); else if v is a
  NON-escaping own local (v∈ctx->slot, i.e. varOwner[v]==F ∧ v∉E(F)) → OP_GET_LOCAL
  (unchanged); else (env-routed) → OP_GET_ENV(depth(F,varOwner[v]),
  envSlotOf[varOwner[v]][v]).  Do NOT populate ctx->upvalue when env-capture.
- **binding store** for an escaping local (v∈E(F)) → OP_SET_ENV(envSlotOf[F][v])
  instead of OP_SET_LOCAL; and such a binding must NOT be #542-deferred (it needs a
  real store into the env). Non-escaping bindings unchanged.
- **prologue**: if createsEnv(F): emit OP_MAKE_ENV(envSlotCount) at entry, then for
  each ESCAPING param/extraParam copy stack→env (OP_GET_LOCAL(paramSlot);
  OP_SET_ENV(envIdx)). (Eager-at-entry, not lazy, for a correct first version — let-
  bindings in the entry block always execute; lazy is a later CPU refinement.)
- **MAKE_CLOSURE/MAKE_THUNK push list**: env-capture ⇒ push NO freeVars (nUp=0),
  only lexicalWiths; runtime sets capturedDefEnv=cur.defEnv (handler, W2b-runtime b).
- **letrec knot**: the env-pointer model handles it NATURALLY (OP_MAKE_ENV allocs
  uninit rec slots; the closure captures the env POINTER; OP_SET_ENV fills the rec
  slot with the closure; later invocations see it) — but it REPLACES the current
  slot-based rec lowering (OP_GET_UPVALUE_REC_BINDING_SLOT etc.), a known bug-nest —
  validate letrec cases explicitly in the brute broadening.
Runtime handlers (W2b-runtime a) DONE; MAKE/frame-entry (b) + thunk tail-slot +
this emit (c) land + byte-id-validate TOGETHER (gate-on==gate-off on the minimal
slice, then hello→git→firefox→python3 + full brute).
