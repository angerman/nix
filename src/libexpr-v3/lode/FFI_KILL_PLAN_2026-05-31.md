# FFI bridge kill + marshalling-minimization — executable plan

**Date:** 2026-05-31
**Per:** user directive "Let's focus on killing as much of the ffi bridges (and only marshalling/passing the absolute minimal data the relevant ffi calls need). See FFI_BRIDGE_INVENTORY_2026-05-31.md, ultrathink, make no mistake, take all the time you need to break this down into a clean executable sequence of clear, well reasoned steps."

Companion docs:
- [`FFI_BRIDGE_INVENTORY_2026-05-31.md`](FFI_BRIDGE_INVENTORY_2026-05-31.md) — authoritative current-state inventory (input to this plan)
- [`EXIT_POST_APP3_BASELINE_2026-05-31.md`](EXIT_POST_APP3_BASELINE_2026-05-31.md) — measurement baseline
- [`GC_AND_MEMORY_ACCOUNTING_AUDIT_2026-05-31.md`](GC_AND_MEMORY_ACCOUNTING_AUDIT_2026-05-31.md) — accounting flaws to avoid claiming false wins against

**Status:** STRATEGIC EXECUTION PLAN — five phases (A–E), each with pre-committed gates, falsification criteria, rollback strategy, and measurement protocol.

---

## 0. Framing & non-goals

### 0.1 What this plan DOES

* Kills FFI bridges that have 0 measured callers (BP3 retirement).
* Eliminates the LARGEST bridge entry class via `primDerivationStrictNative` extensions (the headline lever — closes the 99.8% bridge-retention finding at source).
* Minimizes marshalled data through the irreducible FFI leaves (fetchers / realisePath / store ops).
* Adds deduplication to prevent the same v3 Bindings being bridged into N distinct table entries.

### 0.2 What this plan does NOT do

* Does NOT retire FFI leaves themselves — `realisePath`, `libfetchers`, `store->buildAndWriteDrv` are V3-NATIVE-permitted boundaries and must remain TW. (V3-NATIVE rule per CLAUDE.md §1.1.)
* Does NOT pursue architectural lazy-bridge work (`Kind::LazyTW` Bindings, L1 defer-forceValue) — those are multi-session and risk a 5-pivot situation per session-arc history.
* Does NOT chase the static "bridge count" metric — per inventory §1, that metric is misleading; the levers are RETENTION and MARSHALLING SHAPE.

### 0.3 The meta-discipline

Per session-arc memory ([[measure-twice-cut-once]] + [[falsification-rule]] + [[chain-bindings-phase-c-falsified]]):

* **Every phase has a pre-committed SHIP gate set BEFORE the phase begins.**  Failing the gate triggers REVERT, not threshold recalibration (unless the original premise itself is measurably wrong per [[threshold-recalibration-rule]]).
* **Three failed pivots on the same premise = falsification.**  If Phase C derivationStrict extensions falsify at __structuredAttrs, do NOT pivot to a fourth shape — close the phase and document.
* **No carcasses behind opt-in gates.**  Per measure-twice §3.7: removed callers + removed implementation; only the cleanest staging-point helpers may stay.
* **Every commit answers "what hypothesis does this kill?"** Rule 0.

---

## 1. Pre-flight (Day 0)

### 1.1 Verify current measurement baseline holds

* Re-confirm `EXIT_POST_APP3_BASELINE_2026-05-31` numbers reproduce on today's commit (`3b6be11ec` already includes this; just check no drift since).
* HNE arena = 1476.4 MB (deterministic counter, the LOAD-BEARING reference)
* M5 arena = 5838.5 MB
* hello.drvPath arena = 553.6 MB

Action: re-run `N=3 src/libexpr-v3/bench/baselines/2026-05-31-post-app3-combined/measure.sh hne` to spot-check.

### 1.2 Establish bridge-table size baseline

Capture three new numbers BEFORE any work:

```bash
NIX_V3_DIRECT_EVAL=1 NIX_VM_STATS=1 NIX_V3_NO_DISK_CACHE=1 \
  NIX_V3_MAX_HEAP=4G NIX_V3_MAX_WALL_TIME=120s \
  NIX_V3_NO_NATIVE_CALL_FLAKE=1 \
  ./builddir/src/nix/nix eval --impure --expr \
    '(builtins.getFlake "$HNE").packages.x86_64-linux.hello.drvPath' \
  2>&1 | grep -E '^v3-direct bridge|v3BridgeClosures|v3BridgeAttrs|v3BridgeLists'
```

