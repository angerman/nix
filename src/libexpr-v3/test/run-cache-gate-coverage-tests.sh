#!/usr/bin/env bash
# Self-test / regression guardrail for lint-cache-coherence.sh Rule 3
# (codegen env gate ⇒ present in kGates[] disk-cache fingerprint).
#
# DEFECT_REVIEW_2026-07-03 §1.4 found the kGates[] fingerprint had drifted
# out of sync with the codegen-file getenv() reads (NIX_V3_RAW_FORMALS and
# NIX_V3_NO_NONREC_ATTRS_INIT both change emitted bytecode yet were absent
# from the key), and that the lint claiming to enforce the sync did not.
# Rule 3 closes that hole.  This test is its failing-first proof: it drives
# the lint with synthetic fixtures and asserts BOTH directions —
#   - a gate present in kGates[] (or the non-codegen allowlist) is accepted
#   - a codegen gate absent from BOTH is rejected
# plus a smoke check that the REAL tree is clean.
#
# Registered in the --brute battery so the fingerprint can never silently
# drift again (the failure class recurs until the lint actually checks it).
#
# Usage:   bash src/libexpr-v3/test/run-cache-gate-coverage-tests.sh
# Exit:    0 all sub-tests passed; 1 any failed.
#
# Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
#   Input Output Group.  SPDX-License-Identifier: Apache-2.0

set -u

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
LINT="$ROOT/src/libexpr-v3/test/lint-cache-coherence.sh"

if [[ ! -f "$LINT" ]]; then
  echo "run-cache-gate-coverage: cannot find $LINT" >&2
  exit 1
fi

fails=0
ok()   { printf '  PASS: %s\n' "$1"; }
bad()  { printf '  FAIL: %s\n' "$1" >&2; fails=$((fails+1)); }

# rule3 FIXTURE_PRIMOPS FIXTURE_DIR -> exit code of the lint (Rule 3 only)
rule3() {
  CACHE_LINT_RULE3_ONLY=1 \
  CACHE_LINT_PRIMOPS="$1" \
  CACHE_LINT_CODEGEN_DIR="$2" \
    bash "$LINT" >/dev/null 2>&1
}

echo "== Rule 3 coverage self-test =="

# ----------------------------------------------------------------------
# Test 0: the REAL tree must be Rule-3 clean (every codegen gate covered).
# ----------------------------------------------------------------------
if CACHE_LINT_RULE3_ONLY=1 bash "$LINT" >/dev/null 2>&1; then
  ok "real tree: all codegen gates in kGates[] or allowlist"
else
  bad "real tree: Rule 3 flagged an uncovered codegen gate"
fi

# ----------------------------------------------------------------------
# Synthetic fixtures (self-contained; no committed throwaway sources).
# ----------------------------------------------------------------------
TMP="$(mktemp -d 2>/dev/null || mktemp -d -t cachegate)"
trap 'rm -rf "$TMP"' EXIT

# A minimal primops.cc whose kGates[] covers exactly NIX_V3_GATE_A.
cat > "$TMP/primops.cc" <<'CC'
static const char * const kGates[] = {
    "NIX_V3_GATE_A",
};
CC

# Test 1 (accept): codegen file reads only a covered gate.
cat > "$TMP/emit.cc" <<'CC'
void f() { if (std::getenv("NIX_V3_GATE_A")) { /* codegen */ } }
CC
if rule3 "$TMP/primops.cc" "$TMP"; then
  ok "covered codegen gate accepted"
else
  bad "covered codegen gate should be accepted"
fi

# Test 2 (reject): codegen file reads a gate absent from kGates + allowlist.
cat > "$TMP/emit.cc" <<'CC'
void f() { if (std::getenv("NIX_V3_GATE_B")) { /* codegen */ } }
CC
if rule3 "$TMP/primops.cc" "$TMP"; then
  bad "uncovered codegen gate should be rejected"
else
  ok "uncovered codegen gate rejected"
fi

# Test 3 (accept): codegen file reads an allowlisted (non-codegen dump) gate.
cat > "$TMP/emit.cc" <<'CC'
void f() { if (std::getenv("NIX_V3_EMIT_BYTECODE")) { /* dump only */ } }
CC
if rule3 "$TMP/primops.cc" "$TMP"; then
  ok "allowlisted dump gate accepted"
else
  bad "allowlisted dump gate should be accepted"
fi

# Test 4 (reject): a second codegen file (opt_*.cc glob) with an uncovered gate.
cat > "$TMP/emit.cc" <<'CC'
void f() { if (std::getenv("NIX_V3_GATE_A")) {} }
CC
cat > "$TMP/opt_fixture.cc" <<'CC'
void g() { if (std::getenv("NIX_V3_GATE_C")) { /* codegen */ } }
CC
if rule3 "$TMP/primops.cc" "$TMP"; then
  bad "uncovered gate in opt_*.cc should be rejected"
else
  ok "uncovered gate in opt_*.cc rejected"
fi

echo
if [[ $fails -gt 0 ]]; then
  echo "run-cache-gate-coverage: $fails sub-test(s) FAILED" >&2
  exit 1
fi
echo "run-cache-gate-coverage: all sub-tests passed"
exit 0
