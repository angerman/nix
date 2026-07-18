# nix-eval-jobs → v3 integration: worker architecture, models, and plan

**Date:** 2026-07-18
**Scope:** the concrete patch design for making Hydra's evaluator (`nix-eval-jobs` v2.34.1) drive the v3 bytecode VM, based on reading its actual source. Refines the Phase-0 "one bounded patch" estimate with what the worker really does.
**Copyright:** (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group. SPDX-License-Identifier: Apache-2.0.

---

## 0. TL;DR

The integration is **real work but bounded** — bigger than "read one drvPath," smaller than "rewrite nix-eval-jobs." The worker (`src/worker.cc`) evaluates the jobs root **once**, then per job it (1) **descends** an attrPath, (2) **detects** whether the value is a derivation, (3) **extracts** `name`/`drvPath`/`outputs`/`meta`, and (4) **discovers** children to recurse into. Steps 1-4 are all coupled to the tree-walker's `nix::Value` (via `findAlongAttrPath`, `getDerivation`/`PackageInfo`, `collectAttrsForRecursion`). For v3 to own the *heavy* eval (the nixpkgs/haskell.nix descent — the whole point, since that's what makes the CU shareable and the RSS lower), v3 must drive 1-4, not just return a final string.

**Recommended: Model B (v3-native accessors), not a Value bridge.** Add ~6 small v3-native accessor functions (descend / is-derivation / name / drvPath / outputs / meta) and thread them into the worker, keeping **all store-side logic unchanged** (`readDerivation`, `queryMissing`, `queryCacheStatus`, JSON serialization all key off the `drvPath` + store, not the value). Root eval is a synthesized `getFlake` expression through `runRootExprFromString`.

**#1 risk already retired (2026-07-18):** v3 `getFlake` yields a **byte-identical drvPath** vs TW for a real derivation-through-a-flake (`…-foo-1.0.drv`, store-hash-identical). So the flake→eval→drvPath path is proven; the remaining work is the worker wiring, not the evaluator.

---

## 1. What the worker actually does (`src/worker.cc`, read 2026-07-18)

```
worker(args, toParent, fromParent):                       # one process per worker
  state = make EvalState                                  # ONE EvalState
  vRoot = initializeRootValue(state, autoArgs, args)      # evaluate the jobs ROOT once (lazy)
  loop processJobRequest(...):                            # until restart/exit
     read "do <attrPath-json>" from parent
     vTmp  = findAlongAttrPath(state, attrPathS, autoArgs, vRoot)   # (1) DESCEND (forces along path)
     value = autoCallFunction(autoArgs, vTmp)
     if value is attrs: processDerivation(state, value, …):
        packageInfo = getDerivation(state, value, false)            # (2) DETECT: is it a derivation?
        if !packageInfo: return Attrs{ collectAttrsForRecursion(…) }# (4) DISCOVER children (recurse)
        drv = Drv::fromPackageInfo(attrPathS, state, packageInfo,…) # (3) EXTRACT name/drvPath/outputs/meta
        return Job{ drv, … }
     write response JSON to parent
     if maxrss > maxMemorySize: restart                   # the evaluator_max_memory_size wall
```

- **Root eval** (`initializeRootValue` → `evaluateFlake`): for a flake jobset it builds an `InstallableFlake` and calls `flake::callFlake` (no fragment) or `flake.toValue` (fragment). This is the "all jobs are flakes" path.
- **Job discovery is lazy + recursive**: the *parent* collector asks for `["a"]`, the worker returns `a`'s child names if `a` isn't a derivation (gated by `recurseForDerivations`), the collector then asks for `["a","b"]`, etc. So the worker must be able to (a) descend to any attrPath and (b) list children — both on whatever value type v3 produces.
- **Extraction is mostly store-driven** (`src/drv.cc::fromPackageInfo`): only `name`, `drvPath`, `outputs` (names), and `meta` come from the *value*; `system`/`inputDrvs`/`requiredSystemFeatures`/`cacheStatus` come from `readDerivation(drvPath)` + `queryMissing` — **store operations that need only the drvPath.** This is why Model B is viable: get the drvPath natively and the rest is unchanged.

---

## 2. Two integration models

### Model A — bridge the v3 job value to a `nix::Value`
v3 evaluates root + descends; for each job, marshal the v3 derivation attrset into a TW `nix::Value`, then feed the existing `getDerivation`/`PackageInfo`/`fromPackageInfo` unchanged.
- **Pro:** downstream code untouched.
- **Con:** a faithful v3→TW bridge for a full derivation (with `.drvPath` context, `.outputs`, arbitrary nested `.meta`) is exactly the per-call marshalling that crosses GC ownership — the thing v3's V3-NATIVE constraint (`LESSONS §1.2`) warns against. And discovery/recursion *still* needs to walk the v3 tree. So Model A doesn't actually avoid the v3-native tree walk; it only reuses the extraction. Net: more coupling, more marshalling, less clean. **Not recommended.**

### Model B — v3-native accessors (RECOMMENDED, V3-NATIVE-consistent)
Add a small v3 accessor surface and let the worker call it; keep everything store-side unchanged.

