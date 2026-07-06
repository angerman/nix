# Phase-S positive repro (NONMOVING_INLINE_THUNK_PLAN_2026-07-06 §5.3 +).
#
# A let-heavy expression that forces MANY tenured thunks + list cells across
# ≥2 gen-major safepoints, exercising the non-moving line-region reclaim
# (compile flag NIX_V3_NONMOVING_TENURED): after each safepoint mark,
# rebuildFreeSpansFromLineMarks recomputes the free-line spans and subsequent
# allocations bump into those reclaimed spans of the SAME (non-moving) blocks.
#
# Crossing the threshold ≥2×: the driver runs with NIX_V3_MAJOR_GC_THRESHOLD_MB=16
# (the clamp floor) + NIX_V3_MAJOR_GC_GROWTH=1.5.  This retains a large `big`
# list spine (~hundreds of MB of tenured list + thunk cells), so the arena's
# cumulative bytesAllocated crosses the (growth-raised) threshold repeatedly and
# the always-on gen-major safepoint fires at least twice — VERIFIED 2 fires
# (arena=16MB then arena=160MB) via NIX_V3_MIDEVAL_TRACE at authoring time.  Each
# per-element `mk` also builds + folds a throwaway `junk` list, so every safepoint
# has real dead cells for the non-moving span reclaim to reclaim in place.
#
# Determinism: pure arithmetic + list structure only — no impurity, no store,
# no <nixpkgs>.  The result is a single integer (720009400000 at n=400000),
# byte-identical whether the flag is ON or OFF (the reclaim must not change the
# answer).  The driver asserts ON == OFF rather than hard-coding the value.
#
# Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
#   Input Output Group.  SPDX-License-Identifier: Apache-2.0

let
  # Retained-spine element count.  Sized so the retained `big` list (n list
  # cells + n forced element thunks) forces multiple 16 MB block refills →
  # bytesAllocated crosses the low GC threshold ≥2× → ≥2 gen-major safepoints.
  n = 400000;

  # Each element: a small forced sum over a FRESH throwaway list (`junk` = dead
  # churn the safepoint sweep reclaims) plus the index (keeps the element live +
  # distinct so it is not folded to a constant).
  mk = i:
    let junk = builtins.genList (j: i + j) 8;
    in (builtins.foldl' (a: b: a + b) 0 junk) + i;

  # The retained spine.  genList's callback allocates n tenured elements; the
  # list itself is held live until the final fold, so the arena genuinely grows.
  big = builtins.genList mk n;

  # Fold the whole retained spine into one integer (forces every element).
  total = builtins.foldl' (a: b: a + b) 0 big;
in
  builtins.seq (builtins.length big) total
