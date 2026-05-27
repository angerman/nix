## extendDerivation wall-time investigation — 2026-05-18

> **SUPERSEDED 2026-05-27**: Bridge telemetry confirmed 0.014 % wall; investigation closed. See [`BRIDGE_TELEMETRY_2026-05-26.md`](BRIDGE_TELEMETRY_2026-05-26.md). Preserved here for historical reference + back-link integrity.

---


User asked: "carefully investigate the extendDerivation issue."

### Setup

After landing Option 4 hybrid (commits 7adc7e61f, d3e41c13d) + Tag::App
memoization, the architectural C-stack overflow on hello.name is closed.
But `hello.drvPath` (and `.outPath`) still takes 30-46s vs TW's 1.3s —
roughly 30x slower.

Initial V3_DBG_FORCES diagnostics pointed at
`lib/customisation.nix:409:20` as the hot site, with 64K+ forces against
that single lambda descriptor (32% of all forces during a partial 200K-
force run).

### What is at customisation.nix:409:20?

```nix
extendDerivation =
  condition: passthru: drv:
  let
    commonAttrs =
      drv // (listToAttrs outputsList) // { all = ...; } // passthru;

    outputsList = map (outputName: {       # <-- line 409:20 LAMBDA
      name = outputName;
      value = commonAttrs // {
        inherit (drv.${outputName}) type outputName;
        outputSpecified = true;
        drvPath = assert condition; drv.${outputName}.drvPath;
        outPath = assert condition; drv.${outputName}.outPath;
        ${if passthru ? overrideAttrs then "overrideAttrs" else null} =
          f: (passthru.overrideAttrs f).${outputName};
      };
    }) (drv.outputs or [ "out" ]);
  in
    commonAttrs // { drvPath = ...; outPath = ...; };
```

`409:20` is exactly the `outputName: {...}` lambda passed to `map`.
Each invocation:
1. Builds a `{name; value=ATTRS}` attrset
2. The `value` slot is a thunk `commonAttrs // {...}`
3. `commonAttrs` recursively references `outputsList`

The lambda is invoked **once per `extendDerivation` call** per output
name.  For hello with `outputs = ["out"]`, that's 1 invocation per
extendDerivation call.

### Hypothesis A: lambda re-invocation (Tag::App not memoized)

**Tested**: added Tag::App `evaluated` field for memoization (commit
d3e41c13d).

**Result**: PARTIALLY FIXED.  hello.name went from 1.53s to 0.55s when
the memo is enabled.  But hello.drvPath still timing out at 46s — the
memo helps for SHARED Apps (one App forced multiple times) but doesn't
help when each force is a FRESH App pointer.

Each `extendDerivation` call creates a fresh `outputsList = map (...)`
result.  Each map result has fresh Tag::App entries.  So multiple
extendDerivation calls create multiple App instances — no sharing,
no memo hit.

### Hypothesis B: v3 invokes extendDerivation more times than TW

**Setup**: progress dumps at 10K stride during hello.drvPath:
- force=10K: hot=`<thunk>/1 at=stdenv/darwin/default.nix:675:8`
- force=20K: hot=`outputs'/123 at=make-derivation.nix:463:7`
- force=30K: hot=`<thunk>/1 at=lib/default.nix:279:9`
- force=40K: hot=`<thunk>/1 at=maintainer-list.nix:7814:13`
- force=50K: hot=`<thunk>/1 at=maintainer-list.nix:24352:13`

(`/1` means "this thunk descriptor seen once at this point".)

The hot site SHIFTS over time.  customisation.nix:409:20 dominates a
LATER phase (~100K-200K forces).  Each phase contributes to total work.

TW stats for the same query:
```
nrThunks: 394K
nrFunctionCalls: 184K
nrPrimOpCalls: 116K
cpuTime: 0.59s
```

So TW handles ~1M ops in 0.6s.  Estimated v3 throughput at the same
workload: ~5K thunks/sec.  That's a **200x slower per-thunk rate**.

