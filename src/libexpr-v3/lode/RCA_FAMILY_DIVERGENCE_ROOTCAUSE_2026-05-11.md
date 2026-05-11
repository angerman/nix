# RCA Phase A5: Root cause — closure pool tail-call corruption

**Date:** 2026-05-11 (latest)
**Status:** Root cause identified; fix is partial (more paths to audit)

---

## The bug in one paragraph

The v3 fakeClo pool (Phase 4) recycles a force-frame's `closure` field at
OP_RETURN/CFF_THUNK_RETURN. But when OP_TAIL_CALL fires inside a CFF_THUNK_RETURN
frame, it overwrites `cur.closure` with the callee's REAL closure while
keeping the CFF_THUNK_RETURN flag. OP_RETURN then recycles a REAL closure
(not a fakeClo) into the pool. A later OP_FORCE pops that real-closure
pointer and overwrites its `desc` — silently mutating any cell-stored
`Tag::Closure` that still references that address.

## Concrete failure on `(import <nixpkgs> {}).hello.name`

Trace (cell tracer + recycle/pop diagnostics + frame dump):

1. **Allocation.** Inside the darwinArch thunk's body (parse.nix:953:3,
   codeOff=826), `OP_MAKE_CLOSURE operand=164` allocates the cpu lambda
   closure C at address 0xfa8852250, with `desc = cu->lambdas[164]`
   (codeOff=2863, "cpu" lambda at parse.nix:953:16).

2. **Cell write.** The darwinArch thunk's `OP_RETURN/CFF_THUNK_RETURN`
   handler writes C to the cell (slot in the parse.nix lib bindings):
   ```
   CELL WRITE storage=0xfa8820a28 thunk=0xfa8820d20 value-tag=9
              closure-ptr=0xfa8852250 desc=...c7c0 codeOff=2863
   ```

3. **Tail-call corruption.** Later, a CFF_THUNK_RETURN frame whose closure
   is C (0xfa8852250 — same pointer!) returns. The diagnostic shows:
   ```
   RECYCLE-CALLSITE: fClosure=0xfa8852250 desc=...c7c0 codeOff=2863
                     nUp=0 fThunk=0xfa88428b0 fFlags=0x1 retVal-tag=5
     frame stack post-pop:
       [N]   name=n  codeOff=4577
       [N-1] name=a  codeOff=4550
       [N-2] name=b  codeOff=4597
       [N-3] name=crossSystem  codeOff=312  flags=0x1  thunk=0xfa8801ba0
       ...
   ```
   The frame's `flags = 0x1` (CFF_THUNK_RETURN), `thunk = 0xfa88428b0` (a
   thunk, not nullptr), `closure = 0xfa8852250` (the cpu lambda real
   closure). Some OP_TAIL_CALL replaced the original fakeClo with C.

4. **Pool poisoning.** `recycleFakeClo(0xfa8852250)` pushes C into the
   fakeClo pool.

5. **Later force.** A subsequent OP_FORCE for SOME thunk T2 calls
   `allocFakeClo(0)`. The pool returns 0xfa8852250. Then
   `fakeClo->desc = T2's desc` — overwriting C's desc with whatever T2's
   body location is. We've seen the new desc resolve to codeOff=1265,
   2120, 1111, 1996, 861, 662, 2073, 2075, 2077, 2079, 2081, ... — and
   sometimes codeOff=1927 (the `{family = "wasm";}` thunk in
   inspect.nix:198:13).

6. **Cell read returns wrong closure.** When the cpuName thunk later
   reads the darwinArch slot, it still holds Tag::Closure ptr=0xfa8852250
   — but C's desc is now codeOff=1927. SLOT_REF darwinArch reports:
   ```
   slot-tag=9 chase-tag=9
     closure-body=<thunk>@inspect.nix:198:13 codeOff=1927 nUp=0
   ```
   instead of the expected codeOff=2863.

7. **Call dispatches into wrong body.** cpuName invokes darwinArch with
   cpu. darwinArch is now bound to the `{family = "wasm";}` closure. The
   closure's body just builds the attrs literal and returns it. cpuName's
   `OP_RETURN` returns `{family}` instead of "x86_64" / "arm64".

8. **Final symptom.** `${cpuName}-${vendor.name}-...` triggers
   OP_STR_CONCAT on the attrset; v3 raises
   `STR_CONCAT: cannot coerce type to string (tag=7)`.

## Why the cell-ownership tracker missed it

The I-CELL-1 invariant only catches **two thunks claiming the same cell
pointer**. Here, the cell at 0xfa8820a28 has a single owner (the
darwinArch thunk). The corruption is at a DIFFERENT level: the cell's
Value (a Tag::Closure pointer) stays the same — what mutates is the
*pointed-to Closure object's `desc` field*, via the closure-pool's
internal aliasing.

This is a closure-aliasing bug, not a cell-aliasing bug. The two cell
invariants (single-owner, write-once) hold; STG correctness for the cell
mechanism is preserved. The bug is in the orthogonal fakeClo pool
mechanism (Phase 4 perf optimization).

## Why tail-call into a thunk-return frame is the trigger

`cur.closure` is the FORCE FRAME's fakeClo at OP_FORCE time. The body
of the thunk has a tail call. OP_TAIL_CALL (vm.cc:3388 before fix):

```cpp
cur.cu = tcCalleeCu;
cur.closure = tcCallee;       // <-- replaces fakeClo with real closure
cur.ip = tcDesc->codeOffset;
```

The original fakeClo is lost (no other reference). The flag stays
CFF_THUNK_RETURN. When the body eventually returns, OP_RETURN reads
`fClosure = frRef.closure` — which is now `tcCallee` (a real closure).
`recycleFakeClo(fClosure)` pools the real closure.

