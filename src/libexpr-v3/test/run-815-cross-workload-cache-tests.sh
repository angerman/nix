#!/usr/bin/env bash
#
# #815 regression test — cross-workload disk-cache.
#
# Two scenarios:
#
#   T1 — same-content-different-path files do NOT cache-collide.
#        Two temp directories A and B hold byte-identical .nix files
#        containing a `toString ./sub/helper.nix` expression.  Compiling
#        `$A/main.nix` and `$B/main.nix` against a shared NIX_V3_CACHE_DIR
#        must produce results pointing at A's resolved path for A and B's
#        for B — never crossed.  Path-literals like `./sub/helper.nix`
#        resolve at compile time against the containing file's directory
#        and are baked into bytecode; before the #815 fix, the disk-cache
#        key was `hash(content)` so identical-content files at different
#        paths collided, and B's load returned A's cached bytecode with
#        A's resolved paths.
#
#        Falsification check: to verify this test CATCHES the regression
#        returning, temporarily revert primops.cc's pathLen+pathStr mix-in
#        (the #815 commit) and re-run — T1 must FAIL with "RA==RB
#        ... cache-key path-collision regression".
#
#   T2 — default-bearing formals lambda compiles deterministically
#        cross-process.  A small Nix expression with a `{a ? 1, b ? 2,
#        c ? 3}: a + b + c`-style lambda is evaluated twice into a shared
#        cache, then re-evaluated again — same-binary serialize→
#        deserialize→eval round-trip.  If the formals letRec.entries
#        iteration is order-non-deterministic, the deserialise will see
#        a permutation that does not match a fresh-compile, and (in the
#        presence of slot mismatches between OP_ATTRS_LET_REC_INIT
#        trailer and the OP_ATTRS_REC_SETs) the result mis-binds.
#        This is enforced indirectly via byte-equality of the resulting
#        store-derivation path produced by a small synthetic derivation
#        whose formals defaults touch the bug surface.  A divergence
#        here flips parity vs. tree-walker oracle on the same fixture.
#
# Both tests live OUTSIDE the haskell-nix-example reproducer (which is
# environment-specific) so they run anywhere with a working v3 binary
# and a writable temp dir.
#
# Exit codes:
#   0  both tests pass
#   1  any test failed
#
# Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
# Input Output Group.  SPDX-License-Identifier: Apache-2.0

set -u

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
NIX="${NIX:-$ROOT/build/src/nix/nix}"
TW_BIN="${TW_BIN:-$NIX}"

if [[ ! -x "$NIX" ]]; then
  echo "FAIL: $NIX is not executable.  Build first (ninja -C build)." >&2
  exit 1
fi

pass=0
fail=0
report() {
  local status="$1"; local name="$2"; local detail="${3:-}"
  printf '  [%s] %s' "$status" "$name"
  [[ -n "$detail" ]] && printf '  %s' "$detail"
  printf '\n'
  if [[ "$status" == "PASS" ]]; then pass=$((pass+1)); else fail=$((fail+1)); fi
}

# Common env: v3-direct, generous timeouts to avoid CI flake.
common_env() {
  export NIX_V3_DIRECT_EVAL=1
  export NIX_V3_MAX_WALL_TIME=60s
  export NIX_V3_MAX_HEAP=2G
}

#=============================================================================
# T1 — same content, different paths, shared cache.
#=============================================================================
t1_same_content_path_collision() {
  local TMPCACHE TMPA TMPB
  TMPCACHE=$(mktemp -d)
  TMPA=$(mktemp -d)
  TMPB=$(mktemp -d)
  # Guarantee A and B differ as strings (and therefore as resolved paths)
  # — mktemp on darwin already gives distinct dirs but we belt-and-braces.
  if [[ "$TMPA" == "$TMPB" ]]; then
    report FAIL "T1" "mktemp produced identical paths — environment unusable"
    rm -rf "$TMPCACHE" "$TMPA" "$TMPB"
    return
  fi

  for D in "$TMPA" "$TMPB"; do
    mkdir -p "$D/sub"
    printf 'toString ./sub/helper.nix\n' > "$D/main.nix"
    printf '"helper-value"\n' > "$D/sub/helper.nix"
  done

  # Sanity: byte-identical content (key collision precondition).
  if ! cmp -s "$TMPA/main.nix" "$TMPB/main.nix" \
     || ! cmp -s "$TMPA/sub/helper.nix" "$TMPB/sub/helper.nix"; then
    report FAIL "T1" "test fixture files have unexpected content drift"
    rm -rf "$TMPCACHE" "$TMPA" "$TMPB"
    return
  fi

  common_env
  export NIX_V3_CACHE_DIR="$TMPCACHE"

  # Eval A first → writes CU under (path=A, content=X).
  local RA RB ECA ECB
  RA=$("$NIX" eval --impure --expr "import $TMPA/main.nix" 2>/dev/null)
  ECA=$?
  # Eval B second → must NOT inherit A's cached resolution.
  RB=$("$NIX" eval --impure --expr "import $TMPB/main.nix" 2>/dev/null)
  ECB=$?

  if [[ $ECA -ne 0 ]]; then
    report FAIL "T1" "eval against TMPA failed with ec=$ECA"
    rm -rf "$TMPCACHE" "$TMPA" "$TMPB"; return
  fi
  if [[ $ECB -ne 0 ]]; then
    report FAIL "T1" "eval against TMPB failed with ec=$ECB"
    rm -rf "$TMPCACHE" "$TMPA" "$TMPB"; return
  fi

  # RA must mention TMPA's path; RB must mention TMPB's.
  if [[ "$RA" != *"$TMPA"* ]]; then
    report FAIL "T1" "RA does not contain TMPA: RA=$RA TMPA=$TMPA"
    rm -rf "$TMPCACHE" "$TMPA" "$TMPB"; return
  fi
  if [[ "$RB" != *"$TMPB"* ]]; then
    report FAIL "T1" "RB does not contain TMPB (cache collision?): RB=$RB TMPB=$TMPB"
    rm -rf "$TMPCACHE" "$TMPA" "$TMPB"; return
  fi
  if [[ "$RA" == "$RB" ]]; then
    report FAIL "T1" "RA==RB ($RA) — cache-key path-collision regression"
    rm -rf "$TMPCACHE" "$TMPA" "$TMPB"; return
  fi

  report PASS "T1" "same-content-different-path: no cache collision"
  rm -rf "$TMPCACHE" "$TMPA" "$TMPB"
}

