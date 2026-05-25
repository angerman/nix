# VM / TW timing visibility — design note (2026-05-20)

**Problem**: it's currently too hard to see how much wall time a v3
eval spends inside the VM vs. inside TW callbacks reached via bridge.
The infrastructure exists but is triple-gated and prints zero by
default — so we routinely make perf claims without honest VM/TW
attribution.

## §1 — What already exists

### `run.cc:34-79` — `PhaseTimer` (gated on `V3_TIMING=1`)

Prints once per `runRootExpr` call:

```
v3-direct timing (ms): lower=0.234 optimise=0.156 compile=0.089 run=12.345 vm=11.234 bridge=1.111
```

Phase breakdown:
- `lower_ms`   — AST → IR (`lowerNixExpr`)
- `optimise_ms` — IR optimisation passes (`ir::optimise`)
- `compile_ms` — IR → bytecode (`compile`)
- `run_ms`    — VM dispatch (`run`)
- `vm_ms`     — `run_ms - bridge_ms` (the v3-only fraction)
- `bridge_ms` — sum of `bridgeTotalNs()` (TW-side time reached via
                the bridge); populated ONLY when `NIX_V3_BRIDGE_TIMING=1`

### `primops.cc:9078-9179` — bridge telemetry

Always-on call counts; opt-in nanosecond accumulation:

- `bridgeTelemetryBump(kind, ns)` — bump count and (if `ns > 0`) ns total.
- `BridgeTimer` RAII at six bridge sites:
  - `TwToV3Full`    — `treeWalkerToV3Public` whole-value bridge.
  - `TwToV3Scalar`  — scalar fast-path hits.
  - `TwToV3Attr`    — `tryBridgeAttrLookup` per-attr peek (success).
  - `TwToV3Has`     — `tryBridgeAttrHas` per-attr existence (success).
  - `V3ToTw`        — `v3ToTreeWalkerPublic` whole-value bridge.
  - `TwForce`       — TW `state.forceValue` calls from v3 hooks.
- `dumpBridgeTelemetry(stderr)` — formatted breakdown; called from
  `PhaseTimer` dtor when `bridgeTimingEnabled()`.

`NIX_V3_BRIDGE_TIMING=1` gates the ns accumulation because
`steady_clock::now()` is ~10-30 ns per call and would dominate the
bridges it measures.  Cost when off: a single `std::atomic::fetch_add`
per bridge call (~1 ns).

### `run.cc:180-202` — `NIX_VM_STATS=1` alloc dump

```
v3-direct alloc: values=… closures=… thunks=… lists=… attrsets=…
                 pairs=… thunksForced=… bridge=… insns=…
```

