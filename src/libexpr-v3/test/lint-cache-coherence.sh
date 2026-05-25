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
  # Any explicit "CACHE-COHERENCE-EXEMPT" marker anywhere in the diff
  # (rare; refactor exemption).  We look in the entire diff, not just
  # serialize.hh, since the marker would normally live in the commit
  # message — but pre-commit hooks don't see the message yet, so we
  # accept it inline as well (in a // comment line, for instance).
  if [[ -n "$RANGE" ]]; then
    git log "$RANGE" --format=%B 2>/dev/null | grep -q 'CACHE-COHERENCE-EXEMPT:' && return 0
    git diff "$RANGE" 2>/dev/null | grep -q 'CACHE-COHERENCE-EXEMPT:' && return 0
  else
    git diff --cached 2>/dev/null | grep -q 'CACHE-COHERENCE-EXEMPT:' && return 0
    git diff         2>/dev/null | grep -q 'CACHE-COHERENCE-EXEMPT:' && return 0
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
ld_field_pattern='^[+-]([[:space:]]*(uint[0-9]+_t|int[0-9]+_t|bool|char|std::string|std::vector|mutable|uint8_t|uint16_t|uint32_t|uint64_t|int8_t|int16_t|int32_t|int64_t|size_t|PosIdx32|SymbolId|const)[[:space:]]+[A-Za-z_][A-Za-z0-9_]*[[:space:]]*(=[^;]+)?;)|(^[+-][[:space:]]*[A-Za-z_][A-Za-z0-9_]*[[:space:]]*\{[^;]*\}[[:space:]]*;)'

ld_diff="$(diff_in_body "$CLOSURE_HH" "LambdaDescriptor" \
            | grep -E "$ld_field_pattern" || true)"

if [[ -n "$ld_diff" ]]; then
  if ! schema_bumped_or_exempt; then
    report_violation "Rule 1" \
      "LambdaDescriptor body in closure.hh has added/removed field-like lines but the same diff does not bump kSchemaVersion in serialize.hh. If this is a pure refactor with no on-disk format change, add 'CACHE-COHERENCE-EXEMPT: <reason>' to the commit message body."
    echo "  Offending lines (sample):" >&2
    echo "$ld_diff" | head -5 | sed 's/^/    /' >&2
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
