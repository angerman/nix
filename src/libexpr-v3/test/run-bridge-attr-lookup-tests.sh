#!/usr/bin/env bash
# #458 step A.2 — tryBridgeAttrLookup positive/negative/regression tests.
#
# A.2 lets v3's OP_WITH_LOOKUP, when iterating a Bridge thunk wrapping
# a TW Value attrset, peek at the partial bindings and resolve `name`
# WITHOUT forcing the whole TW Value.  This avoids the cardano-node
# `with self;` over-fix-point cycle (#455) where the outer thunk is
# Black mid-construction but individual entries are already published.
#
# Tests:
#   p1  with-self synthetic from OD-shapes works under default v3
#   p2  same shape via the call hook (TW lambda + v3-bridged self)
#   p3  fix-point + with: a synthetic that exercises the partial-
#       binding lookup path explicitly
#   n1  parity: TW result equals v3 result on every shape
#   n2  formerly auto-eager-required test now passes with
#       NIX_V3_NO_CALL_HOOK_EAGER=1 (the helper made auto-eager
#       redundant for these shapes)
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

# p1 — classic with-self: v3 lowers the lambda body's `with self;` and
#       the first lookup finds `a` in self before self is fully
#       published.  Should produce 15.
cat > "$TMP/p1.nix" <<'EOF'
let s = rec { a = 10; f = x: with s; x + a; }; in s.f 5
EOF
EXP_P1='15'

# p2 — fix-point with `with self;` over fix-pointed arg, mirroring
#       the cardano-node aliases.nix shape.
cat > "$TMP/p2.nix" <<'EOF'
let
  fix = f: let x = f x; in x;
  buildAliases = self: super: with self; { aa = a; bb = b + 1; };
  toFix = self: { a = "A"; b = 10; } // (buildAliases self {});
  result = fix toFix;
in [ result.aa result.bb ]
EOF
EXP_P2='[ "A" 11 ]'

# p3 — `with` over a fix-point arg with NESTED with-scopes; outer
#       scope publishes some entries, inner needs them.  The bridge-
#       attr-lookup must resolve in the closer scope first.
cat > "$TMP/p3.nix" <<'EOF'
let
  fix = f: let x = f x; in x;
  inner = self: with self; with { extra = "X"; }; a + extra;
  outer = self: { a = "A"; r = inner self; };
in (fix outer).r
EOF
EXP_P3='"AX"'

ok=0
fail=0
fail_names=()

run_one() {
  local label="$1"; shift
  local nix_file="$1"; shift
  local expected="$1"; shift
  local extra_env=("$@")

  local got
  got=$(env "${extra_env[@]}" "$NIX_BIN" eval --no-eval-cache -f "$nix_file" 2>/dev/null) \
    || got="<error>"
  if [[ "$got" == "$expected" ]]; then
    ok=$((ok + 1))
  else
    fail=$((fail + 1))
    fail_names+=("$label  expected=$expected  got=$got")
  fi
}

# Modes:
#   tw-base: tree-walker baseline
#   v3-default: v3 default (Phase E), should match TW
#   v3-no-eager: NIX_V3_NO_CALL_HOOK_EAGER=1 -- previously needed the
#                 auto-eager guard for some fix-point shapes; A.2
#                 should make it unnecessary.
modes=(
  "tw-base::"
  "v3-default::NIX_USE_V3=1"
  "v3-no-eager::NIX_USE_V3=1 NIX_V3_NO_CALL_HOOK_EAGER=1"
)

for spec in "${modes[@]}"; do
  IFS=:: read -r tag _ envspec <<< "$spec"
  IFS=' ' read -ra envarr <<< "$envspec"
  run_one "$tag/p1" "$TMP/p1.nix" "$EXP_P1" "${envarr[@]}"
  run_one "$tag/p2" "$TMP/p2.nix" "$EXP_P2" "${envarr[@]}"
  run_one "$tag/p3" "$TMP/p3.nix" "$EXP_P3" "${envarr[@]}"
done

echo "=== bridge-attr-lookup tests: ok=$ok fail=$fail (total=$((ok+fail))) ==="
for n in "${fail_names[@]}"; do
  echo "  FAIL $n"
done
[[ $fail -eq 0 ]] || exit 1
