#!/usr/bin/env bash
# v3 disk-cache regression tests (CRIT-1).
#
# The disk-cache only fires through `primImport`, never through
# --expr.  We exercise the warm-load remap by:
#
#   1. Writing the test expression to a per-test temp file.
#   2. Running `import file.nix` in a process that interns one set
#      of "noise" symbols BEFORE the import resolves — this is the
#      "cold" run; the cache is empty, the file is parsed + lowered
#      + serialized to disk.
#   3. Running `import file.nix` in a fresh process whose noise is
#      DIFFERENT (different number of pre-imported attr names) —
#      different intern order means the second process's global
#      symbol-table assigns SymbolIds differently for the names
#      inside the cached blob.  The serializer's remap pass on load
#      (serialize.cc remapSymbolsInBytecode) must rewrite every
#      SymbolId operand in the bytecode; if any opcode is missing
#      from the remap switch, the warm lookup hits the wrong
#      attribute and throws.
#
# A subtle gotcha: primImport silently falls back to a fresh
# lower+compile if the cache-restored CU throws — masking the
# regression.  V3_STRICT_DISK_CACHE=1 (set below) re-throws instead,
# making the bug surface as a hard test failure.
#
# CRIT-1 was: OP_REC_BINDING_SLOT_REF carries a SymbolId in its
# operand but had no arm in remapSymbolsInBytecode.  With pre-fix
# code + V3_STRICT_DISK_CACHE=1, every expression using a
# rec-attrset entry reference triggers
# "OP_REC_BINDING_SLOT_REF: name '...' not found" on warm load.

set -u

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
V3="${V3:-$ROOT/build/src/libexpr-v3/v3-eval}"

if [[ ! -x "$V3" ]]; then
  echo "v3-eval not found at $V3" >&2
  exit 1
fi

verbose="${V3_DISK_CACHE_VERBOSE:-0}"

# Each test: (id, file_content, expected).  The same file_content is
# imported twice — once cold, once warm — under different prelude
# expressions.  Both must agree.
TESTS=(
  # OP_REC_BINDING_SLOT_REF: rec-attrset slot reference.  CRIT-1
  # regression coverage.  Use unusual attr names so they aren't
  # pre-interned by builtins init — the symbol-table divergence
  # between cold and warm processes is what surfaces a missing
  # remap arm; common names share a SymbolId across processes.
  CRIT-1-with-rec-slot-ref
  'let v3_test_libxyz = rec {
     v3_test_alpha = 1;
     v3_test_beta = v3_test_alpha + 10;
     v3_test_getter = v3_test_name: v3_test_libxyz.${v3_test_name};
   }; in v3_test_libxyz.v3_test_getter "v3_test_beta"'
  '11'

  # OP_ATTRS_SELECT after rec-attr access.
  CRIT-1-attrs-select-rec
  'let r = rec { x = 1; y = x + 2; z = y + 3; }; in r.z'
  '6'

  # OP_ATTRS_HAS on a rec attrset.
  CRIT-1-attrs-has-rec
  'let r = rec { x = 1; y = x + 2; }; in r ? y'
  'true'

  # OP_WITH_LOOKUP across `with rec`.
  CRIT-1-with-lookup-inherit
  'let r = rec { foo = "F"; bar = foo; };
   in with r; bar'
  '"F"'

  # Multi-rec: nested lib-style patterns common in real nixpkgs
  # callPackagesWith chains.
  CRIT-1-nested-rec
  'let outer = rec {
     inner = rec { val = 7; doubled = val * 2; };
     pick = inner.doubled;
   }; in outer.pick'
  '14'
)

pass=0
fail=0
total=0
failed_cases=()

# Per-test temp dir + cache root.
WORK_ROOT="$(mktemp -d -t v3-disk-cache.XXXXXX)"
trap 'rm -rf "$WORK_ROOT"' EXIT

