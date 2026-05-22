#!/usr/bin/env bash
# Regression test for #686 — `with E; body` must evaluate E lazily.
# TW: E is captured as a thunk and forced only when OP_WITH_LOOKUP
# first scans an entry.  Pre-fix v3 eagerly evaluated E at lower
# time, so `with (throw "x"); 1` threw even though the body never
# referenced the with-scope (TW returns 1).
#
# Also covers the related WITH_LOOKUP error message alignment:
#   pre: `v3 OP_WITH_LOOKUP: name 'b' not found in with-scope`
#   TW:  `undefined variable 'b'`
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
    printf "  POS-OK   %-30s => %s\n" "$label" "$tw"
    return 0
  else
    printf "  POS-FAIL %-30s expected=%s\n    TW: %s\n    V3: %s\n" "$label" "$expected" "$tw" "$v3"
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
    printf "  ERR-OK   %-30s => %s\n" "$label" "$v3"
    return 0
  else
    printf "  ERR-FAIL %-30s\n    TW: %s\n    V3: %s\n" "$label" "$tw" "$v3"
    return 1
  fi
}

fail=0

# POSITIVE — with-arg is NOT forced when body doesn't reference it.
run_pos_case "lazy-throw-body-1" \
  'with (throw "x"); 1'                 '1' || fail=$((fail+1))
run_pos_case "lazy-throw-via-let" \
  'with (let x = throw "x"; in {}); 1'  '1' || fail=$((fail+1))
run_pos_case "lazy-throw-via-let-y" \
  'with (let x = throw "x"; in { y = 1; }); y' '1' || fail=$((fail+1))
run_pos_case "lazy-side-attr-ok" \
  'with (let x = throw "x"; in { x = 1; }); x' '1' || fail=$((fail+1))

# POSITIVE — with-arg IS forced when body looks up a name not in scope.
run_pos_case "simple-with-lookup" \
  'with { a = 1; }; a'                  '1' || fail=$((fail+1))
run_pos_case "with-binds-multi" \
  '(with { a = 1; }; a) + (with { a = 2; }; a)' '3' || fail=$((fail+1))

# NEGATIVE — with-arg IS forced when body uses scope.
run_err_case "force-on-lookup" \
  'with (throw "x"); a' || fail=$((fail+1))

# NEGATIVE — undefined variable in with-scope: TW-matching message.
run_err_case "undef-via-with" \
  'with { a = 1; }; b'  || fail=$((fail+1))
run_err_case "undef-via-with-or" \
  'with { a = 1; }; b or 42'  || fail=$((fail+1))

if [[ "$fail" -eq 0 ]]; then
  echo "run-686: PASS (6 positive + 3 negative shapes match TW)"
  exit 0
else
  echo "run-686: FAIL ($fail divergence(s))"
  exit 1
fi