**~6 accessor functions (v3-native, on `v3::Value` via `v3::forceValue` + attrset lookup):**
1. `v3DescendAttrPath(root, ["a","b",…]) -> v3::Value` — the attrPath descent (already exists as the `runV3DirectEval` loop, `eval.cc:142-168`).
2. `v3IsDerivation(v) -> bool` — force `v`, check it's attrs with `.type == "derivation"` (mirrors `getDerivation`).
3. `v3ChildAttrNames(v, &recurse) -> vector<string>` — lexicographic child names + read `recurseForDerivations` (mirrors `collectAttrsForRecursion`).
4. `v3DrvPath(v) -> string` — force `.drvPath` → store-path string (the proven path).
5. `v3Name(v) -> string`, `v3System(v) -> string` — force `.name` / `.system`.
6. `v3Outputs(v) -> vector<string>` + `v3Meta(v) -> json` — force `.outputs`/`.meta` (meta via v3's value→JSON; or defer meta to a TW re-force of just the drv value if the JSON printer is TW-only — a narrow, per-job bridge acceptable because it's leaf-only).

**Worker wiring (which functions change):**
- `initializeRootValue`/`evaluateFlake` → produce a v3 root by running `(builtins.getFlake "<locked-ref>")` (+ `--select`/fragment) through `runRootExprFromString`; hold the `RootResult.cu` + a `VMState` alive for the worker's lifetime (like `runV3DirectEval`).
- `processJobRequest`'s `findAlongAttrPath` → `v3DescendAttrPath`.
- `processDerivation`'s `getDerivation` → `v3IsDerivation`; `collectAttrsForRecursion` → `v3ChildAttrNames`.
- `Drv::fromPackageInfo`'s value-parts (`queryName`/`requireDrvPath`/`queryOutputs`/`queryMeta`) → the accessors; **its store-parts (`readDerivation`, `queryCacheStatus`, `queryInputDrvs`) stay byte-for-byte unchanged** (they take a `drvPath`/`Derivation`).
- `meson.build` → add `dependency('nix-expr-v3')` + include the v3 headers (same wiring the 2.34 backport added to the `nix` CLI).

**The locked flakeref is load-bearing:** the synthesized `getFlake` must use the same rev-pinned ref nix-eval-jobs/Hydra resolved (`git+file://…?rev=…&narHash=…`), or drvPaths diverge. nix-eval-jobs already computes the locked flake (`getLockedFlake()`); pass its locked ref to `getFlake`. A4 flake-lock keying caches flake metadata cross-process by byte-id — a bonus for the fleet.

---

## 3. De-risking status + plan

| step | status |
|---|---|
| v3 evaluates flakes natively (getFlake sole impl → callFlakeV3) | ✅ verified (Phase-0) |
| v3 getFlake `.attr` engages under `NIX_V3_REQUIRE=1`, no fallback | ✅ verified (Phase-0) |
| v3 getFlake **drvPath byte-identical to TW** on a real derivation | ✅ verified 2026-07-18 (`…-foo-1.0.drv`) |
| nix-eval-jobs builds against the v3 nix (`nix-expr-v3` dep) | ☐ next |
| Model-B accessors + worker wiring | ☐ the patch |
| shadow: real flake jobset, v3 drvPath set == TW drvPath set | ☐ Phase-1 gate |
| fleet: N workers share one AOT (RSS + `evaluator_max_memory_size`) | ☐ (WS-5 already proved 105 MB Shared_Clean; confirm under the patched worker) |

**Phased build:**
1. **Build** nix-eval-jobs v2.34.1 against `angerman/2.34-v3` (expose `nix-expr-v3` pkg-config from the v3 nix; point nix-eval-jobs' flake `nix` input at the v3 nix). Gate: unpatched nix-eval-jobs builds + runs (TW) — establishes the baseline oracle.
2. **Patch** Model-B accessors + worker wiring behind `NIX_V3_DIRECT_EVAL` (fallback to TW when unset — the existing gate semantics). Gate: `NIX_V3_REQUIRE=1` forces a real flake job through v3, no fallback.
3. **Shadow parity** on a real flake jobset: run patched (v3) and stock (TW) nix-eval-jobs over the same locked flake; **byte-compare the emitted `drvPath` set per attrPath.** Gate: identical. Any diff = a WS-1-class correctness bug, fix it, never touch the jobset.
4. **Fleet density**: run the patched worker with `NIX_V3_AOT_CACHE_FILE`; confirm cross-worker `Shared_Clean` + per-worker RSS drop under `evaluator_max_memory_size`.

**Estimate:** the accessors are small (v3 already forces attrs + reads strings); the bulk is build setup + worker wiring + shadow debugging. Bounded, ~2-3 focused days, not weeks — and the highest-risk piece (drvPath parity) is already retired.

---

## 4. Honest scope correction

Phase-0 called this "one bounded patch." Reading the worker shows it touches **discovery + detection + extraction**, all TW-`Value`-coupled — so it's a *small accessor library + worker wiring*, not a one-liner. It is still bounded and does not need a new flake front-end (v3 has native getFlake) or a Value bridge (Model B avoids it). The store-side pipeline (the majority of `drv.cc`) is untouched. No fundamental blocker surfaced; the scope is "a real patch across ~3 files + a build," and the correctness core is proven.
