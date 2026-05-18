# Phase 1.6 cap fixture — infinite tail recursion.
#
# Without NIX_V3_MAX_CPU_TIME / NIX_V3_MAX_WALL_TIME, evaluating this
# hangs in a hot loop until SIGKILL.  With either cap, the dispatch
# loop's periodic poll throws a typed exception within ~1 poll-
# interval (10000 opcodes ≈ 10ms) of the cap being exceeded.
#
# Used by:
#   - run-resource-limits-tests.sh — asserts the cap fires + the
#     exception type matches.
#   - The bench harness's default cap setup — any benchmark that
#     accidentally introduces a hot loop will fail-fast instead of
#     stalling the runner.

let f = x: f (x + 1); in f 0
