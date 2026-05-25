# RCA Deeper Findings: cpuName thunk produces `{family}` without going through SELECT
## (continued from RCA_FAMILY_DIVERGENCE_FINDINGS_2026-05-11.md)

**Date:** 2026-05-11 (late evening)
**Status:** Phase A3 diagnostic-driven deepening of root cause

---

## What Phase A3 added

The phase-A diagnostic was extended with:

- **`V3_DBG_REC_SET_NAME=<name>`** — trace every `OP_ATTRS_REC_SET` writing to a slot whose name matches.  Dumps value tag, source position, thunk upvalue shapes, and (when the value is a Suspended thunk) the body's disassembly.
- **Symbol annotation for `OP_REC_BINDING_SLOT_REF`** — `disasm.cc` now shows the symbol name for SLOT_REF operands (e.g. `operand=712 (cpu)`).  Pre-fix, SLOT_REF operands were opaque numbers.
- **`V3_DBG_SELECT_RESULT=<key>`** — trace every `OP_ATTRS_SELECT` whose result is a size-1 attrs with the given key.  Dumps the SOURCE attrset's full keyset + origin so we can identify which intermediate has the malformed `.name = <key>` entry.
- **`V3_DBG_SELECT_PATTERN=<key>`** — trace every `OP_ATTRS_SELECT` where the source is a size-1 attrs matching the key.

These let us reconstruct the failing thunk's evaluation step by step.

## What we learned

The failing `cpuName` slot in `tripleFromSystem`'s let-block (parse.nix:978) holds a Suspended thunk with bytecode:

```
[2148] OP_GET_UPVALUE         operand=1                    ; push sys
[2149] OP_REC_BINDING_SLOT_REF operand=209 (kernel)
[2150] OP_ATTRS_SELECT        operand=945 (families)
[2152] OP_ATTRS_HAS           operand=753 (darwin)
[2153] OP_BRANCH_FALSE        operand=2164                 ; FALSE → 2164
[2154-2163] TRUE branch                                    ; darwinArch cpu
[2164] OP_GET_UPVALUE         operand=1                    ; push sys
[2165] OP_REC_BINDING_SLOT_REF operand=712 (cpu)
[2166] OP_ATTRS_SELECT        operand=674 (name)
[2168] OP_RETURN
```

This is the correct lowering of:
```nix
cpuName = if kernel.families ? darwin then darwinArch cpu else cpu.name;
```

The thunk's upvalues (captured at let-block init):
- `up[0]`: Tag::Slot → Tag::Closure  (the enclosing rec scope for `darwinArch`)
- `up[1]`: Tag::Attrs `{kernel,cpu,vendor,abi}` size=4 — **the destructured `sys` formal**

So when the thunk runs:
1. `kernel = sys.kernel` (via REC_BINDING_SLOT_REF on the sys attrs)
2. `kernel.families ? darwin` — checked; for Linux this is false, so branch to 2164
3. `cpu = sys.cpu` (REC_BINDING_SLOT_REF 712)
4. `cpu.name` (ATTRS_SELECT 674)
5. return

## The contradiction

