<!--
Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.
SPDX-License-Identifier: Apache-2.0
-->
# RCA (RESOLVED) — systemic v3-direct drvPath divergence (2026-06-11)

## RESOLVED — SELECT App-writeback poisoned under-applied PAPs

**Fix:** in `OP_ATTRS_SELECT`'s two memoizing App-writeback sites (vm.cc ~8658
IC-hit, ~8853 IC-install), the `if (slot.isAppLike())` force-writeback fired on
an **under-applied closure-PAP**. A PAP is already WHNF; forcing+memoizing it
**saturated it to its result type (Bool)** and wrote that back into the (often
shared) attrset entry. Guarded both with `&& !isUnderappliedClosurePap(slot)`
(mirrors the OP_FORCE PAP break at vm.cc:7161) → a PAP slot is pushed as-is, not
force-written.

**Result:** `run-759 --quick` 0/8 → **6/8** (python3/hello/git/openssl/coreutils/
ripgrep all byte-identical to TW); lang **143/143**; PAP suite 4/4; new
`test/run-pap-select-writeback-tests.sh` 3/3 (pos/neg + python3.drvPath
regression). Same PAP family as the shipped `isFunction`/`typeOf` fix
(f5875a89c).

**Residual A (OPEN — `firefox-getLib-poison`):** `firefox.drvPath` fake-stores via
`OP_CALL: callee is not a closure` **tag=5 (String)** at the cc-wrapper `cc_solib`
thunk doing `getLib cc` (`getLib` = `lib.getOutput "lib"`). `getLib` (a function)
resolves to a **String** *only inside firefox's cc-wrapper rec-scope* — `lib.getLib`
/ `lib.getOutput` / `getOutput "lib"` are all correct lambdas in isolation. Same
*class* as the python3 `pythonAtLeast` poison (a function-valued shared slot
overwritten with its result type), but a DIFFERENT, unpinned vector. RULED OUT
(no-guess, empirical): the optimiser (`NO_OPTIMISE`), eval/apply
(`NO_EVAL_APPLY` — so it is NOT the multi-arity-PAP path), `NO_SATURATED_CALL`/
`NO_CALL_N`/`NO_BETA_REDUCE`, `op_force_slow` (a `V3_DBG_WBAPP` probe showed it
breaks on every recognized PAP and only saturates genuinely-saturated apps), and
the 3 fixed `OP_ATTRS_SELECT` writeback sites + `deepForceList`. Remaining
candidate vectors (need targeted instrumentation, NOT yet done): in-place
cell-update of a Thunk/slot, App-memo writeback, or a rec-binding-slot
mis-resolution (#458 family). Deferred to avoid a speculative fix.

**Residual B (OPEN — `ghc98-slot-leak`):**
`haskell.compiler.ghc98.drvPath` still fake-stores. Native derivationStrict first
fails with `cannot coerce a value to a string: «slot»` (a Tag::Slot reaching
`coerceToString`, vm.cc:1035). A defensive Slot-deref there (deref the let-rec
indirection, mirroring forceValue) is CORRECT but only peels one layer: the next
failure is `cannot coerce a SET to a string: { … system = «slot»; env = «slot»; … }`
— ghc's own derivation attrset with **unforced `system`/`env` slot fields** being
coerced. So the real bug is a **Slot leak**: ghc's let-rec derivation attrs
(system/env/…) remain `Tag::Slot` (unforced let-rec indirections) when
derivationStrict serialises them, where TW has forced values. NOT the PAP class;
needs its own RCA (where do these env/system slots come from un-forced, and the
derivation-set→outPath coercion path). The standalone coerceToString Slot-deref
was reverted (incomplete + no green test on its own) — fold it into the
ghc98-slot-leak fix.

---

(original RCA below, kept for the investigation trail)

# RCA — systemic v3-direct drvPath divergence (2026-06-11)

## Symptom — CRITICAL, ship-blocking

Under pure v3-direct (`NIX_V3_DIRECT_EVAL=1`), **every** nixpkgs package's
`.drvPath` differs from the tree-walker. Confirmed by the authoritative harness:

```
test/run-759-nixpkgs-drvpath-sweep.sh --quick  →  0/8 pass, 7 fail
```

`hello git curl python3 openssl zlib ncurses jq ripgrep nix coreutils bash
gnumake cmake nodejs …` — ALL diverge. `python3` / `haskell.compiler.ghc98` even
fall back to `/v3-fake-store/…` (a secondary failure). Both nixpkgs 26.05 and
26.11 affected ⇒ universal, not version-specific.

Wrong drvPath = wrong store paths = v3 would build the wrong things. This is the
single most important v3 correctness bug.

## NOT the PAP fix (f5875a89c) — same-host-bisected

Reverting vm.cc+primops.cc to the PAP fix's parent (7358badda) and rebuilding gave
the **identical** divergent `hello.drvPath` (`rpj9…`). The divergence pre-dates the
PAP work.

## Localization (top-down, as far as it goes)

- **`derivation { … }` raw primop drvPaths are byte-IDENTICAL** (bare / with env
  attrs / fixed-output). So `derivationStrict` + the .drv serializer + the hasher
  are CORRECT. The divergence is in **stdenv-built derivations' eval-time attrs**.
- Walking the darwin stdenv bootstrap stage chain (`stdenv(.__bootPackages.stdenv)*`):
  - `bootstrap-tools.drv` — **IDENTICAL** (true leaf, 0 inputDrvs).
  - `bootstrap-stage1-stdenv` — **IDENTICAL**.
  - `bootstrap-stage-xclang-stdenv` — **DIVERGES** (first divergent stage).
- Every derivation's *recipe* (builder/args/env/system/outputs/inputSrcs) is
  **byte-identical after hash-normalization** at every level inspected (hello,
  hello.src, stdenv, clang-wrapper-boot, libiconv, gnugrep, gnused, cctools …).
  The divergence is ALWAYS in the **inputDrvs' output hashes** — a pure cascade.
- The divergence therefore enters with **packages built ON the (identical) stage1
  stdenv** — apple-sdk-14.4, llvm, cctools, libiconv, meson, ninja, gnugrep, … —
  whose mutual dependency graph is deep and fully interconnected.

## Why top-down localization is BLOCKED

1. **v3-direct does not persist intermediate `.drv` files** — `nix eval .drvPath`
   writes only the top drv; its computed input drvs are absent from the store, so
   `nix derivation show -r` / a drv-graph descent cannot reach v3's inner nodes.
   (Re-evaluating each input by ATTR works but the graph is deep and every hop is
   another cascade.)
2. **The 16B Value path is retired** (2544024bb) — so the cheap "swap value.hh +
   rebuild" bisect is invalid: the HEAD sources are 8B-only and a 16B-value.hh
   build produces `result is not a string (tag=2)` (broken, not a baseline).

## UPDATE 2026-06-11 (later) — Lever B RULED OUT; measurement corrections

- **NOT Lever B.** Built the pre-Lever-B commit `7beaf0746` (= c690b3f19^, 16B
  Value default) in an isolated `git worktree` (meson+ninja, clean build) and ran
  `nix eval hello.drvPath`: it produces the **identical wrong path** (`rpj9…`) as
  8B HEAD, both ≠ TW (`r77j…`), and the 16B binary fully engages v3
  (insns=6.2M, bridge=0). So the divergence predates Lever B — it is an **older,
  core v3 eval bug**, present in both 16B and 8B. (Confirms the cheap
  value.hh-swap was invalid, and justifies the worktree.)
- **`nix-instantiate` is NOT a v3 baseline** — it emits no v3 stats ⇒ runs TW.
  An earlier "nix-instantiate byte-identical ⇒ core eval correct" conclusion was a
  measurement-gate trap (TW=TW) and is RETRACTED.
- **`nix eval` DOES fully engage v3** for `(import nixpkgs {}).hello.drvPath`:
  `closures=28679 thunks=497053 … bridge=0 insns=6212201` (the earlier `insns=2`
  was a premature cumulative dump — head-N counter trap). So `import` is NOT
  bridged to TW; v3 evaluates nixpkgs natively and computes the wrong drvPath.
- **Ruled out by env-gate:** disk cache, content cache, optimiser, chain-bindings,
  eval-result/drv-hash caches — `hello.drvPath` stays `rpj9…` with each disabled.
- **Self-contained derivation shapes are all byte-identical** (multi-output,
  dep-ref via list/concat, two-refs-one-env, output-ref `a.dev`, drvPath-ctx) —
  so basic derivationStrict + string-context propagation are correct. The bug
  needs the deeper nixpkgs stdenv eval (first divergent stage = **xclang**).

⇒ Revised: a **core v3 eval divergence** (likely a string-context / value subtlety
that only the xclang-stage stdenv eval triggers), exposed on current nixpkgs.
Not version-gated to 26.11 (26.05 also diverges). The historical "byte-identical"
baselines were on older `flake:nixpkgs` snapshots no longer pinned here.

## UPDATE 2026-06-11 (PRECISE ROOT — `nix eval` path, PAP→Bool in derivationStrict)

The divergence is **not** a generic stdenv eval bug — it is a **fake-store
fallback** in v3's native `derivationStrict`, triggered by a specific
**partial-application (PAP) being corrupted to its result type (Bool)**.

**Chain (hello.drvPath, via `nix eval`):**
1. hello's closure transitively includes **python3** (a build tool via meson etc.);
   `V3_DRV_DEBUG=1` shows hello has **7 native-derivationStrict fallbacks, all on
   `python3-3.13.13`**.
2. `nix eval python3.drvPath` → `/v3-fake-store/…` because native derivationStrict
   throws and falls back to the fake-store stub. `V3_DRV_DEBUG` reason:
   **`v3 OP_CALL: callee is not a closure`**.
3. `V3_DBG_CALL=1` pinpoints the callee: **tag=3 (Bool)**, at the bytecode
   `OP_GET_UPVALUE_REC_BINDING passthru ; OP_ATTRS_SELECT pythonAtLeast ; OP_CALL`
   — i.e. nixpkgs `cpython/default.nix:251`
   `optionals ((!isDarwin || passthru.pythonAtLeast "3.14") && …) [...]`.
4. `passthru.pythonAtLeast` is defined (`passthrufun.nix:135`) as
   `lib.versionAtLeast pythonVersion` — an **under-applied PAP** (arity-2
   `versionAtLeast` given 1 arg). In python3's mkDerivation/finalAttrs eval context
   v3 has it as a **Bool**, so `OP_CALL` tries to call a Bool → throws.

So: a PAP is **prematurely saturated to its Bool result** somewhere in python3's
real eval, derivationStrict's `OP_CALL` chokes on the Bool, native bails to the
fake-store stub (drvPath `/v3-fake-store/…`), and every package depending on
python3 (≈ all, via the stdenv build-tool closure) gets a wrong/dropped input →
systemic drvPath divergence. This is **also** the cardano `OP_CALL not closure`.

**Direct access is CORRECT** — `python3.pythonAtLeast` is a `lambda` (callable) in
both v3 and TW; only the **in-context** access during derivationStrict forcing is a
Bool. So it's a context-specific PAP corruption (finalAttrs / rec-binding / App-memo
interaction), NOT a global PAP bug.

