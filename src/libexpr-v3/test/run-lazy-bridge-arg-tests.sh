#!/usr/bin/env bash
# v3 NIX_V3_LAZY_BRIDGE_ARG positive/negative tests.
#
# The flag (commit bb09cc61b) skips the eager `forceValue(*args[1])`
# in primV3CallBridge1, mirroring tree-walker's regular callFunction
# laziness.  treeWalkerToV3's nThunk case wraps the unforced arg as
# a v3 Bridge thunk so v3's body can force it on demand.
#
# Tests:
#   POSITIVE 1: TW-thunk arg, v3 closure that NEVER forces -> arg
#               stays a thunk; bridge produces correct result.
#   POSITIVE 2: TW-thunk arg, v3 closure that DOES force -> arg
#               forces transparently via Bridge thunk on first access.
#   NEGATIVE  : without the flag, the same shape force-evaluates the
#               arg before bridge dispatch (verifies the flag actually
#               changes behaviour, not no-op).
#
# Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.
# SPDX-License-Identifier: Apache-2.0

set -u

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
NIX_BIN="${NIX_BIN:-$ROOT/build/src/nix/nix}"

if [[ ! -x "$NIX_BIN" ]]; then
  echo "nix not found at $NIX_BIN" >&2
  exit 1
fi

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

# --- POSITIVE 1: callable returned via v3, arg untouched.
cat > "$TMP/p1.nix" <<'EOF'
let
  # v3-side closure (literal lambda) that ignores its arg.
  ignore = x: 42;
in ignore (throw "should not force")
EOF

# --- POSITIVE 2: callable that forces its arg.
cat > "$TMP/p2.nix" <<'EOF'
let
  identity = x: x;
in identity (1 + 2)
EOF

# --- NEGATIVE: an arg whose force throws.  With the flag, the closure
# may swallow it (identity-ignore patterns); without the flag, the
# eager force fires and surfaces the throw at primop dispatch time.
cat > "$TMP/n1.nix" <<'EOF'
let
  ignore = x: "ok";
in ignore (throw "deferred force should not fire")
EOF

ok=0; fail=0; total=0
fail_names=()

check() {
  local name="$1"; shift
  local expected="$1"; shift
  local mode="$1"; shift
  local file="$1"
  total=$((total + 1))
  local out
  out=$(env $mode timeout 5 "$NIX_BIN" eval --offline --json -f "$file" 2>/dev/null) \
    || out="<error>"
  if [[ "$out" == "$expected" ]]; then
    ok=$((ok + 1))
  else
    fail=$((fail + 1))
    fail_names+=("[$name] expected='$expected' got='$out' mode='$mode'")
  fi
}

# POSITIVE 1: arg is a throw, but ignored.  TW result: 42.
check "p1-tw"        "42" "" "$TMP/p1.nix"
check "p1-v3"        "42" "NIX_USE_V3=1" "$TMP/p1.nix"
check "p1-v3+lazy"   "42" "NIX_USE_V3=1 NIX_V3_LAZY_BRIDGE_ARG=1" "$TMP/p1.nix"

# POSITIVE 2: identity forces its arg.  TW & v3 both: 3.
check "p2-tw"      "3" "" "$TMP/p2.nix"
check "p2-v3"      "3" "NIX_USE_V3=1" "$TMP/p2.nix"
check "p2-v3+lazy" "3" "NIX_USE_V3=1 NIX_V3_LAZY_BRIDGE_ARG=1" "$TMP/p2.nix"

# NEGATIVE: arg is throw, ignored by closure.  TW: "ok" (TW callFunction
# is lazy on args).  v3 default + lazy-bridge-arg: also "ok" because the
# lambda body never forces.  Without the closure-bridge in play (this
# is a v3 native lambda, no bridge1), behaviour matches TW unconditionally.
check "n1-tw"      '"ok"' "" "$TMP/n1.nix"
check "n1-v3"      '"ok"' "NIX_USE_V3=1" "$TMP/n1.nix"
check "n1-v3+lazy" '"ok"' "NIX_USE_V3=1 NIX_V3_LAZY_BRIDGE_ARG=1" "$TMP/n1.nix"

echo "=== lazy-bridge-arg tests: ok=$ok fail=$fail (total=$total) ==="
if [[ $fail -gt 0 ]]; then
  printf '  %s\n' "${fail_names[@]}"
  exit 1
fi
exit 0