Record:
* `v3BridgeClosures.size()` at end-of-eval
* `v3BridgeAttrs.size()` at end-of-eval
* `v3BridgeLists.size()` at end-of-eval
* `__v3_call_bridge_1=N __v3_force_attr=M __v3_force_list_elem=K` per `BRIDGE_TELEMETRY_2026-05-26`

Per the inventory's BP3=0 calls finding: confirm `__v3_force_list_elem=0` reproduces on hello / HNE / firefox.

Save raw output to `bench/baselines/2026-05-31-post-app3-combined/bridge-tables-baseline.txt`.

### 1.3 Snapshot lang-test pass count

```bash
nix develop -c bash -c 'cd src/libexpr-v3 && ./test/run-lang-tests --quiet 2>&1 | tail -5'
```

Baseline: per `EXIT_DAY4_TAG_APP3_LANDED` claims, 143/143 lang tests PASS.  Confirm; if not, fix that first.

### 1.4 Correctness gate definition (used by every subsequent phase)

A phase passes correctness ONLY when:

1. `--quick 6/6` PASS (existing test suite)
2. `--core 15/15` PASS
3. lang tests: same N/N as baseline
4. **drvPath byte-equal** on:
   * `(import <nixpkgs> {}).hello.drvPath`
   * `(builtins.getFlake HNE).packages.x86_64-linux.hello.drvPath`
   * `(builtins.getFlake CN).packages.aarch64-darwin.cardano-node.drvPath`
5. No new `V3_DRV_DEBUG` fallback diagnostics on the three workloads above
6. No regression in `--brute 11/11` (per `[[brute-in-ci-2026-05-21]]`)

Any phase that fails this gate REVERTS.

---

## 2. Phase A — Hygiene wins (Day 1; ~6 hours)

Goal: remove dead code; tighten one BP2 fast path.  Pure semantics-preserving.

### 2.1 A1: BP3 retirement (`__v3_force_list_elem_inner`)

**File:line:** `primops.cc:5183-5400` + registration

**Hypothesis killed:** *"BP3 has any live caller we don't know about."*

