# RCA & Plan: v3-direct nixpkgs `{family}` STR_CONCAT divergence

**Date:** 2026-05-11
**Owner:** Moritz Angermann
**Status:** RCA partial; plan documented; fix not yet started

---

## Symptom

v3-direct on real nixpkgs fails:

```
error: v3 STR_CONCAT: cannot coerce type to string (tag=7=Attrs)
  failing operand: attrs size=1, key={family}
```

Same expression in tree-walker (TW): completes in 0.27 s.

Fails on every shape that triggers nixpkgs bootstrap:
- `(import <NP> {}).system`
- `(import <NP> {}).pkgs`
- `builtins.attrNames (import <NP> {})`
- **`builtins.isAttrs (import <NP> {})` — fails even though only WHNF type-tag is asked for**

Does NOT fail on direct calls into `lib.systems`:
- `(import <NP>/lib).systems.parse.tripleFromSystem (mkSystemFromString "x86_64-linux")` → works
- `(import <NP>/lib).systems.elaborate "x86_64-linux"` → works

So the bug is in the **nixpkgs bootstrap chain**, not in `lib.systems` itself.

## Where it fires

Pinpointed via enhanced `V3_DBG_STRCONCAT` (commits HEAD~1..HEAD):

| Frame | File:line | Lambda | Note |
|---|---|---|---|
| fr[7] | `lib/systems/default.nix:94:18` | `<thunk>` (CFF_THUNK_RETURN) | **STR_CONCAT site** = `config = parse.tripleFromSystem final.parsed;` |
| fr[6] | `lib/systems/default.nix:47:51` | `n` (filter lambda) | inside `equals`'s `removeFunctions` |
| fr[5] | `lib/systems/default.nix:47:25` | `a` (removeFunctions) | |
| fr[4] | `lib/systems/default.nix:49:8` | `b` (`a: b: removeFunctions a == removeFunctions b`) | |
| fr[3] | `pkgs/top-level/default.nix:129:3` | `crossSystem` (CFF_THUNK_RETURN) | the let-block `if ... lib.systems.equals system localSystem ...` |
| fr[2] | `pkgs/top-level/default.nix:104:9` | `<thunk>` (CFF_THUNK_RETURN) | the `((localSystem.isDarwin && localSystem.isx86) \|\| (crossSystem.isDarwin && crossSystem.isx86))` thunk |
| fr[1] | `pkgs/top-level/default.nix:40:1` | `args` | top-level pkgs wrapper |
| fr[0] | `<no-pos>` | `<anon>` | entry |

The failing call is `tripleFromSystem`'s string interpolation:

```nix
# parse.nix line 980
"${cpuName}-${vendor.name}-${kernelName kernel}${optExecFormat}${optAbi}"
```

= 7 parts. Operand at index 0 is `cpuName`. From `cpuName = if kernel.families ? darwin then darwinArch cpu else cpu.name`. For `x86_64-linux` we take the `else` branch: `cpu.name`. **`cpu.name` is supposed to be the string `"x86_64"`**.

`cpuTypes.x86_64` in TW is `{_type, arch, bits, family, name, significantByte}` — full 6-field attrset.
**In v3-direct, the operand that reaches STR_CONCAT is `{family}` — a 1-entry attrset.**

## Why it matters that `builtins.isAttrs` triggers it

A type-tag query should be O(1) on the WHNF outer attrset. The fact that even `isAttrs (import <NP> {})` triggers the STR_CONCAT means **nixpkgs's `args` lambda body runs work that's not strictly required to know the outer is an attrset**.

Inspection of `pkgs/top-level/default.nix:97-114`:

```nix
checked =
  (throwIfNot ...)
    (throwIfNot ...)
    ...
    (if ((localSystem.isDarwin && localSystem.isx86) || (crossSystem.isDarwin && crossSystem.isx86))
         && config.allowDeprecatedx86_64Darwin == false
     then x86_64DarwinDeprecationWarning else x: x);
```

`checked` is referenced from the body to gate the pkgs construction. Forcing the top-level attrset shape forces `checked`, which forces the `isDarwin / isx86` checks. To resolve `crossSystem.isDarwin`, v3 forces the `crossSystem` thunk.

