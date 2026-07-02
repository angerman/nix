#!/usr/bin/env bash
# v3 warm-disk-cache OP_ATTRS_INIT round-trip guardrail.
#
# WHY THIS EXISTS (P3.3 / audit §3.4, 2026-07-02): an attempt to move the
# OP_ATTRS_INIT entry sort from runtime to emit time (flagging the operand +
# skipping the VM's std::sort) introduced a warm-cache CORRUPTION that the
# then-existing brute battery did not catch — the serialize collect/remap
# walkers read the OP_ATTRS_INIT operand as a raw count and overshot the 2n-word
# (name,pos) trailer once bit 23 was set, and, more subtly, a pre-sorted trailer
# is NOT order-preserved across the cross-process SymbolId remap (only the
# slot-indexed OP_ATTRS_REC_INIT is, via its REC_SET slot rewrite).  The attempt
# was reverted; the RUNTIME sort is load-bearing.  This test locks in the
# invariant: a non-rec attrset literal imported through the disk cache must
# round-trip byte-identically to the no-cache path across TWO separate
# (cross-process) v3-eval invocations.  It would fail loudly if the pre-sort
# lever were ever re-introduced without also re-sorting the trailer after remap.
#
# Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
#   Input Output Group.  SPDX-License-Identifier: Apache-2.0
set -u

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
V3="${V3:-$ROOT/build/src/libexpr-v3/v3-eval}"
if [[ ! -x "$V3" ]]; then echo "attrs-init-cache: v3-eval not at $V3" >&2; exit 2; fi

pass=0; fail=0; failed=()

WORK="$(mktemp -d "${TMPDIR:-/tmp}/v3-attrs-init-cache.XXXXXX")"
trap 'rm -rf "$WORK"' EXIT

# Each case: a module body (a non-rec attrset literal) + a driver expression
# selecting from it + the expected result.  The module keys are deliberately in
# NON-sorted textual order to exercise the sort/trailer mapping.
run_case() {
  local name="$1" modbody="$2" driver="$3" want="$4"
  local dir="$WORK/$name"; mkdir -p "$dir"
  printf '%s\n' "$modbody" > "$dir/mod.nix"
  printf '%s\n' "$driver"  > "$dir/main.nix"
  local cache="$dir/cache"

  # No-cache oracle (disk cache disabled).
  local oracle
  oracle=$(cd "$dir" && NIX_V3_NO_DISK_CACHE=1 NIX_V3_DIRECT_EVAL=1 "$V3" --file ./main.nix 2>/dev/null | tail -1)
  # COLD: fresh cache dir → writes the CU to disk.
  local cold
  cold=$(cd "$dir" && NIX_V3_CACHE_DIR="$cache" NIX_V3_DIRECT_EVAL=1 "$V3" --file ./main.nix 2>/dev/null | tail -1)
  # WARM: second, separate process → reads the CU back (collect/remap walk).
  local warmout warm
  warmout=$(cd "$dir" && NIX_V3_CACHE_DIR="$cache" NIX_V3_DIRECT_EVAL=1 "$V3" --file ./main.nix 2>&1)
  warm=$(printf '%s' "$warmout" | tail -1)

  if [[ "$warmout" == *"unhandled opcode"* ]]; then
    fail=$((fail+1)); failed+=("$name WARM CORRUPTED: $(printf '%s' "$warmout" | tail -1)")
    return
  fi
  if [[ "$oracle" == "$want" && "$cold" == "$want" && "$warm" == "$want" ]]; then
    pass=$((pass+1))
  else
    fail=$((fail+1))
    failed+=("$name want=$want oracle=$oracle cold=$cold warm=$warm")
  fi
}

# 1. Out-of-order keys, select the last-sorted key.
run_case outoforder '{ z = 1; a = 2; m = 3; b = 4; }' '(import ./mod.nix).z + (import ./mod.nix).b * 10' 41
# 2. attrNames order must be sorted after a warm load.
run_case attrnames '{ z = 1; a = 2; b = 3; }' 'builtins.concatStringsSep "," (builtins.attrNames (import ./mod.nix))' '"a,b,z"'
# 3. >16 entries: exercises the heap-fallback path in OP_ATTRS_INIT.
run_case heappath \
  '{ e=5;d=4;c=3;b=2;a=1;j=10;i=9;h=8;g=7;f=6;o=15;n=14;m=13;l=12;k=11;t=20;s=19;r=18;q=17;p=16; }' \
  'let s = import ./mod.nix; in s.a + s.t + s.k + s.p' 48
# 4. Nested non-rec attrsets round-trip.
run_case nested '{ outer = { y = 2; x = 1; }; z = 9; }' '(import ./mod.nix).outer.x + (import ./mod.nix).z' 10
# 5. Lazy value in an unused attr must not be forced on warm load.
run_case lazy '{ good = 42; bad = throw "boom"; }' '(import ./mod.nix).good' 42

echo "attrs-init-cache: pass=$pass fail=$fail"
if (( fail > 0 )); then printf '  FAIL %s\n' "${failed[@]}" >&2; exit 1; fi
exit 0