**Action:**
1. Confirm `__v3_force_list_elem=0` on ALL of: hello, HNE, M5, firefox.drvPath, gcc.drvPath, bash.drvPath, python3.drvPath.  If ANY workload shows ≥1, ABORT (BP3 is not safe to retire).
2. Add an instrumented "BP3 caller" trap (1 day's measurement) before deletion:
   ```cpp
   // primops.cc:5187, at primV3ForceListElem entry:
   std::fprintf(stderr, "v3 BP3-HIT: callstack-marker for retirement audit\n");
   ```
3. Re-run the full workload matrix; confirm 0 fires.
4. Remove `primV3ForceListElem` (lines 5183-5400).
5. Remove the registration call (search `__v3_force_list_elem`).
6. Verify build + correctness gate (§1.4).

**Pre-committed SHIP gate:** correctness gate passes AND no `BP3-HIT` print in any workload.

**Falsification criterion:** if ANY workload shows ≥1 BP3 call during the 1-day measurement window, the retirement hypothesis is killed.  Document, restore, move on.

**Rollback:** `git revert` is clean (single commit, no entangled changes).

**Yield:** ~217 LoC removed; -1 primop registered with TW; code hygiene.  No expected RSS/wall change (it wasn't firing).

**Effort:** 0.5-1 day total (mostly measurement window).

### 2.2 A2: L2 — short-circuit `primV3ForceAttr` when value is already a Bridge thunk

**File:line:** `primops.cc:5099` (the `*found` access path in `primV3ForceAttr`)

**Hypothesis killed:** *"Every `__v3_force_attr` call must perform a full v3-side force regardless of value tag."*

**Today's behaviour:**
* TW calls `__v3_force_attr(handle, attrName)`.
* v3 looks up the bridge entry → finds attrset → looks up attr → returns Value.
* If the Value is itself a Bridge thunk (a TW Value bridged INTO v3), force chain is: TW Value → v3 Bridge thunk → force triggers `treeWalkerToV3(...)` → returns a NEW v3 Value → bridged BACK to TW.
* Double-bridge round-trip when the value was already TW.

**Action:** detect `*found` is a Bridge thunk pointing at a TW Value; return the original TW Value directly to TW (skip the round-trip).

**Inspect first:** verify the round-trip pattern actually happens.  Add a counter at `primops.cc:5099` for `bridge-thunk-fast-path-eligible` and measure on HNE.  If count < 100, the optimization is below noise floor — DROP and document.

**Pre-committed SHIP gate:**
* Correctness gate (§1.4) passes.
* Measured BP2 wall time on HNE strictly decreases (per `NIX_VM_PRIMOP_TIME=1` line for `__v3_force_attr_inner`).
* If wall is within noise, the patch SHIPS only if eligible-count is ≥1000 (architectural-value vote, even if no measurable wall delta).

**Falsification criterion:** if the count is < 100 AND wall is within noise, drop the patch.

**Effort:** 0.5 day.

### 2.3 A3: Pre-Phase-B measurement re-baseline

After A1+A2 land:
* Re-run §1.2 (bridge-table size) and §1.4 (correctness).
* Re-run HNE + M5 N=10 measurement.
* Save as `bench/baselines/2026-05-31-post-app3-combined/phase-a-post.json`.
* Commit a doc snippet recording delta from baseline (expected: ~0 because A1 was dead code and A2 is fast-path).

**Phase A SHIP gate:** correctness gate passes AND we have clean measurement before starting Phase B.

---

## 3. Phase B — D1 dedupe (Day 2-3; 1-2 days)

Goal: dedupe `v3BridgeAttrs` / `v3BridgeClosures` / `v3BridgeLists` entries when the same v3 pointer is bridged twice.  Per inventory §5 D1, projected ~50% reduction on M5 entry count.

### 3.1 B1: Dedupe `v3BridgeAttrs` entries

**File:line:** `primops.cc:5565` (the `push_back` in `v3ToTreeWalker`'s attrset branch)

**Hypothesis killed:** *"Same v3 Bindings* bridged multiple times produces distinct semantics for TW."*  (It doesn't — handles are interchangeable; the table is a pure cache.)

**Today's behaviour:**
```cpp
// primops.cc near :5565
auto handle = tbl.size();
tbl.push_back(BridgeAttrEntry{v3Value, /*fallback*/{}, ...});
return handle;
```

Same Bindings* called twice → handle 100 and handle 101.  Two entries, same content.

**Proposed change:**

```cpp
// Add per-table reverse map:
static std::unordered_map<const void *, size_t> & v3BridgeAttrsByPayload() {
    static std::unordered_map<const void *, size_t> m;
    return m;
}

// Before push_back, lookup:
const void * key = v3Value.payload.bindings;
auto it = v3BridgeAttrsByPayload().find(key);
if (it != v3BridgeAttrsByPayload().end()) {
    ++allocStats().bridgeAttrsDedupHits;
    return it->second;  // reuse existing handle
}
auto handle = tbl.size();
tbl.push_back(BridgeAttrEntry{v3Value, ...});
v3BridgeAttrsByPayload().emplace(key, handle);
return handle;
```

**Concerns to address:**

1. **Eviction interaction (Stage 2 weak bridges):** if entry at handle K is evicted, we MUST remove the reverse-map entry.  Hook into `evictBridgeEntry` at `primops.cc:4081-4158`.  Skip-this-step concern: if eviction is rare in practice (per `WEAK_BRIDGE_EVICTION_DESIGN_2026-05-29` — Stage 2 is memory-INERT), defer the eviction-handler; just track a "stale handles" counter and verify it stays at 0.
2. **TW's `forceValue` chain mutation:** `tryUnwrapBridge1Closure` is documented safe per inventory §5 D1 (compares handle ints, not pointers).  Same applies for attrs path; verify via the existing `v3BridgeUniquePtrCounts` diagnostic at `primops.cc:4369`.
3. **Stale-pointer concern:** could `v3Value.payload.bindings` get reused (cell freed and re-allocated as another Bindings)?  Today's arena is monotonic-grow (per the accounting audit §2.1) — no cell-level free.  So pointer reuse can't happen.  **This invariant must be re-verified if Stage 6 default-on lands later.**

**Pre-committed SHIP gate:**
* Correctness gate (§1.4) passes.
* `bridgeAttrsDedupHits / (hits + push_backs)` ≥ 25% on M5 (the projected 50% is a hypothesis; 25% is the SHIP threshold).
* M5 `v3BridgeAttrs.size()` reduction ≥ 25%.
* Wall regression ≤ 1% (lookup is O(1) average; tight tolerance).
* `--brute 11/11` PASS (catches dedupe-vs-eviction interaction bugs).

**Falsification criterion:**
* If dedupe hit rate < 10% → not worth the bookkeeping; REVERT.
* If wall regression > 1% → lookup cost dominates win; REVERT.
* If correctness breaks → fundamental dedupe-vs-mutation bug; REVERT with detailed RCA.

**Effort:** 1 day implementation + 0.5 day measurement.

### 3.2 B2: Apply same pattern to `v3BridgeClosures` and `v3BridgeLists`

Once B1 lands and PASSES gate, replicate for the other two tables.

**Effort:** 0.5 day (mechanical replication).

**Same SHIP gate as B1.**

### 3.3 B3: Measurement closure for Phase B

* Re-run M5 N=10 + HNE N=10.
* Compare against §1.2 baseline.
* Write `EXIT_PHASE_B_DEDUPE_LANDED_<date>.md` with the actual numbers.

**Phase B SHIP gate:** combined B1+B2+B3 deliver:
* ≥ 25% reduction in total bridge-table entry count on M5
* Correctness gate passes
* No wall regression > 1%

If Phase B fails the gate AT ALL, REVERT and skip directly to Phase C (the headline lever).

---

## 4. Phase C — derivationStrict native completion (Day 4-15; 1-2 weeks)

**This is the headline phase.** Per inventory §3.1 and action ladder #1, completing `primDerivationStrictNative` to cover __structuredAttrs / outputChecks / content-addressed eliminates the largest bridge entry class.  Projected yield: most of the 311 MB at `all-packages.nix:9112` bridge retention on HNE disappears.

Current state per `primops.cc:6387-6390`: `isSimpleDerivationAttrs(b)` returns `b != nullptr` (just non-null check).  The actual filtering is INSIDE `primDerivationStrictNative` via try/catch → fallback to bridge.

The three missing cases (per source comment at `primops.cc:6418-6421`):
* `__structuredAttrs` (JSON-encoded env vars)
* `outputChecks` (per-output constraint validation)
* `__contentAddressed` (CA hash mode)
* `__impure` (impure derivations — less critical)

**Strategy:** sub-phase per case.  Land each independently with its own SHIP gate.

### 4.1 C0: Inventory the actual fallback population

Before any code change, measure WHICH cases fire `drvNativeFallbacks` on:
* hello.drvPath (small reproducer)
* HNE (medium)
* M5 (full nixpkgs)

Action:
1. Set `V3_DRV_DEBUG=1` + `V3_DRV_STATS=1`.
2. Run each workload.
3. Capture the fallback reason histogram (per-derivation name + error message).
4. Save to `bench/baselines/2026-05-31-post-app3-combined/drv-fallback-histogram.txt`.

This determines sub-phase ordering: implement the cases that fire MOST FIRST.  If __impure never fires on M5, deprioritize it.

**Effort:** 0.5 day.

**Hypothesis to confirm:** *"__structuredAttrs is the most-common fallback cause on M5."*  If the histogram disagrees, re-order sub-phases.

### 4.2 C1: Sub-phase B — `__structuredAttrs` support

**Background:** `__structuredAttrs = true` causes drvs to encode env vars as JSON instead of strings.  TW's `derivationStrict` handles this via `attrsToStructuredAttrs()` (libstore).  Native impl must replicate the JSON encoding rules + the `.json` env-var fan-out.

**File:line:** `primops.cc:6791+ (buildAndWriteDrvNative)` is the shared helper used by both native and bridge paths.

**Approach:**

1. Read TW's reference implementation: in `src/libexpr/primops.cc` find `derivationStrictInternal` and the `__structuredAttrs` branch.  Identify exactly which fields it produces and how.
2. Implement `v3StructuredAttrsToJson(state, bindings)` — walks v3 Bindings recursively, produces JSON identical to TW's output.  Use the existing native `toJSON` in v3 (already NATIVE per inventory §2.4 T2).
3. Wire into `primDerivationStrictNative` at the env-var processing step: if `__structuredAttrs == true`, emit `.json` aux file via JSON serialization; otherwise emit string env vars.
4. Test on a synthetic `pkgs.runCommand` with `__structuredAttrs = true; passAsFile = [...]`.

**Pre-committed SHIP gate:**
* C1 must produce **byte-identical drvPath** to TW for:
  1. `nixpkgs.coreutils` (uses __structuredAttrs)
  2. `nixpkgs.python3` (uses __structuredAttrs)
  3. Any other __structuredAttrs-using drv in the C0 fallback histogram top 10
* `drvNativeFallbacks` count for "structuredAttrs" must drop to 0 on each workload.
* Correctness gate §1.4 passes.

**Falsification criterion:** if drvPath diverges on ANY of the 3 reproducers, REVERT.  Do NOT iterate without RCA.  If the divergence is a libstore-internal hash difference (not our JSON encoding), the task is OUT OF SCOPE and falls to a libstore patch (escalate).

**Effort:** 3-5 days.

**Rollback:** the new code path is enabled by extending the try-block in `primDerivationStrictNative`; on revert just remove the structuredAttrs branch and re-introduce the throw.  The bridge fallback path is unchanged through this phase.

### 4.3 C2: Sub-phase C — `outputChecks` support

**Background:** `outputChecks = { allowed/disallowed/required {Requisites,References} }` are per-output constraints validated against the closure of the realized output.  TW handles this via `Derivation::checkInvariants` + lateral validation in `Worker::resolveAndScheduleAllDependants`.

**Critical observation:** `outputChecks` are STORED in the drv (`Derivation::env`) and consumed at REALIZATION TIME, not evaluation time.  At eval time we just need to serialise the constraint values into the appropriate env vars AND the appropriate `Derivation` fields.

**File:line:** same as C1 — extend `buildAndWriteDrvNative`.

**Approach:**

1. Read TW's reference: `derivationStrictInternal` handling of `outputChecks` attr → populates `drv.env["allowedReferences"]` etc.
2. Implement v3-side: when `outputChecks` is in input attrset, iterate per-output, populate the matching drv.env entries.  Reuse v3's existing string coercion (already NATIVE).
3. Test on `nixpkgs.busybox-sandbox-shell` (uses allowedReferences) or similar.

**Pre-committed SHIP gate:** byte-identical drvPath on 3 reproducers using `outputChecks`.  Same as C1.

**Effort:** 2-3 days.

### 4.4 C3: Sub-phase D — `__contentAddressed` support

**Background:** content-addressed derivations have hash modes (`text`, `recursive`, `flat`) and produce CA-store paths instead of input-addressed.  TW handles via `Hash::hashCA` + `makeFixedOutputPathFromCA`.

**File:line:** v3 has `primToFile` doing similar CA store path computation (`primops.cc:11709` per inventory).  The infrastructure exists.

**Approach:**

1. Identify the CA-specific code in TW's `derivationStrictInternal`.
2. Extract / reuse v3's CA path computation from `primToFile`.
3. Wire into `primDerivationStrictNative`.

**Pre-committed SHIP gate:** byte-identical drvPath on 3 CA-using reproducers.

**Effort:** 2-3 days.

**Optional skip:** if C0 histogram shows __contentAddressed is < 5% of fallbacks on M5, defer C3 to a follow-up.  CA drvs are a minority pattern in standard nixpkgs.

### 4.5 C4: Sub-phase E — `__impure` support

**Background:** `__impure = true` marks a derivation as having non-reproducible inputs.  This is essentially a flag passed to the store; little marshalling work.

**Effort:** 0.5 day.  Likely just a flag plumb-through.

### 4.6 C5: Phase C measurement closure

Once C1+C2+(C3)+C4 land:

1. Re-run M5 N=10, HNE N=10, hello.drvPath N=10.
2. Capture `v3BridgeAttrs.size()` end-of-eval — expected SIGNIFICANT reduction.
3. Capture `drvNativeFallbacks` count — expected near-zero.
4. Capture `v3_arena` deltas — expected reduction on M5 due to less bridge retention.
5. Capture `peak_rss` deltas with σ envelope.

**Pre-committed Phase C SHIP gate:**
* All sub-phase C1+C2+C3+C4 individually pass their gates.
* `drvNativeFallbacks` count drops by ≥ 90% on M5 (vs §1.2 baseline).
* `v3BridgeAttrs.size()` drops by ≥ 25% on M5 (the 311 MB retention claim's surface).
* HNE arena reduction ≥ 50 MB OR M5 peak RSS reduction ≥ 50 MB.

**Phase C falsification:** if `drvNativeFallbacks` doesn't drop substantially despite all sub-phases landing, the inventory's "311 MB retention originates at derivationStrict bridge" hypothesis is FALSIFIED.  Open a fresh investigation; do NOT extend Phase C further.

### 4.7 C6 (optional): Retire the TW-bridge fallback in `primDerivationStrict`

If C5 shows `drvNativeFallbacks = 0` across all measured workloads:
* The fallback at `primops.cc:6619-6663` is dead code.
* RETIRE it.  Remove the entire `if (state.nixEvalState && ...)` block.
* `primDerivationStrict` becomes a 1-line wrapper: `primDerivationStrictNative(state, args, out);`
* Verify: any drv shape that previously fell back now THROWS a useful native error.

**Pre-committed gate for C6:** all workloads in `bench/baselines/2026-05-31-post-app3-combined/` AND nixpkgs builds across multiple architectures must drvPath-equal-TW.

This is the actual KILL of the headline bridge.

---

## 5. Phase D — Fetcher arg minimization (Day 16-20; optional, ~1 week)

Goal: minimize the data crossing into TW for the 8 fetcher primops (F1-F8 per inventory §2.4).  Currently each fetcher round-trips full attrsets through `v3ToTreeWalker`.

### 5.1 Strategic question

Fetchers are NOT hot on cache-hit evals (HNE: 0 calls; M5: similarly low).  Per inventory §3.5 O5: "Win on HNE: small."  Phase D is **deferred unless C5 measurement shows fetcher bridges are now the next-biggest contributor**.

Decision criterion at end of Phase C:
* If fetchers contribute ≥ 50 MB to HNE or M5 bridge retention → proceed with Phase D
* Otherwise → SKIP Phase D and close the plan after Phase C

### 5.2 If proceeding: per-fetcher flat-arg variants

Pattern per inventory §3.5: replace `__fetchTree { url, rev, sha256, type }` with `__fetchTreeFlat(url, rev, sha256, type)`.

* Wire at v3 emit: when `lowerExpr` sees `fetchTree { ... }` with a STATIC attrset, lower to `__fetchTreeFlat` instead.
* For dynamic attrsets, keep the old path (still bridges through).
* Per fetcher: ~1 day.

**Effort:** 5-7 days total if all 8 fetchers covered.

**Pre-committed SHIP gate per fetcher:** byte-identical fetch result on 3 representative URLs.

---

## 6. Phase E — Deferred items (NOT in this plan; documented for context)

Per inventory §10 action ladder #9, #10:

### 6.1 O3 (shared lazy primop for bridge attrs)

* Eliminates per-entry `vName` + `vApp` TW Value allocations during attrs bridge.
* Architectural change (touches `Alloc::allocBridgeThunk` + the bridge primop machinery).
* Effort: ~1 week.
* **DEFERRED** because: (a) wall impact is uncertain (per inventory §8.2 critical review — 0.014% bridge wall on HNE means TW Boehm allocation overhead is small); (b) C5 should be done first because if Phase C eliminates most bridges anyway, the per-entry overhead matters less.

### 6.2 O4 (`Kind::LazyTW` Bindings flavor)

* Same risk profile as `Kind::Chain` (which 5-pivot-falsified per `EXIT_PHASE_C_5_INVESTIGATION_2026-05-30`).
* The 190-site `entries[]` audit blocker applies equally.
* **NOT IN THIS PLAN.**  Multi-session prerequisite work required first.

### 6.3 BP1 / BP2 retirement

* These are callbacks TW uses to force bridged values.
* Cannot retire until the bridge tables are EMPTY (i.e., we never bridge into TW).
* Even after Phase C, fetchers + realisePath still produce some bridge entries.
* Retirement is a post-Phase-C+D consideration.

### 6.4 HX1 / HX2 wall optimization

* HX1 (`__derivationFromPreprocessed`) + HX2 (`__derivCoerce`) are 76-92% of primop wall.
* These are FFI LEAVES (per inventory §2.5) — they call `store->buildAndWriteDrv` etc.
* Wall-level optimization is ORTHOGONAL to bridge-table retention.
* Not in this plan; should be a separate wall-focused exercise.

---

## 7. Phase ordering & dependencies

```
       Phase A (Day 1)
         A1 BP3 kill
         A2 L2 short-circuit                     [hygiene; low risk]
              |
              v
       Phase B (Day 2-3)
         B1 dedupe v3BridgeAttrs
         B2 dedupe Closures + Lists              [cheap yield; ~50% M5 entry reduction]
              |
              v
       Phase C (Day 4-15)
         C0 fallback histogram measurement
         C1 __structuredAttrs native
         C2 outputChecks native
         C3 __contentAddressed native (optional per histogram)
         C4 __impure native
         C5 measurement + ship gate
         C6 retire TW-bridge fallback             [headline; eliminates largest bridge class]
              |
              v
       Phase D (Day 16-20, conditional)
         Per-fetcher flat-arg variants            [only if C5 leaves fetchers as biggest residual]
```

**Dependencies between phases:**
* A is independent of B, C, D.
* B is independent of A but easier to verify after A1 (fewer moving parts).
* C is INDEPENDENT of B; B's measurements feed C0's prioritization (which fallback cases to handle first).
* D depends on C's measurement to justify.

**Recommended ordering:** A → B → C → D.  Each phase's SHIP gate must pass before the next begins.

---

## 8. Risk register

| Risk | Likelihood | Mitigation | Owner |
|---|---|---|---|
| Phase C __structuredAttrs JSON encoding diverges from TW | medium | golden-test against TW byte-by-byte before scaling up | session |
| Phase C drvPath diverges → nixpkgs broken | low (correctness gate catches it) | revert immediately on divergence; no iteration | session |
| Dedupe (B1) creates a use-after-free if pointer is reused | low (arena is monotonic) | document invariant; add invariant assertion; re-audit if Stage 6 default-on lands | session |
| Phase C effort overruns (1-2 wk → 3-4 wk) | medium-high | timebox each sub-phase; if C1 alone takes > 5 days, raise scope concern | session |
| Bridge retention claim (99.8%) was per-end-of-eval and not load-bearing for peak | medium | spike: measure mid-eval bridge retention via DIAG-3 before betting on Phase C | session |
| C6 retirement breaks corner-case drvs not in any reproducer | medium | keep the fallback for 1 session after C5; flip gate to opt-OUT (`V3_DRV_KEEP_BRIDGE=1`); soak | session |
| Eviction (Stage 2 weak bridges) interacts badly with dedupe map | medium | maintain reverse map in `evictBridgeEntry`; OR skip eviction integration and verify Stage 2 stays memory-INERT (current state per inventory §6.2 / §6.3) | session |
| Per [[chain-bindings-phase-c-falsified]], the 5-pivot rule may trigger | low for this plan; HIGH if Phase E architectural items get pulled in | hard line: NO architectural lazy-bridge work in this plan | session |

---

## 9. Pre-committed thresholds summary

| Phase | Metric | Threshold | Source |
|---|---|---|---|
| A1 | BP3 calls across workload matrix | exactly 0 | inventory §2.2 |
| A2 | BP2 fast-path eligibility count on HNE | ≥ 1000 (or skip) | this plan §2.2 |
| A2 | Wall regression | ≤ 0% (must improve or stay flat) | this plan §2.2 |
| B1 | Dedupe hit rate on M5 | ≥ 25% | this plan §3.1 |
| B1 | M5 `v3BridgeAttrs.size()` reduction | ≥ 25% | this plan §3.1 |
| B1 | Wall regression | ≤ 1% | this plan §3.1 |
| C1-C4 | drvPath byte-equal vs TW | exact match (always) | correctness gate §1.4 |
| C5 | `drvNativeFallbacks` reduction | ≥ 90% on M5 | this plan §4.6 |
| C5 | `v3BridgeAttrs.size()` reduction | ≥ 25% on M5 | this plan §4.6 |
| C5 | Memory reduction (HNE arena OR M5 peak) | ≥ 50 MB | this plan §4.6 |

---

## 10. Falsification ledger (to be appended as phases land)

Per `[[falsification-rule]]`: each commit ends with "what hypothesis does this kill?".  Phase-level falsification table:

| Phase | Hypothesis killed if SHIP | Hypothesis killed if FALSIFIED |
|---|---|---|
| A1 | "BP3 has hidden callers" | "BP3 retirement is safe" |
| A2 | "BP2 fast-path-eligible cases are rare" OR "fast-path matters" depending on result | "BP2 round-trip is the bottleneck" |
| B1 | "Same Bindings bridged twice is statistically common" OR "dedupe yield is real" | "Dedupe is cheap enough to be worth maintaining" |
| C1 | "v3 can produce TW-byte-identical drvPath for __structuredAttrs natively" | inverse |
| C2 | same for outputChecks | inverse |
| C5 | "311 MB HNE retention originates at the bridge fallback" | "311 MB has other origins" |
| C6 | "Bridge fallback is dead code post-C5" | "Bridge fallback covers cases the native impl doesn't" |
| D | "Fetcher arg attrset overhead is measurable" | inverse |

Each landed commit must explicitly state which hypothesis it kills.

---

## 11. Measurement protocol (used by every phase gate)

Per `EXIT_POST_APP3_BASELINE_2026-05-31` methodology:

* **Workload N=10 back-to-back; trim-2 mean ± σ.**
* `NIX_V3_NO_DISK_CACHE=1` for HNE measurements (cache-disabled methodology stabilises σ).
* Standard workloads: hello.drvPath, HNE, M5.  Add nixpkgs.python3 + nixpkgs.python3-pkgs.numpy if any sub-phase needs structuredAttrs / outputChecks reproducers.
* Capture: `peak_rss`, `v3_arena`, `elsewhere`, `wall`, `v3BridgeXxx.size()`, `__v3_call_bridge_1` count, `__v3_force_attr` count.
* Save under `bench/baselines/2026-05-31-post-app3-combined/phase-{a,b,c,d}-{pre,post,split}.json`.

Per the accounting audit `GC_AND_MEMORY_ACCOUNTING_AUDIT_2026-05-31`:
* Trust `v3_arena` (deterministic counter).
* Treat `peak_rss` and `elsewhere` as informational on M5-class workloads (arena > peak silently clamps).
* DO NOT use "elsewhere shrunk" as a SHIP signal.  Use arena + bridge-table.size().

---

## 12. Honest limits

* **Per [[chain-bindings-phase-c-falsified]]**: the session has had 5 falsifications on Chain Bindings work.  Phase C derivationStrict native extensions are ARCHITECTURALLY DIFFERENT (no consumption-site audit blocker) — but the SHIP-gate discipline must be tight enough to catch a Phase-C-class problem early.
* **The 99.8% bridge retention claim** is end-of-eval and not validated mid-eval per `GC_AND_MEMORY_ACCOUNTING_AUDIT §11`.  Phase C bet is *conditional on retention being load-bearing for peak* — verify mid-eval before committing fully.
* **`drvPath byte-equal`** is a strong correctness gate but doesn't catch RUNTIME builder semantics.  If a built drv has different runtime behaviour, the v3 impl is wrong even if drvPath matches.  This plan doesn't address that risk; relies on the broader nixpkgs eval being byte-identical at the *drv level* (which is the standard nixpkgs evaluation contract).
* **Effort estimates** are based on the inventory's per-action ladder.  Phase C 1-2 wk is uncertain; Phase C overrun is the single largest plan risk.
* **Phase C6 retirement** is gated on "no fallback fires anywhere we measure".  Production nixpkgs has shapes we haven't measured; a soak period is built in (`V3_DRV_KEEP_BRIDGE=1` opt-out for 1 session).
* **Per [[memory-first-class]]**: SHIP gates here measure BOTH arena (deterministic) AND peak (with σ).  Phase B SHIP threshold of 25% entry reduction is decision-quality on the deterministic counter; the peak/arena delta is a secondary signal.
* **No "all-falsified" trapdoor:** if all of Phase A+B+C falsify, the plan exits with documentation per session-arc pattern.  No 6th pivot.

---

## 13. What this plan does NOT solve

* M5 peak RSS variance σ=380 MB (per EXIT_POST_APP3_BASELINE).  This is environmental and outside FFI scope.
* Stage 6 production GC default-on (paused per `GC_PAUSE_2026-05-29`).  When/if it lands, the dedupe (B1) invariant ("arena is monotonic") must be re-audited.
* Wall optimization on HX1 / HX2 (76-92% of primop wall).  Separate exercise.
* The `walkAllV3Roots` audit concern from `GC_AND_MEMORY_ACCOUNTING_AUDIT §3` — orthogonal to this plan.

---

## 14. Cross-references

### Strategic
- [`FFI_BRIDGE_INVENTORY_2026-05-31.md`](FFI_BRIDGE_INVENTORY_2026-05-31.md) — input inventory
- [`EXIT_POST_APP3_BASELINE_2026-05-31.md`](EXIT_POST_APP3_BASELINE_2026-05-31.md) — measurement methodology
- [`GC_AND_MEMORY_ACCOUNTING_AUDIT_2026-05-31.md`](GC_AND_MEMORY_ACCOUNTING_AUDIT_2026-05-31.md) — accounting flaws to navigate around

### Code anchors (Phase A)
- `primops.cc:5183-5400` — primV3ForceListElem (BP3 kill target)
- `primops.cc:5099` — primV3ForceAttr fast path (L2)

### Code anchors (Phase B)
- `primops.cc:5565` — v3BridgeAttrs push_back (D1 dedupe insert)
- `primops.cc:5464` — v3BridgeClosures push_back
- `primops.cc:5733` — v3BridgeLists push_back
- `primops.cc:4081-4158` — evictBridgeEntry / makeBridgeEntry / reviveBridgeEntry
- `primops.cc:4369` — v3BridgeUniquePtrCounts diagnostic

### Code anchors (Phase C)
- `primops.cc:6387` — isSimpleDerivationAttrs (current shape filter)
- `primops.cc:6427` — primDerivationStrictNative
- `primops.cc:6435-6663` — primDerivationStrict (HYBRID dispatch + fallback)
- `primops.cc:6791+` — buildAndWriteDrvNative (shared helper)
- `primops.cc:6107-6140` — DrvStrictSymbols (TW's actual consumption surface)
- `primops.cc:7082` — __derivationFromPreprocessed (HX1 leaf — out of this plan's scope but referenced for completion)

### Code anchors (Phase D)
- `primops.cc:11829-11857` — bridgeBuiltin<Arity>

### Methodology
- [[falsification-rule]]
- [[measure-twice-cut-once]]
- [[memory-first-class]]
- [[same-host-bisect]]
- [[chain-bindings-phase-c-falsified]] — cautionary tale; this plan avoids the 5-pivot trap

---

*Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group. SPDX-License-Identifier: Apache-2.0.*
