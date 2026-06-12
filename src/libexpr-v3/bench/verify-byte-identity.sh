#!/usr/bin/env bash
# Fast correctness gate for the CODEBASE_REVIEW Sprint 4/5 implementation pass.
#
# Rebuilds v3-eval + nix + nix-instantiate, then runs the two host-runnable
# byte-identity gates: the 143-case lang suite and the 20-case derivation
# parity battery.  Perf is NOT measured here (that needs darwin-4 per the
# v3 measurement gate) — this only proves a change preserved CORRECTNESS.
#
#   bash bench/verify-byte-identity.sh          # build + both gates
#   SKIP_BUILD=1 bash bench/verify-byte-identity.sh
#
# Exit 0 iff lang == 143/143 AND parity == 20/20.
#
# Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.
# SPDX-License-Identifier: Apache-2.0
set -u
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
cd "$ROOT"

if [[ "${SKIP_BUILD:-0}" != "1" ]]; then
  echo "── build ──"
  if ! nix develop -c ninja -C build \
        src/libexpr-v3/v3-eval src/nix/nix src/nix/nix-instantiate 2>&1 | tail -3; then
    echo "BUILD FAILED" >&2; exit 3
  fi
fi

echo "── lang (expect 143/143) ──"
lang_out="$(nix develop -c bash src/libexpr-v3/test/run-lang-tests.sh 2>&1)"
echo "$lang_out" | tail -5
lang_pass="$(echo "$lang_out" | sed -n 's/.*passing:[[:space:]]*\([0-9]*\).*/\1/p' | tail -1)"
lang_fail="$(echo "$lang_out" | sed -n 's/.*failing:[[:space:]]*\([0-9]*\).*/\1/p' | tail -1)"

echo "── derivation-parity (expect 20/20) ──"
drv_out="$(nix develop -c bash src/libexpr-v3/test/derivation-parity.sh 2>&1)"
echo "$drv_out" | tail -4
drv_fail="$(echo "$drv_out" | sed -n 's/.*fail:[[:space:]]*\([0-9]*\).*/\1/p' | tail -1)"

echo "──────────"
if [[ "$lang_pass" == "143" && "${lang_fail:-x}" == "0" && "${drv_fail:-x}" == "0" ]]; then
  echo "VERIFY OK: lang 143/143, parity 20/20"; exit 0
else
  echo "VERIFY FAIL: lang $lang_pass/$((lang_pass+lang_fail)) fail=$lang_fail, parity fail=$drv_fail" >&2
  exit 1
fi
