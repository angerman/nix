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

## 5. `fold-add-1M` 17× root cause (2026-06-04) — per-element curried-capture allocation

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
- **It is per-element ALLOCATION in `foldl''s` per-element machinery** — even a
  *no-op* lambda allocates ~2 closures + **1 attrset** + 4 thunks + 2 pairs +
  ~90 insns per element.
- **The attrset is the tell:** `NIX_V3_BINDINGS_ATTR` attributes 100001/100000
  elements to **`vm.cc:7500` = `OP_ATTRS_LET_REC_INIT`, a size-1 `let…in`
  rec-attrset.** `(a:b:a)` has no `let` — it is a **v3 lowering artifact**: the
  curried lambda's inner `b: a` captures the outer param `a`, and v3 boxes that
  captured param in a per-call size-1 let-rec cell. **fib proves native
  `OP_CALL` does NOT do this** (param `n` isn't captured by a nested closure →
  0 attrsets/call); only the captured-curried-param path (and the C++-primop
  `callClosure` re-entry that drives it) pays it.

**Why `callClosure` (the "FFI helper") is involved at all:** `foldl'`/`map`/
`genList` are **C++ primops** (inherited from cppnix), not bytecode. To invoke
the user's bytecode lambda each element, the C++ primop re-enters the VM via
`callClosure` (frame push + nested `dispatchLoop` + result marshalling) — TWICE
per element for a curried 2-arg op (`op acc` → partial closure + let-rec, then
`· elem`). TW re-enters too (its C++ `foldl'` → `callFunction`), so re-entry per
se isn't the gap; **v3's re-entry allocates ~9 objects/element where TW
allocates ~1 Env.**

### Fix directions (ranked; each needs its own measured implementation)
1. **Don't box captured scalar params in a per-call let-rec** — capture
   immutable params by value into the closure's upvalue array. Removes the 1M
   attrsets and likely a closure/element. Highest-leverage + broad (helps every
   curried/capturing lambda), but a correctness-sensitive lowering change
   (`lower_v3.cc`; mind the OP_ATTRS_LET_REC_INIT publish semantics at vm.cc:7475).
2. **2-arg call path for `foldl'`** — apply `op acc elem` in one VM entry so the
   intermediate partial-app closure + its let-rec never materialize; halves the
   per-element re-entries.
3. **Bytecode-compile hot HOFs** (`foldl'`/`map`/`filter`) to loops that call the
   lambda via native `OP_CALL` (proven ~alloc-free/call by fib) — the genuinely
   v3-native answer; largest scope.

Pre-committed gate for any fix: byte-identical results on the matrix + `--quick`/
`--core` green + hyperfine wall on `fold-add-1M` (target: close a large fraction
of the 17×), reported with the alloc-count delta.

## 6. Cross-references
- [[memory-first]] — peak-RSS is the higher-slope axis (confirmed: 4–6× gap)
- [[dispatch-lever-falsified]] / BYTECODE_NGRAM_ANALYSIS §9 — why eval-CPU
  micro-fusions (SET_LOCAL_KEEP) are ~1% and don't close the 2–6× gap
- HNE_BUCKET_DECOMP_2026-05-27 — the "elsewhere" CU-cache 594 MB lineage
- Code: `bench/v3-vs-tw-phases.sh`, `run.cc` (V3_TIMING / importTimingTotals),
  `live_trace.cc` (NIX_V3_MEM_BUCKETS)

---

*Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group. SPDX-License-Identifier: Apache-2.0.*
