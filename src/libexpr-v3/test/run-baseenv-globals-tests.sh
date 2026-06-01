#!/usr/bin/env bash
# Regression suite for native-lower base-env name resolution
# (PARSER_PROJECT_PLAN §5.3 / the firefox.drvPath store-path divergence).
#
# A v3-registered primop name that is NOT a Nix base-env global (e.g. bare
# `fetchurl`, `head`, `foldl`) must NOT be shortcut to the builtin by the
# native lowerer — it must resolve via `with`/lexical, matching TW's
# bindVars.  Otherwise nixpkgs' bare `fetchurl` (the FOD) wrongly becomes
# `builtins.fetchurl`, producing a wrong (fake-store) drvPath.
#
# Shapes:
#   1 (positive) repro-baseenv-nonglobal-primop.nix — `with`-shadowed
#     non-global names resolve to the with-binding; native-lower output
#     must byte-match TW.  Run under BOTH native-lower and bridge.
#   2 (negative) bare non-global name with NO enclosing scope must be an
#     "unbound/undefined variable" error under native-lower, exactly as TW
#     rejects it (TW: "undefined variable"; v3: "unbound variable").
#   3 (positive) a genuine global (`map`) must still resolve to the builtin
#     under native-lower (byte-match TW).
#
# Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.
# SPDX-License-Identifier: Apache-2.0
set -u

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
V3="${V3:-$ROOT/builddir/src/libexpr-v3/v3-eval}"
TW="${TW:-$ROOT/builddir/src/nix/nix-instantiate}"
HERE="$(cd "$(dirname "$0")" && pwd)"

if [[ ! -x "$V3" ]]; then echo "v3-eval not found at $V3" >&2; exit 2; fi
if [[ ! -x "$TW" ]]; then echo "nix-instantiate not found at $TW" >&2; exit 2; fi

pass=0; fail=0
# v3 under native-lower; strip the harmless darwin stack-size warning.
v3n() { NIX_V3_NATIVE_PARSER=1 NIX_V3_NATIVE_LOWER=1 "$V3" "$@" 2>&1 | grep -vE "Failed to increase stack"; }
v3b() { NIX_V3_NATIVE_PARSER=1 "$V3" "$@" 2>&1 | grep -vE "Failed to increase stack"; }

check_eq() { # name expected actual
    if [[ "$2" == "$3" ]]; then echo "  ok   $1"; pass=$((pass+1));
    else echo "  FAIL $1"; echo "    expected: $2"; echo "    actual:   $3"; fail=$((fail+1)); fi
}

echo "=== base-env non-global primop resolution ==="

# 1. positive: with-shadowed non-globals resolve to the with-binding.
want=$("$TW" --eval --strict "$HERE/repro-baseenv-nonglobal-primop.nix" 2>/dev/null)
gotN=$(v3n --file "$HERE/repro-baseenv-nonglobal-primop.nix" --strict)
gotB=$(v3b --file "$HERE/repro-baseenv-nonglobal-primop.nix" --strict)
check_eq "with-shadowed non-globals (native-lower)" "$want" "$gotN"
check_eq "with-shadowed non-globals (bridge)"       "$want" "$gotB"

# 2. negative: bare non-global with no scope must be unresolved under both.
for name in fetchurl head foldl filter genList length attrNames; do
    tw_err=$("$TW" --eval --expr "$name" 2>&1 | grep -cE "undefined variable|unbound variable")
    v3_err=$(v3n --expr "$name" --strict 2>&1 | grep -cE "undefined variable|unbound variable")
    if [[ "$tw_err" -ge 1 && "$v3_err" -ge 1 ]]; then echo "  ok   bare '$name' unresolved (both)"; pass=$((pass+1));
    else echo "  FAIL bare '$name' resolution differs (tw_err=$tw_err v3_err=$v3_err)"; fail=$((fail+1)); fi
done

# 3. positive: a genuine global still resolves to the builtin.
for g in map throw import toString derivation; do
    want=$("$TW" --eval --expr "builtins.typeOf $g" 2>/dev/null)
    got=$(v3n --expr "builtins.typeOf $g" --strict)
    check_eq "genuine global '$g' resolves" "$want" "$got"
done

echo ""
echo "=== results: $pass passed, $fail failed ==="
[[ "$fail" -eq 0 ]]
