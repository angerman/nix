# RCA Phase A4: cell aliasing REFUTED, cpuName thunk body returns {family}
## (continued from RCA_FAMILY_DIVERGENCE_DEEPER_2026-05-11.md)

**Date:** 2026-05-11 (very late)
**Status:** Cell-aliasing hypothesis refuted; new hypothesis on the table

---

## Phase A4 deliverables

### Cell-ownership invariant tracker (load-bearing, permanent)

`lode/CELL_INVARIANTS.md` + `alloc.hh` infrastructure:
- 5 invariants formalized (I-CELL-1 through I-CELL-5)
- Side-table `cellOwner : Value* → Thunk*` records every cell setter
- Mismatch at setter or writer logs (or asserts under
  `NIX_V3_ASSERT_CELL_OWN=1`)
- All 3 setter sites + 3 writer sites wired
- Zero hot-path cost when off

### Cell-write trace (NIX_V3_DBG_CELL_TRACE=1)

- Logs every `*cell = retVal` with (storage, thunk, value-tag,
  bindings-origin)
- Lets us correlate the failing attrs pointer with the thunk that
  wrote it

### OP_RETURN tracer for size-1 attrs (V3_DBG_RETURN_KEY=family)

- Logs every OP_RETURN whose retVal is a size-1 attrs with the given
  key
- Shows the (name, pos, codeOff, thunk, flags) of the returning frame
- Cross-references against the failing attrs's cell-write

### OP_ATTRS_SELECT result trace (V3_DBG_SELECT_RESULT=family)

- Logs every SELECT (BOTH IC fast-path AND slow path) whose result is
  a size-1 attrs with the given key
- Shows source bindings + origin

## Key findings

### Cell aliasing is REFUTED

`NIX_V3_DBG_CELL_OWN=1` on the failing eval: **zero violations**.  No
two thunks share a cell pointer.  Hypothesis P3 is out.

### The cpuName thunk body DOES return {family}

`V3_DBG_RETURN_KEY=family` captured **22 OP_RETURNs producing {family}**
during the failing eval.  Among them:

```
v3 OP_RETURN with retVal={family} (size=1)
  from frame: name=cpuName pos=parse.nix:978:7 codeOff=2148
  thunk=0xec68b08b0 flags=0x1 ip=2168
```

So the cpuName thunk (codeOff=2148, parse.nix:978:7) does in fact
produce `{family}` as its body's result.  This thunk's pointer matches
the writer thunk in the failing cell write — confirming it's the same
thunk, not aliased.

### But the bytecode disasm shows `sys.cpu.name`

The bytecode at codeOff=2148:

```
[2148] OP_GET_UPVALUE         operand=1
[2149] OP_REC_BINDING_SLOT_REF operand=209 (kernel)
[2150] OP_ATTRS_SELECT        operand=945 (families)
[2152] OP_ATTRS_HAS           operand=753 (darwin)
[2153] OP_BRANCH_FALSE        operand=2164          ; FALSE → 2164
[2154-2163] TRUE branch  (TAIL_CALL darwinArch cpu)
[2164] OP_GET_UPVALUE         operand=1
[2165] OP_REC_BINDING_SLOT_REF operand=712 (cpu)
[2166] OP_ATTRS_SELECT        operand=674 (name)
[2168] OP_RETURN              operand=0
```

This is the correct lowering of
`if kernel.families ? darwin then darwinArch cpu else cpu.name`.

For x86_64-linux, the FALSE branch is taken: `sys.cpu.name`.  Result
should be `"x86_64"` (string).  But the OP_RETURN trace shows the body
returns `{family}` attrs.

### V3_DBG_SELECT_RESULT=family captures ZERO matching SELECTs

Even with the IC fast-path also instrumented, no OP_ATTRS_SELECT returns
a size-1 `{family}` attrs.

This is the contradiction: the body's last expression is
`OP_ATTRS_SELECT 674 (name)`, the SELECT *must* push something onto the
stack, OP_RETURN pops it and returns.  Yet:
- The SELECT's result is NOT a `{family}` attrs (per the V3_DBG_SELECT_RESULT)
- The OP_RETURN's retVal IS a `{family}` attrs (per V3_DBG_RETURN_KEY)

How can both be true?

## Hypothesis P4 (working theory)

**The SELECT pushes a Tag::Thunk whose `evaluated` slot already
contains `{family}`.**  OP_RETURN's chase-loop then chases through the
Evaluated thunk to reach the `{family}` attrs.

```cpp
while (retVal.isThunk() && retVal.payload.thunk->state == ThunkState::Evaluated)
    retVal = retVal.payload.thunk->evaluated;
```

So retVal IS Tag::Attrs `{family}` after chase, but BEFORE the chase
it was Tag::Thunk pointing at `{family}`.  My SELECT diagnostic only
checks the FINAL pushed value's tag (which is still Thunk before
chase), so it doesn't fire.

This means: **cpu.entries[name].value is a Tag::Thunk whose body
already produced `{family}`**.  cpu = cpuTypes.x86_64 — its `name`
entry should be the string `"x86_64"`, NOT a thunk that resolves to
`{family}`.

Either:
- Some thunk has its `cell` pointing at cpu.entries[name].value and
  writes a thunk-reference there (but cell-tracker shows zero
  violations)
- mapAttrs's lazy entries result in `cpu.entries[name].value` being a
  Tag::App that resolves to a thunk that produces `{family}`
- cpuTypes.x86_64 itself is wrong (some path produces wrong attrset
  shape for x86_64)

## Concrete next steps

### Phase A5: trace SELECT pushes for Tag::Thunk results

Extend V3_DBG_SELECT_RESULT to also fire when the pushed value is a
Tag::Thunk whose `evaluated` (if any) is the matching attrs.  Then we
can see exactly when `cpu.name` returns a chasable Thunk.

### Phase A6: trace cpuTypes.x86_64 force

What does cpuTypes.x86_64 actually evaluate to in the failing context?
Add a one-shot instrumentation at OP_FORCE that, when forcing a thunk
whose desc-name is "x86_64" or whose source position is inside
parse.nix's cpuTypes definition, dumps the resulting attrset shape.

### Phase A7: differential audit of mapAttrs in this context

If mapAttrs's App-pair entries somehow build `{family}` for x86_64,
that's the root cause.  Instrument primMapAttrs's App allocation to
log (key, fn, value) at construction.  Then on the failing eval, see
what the x86_64 App pair was constructed with.

## What we have so far

The diagnostic infrastructure is now substantial and reusable:
1. Bindings-origin side-table (A1) — every Bindings tagged with
   alloc site
2. STR_CONCAT-attrs diagnostic (A2) — frame chain + locals + origin
3. Cell-ownership tracker (A4a) — I-CELL-1 invariant enforced
4. Cell-write trace (A4) — every `*cell = v` logged with origin
5. OP_RETURN size-1-attrs trace (A4) — find producer
6. SELECT size-1-result trace (A4) — find lookup point

These persist in the codebase as `NIX_V3_DBG_*` env-var-gated
diagnostics.  Future investigations of this class start with this
infrastructure already in place.

The invariants in `lode/CELL_INVARIANTS.md` are documented and
enforced.  The codebase audit is complete; no other cell-write paths
exist.

---

**RESOLVED 2026-05-18** (A-series closed). A4's "cell aliasing REFUTED" hypothesis stood; A5 (commit `1708d31bd`) landed the real fix via fakeClo sentinel tag. Phase 1 architectural fix (Option 4 hybrid, commit `7adc7e61f`) made the underlying gate non-load-bearing for hello.name.

Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.
SPDX-License-Identifier: Apache-2.0
