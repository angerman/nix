#!/usr/bin/env bash
# Phase-S non-moving tenured region tests
# (NONMOVING_INLINE_THUNK_PLAN_2026-07-06 §5.3).
#
# Proves the NON-MOVING tenured line-region reclaim (compile flag
# NIX_V3_NONMOVING_TENURED) is CORRECT: byte-identical results + no crash, both
# with the flag ON and under aggressive GC stress.  The flag is COMPILE-TIME
# (like NIX_V3_BARRIER_NOOP), so this driver runs in one of two modes:
#
#   * DEFAULT (V3_NMT unset — how it runs inside --brute): the flag is inert in
#     the shipped binary, so this acts as a POSITIVE + REGRESSION guard — it
#     evaluates the size-tuned repro, asserts the (known) deterministic result,
#     and (under --brute's moving-GC stress gates) exercises the safepoint path.
#
#   * FLAG-ON (V3_NMT=/path/to/flag-on/v3-eval): runs the full spike gate —
#     (+) positive: ON result == OFF result (byte-id);
#     (−) stress:   ON under V3_DBG_GC_STRESS=1000 + NIX_V3_MAJOR_GC_THRESHOLD_MB=16
#                   (constant non-moving reclaim = missed-root surfacer) — expect
#                   NO crash + byte-id.
# The regression guard is the full --brute battery (this suite is registered in
# core so it runs there under the moving-GC stress).
#
# Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
#   Input Output Group.  SPDX-License-Identifier: Apache-2.0
set -u

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
TEST_DIR="$(cd "$(dirname "$0")" && pwd)"
V3="${V3:-$ROOT/build/src/libexpr-v3/v3-eval}"          # flag-OFF (shipped) binary
V3_NMT="${V3_NMT:-}"                                    # flag-ON binary, if built
POS="$TEST_DIR/repro-nonmoving-tenured-pos.nix"

if [[ ! -x "$V3" ]]; then echo "nonmoving-tenured: v3-eval not at $V3" >&2; exit 2; fi
if [[ ! -f "$POS" ]]; then echo "nonmoving-tenured: repro not at $POS" >&2; exit 2; fi

# Common eval limits (see CLAUDE.md "Running v3 probes safely").  Low major-GC
# threshold (16 MB clamp floor) + min growth (1.5) so the always-on gen-major
# safepoint fires ≥2× during the eval (VERIFIED 2 fires on the repro) — the
# non-moving span reclaim then runs across multiple safepoints.
common_env=(NIX_V3_DIRECT_EVAL=1 NIX_V3_MAX_WALL_TIME=180s NIX_V3_MAX_HEAP=6G
            NIX_V3_MAJOR_GC_THRESHOLD_MB=16 NIX_V3_MAJOR_GC_GROWTH=1.5)

fail=0

# --- (+) evaluate the repro under the flag-OFF binary (baseline result) -------
off_out="$(env "${common_env[@]}" "$V3" --file "$POS" 2>/dev/null)"
off_rc=$?
if [[ $off_rc -ne 0 || -z "$off_out" ]]; then
  echo "nonmoving-tenured: FAIL — flag-OFF eval rc=$off_rc out='$off_out'" >&2
  exit 1
fi

if [[ -z "$V3_NMT" ]]; then
  # DEFAULT MODE (inside --brute): positive + regression only.  The shipped
  # binary has the flag OFF; a clean, deterministic result here (and no crash
  # under --brute's exported moving-GC stress gates) is the regression guard.
  echo "nonmoving-tenured: PASS (flag-OFF regression guard; result=$off_out;"
  echo "                        set V3_NMT=<flag-on v3-eval> for the ON byte-id + stress gate)"
  exit 0
fi

# --- FLAG-ON spike gate -------------------------------------------------------
if [[ ! -x "$V3_NMT" ]]; then
  echo "nonmoving-tenured: FAIL — V3_NMT set but not executable: $V3_NMT" >&2
  exit 1
fi

# (+) positive: ON result must byte-equal OFF result.
on_out="$(env "${common_env[@]}" "$V3_NMT" --file "$POS" 2>/dev/null)"
on_rc=$?
if [[ $on_rc -ne 0 ]]; then
  echo "nonmoving-tenured: FAIL — flag-ON eval crashed/errored rc=$on_rc" >&2
  fail=1
elif [[ "$on_out" != "$off_out" ]]; then
  echo "nonmoving-tenured: FAIL — byte-id (+) ON='$on_out' != OFF='$off_out'" >&2
  fail=1
else
  echo "nonmoving-tenured: PASS (+) byte-id ON==OFF ($on_out)"
fi

# (−) stress: constant non-moving reclaim (scavenge + mark every 1000 ops, tiny
# GC threshold) is the missed-root surfacer.  Expect NO crash + byte-id.
stress_out="$(env "${common_env[@]}" V3_DBG_GC_STRESS=1000 \
                  "$V3_NMT" --file "$POS" 2>/dev/null)"
stress_rc=$?
if [[ $stress_rc -ne 0 ]]; then
  echo "nonmoving-tenured: FAIL (−) STRESS crashed/errored rc=$stress_rc" \
       "(signal $((stress_rc - 128))?) — missed root under non-moving reclaim" >&2
  fail=1
elif [[ "$stress_out" != "$off_out" ]]; then
  echo "nonmoving-tenured: FAIL (−) STRESS diverged '$stress_out' != OFF '$off_out'" >&2
  fail=1
else
  echo "nonmoving-tenured: PASS (−) stress byte-id + no crash ($stress_out)"
fi

[[ $fail -eq 0 ]] && exit 0 || exit 1
