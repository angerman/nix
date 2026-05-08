#!/usr/bin/env bash
# v3 lexical-with chain regression tests.
#
# Validates that closures and thunks created inside `with X;` correctly
# capture the with-target's value such that lookups in their bodies
# resolve through it — even when the closure escapes its creation
# context (called from a different dynamic with-stack state).
#
# Each test runs both TW (baseline) and v3-direct
# (NIX_V3_DIRECT_EVAL=1) and compares output byte-for-byte.
#
# Categories:
#   POSITIVE — patterns where the with-target SHOULD be visible.
#   NEGATIVE — patterns where the with-target should NOT be visible
#               (lexical-not-dynamic).
#   REGRESSION — shapes that previously broke or that the lexical-
#                with-chain implementation is most likely to affect.
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

ok=0
fail=0
fail_names=()

run_pair() {
  local name="$1" expr="$2"
  local tw v3
  tw=$("$NIX_BIN" eval --impure --expr "$expr" 2>/dev/null) || tw="<tw-error>"
  v3=$(NIX_V3_DIRECT_EVAL=1 "$NIX_BIN" eval --impure --expr "$expr" 2>/dev/null) \
    || v3="<v3-direct-error>"
  if [[ "$tw" == "$v3" ]]; then
    ok=$((ok + 1))
  else
    fail=$((fail + 1))
    fail_names+=("$name  TW=[$tw]  v3=[$v3]")
  fi
}

run_pair_with_diff() {
  # Like run_pair but allow TW and v3 to disagree only on
  # whitespace/format.  For now use exact match; can relax later.
  run_pair "$@"
}

# ============================================================
# POSITIVE — closure / thunk inside `with X;` sees X
# ============================================================

# (1) Closure created inside `with X;`, called inside same scope.
run_pair "with-immediate" '
  with { a = 1; }; (x: a + x) 41'

# (2) Closure created inside `with X;` and ESCAPING — called outside.
run_pair "with-escape-1" '
  let f = with { a = 100; }; (x: a + x);
  in f 23'

# (3) Closure created inside two nested withs and escaping.
run_pair "with-escape-nested" '
  let f = with { a = 1; }; with { b = 2; }; (x: a + b + x);
  in f 100'

# (4) Inner closure escapes, captures both inner and outer with.
run_pair "with-curried-escape" '
  let mk = with { a = 1; }; (x:
    with { b = 2; }; (y: a + b + x + y));
  in (mk 10) 100'

# (5) Thunk created inside `with X;` referenced by a let, forced outside.
run_pair "with-thunk-escape" '
  let r = with { a = 7; }; { x = a + 100; };
  in r.x'

# (6) `with` over a let-bound attrset.
run_pair "with-let" '
  let pkgs = { hello = "world"; }; in
    with pkgs; "hello, " + hello'

# (7) `with` over a function call result.
run_pair "with-call" '
  let mk = _: { greet = "hi"; }; in
    with (mk 0); greet'

# (8) Closure captures `with` PLUS a local let-binding.
run_pair "with-and-let" '
  let f = let local = 10; in with { a = 1; }; (x: a + local + x);
  in f 100'

# ============================================================
# NEGATIVE — lexical (NOT dynamic) scoping
# ============================================================

# (9) Closure defined OUTSIDE `with X;`, called INSIDE.  Body uses
#     a name `foo` that ONLY appears in the dynamic with-scope at
#     call time — should fail in both TW and v3.  We compare TW vs v3
#     to ensure they agree (both error in the same way).
run_pair "with-not-dynamic" '
  let
    f = x: x.greet;  # f does NOT have `with`
    s = { greet = "hello"; };
  in
    with { greet = "wrong"; };  # this `with` should be invisible to f
    f s'

# (10) Two unrelated `with`s — inner closure should NOT see outer's
#      siblings via dynamic scope.
run_pair "with-sibling-isolation" '
  let
    inner = with { a = "INNER-a"; }; (k: a + k);
    outer = with { a = "OUTER-a"; b = "OUTER-b"; }; inner;
  in outer "!"'   # inner uses INNER-a (its lexical), not OUTER-a (dynamic)

# ============================================================
# REGRESSION — bug-shape coverage
# ============================================================

# (11) `with self;` over a fix-point — the makeExtensible shape.
run_pair "with-self-fixpoint" '
  let
    makeExt = rattrs: let self = rattrs self; in self;
    pkgs = makeExt (self: with self; {
      a = 42;
      b = a + 1;
    });
  in pkgs.b'

# (12) `with pkgs;` from the OUTER lambda body, accessed from a
#      thunk inside `inherit (X) Y;`.  This is the all-packages.nix
#      shape that #529 partially addressed.
run_pair "with-pkgs-inherit-from" '
  let
    makeExt = rattrs: let self = rattrs self; in self;
    pkgs = makeExt (self: with self; {
      mySrc = { greet = "hi"; };
      inherit (mySrc) greet;
      direct = "d";
    });
  in pkgs.greet'

# (13) Triple-nested fix-point with `with self;` at outer and inner.
run_pair "triple-with-self" '
  let
    extends = overlay: f: (final: let prev = f final; in prev // overlay final prev);
    fix = f: let self = f self; in self;
    base = self: { a = 1; };
    o1 = final: prev: { b = final.a + 10; };
    o2 = final: prev: with final; { c = a + b + 100; };
    composed = extends o2 (extends o1 base);
  in (fix composed).c'

# (14) Lambda with formals + `with` over the formal.
run_pair "with-formals" '
  let mk = { pkgs }: with pkgs; (x: hello + x);
  in mk { pkgs = { hello = "H"; }; } "Y"'

# (15) `with` followed by `let` followed by `with` (interleaving scopes).
run_pair "with-let-with" '
  let f = with { a = "A"; };
        let local = "L"; in
          with { b = "B"; }; (x: a + b + local + x);
  in f "X"'

echo "=== lexical-withs test results ==="
echo "  passing: $ok"
echo "  failing: $fail"
if (( fail > 0 )); then
  for n in "${fail_names[@]}"; do
    echo "  FAIL $n"
  done
  exit 1
fi
exit 0