When the cpuName thunk is forced (during STR_CONCAT's force on parts[0]), it returns a size=1 attrs `{family}` — the literal from `inspect.nix:199` (`isWasm.cpu`).

**But:** `V3_DBG_SELECT_RESULT=family` (which traces every `OP_ATTRS_SELECT` returning a size-1 `{family}` attrs) catches **zero** such SELECTs.  Even with `V3_DBG_NO_IC=1` to disable the inline-cache fast path, zero SELECTs return `{family}`.

So `{family}` is **NOT** the result of `cpu.name` via OP_ATTRS_SELECT.  Yet it is the cpuName thunk's return value.

## What this means

`{family}` must arrive at the cpuName thunk's return value via a path that does NOT execute OP_ATTRS_SELECT.  Three possibilities:

### P1 — The thunk's body never executes; the cpuName slot is read directly

If something between SET and FORCE OVERWROTE the slot with `{family}` directly, then forcing the slot returns `{family}` without running the body.  But our REC_SET trace only shows Suspended-thunk writes to `cpuName`, no direct attrs writes.  **Refuted (probably).**

### P2 — The TRUE branch fires (Darwin), and darwinArch returns `{family}`

`kernel.families ? darwin` resolves to TRUE in v3's misexecution (it should be FALSE for Linux).  The TRUE branch does `darwinArch cpu`, which is `if cpu.name == "aarch64" then "arm64" else cpu.name`.  If `cpu.name` is `{family}` here too, we recurse.  But `cpu.name` would go through OP_ATTRS_SELECT, which our diagnostic should have caught.  **Refuted (probably).**

### P3 — The thunk runs but writes `{family}` to its `evaluated` slot/cell through a non-SELECT path

The OP_RETURN handler writes the body's retVal to `t->evaluated` AND optionally to `t->cell`.  If the thunk's cell was SET by an unrelated path to point at `{family}` storage, the cell write doesn't enter the thunk body.

The OP_ATTRS_REC_SET handler does:
```cpp
v.payload.thunk->cell = &recAttrs.payload.bindings->entries[i].value;
```

This sets the thunk's cell to point at the let-block entry's storage.  The cell-write-back at OP_RETURN then copies the thunk's `evaluated` to `*cell`.

**But there's a converse path:** if some OTHER thunk's body writes to its own cell, and that cell happens to be `cpuName`'s slot, the cpuName slot becomes the OTHER thunk's value.  This requires cell-aliasing: two thunks sharing the same cell pointer.

**P3 is the most likely candidate.**  The mechanism would require the inspect.nix isWasm.cpu thunk's `cell` to be aliased to the tripleFromSystem cpuName slot — or some intermediate thunk in the chain.

## Why this is structurally STG-relevant

STG semantics:
- Each thunk has ONE update slot (its own `evaluated` cell)
- A consumer that forces the thunk sees the `evaluated` cell after update
- Different thunks have different cells; their cells don't alias

If v3's cell mechanism allows two thunks' cells to alias the same storage (e.g., via Tag::Slot capturing a heap pointer that's later reused by REC_SET cell wiring), we VIOLATE STG's no-aliasing invariant.  The fix family is then about CELL ALIASING SAFETY, not about REC_INIT_TAIL semantics (the F1/F2/F3 from the original RCA).

## Revised fix candidates (replacing F1/F2/F3)

### G1 — Audit cell aliasing in OP_ATTRS_REC_SET

When `OP_ATTRS_REC_SET` sets `thunk->cell = &bindings->entries[i].value`, ensure:
- The thunk has no PRIOR cell (already-checked: `thunk->cell == nullptr`)
- No OTHER thunk's cell points at the same storage

The second condition is currently NOT enforced.  If two REC_SETs in different attrsets point their thunks' cells at storage that happens to alias (e.g., the same Bindings via cross-thunk Slot capture), updates collide.

### G2 — Audit Tag::Slot capture at OP_MAKE_THUNK

When a thunk captures upvalues, any Tag::Slot upvalues point at heap storage.  If that storage is ALSO the target of another thunk's cell, forcing the captured Slot reads the OTHER thunk's evaluated value — not the original.

Specifically: if `cpuName` thunk's upvalue `up[1]` is `sys` (Tag::Attrs ptr) and `sys.cpu` is a Tag::Thunk whose cell points at `sys.entries[cpu].value`, then the cpu thunk's update writes to that storage.  When cpuName runs and dereferences sys.cpu, it reads the post-update value — but if some intermediate step ALSO writes there (a different thunk's cell pointing at the same slot, or a Tag::Slot from inspect.nix's processing), we get the wrong value.

### G3 — STG-correct: only ONE thunk owns a cell

In STG, each closure has exactly one update slot owned by it.  Sharing/aliasing is via Tag::Ind (indirection) — explicit, not implicit.

v3 should adopt this: when REC_SET wires a thunk's cell to a Bindings slot, that's an OWNERSHIP transfer.  No OTHER thunk should later wire its cell to the same storage.  Add an assertion to detect violations.

## Phase A3 next: confirm P3 via cell-aliasing instrumentation

Add diagnostics:
- At every `OP_ATTRS_REC_SET` cell wiring (`t->cell = &b->entries[i].value`), log the (thunk, cell) pair.
- At every OP_RETURN cell write (`*cell = retVal`), log the (thunk, cell, value) triple.
- After STR_CONCAT failure, check: was the failing slot's storage written multiple times?  By how many distinct thunks?

If multiple thunks wrote to the same slot, **G3** is the fix family.

## STG perspective on the fix

The user explicitly asked: "Always keep in mind that we want to somewhat stay close to STG."

STG's update mechanism (3.6 of SPJ's "STG paper"):
- A thunk's body computes a value V
- The body atomically replaces the thunk's closure with `Ind V` (indirection) or with V directly (eval-apply variant)
- Future forces see the updated closure

The key STG invariant: **each closure has one update site**.  Updates are not "publish to multiple slots" — they're "replace the closure header".

v3's CELL mechanism (Phase 1.5 `shapeCell` + the existing REC_SET cell-wiring) approximates this by allowing the closure's update to also write to an external storage cell.  This is NOT in STG — it's an optimization for cross-thunk visibility.  If the optimization allows multiple thunks to share a cell, the STG invariant is broken.

**The STG-correct fix is G3**: ensure each (cell-bearing) thunk has a unique cell, OR replace the cell mechanism with Tag::Ind (explicit indirection) in cases where v3 currently uses cell-aliasing.

---

**RESOLVED 2026-05-18** (rolled into the A-series resolution; see `RCA_FAMILY_DIVERGENCE_2026-05-11.md` and `OPTION_4_COMPLETE_2026-05-18.md`). The deeper-cause hypotheses about Bindings-origin tracking were superseded by A5's actual root cause (fakeClo closure-pool aliasing) and ultimately sidestepped by Phase 1's Option 4 hybrid architecture.

Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.
SPDX-License-Identifier: Apache-2.0
