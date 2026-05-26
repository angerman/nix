#!/usr/bin/env bash
# v3 serialize.cc SymbolId-coverage lint — CR2 closure (per
# lode/CR1_CR2_AUDIT_RESULTS_2026-05-26.md §3.3 + lode/
# DIRECTION_NOTE_2026-05-26.md §6 action #4).
#
# What this enforces
# ------------------
# `serialize.cc` defines two functions that MUST stay in lockstep:
#
#   collectReferencedSymbols(cu) - walks the bytecode + lambda metadata,
#       returns the sorted-unique set of SymbolIds the writer needs to
#       emit in the sparse symbolTable section.
#
#   remapSymbolsInBytecode(cu, remap) - walks the bytecode + lambda
#       metadata at load time, patches every SymbolId operand /
#       trailing-data slot from the writer's ID to the reader's ID.
#
# Any opcode that carries a SymbolId operand or has SymbolId-bearing
# trailing data MUST appear in BOTH functions.  Any opcode that has
# trailing data of ANY kind (even non-SymbolId) MUST appear in both
# functions' ip-advance logic — otherwise the walk de-syncs and the
# whole CU corrupts.
#
# AR24 (ARCHITECTURE_CRITIQUE_2026-05-26.md §3.3) — "sparse symbol-
# table walk vs remap dispatch can drift on new opcode" — is the
# class this lint closes structurally.
#
# How it works
# ------------
# 1. Carve out the bodies of `collectReferencedSymbols` and
#    `remapSymbolsInBytecode` (signature line up to first `^}` at
#    column 0 after the opening `^{`).
# 2. Extract the set of `OP_*` tokens referenced in each body.
# 3. Diff the sets.  An asymmetry that is NOT in the documented
#    exception list (currently: OP_ATTRS_REC_SET — patched via
#    `pending` permutation in remap, no walk in collect because slot
#    is index not SymbolId) is a violation.
#
# Exception list
# --------------
# OP_ATTRS_REC_SET appears in remap (operand-patch via pending
# permutation stack) but NOT in collect (slot is index, not
# SymbolId).  This is documented in serialize.cc and is BY DESIGN.
# To add a new exception, document inline in serialize.cc AND add
# the opcode token to ASYMMETRY_EXCEPTIONS below with rationale.
#
# Process rule (codify alongside this lint)
# -----------------------------------------
# When adding a new opcode that carries a SymbolId operand or
# SymbolId-bearing trailing data, the commit MUST:
#   (a) update `collectReferencedSymbols` to bump every SymbolId
#       referenced, AND
#   (b) update `remapSymbolsInBytecode` to remap every SymbolId
#       referenced + advance ip past trailing data correctly, AND
#   (c) bump kSchemaVersion in include/v3/serialize.hh (existing
#       cache-coherence Rule 2 from lint-cache-coherence.sh).
#
# Falsification check (per [[falsification-rule]])
# -------------------
# To verify the lint catches the drift class: revert just the
# `OP_REC_BINDING_SLOT_REF` branch from `remapSymbolsInBytecode`
# (the Phase-13 CRIT-1 fix in commit `??`) — the lint MUST reject.
#
# Usage
# -----
#   bash src/libexpr-v3/test/lint-serialize-symbolid-coverage.sh
#
# Exit codes
# ----------
#   0  no violation
#   1  asymmetry detected (likely missing handler in one of the two
#      functions)
#   2  preflight failed (file missing, awk not available, ...)
#
# Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
# Input Output Group.  SPDX-License-Identifier: Apache-2.0

set -u

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
SERIALIZE_CC="$ROOT/src/libexpr-v3/serialize.cc"

# Documented asymmetry exceptions.  Each entry is "OP_<NAME>:<which>"
# where <which> is "collect-only" or "remap-only".  Adding to this
# list requires inline documentation in serialize.cc justifying the
# asymmetry.
#
# OP_ATTRS_REC_SET: remap patches its operand (a slot index) via the
# `pending` permutation stack built when the matching REC_INIT was
# re-sorted; collect doesn't bump because the operand is not a
# SymbolId.  See remapSymbolsInBytecode comment near "Active
# permutations are tracked in a small stack".
ASYMMETRY_EXCEPTIONS=(
  "OP_ATTRS_REC_SET:remap-only"
)

if [[ ! -f "$SERIALIZE_CC" ]]; then
  echo "lint-serialize-symbolid-coverage: missing $SERIALIZE_CC" >&2
  exit 2
fi
if ! command -v awk >/dev/null 2>&1; then
  echo "lint-serialize-symbolid-coverage: awk not available" >&2
  exit 2
fi

