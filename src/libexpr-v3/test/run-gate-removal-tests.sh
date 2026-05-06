#!/usr/bin/env bash
# #457/#458 — gate-removal regression tests.
#
# As we systematically lift the gates that decline v3 ownership
# (`stay in the v3 VM as much as possible`), each gate flip needs
# accompanying positive/negative/regression cases to pin the new
# behaviour and catch any future re-decline.
#
# Tests:
#   T1 (formals gate): formals lambdas now run through v3 by default.
#       Positive: NixOS-module-shape lambdas evaluate identically
#                 to TW.
#       Negative: NIX_V3_NO_CALL_FORMALS=1 declines formals (legacy
#                 path); result still parity.
#       Regression: arg-laziness preserved (per-attr Bridge thunk
#                   matches TW's per-formal lazy semantics).
#
# Future tests appended as we lift more gates.
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

# T1.p1 — basic formals lambda
cat > "$TMP/t1p1.nix" <<'EOF'
let f = { a, b ? 10 }: a + b; in f { a = 5; }
EOF
EXP_T1P1='15'

# T1.p2 — formals + ellipsis (NixOS-module shape)
cat > "$TMP/t1p2.nix" <<'EOF'
let f = { config, options ? {}, ... }@args: config.x or 0;
in f { config = { x = 42; }; }
EOF
EXP_T1P2='42'

# T1.p3 — formals lambda + arg laziness: v3 must NOT force unused entries.
cat > "$TMP/t1p3.nix" <<'EOF'
let f = { a, b ? 0 }: a;
in f { a = 99; b = throw "should not force"; }
EOF
EXP_T1P3='99'

# T1.p4 — formals + recursion (the `lib: self: super:` shape from cardano-node).
cat > "$TMP/t1p4.nix" <<'EOF'
let
  fix = f: let x = f x; in x;
  buildLayer = { lib, self, super }: { result = self.base + super.add; };
  toFix = self: { base = 10; result = (buildLayer { inherit lib self super; }).result; };
  lib = { id = x: x; };
  super = { add = 5; };
in (fix toFix).result
EOF
EXP_T1P4='15'

# T1.n1 — same as p1 but with NIX_V3_NO_CALL_FORMALS=1 (legacy path).
#         Should still produce identical result.

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
#   tw:        TW baseline.
#   v3:        v3 default (formals now ON by default).
#   v3-noform: NIX_V3_NO_CALL_FORMALS=1 (legacy refuse-formals path).
modes=(
  "tw::"
  "v3::NIX_USE_V3=1"
  "v3-noform::NIX_USE_V3=1 NIX_V3_NO_CALL_FORMALS=1"
)

for spec in "${modes[@]}"; do
  IFS=:: read -r tag _ envspec <<< "$spec"
  IFS=' ' read -ra envarr <<< "$envspec"
  run_one "$tag/T1p1" "$TMP/t1p1.nix" "$EXP_T1P1" "${envarr[@]}"
  run_one "$tag/T1p2" "$TMP/t1p2.nix" "$EXP_T1P2" "${envarr[@]}"
  run_one "$tag/T1p3" "$TMP/t1p3.nix" "$EXP_T1P3" "${envarr[@]}"
  run_one "$tag/T1p4" "$TMP/t1p4.nix" "$EXP_T1P4" "${envarr[@]}"
done

echo "=== gate-removal tests: ok=$ok fail=$fail (total=$((ok+fail))) ==="
for n in "${fail_names[@]}"; do
  echo "  FAIL $n"
done
[[ $fail -eq 0 ]] || exit 1
