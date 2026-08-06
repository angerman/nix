#!/usr/bin/env bash
# v3 cache-coherence lint: enforce two operating rules codified after
# the #803 / #814 / #815 cache-coherence arc (see lode/
# NEXT_STEPS_2026-05-25.md §2.6 + §A4).
#
# Rule 1: schema bump on LambdaDescriptor field add/remove/rename.
#   Any commit that adds, removes, or reorders fields in
#   `LambdaDescriptor` (declared in `include/v3/closure.hh` and serialised
#   in `serialize.cc`) MUST also bump `kSchemaVersion` in
#   `include/v3/serialize.hh`.  Without the bump, older on-disk CUs
#   load under the new layout and produce garbage values for the
#   shifted fields — exactly the failure mode of #814 (selectorSym
#   silently consumed the wrong byte stream).
#
# Rule 2: schema bump on deserialise-path interpretation change.
#   Any change to `serialize.cc`'s deserialiseCU body that alters how
#   existing bytes are interpreted (e.g., new remap, new re-sort, new
#   conditional read) MUST also bump `kSchemaVersion`.  Pure refactors
#   that touch deserialiseCU but don't change behaviour are exempt and
#   must be flagged via the `# CACHE-COHERENCE-EXEMPT: <reason>` marker
#   in the commit message body.
#
# How the lint detects violations:
#   - Inspect the diff (staged for pre-commit, or HEAD..main for CI).
#   - If `closure.hh`'s `struct LambdaDescriptor` body has any added /
#     removed line (modulo whitespace, comments) → Rule 1 applies.
#   - If `serialize.cc`'s `deserializeCU` body has any added line
#     (modulo whitespace, comments) → Rule 2 applies.
#   - In either case, `include/v3/serialize.hh`'s `kSchemaVersion`
#     constant MUST also be modified in the same diff (an integer
#     increment is verified textually, not numerically).
#   - Either a matching schema-bump line OR a `CACHE-COHERENCE-EXEMPT`
#     marker (in the diff body) passes.
#
# Falsification check (per [[falsification-rule]]):
#   - To verify the lint catches the #814 historical pattern: replay
#     `git diff $(git merge-base 9e09a7e4c~1 master) 9e09a7e4c~1` (the
#     pre-#814 state where selectorSym was added without bump) — lint
#     must reject.  Then replay 9e09a7e4c (the fix commit that bumps
#     schema 12→13) — lint must accept.
#   - To verify lint doesn't fire on pure refactors: a no-op
#     whitespace-only diff in serialize.cc must NOT trigger Rule 2.
#
# Usage:
#   bash src/libexpr-v3/test/lint-cache-coherence.sh             # pre-commit (staged)
#   CACHE_LINT_RANGE=HEAD..origin/master \
#     bash src/libexpr-v3/test/lint-cache-coherence.sh           # CI range
#
# Exit codes:
#   0  no violation
#   1  one or both rules violated
#   2  lint preflight failed (file missing, git not available, ...)
#
# Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
# Input Output Group.  SPDX-License-Identifier: Apache-2.0

set -u

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
CLOSURE_HH="$ROOT/src/libexpr-v3/include/v3/closure.hh"
SERIALIZE_CC="$ROOT/src/libexpr-v3/serialize.cc"
SERIALIZE_HH="$ROOT/src/libexpr-v3/include/v3/serialize.hh"