# Carve out a function body.  Args: file, function name.
# Prints the body lines from the signature through the first '^}' at
# column 0 after the opening '{'.  Robust against nested braces inside
# the body because we anchor on column 0.
extract_body() {
  local file="$1"
  local fname="$2"
  awk -v fname="$fname" '
    # Match the signature line: the function name followed by `(`.
    # We do NOT match calls (`fname(arg)`) because the awk pattern is
    # anchored against either the function-definition style with
    # return type on the prior line OR `void fname(`/`std::vector<...>
    # fname(` style.  We rely on the convention that the function
    # signature in this codebase starts at column 0 (or has return type
    # at column 0 with the name on the next line).
    {
      if (in_body) {
        # End of body: a `}` at column 0.
        if ($0 ~ /^}/) {
          in_body = 0
          next
        }
        print
      } else {
        # Detect signature line.  Two patterns:
        #   <type> fname(...
        #   <return type on prev line>; fname(...   (rare)
        # The function names of interest both have their signature on
        # the line containing the name + `(`.
        if ($0 ~ ("(^|[^A-Za-z0-9_])" fname "[[:space:]]*\\(")) {
          # Skip declarations: a `;` on the same line means this is
          # a forward declaration, not a definition.
          if ($0 ~ /;[[:space:]]*$/) next
          # Begin body.  We consume the signature line(s) + opening
          # brace, then start emitting from the line after `{`.
          while (!/\{[[:space:]]*$/ && !/\{[[:space:]]*\/\//) {
            if (getline next_line <= 0) exit
            $0 = next_line
            if ($0 ~ /;[[:space:]]*$/) next
          }
          in_body = 1
        }
      }
    }
  ' "$file"
}

# Extract OP_* tokens from a body.  Returns one token per line, deduped
# and sorted.  Strips C++ `//` line comments BEFORE matching: the lint
# is about CODE coverage, not comment mentions.  serialize.cc has a
# comment in `collectReferencedSymbols` that lists opcodes
# deliberately NOT handled (OP_ATTRS_REC_SET et al.) — those mentions
# should NOT count toward the "appears in collect" set.  Also strips
# C-style /* ... */ comments restricted to a single line (no
# multi-line C-comment support here; rare in this file).
extract_op_tokens() {
  sed -E 's|//.*$||; s|/\*[^*]*\*/||g' \
    | grep -oE 'OP_[A-Z][A-Z0-9_]*' \
    | sort -u
}

collect_body=$(extract_body "$SERIALIZE_CC" "collectReferencedSymbols")
remap_body=$(extract_body   "$SERIALIZE_CC" "remapSymbolsInBytecode")

if [[ -z "$collect_body" ]]; then
  echo "lint-serialize-symbolid-coverage: could not extract collectReferencedSymbols body" >&2
  exit 2
fi
if [[ -z "$remap_body" ]]; then
  echo "lint-serialize-symbolid-coverage: could not extract remapSymbolsInBytecode body" >&2
  exit 2
fi

collect_ops=$(printf '%s\n' "$collect_body" | extract_op_tokens)
remap_ops=$(printf   '%s\n' "$remap_body"   | extract_op_tokens)

# Compute set differences.
collect_only=$(comm -23 <(printf '%s\n' "$collect_ops") <(printf '%s\n' "$remap_ops"))
remap_only=$(  comm -13 <(printf '%s\n' "$collect_ops") <(printf '%s\n' "$remap_ops"))

# Filter out documented exceptions.
filter_exceptions() {
  local which="$1"  # "collect-only" or "remap-only"
  local input="$2"
  local filtered="$input"
  for exc in "${ASYMMETRY_EXCEPTIONS[@]}"; do
    local op="${exc%%:*}"
    local exc_which="${exc##*:}"
    if [[ "$exc_which" == "$which" ]]; then
      filtered=$(printf '%s\n' "$filtered" | grep -vxF -- "$op" || true)
    fi
  done
  printf '%s' "$filtered"
}

collect_only_real=$(filter_exceptions "collect-only" "$collect_only")
remap_only_real=$(  filter_exceptions "remap-only"   "$remap_only")

violations=0

if [[ -n "${collect_only_real// /}" ]]; then
  echo "lint-serialize-symbolid-coverage FAIL: opcodes in collectReferencedSymbols but NOT in remapSymbolsInBytecode:" >&2
  printf '  %s\n' $collect_only_real >&2
  echo "" >&2
  echo "  Likely cause: the writer collects these SymbolIds into the sparse" >&2
  echo "  symbolTable but the reader doesn't remap them.  Cross-process" >&2
  echo "  deserialization will leave stale writer-side IDs in the bytecode." >&2
  echo "  Fix: add a remap branch for each missing opcode in" >&2
  echo "  remapSymbolsInBytecode + bump kSchemaVersion." >&2
  violations=$((violations + 1))
fi

if [[ -n "${remap_only_real// /}" ]]; then
  echo "lint-serialize-symbolid-coverage FAIL: opcodes in remapSymbolsInBytecode but NOT in collectReferencedSymbols:" >&2
  printf '  %s\n' $remap_only_real >&2
  echo "" >&2
  echo "  Likely cause: the reader tries to remap SymbolIds that the writer" >&2
  echo "  never added to the sparse symbolTable.  Lookups against the sparse" >&2
  echo "  table will fall off the end and use the unmapped writer ID (which" >&2
  echo "  is a different symbol on the reader side)." >&2
  echo "  Fix: either add a bump() call in collectReferencedSymbols for" >&2
  echo "  each missing opcode (if it actually carries a SymbolId), OR" >&2
  echo "  document the asymmetry in ASYMMETRY_EXCEPTIONS in this lint" >&2
  echo "  (with inline rationale in serialize.cc)." >&2
  violations=$((violations + 1))
fi

if [[ $violations -gt 0 ]]; then
  echo "" >&2
  echo "See lode/CR1_CR2_AUDIT_RESULTS_2026-05-26.md §3 for the audit" >&2
  echo "that motivated this lint.  AR24 from ARCHITECTURE_CRITIQUE" >&2
  echo "§3.3 was closed by the audit; this lint catches FUTURE drift." >&2
  exit 1
fi

# Success — emit a short summary so CI logs show what was checked.
ops_count=$(printf '%s\n' "$collect_ops" "$remap_ops" | sort -u | wc -l | tr -d ' ')
echo "lint-serialize-symbolid-coverage: OK ($ops_count distinct OP_* tokens; $(echo "$collect_ops" | wc -l | tr -d ' ') in collect, $(echo "$remap_ops" | wc -l | tr -d ' ') in remap; ${#ASYMMETRY_EXCEPTIONS[@]} documented asymmetry exception(s))"
exit 0
