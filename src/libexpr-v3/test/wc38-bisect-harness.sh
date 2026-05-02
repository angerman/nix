#!/usr/bin/env bash
# WC-38 bisection harness.
# Exit 0 if bug reproduced (v3 fails WC-38, tw OK); 1 otherwise.

set -u

ROOT="/Users/angerman/Projects/iohk/nix"
NIXPKGS="${NIXPKGS:-/tmp/nixpkgs-bisect}"
TIMEOUT="${TIMEOUT:-60}"
# Single-quoted default to avoid shell glob/redirect on <nixpkgs>{}.
if [[ -z "${EXPR:-}" ]]; then
  EXPR='builtins.isAttrs (import <nixpkgs>{})'
fi

export NIX_PATH="nixpkgs=$NIXPKGS:nixpkgs-overlays=/tmp/wc38-overlays"

# Tree-walker
tw_out=$(timeout "$TIMEOUT" "$ROOT/build/src/nix/nix-instantiate" --eval -E "$EXPR" 2>/dev/null)
tw_rc=$?

# v3
v3_out=$(timeout "$TIMEOUT" "$ROOT/build/src/libexpr-v3/v3-eval" --expr "$EXPR" 2>&1)
v3_rc=$?

v3_has_wc38=0
if printf '%s' "$v3_out" | grep -q "OP_WITH_LOOKUP"; then
  v3_has_wc38=1
fi
tw_succeeded=0
if [[ $tw_rc -eq 0 ]] && [[ -n "$tw_out" ]]; then
  tw_succeeded=1
fi

echo "tw  rc=$tw_rc out='$(echo "$tw_out" | tail -1)'"
echo "v3  rc=$v3_rc last='$(echo "$v3_out" | tail -1)'"

if [[ $v3_has_wc38 -eq 1 && $tw_succeeded -eq 1 ]]; then
  echo "STATUS: BUG-REPRODUCED"
  exit 0
else
  echo "STATUS: NOT-REPRODUCED (v3_wc38=$v3_has_wc38 tw_ok=$tw_succeeded)"
  exit 1
fi
