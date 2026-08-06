#!/usr/bin/env bash
# String-coercion parity — concatStringsSep / substring / stringLength.
# TW vs v3-direct byte-equality regression guard for #740.
#
# Four CONFIRMED tree-walker-parity bugs in v3's string primops, all now
# fixed to match TW's `coerceToString` (eval.hh defaults for these three
# builtins: coerceMore=FALSE, copyToStore=TRUE):
#
#   A1  concatStringsSep with raw Path elements — TW copies each path to
#       /nix/store and attaches an Opaque string-context entry; pre-fix v3
#       returned the SOURCE path with NO context (→ wrong drvPath, silently).
#   A2  substring with a raw Path arg — same store-copy divergence.
#   A3  concatStringsSep / substring / stringLength on int/float/bool/null/
#       list — TW THROWS `cannot coerce <type> to a string: <value>`; pre-fix
#       v3 fail-open coerced (coerceMore was implicitly true).
#   A4  stringLength on a Path — TW coerces (copies to store, returns the
#       store-path byte length, e.g. 47); pre-fix v3 hard-threw a typeError.
#
# Positive cases assert TW == v3 byte-for-byte (store-path HASH included, via
# --raw; string context via getContext --json).  Negative cases assert BOTH
# evaluators throw AND emit a byte-identical `cannot coerce ...` message
# (compared dynamically, not against a hardcoded string, so float formatting
# etc. can't drift the assertion).  Regression cases prove strings + attrsets
# with outPath/__toString still coerce (coerceMore=false must not reject them).
#
# The store copy makes this need `--impure` + store write access, but no
# nixpkgs / IFD build — local paths with content realise offline + fast.
#
# Usage:  NIX=/path/to/nix ./run-coerce-parity-tests.sh
#
# Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
# Input Output Group.  SPDX-License-Identifier: Apache-2.0

set -u

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
NIX="${NIX:-$ROOT/build/src/nix/nix}"

if [[ ! -x "$NIX" ]]; then
  echo "coerce-parity: nix not found at $NIX" >&2
  exit 1
fi

EXF=(--extra-experimental-features "nix-command flakes")
pass=0
fail=0
failed=()

# Positive: TW and v3-direct must agree byte-for-byte on the result.
# v3's stderr is left attached to this suite's stderr (not swallowed) so
# the --brute harness's SCAVENGE BRUTE / AUDIT log scan still sees any
# missed-root signature emitted during these allocating evals; only stdout
# feeds the comparison.
check() {  # label -- <nix eval args...>
  local label="$1"; shift
  local tw v3
  tw=$("$NIX" "${EXF[@]}" "$@" 2>/dev/null)
  v3=$(NIX_V3_DIRECT_EVAL=1 NIX_V3_REQUIRE=1 NIX_V3_MAX_WALL_TIME=60s \
        "$NIX" "${EXF[@]}" "$@")
  if [[ -n "$tw" && "$tw" == "$v3" ]]; then
    pass=$((pass + 1))
  else
    fail=$((fail + 1))
    failed+=("$label (TW=[$tw] v3=[$v3])")
  fi
}

# Negative: both must FAIL, and the `cannot coerce ... to a string: ...`
# message must be byte-identical between TW and v3 (extracted, then compared
# — no hardcoded expectation, so value-repr formatting can't drift it).
check_err() {  # label expr
  local label="$1" expr="$2" twerr v3err twmsg v3msg
  twerr=$("$NIX" "${EXF[@]}" eval --impure --expr "$expr" 2>&1 >/dev/null)
  v3err=$(NIX_V3_DIRECT_EVAL=1 NIX_V3_REQUIRE=1 NIX_V3_MAX_WALL_TIME=60s \
        "$NIX" "${EXF[@]}" eval --impure --expr "$expr" 2>&1 >/dev/null)
  twmsg=$(printf '%s\n' "$twerr" | grep -o 'cannot coerce .* to a string:.*' | head -1)
  v3msg=$(printf '%s\n' "$v3err" | grep -o 'cannot coerce .* to a string:.*' | head -1)
  if [[ -n "$twmsg" && "$twmsg" == "$v3msg" ]]; then
    pass=$((pass + 1))
  else
    fail=$((fail + 1))
    failed+=("$label (TWmsg=[$twmsg] v3msg=[$v3msg])")
  fi
}