Plus `dumpPrimOpStats(stderr)` (per-primop bridge call counts;
#660-era retirement criterion).

## §2 — The gap

Three pieces of friction:

1. **Triple gate**.  To see VM/TW split with timings you need
   `V3_TIMING=1 NIX_V3_BRIDGE_TIMING=1` AND opt into `NIX_V3_DIRECT_EVAL=1
   NIX_V3_SKIP_INSTALLABLE_PREEVAL=1` for honest v3-direct.  Easy to
   forget; easy to publish numbers without it.

2. **Default output lies**.  Without `NIX_V3_BRIDGE_TIMING=1`,
   `bridge_ms` reads 0 — so the line claims `vm = run` even when v3
   is bouncing through TW heavily.  Worse than no number: it's a
   misleading number.

3. **No always-on summary**.  Every v3 eval ought to report at least
   a count-based VM/TW split for free, the way `--time` etc. do in
   peer tools.  Currently `NIX_V3_DIRECT_EVAL=1` users get nothing
   unless they remember the timing flags.

## §3 — Options

### Option A — flip default-on when `NIX_V3_DIRECT_EVAL=1` (recommended)

Land:
1. `PhaseTimer::s_active()` returns true whenever `NIX_V3_DIRECT_EVAL=1`,
   not just when `V3_TIMING=1`.  Suppress via `NIX_V3_QUIET=1`.
2. The line becomes honest about its uncertainty.  When
   `NIX_V3_BRIDGE_TIMING=1` is off, print
   `bridge=N calls (timing off)` instead of `bridge=0.000`.
3. Roll a short alloc summary into the default output (already free
   via atomics — `NIX_VM_STATS=1` becomes a no-op alias).

**Pros**:
- Honest baseline visible without extra flags.
- Zero cost when v3 isn't in use (tree-walker untouched).
- Existing flags still control the expensive bits (ns-accurate bridge
  timing stays opt-in).
- v3-direct is opt-in already, so blast radius is contained to v3 users.

**Cons**:
- CI/scripts using v3-direct see a new stderr line per eval.
  Mitigation: `NIX_V3_QUIET=1` to mute.
- A few benchmarks that grep stderr for specific patterns may need
  updating.  Reviewed pre-land.

### Option B — new `NIX_V3_LOG=1` opt-in

Land a new env var that wraps the same display.  Users set it in
their shell rc; no surprise breakage.

**Pros**:
- Zero risk to scripts/CI.

**Cons**:
- Anyone forgetting to set it (colleagues, agents, future-self after a
  fresh shell) keeps publishing un-attributed perf claims.  This is
  what got us here; another env var doesn't fix it.

### Option C — heavier instrumentation (NOT recommended yet)

Periodic in-eval snapshots (every N seconds), per-op category
breakdown (closure-dispatch vs primop-dispatch vs force-chain), flame
graph friendly output.  Defer until A is in and we know what we're
missing.

## §4 — Recommendation

Option A.  Concrete sequence:

1. `PhaseTimer::s_active()`: `getenv("V3_TIMING") || (getenv("NIX_V3_DIRECT_EVAL") && !getenv("NIX_V3_QUIET"))`.
2. Update the `~PhaseTimer()` print to handle `bridgeTimingEnabled() == false`:
   show `bridge=Nc (timing off — set NIX_V3_BRIDGE_TIMING=1)` instead
   of `bridge=0.000`.
3. Append alloc summary to the same line (or a second line) — pulled
   from `allocStats()` which is already populated.
4. Document in `USAGE.md` under "Diagnostics": the new default and how
   to mute / enable full timings.
5. Audit `test/run-*-tests.sh` for any grep on `bridge=0.000` (likely
   none, but check).

Cost to land: ~1-2 hours including the audit.  No invariants change.

## §5 — Open questions

1. Should the per-primop bridge-call dump (`dumpPrimOpStats`) be
   default-on too?  It's a long block of text — probably not by
   default.  Leave behind `NIX_VM_STATS=1` as opt-in for the longer
   dump.
2. Should we add a `--time` flag at the `v3-eval` / `nix eval` CLI
   that maps to the same display?  Cleaner UX than an env var.
   Defer to follow-up; env var is enough for now.
3. Should the bridge `count` be sampled (not just totalled) over the
   eval so we can see whether bridges concentrate at start vs. end?
   Defer to Option C / periodic snapshots; not needed for the
   honest-baseline goal.

## §6 — Cross-references

- `src/libexpr-v3/run.cc:34-79` — `PhaseTimer`.
- `src/libexpr-v3/primops.cc:9078-9179` — bridge telemetry.
- `src/libexpr-v3/include/v3/primop.hh:395-443` — `BridgeKind` enum,
  `BridgeTimer`, `bridgeTotalNs`, `dumpBridgeTelemetry`.
- CLAUDE.md §"Running v3 probes safely" — current diagnostic env-var
  list (would document the new default here).
- `feedback_nix_vm_v2_benchmarking.md` (memory) — earlier
  benchmarking-flag discipline; same theme.

## Copyright

Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
Input Output Group.  SPDX-License-Identifier: Apache-2.0
