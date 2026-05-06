#!/usr/bin/env bash
# REVIEW B3 — regression test for mutually-circular formal defaults.
#
# Per USAGE.md:292, v3 reads sibling slots eagerly when lowering
# formal defaults of the shape `{ a ? b, b ? a }: ...`.  Tree-walker
# uses per-default thunks so the unforced side stays lazy.  When only
# ONE side is provided at the call site, TW resolves correctly while
# v3 may diverge (eager read of the unprovided sibling).
#
# Documented limitation; this test exists so the divergence is
# OBSERVED (and remains observable) rather than silently flipping.
# Forward / backward refs (`b ? a + 1`, `a ? b - 1`) DO work.
#
# What we test:
#   p1 forward ref:  `{ a, b ? a + 1 }: { a = a; b = b; }` called {a=10;}
#   p2 backward ref: `{ a ? b - 1, b }: { a = a; b = b; }` called {b=10;}
#   p3 mutual-with-call-side-fill: `{ a ? b, b ? a }: a + b` called {a=5;b=5;}
#       — both sides provided, no default consulted.  Must succeed.
#   p4 mutual-with-only-a:  `{ a ? b, b ? a }: a + b` called {a=5;}
#       — TW: error or recurse (b's default reads a's default which
#       reads b's default ...).  v3: same (forces both eagerly).
#       Both should error in a consistent way; the test asserts that
#       v3's behavior is at parity with TW (both error, both produce
#       same value, etc.).
#
# Per the testing rules (CLAUDE.md), this is a regression test that
# observes current behavior; it doesn't FORCE v3 to match TW where
# they intentionally diverge.  If a future change unifies them
# (proper per-default thunkification), the test will need updating.
#
# Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.
# SPDX-License-Identifier: Apache-2.0

set -u

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
NIX_BIN="${NIX_BIN:-$ROOT/build/src/nix/nix}"

if [[ ! -x "$NIX_BIN" ]]; then
  echo "nix not found at $NIX_BIN" >&2
  exit 1
fi

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

# p1 — forward ref: b's default refers to a (provided).  Both engines
# should produce the result without error.
cat > "$TMP/p1.nix" <<'EOF'
let f = { a, b ? a + 1 }: a * 100 + b;
in f { a = 10; }
EOF
EXP_P1='1011'

# p2 — backward ref: a's default refers to b (provided).
cat > "$TMP/p2.nix" <<'EOF'
let f = { a ? b - 1, b }: a * 100 + b;
in f { b = 10; }
EOF
EXP_P2='910'

# p3 — mutually-circular but BOTH provided: defaults are not consulted,
# so the eager-vs-lazy discrepancy is irrelevant.
cat > "$TMP/p3.nix" <<'EOF'
let f = { a ? b, b ? a }: a + b;
in f { a = 5; b = 5; }
EOF
EXP_P3='10'

# p4 — only a provided; b's default refers to a (which IS provided).
# This exercises the "only-one-side" case but b's default is reachable
# (a is in scope).  TW: 5 + 5 = 10.  v3 should match because b's
# default just reads a (provided slot, not a default-only sibling).
cat > "$TMP/p4.nix" <<'EOF'
let f = { a ? b, b ? a }: a + b;
in f { a = 5; }
EOF
EXP_P4='10'

# p5 — only b provided.  Symmetric to p4.
cat > "$TMP/p5.nix" <<'EOF'
let f = { a ? b, b ? a }: a + b;
in f { b = 7; }
EOF
EXP_P5='14'

ok=0
fail=0
fail_names=()

run_one() {
  local label="$1"; shift
  local nix_file="$1"; shift
  local expected="$1"; shift
  local extra_env=("$@")
  local got
  got=$(env "${extra_env[@]}" "$NIX_BIN" eval --no-eval-cache -f "$nix_file" 2>/dev/null) \
    || got="<error>"
  if [[ "$got" == "$expected" ]]; then
    ok=$((ok + 1))
  else
    fail=$((fail + 1))
    fail_names+=("$label  expected=$expected  got=$got")
  fi
}

# Modes: TW vs v3.  Both should produce the same result for the
# documented-working shapes.  If v3 diverges, the test fails so the
# divergence is visible (not silent).
for spec in "tw::" "v3::NIX_USE_V3=1"; do
  IFS=:: read -r tag _ envspec <<< "$spec"
  IFS=' ' read -ra envarr <<< "$envspec"
  run_one "$tag/p1-forward-ref"  "$TMP/p1.nix" "$EXP_P1" "${envarr[@]}"
  run_one "$tag/p2-backward-ref" "$TMP/p2.nix" "$EXP_P2" "${envarr[@]}"
  run_one "$tag/p3-mutual-both"  "$TMP/p3.nix" "$EXP_P3" "${envarr[@]}"
  run_one "$tag/p4-mutual-a"     "$TMP/p4.nix" "$EXP_P4" "${envarr[@]}"
  run_one "$tag/p5-mutual-b"     "$TMP/p5.nix" "$EXP_P5" "${envarr[@]}"
done

echo "=== mutual-circular formal defaults tests: ok=$ok fail=$fail ==="
for n in "${fail_names[@]}"; do
  echo "  FAIL $n"
done
[[ $fail -eq 0 ]] || exit 1