# --- Fixture: two real paths with content (so copyPathToStore works). ---
D=$(mktemp -d)
mkdir -p "$D/bin" "$D/sbin"
echo x > "$D/bin/f"
echo y > "$D/sbin/f"
P1="$D/bin"     # path literals below are UNQUOTED → Nix path values
P2="$D/sbin"

# =====================================================================
# POSITIVE — raw Path elements copy to /nix/store (byte-identical store
# path hash + string context) [A1, A2, A4].
# =====================================================================
check "A1 concatStringsSep paths (store path + hash)" \
  eval --raw --impure --expr "builtins.concatStringsSep \":\" [ $P1 $P2 ]"
check "A1 concatStringsSep paths (string context)" \
  eval --impure --json --expr \
  "builtins.attrNames (builtins.getContext (builtins.concatStringsSep \":\" [ $P1 $P2 ]))"
check "A2 substring path (store path + hash)" \
  eval --raw --impure --expr "builtins.substring 0 999 $P1"
check "A2 substring path (string context)" \
  eval --impure --json --expr \
  "builtins.attrNames (builtins.getContext (builtins.substring 0 999 $P1))"
check "A4 stringLength path (store-path byte length)" \
  eval --impure --expr "builtins.stringLength $P1"

# =====================================================================
# REGRESSION — strings still coerce; attrsets with outPath / __toString
# still coerce (coerceMore=false must NOT reject these).
# =====================================================================
check "S1 concatStringsSep strings" \
  eval --raw --impure --expr "builtins.concatStringsSep \":\" [ \"a\" \"b\" ]"
check "S2 substring string" \
  eval --raw --impure --expr "builtins.substring 0 3 \"nixos\""
check "S3 stringLength string" \
  eval --impure --expr "builtins.stringLength \"hello\""
check "S4 concatStringsSep outPath-attrs" \
  eval --raw --impure --expr \
  "builtins.concatStringsSep \":\" [ { outPath = \"x\"; } { outPath = \"y\"; } ]"
check "S5 substring __toString-attrs" \
  eval --raw --impure --expr \
  "builtins.substring 0 3 { __toString = self: \"nixos\"; }"
check "S6 stringLength outPath-attrs" \
  eval --impure --expr "builtins.stringLength { outPath = \"abc\"; }"

# =====================================================================
# NEGATIVE — int / float / bool / null / list all THROW the SAME
# `cannot coerce <type> to a string: <value>` as TW [A3].
# =====================================================================
check_err "A3 concatStringsSep int"   "builtins.concatStringsSep \":\" [ 1 2 3 ]"
check_err "A3 concatStringsSep float"  "builtins.concatStringsSep \":\" [ 1.5 ]"
check_err "A3 concatStringsSep bool"   "builtins.concatStringsSep \":\" [ true false ]"
check_err "A3 concatStringsSep null"   "builtins.concatStringsSep \":\" [ null ]"
check_err "A3 concatStringsSep list"   "builtins.concatStringsSep \":\" [ [ 1 ] ]"
check_err "A3 substring int"           "builtins.substring 0 2 12345"
check_err "A3 substring bool"          "builtins.substring 0 2 true"
check_err "A3 substring null"          "builtins.substring 0 2 null"
check_err "A3 substring list"          "builtins.substring 0 2 [ 1 2 ]"
check_err "A3 stringLength int"        "builtins.stringLength 12345"
check_err "A3 stringLength float"      "builtins.stringLength 1.5"
check_err "A3 stringLength bool"       "builtins.stringLength true"
check_err "A3 stringLength null"       "builtins.stringLength null"
check_err "A3 stringLength list"       "builtins.stringLength [ 1 2 ]"

rm -rf "$D"

# ---------------------------------------------------------------------
echo
echo "=== string-coercion parity (concatStringsSep / substring / stringLength) ==="
echo "  passing: $pass"
echo "  failing: $fail"
if (( fail > 0 )); then
  printf '  FAIL: %s\n' "${failed[@]}"
  exit 1
fi
echo "  ALL PASS"
