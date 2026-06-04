# v3 VM vs stock-nix tree-walker (TW) — CPU + memory, eval(hot) vs compile separated

**Date:** 2026-06-04
**Status:** MEASUREMENT BASELINE. CPU + peak-RSS comparison of the v3 bytecode
VM against cppnix's mature tree-walker, on the SAME binary (TW = default
`nix eval`; v3 = `NIX_V3_DIRECT_EVAL=1`). Eval (hot) is measured separately
from the compilation/lowering phase. Harness: `bench/v3-vs-tw-phases.sh`.
Memory accounting uses the post-`NIX_V3_MEM_BUCKETS` instruments (reconciled
to `/usr/bin/time -l` within ±0.1%).

---

## 1. Methodology (what is and isn't comparable)

- **Phase separation (v3):** `V3_TIMING=1`. Eval(hot) = `max(run=)` over the
  per-`runRootExpr` timing lines (the dominant top-level run; install/wrapper
  lines are sub-ms), measured **WARM** (disk cache populated → nested imports
  *deserialize*, not compile). Compile/lowering = the `import timing` line's
  `parse+lower+optimise+compile`, measured **COLD** (`NIX_V3_NO_DISK_CACHE=1`)
  — the per-CU miss-path total across all imports.
- **TW eval CPU:** `NIX_SHOW_STATS=1` → `cpuTime` + `gc.heapSize`. TW has no
  compile phase (it walks the AST); its analogue is parse + bindVars.
- **Memory:** `/usr/bin/time -l` "maximum resident set size" is the fair
  peak-RSS metric for BOTH. Boehm RESERVES ~400 MB of mostly-non-resident
  address space, so `heapSize` / `boehm_heap` are NOT comparable RSS.
- **Wall:** `hyperfine` (warm host, n≥15). A hand-rolled timer cannot resolve
  the variance — an earlier draft mis-ranked cold-vs-warm with one.

**The load-bearing caveat:** for PURE-eval workloads (no store/FFI) TW
`cpuTime` and v3 `run` are apples-to-apples (both = eval engine only). For
REAL workloads the v3 `run` bucket **includes** the FFI/store work
(derivationStrict, drvPath, IFD) that happens during the dispatch loop, while
TW `cpuTime` **excludes** store/CLI work. So real-workload engine ratios are
quoted from **wall** (both inclusive); the CPU column is shown but flagged.

**WARM must be bytecode-cached but RESULT-cache-cold** (fresh
`NIX_V3_CACHE_DIR` per workload, pre-warmed once). A fully *result*-cached run
does ~no work and reports a misleadingly tiny arena (a 32 MB anomaly seen
early was exactly this).

---

## 2. Results (2026-06-04, quiet host)

### Eval (hot) — CPU

| workload | TW eval | v3 eval | v3/TW | notes |
|---|---|---|---|---|
| fib33 (pure) | 1.64 s | 7.33 s | **4.5×** | pure recursion, apples-to-apples |
| ackermann-3-7 (pure) | 0.156 s | 0.454 s | **2.9×** | pure recursion |
| fold-add-1M (pure) | 0.137 s | 2.39 s | **17.5×** | **primop-iteration — v3's worst case** |
| hello.drvPath | 0.30 s¹ | 1.46 s² | 4.9×¹ | ¹TW excl. store; ²v3 incl. FFI — use wall |
| HNE.drvPath | 1.55 s¹ | 9.79 s² | 6.3×¹ | as above; HNE adds IFD/haskell.nix |

### Eval (hot) — WALL (real workloads, both incl. store/FFI; hyperfine n=15)

| workload | TW wall | v3-warm wall | v3-cold wall |
|---|---|---|---|
| hello.drvPath | 0.63 s | **1.43 s (2.26×)** | 1.98 s (3.14×) |

### Peak RSS (resident, `/usr/bin/time -l`)

| workload | TW | v3 | v3/TW | v3 arena / elsewhere |
|---|---|---|---|---|
| fib33 | 405 MB | 361 MB | **0.9×** | arena 201 MB |
| ackermann-3-7 | 72 MB | 247 MB | 3.4× | arena 218 MB |
| fold-add-1M | 135 MB | 679 MB | 5.0× | arena 536 MB |
| hello.drvPath | 138 MB | 733 MB | **5.3×** | arena 570 MB |
| HNE.drvPath | 570 MB | 2535 MB | **4.4×** | arena 1661 MB + elsewhere 594 MB (CU cache) |

