#!/usr/bin/env bash
# Regression test for #684 — addErrorContext must preserve the wrapped
# exception's RUNTIME TYPE so tryEval still catches AssertionError-
# class errors through it.
#
# Pre-fix v3 collapsed every caught exception into `std::runtime_error`,
# breaking these semantics:
#   tryEval (addErrorContext "ctx" (throw "x"))
#     → TW: { success = false; value = false; }
#     → V3 pre-fix: error: ctx \n x  (NOT caught)
#
# Tree-walker uses `e.addTrace(...)` which mutates the SAME exception
# object and rethrows.  v3 doesn't have the frame machinery yet but
# at least preserves the type by catching each error class
# specifically and rethrowing with the same class.
#
# Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
# Input Output Group.  SPDX-License-Identifier: Apache-2.0
set -u

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
NIX="${NIX:-$ROOT/build/src/nix/nix}"

last_error_line() { grep -E "^[[:space:]]*error: " | tail -1; }
strip_ws() {
  local s="$1"
  s="${s#"${s%%[![:space:]]*}"}"
  s="${s%"${s##*[![:space:]]}"}"
  echo "$s"
}

run_pos_case() {
  local label="$1" expr="$2" expected="$3"
  local tw v3
  tw="$("$NIX" eval --impure --expr "$expr" 2>/dev/null || true)"
  v3="$(NIX_V3_DIRECT_EVAL=1 \
        NIX_V3_MAX_WALL_TIME=5s "$NIX" eval --impure --expr "$expr" 2>/dev/null || true)"
  if [[ "$tw" == "$v3" && "$tw" == "$expected" ]]; then
    printf "  POS-OK   %-35s => %s\n" "$label" "$tw"
    return 0
  else
    printf "  POS-FAIL %-35s expected=%s\n    TW: %s\n    V3: %s\n" "$label" "$expected" "$tw" "$v3"
    return 1
  fi
}

run_err_case() {
  local label="$1" expr="$2"
  local tw v3
  tw="$("$NIX" eval --impure --expr "$expr" 2>&1 | last_error_line)"
  v3="$(NIX_V3_DIRECT_EVAL=1 \
        NIX_V3_MAX_WALL_TIME=5s "$NIX" eval --impure --expr "$expr" 2>&1 | last_error_line)"
  tw="$(strip_ws "$tw")"
  v3="$(strip_ws "$v3")"
  if [[ "$tw" == "$v3" || "$tw" == "$v3"* ]]; then
    printf "  ERR-OK   %-35s => %s\n" "$label" "$v3"
    return 0
  else
    printf "  ERR-FAIL %-35s\n    TW: %s\n    V3: %s\n" "$label" "$tw" "$v3"
    return 1
  fi
}

fail=0

# POSITIVE — tryEval-wrapped contexts must still produce { success=false; ... }.
run_pos_case "tryEval-ctx-throw" \
  'builtins.tryEval (builtins.addErrorContext "ctx" (throw "x"))' \
  '{ success = false; value = false; }' || fail=$((fail+1))
run_pos_case "tryEval-ctx-assert" \
  'builtins.tryEval (builtins.addErrorContext "ctx" (assert false; 1))' \
  '{ success = false; value = false; }' || fail=$((fail+1))
# Successful path — addErrorContext on a non-throwing expr returns the value.
run_pos_case "passes-thru-int" \
  'builtins.addErrorContext "ctx" 42' '42' || fail=$((fail+1))
run_pos_case "passes-thru-str" \
  'builtins.addErrorContext "ctx" "hi"' '"hi"' || fail=$((fail+1))

# NEGATIVE — abort context must NOT be caught (TW's tryEval doesn't
# catch Abort either).  The main error message is the abort's text;
# v3 also appends the context line, matching the spirit of TW's
# frame architecture.
run_err_case "tryEval-ctx-abort" \
  'builtins.tryEval (builtins.addErrorContext "ctx" (abort "boom"))' \
  || fail=$((fail+1))
# Plain context on a throw: TW shows the throw text in the bottom error line.
run_err_case "ctx-throw-msg" \
  'builtins.addErrorContext "ctx" (throw "x")' || fail=$((fail+1))

if [[ "$fail" -eq 0 ]]; then
  echo "run-684: PASS (4 positive + 2 negative shapes match TW)"
  exit 0
else
  echo "run-684: FAIL ($fail divergence(s))"
  exit 1
fi