# ----------------------------------------------------------------------
# Rule 3: every codegen-affecting env gate is in the disk-cache
#         fingerprint (kGates[])
# ----------------------------------------------------------------------
# The CU disk-cache key keys only on (path, content, schema); the
# `codegenGateFingerprint()` in primops.cc mixes the *values* of a
# canonical list of env gates (kGates[]) into the key so that a CU
# compiled under e.g. NIX_V3_RAW_FORMALS=1 is namespaced away from a
# default-codegen CU.  DEFECT_REVIEW_2026-07-03 §1.4 found that this
# list had silently drifted out of sync with the actual getenv() reads
# in the codegen files (NIX_V3_RAW_FORMALS + NIX_V3_NO_NONREC_ATTRS_INIT
# both emit different opcodes but were absent from kGates), and that the
# comment claiming `lint-cache-coherence.sh` enforced the sync was
# fictional — the lint only checked Rules 1/2.  This rule closes that:
# every `getenv("NIX_V3_*")` in a codegen file MUST be either in
# kGates[] OR in the explicit non-codegen allowlist below.  A CU whose
# bytecode depends on a gate not in kGates would be loaded under the
# wrong key on any warm run after a toggled run — the exact silent
# wrong-codegen failure the fingerprint exists to prevent.
#
# This is a FULL-TREE invariant (not diff-scoped): it re-verifies the
# whole codegen surface on every run, so a hole cannot slip through a
# commit that doesn't touch primops.cc.
#
# Paths are overridable (CACHE_LINT_PRIMOPS / CACHE_LINT_CODEGEN_DIR)
# so the self-test (run-cache-gate-coverage-tests.sh) can drive it with
# synthetic fixtures.  CACHE_LINT_RULE3_ONLY=1 runs ONLY this rule
# (skipping the git/diff-based Rules 1/2), for the self-test.
PRIMOPS_CC="${CACHE_LINT_PRIMOPS:-$ROOT/src/libexpr-v3/primops.cc}"
CODEGEN_DIR="${CACHE_LINT_CODEGEN_DIR:-$ROOT/src/libexpr-v3}"

# Non-codegen gates that legitimately live in the codegen files: pure
# diagnostics/dumps that leave the emitted opcode stream bit-identical,
# so they need not namespace the disk-cache key.  Every entry is a
# deliberate assertion "this gate does NOT change codegen"; a NEW gate
# added to a codegen file must go to kGates[] OR here (with a reason),
# never neither.
rule3_allowlist=(
  "NIX_V3_EMIT_BYTECODE"          # dump: disassemble CU to stderr/file
  "NIX_V3_EMIT_BYTECODE_OUT"      # dump: output-path for the above
  "NIX_V3_DBG_STRICTNESS_VERBOSE" # dump: per-Function strictArgs report
  "NIX_V3_FREE_LIST_STATS"        # dump: end-of-eval free-list stats (run.cc; post-eval, codegen bit-identical)
  "NIX_V3_STRINGS_ATTR"           # dump: end-of-eval allocChars attribution (run.cc; post-eval, codegen bit-identical)
)