### Compilation / lowering phase (v3-only; cold; disk-cache-amortized)

| workload | CUs | compile (parse+lower+optimise+compile) | warm deserialize |
|---|---|---|---|
| hello.drvPath | 270 | 635 ms (parse 164 / lower 99 / optimise 297 / compile 65) | 36 ms (disk=270 hits) |
| HNE.drvPath | 2449 | 4245 ms | (disk=2449 hits) |

cold − warm wall on hello (1.98 − 1.43 = 0.55 s) ≈ the 635 ms compile − the
36 ms deserialize ✓ (independent cross-check of the phase split).

---

## 3. Findings

1. **The bytecode VM is currently SLOWER than the mature tree-walker on eval.**
   Pure recursion: **2.9–4.5×** slower. Real-workload wall: **2.26×** (hello,
   warm). This is the honest interpreter gap — cppnix's TW is a heavily-tuned
   interpreter; v3's per-op machinery (frames, force, GC barriers) has not yet
   beaten it. (The recent SET_LOCAL_KEEP win was ~1.4% — a rounding error at
   this scale.)
2. **`fold-add-1M` is v3's worst case at 17.5×** — primop-driven iteration.
   **Root-caused 2026-06-04 (see §6): it is per-element ALLOCATION, not raw
   frame setup.** `genList` is cheap (47 insns) and native `OP_CALL` is
   alloc-free per call (fib: 0 closures, 0 attrsets/call); the cost is that
   `foldl'` is a C++ primop re-entering the VM via `callClosure` per element,
   and the curried 2-arg op boxes its captured param in a **per-call size-1
   `let-rec` attrset** (+ ~2 closures + 4 thunks + 2 pairs). **Highest-leverage
   eval-CPU target** — hot map/fold/filter loops are pervasive in nixpkgs.
3. **Memory is the bigger gap: v3 uses 4.4–5.3× more peak RSS than TW** on real
   workloads. It is an **eval-phase** cost (warm ≈ cold RSS) — the
   `v3_arena` (eval working set, historically ~84% Bindings intermediates) is
   570 MB on hello / 1.66 GB on HNE, vs TW's whole-process 138 MB / 570 MB.
   Plus HNE's 594 MB "elsewhere" = the 2449-CU deserialized-bytecode cache.
   This is consistent with the project's [[memory-first]] thesis: memory is the
   higher-slope axis.
4. **Compilation is real but disk-cache-amortized.** hello 635 ms / HNE 4.2 s
   cold; the disk cache collapses it to ~tens of ms of deserialize on re-eval.
   `optimise` (297 ms on hello) is the largest compile sub-phase.

---

## 4. Reproduction
```bash
nix develop -c bash src/libexpr-v3/bench/v3-vs-tw-phases.sh   # CPU/phase/RSS table
# rigorous wall (real workload):
nix run nixpkgs#hyperfine -- --warmup 3 --runs 15 \
  -n TW      "./build/src/nix/nix eval --impure --expr '(import <nixpkgs> {}).hello.drvPath'" \
  -n v3-cold "env NIX_V3_DIRECT_EVAL=1 NIX_V3_NO_DISK_CACHE=1 NIX_V3_MAX_HEAP=6G ./build/src/nix/nix eval --impure --expr '(import <nixpkgs> {}).hello.drvPath'" \
  -n v3-warm "env NIX_V3_DIRECT_EVAL=1 NIX_V3_CACHE_DIR=/tmp/cw NIX_V3_MAX_HEAP=6G ./build/src/nix/nix eval --impure --expr '(import <nixpkgs> {}).hello.drvPath'"
```

## 5. `fold-add-1M` 17× root cause (2026-06-04) — the curried calling convention

`builtins.foldl' (a: b: a + b) 0 (builtins.genList (x: x) 1000000)` — for 1M
elements v3 executes **107M instructions** and allocates **1 GB** (2M closures,
4M thunks, 4M pairs, **1M attrsets**). Decomposition (NIX_VM_STATS alloc + V3_TIMING):

| sub-workload | closures | thunks | attrsets | pairs | insns |
|---|---|---|---|---|---|
| `genList…` + length (build only) | 15 | 1 | **0** | 1.0M | 47 |
| `foldl' (a:b:a)` (no-op lambda) | 2.0M | 4.0M | **1.0M** | 3.0M | 90M |
| `foldl' (a:b:a+b)` (full) | 2.0M | 4.0M | 1.0M | 4.0M | 107M |
| **fib25** (native OP_CALL, single-arg) | **0/call** | — | **0/call** | — | — |

