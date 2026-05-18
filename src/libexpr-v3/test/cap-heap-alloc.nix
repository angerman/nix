# Phase 1.6 cap fixture — large allocation.
#
# Builds a list of 10K elements, each a 10K-element sublist of
# strings.  Concat-and-length forces every element, so the working
# set should peak well above 256 MB.  NIX_V3_MAX_HEAP=256M should
# trigger Boehm's OOM handler (best-effort; Boehm may exceed the
# cap to make progress per its docs).
#
# Note: Boehm's `GC_set_max_heap_size` is APPROXIMATE — the runtime
# may exceed the cap when the OS has free memory, in which case the
# OOM handler never fires.  For deterministic OOM testing, pair
# NIX_V3_MAX_HEAP with `setrlimit(RLIMIT_AS)` (not currently wired
# from `initLimits`; see ACTION_PLAN_2026-05-15.md Phase 1.6 kill
# criterion for the fallback).

let
  inner = builtins.genList (j: builtins.toString (j * 12345 + 6789)) 10000;
  outer = builtins.genList (i: inner) 10000;
in
  builtins.length (builtins.concatLists outer)
