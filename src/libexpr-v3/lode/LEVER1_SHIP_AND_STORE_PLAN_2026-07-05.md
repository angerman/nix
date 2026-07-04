# LEVER-1 ship + persistent store + flake-keys + LEVER-2 — execution plan (2026-07-05)

Post-`/goal` plan covering the four forward items after the 17-task run
(commits `edf869bee`..`a50609461`). Rule 0 throughout: every phase states a
pre-committed SHIP/KILL gate + a measurement-first falsifier; no build before
its step-0 measures; full `--brute` on darwin-4 before every commit; perf on
the quiet host (detached `nohup`); every measurement git-noted; a KILL is a
valid deliverable (write it, don't force a ship).

Current state (verified 2026-07-05):
- Applied cache SHIPPED, **default-OFF** (`NIX_V3_APPLIED_CACHE=1` enables).
  Correct (miscompile fixed a50609461), tax-free (WHNF pre-check), shadow-
  validated (hello/firefox/HNE 0 mismatch). Battery = 15 checks.
- Const-eager literal lowering is **default-ON** (opt-out `NIX_V3_NO_CONST_EAGER`).
- `clearPostEvalGlobalRoots` clears the import cache but NOT the applied cache:
  applied entries deliberately survive across root evals (daemon reuse) and
  stay GC-rooted (walkAppliedCacheRoots) → no UAF, but they PIN graphs → RSS
  retention bounded only by the LRU cap (`NIX_V3_APPLIED_CACHE_MAX_ENTRIES`=64).
- kSchemaVersion = 20. Perf host darwin-4; goldens pinned via nixpkgs-pin.sh.

Dependency graph: **#1 independent (do first)** → **#2 falsifier independent
(cheap kill-or-go, gates #3)** → **#3 conditional on #2 GO + own headroom** ;
**#4 fully independent** (RSS lever; its step-0 is pure instrument, can run on
darwin-4 in parallel with #1/#2).

---

## ITEM 1 — Ship LEVER-1 default-on (the built CPU win, currently gated off)

Goal: make `(import f) args` memoization active by default so real repeated-
eval workloads get eval#2..N-free without an env var. The risk is real
(a default-on cache miscompile is a silent wrong drvPath — see the R_CALL bug
we just fixed), so this is validation-heavy, not a one-line flip.

### 1.0 — Pre-flip measurements (darwin-4, no code; git-note)
Falsifier for "default-on is free on single evals":
- Single-eval CPU+RSS overhead, cache-on vs off, on hello/firefox/M5 (SINGLE
  eval, N=5 medians). The tax trim (#16c) should make this ≈0, but default-on
  pays arming/lookup/insert + GC-root walk on EVERY eval, reuse or not.
  **GATE: ≤2% CPU AND ≤ noise (~1%/20MB) RSS. KILL-the-flip if >5% CPU.**
- RSS-under-accumulation: eval K distinct `(import f) argᵢ` in one process
  (daemon simulation), K ≫ LRU cap; confirm RSS plateaus at the cap, not
  grows unbounded. **GATE: plateaus; if not, lower the default cap first.**

### 1.1 — Impurity/taint audit + regression lock (correctness)
In-process constancy makes v1 safe (currentTime fixed per process, getEnv/
NIX_PATH/currentSystem stable, store paths immutable), AND the applied cache
surviving post-eval-clear is in-process reuse — still constant. LOCK it:
- Failing-first tests INTO the battery: (T10) an import-result whose body reads
  `builtins.currentTime`/`getEnv`, applied twice in one process → cache-on ==
  cache-off (consistent, since constant per-process); (T11) two root evals in
  ONE process (repl-style / `v3-eval` multi-expr) referencing the same import
  → second sees a correct (not freed) cached graph — the post-eval-clear ×
  applied-cache-survival interaction, the UAF hazard, guarded.
- **GATE: T10/T11 green cache-on==off; if any impure body is wrongly memoized,
  add a taint-skip (don't cache when the body touched an impure builtin) BEFORE
  the flip — do not ship a default-on cache that can serve a stale impure result.**

### 1.2 — The flip + full sweep
- Invert the gate: `NIX_V3_APPLIED_CACHE` unset ⇒ ON; `=0` ⇒ off (opt-out).
  Update the inline retirement comment. Applied-cache enters kGates only if it
  changes emitted bytecode (it does NOT — runtime only — so NO kGates/schema
  change; confirm with lint-cache-coherence).
- Full darwin-4 nixpkgs byte-equality sweep (the ≥59-pkg drvPath sweep) with
  cache DEFAULT-ON vs the pinned goldens. **GATE: zero divergence.**
- Full `--brute` (now exercises cache-on as the default path) ALL GREEN + the
  gate-OFF brute (opt-out path) ALL GREEN.
- **SHIP: sweep 0-diverge + brute both + 1.0 overhead ≤2% + 1.1 green.
  Else: keep default-off, root-cause (Rule 0), do not force.**

### 1.3 — Retire the const-eager opt-out (fold, lower priority)
Const-eager is already default-on; delete `NIX_V3_NO_CONST_EAGER` (hard-true) +
its kGates entry after its OWN nixpkgs byte-eq sweep confirms 0-diverge.
Schema bump only if the kGates removal changes the fingerprint namespace (it
does — but removing a gate is safe: it can only merge previously-separate
namespaces, and default behavior is unchanged). Defer if 1.2 is enough for now.

---

## ITEM 2 — Part C persistent store: build the falsifier FIRST (cheap kill-or-go)

Goal: decide whether the mmap on-disk result store (RESULT_STORE_DESIGN_2026-07-04)
is worth building, by measuring its hit-path against re-eval on a synthetic
prototype BEFORE any store. The store only wins if loading+relinking a cached
graph is much cheaper than re-evaluating it.

### 2.0 — Scope the minimal prototype (design note, ~0.5d)
The hard core the in-memory tier never needed: **Thunk/Closure serialization**
(v1 `value_serialize` throws on them). Prototype encoding per the design:
thunk = (CU content-key, funcIdx, captured-upvalue refs); deserialize = relink
against the existing CU disk cache (schema-20 keyed). Pick ONE representative
graph: `(import <nixpkgs> {}).hello` (WHNF attrset with thunk-valued entries) —
small, real, contains the unforced-thunk case that is the whole difficulty.

### 2.1 — Build the serialize/deserialize+relink spike (gated, throwaway-quality)
`NIX_V3_RESULT_STORE_SPIKE=1`. Extend value_serialize to emit thunk/closure
stubs; deserialize + relink against CU disk cache; materialize on force.
Validate byte-identity: forcing the deserialized graph's `.drvPath` == the
freshly-evaluated `.drvPath` (correctness precondition — a store that
round-trips wrong is dead on arrival regardless of speed).

### 2.2 — Measure the pre-committed falsifier (darwin-4; git-note)
- `T_hit` = load blob + relink + force leaf projection (drvPath).
- `T_eval` = eval the same from scratch, cache-off, cold.
- `materialized_arena_bytes / blob_bytes`.
- **GO: T_hit / T_eval ≤ 0.20 (store beats re-eval by ≥5×).
  KILL: > 0.20 — the store loses to re-evaluation; the in-memory tier already
  covers the in-process/daemon case for free. Document + stop; a defended KILL
  is the deliverable (mirrors BiBOP/env-capture).**

### 2.X — VERDICT: KILL (2026-07-05, defended by measurement)
Rule 0 sharpening: the 2.0 pre-falsifier (bench/lever1-store-prefalsifier.sh)
measured T_run/T_leaf=0.27 — a SAVINGS-CEILING proxy, not the real T_hit.
Rather than build the multi-day thunk-serialize spike on a proxy, measured the
REAL cross-process T_hit via #741's EXISTING persistent drv-result disk cache
(NIX_V3_DRV_HASH_CACHE_DISK — the closest working analogue of the store), which
serializes+reloads forced drv results (value_serialize + SQLite):
  T_eval(cold, warm-CU)=0.600s ; T_hit(warm x-proc, #741 disk @ 100% hit)=0.600s
  → **T_hit/T_eval = 1.00 ≫ 0.20 gate → KILL.**
Even at 100% hit rate (691 drv-hash + 496 eval-result disk hits), the persistent
cache delivers ZERO wall-clock speedup: reload/relink of a cached result costs
as much as recomputing it, because hello.drvPath's cost is import+run+force
(0.44s of 0.60s per 2.0), NOT the derivationStrict call the cache short-circuits.
A general thunk-graph store faces the same wall (reload ≈ re-eval).
CONCLUSION: a persistent RESULT store is NOT worth building.  The realizable
repeated-eval wins are ALREADY captured by (a) the in-memory applied cache
(default-on, #1 — eval#2 free in-process/daemon) and (b) the CU disk cache
(parse+lower persisted).  #741's own disk eval-result layer is measured
no-speedup here — a candidate for retirement, tracked separately.
NARROW residual (NOT built): persisting FINAL LEAF projections (a drvPath
STRING) keyed by content would give T_hit≈0 cross-process, but that is thin
cross-process memoization of the applied cache's leaf results (overlaps #741 +
the applied cache), value limited to repeated identical `nix eval .#pkg.drvPath`
across processes — filed as a possible small future item, below the bar now.
Falsifier method (git-noted): bench/lever1-store-prefalsifier.sh + the #741
cross-process T_hit measurement (scratchpad p2-real-thit.sh).

### 2.3 — (WOULD-BE only on GO; not reached) phased store build per RESULT_STORE_DESIGN
Writable LRU segments on aot_cache mmap; key discipline (schema ‖ nixVersion ‖
gateFingerprint ‖ currentSystem ‖ storePathPin ‖ argsHash); content-hash
verify-don't-trust; no persistent entry for mutable-working-tree paths or
__currentTime taint; cross-machine (endianness/path-remap/signing). Each phase
its own byte-id + brute gate. LARGE — scope after GO.

---

## ITEM 3 — Flake-workload reuse (conditional on #2 GO + its OWN headroom gate)

Honest framing: M5/HNE get ~0 cache hits because haskell.nix applies nixpkgs
with COMPUTED args + overlays (functions) — **unhashable under ANY key scheme**
(a function has no canonical hash). Content-keys fix cross-PROCESS identity,
NOT the arg-is-a-function problem. So #3 is the most likely KILL; gate it on a
measurement before any design.

### 3.0 — Addressability characterization FIRST (measurement, git-note)
Instrument M5/HNE re-eval: of the eligible import-CU applications, what % of
re-eval CPU is spent in applications whose args are (a) content-hashable, vs
(b) genuinely unhashable (overlays/functions)? Also probe a COARSER granularity:
would keying `import <nixpkgs>` by the flake-lock pin (not the callPackage
applications) capture the bulk of M5/HNE reuse?
- **GATE: if <30% of M5/HNE re-eval CPU is addressable by any realizable
  keying → KILL #3; document that flake workloads are served by the file-level
  CU disk cache (already exists), not an applied-result cache. Stop.**

### 3.1 — (only if 3.0 ≥30% AND #2 GO) design + build content-key v2
Per RESULT_STORE §key-discipline. New failing-first flake regression tests.
Own byte-id + brute + gate.

---

## ITEM 4 — LEVER-2 RSS (independent; execute lode/LEVER2_PLAN_2026-07-04.md)

Its step-0 is pure instrument → run on darwin-4 in parallel with #1/#2.

### 4.0 — Step-0 measurements (darwin-4, no code; git-note)
- **B0**: LambdaDescriptor/CU malloc footprint on firefox+M5, split load-bearing
  vs diagnostic bytes (the #139 ~75MB M5 estimate).
- **A0/D0 shared harness**: forced peak-live major mark-sweep
  (`NIX_V3_MAJOR_GC_THRESHOLD_MB=<just-under-peak>`) → live-thunk forced-once
  count (A0) + live-Bindings shape-dup + entry-count histogram (D0).

### 4.1 — Build WS-B if B0 clears its line
**GATE (pre-committed): WS-B SHIP ≥40MB M5 peak-RSS reduction, byte-id, CPU
regression ≤2%. KILL <20MB.** WS-B = LambdaDescriptor diagnostic side-table +
lazy-load descriptors — lowest risk, attacks the `elsewhere` bucket the whole
prior campaign ignored.

### 4.2 — WS-D / WS-C conditionally per D0/C0 lines (LEVER2_PLAN gates verbatim)
WS-D (Bindings shared-shape/inline-small, ≥80MB, highest correctness risk) only
if D0 shows the dup; WS-C (arena-avoidance, ≥60MB & M5 CPU ≤5%) only if C0 finds
a cheap M5-safe nursery signal. A0 (thunk count) is documented-KILL-expected —
run A0, confirm ~3.5% residue, close.

### 4.3 — Program STOP (pre-committed, honest)
If WS-B ships and A0/C0/D0 all measure below their SHIP lines: **declare
"1.6–2.0× TW resident is v3's structural floor under the current Value/VM
representation"** and stop the RSS-lever hunt (parity = a representation
rewrite, a separate multi-quarter decision). The banked WS-B win + a quantified,
defended floor IS the deliverable.

---

## Sequencing for the loop
1. **#1.0 + #4.0 measurements** fire together on darwin-4 (both pure instrument).
2. **#1.1 → #1.2** flip (gated). In parallel: **#2.0 → #2.2** falsifier.
3. Branch on #2: GO → #2.3 store (large) ; either way → **#3.0** characterization.
4. Branch on #3.0: ≥30% & #2 GO → #3.1 ; else KILL #3.
5. **#4.1** build WS-B if B0 clears; then #4.2 conditionals; then #4.3 STOP eval.
6. #1.3 const-eager opt-out retirement folds in whenever convenient.

Every KILL is written up (memory + lode + git-note) and closes its branch.
Stop the whole `/goal` when: #1 shipped-or-defended, #2 GO-built-or-KILLed,
#3 built-or-KILLed, #4 WS-B shipped-or-floor-declared.

Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
Input Output Group. SPDX-License-Identifier: Apache-2.0.