`crossSystem` thunk body (line 129–133):

```nix
crossSystem =
  let system = lib.systems.elaborate crossSystem0;
  in if crossSystem0 == null || lib.systems.equals system localSystem
     then localSystem else system;
```

Default `crossSystem` is `localSystem` (line 47), so `crossSystem0 = localSystem` (line 73), so `crossSystem0 == null` is `false`. The `||` does NOT short-circuit; we DO evaluate `lib.systems.equals system localSystem`.

`equals` (lib/systems/default.nix:44–49):

```nix
equals =
  let removeFunctions = a: removeAttrs a (filter (n: builtins.isFunction a.${n}) (attrNames a));
  in a: b: removeFunctions a == removeFunctions b;
```

`removeFunctions` calls `attrNames a`, then for EACH name iterates `builtins.isFunction a.${n}`. **This deep-forces every field of `system`.** That includes `config`.

`config` thunk body (line 94): `parse.tripleFromSystem final.parsed`.

So the chain that reaches STR_CONCAT is:
1. `import <NP> {}` → constructs the top pkgs attrset (lazy bindings, but the outer shape needs the `args` body to run)
2. `args` body forces `checked` (line 97) — gating the construction
3. `checked` forces `crossSystem.isDarwin` (line 104)
4. `crossSystem` thunk forces `lib.systems.equals system localSystem` (line 133)
5. `equals` calls `removeFunctions system` → walks every field of elaborate's result
6. Walking touches `system.config` → forces `parse.tripleFromSystem final.parsed`
7. `tripleFromSystem` interpolates `"${cpu.name}-..."` → STR_CONCAT
8. `cpu.name` returns `{family}` instead of `"x86_64"` ← **the divergence**

**TW reaches step 7 too — it also evaluates `equals`. But TW gets a string back from `cpu.name`. v3 gets `{family}`.**

## Hypotheses for the divergence

Ordered by likelihood given the evidence:

### H1 — `cpu` itself is a partial Bindings; `.name` lookup misses, falls back somewhere

If v3 hands the formal-destructured `cpu` parameter a wrong/partial value, `cpu.name` lookup would either:
- Return null (and OP_ATTRS_SELECT errors with "missing attr") — NOT what we see
- Be re-routed to a synthesized Bindings via cell/slot — possibly returns `{family}`

The `{family}` attrset has size=1 with EXACTLY the field `family`. This is the same shape one inner cpuTypes value has after STRIPPING all attrs except `family` — which is **suspiciously specific**.

### H2 — `cpu` parameter is the INNER cpuTypes record (pre-setTypes merge)

`setTypes` mapAttrs over inner cpuTypes values like `{ bits=64; significantByte=...; family="x86"; arch="x86-64"; }`. After the mapper runs, the result has 6 fields. **If v3 leaks the INNER value (4 fields) somewhere, we'd see {bits, significantByte, family, arch}. We see {family} only.**

So the value isn't the unmerged inner record; it's even smaller.

### H3 — `{family}` is an OP_ATTRS_INIT result for a Bindings that's being constructed and only `family` has been set

This is the partial-Bindings shape that the retired registry was supposed to handle. With THUNK_ALL default-on (Phase 3.2), inherit-from is blanket-lazy → cycle never forms → `cpu` Bindings should NEVER be observed mid-construction.

