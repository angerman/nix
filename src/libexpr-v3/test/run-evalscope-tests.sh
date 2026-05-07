#!/usr/bin/env bash
# #485: regression for the EvalScope skeleton (ffi.cc).  Validates the
# RAII ctor/dtor behavior + nested-scope chaining via a small C++ harness.
#
# This is currently a build-and-link smoke test: ffi.cc must compile and
# link, and the symbols must be visible.  Behavioural unit tests for
# scope-bound handle invalidation will land alongside the migration of
# v3FormalsLambdaBridges to use ClosureHandle.
#
# Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.
# SPDX-License-Identifier: Apache-2.0

set -u

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
LIB="${LIB:-$ROOT/build/src/libexpr-v3/libnixexprv3.dylib}"

if [[ ! -e "$LIB" ]]; then
  echo "libnixexprv3 not found at $LIB" >&2
  exit 1
fi

PASS=0; FAIL=0
fail_names=()

assert_present() {
  local name="$1" sym="$2"
  # Itanium C++ ABI mangled names (used by clang on Apple/Linux):
  #   EvalScope ctor:        nix::v3::EvalScopeC1ERNS0_9EvaluatorE
  #   EvalScope dtor:        nix::v3::EvalScopeD1Ev
  #   promoteToGlobal:       nix::v3::15promoteToGlobalERNS0_9EvalScope...
  #   releaseGlobal:         nix::v3::13releaseGlobalENS0_19GlobalClosureHandleE
  # All start with `__ZN3nix2v3` (Apple) or `_ZN3nix2v3` (Linux); the
  # method/function name follows.
  if nm -gU "$LIB" 2>/dev/null | grep -E "_ZN3nix2v3.*${sym}" > /dev/null; then
    PASS=$((PASS + 1))
  else
    FAIL=$((FAIL + 1))
    fail_names+=("$name: symbol nix::v3::$sym not found in $LIB")
  fi
}

# Verify the FFI framework types actually link in.
assert_present "EvalScope ctor"   "9EvalScopeC"
assert_present "EvalScope dtor"   "9EvalScopeD"
assert_present "promoteToGlobal"  "15promoteToGlobal"
assert_present "releaseGlobal"    "13releaseGlobal"

echo
echo "=== evalscope tests: ok=$PASS fail=$FAIL ==="
if [[ $FAIL -gt 0 ]]; then
  for n in "${fail_names[@]}"; do echo "  FAIL: $n"; done
  exit 1
fi
exit 0
