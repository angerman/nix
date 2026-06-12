# PLAN_BEAT_TW execution status + reframe (2026-06-12)

Companion to `PLAN_BEAT_TW_2026-06-12.md`. Records what was executed, what the
measurements changed, and the executable next steps. Host: **Air** (laptop) —
CPU is min-of-N user-CPU (low noise); RSS is noisy (±10-15%, firefox 574-668 MB
across runs). The drv-hash-critical levers were NOT landed here because they
require the full nixpkgs sweep (darwin-4), which the laptop cannot run.

## Landed (committed, measured, lang 143/143, byte-identical)

| Commit | Lever | Result |
|---|---|---|
| d59f3f692 | Phase 0 falsifiers + 0.5 freeListBins gate | reshaped Phases 1+2; see PHASE0_RESULTS |
| 68e9d0cba | 3.1 identityLambda fix (R_RETURN-peephole bug) | foldl-over-genList-id **0.50→0.33s (−34%)**, control 0% |

foldl row on this host: **2.18× (plan baseline) → ~1.19×**.

## Falsified / reverted this session (with data — do NOT re-propose)

- **0.1 retire wrapper** — all-C path infinite-recurses. Wrapper stays.
- **0.3 lower GC threshold lowers RSS** — RAISED RSS on light rows; gc_count=1
  regardless of threshold (safepoint is exitDepth==0-gated). Inert.
- **2.2-as-gated flip Immix** — V3_DBG_IMMIX_ALLOC engages ~14 allocs (not hot
  path) and reuse can't lower a high-water peak. ~0 MB on hello/firefox.
- **3.3 OPCYCLES-TLS gating** — 0% on fib + foldl (LLVM caches the TLS addr).
- **1.3 context single-source sort-skip** — byte-identical on hello/git/firefox
  but a CPU wash (single-entry contexts make the sort already-trivial). Reverted;
  drv-hash-adjacent so would need the full sweep anyway. Subsumed by 1.1.

## The reframe that matters: RSS (Phase 2)

**The plan's stated RSS levers are inert for a measured structural reason.** Peak
RSS on a drvPath eval is the arena bump **high-water mark**; the major GC fires
**once** (gc_count=1) because its safepoint is gated to `exitDepth==0`
(vm.cc:3204) and deep evals stay in nested dispatch loops until the end. So:

- **2.1 (lower GC threshold)** cannot fire more GCs → only adds CPU. INERT alone.
- **2.2 (flip Immix line reuse)** reuses freed cells for *future* allocs (caps
  virtual growth) but cannot lower a high-water peak, and isn't even on the hot
  alloc path as gated. INERT for peak RSS.

The arena DOES return pages to the OS (`freeWholeBlock`→munmap, RSS-returning on
macOS) and has evacuation (`NIX_V3_EVAC`, moves live cells to free more blocks),
but both run at the one late safepoint, so peak is unaffected.

**Real RSS levers (replace 2.1/2.2 with these):**
1. **Allocate less** — the arena is 96 MB (hello) / 264 MB (firefox) of `//`
   (OP_ATTRS_UPDATE) materialisation, **100 % of mergeBindings**. Cutting that
   volume lowers the high-water mark directly (and is CPU-positive). Levers:
   1.1 (context copies), 1.2b (wrapper churn), 2.3 (chain-SELECT
   lookup-without-materialize), and a new "`//` that shares/extends instead of
   copying the base" lever.
2. **Fire the (non-moving) major GC at depth>0 safepoints** — flat mark-sweep
   does not move cells (vm.cc:3182-3184), so the C-stack-dangling risk that gates
   it to exitDepth==0 is null for the *non-evacuating* path. Letting whole-block-
   free + freeWholeBlock run mid-eval at nested safepoints is what actually lowers
   the high-water mark. This is the real Phase-2 lever; it is NOT in the original
   plan. (Evacuation must stay exitDepth==0 — it moves cells.)

All RSS work needs darwin-4 to measure to the bars (laptop RSS noise ≈ the bars).

## The reframe that matters: drv-path CPU (Phase 1)

- **0.4 narrowed 1.1 to span-sharing** — parse-back is only 2.4 % of firefox CPU.
  The cost is the per-edge *copies* (vector<string> deep-copied at every
  concat/coerce) + the sort-over-strings, not NixStringContextElem::parse.
- **1.2 wrapper**: stays (can't retire). The wrapper's `attrNames → map(args.${k})
  → listToAttrs` pipeline (bytecode_primops.cc:918,990-998) round-trips every
  attr key SymbolId→string→SymbolId **3×** per drv. Lever 1.2b replaces it.

**Both 1.1 and 1.2b are drv-hash-critical** → must be validated with the full
nixpkgs sweep (`bench/chain-nixpkgs-fullsweep.sh`) on darwin-4, not the 7 rows.
A subtle env/context divergence on any package = wrong hash + cascade. Do NOT
land either validated only on hello/git/firefox.

### 1.2b implementation note (lower-risk first cut)

For the common case (non-structured, non-__ignoreNulls), the wrapper's
`baseEnv` can be `builtins.mapAttrs (k: v: builtins.__derivCoerce v)
(builtins.removeAttrs args (flagKeys ++ specialEnvKeys))` — mapAttrs iterates
Bindings entries SymbolId-keyed (no attrNames string alloc, no `args.${k}`
dynamic re-select, no listToAttrs re-intern). Keep the old filter-map-listToAttrs
path under `if ignoreNullsFlag` (it must DROP nulls, which mapAttrs can't).
Verify v3's mapAttrs doesn't itself stringify keys; gate on full-sweep byte-identity.

## Prioritised next steps (by value × confidence ÷ risk)

1. **[darwin-4] Phase 2 depth>0 non-moving GC** — the real RSS lever; biggest
   measured headroom (firefox ~450 MB dead-but-resident arena). Soak with
   GC_STRESS + full-sweep byte-identity.
2. **[darwin-4] 1.2b wrapper de-stringification** (mapAttrs cut first) — 3 drv
   rows CPU + allocation volume (helps RSS too). Full-sweep gate.
3. **[darwin-4] 1.1 context span-sharing** — 3 drv rows; the per-edge copy lever
   (NOT parse memo). Full-sweep gate.
4. **[laptop-safe] 3.4 OP_FOLD resident superinstruction** — foldl 1.19×→≤1.0×;
   the one remaining big lever validatable by lang-tests + byte-identity. Use the
   existing ip-rewind re-entry protocol (emit.cc Phase-5 R_CALL family is the
   template); fold driver as an opcode, body OP_RETURN rewinds to it, zero nested
   dispatchLoop. Bar ≥20%/<10%.
5. **[laptop-safe] fib 1.38×** — dispatch-bound; rides 3.5 computed-goto (the
   last dispatch lever) once OP_FOLD lands and dispatch share rises.

3.2 (GET_LOCAL+FORCE fusion) is **already implemented** at the IR level
(`emitGet*ForceFromIR`, emit.cc:730/1138 + the compacting peephole at :2092);
its only residual is the narrow "fuses under one expr shape but not under `let`"
emitter inconsistency — a disasm-diff micro-fix, low yield.

*Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output
Group. SPDX-License-Identifier: Apache-2.0.*