**Minimal repro NOT yet isolated** — all of these are byte-identical v3/TW (do NOT
reproduce): structuredAttrs (scalar/list/nested/outputs), `__functor` calls,
`deepSeq`/`seq` of a PAP, `fix`/`rec` passthru-PAP-capturing-sibling called via
self, PAP capturing a **formal parameter** (+ big formals + ellipsis + `optionals`),
and a real `stdenv.mkDerivation` finalAttrs+structuredAttrs+PAP-passthru-conditional
(that one only diverges by transitively depending on python3). The trigger needs
python3's *full* construction (makeScope + passthruFun + overrideAttrs + finalAttrs).

**Next surgical step:** trace the *value* of `passthru.pythonAtLeast` at the failing
`OP_CALL` (is it a memoized App whose `evaluated` slot holds a Bool? a rec-binding
slot resolving to the wrong value?) — e.g. instrument the OP_CALL-on-non-closure
path to dump the callee's provenance, or bisect python3's actual passthru/native-
BuildInputs expression with real lib/stdenv.

## (earlier) Leading hypothesis (UNCONFIRMED)

A **string-context divergence**: at some bootstrap derivation, v3 computes a
derivation env string with the same TEXT but a different string-CONTEXT (the set
of store-path references), so its inputDrvs/inputSrcs set differs → different
.drv → cascades to everything. (#682 is the canonical instance of this class:
"primToFile ignored contents' string context → wrong-hash drvPath".) The
"recipe text identical, only input hashes differ at every level" signature is
consistent with a context bug at the bottom that I could not reach top-down.

