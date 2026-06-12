# Plan: make v3 beat TW on CPU **and** RSS on every gate row (2026-06-12)

**Input**: the 2026-06-11 review was fully executed (46 commits); the fresh gate table
(darwin-4, min user-CPU, byte-identical, peak-RSS via `/usr/bin/time -l`) now reads:

| Row | CPU | RSS | status |
|---|---|---|---|
| attrNames nixpkgs | **0.70×** | 1.30× | CPU won; RSS to close |
| fib 30 | 1.38× | **0.27×** | RSS won; CPU dispatch floor |
| map+length 1e6 | 1.00× | 1.18× | (fused away; not honest iteration) |
| foldl 1e6 | 2.18× | **0.52×** | the iteration floor |
| hello.drvPath | 1.65× | 1.89× | drv path |
| git.drvPath | 1.91× | 1.96× | drv path |
| firefox.drvPath | 2.83× | 2.70× | drv path, context-heavy |

**Goal**: ≤1.0× CPU and ≤1.0× RSS on all seven rows.
**Method**: this plan rests on four fresh code-level analyses (drv-path CPU ledger,
RSS ledger, per-element iteration ledger — *measured* via NIX_VM_OPCOUNTS on the real
binary — and a TW anatomy read). Every work item carries a pre-committed keep/revert
bar (measure-twice) and names the hypothesis it kills (Rule 0). All numbers below the
bars must come from `bench/v3-vs-tw-gate.sh`.

---

## 0. The diagnosis in three paragraphs

**drvPath CPU.** TW's `derivationStrict` is a single C++ pass: one shared
`NixStringContext` set per drv, `coerceToString` returning a zero-copy
`BackedStringView`, one `.toOwned()` per attr, three unparse+SHA256 passes, and the
global `drvHashes` memo making input hashing O(N+E) over the whole eval
(`src/libexpr/primops.cc:1612-2004`, `src/libstore/derivations.cc:854-965`). v3's
libstore tail is structurally identical (`buildAndWriteDrvNative`,
`primops.cc:4687-4948`) — **the gap is upstream of libstore**, in two places:
(a) the bytecode hybrid wrapper executes ~137 insns + 22 thunk allocs per drv plus a
map/filter/listToAttrs pipeline that stringifies every attr key (it was a SymbolId!)
and re-hashes it **three times** (`attrNames` → `vmIntern` → `globalInternSymbol`,
`primops.cc:658, ir.cc:85, vm.cc:9353`), with 2 dispatchLoop entries + 3 full string
copies per env attr; and (b) **the string-context representation**: v3 keeps context
as `vector<std::string>` tokens in a char*-keyed side table, so every dependency edge
lives a *format → deep-copy-per-string-op → re-parse-at-the-consuming-drv* lifecycle
(`encodeStringContext` `primops.cc:322` → copies at every concat/coerce → `absorbCtx`
re-parsing through `NixStringContextElem::parse` per token per consumer,
`primops.cc:5089-5095, 298`), plus a sort+unique over `std::string`s per contexted
concat (`vm.cc:10810-10818`). TW parses context **zero** times on this path. This
class scales with context depth × string traffic — exactly the firefox (2.83×) vs
hello (1.65×) spread.

**drvPath RSS.** Both engines are *cumulative-allocation* numbers, not live sets:
TW's Boehm pre-expands to min(25% RAM, 384 MB) and "in most cases never collects"
(`src/libexpr/eval-gc.cc:47-100`); v3's major GC has **initial threshold 256 MB,
growth 2.0** (vm.cc:335-345) — so hello (239 MB) **never GCs**, and firefox GCs
~twice with whole-block-free-only sweep (Immix line/span reuse still gated off,
`alloc.hh:723/1267`), so the measured 46-50 % dead lines stay resident. On top:
656 KB metadata per 16 MB block (4.1 %), the sweep pushes 8 B per dead cell into a
`freeListBins_` that nothing consumes or clears on the default path
(mark_sweep.cc:878, alloc.hh:1973-1977), SQLite page cache + ImportCache retention,
and the allocation *volume* itself (wrapper Bindings/thunk churn, context token
copies, chain-SELECT materialize copies, 40 B dead thunk headers). The race is
winnable precisely because TW *cannot* reclaim (conservative, never collects, 16 B
Value floor) and v3 *can* (precise GC, 8 B Values) — v3 just currently neither
reclaims nor allocates less.

