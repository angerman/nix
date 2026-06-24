# S1.3 — Move-safepoint verification (RSS/Bartlett path) — 2026-06-24

Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output
Group. SPDX-License-Identifier: Apache-2.0.

S1.3 (#168) verifies the EXISTING mid-eval GC trigger is a valid MOVE safepoint for the
Bartlett collector (S0.3 decision), so S2.1 only needs to add the mover — not new
safepoint plumbing. No code change; conclusions from reading the trigger + walkers.

## Where the safepoint is

`dispatchLoop` (vm.cc) runs a mid-eval `runMajorMarkSweep` at a heap-pressure threshold
BETWEEN opcodes (vm.cc:4237). This is the safepoint. It currently runs NON-MOVING
("Frames are NOT forwarded ... no post-GC re-read" — vm.cc:4234-4236); the moving
gen-major path (frame-forward + dispatch-local re-read) exists but is gated to
`exitDepth==0`.

## Move-safety: the four conditions, all satisfied

1. **Value-stack consistent.** The trigger fires between opcodes, so the operand stack /
   frames / withStack are in a consistent state (no half-evaluated opcode).

2. **Precise roots enumerable at any depth.** `walkAllV3Roots` enumerates
   `activeVMStack()` → every active VMState's value-stack, frames, withStack, IC, and
   standalone roots, regardless of `exitDepth` (vm.cc:4279-4281). The mover updates these.

3. **Remembered set covers inter-generational roots the precise mark misses.** S0.2
   measured the `dirtyContainers` walk marking 61–181K cells the precise mark alone
   misses ("remembered set IS a needed root"). The mid-eval path already walks it
   (mark_sweep.cc:2089-2120). **S2.1 requirement:** the mover must RELOCATE+fixup via the
   remembered set, not merely mark it.

4. **Full C-stack + registers pinned.** `walkCStackConservative` (mark_sweep.cc:768)
   scans the ENTIRE machine stack from `outerSp` (deepest frame's SP, captured at the
   trigger) up to `stackHi = pthread_get_stackaddr_np` (thread base), AND spills
   callee-saved registers to a `jmp_buf` via `setjmp` so register-held pointers are
   scanned. So every cell referenced by a nested-dispatchLoop local (`closure*`) or a
   primop C-local (the S0.2 re-entrant roots) is found and pinned. The mid-eval path runs
   this (the S0.2 `conservativeOnly` > 0 measurement is only possible if it fires).
   Non-arena locals (`cu*`, `ip` into bytecode, `stackBase` index) never move, so they
   need no fixup.

## Conclusion

The existing mid-eval trigger is a VALID Bartlett move-safepoint. S1.3 adds no plumbing.
What S2.1 must do (carried forward): (a) make the collector MOVE precisely-rooted +
remembered-set cells with pointer fixup; (b) PIN conservatively-marked cells (pin wins
over move; line granularity); (c) the de-box for v8nan Value cells in the byte-scan
already exists under `g_midEvalGcEnabled` (mark_sweep.cc:510).

## Regression guard (load-bearing in S2.1, not now)

The move-safety INVARIANT to assert when S2.1 enables moving: `outerSp <= currentSP` at
the scan (the scan's low bound must cover the deepest live frame — if `outerSp` were
captured too high, deep frames' pins would be missed → UAF once moving). Pre-moving a
missed pin is harmless (non-moving mark only over-retains), so the assertion belongs in
S2.1 where it becomes fatal. The standing empirical guard is the auditor (`--brute`
1 MB-nursery + `V3_DBG_NURSERY_AUDIT`) which already catches missed roots; S1.4 runs it
to prove root completeness before S2.1 moves anything. The `conservativeOnly` line is the
standing measurement that the C-stack scan keeps firing.
