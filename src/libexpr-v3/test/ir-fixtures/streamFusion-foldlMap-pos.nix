# RUN: NIX_V3_STREAM_FUSION=1 v3-eval --file %s --emit-ir | v3-check %s
#
# Phase C / opt_stream_fusion: `foldl' op nul (map f xs)` rewrites to
# the fused `__foldlMap` primop dispatched as an App-chain over
# LitPrimOp.  NOTE: stream fusion is DEFAULT-OFF since 2026-06-05 (it was
# measured a net regression — the C-built genList spine beats the bytecode
# loop; see opt_stream_fusion.cc registry).  This fixture opts in via
# NIX_V3_STREAM_FUSION=1 to exercise the retained MECHANISM.  Confirms:
#   1. `LitPrimOp "__foldlMap"` appears in the post-opt IR.
#   2. It's followed by exactly 4 Apps (op, init, f, xs).
#   3. The original `LitPrimOp "foldl'"` is NOT present (rewritten away).
#
# Note: opt_stream_fusion intentionally emits an App-chain shape (not a
# `PrimOpCall(__foldlMap, ...)`) so OP_LIT_PRIMOP's bytecode-closure
# redirect fires.  See commit bc346f9a5 + 1af56ff25 for the why.

builtins.foldl' (a: b: a + b) 0 (map (x: x * 2) [1 2 3 4 5])

# CHECK-LABEL: B1:
# CHECK: v{{[0-9]+}} = LitPrimOp "__foldlMap"
# CHECK: v{{[0-9]+}} = App
# CHECK: v{{[0-9]+}} = App
# CHECK: v{{[0-9]+}} = App
# CHECK: v{{[0-9]+}} = App
# CHECK-NOT: LitPrimOp "foldl'"