But if v3 has a code path that materializes a partial Bindings (e.g., during scope construction for `cpuTypes`'s rec attrset where `family` happens to be set first), AND a Tag::Slot captures that Bindings pointer, a later force would see `{family}` only.

### H4 — A `with` lookup or scope-walk returns the wrong attrset

If `cpu` is accessed via `with` and the with-stack has a `{family}` attrset that's not the intended cpuTypes record, the wrong value is found.

But `tripleFromSystem` has formals `{cpu, vendor, kernel, abi, ...}@sys` — it gets `cpu` from destructuring, not from `with`.

### H5 — lambda-formals destructuring is binding `cpu` to a sub-attrset

If v3's formals destructuring for `{cpu, ...}@sys` reads `sys.cpu` correctly when `sys` is fully built but gets a stale read when `sys` is mid-build, the slot for `cpu` could end up pointing at a partial.

`sys = final.parsed`. `final.parsed = mkSystemFromSkeleton (mkSkeletonFromList (...))`. `mkSystemFromSkeleton` has a `parsed` LET-block:
```nix
parsed = { cpu = getCpu args.cpu; vendor = ...; kernel = ...; abi = ...; };
```
This is a NON-recursive attrset literal, but `vendor` and `abi` reference `parsed` (the surrounding let-binding), making the WHOLE attrset construction recursive in the runtime sense.

**This is the most likely root cause**: v3-direct materialises `mkSystemFromSkeleton`'s `parsed` rec-attrset incrementally; an early thunk that captures `parsed` (e.g., via `parsed.cpu` selector) observes a state where only one entry (`cpu`, but with a partial inner value) has been set.

Combined with H3: the inner `cpu = getCpu args.cpu = cpuTypes.${args.cpu}` flows through `setTypes`'s mapAttrs. If a Tag::Slot captures the mid-construction state of one of those values, we'd see `{family}`.

## Critical pieces of evidence still needed

To confirm H5 / H3:

1. **Pointer identity** of the failing Bindings — same across multiple force events? Or freshly allocated? (Add print of `b` pointer + the failing-thunk pointer to the diagnostic.)

2. **Origin position** of the failing Bindings allocation — where did OP_ATTRS_INIT / OP_ATTRS_REC_INIT allocate it? (Add a side-table mapping Bindings* → source position at OP_ATTRS_INIT time.)

3. **`shapeCell` vs cell pointer** on the surrounding thunk — is the thunk's `cell` or `shapeCell` being used or null? (Print thunk->cell / thunk->shapeCell at the OP_RETURN-on-cell write site.)

4. **The actual `cpu` slot value** before `cpu.name` selector — print all locals at fr[7] entry. The `cpu` slot tells us whether the destructuring got the wrong value or the right value with the wrong content.

## Plan

### Phase A — Confirm root cause (1–2 days)

A1. **Bindings origin side-table** (~50 LOC in `alloc.hh`/`vm.cc`): at every `OP_ATTRS_INIT`/`OP_ATTRS_REC_INIT` (and bridge-side `treeWalkerToV3` materialization), record `(Bindings* → posHandle)` so the diagnostic can answer "where was THIS Bindings allocated?"

A2. **Extended `V3_DBG_STRCONCAT_ATTRS`**: dump
   - Failing Bindings pointer
   - Origin position from A1's side-table
   - The current frame's locals (for fr[7], that's `cpu`, `vendor`, `kernel`, `abi`, etc.)
   - Whether any thunk on the frame stack has `cell` / `shapeCell` pointing at the failing Bindings

A3. **Add side-by-side eval-state comparison harness** (~100 LOC, standalone): run both TW and v3-direct on the same expression in the same process; at each `OP_FORCE`/`callClosure` step, compare the resulting value's tag + (for attrs) size + key-set. First divergence point is the smoking gun.

A4. Run A2/A3 on `(import <NP> {}).system`. **Outcome:** confirmed origin file:line of the partial `{family}`, plus the precise step where v3 and TW diverge.

### Phase B — Fix the immediate divergence (depends on what A confirms)

The fix branches on the root cause class. Most likely, ONE of:

- **B-i (H3 confirmed):** A Bindings pointer is escaping mid-construction. Either via Tag::Slot leak or via `mapAttrs`/`setTypes` interaction. Fix at the producer site: either the `mapAttrs` primop must publish ONLY the final Bindings (not an intermediate), or the consumer must read through the cell instead of the raw pointer. **Wire `Thunk::shapeCell` default-on** to enforce the correctness invariant — its semantics are exactly "read final WHNF, not mid-state".

- **B-ii (H5 confirmed):** Lambda-formals destructuring captures wrong slot. Fix in `lower.cc`'s formals handling — likely a Slot is built against the wrong scope index, OR the `parsed` let-block's runtime layout differs between TW and v3 (TW uses Env+Displ; v3 uses Bindings+Slot — the migration may have a subtle off-by-one on the `parsed.cpu` Slot for nested let-rec).

- **B-iii (orthogonal cause discovered in A):** Address as case demands.

