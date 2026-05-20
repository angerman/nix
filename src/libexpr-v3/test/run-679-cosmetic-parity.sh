#!/usr/bin/env bash
# Regression test for #679 — small cosmetic parity fixes.
#
# 1. builtins.unsafeGetAttrPos must return null for SYNTHETIC sources
#    (`--expr`, `<stdin>`, `<unknown>`) and only return a position
#    attrset when the origin is a real SourcePath.  TW emits null in
#    `EvalState::mkPos` (libexpr/eval.cc:1015) when the PosIdx's
#    origin isn't a SourcePath; v3 pre-fix unconditionally produced
#    the synthetic-source position info `{ column; file = "<string>";
#    line; }`, which leaked v3-internal source markers to users.
#
# 2. Closure-token printing factored into a helper (`printClosureToken`)
#    so both lazy and non-lazy printer paths emit a consistent shape.
#    Doesn't change output for any tested case — pure refactor.
#
# Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
# Input Output Group.  SPDX-License-Identifier: Apache-2.0
set -u

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
NIX="${NIX:-$ROOT/build/src/nix/nix}"

if [[ ! -x "$NIX" ]]; then
  echo "run-679: nix not executable at $NIX" >&2
  exit 2
fi

run_case() {
  local label="$1" expr="$2"
  local tw v3
  tw="$("$NIX" eval --impure --expr "$expr" 2>/dev/null || true)"
  v3="$(NIX_V3_DIRECT_EVAL=1 NIX_V3_SKIP_INSTALLABLE_PREEVAL=1 \
        NIX_V3_MAX_WALL_TIME=5s "$NIX" eval --impure --expr "$expr" 2>/dev/null || true)"
  if [[ "$tw" == "$v3" && -n "$tw" ]]; then
    printf "  MATCH    %-40s => %s\n" "$label" "${tw:0:60}"
    return 0
  else
    printf "  DIVERGE  %-40s\n    TW: %s\n    V3: %s\n" "$label" "$tw" "$v3"
    return 1
  fi
}

# Create a tiny file so we can compare file-origin behaviour too.
TMPF="$(mktemp /tmp/run-679.XXXXXX.nix)"
trap 'rm -f "$TMPF"' EXIT
printf '{ a = 1; }\n' > "$TMPF"

fail=0
run_case "unsafe-pos-expr-present" \
  'builtins.unsafeGetAttrPos "a" { a = 1; }'              || fail=$((fail+1))
run_case "unsafe-pos-expr-missing" \
  'builtins.unsafeGetAttrPos "x" { a = 1; }'              || fail=$((fail+1))
run_case "unsafe-pos-file-present" \
  "builtins.unsafeGetAttrPos \"a\" (import $TMPF)"        || fail=$((fail+1))
run_case "unsafe-pos-file-missing" \
  "builtins.unsafeGetAttrPos \"x\" (import $TMPF)"        || fail=$((fail+1))

if [[ "$fail" -eq 0 ]]; then
  echo "run-679: PASS (4/4 cosmetic-parity shapes match TW)"
  exit 0
else
  echo "run-679: FAIL ($fail divergence(s))"
  exit 1
fi