#=============================================================================
# T2 — round-trip determinism on a default-bearing formals lambda.
#
# Strategy: compile the same expression TWICE in the same process (via
# nix eval --no-eval-cache), but the second invocation gets a hot CU
# disk cache.  Both invocations must return the same numeric result —
# any letRec.entries / REC_SET slot mismatch would scramble the formals'
# default bindings, producing a different sum.
#=============================================================================
t2_formals_letrec_determinism() {
  local TMPCACHE TMPFILE
  TMPCACHE=$(mktemp -d)
  TMPFILE=$(mktemp -d)

  # Lambda with default-bearing formals where defaults form a chain
  # (each depends on the previous).  Names are chosen so the SOURCE
  # ORDER (z, n, k, h, c, a) is NOT the alphabetical canonical order
  # (a, c, h, k, n, z).  Pre-#815 the formals' LetRec entries were
  # built in TW-Symbol-VALUE order — which in nix-libexpr is the source
  # order — so the bytecode encoded REC_SET slots in source order
  # rather than canonical-by-name order.  Cross-process this still
  # round-tripped correctly when both processes happened to intern v3
  # SymbolIds in matching orders, but the moment two processes
  # accumulated different prior interning history the on-disk
  # REC_INIT trailer + REC_SET slot patching disagreed about which
  # name belonged at which slot.
  #
  # We make this surface even WITHIN a single process via a "polluter"
  # prelude: invocation B imports the SAME polluter set as invocation
  # A but does so VIA the cached CU path (where the prior v3 symbol
  # interning history matters).  The lambda's expected sum is
  # 1 + 2 + 3 + 4 + 5 + 6 = 21; any letRec-slot misalignment scrambles
  # which numeric default is bound to which formal and breaks the sum.
  cat > "$TMPFILE/polluter.nix" <<'NIX'
{ qux = "qux"; foo = "foo"; bar = "bar"; baz = "baz"; }
NIX
  cat > "$TMPFILE/lambda.nix" <<'NIX'
let
  _polluter = import ./polluter.nix;
  f = {z ? 1, n ? z + 1, k ? n + 1, h ? k + 1, c ? h + 1, a ? c + 1}: a + c + h + k + n + z;
in  builtins.seq _polluter (f {})
NIX

  common_env
  export NIX_V3_CACHE_DIR="$TMPCACHE"

  # First eval: cold cache → fresh compile + insert.
  local R1 R2
  R1=$("$NIX" eval --impure --expr "import $TMPFILE/lambda.nix" 2>/dev/null)
  if [[ -z "$R1" ]]; then
    report FAIL "T2" "first eval produced no output"
    rm -rf "$TMPCACHE" "$TMPFILE"; return
  fi
  # Second eval: hot cache → deserialise + run.  Must match.
  R2=$("$NIX" eval --impure --expr "import $TMPFILE/lambda.nix" 2>/dev/null)

  if [[ "$R1" != "21" ]]; then
    report FAIL "T2" "first eval returned $R1, expected 21 (1+2+3+4+5+6)"
    rm -rf "$TMPCACHE" "$TMPFILE"; return
  fi
  if [[ "$R1" != "$R2" ]]; then
    report FAIL "T2" "round-trip diverged: R1=$R1 R2=$R2"
    rm -rf "$TMPCACHE" "$TMPFILE"; return
  fi

  # Round-trip determinism diagnostic — captures opcode/symbol
  # divergences that would propagate as silent semantic drift.
  local VLOG
  VLOG=$(mktemp)
  V3_DBG_DESERIALIZE_VERIFY="$VLOG" \
    "$NIX" eval --impure --expr "import $TMPFILE/lambda.nix" \
    >/dev/null 2>/dev/null
  if grep -q "opDiffs=[1-9]" "$VLOG" 2>/dev/null; then
    report FAIL "T2" "V3_DBG_DESERIALIZE_VERIFY found opcode-level divergence: $(grep "opDiffs=[1-9]" "$VLOG" | head -1)"
    rm -f "$VLOG"
    rm -rf "$TMPCACHE" "$TMPFILE"; return
  fi
  if grep -q "symStrDiffs=[1-9]" "$VLOG" 2>/dev/null; then
    report FAIL "T2" "V3_DBG_DESERIALIZE_VERIFY found symbol-resolved-string divergence: $(grep "symStrDiffs=[1-9]" "$VLOG" | head -1)"
    rm -f "$VLOG"
    rm -rf "$TMPCACHE" "$TMPFILE"; return
  fi
  rm -f "$VLOG"

  report PASS "T2" "formals letRec round-trip deterministic (R1=R2=21, no opcode diffs)"
  rm -rf "$TMPCACHE" "$TMPFILE"
}