**Conclusions:**
- **genList is NOT the problem** (47 insns; builds the lazy spine, 0 allocs/elem).
- **The `+` is minor** (+1M pairs, +17M insns).
- **It is per-element ALLOCATION** — even a *no-op* lambda allocates ~2 closures
  + **1 attrset** + 4 thunks + 2 pairs + ~90 insns per element.

**Root cause — the curried (one-arg-at-a-time) calling convention** (confirmed
by an A/B/C per-call experiment, 3000 calls each):

| call | closures/call | attrsets/call | thunks/call |
|---|---|---|---|
| **C** 1-arg native (`go n`) | 0 | 0 | 1 |
| **A** 2-arg native (`g n n`) | **1** | **0** | 1 |
| **B** 2-arg via `foldl'`/`callClosure` | **2** | **1** | 4 |

The per-call `let-rec` attrset (`vm.cc:7500 OP_ATTRS_LET_REC_INIT`, size-1) is
**NOT inherent to 2-arg currying** — the native 2-arg call (A) does not allocate
it. It appears **only** on the `callClosure` path (B). Mechanism: a 2-arg apply
`op acc elem` must build the partial application `op acc` as a heap closure.
- **Native (A):** the partial stays *inside* the VM, immediately applied → `acc`
  captured **by value**, no cell. Cost: 1 closure.
- **callClosure (B):** `foldl'` applies in two steps (`step1 = callClosure(op,
  acc)`, then `callClosure(step1, elem)`); the partial **escapes the VM into
  C++** between the re-entries, so v3 makes it heap-stable → **boxes `acc` in a
  `let-rec` cell** + an extra closure/thunks.

Both A and B are symptoms of the SAME thing: **there is no "apply N args at once"
calling convention.** Every multi-arg call manufactures (and on the C++ path,
heap-escapes) an intermediate partial closure. `foldl'`/`map`/`genList` being
C++ primops that re-enter via `callClosure` is what turns A's cheap in-VM partial
into B's escaping, heap-boxed one.

### The fundamentally correct fix: arity-aware uncurried calling (eval/apply)

**Adopt a multi-argument, arity-aware calling convention** (GHC "Making a Fast
Curry" / eval-apply): compile `a: b: body` as ONE arity-2 function; a function of
arity N applied to N args is called **once**, one frame holding all N params, the
uncurried body, **zero** intermediate closures/cells (A's 1 → 0; B's 2+attrset
→ 0). Only a genuine *partial* application (< N args) builds a closure. The same
convention serves BOTH `OP_CALL` and the `callClosure` primop-entry, so a lambda
called from `foldl'` costs what one called from bytecode costs. v3 already has
*fragments* (`Tag::App3`, the compile-time App-spine fold `opt_app_spine_fold.cc`
— which is exactly why A < B); the fix **completes them into a real runtime
arity-aware convention** instead of a compile-time-only half-measure.

**Why the alternatives are patches, not the root:**
- *2-arg `foldl'` path* — this IS the fix, applied to ONE primop; every other HOF
  needs the same. Right idea, wrong scope.
- *Capture-by-value lowering* — secondary (how a genuinely-escaping closure boxes
  captures); doesn't stop the partial being manufactured + escaping for
  fully-applied calls.
- *Bytecode-compile HOFs* — removes the C++ re-entry but the bytecode loop still
  curries (stays case A, 1 closure/call); treats the symptom, not the convention.

Pre-committed gate for the fix (scoped spike: arity-2 fast path in `OP_CALL` +
`callClosure` first, then generalize): byte-identical on the matrix + `--quick`/
`--core` green + hyperfine wall on `fold-add-1M` + fib/ackermann, target A→~0 and
B→~A, reported with the alloc-count delta.

### 5.1 Progress + corrected understanding (2026-06-04, this session)

**Shipped optimizations (all byte-identical: --quick 9/9, --core 19/19):**
| opt | commit | effect on fold-add-1M |
|---|---|---|
| non-recursive `let` demotion (lowering) | `cd1da2577` | per-elem attrsets 1M→1; 107M→94M insns |
| `seq a b → Force(a); b` (lowering) | `72e85e2d9` | drops seq primop App-spine; 94M→87M insns; 12.76→11.18× TW |
| eager-forced-let (multi-use, eval-path) | `c62e05f2f` | `next` thunk gone, MAKE_THUNK 4→3/elem; 87M→83M insns |
| **combined** | | **fold-add 13.67× → 9.52× TW (~30%), insns 107M→83M** |

**Corrected root-cause weighting:** §5 framed the cause as "the curried calling
convention." A later measurement refined it: the C++ `primFoldl` (`primops.cc:1281`,
**v3-native**) is **2.43× TW** vs the bytecode foldl''s 9.52× — so the bytecode
LOOP INTERPRETATION (not just currying) is the dominant residual. **Directive
(user, 2026-06-04): make the bytecode-native VM fast — do NOT switch hot primops
to C++ (that masks the goal). Build the VM optimizations: eval/apply, LICM,
strictness analysis, specialization.** So the §5 "fundamentally correct fix"
(eval/apply) is confirmed as the path; the C++-primFoldl shortcut is explicitly
rejected.

