#!/usr/bin/env bash
# WS-1 H4 lint — every read-class IFD-probe primop must realise its argument.
#
# The C1/C2 divergence (2026-07-13 review) was a read-class primop
# (hashFile / readFileType) touching the filesystem via raw std::filesystem
# WITHOUT first realising the arg's derivation context — so an un-built IFD
# output was mis-read instead of built.  This lint pins the invariant
# structurally: each read-class primop body must contain a realise call, so a
# future edit that drops realisation fails CI instead of silently regressing.
#
# It is a SOURCE lint (greps primops.cc), not a runtime test — cheap, and it
# guards the shape the runtime parity suite (run-ws1-realise-parity-tests.sh)
# checks behaviourally.
#
# Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.
# SPDX-License-Identifier: Apache-2.0

set -u

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
SRC="$ROOT/src/libexpr-v3/primops.cc"

if [[ ! -f "$SRC" ]]; then
  echo "lint-ifd-realise-coverage: primops.cc not found at $SRC" >&2
  exit 2
fi

# Read-class primops that MUST realise (IfdProbeKind read tier + hashFile,
# which TW realises but is not separately probe-counted).  Fetchers
# (fetchTree/Git/…) realise through a different store path and are out of
# scope for this lint.
OPS=(primImport primScopedImport primReadFile primReadDir primPathExists primReadFileType primFindFile primHashFile)

# Any of these tokens in the function body counts as "realises".
REALISE_RE='v3RealisePathArg|realisePath|realiseContext|ffi::realisePath|->findFile\('

fail=0
for op in "${OPS[@]}"; do
  # Slice the function body: from its signature line at column 0 to the next
  # closing brace at column 0.  (All these are free functions in primops.cc.)
  body=$(awk "/^(void|static void) ${op}\\(/{f=1} f{print} f&&/^}/{exit}" "$SRC")
  if [[ -z "$body" ]]; then
    echo "FAIL  $op: function not found in primops.cc (renamed? update this lint)"
    fail=1
    continue
  fi
  if echo "$body" | grep -Eq "$REALISE_RE"; then
    echo "OK    $op realises its argument"
  else
    echo "FAIL  $op does NOT realise — a read-class IFD primop must build its"
    echo "      argument's context before touching the filesystem (WS-1 C1/C2)."
    fail=1
  fi
done

echo "-----------------------------------------------------------------------"
if [[ $fail -ne 0 ]]; then
  echo "lint-ifd-realise-coverage: FAIL"
  exit 1
fi
echo "lint-ifd-realise-coverage: PASS (${#OPS[@]} read-class primops realise)"
exit 0
