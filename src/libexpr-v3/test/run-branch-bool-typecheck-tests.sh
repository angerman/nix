#!/usr/bin/env bash
# v3 branch-opcode boolean type-check regression tests (P1.1).
#
# Guards audit DEFECT_AUDIT_2026-07-02 §2.1: the branch opcodes
# (OP_BRANCH_FALSE / OP_AND_BRANCH / OP_OR_BRANCH / OP_IMPL_BRANCH /
# OP_R_BRANCH_FALSE) used to accept a NON-Boolean condition and silently
# treat it as truthy -- `if 1 then a else b` evaluated `a`, `1 && x`
# returned `x`.  A live semantic divergence from the tree-walker (TW).
#
# Failing-first regression battery kept forever per the debug story
# (CLAUDE.md).  Checks BOTH halves of the fix:
#   NEGATIVE -- a non-bool condition must ERROR, with byte-parity to TW's
#               "expected a Boolean but found <type>: <value>" message body.
#   POSITIVE -- valid boolean code (if / && / || / ->) is UNCHANGED and
#               agrees with TW (byte-identity guard against over-eager checks).
#
# Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
#   Input Output Group.  SPDX-License-Identifier: Apache-2.0
set -u

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
V3="${V3:-$ROOT/build/src/libexpr-v3/v3-eval}"
NIX="${NIX:-$ROOT/build/src/nix/nix}"   # tree-walker oracle (no NIX_V3_DIRECT_EVAL)

if [[ ! -x "$V3" ]];  then echo "branch-bool-typecheck: v3-eval not at $V3" >&2; exit 2; fi
if [[ ! -x "$NIX" ]]; then echo "branch-bool-typecheck: nix not at $NIX" >&2; exit 2; fi

pass=0; fail=0
failed=()

# NEGATIVE: "<expr>@@@<expected message fragment>".  The fragment is TW's
# parity body (libexpr/eval.cc:1228); v3-eval reproduces it verbatim.
# Delimiter is @@@ so exprs may contain '|'.
neg_cases=(
  'if 1 then "a" else "b"@@@expected a Boolean but found an integer: 1'
  '1 && true@@@expected a Boolean but found an integer: 1'
  '"x" || true@@@expected a Boolean but found a string: "x"'
  '1 -> true@@@expected a Boolean but found an integer: 1'
  'if (builtins.head [ 3 ]) then 1 else 2@@@expected a Boolean but found an integer: 3'
  'if null then 1 else 2@@@expected a Boolean but found null'
)
for row in "${neg_cases[@]}"; do
  expr="${row%%@@@*}"
  frag="${row##*@@@}"
  v3_err="$(NIX_V3_DIRECT_EVAL=1 "$V3" --expr "$expr" 2>&1 >/dev/null || true)"
  tw_err="$("$NIX" eval --expr "$expr" 2>&1 >/dev/null || true)"
  if [[ "$v3_err" == *"$frag"* && "$tw_err" == *"$frag"* ]]; then
    pass=$((pass + 1))
  else
    fail=$((fail + 1))
    failed+=("NEG [${expr}] want[${frag}] v3[$(printf '%s' "$v3_err" | head -c 100)] tw[$(printf '%s' "$tw_err" | head -c 100)]")
  fi
done

# POSITIVE: valid boolean code unchanged and TW-equal.
pos_cases=(
  'if true then 10 else 20'
  'if false then 10 else 20'
  'if 1 < 2 then 10 else 20'
  'true && false'
  'true && true'
  'false || true'
  'false || false'
  'false -> true'
  'true -> false'
  'true -> true'
  'if (1 == 1) && (2 > 1) then "y" else "n"'
)
for expr in "${pos_cases[@]}"; do
  v3_out="$(NIX_V3_DIRECT_EVAL=1 "$V3" --expr "$expr" 2>/dev/null || echo '<v3-error>')"
  tw_out="$("$NIX" eval --expr "$expr" 2>/dev/null || echo '<tw-error>')"
  if [[ "$v3_out" == "$tw_out" && "$v3_out" != '<v3-error>' ]]; then
    pass=$((pass + 1))
  else
    fail=$((fail + 1))
    failed+=("POS [${expr}] v3[${v3_out}] tw[${tw_out}]")
  fi
done

echo "branch-bool-typecheck: pass=${pass} fail=${fail}"
if (( fail > 0 )); then
  printf '  FAIL %s\n' "${failed[@]}" >&2
  exit 1
fi
exit 0