**Why eval/apply is still the next lever (per-elem opcode profile after the 3 opts,
~83/elem):** GET_UPVALUE 21 + GET_LOCAL 15 + SET_LOCAL 12 + RETURN 6 + CALL 5 +
MAKE_THUNK 3 + LIT_PRIMOP 2 + CALL_PRIMOP 3 + TAIL_CALL 3 + ELEM_AT 3 +
**MAKE_CLOSURE 2 (the partial-app closures)**. eval/apply removes the 2
partial-app MAKE_CLOSURE/elem AND gives the recursive `go` a COMBINED `strictArgs`
[i:strict, acc:lazy] — which then unthunks `i+1` (the curry-split that blocked the
reverted #2(A) recursive-callee resolution).

### 5.2 eval/apply implementation plan (the next major effort — multi-session)

Large, multi-component change to lowering + emit + the hottest VM opcode + a PAP
representation; a half-done state risks silent miscompiles, so land it in gated,
byte-identical increments:

1. **Multi-arity lambda lowering** (`cli/lower_v3.hh lowerLambda`): collapse a
   curried chain `x: y: … : body` of simple single-param lambdas (no formals, no
   intervening non-lambda) into ONE Function with `arity=N` and N param VarIds,
   body lowered with all N in scope. Gate `NIX_V3_EVAL_APPLY`. Keep the curried
   form when the gate is off.
2. **PAP (partial application) in the VM** (`closure.hh` + `OP_CALL`): a closure of
   arity N applied to k<N args becomes a PAP{fn, k captured args}; a PAP applied to
   more args accumulates until N, then enters the combined body. This keeps
   single-arg `OP_CALL` + binary `App` working (consistency) — byte-identical, no
   win yet (foundation).
3. **Multi-arg call at emit** (`emit.cc`): recognise the N-deep `App`-spine on an
   arity-N callee and emit `OP_CALL_N` (apply N args in one frame) — bypasses the
   PAP for saturated calls. THIS is the win (removes MAKE_CLOSURE/elem). Over-app
   (m>N): enter with N, then continue applying. Under-app (m<N): build a PAP.
4. **Combined strictArgs** (`opt_func_strictness.cc`): an arity-N Function's
   `strictArgs` now spans all N params (i forced via `if i>=n` → strictArgs[i]
   set), so `applyStrictnessAtCallSites` unthunks the saturated-call args (i+1).
5. Validate each increment: byte-identical matrix + --quick/--core + hyperfine
   fold-add/fib/ackermann; FileCheck `(x:y:x+y) a b` → no intermediate partial-app
   closure (a `--emit-bytecode` check, since the calling convention is a
   bytecode-level property).

## 6. Cross-references
- [[memory-first]] — peak-RSS is the higher-slope axis (confirmed: 4–6× gap)
- [[dispatch-lever-falsified]] / BYTECODE_NGRAM_ANALYSIS §9 — why eval-CPU
  micro-fusions (SET_LOCAL_KEEP) are ~1% and don't close the 2–6× gap
- HNE_BUCKET_DECOMP_2026-05-27 — the "elsewhere" CU-cache 594 MB lineage
- Code: `bench/v3-vs-tw-phases.sh`, `run.cc` (V3_TIMING / importTimingTotals),
  `live_trace.cc` (NIX_V3_MEM_BUCKETS)

---

*Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group. SPDX-License-Identifier: Apache-2.0.*