# run_rule3 PRIMOPS CODEGEN_DIR -> prints violations; returns 0 clean,
# 1 violation(s), 2 preflight error.
run_rule3() {
  local primops="$1"; local cgdir="$2"
  if [[ ! -f "$primops" ]]; then
    echo "lint-cache-coherence Rule 3: missing $primops" >&2
    return 2
  fi
  # Extract the kGates[] array body, then the "NIX_V3_*" string tokens.
  local kgates
  kgates="$(awk '
      /kGates\[\][[:space:]]*=[[:space:]]*\{/ { f=1 }
      f { print }
      f && /\};/ { exit }
    ' "$primops" \
    | grep -oE '"NIX_V3_[A-Z0-9_]+"' | tr -d '"' | sort -u)"
  if [[ -z "$kgates" ]]; then
    echo "lint-cache-coherence Rule 3: could not parse kGates[] from $primops" >&2
    return 2
  fi
  # Collect the codegen files (missing ones are simply skipped — the
  # fixture dir only has a subset).
  local files=() f
  for f in "$cgdir/emit.cc" "$cgdir/ir.cc" "$cgdir/run.cc" "$cgdir"/opt_*.cc \
           "$cgdir/cli/lower_v3.hh" "$cgdir/lower_v3.hh"; do
    [[ -f "$f" ]] && files+=("$f")
  done
  if [[ ${#files[@]} -eq 0 ]]; then
    echo "lint-cache-coherence Rule 3: no codegen files under $cgdir" >&2
    return 2
  fi
  # Every gate READ in a codegen file.
  local found
  found="$(grep -rhoE 'getenv\("NIX_V3_[A-Z0-9_]+"\)' "${files[@]}" 2>/dev/null \
           | grep -oE 'NIX_V3_[A-Z0-9_]+' | sort -u)"
  local miss=0 gate a ok
  while IFS= read -r gate; do
    [[ -z "$gate" ]] && continue
    if grep -qxF "$gate" <<<"$kgates"; then continue; fi
    ok=0
    for a in "${rule3_allowlist[@]}"; do
      [[ "$a" == "$gate" ]] && { ok=1; break; }
    done
    [[ $ok -eq 1 ]] && continue
    if [[ $miss -eq 0 ]]; then
      echo "lint-cache-coherence FAIL (Rule 3):" >&2
      echo "  codegen env gate(s) missing from kGates[] in primops.cc" >&2
      echo "  (and not in the non-codegen allowlist):" >&2
    fi
    # Show where it is read, for the fix.
    local where
    where="$(grep -rnE "getenv\\(\"$gate\"\\)" "${files[@]}" 2>/dev/null | head -1)"
    echo "    $gate    read at: ${where:-<unknown>}" >&2
    miss=$((miss+1))
  done <<<"$found"
  if [[ $miss -gt 0 ]]; then
    cat >&2 <<EOF
  Fix: if the gate changes emitted bytecode, add it to kGates[] in
  primops.cc (codegenGateFingerprint()).  If it is a pure dump/diagnostic
  that leaves codegen bit-identical, add it to rule3_allowlist in
  this lint with a one-line justification.  See DEFECT_REVIEW §1.4.
EOF
    return 1
  fi
  return 0
}

# Self-test entry point: run only Rule 3 and exit.
if [[ -n "${CACHE_LINT_RULE3_ONLY:-}" ]]; then
  run_rule3 "$PRIMOPS_CC" "$CODEGEN_DIR"
  rc=$?
  [[ $rc -eq 0 ]] && echo "lint-cache-coherence Rule 3: OK"
  exit $rc
fi

# Preflight: every relevant file must exist.
for f in "$CLOSURE_HH" "$SERIALIZE_CC" "$SERIALIZE_HH"; do
  if [[ ! -f "$f" ]]; then
    echo "lint-cache-coherence: missing $f" >&2
    exit 2
  fi
done

if ! command -v git >/dev/null 2>&1; then
  echo "lint-cache-coherence: git not available" >&2
  exit 2
fi
cd "$ROOT" || { echo "cannot cd to repo root" >&2; exit 2; }

# Diff scope: either an explicit range (CI) or the staged set (pre-commit).
RANGE="${CACHE_LINT_RANGE:-}"
diff_cmd() {
  if [[ -n "$RANGE" ]]; then
    git diff --unified=0 "$RANGE" -- "$1"
  else
    git diff --cached --unified=0 -- "$1"
    # If nothing is staged, also consider unstaged changes — pre-commit
    # hooks typically run after `git add`, but a contributor may want to
    # eyeball-check before staging.
    if [[ -z "$(git diff --cached --name-only -- "$1")" ]]; then
      git diff --unified=0 -- "$1"
    fi
  fi
}

# Extract added/removed *content* lines (ignoring hunks, file headers,
# blanks, and lines that are pure-whitespace).  We strip the leading
# '+'/'-' so downstream callers can filter on the actual code.
diff_content_lines() {
  local file="$1"
  diff_cmd "$file" \
    | grep -E '^[+-][^+-]' \
    | grep -vE '^[+-][[:space:]]*$' \
    || true
}

# Return non-empty if the file's diff has any meaningful added/removed
# lines INSIDE the named struct/function body.  We use a coarse scope
# heuristic: capture all hunks whose header (@@) line shows a function
# context matching the named token.  False positives (a hunk whose
# context isn't actually inside the body) are tolerated — the lint is
# advisory, not load-bearing.
diff_in_body() {
  local file="$1"
  local body_token="$2"
  diff_cmd "$file" \
    | awk -v tok="$body_token" '
      /^@@/ { in_scope = (index($0, tok) > 0); next }
      in_scope && /^[+-][^+-]/ && !/^[+-][[:space:]]*$/ { print; found=1 }
      END { exit (found ? 0 : 1) }
    '
}

# Detect schema-bump indication in the same diff.  Three signals
# count as a bump:
#   1) any `+constexpr uint32_t kSchemaVersion = N;` line in serialize.hh
#   2) any `+kSchemaVersion = N` redefinition
#   3) a commit-message-style `CACHE-COHERENCE-EXEMPT: <reason>` marker
#      in the diff (rare; for refactors that don't break compatibility)
schema_bumped_or_exempt() {
  if diff_cmd "$SERIALIZE_HH" \
     | grep -qE '^\+[[:space:]]*constexpr[[:space:]]+uint32_t[[:space:]]+kSchemaVersion[[:space:]]*=[[:space:]]*[0-9]+'
  then return 0; fi
  # Exempt marker.  Two acceptance patterns:
  #   (a) commit log body contains a line starting with the marker
  #       (CI mode, range given) — the developer explicitly opted out
  #       in the commit message body.
  #   (b) a NEWLY-ADDED line in the diff that starts with `//
  #       CACHE-COHERENCE-EXEMPT:` (pre-commit mode) — placed in the
  #       commit's own diff as a self-documenting marker, e.g., above
  #       the refactor.
  # The pattern is anchored against pre-existing text containing the
  # string (e.g., this lint's own error message or doc comments that
  # mention the marker as documentation).
  if [[ -n "$RANGE" ]]; then
    if git log "$RANGE" --format=%B 2>/dev/null \
       | grep -qE '^[[:space:]]*CACHE-COHERENCE-EXEMPT:'; then
      return 0
    fi
    if git diff "$RANGE" 2>/dev/null \
       | grep -qE '^\+[[:space:]]*(//[[:space:]]*)?CACHE-COHERENCE-EXEMPT:'; then
      return 0
    fi
  else
    if git diff --cached 2>/dev/null \
       | grep -qE '^\+[[:space:]]*(//[[:space:]]*)?CACHE-COHERENCE-EXEMPT:'; then
      return 0
    fi
    if git diff 2>/dev/null \
       | grep -qE '^\+[[:space:]]*(//[[:space:]]*)?CACHE-COHERENCE-EXEMPT:'; then
      return 0
    fi
  fi
  return 1
}

violations=0
report_violation() {
  local rule="$1"; local msg="$2"
  printf 'lint-cache-coherence FAIL (%s):\n  %s\n' "$rule" "$msg" >&2
  violations=$((violations+1))
}

# ----------------------------------------------------------------------
# Rule 1: LambdaDescriptor field churn ⇒ schema bump
# ----------------------------------------------------------------------
# Detect any added/removed field-like line within the `struct
# LambdaDescriptor` body.  Heuristic: hunks whose @@ context line
# mentions `LambdaDescriptor`, AND added/removed lines that look like
# field declarations (a type token + identifier + `;`).
#
# Pair `+`/`-` lines and strip the trailing `// comment` portion before
# comparing.  Lines that differ ONLY in the trailing comment (rephrasing,
# adding context) are NOT considered field churn — the binary layout
# is unaffected.  Pairing keeps Rule 1 actionable (false positives on
# pure comment edits within a field line block were causing churn).
ld_field_pattern='^[+-]([[:space:]]*(uint[0-9]+_t|int[0-9]+_t|bool|char|std::string|std::vector|mutable|uint8_t|uint16_t|uint32_t|uint64_t|int8_t|int16_t|int32_t|int64_t|size_t|PosIdx32|SymbolId|const)[[:space:]]+[A-Za-z_][A-Za-z0-9_]*[[:space:]]*(=[^;]+)?;)|(^[+-][[:space:]]*[A-Za-z_][A-Za-z0-9_]*[[:space:]]*\{[^;]*\}[[:space:]]*;)'

# Strip leading `+`/`-` and the trailing `// ...` (preserving the
# code portion of the field declaration).  The trailing comment is
# format-irrelevant: it doesn't change the on-disk byte layout.
strip_code_portion() {
  # `sed` is portable enough; awk would also do.
  sed -E 's|^[+-]||; s|//.*$||; s|[[:space:]]+$||'
}

# Filter `ld_field_pattern`-matching lines and ELIMINATE pairs that
# differ only in trailing comments.  Implemented in awk: bucket each
# pair by their code-portion, count `+`/`-`; a balanced pair (one `-`,
# one `+` with the same code portion) is a comment-only change and
# must NOT fire Rule 1.
ld_diff_raw="$(diff_in_body "$CLOSURE_HH" "LambdaDescriptor" \
                | grep -E "$ld_field_pattern" || true)"

ld_diff_real=""
if [[ -n "$ld_diff_raw" ]]; then
  ld_diff_real="$(printf '%s\n' "$ld_diff_raw" \
    | awk '
      {
        sign = substr($0, 1, 1)
        rest = $0
        sub(/^[+-]/, "", rest)
        # Strip trailing `// ...` comment + trailing whitespace.
        sub(/\/\/.*$/, "", rest)
        sub(/[[:space:]]+$/, "", rest)
        key = rest
        if (sign == "+") plus[key]++
        else if (sign == "-") minus[key]++
        order[NR] = $0
        idx[NR] = key
        sign_of[NR] = sign
      }
      END {
        for (i = 1; i <= NR; ++i) {
          k = idx[i]
          # Comment-only change: code portion exists on BOTH sides.
          if (plus[k] > 0 && minus[k] > 0) continue
          print order[i]
        }
      }
    ')"
fi

if [[ -n "$ld_diff_real" ]]; then
  if ! schema_bumped_or_exempt; then
    report_violation "Rule 1" \
      "LambdaDescriptor body in closure.hh has added/removed field-like lines but the same diff does not bump kSchemaVersion in serialize.hh. If this is a pure refactor with no on-disk format change, add 'CACHE-COHERENCE-EXEMPT: <reason>' to the commit message body."
    echo "  Offending lines (sample):" >&2
    echo "$ld_diff_real" | head -5 | sed 's/^/    /' >&2
  fi
fi

# ----------------------------------------------------------------------
# Rule 2: deserialiseCU body interpretation change ⇒ schema bump
# ----------------------------------------------------------------------
# Heuristic: hunks whose @@ context mentions `deserializeCU`, AND any
# added/removed non-comment non-whitespace line.
deser_diff="$(diff_in_body "$SERIALIZE_CC" "deserializeCU" \
              | grep -vE '^[+-][[:space:]]*//' || true)"

if [[ -n "$deser_diff" ]]; then
  if ! schema_bumped_or_exempt; then
    report_violation "Rule 2" \
      "deserializeCU body in serialize.cc has added/removed lines but the same diff does not bump kSchemaVersion in serialize.hh. If the change is a pure refactor (e.g., variable rename, inline a helper) with bit-identical interpretation of existing bytes, add 'CACHE-COHERENCE-EXEMPT: <reason>' to the commit message body."
    echo "  Offending lines (sample):" >&2
    echo "$deser_diff" | head -5 | sed 's/^/    /' >&2
  fi
fi

# ----------------------------------------------------------------------
# Rule 3: codegen gate ⇒ present in kGates[] (full-tree invariant)
# ----------------------------------------------------------------------
run_rule3 "$PRIMOPS_CC" "$CODEGEN_DIR"
case $? in
  0) : ;;
  1) violations=$((violations+1)) ;;
  2) echo "lint-cache-coherence: Rule 3 preflight failed" >&2; exit 2 ;;
esac

# ----------------------------------------------------------------------
# Summary
# ----------------------------------------------------------------------
if [[ $violations -gt 0 ]]; then
  echo "" >&2
  echo "$violations lint violation(s).  See docs/lode/NEXT_STEPS_2026-05-25.md §2.6 + §A4 for rationale." >&2
  exit 1
fi

# Friendly success message only when there's relevant diff to lint
# (else stay silent to keep batch runs quiet).
relevant_changed=0
for f in "$CLOSURE_HH" "$SERIALIZE_CC" "$SERIALIZE_HH"; do
  if [[ -n "$(diff_cmd "$f" 2>/dev/null)" ]]; then
    relevant_changed=1; break
  fi
done
if [[ $relevant_changed -eq 1 ]]; then
  echo "lint-cache-coherence: OK"
fi
exit 0
