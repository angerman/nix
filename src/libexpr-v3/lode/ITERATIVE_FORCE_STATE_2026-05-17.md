## Iterative-force status — 2026-05-17

**Hypothesis under test**: the audit's Phase 1.2 closure was sufficient
for hello.name. **FALSIFIED today**.

Crash repro:
```
NIX_V3_DIRECT_EVAL=1 NIX_V3_SKIP_INSTALLABLE_PREEVAL=1 \
  nix eval --impure --expr '(import <nixpkgs> {}).lib.version'
# → EXC_BAD_ACCESS (code=2) at dispatchLoop entry; SP guard hit
```

The binary already links with `-Wl,-stack_size,0x4000000` (64 MiB).
Crash happens at v3 frame depth 2818 (well under `kMaxCallDepth =
5000`). The C-stack — not vm.frames — is exhausted.

LLDB backtrace (~80 frames captured; cycle visible):
```
forceValue 9674  → dispatchLoop 2854 (OP_CALL_PRIMOP arg-force)
                 → callClosure 10127 → dispatchLoop 3026
                 → callClosure 10127 → forceValue 9165 (App-spine apply)
                 → dispatchLoop 6225 (OP_ATTRS_SELECT_IC)
                 → forceValue 9674
                 → primConcatLists 847 → forceValue 9674
                 → primDerivationStrictNative 5183 → primDerivationStrict 4865
                 → primDerivation 5564 → dispatchLoop 7870
                 ↺ (next stdenv input layer)
```

Each cycle of the stdenv-input recursion adds **~14 C-frames**. With
optimized build (~1-3 KB per frame on average), ~200 such cycles
exhaust the 64 MiB stack.

### What's iterative today

The audit's Phase 1.2 conversions DID eliminate:
- Bytecode-level OP_FORCE chase (op_force_slow + writeback + retry)
- Bytecode-level App-spine apply (identity-lambda fast path)
- OP_LIST_CONCAT / OP_ATTRS_UPDATE / OP_STR_CONCAT element forces
- primConcatMap / primPartition / primAll / primAny / primMap inline
  WHNF skip
- valueEqual recursive depth (falsified as a real-world risk)

A12b (2026-05-17) ALSO converted (via bytecode-closure primops):
- primFilter / primFoldl' / primMap / primAll / primAny / primConcatMap
  / primPartition / primGroupBy

Each of these no longer C-recurses via callClosure — the callback
chain dispatches inside the SAME dispatchLoop frame.

### What still C-recurses (this is the architectural gap)

Two call sites in vm.cc are NOT iterative when reached from outside
the bytecode dispatch:

1. **`forceValue` Suspended-thunk handler at vm.cc:9674.**
   `dispatchLoop(vm, exitDepth)` recursively. The bytecode equivalent
   (op_force_slow at vm.cc:5199) pushes the frame and `break`s — the
   outer dispatchLoop drives it. But forceValue is also called from
   PRIMOP C-BODIES (e.g. primConcatLists:847, primDerivationStrictNative
   :5183), which expect a synchronous return — they can't `break` back
   to an outer loop because there is no outer loop.

2. **`callClosure` at vm.cc:10127.** Same pattern: pushes the callee
   frame, calls `dispatchLoop` C-recursively. The bytecode equivalent
   (OP_CALL) pushes the frame and falls through to the same
   dispatchLoop's switch via `break`.

Both sites are FUNDAMENTAL: while v3 owns primops in C, every primop
that calls callClosure or forceValue from its C-body produces a fresh
dispatchLoop on the C-stack.

### Why this can't be fixed by "convert N more primops"

The dominant recursion in the hello.name backtrace is **primDerivation
→ primDerivationStrict → primDerivationStrictNative**. This primop:
- Walks the derivation's attr set
- Forces each attr (line 5183) — which may be ANOTHER derivation's
  outPath, triggering another primDerivation call
- Builds the .drv on the v3 side via libnixstore

primDerivation is ~700 lines of C++ that interacts with libnixstore,
NixStringContext, drv parser, hashing, etc. Converting it to bytecode
is impractical (each libnixstore call would need an FFI primop, which
re-introduces C-recursion via the FFI primop's C-body).

### The actual fix space

Three structural options remain (none small):

**A. Fiber-driven primops.** The primop body runs on a fiber; when
it needs to force a value, it yields to the bytecode driver, which
runs the force on the main fiber, then resumes the primop fiber with
the forced value. Rejected previously due to macOS arm64 ucontext_t
issues (`bridge_yield.cc` was abandoned).

**B. Worklist-style primops.** Restructure primDerivation /
primDerivationStrict / primDerivationStrictNative as a state machine
that returns "needs force" tags and re-enters with the forced value.
Requires fundamentally rewriting these primops with explicit
suspension points.

**C. Accept the C-stack cost; defer hello.name.** Document the
architectural limit, focus on workloads that don't hit deep stdenv
recursion (lang tests, micro-benchmarks, simple flake outputs).

The action plan's Phase 1 kill criterion was:
> "If after 10 days we still C-stack-overflow on hello.name, the
> iterative-conversion approach has missed a recursive site we cannot
> find. Pause and reconsider whether the recursion lives in C++
> (forceValue) or in the bytecode dispatch (a misdesigned opcode
> chain)."

We've reached that decision point. The recursion lives in **C++
(forceValue called from primop C-bodies)**, not in the bytecode
dispatch. The audit's Phase 1.2 work was correct as far as it went;
it cannot close the gap because the remaining recursion is in
primop C-bodies that the iterative-force writeback pattern
fundamentally cannot reach.

### Rule 0: what this doc kills

H1 "v3 can complete hello.name with the audited Phase 1.2 iterative-
force conversions plus the A12b bytecode primops." **FALSIFIED** —
the bt shows the recursion is in primDerivation*'s C-body, which is
not reachable by either mechanism.

H2 "primDerivation's recursion is bounded by realistic stdenv depth
and can be tolerated under the 64 MiB darwin link-time stack."
**FALSIFIED** — the recursion produces ~200 iteration-deep nesting
before crash, but each iteration consumes ~14 C-frames * ~3 KB =
~42 KB, totalling ~8 MB. Combined with the prior 56 MiB consumption
from the call chain leading here, the 64 MiB ceiling is hit.

### Next action

Open the decision among A/B/C as a strategic question to the user.
This commit does NOT continue iterative-force conversions: the
remaining sites (line 9674, 10127) are architecturally reachable
only by fibers or worklist restructuring, neither of which is a
single-day fix.

The genericClosure / sort / groupBy / foldl' force-on-return fix
(this PR, commit b0a0ff2e1) lands separately as it kills a real
A12b regression independent of the iterative-force decision.

Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
Input Output Group. SPDX-License-Identifier: Apache-2.0