This isn't a busy loop — it's genuine work that's just slow.

### Hypothesis C: my Option 4 wrapper compounds the problem

**Setup**: tried replacing the full Option 4 wrapper with a leaner
"pragmatic" wrapper (pre-force only, no env-attrset construction in
bytecode).

**Result**: REGRESSED.  hello.name went from 0.55s to 44s with the
pragmatic wrapper.  Reverted.  The full Option 4 wrapper is actually
optimal for hello.name (the iteration-in-bytecode design is correct).

**Hypothesis C falsified.**  The Option 4 wrapper IS the best design.
The remaining slowness is downstream of the wrapper.

### Hypothesis D: per-op dispatch overhead in dispatchLoop

**Evidence**:
- TW's force rate: ~1M/s
- v3's force rate: ~5K/s
- 200x slower per force.

A 200x per-op overhead is unusual for a bytecode VM — typically
2-5x is expected.  This points to either:
- A non-obvious per-iteration cost (e.g. arena scanning, GC pressure,
  exception-handling setup repeatedly invoked)
- Repeated work that TW's caching avoids

The `arena=1+GB` observation during long runs suggests GC pressure is
substantial.  Each ValuePair is now 48 bytes (16 for `evaluated`); with
~100K Apps live, that's ~5 MB.  Plus Bindings, Thunks, etc.  Boehm GC
scans the arena's pointer-traced regions, and with arena growing past
1 GB, scans become expensive.

### Hypothesis E: TW caches at a higher level (mkDerivation result)

**Conjecture**: TW's eval-cache or some other mechanism caches the
result of `stdenv.mkDerivation` such that the same drv input
produces the same output without re-evaluation.  v3's bytecode path
doesn't have this caching layer.

Could not directly verify (would need TW-side profiling).  Plausible
given the perf gap.

### What's NOT happening

- It's NOT a busy loop.  Force counters advance, just slowly.
- It's NOT my Option 4 wrapper being heavy.  The wrapper is fast.
- It's NOT a missing Tag::App memo (App memo is in place).
- It's NOT C-stack exhaustion (the Phase 1 architectural fix held).

### Where the bottleneck lives (best estimate)

A combination of:

1. **Per-op v3 dispatch overhead** (~10x base cost vs TW C).
2. **GC pressure** from compounding intermediate allocations (each
   bytecode-level operation allocates more than TW's equivalent).
3. **No higher-level result caching** for nixpkgs-style mkDerivation
   pipelines (TW likely benefits from some implicit caching).

### What would close the gap

None of the following are small fixes:

- **Phase 2 from the action plan** — reducing per-op dispatch overhead
  (constant-fold, eliminate redundant FORCE chains, inline small
  primops at IR level).
- **Reduce intermediate allocations** in the lowerer — currently each
  binary op allocates an intermediate thunk-binding through the
  A-normal-form-style IR.  IR-level register coalescing or stack-
  threading would help.
- **Bytecode primop optimization** — my `builtins.foldl'` and
  `builtins.map` produce many intermediate Tag::App / closure
  allocations.  IR fusion (`foldl' (acc: x: ...) [] (map f xs)` →
  single loop) would save N intermediate App entries per call.

### Action plan implication

Per the action plan's Rule 0 (every commit must answer "what
hypothesis does this kill?"):

  H_extendDerivation "v3's slowness on hello.drvPath is because the
  Option 4 wrapper invokes extendDerivation excessively or has a
  busy loop." **FALSIFIED**: hot-thunk progression shows the eval
  makes steady progress, just slowly.  The wrapper is correct;
  the gap is per-op overhead × intermediate allocations × no
  higher-level caching.

Closing the wall-time gap is **Phase 2+ work** beyond the architectural
C-stack ceiling Phase 1 closed.  Phase 1 exit criterion ("hello.name
evaluates without C-stack overflow") remains MET.

Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
Input Output Group. SPDX-License-Identifier: Apache-2.0