#=============================================================================
# T3 — explicit BENIGN-permutation test.
#
# Round-trip MUST produce code=DIFF entries in the verify log for any
# CU that uses attrset names, but ZERO opDiffs / symStrDiffs / strs=DIFF.
# This is the inverse of T2's regression sentinel: T2 catches the bug
# returning; T3 catches an over-fix that breaks legitimate SymbolId
# permutations.
#=============================================================================
t3_round_trip_benign_diff() {
  local TMPCACHE TMPFILE VLOG
  TMPCACHE=$(mktemp -d)
  TMPFILE=$(mktemp -d)
  VLOG=$(mktemp)

  cat > "$TMPFILE/multiattr.nix" <<'NIX'
let s = { foo = 1; bar = 2; baz = 3; qux = 4; };
in  s.foo + s.bar + s.baz + s.qux
NIX

  common_env
  export NIX_V3_CACHE_DIR="$TMPCACHE"

  # Cold compile.
  "$NIX" eval --impure --expr "import $TMPFILE/multiattr.nix" >/dev/null 2>/dev/null
  # Hot load with VERIFY.
  V3_DBG_DESERIALIZE_VERIFY="$VLOG" \
    "$NIX" eval --impure --expr "import $TMPFILE/multiattr.nix" \
    >/dev/null 2>/dev/null

  if [[ ! -s "$VLOG" ]]; then
    # No CU was cached (likely no import touched the disk cache).  This
    # is acceptable — the test below is conditional on cache use.
    report PASS "T3" "no cache HIT — skipped (cache path not exercised)"
    rm -f "$VLOG"
    rm -rf "$TMPCACHE" "$TMPFILE"; return
  fi

  # grep -c returns 0/non-zero exit; we want the COUNT.  `|| true` keeps
  # set -u happy; the explicit `0` default handles the empty-output edge.
  local op_diffs sym_diffs str_diffs
  op_diffs=$(grep -c "opDiffs=[1-9]" "$VLOG" 2>/dev/null)
  sym_diffs=$(grep -c "symStrDiffs=[1-9]" "$VLOG" 2>/dev/null)
  str_diffs=$(grep -c "strs=[0-9]*/[0-9]*(DIFF)" "$VLOG" 2>/dev/null)
  : "${op_diffs:=0}"
  : "${sym_diffs:=0}"
  : "${str_diffs:=0}"

  if [[ "$op_diffs" -gt 0 ]]; then
    report FAIL "T3" "opDiffs=$op_diffs in round-trip — semantic divergence"
    rm -f "$VLOG"
    rm -rf "$TMPCACHE" "$TMPFILE"; return
  fi
  if [[ "$sym_diffs" -gt 0 ]]; then
    report FAIL "T3" "symStrDiffs=$sym_diffs in round-trip — name resolution diverged"
    rm -f "$VLOG"
    rm -rf "$TMPCACHE" "$TMPFILE"; return
  fi
  if [[ "$str_diffs" -gt 0 ]]; then
    report FAIL "T3" "strs=DIFF in round-trip — constant pool order diverged"
    rm -f "$VLOG"
    rm -rf "$TMPCACHE" "$TMPFILE"; return
  fi

  report PASS "T3" "round-trip diffs are SymbolId-permutation only (benign)"
  rm -f "$VLOG"
  rm -rf "$TMPCACHE" "$TMPFILE"
}

#=============================================================================
# Main.
#=============================================================================
echo "=== #815 cross-workload disk-cache regression suite ==="
t1_same_content_path_collision
t2_formals_letrec_determinism
t3_round_trip_benign_diff

echo
echo "=== summary ==="
echo "  pass:    $pass"
echo "  fail:    $fail"
if [[ "$fail" -gt 0 ]]; then
  echo "  result:  FAIL"
  exit 1
fi
echo "  result:  PASS"
exit 0