## Why the cpu lambda gets aliased specifically

Sympathetic resonance:
- C is allocated in the body of darwinArch (codeOff=826), via
  `allocClosure(nUp=0)`. Address X.
- darwinArch's thunk body returns C. *cell = C.
- Some OTHER place (call site, tail-call, or v3_hook path) eventually
  has a CFF_THUNK_RETURN frame whose closure becomes X (whether by tail
  call to cpu lambda or some other indirect path).
- That frame OP_RETURNs. recycleFakeClo(X) is called.
- X is in pool.
- Future allocFakeClo for any 0-upvalue thunk pops X.

The cpu lambda's body codeOffset=2863 ends up as `cur.closure` for a
CFF_THUNK_RETURN frame because **SOMETHING tail-calls the cpu lambda
from a thunk-return-flagged frame**. The trace shows the call chain
involves frames at parse.nix lambdas (n, a, b at codeOff=4577, 4550,
4597 — likely `kernelName` and similar string-building helpers) and
`crossSystem` (parse.nix lib/systems/default.nix:312).

## Partial fix (landed)

`vm.hh`: new flag `CFF_FAKECLO_TAINTED = 1 << 3`.

`vm.cc` OP_TAIL_CALL (line ~3388): when the current frame is
CFF_THUNK_RETURN and not yet CFF_FAKECLO_TAINTED:
1. Recycle the ORIGINAL fakeClo (current cur.closure) eagerly.
2. Set CFF_FAKECLO_TAINTED on the frame.

`vm.cc` OP_RETURN (line ~3920): skip `recycleFakeClo(fClosure)` when
the frame had CFF_FAKECLO_TAINTED set — `fClosure` is now a real
closure, not a fakeClo.

This fixes the OP_TAIL_CALL → recycle-real-closure path.

## Status (NOT yet fixed)

`(import <nixpkgs> {}).hello.name` still segfaults (EXIT=139, same as
`NIX_V3_NO_CLOSURE_POOL=1`). The RECYCLE-CALLSITE diagnostic still
fires 9 times with fFlags=0x1 (NOT 0x9 — meaning CFF_FAKECLO_TAINTED
was never set on those frames).

That means there's ANOTHER path that puts a real closure as a
CFF_THUNK_RETURN frame's closure — bypassing OP_TAIL_CALL. Candidates:
- `v3_hook.cc` frame setup that reuses cur.closure.
- forceValue's runFunctionWithArg / runOnExistingVm paths.
- Some primop bridge that puts a real Value(Closure) into a force frame.

The next investigative step is to instrument all `frRef.closure =` /
`vm.frames.back().closure =` sites and detect when a non-fakeClo
closure lands in a CFF_THUNK_RETURN frame.

Or: the simpler workaround is to mark every fakeClo with a sentinel
field (e.g., `closure->nUpvalues = 0xFFFE`) and detect at recycle time
that the closure is the expected fakeClo. Real closures created by
OP_MAKE_CLOSURE always have nUpvalues equal to the lambda's declared
nUpvalues count — they should never have the sentinel.

## Long-term proper fix

The fakeClo pool optimization is correct in spirit (avoid Boehm
allocations per force) but the recycle protocol is ill-defined: a
"fakeClo" pointer becomes indistinguishable from a "real closure"
pointer once the body has potentially captured it (via OP_MAKE_CLOSURE
sharing, tail-call replacement, etc.). The right fix is one of:

1. **Don't recycle on tail-call paths** (already attempted; not
   complete). Audit ALL paths where cur.closure can be replaced.

2. **Tag fakeClo at allocation.** `allocFakeClo` stamps a sentinel
   (e.g., a magic value in an unused Closure field). `recycleFakeClo`
   ONLY recycles when the sentinel matches. Costs one extra word at
   allocation, one cmp on recycle.

3. **Retire the pool.** Phase 4's per-force allocation savings are
   real (hundreds of millions of allocs on full-nixpkgs THUNK_ALL),
   but correctness has to win. We can revisit pooling with a more
   principled scheme (e.g., the per-frame fakeClo lives on the
   value-stack instead of in the GC heap, or we use a thread-local
   single-element arena that resets at each force).

## Permanent diagnostic infrastructure added

These stay in tree (off by default, env-var gated):

- `V3_DBG_SELECT_AT_CODEOFF=<co>`: trace OP_GET_UPVALUE,
  OP_REC_BINDING_SLOT_REF, OP_ATTRS_SELECT inside any frame whose
  thunk/closure desc has the matching codeOffset.
- `V3_DBG_RETURN_AT_CODEOFF=<co>`: trace OP_RETURN inside matching
  frames, dumping retVal info + body disasm.
- `V3_DBG_MAKE_CLO_AT_CODEOFF=<co>`: trace OP_MAKE_CLOSURE results
  whose desc has matching codeOffset, with allocated pointer.
- `V3_DBG_RECYCLE_FAKECLO=1`: log every recycleFakeClo with closure
  pointer + previous desc codeOff.
- `V3_DBG_POP_FAKECLO=1`: log every tryPopFakeClo result with
  previous desc.
- `V3_DBG_RECYCLE_OF_CODEOFF=<co>` / `V3_DBG_RECYCLE_OF_PTR=<ptr>`:
  recycle-callsite dump with frame stack post-pop.
- `NIX_V3_DBG_CELL_TRACE=1` (extended): logs closure-ptr + desc
  codeOff in cell writes that store a Closure.

These together pin down "which real closure address gets recycled, and
by which call chain". Future regressions in this class fire with
visible diagnostics gated by these env vars.

---

Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.
SPDX-License-Identifier: Apache-2.0