i=0
while [[ $i -lt ${#TESTS[@]} ]]; do
  id="${TESTS[$i]}"; file_content="${TESTS[$((i+1))]}"; expected="${TESTS[$((i+2))]}"
  i=$((i+3))
  total=$((total+1))

  test_dir="$WORK_ROOT/$id"
  mkdir -p "$test_dir/cache" "$test_dir/files"
  test_file="$test_dir/files/test.nix"
  echo "$file_content" > "$test_file"

  # Same XDG_CACHE_HOME for cold + warm so the second run hits the
  # serialized blob the first run produced.
  export XDG_CACHE_HOME="$test_dir/cache"

  # Cold run.  Different "noise" prelude than the warm run — interns
  # a different set of symbols before the import resolves, so the
  # global SymbolId for any name inside `test.nix` differs between
  # the two processes.
  cold_expr="let _cold_noise = { aaa_only = 1; mmm_only = 2; zzz_only = 3; }; in import $test_file"
  # V3_STRICT_DISK_CACHE=1 turns the silent "deserialize failed -> fall
  # back to fresh compile" path in primops.cc:primImport into a hard
  # throw.  Without this, a CRIT-1-class miss-remap silently falls back
  # to fresh compile and the test masks the bug it's supposed to catch.
  cold_out=$(NIX_V3_DISK_CACHE=1 V3_STRICT_DISK_CACHE=1 "$V3" --expr "$cold_expr" --strict 2>&1)
  cold_rc=$?
  cold_trim=$(echo -n "$cold_out" | sed 's/^[[:space:]]*//;s/[[:space:]]*$//')

  # Warm run.  Different prelude → different intern order → different
  # SymbolIds for the cached opcode operands.  remap pass must fix.
  warm_expr="let _warm_noise = { bbb_only = 4; nnn_only = 5; yyy_only = 6; xxx_only = 7; }; in import $test_file"
  warm_out=$(NIX_V3_DISK_CACHE=1 V3_STRICT_DISK_CACHE=1 "$V3" --expr "$warm_expr" --strict 2>&1)
  warm_rc=$?
  warm_trim=$(echo -n "$warm_out" | sed 's/^[[:space:]]*//;s/[[:space:]]*$//')

  if [[ $cold_rc -ne 0 ]]; then
    fail=$((fail+1))
    failed_cases+=("$id (cold run errored: $cold_trim)")
    [[ "$verbose" == "1" ]] && echo "ERR-COLD  $id — $cold_trim"
  elif [[ $warm_rc -ne 0 ]]; then
    fail=$((fail+1))
    failed_cases+=("$id (warm run errored: $warm_trim)")
    [[ "$verbose" == "1" ]] && echo "ERR-WARM  $id — $warm_trim"
  elif [[ "$cold_trim" != "$expected" ]]; then
    fail=$((fail+1))
    failed_cases+=("$id (cold mismatch: got '$cold_trim' expected '$expected')")
    [[ "$verbose" == "1" ]] && echo "FAIL-COLD $id — got '$cold_trim' expected '$expected'"
  elif [[ "$cold_trim" != "$warm_trim" ]]; then
    fail=$((fail+1))
    failed_cases+=("$id (cold='$cold_trim' warm='$warm_trim' — symbol-remap drift?)")
    [[ "$verbose" == "1" ]] && echo "DRIFT     $id — cold='$cold_trim' warm='$warm_trim'"
  else
    pass=$((pass+1))
    [[ "$verbose" == "1" ]] && echo "OK        $id"
  fi
done

echo ""
echo "=== v3 disk-cache regression results ==="
echo "  total:    $total"
echo "  passing:  $pass"
echo "  failing:  $fail"
if [[ ${#failed_cases[@]} -gt 0 ]]; then
  echo ""
  echo "Failed cases:"
  for c in "${failed_cases[@]}"; do echo "  $c"; done
fi

[[ $fail -eq 0 ]]