**foldl CPU (measured: 10 dispatches/element, 0 B/element allocated).** v3 loses
2.18× on pure orchestration, not allocation (TW allocates ~88 B/elem from Boehm
freelists; v3 allocates zero). Per element v3 pays: **two full `dispatchLoop`
re-entries** (one for the fold body via `callClosure2`, one for the genList
callback's *single* `OP_R_RETURN` — the latter only because the R_RETURN peephole
(emit.cc:2333-2355) breaks identityLambda detection (emit.cc:2513-2528), a
straight-up bug), each re-entry re-paying ~8 function-local static guards, a
`kAnySlowGate` recompute, and an **unconditional OPCYCLES TLS save/restore**
(vm.cc:2874-2892); plus 4 of the 10 dispatched opcodes are dead stack motion the
emitter fuses in one expression shape but not another (verified disasm divergence);
plus a per-iteration `majorGcEnabled() && exitDepth==0` test that is loop-invariant
(vm.cc:3193-3204).

---

## 1. Phase 0 — zero/near-zero-code falsifiers (run first, ~1 day, darwin-4)

These bound every later investment; do not skip (measure-twice).

| # | Probe | Kills/keeps |
|---|---|---|
| 0.1 | Gate hello/git/firefox with `NIX_V3_NO_BC_DERIVATION_HYBRID=1` (routes to the all-C `primDerivationStrictNative` that iterates Bindings directly, `primops.cc:5231`) | Isolates the wrapper's share of drv CPU (W1-W3). If the C arm is materially faster AND survives deep graphs (the wrapper exists only to break C-stack recursion — iterative op_force_slow may have obsoleted that, `bytecode_primops.cc:704-721`), **retire the wrapper** instead of optimizing it |
| 0.2 | `NIX_V3_MEM_BUCKETS=1` decomposition of hello-239 MB and firefox-1294 MB **at HEAD** | The existing ledger docs predate the last 46 commits; re-pin arena/boehm/elsewhere before committing RSS bars |
| 0.3 | Gate hello + firefox with the major-GC initial threshold forced low (e.g. 32 MB) and growth 1.5 | Tests "peak = cumulative because GC never fires / fires too late". Expect: RSS drops materially, CPU pays some mark cost (mark is O(log) per pointer post-P-7). The CPU/RSS trade curve from this run calibrates Phase 3's default |
| 0.4 | Add a 20-LoC memo on `NixStringContextElem::parse` keyed by token at `v3InsertContextToken` (`primops.cc:298`) and gate firefox | Direct falsifier for "context parse-back is material" before funding the full interning lever |
| 0.5 | One-line: stop pushing dead cells into `freeListBins_` when reuse is off (mark_sweep.cc:878) | Free RSS + sweep CPU; also removes a confound from 0.3 |

---

## 2. Phase 1 — drv-path CPU: stop re-doing per-edge work TW does once

**Lever 1.1 — interned string context (the big one).** Keep the char*-keyed side
table, but store context as a span of **interned context-element ids** (global
token ↔ u32 table with one cached parsed `NixStringContextElem` per id). Producers
intern once at the drv result (`primops.cc:4888/4910`); copy = span share/copy of
u32s; merge at STR_CONCAT/coerce = small sorted-int union (kills the
sort+unique-over-strings at `vm.cc:10814-10816`); the consuming drv's `absorbCtx`
uses the cached parsed elem — **zero re-parsing, zero per-edge string copies**.
~63 touch sites, mostly mechanical (`primops.cc:314-341` becomes intern/deref).
This beats even TW, which still re-parses at `copyContext` (`eval.cc:2585-2590`) —
one of TW's named weaknesses.
*Effort 2-4 d. Risk: drv-hash-critical → byte-identity on the full corpus is the
hard check. Bar: hello.drvPath CPU ≤1.35× and firefox ≤2.2× after this lever alone,
else re-diagnose before continuing. Hypothesis killed if 0.4's memo shows <3 % —
then the cost is in the copies, and the lever narrows to span-sharing only.*

**Lever 1.2 — wrapper disposition (decided by 0.1).**
- If the C arm wins and holds depth: flip `NIX_V3_NO_BC_DERIVATION_HYBRID` semantics
  (C path default, wrapper opt-in), stress deep-graph C-stack first (HNE/M5).
- If the wrapper must stay: (a) replace `attrNames`+`args.${k}` with an internal
  iterate-entries primop yielding SymbolId-keyed values — removes n string allocs +
  the 3× key hashing per drv; (b) drop the leaf chain-materialize copy
  (`primops.cc:5205-5217`) by iterating the chain cursor directly.
*Bar: ≥5 % drv-row CPU or revert. Expected from the ledger: 5-15 %.*

**Lever 1.3 — context fast paths (independently shippable, subsumed by 1.1):**
single-contexted-part move instead of copy+sort in OP_STR_CONCAT; reserve ctxAccum;
skip sort when ≤1 entry (`vm.cc:10768-10818`). *0.5-1 d, low risk.*

