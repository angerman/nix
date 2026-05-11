# RCA Findings (Phase A1/A2 complete): the `{family}` divergence
## (continued from RCA_FAMILY_DIVERGENCE_2026-05-11.md)

**Date:** 2026-05-11 (evening)
**Status:** Origin pinpointed; root cause class narrowed; fix not yet implemented

---

## What Phase A delivered

With `NIX_V3_DBG_BINDINGS_ORIGIN=1` + `V3_DBG_STRCONCAT=1`, the STR_CONCAT divergence on `builtins.isAttrs (import <NP> {})` produces:

```
v3 STR_CONCAT: attrs missing __toString/outPath (ptr=0xec28b0900 size=1): {family}
  bindings origin: source=OP_ATTRS_REC_INIT_TAIL
                   pos=lib/systems/inspect.nix:199:9
  top-frame locals (base=72, 8 shown):
    local[1]: tag=7 attrs ptr=0xec28b0620 size=4 {kernel,cpu,vendor,abi}
              origin=alloc@vm.cc:4645@?:0      # sys = parsed
    local[6]: tag=7 attrs ptr=0xec28b0790 size=3
              {optExecFormat,optAbi,cpuName}
              origin=alloc@vm.cc:4645@?:0      # the let-block
```

The failing operand (`parts[0] = cpuName`) is the Bindings allocated by **`OP_ATTRS_REC_INIT_TAIL` for `lib/systems/inspect.nix:199:9` — `isWasm.cpu = { family = "wasm"; }`**.

`{family="wasm";}` is a literal 1-attr value that nixpkgs uses to define the Wasm-platform predicate.  It has nothing semantically to do with `tripleFromSystem`'s `cpuName` (which should be the string `"x86_64"`).

**v3-direct ends up materialising `cpuName = isWasm.cpu` instead of `cpuName = cpu.name`.**

## What this rules out