In all three, the fix is structural — it MUST be tested against a regression that pins this exact divergence (the `{family}` shape, the eval-order at `lib.systems.equals`). Add the regression test to `run-thunk-all-regression-tests.sh`.

### Phase C — Catch this class of bug systemically (1 week)

The HONEST_ASSESSMENT explicitly flags this as the recurring failure mode: "STR_CONCAT today, callPackage yesterday, libsForQt5 the week before". The fix-and-move pattern hasn't been working.

C1. **Cross-evaluator differential harness, default-on in CI** (~200 LOC):
   - For a curated set of nixpkgs paths (top-level pkgs.system, pkgs.hello.name, pkgs.lib.systems.elaborate, lib.evalModules 100 options), run TW and v3-direct in the SAME process and compare:
     - Final value (deep-equal, modulo store paths)
     - First-divergence point in eval order (if mismatch)
   - Fail the build if v3 produces a different value/error than TW.

C2. **Per-source-line force-trace diff**: emit `(file:line, op, resulting-tag)` triples in both evaluators; diff on the canonical set. First diverging line is the cause. (~100 LOC, can be limited to when an env-var is set.)

C3. **Make `eval-fail` tests actually diff `.err.exp`** (per HONEST_ASSESSMENT.md). The current "an error was raised" regex hides every silent-correctness gap including C1–C7 in this morning's audit. ~30 LOC change in `run-fail-tests.sh`.

C4. **Promote the diagnostic suite**: every `V3_DBG_*` env var that surfaces a divergence becomes part of a documented post-mortem template. Build a `lode/DIVERGENCE_TRIAGE.md` that lists the typical observation points (frame-chain dump, Bindings-origin side-table, slot-vs-thunk discrimination) and the questions each answers.

### Phase D — Architectural decision on cell-update-everywhere

Per HONEST_ASSESSMENT: "either finish cell-update-everywhere or formally accept THUNK_ALL as the lazy strategy and delete the `s_cellEverywhere` gate." The `{family}` bug is exactly the shape `Thunk::shapeCell` was built for.

D1. After Phase B confirms the producer/consumer mismatch, decide:
- **D-a:** Flip `NIX_V3_CELL_EVERYWHERE` default-on. Validate against regression suite + the new C1 harness. **Cost:** unknown — `CELL_UPDATE_EVERYWHERE_2026-05-12.md` flagged conceptual walls that may resurface.
- **D-b:** Keep cell-update opt-in. Delete the gate, accept THUNK_ALL as THE laziness mechanism. **Cost:** lose the STG-correct story; correctness depends on lowering blanket-thunkifying enough patterns to never observe mid-state.

The data from A/B + C1 differential harness is the input to this decision.

## Risk inventory

| Risk | Severity | Mitigation |
|---|---|---|
| Phase A diagnostics add hot-path cost | low | gate behind env-var, off by default |
| Phase B fix exposes another divergence | **high** (the HONEST_ASSESSMENT pattern) | Phase C is the structural answer — without C, B-i/B-ii are just the next-error-floor move |
| Phase D-a flips a gate that hits Phase 2 walls | medium | gated, opt-back-able, validated against C1 first |
| Phase D-b loses cell-update-everywhere infrastructure | low if validated | keep the code in a branch; document the decision |

## What this RCA does NOT claim

- **It does not say what `cpu` is** at the failing site. That's what Phase A1/A2 will tell us.
- **It does not say whether shapeCell would prevent this** without testing. Phase D's decision depends on Phase A's data.
- **It does not propose ad-hoc fixes for the immediate symptom** (like "thunkify cpu in formals destructuring"). The pattern of fix-the-symptom-find-the-next-one is exactly what HONEST_ASSESSMENT flagged.

---

**RESOLVED 2026-05-18** (A-series closed). The {family} STR_CONCAT divergence was rooted in fakeClo closure-pool aliasing (A5, commit `1708d31bd`). The downstream C-stack overflow was resolved via Phase 1.2 iterative `forceValue` conversions (A8 series). Phase 1 exit criterion (hello.name evaluates without C-stack overflow) MET 2026-05-18 via Option 4 hybrid (commit `7adc7e61f` + `ecc99fd07`). See `OPTION_4_COMPLETE_2026-05-18.md`.

Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.
SPDX-License-Identifier: Apache-2.0
