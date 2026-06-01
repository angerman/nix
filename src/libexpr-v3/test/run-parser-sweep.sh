#!/usr/bin/env bash
# v3-native parser real-file validation sweep (Stage 1.5).
#
# PARSER_PROJECT_PLAN_2026-06-01.md §2.  Parses every .nix in the lang
# corpus with BOTH `v3-parse` (the v3-native parser) and
# `nix-instantiate --parse` (the TW oracle) and byte-compares the show()
# output.  This is the Stage 1 validation surface: byte-equal v3 AST vs
# TW AST over real-world files (it does NOT evaluate — that is Stage 2).
#
# Categories:
#   MATCH      both parse, show() identical                     (good)
#   DIFF       both parse, show() differs                       (PARSER BUG)
#   V3-FAIL    TW parsed, v3 errored                            (PARSER BUG)
#   TW-FAIL    v3 parsed, TW errored                            (bindVars/env)
#   BOTH-FAIL  both rejected                                    (good)
#
# v3-parse is parse-ONLY: it does NOT run bindVars (variable resolution
# is Stage 2 / lowering).  So TW-FAIL is expected for files with free or
# now-unbound variables (`undefined variable …`) — those are NOT parser
# bugs.  The sweep FAILS (exit 1) only on DIFF or V3-FAIL.
#
# Usage:
#   nix develop -c ./src/libexpr-v3/test/run-parser-sweep.sh
#   V3_SWEEP_VERBOSE=1 ... ./run-parser-sweep.sh   # list every category
#
# Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
#   Input Output Group.  SPDX-License-Identifier: Apache-2.0
set -u

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
V3="${V3:-$ROOT/builddir/src/libexpr-v3/v3-parse}"
TW="${TW:-$ROOT/builddir/src/nix/nix-instantiate}"
LANG_DIR="${LANG_DIR:-$ROOT/tests/functional/lang}"
# pipe-operators must be enabled for TW to parse `|>` / `<|` fixtures.
TW_XP=(--extra-experimental-features pipe-operators)

[ -x "$V3" ] || { echo "missing v3-parse: $V3 (build it first)"; exit 2; }
[ -x "$TW" ] || { echo "missing nix-instantiate: $TW"; exit 2; }

match=0 diff=0 v3fail=0 twfail=0 bothfail=0
difflist="" v3faillist="" twfaillist=""

# Parseable corpus (should parse): eval-okay + parse-okay + eval-fail
# (eval-fail tests fail at EVAL, not parse, with a few parse-time
# exceptions that both reject).
for f in "$LANG_DIR"/eval-okay-*.nix "$LANG_DIR"/parse-okay-*.nix "$LANG_DIR"/eval-fail-*.nix; do
  [ -e "$f" ] || continue
  tw=$("$TW" "${TW_XP[@]}" --parse "$f" 2>/dev/null); twrc=$?
  v3=$("$V3" "$f" 2>/dev/null); v3rc=$?
  name=$(basename "$f" .nix)
  if   [ $twrc -ne 0 ] && [ $v3rc -ne 0 ]; then bothfail=$((bothfail+1))
  elif [ $twrc -eq 0 ] && [ $v3rc -ne 0 ]; then v3fail=$((v3fail+1)); v3faillist="$v3faillist $name"
  elif [ $twrc -ne 0 ] && [ $v3rc -eq 0 ]; then twfail=$((twfail+1)); twfaillist="$twfaillist $name"
  elif [ "$tw" = "$v3" ]; then match=$((match+1))
  else diff=$((diff+1)); difflist="$difflist $name"; fi
done

echo "=== v3-native parser real-file sweep (lang corpus) ==="
echo "  MATCH      : $match"
echo "  DIFF       : $diff        (show differs — PARSER BUG)"
echo "  V3-FAIL    : $v3fail        (TW ok, v3 errored — PARSER BUG)"
echo "  TW-FAIL    : $twfail        (v3 ok, TW errored — bindVars/env, not parser)"
echo "  BOTH-FAIL  : $bothfail        (both rejected — ok)"
if [ -n "${V3_SWEEP_VERBOSE:-}" ]; then
  [ -n "$difflist" ]    && echo "  DIFF:$difflist"
  [ -n "$v3faillist" ]  && echo "  V3-FAIL:$v3faillist"
  [ -n "$twfaillist" ]  && echo "  TW-FAIL:$twfaillist"
fi

if [ $diff -ne 0 ] || [ $v3fail -ne 0 ]; then
  echo "FAIL: real parser divergence (DIFF=$diff V3-FAIL=$v3fail)"
  [ -z "${V3_SWEEP_VERBOSE:-}" ] && echo "  (re-run with V3_SWEEP_VERBOSE=1 to list them)"
  exit 1
fi
echo "PASS: no parser divergences (all non-matches are bindVars/env, which is Stage 2)"
exit 0
