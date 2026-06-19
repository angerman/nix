# v3-vs-TW structural analysis — why v3 doesn't beat the tree-walker, and what's left (2026-06-19)

Grounded in code (4 parallel read-only sweeps of v3 value/heap, dispatch, opt-pass
inventory, and the stock TW evaluator) + this session's exhaustive git CPU
measurements. Excludes compilation/lowering time per the question framing.

## The quantitative reframe (the key)

git.drvPath cache-off = **4.8 s over 11.3 M bytecode ops**. At ~15 ns/dispatch that
is **~170 ms ≈ 3.5 % of the eval**. The other **~96 % is the work *inside* the op
bodies** (allocation, attribute lookup, thunk forcing, primop execution, string
building). This is why every dispatch/fusion/scope/strictness lever this session
came back neutral — they nibble a 3.5 % slice. **v3's git gap is work-bound, not
dispatch-bound.**

## v3 vs TW scorecard (grounded)

| Dimension | v3 | TW (cppnix) | Winner |
|---|---|---|---|
| Value word | 8 B, NaN-boxed, inline small-ints (`value.hh:262`) | 16 B, bitpacked discriminator (`value.hh:585`) | **v3** |
| Variable access | O(1) slot index `valueStack[base+slot]` | O(level) `Env`-parent-chain chase (`eval.cc:919-925`) | **v3** |
| Dispatch | `switch` (computed-goto deferred, `vm.cc:2,3793`) | virtual `Expr::eval()` | ~wash |
| Thunk/closure | hdr 24–32 B **+ upvalues copied inline** (8 B each), no sharing (`closure.hh:60-181`) | 16 B `{Env*,Expr*}`, **one `Env` shared per scope** (`eval.hh:192-196`) | **TW** on capture-heavy code |
| Allocator | Boehm + **bump nursery + gen-major** | Boehm only, ~never collects below threshold | **v3** (memory) |
| Ops per source construct | **expands** (`let x=e` → MAKE_THUNK + SET_LOCAL + upvalue pushes) | 1 node-visit (`maybeThunk` into shared Env slot) | **TW** |
| JIT | none | none | — |

v3 already **wins** the two things people blame tree-walkers for (fat values, slow
var lookup): 8 B values + O(1) slot access. It still loses 3.4×. The table says why:
v3 loses on the axes that dominate a work-bound single-shot lazy eval —
**allocation volume and ops-per-construct** — with **no JIT** to escape interpretation.

## Why v3 doesn't beat TW on CPU (3 structural reasons, by impact)

1. **One-shot lazy eval is the worst case for a bytecode VM, best case for a
   tree-walker.** A VM amortizes dispatch over *repeated* execution; Nix forces each
   thunk *once*. TW's "re-walk the AST" costs nothing extra — each node is visited
   exactly once, *fewer* times than v3 executes the bytecode it lowered that node
   into. The repetition that *does* exist (same `lib.*` called millions of times) is
   captured by the **disk cache**, which is exactly why cache-on git is fast and
   cache-off is 3.4×. **v3's real production win is the cache, not interpreter speed.**
2. **Lowering expansion + fat closures = more allocation work.** TW: one `allocEnv`
   into a *shared* Env + `maybeThunk`. v3: `MAKE_THUNK` copies upvalues into a fresh
   24–32 B object + `SET_LOCAL`. On capture-heavy nixpkgs code v3 allocates more
   objects and bytes; allocation + GC scan is *work*, not dispatch.
3. **No JIT.** The classic way to beat a tree-walker on raw CPU (V8, LuaJIT,
   GHC-native, PyPy). v3 is purely interpreted → it can never be faster *per op* than
   the work the op inherently costs; it can only do fewer/cheaper ops, which the 26
   existing passes have largely harvested (this session: remaining peephole headroom
   ≤2-3 %, all neutral on git).

## Why on memory

The Layer-A tax, now precisely attributable: **v3 copies upvalues into every
closure/thunk; TW shares one `Env` per scope.** Scope of K bindings × U captures:
TW ≈ 1 shared Env; v3 = K×(24 B + 8U). v3 claws much back (8 B vs 16 B values,
nursery/gen-major reclaiming churn — already −624 MB on M5), so it wins memory on
*some* workloads; the per-closure upvalue copy is why it isn't a blowout everywhere.

## Levers left (ranked by ceiling × tractability)

**Real ceiling, big effort**
- **Environment sharing** (STG/TW-style): closures hold `Env*`+index instead of
  copied upvalues. The ONLY lever that hits **both** CPU (alloc work) and memory
  (Layer-A) at once. Major representation change; high risk.
- **JIT hot function bodies** (V8/LuaJIT trace, or method-JIT the repeatedly-called
  `lib.*`): the only thing that beats interpretation on raw CPU. Highest ceiling,
  months of effort. *The* answer to "beat TW single-thread CPU."

**Medium effort, real on the right workload**
- **Shapes / hidden classes + PICs for attribute access** (V8 core idea).
  `ATTRS_SELECT`+`ATTRS_HAS` ≈ 4.3 % of git ops, each an O(log n) binary search over
  sorted `Bindings`. nixpkgs attrsets (derivations, package sets) have *stable
  shapes* → a shape map turns search into an O(1) cached offset. v3 has only a
  monomorphic 1-way slot IC today, NOT shapes. Targets *work*, not dispatch. Has a
  clean Rule-0 falsifier (does it clear the bar on git's attr-ops?).
- **Computed-goto / direct-threaded dispatch**: the one cheap dispatch win v3
  deferred. But git is ~3.5 % dispatch → expect ~1-2 %, below bar. Low priority given
  the work-bound finding.

**Orthogonal axes where v3 structurally wins (TW can't follow)**
- **Content-addressed result caching / incrementality** (`LINKING_DESIGN`/salsa):
  captures cross-eval repetition the bytecode can't. Where v3's production advantage
  actually lives.
- **Parallel / multi-core eval** (`PARALLEL_EVAL`): TW is single-threaded by design.
  A 4× from 4 cores beats any single-thread peephole.

## Literature: tried vs not

- NaN-boxing (LuaJIT) — DONE (8 B values).
- Register VM (LuaJIT) — partial (`R_CALL`/`R_STR_CONCAT2`); stack-dominated.
- Stream fusion / deforestation (GHC) — tried, falsified/opt-out.
- Strictness/demand analysis (GHC) — 5 passes; this session proved CPU-neutral on git.
- eval/apply vs push/enter (STG) — v3 uses eval/apply; push/enter workload-dependent, **untried**.
- Pointer/constructor tagging, tables-next-to-code (STG) — value word is tagged; thunks still deref'd to check WHNF; minor.
- **Hidden classes + PICs (V8) — NOT tried; most promising medium lever.**
- **Trace/method JIT (V8/LuaJIT/PyPy) — NOT tried; highest ceiling.**
- Content-addressed code (Unison) — designed (`LINKING_DESIGN`), not for raw CPU.

## Conclusion

v3 will **not** beat a tuned tree-walker on single-thread, cache-off, one-shot CPU
through any incremental lever — that race is structurally unfavorable and the
peephole space is exhausted (fusion, scope-push, strictness all neutral; the gap is
work, not dispatch). The three real wins are structural: **environment sharing**
(both axes), **shapes+PICs** (attr-lookup work), and the long game — **JIT** (raw
CPU) + **caching/parallelism** (production), the axes where v3 isn't fighting the
tree-walker on its home turf.

*Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.
SPDX-License-Identifier: Apache-2.0*