**Lever 1.4 — memo-trio parity check (cheap audit, not code):** confirm v3 hits
`nix::drvHashes` once per drv (it does — `primops.cc:4874`, shared with TW), that
`srcToStore`-equivalent memoization covers v3's path-coercion sites, and that
`ImportCache::results` gives per-file value sharing equivalent to `fileEvalCache`.
Any miss here is an O(N²)-class regression hiding behind constants.

**Non-goals (already parity):** the libstore tail (unparse/SHA/writeDerivation) is
identical code; the 3×-unparse inefficiency is TW-shared — a *later* differentiator
(single-pass hashing) once we're at parity, not on the critical path now.

---

## 3. Phase 2 — RSS: allocate less, then actually reclaim (TW can do neither)

Ordering matters: volume reducers first (they also cut CPU), then the collector
flip, because a smaller, churnier heap maximizes what line reuse recovers.

**Lever 2.1 — GC trigger policy (from 0.3's curve).** Default initial threshold
down from 256 MB to ~32-64 MB with growth ~1.5, trigger on *post-sweep live* rather
than mapped bytes, so hello-class workloads GC 1-3 times instead of never.
*Bar: hello RSS ≤150 MB at ≤+5 % CPU; revert the default (keep the knob) if the CPU
toll exceeds that.*

**Lever 2.2 — flip Immix line/span reuse default-ON.** The two correctness prereqs
the review demanded are now landed (matMemo GC-coherent a3fc6c708; string-context
side-table swept against the mark bitmap 6fa9664e5; recSlotCache name-validated).
Soak with `NIX_V3_GC_STRESS` + run-pap suites + full corpus byte-identity first.
With 2.1, dead lines (46-50 % of blocks) become allocatable instead of resident.
*Bar (pre-committed): −150 MB firefox / −60 MB hello at ≤2 % CPU, else keep gated
and escalate per GC_PAUSE's re-entry clause.*

**Lever 2.3 — allocation-volume reducers** (each also CPU-positive on drv rows):
context token copies (Phase 1.1), wrapper churn (1.2), and the chain-SELECT
materialize policy: SELECT on a chain currently materializes a full flat copy that
the memo then retains alongside the chain (double storage; `vm.cc:8723`,
`value.cc:107-163`). With the per-GC-epoch memo clear in place, evaluate
lookup-without-materialize for SELECT again — the −132 MB firefox win that was
deferred for the shared-parent writeback hazard is now defensible because the
writeback sites are PAP-guarded + disarm-fixed (C-1) and name-validated (C-3).
*Bar: −80 MB firefox byte-identical, else stays deferred.*

**Lever 2.4 — fixed-cost diet:**
- Per-block metadata 656 KB/16 MB → lazy lineMarks allocation (only when GC has
  ever run) and pack cellStarts; target <300 KB/block.
- SQLite page cache: set `PRAGMA cache_size` to a sane cap (it is unbounded today;
  the in-memory shadow was ~270 MB on HNE-class); `NIX_V3_NO_DISK_CACHE` rows in the
  gate confirm the delta.
- ImportCache: default `NIX_V3_IMPORT_CACHE_MAX_ENTRIES` to a real cap (the LRU
  exists, default 0 = never) and re-measure post-munmap (the PHASE_4B falsification
  premise is gone — threshold-recalibration rule applies).
- attrNames-row specific (153 vs 118 MB): the excess is CU bytecode + IC vectors +
  symbol tables for all of nixpkgs retained in `ImportCache::cus` — the cap plus
  per-CU IC trimming (AttrSelectIC is 64 B/call-site) is the lever.

**Why this beats TW**: TW's 126/479 MB is uncollected churn it structurally cannot
reclaim (conservative GC, never fires below 384 MB, 16 B Value floor, Env chains).
v3 with 8 B Values + precise reclaim + reuse has a lower floor on both axes; after
2.1+2.2 the drv rows' RSS should track v3's *live* set (~Bindings live ≈ 400 MB on
firefox per live-trace) rather than cumulative — i.e. below TW is achievable, not
aspirational.

---

## 4. Phase 3 — the iteration floor: foldl 2.18× → ≤1.0×

From the measured per-element ledger (10 dispatches, 2 dispatchLoop re-entries,
0 allocations). In order:

| # | Lever | Mechanism | Expected | Bar (keep / revert) |
|---|---|---|---|---|
| 3.1 | **identityLambda fix** (bug) | extend emit.cc:2513 to accept the 1-insn `OP_R_RETURN 0` body the peephole produces; restores the no-frame fast paths (vm.cc:7518-7525, 13851, 12067, 5539) for every `x: x`/`lib.id`/genList-gen callback | −15-30 % on foldl row; helps any genList/map workload | ≥8 % / <3 % |
| 3.2 | **Peephole: GET_LOCAL+FORCE → GET_LOCAL_FORCE + dead-temp elision** | generic post-pass; also fixes the verified emitter inconsistency where the same lambda fuses under one expression shape but not under `let` | body 9→~4 dispatches; −10-20 % | ≥6 % / <2 % |
| 3.3 | **Lightweight dispatch entry** | OPCYCLES TLS save/restore behind its gate (vm.cc:2874/2891 — unconditional today); fold the ~8 entry statics into one startup-init POD; hoist `majorGcEnabled() && exitDepth==0` to a pre-loop const | −5-10 % | ≥4 % / <2 % |
| 3.4 | **OP_FOLD resident superinstruction** | fold driver as an opcode using the existing ip-rewind re-entry protocol (cursor in CallFrame, list/acc in frame slots); body OP_RETURN pops back to the fold frame; **zero dispatchLoop re-entries**. Distinct from the falsified bytecode-foldl' (that was a Nix-level rec-let loop; this fuses the C loop into the dispatch state machine) | with 3.1+3.2: projected ≤1.0-1.3× TW (v3 allocates 0 B/elem vs TW 88 B) | ≥20 % / <10 % |
| 3.5 | **Computed-goto dispatch** | after 3.1-3.4 the per-dispatch share rises as dispatch count drops 10→~5; also moves fib (1.38×) | −8-20 % on dispatch-bound rows | ≥8 % / <4 % |

fib 30 (1.38×): expected to ride 3.3 + 3.5; if it doesn't reach ≤1.1×, profile the
call path separately (OP_RETURN teardown breadth — 2 vector resizes + 3 static
guards per return, vm.cc:6648 — is the named suspect; fold into 3.3's cleanup).

**Do not re-propose (falsified, with commits):** closure arity-cache byte
(6b5284374), OP_LESS opcode, reuseScope-TLS-skip-as-sole-lever, bytecode foldl',
ValuePair 24/32 split (38c263bdb), string-value dedup, Boehm tuning.

**Measurement discipline for this phase:** NIX_VM_OPCYCLES is structurally blind
across nested dispatch loops (attribution resets at the boundary) — validate with
the gate + NIX_VM_OPCOUNTS only.

---

## 5. Phase 4 — drv-path RSS+CPU interaction & the endgame differentiators

Once Phases 1-3 land, re-run the full 7-row table. Remaining headroom in order of
expected value:

1. **Single-pass drv hashing** (beats TW, not just matches): TW pays 3 unparse+SHA
   passes per drv over near-identical multi-KB texts (`derivations.cc:107-126, 635,
   854-965`); v3 shares that code today. An incremental/cached unparse at
   `buildAndWriteDrvNative` cuts ~2/3 of per-drv hashing for both arms of the FFI —
   v3-only win since TW won't take the patch from us.
2. **Thunk-header recycling**: 40 B dead header per forced thunk; with line reuse on
   (2.2) these become allocatable automatically — verify via live-trace that
   Thunks' live% drops; if headers still dominate a bucket, a header→cell collapse
   for nUpvalues==0 thunks is the follow-on.
3. **attrNames-row CPU (already 0.70×)**: protect it — the GC-threshold change
   (2.1) must not regress the one row we already win; the gate run for 2.1 includes
   it.

**Success criteria (the whole plan):** all seven rows ≤1.0× CPU and ≤1.0× RSS,
byte-identical, ENGAGED — plus lang 143/143, run-pap suites, scaling-check linear.
**Projected post-plan table** (honest, from the ledgers): foldl ~0.9-1.2× CPU
(3.4-dependent) / 0.5× RSS; hello ~1.0-1.2× CPU / ~0.6-0.9× RSS; firefox
~1.1-1.4× CPU / ~0.5-0.8× RSS. CPU on firefox is the row most likely to need the
Phase 4 hashing differentiator to cross 1.0× — flag it now so nobody declares
failure early.

## 6. Risk register

- **Context interning (1.1) is drv-hash-critical** — every byte of every produced
  .drv must stay identical; run the full-sweep harness, not just the 7 rows.
- **GC threshold (2.1) trades CPU for RSS** — both numbers go in every report
  (memory-first-class rule); the knob stays for ops escape.
- **Immix flip (2.2) converts latent stale-pointer bugs into live ones** — the
  review's cache-coherence fixes are exactly the prereqs; soak under GC_STRESS
  before default-on, and keep the one-line opt-out.
- **Wrapper retirement (1.2) risks C-stack depth** — stress HNE/M5 graph depth
  before flipping; keep the wrapper as the opt-in fallback for one release.
- Every lever lands as its own commit with the A/B in the commit body (Rule 0).

---

*Compiled 2026-06-12 from four parallel code-level analyses (drv CPU ledger, RSS
ledger + 3 sub-probes, measured per-element iteration ledger, TW anatomy).
Line numbers at HEAD post-46-commit review execution.*
*Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output
Group. SPDX-License-Identifier: Apache-2.0.*
