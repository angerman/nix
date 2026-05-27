# V3 EVAL IS V3-NATIVE — TW IS FFI-LEAF ONLY

> **ARCHIVED-IN-PLACE 2026-05-27**: This doc is historical (pre-strategic-doc-set or one-off RCA). Preserved here because it is still referenced from one or more KEEP docs. See [`LODE_CLEANUP_REVIEW_2026-05-27.md`](LODE_CLEANUP_REVIEW_2026-05-27.md) for the archival rationale.

---


**Status: load-bearing architectural invariant.  Future fixes that
violate it should be rejected at review.**

## Rule

V3 owns evaluation semantics end-to-end.  Tree-walker (TW) is
permitted only at the **FFI boundary** — code that crosses into the
nix store, the file system, the derivation graph, or other
out-of-band state that v3 does not (and should not) reimplement.

Concrete examples of LEGITIMATE TW invocations:
- `import` primop reading and parsing source files
- `derivation` strict-attr realization
- `builtins.path` / `builtins.fetchurl` etc. resolving paths
- Anything that writes to the eval log / sets up addErrorContext
  via TW's positions table

Concrete examples of FORBIDDEN TW invocations:
- Forcing a v3 thunk via TW's `forceValue(*nix::Value*)` from the
  v3 dispatch loop ("borrow TW's mid-flight observation")
- Bridging a v3 cycle to TW so TW can throw a different exception
- Any path that re-enters TW's eval just to advance v3's evaluation

## Why

The whole inversion plan (#454 → #457 → #458) is to make v3 the
primary evaluator with TW as a sealed leaf.  Mixing TW into eval
re-creates the cross-VMState fresh-thunk-per-call problem from
`EVAL_ORDER_DIVERGENCE_2026-05-08.md`: every TW callback allocates
a new v3 Bridge thunk, the per-Thunk cycle detector loses identity
across re-entries, and we end up with the unbounded-thunk-allocation
patterns that broke nixpkgs evaluation in `f216c3bcb` and earlier.

The slot architecture (#458, #459-#465) was designed around this
constraint: heap-stable cells, OP_RETURN-time updates, Tag::Slot
captures.  TW-routing for Black-thunk observation defeats the
architecture.

## Practical implication for the v3-direct nixpkgs cycle

When v3-direct's `(import nixpkgs {}).hello.name` cycles, the fix
is V3-NATIVE.  Two angles, both v3-only:

- **Angle A**: match TW's eval-order (v3's lower / dispatch fires
  an OP_FORCE that TW's eval doesn't have).  Find the divergence,
  remove it.

- **Angle B**: extend v3's mid-flight observation in the slot
  mechanism so foreign Tag::Slot derefs see partial Bindings state
  (similar to the existing `rec { x = 1; y = self.x; }` path, but
  for `let prev = ...; in body` shapes).

Either is fine.  TW-routing is not.

## What previous attempts got wrong

`STG-13` (#509, pending — DO NOT IMPLEMENT AS DESCRIBED) was
sketched as "lazy v3-to-TW bridge for Tag::Slot/Tag::Thunk".  That
bridges to TW.  Forbidden by this memo.  The replacement is
Angle A or Angle B above.

The `V3_DIRECT_NIXPKGS_2026-05-09.md` memo's first draft proposed
the same thing.  It has been corrected.  Do not regress.

Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input
Output Group.
SPDX-License-Identifier: Apache-2.0