Plausibly introduced by **Lever B (Value 16→8B NaN-box, c690b3f19 / 2544024bb)** —
the only large eval-affecting change since the 2026-06-07 "M5/cardano byte-identical"
baseline; the regression window is {c690b3f19, 2544024bb, 4069a4cdb} (doc-only RCA
commits excluded). UNCONFIRMED because the bisect needs a full pre-Lever-B build.

## Next steps (pick one; each a real sub-effort)

1. **Confirm Lever B via a full pre-Lever-B build** — `git worktree add` at
   `c690b3f19^` (16B default), fresh `meson setup` + `ninja`, run #759. Green ⇒
   regression is in the 8B Value code (then diff the 8B codec / call sites,
   focusing on string-context propagation in derivationStrict's attr path).
2. **Build a "v3 persists intermediate drvs" debug mode** (or use `nix-instantiate`
   if it writes the full graph under v3) → unblock the drv-graph descent to the
   clean root.
3. **NIX_TRACE_EVAL differential** on the smallest divergent bootstrap derivation
   (e.g. boot `meson`/`ninja`/`libiconv`) — diff v3-vs-TW force/context order to
   pin the exact op that adds/drops a context entry.

## Repro pointers

- `NIX_V3_DIRECT_EVAL=1 build/src/nix/nix eval --impure --raw --expr
  '(import <nixpkgs> {}).hello.drvPath'` vs the same without the env var.
- `test/run-759-nixpkgs-drvpath-sweep.sh --quick` (0/8).
- bootstrap chain probe: `(import <nixpkgs> {}).stdenv(.__bootPackages.stdenv)*.drvPath`
  — stage1 IDENTICAL, xclang DIVERGES.

---

## firefox residual — DETERMINISTIC RCA (2026-06-11, deep instrumentation)

**Status: root cause localized to a single deterministic mechanism; no fix shipped
(fix domain identified but needs design, not a speculative one-liner).**

### Symptom (HARD, value-based facts)
- `(import <nixpkgs> {}).firefox.drvPath` under `NIX_V3_DIRECT_EVAL=1` → native
  `derivationStrict` throws `v3 OP_CALL: callee is not a closure tag=5` (a **String**),
  falls back to `/v3-fake-store/6444c591…-firefox-151.0.3.drv`. Deterministic
  (identical fake-store hash across 3 runs).
- The failing op is **`OP_TAIL_CALL` at codeOff=1434** — a thunk whose body is
  `getLib cc` (`[1434] GET_UPVALUE_REC_BINDING_SLOT getLib; [1438] GET_UPVALUE_REC_BINDING cc;
  [1443] OP_TAIL_CALL`). The popped callee `fun` = **String `'21'`** (V3_DBG_TC_STR).
- `getLib` = `lib.getOutput "lib"` (RBS leaf closure `'output'`, arity 2, depth 1 →
  a proper under-applied PAP — the SAME class as python3's `pythonAtLeast`).

### Mechanism (verified by single-step canary + writeback poison probes)
1. The `getLib cc` thunk reads `getLib` into local 0 as a `Tag::Slot` → its
   rec-binding (App PAP / Thunk; **never a String at read** — `foundTag` ∈ {App,Thunk}
   across all 18 reads).
2. At `OP_TAIL_CALL`, `fun` = the Slot → `op_call_dispatch` → `op_call_iter_force`
   forces the Slot and writes the result back to the fun stack slot, then **retries**
   the op. The IPT trace shows `OP_TAIL_CALL@1443` runs **twice**: first with
   `fun = Slot`, then (after the writeback) with `fun = String "21"`.
3. So **forcing `getLib`'s Slot → the String "21"** (WB-STACK-POISON: idx of the fun
   slot, oldTag=16 Slot → String "21"). I.e. **getLib's shared rec-binding evaluates
   to / is memoized as the String "21"** instead of the `getOutput "lib"` PAP.
4. Independently, an **STG-8 `OP_RETURN` in-place cell-writeback** (`*cell = retVal`)
   writes the String "21" into a `getLib` binding cell (RET-CELL-POISON), with the
   returning frame at codeOff=1434. The codeOff=1434 thunk is stored at an entry named
   **`derivationArgs`** (slot 2 of a size-4 Bindings) — so a `getLib` binding cell and a
   `derivationArgs`-context binding cell **alias the same `Value*`**. Writing the
   `derivationArgs`/`getLib cc` result corrupts the shared `getLib` PAP.

### Conclusion
A **Bindings overlay/merge cell-sharing** bug (overrideAttrs / extendDerivation / `//`
— the #455-family class): a merged Bindings reuses the base's `Value*` entry cells, and
the STG-8 in-place cell-writeback (`OP_ATTRS_REC_SET` sets `thunk->cell =
&entries[i].value`; `OP_RETURN` does `*cell = retVal`) then **overwrites the shared base
`getLib` PAP binding with a String**, so every later `getLib cc` does `OP_CALL` on a
String → native fallback → fake-store. **Fix domain: STG-8 cell privacy under Bindings
overlay/merge** (an overlay entry must get a FRESH cell, never reuse a base entry's
`Value*`, before STG-8 attaches it as a writeback target).

### Ruled OUT (same-host bisects / probes)
- ChainBindings (Lever A): `NIX_V3_NO_CHAIN_BINDINGS=1` → identical fake-store hash.
- Major GC: `NIX_V3_NO_MAJOR_GC=1` (16G heap) → identical. Nursery default-off.
- The 3 OP_ATTRS_SELECT(/DYN) writebacks (already PAP-guarded), `deepForceList`,
  stale rec-slot IC, optimizer (`NO_OPTIMISE`), eval/apply force paths (both
  `forceValue` and `op_force_slow` check `isUnderappliedClosurePap` BEFORE the App
  `evaluated` memo, so a recognized PAP is never saturated/memo-poisoned by them).

### Caveat
Pointer-based probes (`dbgGetLibCells`) can false-positive via freed-cell address
reuse over a long eval; the value-based facts (1)-(3) above are the load-bearing ones.

### Repro / probes (all reverted — were debug-only, never shipped)
`V3_DBG_TC_STR` (failing-callee + local0), `V3_DBG_CANARY` (single-step slot watch +
`CANARY-ARM(1434)`), `V3_DBG_WBPOISON` (writeback/cell poison + `RET-CELL-POISON` +
`REC-SET-1434`), `V3_DBG_IPTRACE_1434` (ip+stack-size trace), `V3_DBG_FSS`
(Slot→String force classifier).

---

## firefox residual — CORRECTION (2026-06-11, later): STG-8 cell-alias mechanism FALSIFIED

The "STG-8 cell-writeback / overlay cell-sharing" mechanism asserted in the section
above is **FALSIFIED** by airtight, CU-agnostic, value-based probes. **Rule 0
walk-back.** Do NOT trust the STG-8/derivationArgs-alias story.

**Why the earlier story was wrong — two confounds:**
1. **`codeOffset` is PER-CU, not global.** Probes gated on `closure->desc->codeOffset
   == 1434` matched DIFFERENT functions across different compilation units, so
   "RET-CELL-POISON frame codeOff=1434", "REC-SET-1434 entryName=derivationArgs",
   "WB-STACK-POISON codeOff=1434" mixed unrelated instances.
2. **Pointer-reuse false positives.** `dbgGetLibCells` accumulated `found` cell
   pointers over a long eval; freed cells get reused, so a later unrelated write to a
   reused address spuriously "matched" a getLib cell.

**Airtight probes that came back NEGATIVE (value-/name-based, CU-agnostic):**
- `cellWrite`: no cell holding a `getOutput <arg>` PAP (leaf Closure name "output",
  arity 2, depth 1) is ever overwritten with a non-callable. → getLib's binding cell
  is NOT clobbered via the STG-8 `OP_RETURN` writeback / `applyForceWriteback`.
- `thunkSetEvaluated`: no thunk named "getLib" is ever memoized to a String.
- `pairSetEvaluated`: no `getOutput`-App pair is ever memoized to a String (App memo).
- `OP_GET_UPVALUE_REC_BINDING_SLOT` resolution (`sym==getLib`): the resolved entry's
  name ALWAYS == getLib and its value is NEVER a String → the rec-slot IC / binary
  search is correct; getLib resolves to the right App-PAP/Thunk binding.
- `op_force_slow` PAP-recognition: a depth-1 `getOutput`-App (= getLib's PAP) is
  recognized as a PAP **5691/5691** times (`isUnderappliedClosurePap=1` → break →
  never saturated). The only `isPAP=0` cases are depth-2 (fully-applied `getLib cc`,
  legitimately saturated) and a different arity-3 closure that happens to share the
  param name "output".

**What remains SOLID (narrow):**
- firefox.drvPath → `/v3-fake-store/6444c591…` deterministically (3/3 runs).
- native `derivationStrict` throws `OP_CALL: callee is not a closure tag=5`; the
  callee String content is `"21"` (dumped at the throw — reliable, single instance).
- The failing-frame disasm (reliable: actual failing `cu`+ip) shows `getLib cc`.
- NOT ChainBindings, NOT major GC, nursery default-off.

**Mechanism: RE-OPENED.** getLib resolves correctly and its binding is never written
to a String through any standard path, yet the failure presents as `getLib`→String.
The next investigation must (a) key probes on `(cu, codeOffset)` PAIRS or value
signatures, never bare codeOffset; (b) avoid accumulating-pointer-set matching; and
(c) bridge the gap between "getLib resolves correctly + is never clobbered" and "the
failing OP_CALL sees a String callee" — e.g. by capturing the exact `cu` pointer of
the failing frame (from V3_DBG_CALL) and instrumenting ONLY that cu's getLib reads,
or by tracing the single failing `derivationStrict` call's argument force end-to-end.

**Lesson (codify):** per-CU `codeOffset` and accumulating raw-pointer sets are NOT
valid identity keys for cross-eval correlation — they silently conflate instances.
Use value/name signatures or `(cu, off)` pairs. (Sibling of the same-host-bisect and
head-N-counter traps.)