| Hypothesis (from original RCA) | Status |
|---|---|
| H1: `cpu` is partial Bindings | **Refuted.** `local[1]` (sys) has all 4 fields; sys.cpu was not observed. |
| H2: `cpu` is the unmerged inner cpuTypes record | **Refuted.** The failing attrs is `{family}` from inspect.nix, not from parse.nix's cpuTypes. |
| H3: Mid-construction Bindings observation | **Partially.** The Bindings was correctly built (it's `isWasm.cpu`, a real 1-entry attrset).  It just SHOULDN'T be at `cpuName`. |
| H4: with-lookup wrong attrset | **Refuted.** No `with` in tripleFromSystem; `cpuName` comes from a let-block. |
| H5: formals destructuring wrong slot | **Refuted.** sys has the right keys; the local[1] Bindings shape matches expectations. |

**The actual root cause class is NEW: cross-attrset value contamination via the OP_ATTRS_REC_INIT_TAIL publish path.**  Either:
- (a) A specific OP_ATTRS_REC_SET fills `cpuName`'s slot with the wrong value, OR
- (b) `cpuName`'s slot pointer/cell aliases the slot of `isWasm.cpu` and reads it after `isWasm.cpu` was set.

## Why the publish path is the suspect

`OP_ATTRS_REC_INIT_TAIL` (vm.cc:4683) has a deliberate side-effect: it can publish the in-progress Bindings to outer thunk frames' `shapeCell`s for `with self;` / `with pkgs;` lookups.  Even with `NIX_V3_CELL_EVERYWHERE=0` (the default), this path **allocates** Bindings that are visible to outer scopes via the partial-Bindings publish that Phase 3 was supposed to retire.

Verified:
- `NIX_V3_CELL_EVERYWHERE=1` does NOT prevent the failure — the cell-update everywhere mechanism isn't load-bearing here.
- `NIX_V3_NO_INHERIT_FROM_THUNK_ALL=1` (Phase 3.2 opt-out) does NOT prevent the failure — pre-Phase-3 lowering reproduces it.
- The failure fires on EVERY shape that bootstraps nixpkgs (even `builtins.isAttrs (import <NP> {})`).

## Most likely concrete root cause

When `lib.systems.equals system localSystem` runs during `crossSystem` evaluation:

1. `equals = a: b: removeFunctions a == removeFunctions b`
2. `removeFunctions a = removeAttrs a (filter (n: builtins.isFunction a.${n}) (attrNames a))`
3. The `filter` walks **every** attr of `a = system` (elaborate's full result, which includes `predicates`, `parsed`, `kernel`, etc.).
4. For each attr `n`, `a.${n}` is selected and `isFunction` checked.  This forces the value of every attr.
5. **`system.predicates` is `mapAttrs (_: matchAnyAttrs) inspect.patterns`.**  Forcing this evaluates the patterns thunks.
6. `inspect.patterns.isWasm.cpu` is `{family="wasm";}` — its OP_ATTRS_REC_INIT_TAIL runs.
7. Simultaneously (because v3-direct interleaves call stacks via `equals` forcing the call to `tripleFromSystem`), `tripleFromSystem`'s let-block is being initialised.
8. **Some slot in `tripleFromSystem`'s let-block Bindings is incorrectly written by the inspect.nix thunk's OP_ATTRS_REC_INIT_TAIL.**

The smoking gun: the failing Bindings was allocated via `OP_ATTRS_REC_INIT_TAIL` (not `OP_ATTRS_INIT`), and OP_ATTRS_REC_INIT_TAIL is the only opcode that has a cross-thunk side-effect on the call stack.

## Proposed fix path

### Option F1 — Constrain OP_ATTRS_REC_INIT_TAIL publishing

The opcode comments document that it "registers the partial Bindings with EVERY thunk frame on the call stack".  Verify whether the inspect.nix `isWasm.cpu` Bindings is publishing into `tripleFromSystem`'s thunk frame's cell — and if so, restrict the publish to genuine `rec`-let-rec-thunk relationships, not any THUNK_RETURN frame on the stack.

Specifically, look at the publish target selection logic at the OP_ATTRS_REC_INIT_TAIL site and confirm it doesn't write into a frame that isn't structurally an ancestor of THIS attrset.

### Option F2 — Lower `{family="wasm";}` (non-rec literal) as OP_ATTRS_INIT, not REC_INIT_TAIL

The literal `{family="wasm";}` at `inspect.nix:199` is non-recursive — no entry references a sibling.  The `isFunctionReturn` mark from `markTailReturnAttrSets` is currently applied REGARDLESS of whether the AttrSet is recursive.  Tightening the mark so only recursive attrsets get OP_ATTRS_REC_INIT_TAIL (and non-rec tail-return attrsets get OP_ATTRS_INIT) would eliminate the publish path for the inspect.nix case.

### Option F3 — Per-thunk scope-fence on publish

Make the OP_ATTRS_REC_INIT_TAIL publish only target THIS thunk's own frame, not the caller chain.  Effectively, downgrade INIT_TAIL to INIT_REC in cases where the publish would cross a non-lexically-related thunk boundary.

## Phase A3 next: cross-evaluator differential harness

To rule out remaining alternatives and confirm F2/F3, we need the cross-evaluator differential harness from the original plan.  Specifically: run TW and v3-direct on `(import <NP> {}) ? system`, step both forward, compare what each evaluator returns from the `predicates` and `parsed` slot accesses.  First divergence point IS where the slot contamination occurs.

That harness is C++ code (~200 LOC) and a meaningful build effort — but it's the load-bearing piece per the HONEST_ASSESSMENT.  Without it, every v3-direct-specific bug requires the same forensics from scratch.

## What's committed in Phase A

- `fc5b95b1e` — Bindings-origin side-table + STR_CONCAT diagnostic extension.
  - `NIX_V3_DBG_BINDINGS_ORIGIN=1` records the origin (Nix posHandle when known,
    else C++ file:line) of every Bindings allocation, default-on for all
    allocBindings call sites via `__builtin_FILE`/`__builtin_LINE`.
  - Explicit semantic labels at hot sites (OP_ATTRS_INIT, OP_ATTRS_REC_INIT,
    OP_ATTRS_REC_INIT_TAIL, OP_ATTRS_INIT_DYN, primMapAttrs, treeWalkerToV3).
  - `V3_DBG_STRCONCAT=1` STR_CONCAT diagnostic now dumps the origin, frame
    chain, and top-frame locals (with their Bindings origins).

This infrastructure is reusable for ANY future divergence investigation —
including the `callPackage` / `texlive` / `libsForQt5` cycle of error
floors HONEST_ASSESSMENT.md flagged.

---

Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.
SPDX-License-Identifier: Apache-2.0
