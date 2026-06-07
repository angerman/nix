#!/usr/bin/env bash
#
# scaling-check.sh — COMPLEXITY-CLASS regression guard for v3 (comprehensive).
#
# Catches O(n)→super-linear blowups that absolute-threshold tests MISS (latent
# at small n; e.g. listToAttrs was fine at 1k, 127× at 200k; sort hangs at 200k).
# Measures each workload at n, 2n, 4n and asserts the doubling RATIO stays in the
# expected class — ~2× O(n)/O(n log n); ~4× O(n²); TIMEOUT/huge ⇒ super-linear.
# The ratio CANCELS host speed/load, so it's noise-robust (MEASUREMENT_GATE §8/§9).
#
# Coverage goal: ALL size-dependent (list / attrs / string / deep) builtins — the
# ~40 of v3's 114 that scale with input. Scalar/fixed-arity builtins (add, typeOf,
# …) don't scale and are correctness-covered by the lang/--core suites instead.
#
# Robustness (learned the hard way):
#   * FILE-based workloads (--file): no shell-quoting limits → string primops with
#     literal "," / "sha256" / regex are testable.
#   * PER-EVAL TIMEOUT ($TIMEOUT_S) that REAPS the nix child (pkill on the unique
#     workload-file path) → a super-linear primop fails as TIMEOUT, never hangs
#     or orphans the suite.
#   * metric tiers: user (timing; idle host) / insns / attrs / rss (deterministic).
#
# XFAIL: known-blown entries (sort today) keep the suite green AND auto-detect a
# fix — when one measures linear it reports XPASS ("promote me, remove xfail").
#
# Usage:  [NIX_BIN=…] [RUNS=2] [TIMEOUT_S=45] ./scaling-check.sh [list|attrs|string|deep|all]
# Exit:   1 on a hard FAIL (a non-xfail workload went super-linear) or ERROR.

set -uo pipefail
SELF_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(git -C "$SELF_DIR" rev-parse --show-toplevel 2>/dev/null || echo "$SELF_DIR/../../..")"
NIX_BIN="${NIX_BIN:-$REPO/build/src/nix/nix}"
RUNS="${RUNS:-2}"
TIMEOUT_S="${TIMEOUT_S:-45}"
FILTER="${1:-all}"
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"; pkill -9 -f "$TMP/wl.nix" 2>/dev/null' EXIT
[[ -x "$NIX_BIN" ]] || { echo "nix not found: $NIX_BIN (set NIX_BIN=)"; exit 2; }
WL="$TMP/wl.nix"

# ── workload templates (file-based; __N__ = size; ANY Nix is fine here) ───────
# LIST ──────────────────────────────────────────────────────────────────────
wl_map()         { cat <<'N'
let xs = builtins.map (x: x + 1) (builtins.genList (i: i) __N__); in builtins.length xs
N
}
wl_filter()      { cat <<'N'
let xs = builtins.filter (x: x > 1) (builtins.genList (i: i) __N__); in builtins.length xs
N
}
wl_foldl()       { cat <<'N'
let xs = builtins.genList (i: i) __N__; in builtins.foldl' (a: x: a + x) 0 xs
N
}
wl_concatMap()   { cat <<'N'
let xs = builtins.concatMap (x: [ x x ]) (builtins.genList (i: i) __N__); in builtins.length xs
N
}
wl_concatLists() { cat <<'N'
let xs = builtins.concatLists (builtins.genList (i: [ i ]) __N__); in builtins.length xs
N
}
wl_all()         { cat <<'N'
builtins.all (x: x > (0 - 1)) (builtins.genList (i: i) __N__)
N
}
wl_any()         { cat <<'N'
builtins.any (x: x > __N__) (builtins.genList (i: i) __N__)
N
}
wl_partition()   { cat <<'N'
let p = builtins.partition (x: x > 1) (builtins.genList (i: i) __N__); in builtins.length p.right
N
}
wl_groupBy()     { cat <<'N'
let g = builtins.groupBy (x: toString (x - (x / 2) * 2)) (builtins.genList (i: i) __N__); in builtins.length (builtins.attrValues g)
N
}
wl_elemAt()      { cat <<'N'
let xs = builtins.genList (i: i) __N__; in builtins.foldl' (a: k: a + (builtins.elemAt xs k)) 0 (builtins.genList (i: i) __N__)
N
}
wl_elem()        { cat <<'N'
builtins.elem (__N__ - 1) (builtins.genList (i: i) __N__)
N
}
wl_tail()        { cat <<'N'
builtins.length (builtins.tail (builtins.genList (i: i) __N__))
N
}
wl_length()      { cat <<'N'
builtins.length (builtins.genList (i: i) __N__)
N
}
wl_sort()        { cat <<'N'
let xs = builtins.sort (a: b: a < b) (builtins.genList (i: __N__ - i) __N__); in builtins.length xs
N
}
# ATTRS (input built via listToAttrs — O(n) since the e26d15612 fix) ──────────
wl_listToAttrs() { cat <<'N'
let s = builtins.listToAttrs (builtins.genList (i: { name = toString i; value = i; }) __N__); in builtins.seq s 0
N
}
wl_attrNames()   { cat <<'N'
let s = builtins.listToAttrs (builtins.genList (i: { name = toString i; value = i; }) __N__); in builtins.length (builtins.attrNames s)
N
}
wl_attrValues()  { cat <<'N'
let s = builtins.listToAttrs (builtins.genList (i: { name = toString i; value = i; }) __N__); in builtins.foldl' (a: v: a + v) 0 (builtins.attrValues s)
N
}
wl_mapAttrs()    { cat <<'N'
let s = builtins.listToAttrs (builtins.genList (i: { name = toString i; value = i; }) __N__); in builtins.foldl' (a: v: a + v) 0 (builtins.attrValues (builtins.mapAttrs (k: v: v + 1) s))
N
}
wl_removeAttrs() { cat <<'N'
let s = builtins.listToAttrs (builtins.genList (i: { name = toString i; value = i; }) __N__); in builtins.length (builtins.attrNames (builtins.removeAttrs s [ (toString 0) (toString 1) ]))
N
}
wl_intersectAttrs() { cat <<'N'
let s = builtins.listToAttrs (builtins.genList (i: { name = toString i; value = i; }) __N__);
    t = builtins.listToAttrs (builtins.genList (i: { name = toString i; value = i; }) __N__);
in builtins.length (builtins.attrNames (builtins.intersectAttrs s t))
N
}
wl_hasAttr()     { cat <<'N'
let s = builtins.listToAttrs (builtins.genList (i: { name = toString i; value = i; }) __N__); in builtins.foldl' (a: k: a + (if builtins.hasAttr k s then 1 else 0)) 0 (builtins.attrNames s)
N
}
wl_getAttr()     { cat <<'N'
let s = builtins.listToAttrs (builtins.genList (i: { name = toString i; value = i; }) __N__); in builtins.foldl' (a: k: a + (builtins.getAttr k s)) 0 (builtins.attrNames s)
N
}
wl_catAttrs()    { cat <<'N'
let xs = builtins.genList (i: builtins.listToAttrs [ { name = toString 0; value = i; } ]) __N__; in builtins.length (builtins.catAttrs (toString 0) xs)
N
}
wl_update()      { cat <<'N'
let base = builtins.listToAttrs (builtins.genList (i: { name = toString i; value = i; }) __N__); in builtins.length (builtins.attrNames (base // { extra = 0; }))
N
}
wl_zipAttrsWith(){ cat <<'N'
let xs = builtins.genList (i: builtins.listToAttrs [ { name = toString i; value = i; } ]) __N__; in builtins.length (builtins.attrNames (builtins.zipAttrsWith (k: vs: vs) xs))
N
}
# STRING (literals now legal: file-based) ─────────────────────────────────────
wl_concatStringsSep() { cat <<'N'
let xs = builtins.genList (i: toString i) __N__; in builtins.stringLength (builtins.concatStringsSep "," xs)
N
}
wl_toJSON()      { cat <<'N'
builtins.stringLength (builtins.toJSON (builtins.genList (i: i) __N__))
N
}
wl_fromJSON()    { cat <<'N'
let j = builtins.toJSON (builtins.listToAttrs (builtins.genList (i: { name = toString i; value = i; }) __N__)); in builtins.length (builtins.attrNames (builtins.fromJSON j))
N
}
wl_replaceStrings() { cat <<'N'
let s = builtins.concatStringsSep "," (builtins.genList (i: toString i) __N__); in builtins.stringLength (builtins.replaceStrings [ "," ] [ ";" ] s)
N
}
wl_split()       { cat <<'N'
let s = builtins.concatStringsSep "," (builtins.genList (i: toString i) __N__); in builtins.length (builtins.split "," s)
N
}
wl_splitString() { cat <<'N'
let s = builtins.concatStringsSep "," (builtins.genList (i: toString i) __N__); in builtins.length (builtins.splitString "," s)
N
}
wl_substring()   { cat <<'N'
let s = builtins.concatStringsSep "" (builtins.genList (i: "x") __N__); in builtins.stringLength (builtins.substring 0 __N__ s)
N
}
wl_stringLength(){ cat <<'N'
let s = builtins.concatStringsSep "" (builtins.genList (i: "x") __N__); in builtins.stringLength s
N
}
wl_match()       { cat <<'N'
let s = builtins.concatStringsSep "" (builtins.genList (i: "a") __N__); in builtins.isList (builtins.match "a*" s)
N
}
wl_hashString()  { cat <<'N'
let s = builtins.concatStringsSep "" (builtins.genList (i: "x") __N__); in builtins.stringLength (builtins.hashString "sha256" s)
N
}
# DEEP ────────────────────────────────────────────────────────────────────────
wl_deepSeq()     { cat <<'N'
let xs = builtins.genList (i: { a = i; b = i + 1; }) __N__; in builtins.deepSeq xs 0
N
}

# ── manifest:  category | wl-func | sizes | metric | expected-class | xfail ───
# sizes target ~0.3–3s for a LINEAR op (good ratio); a super-linear op hits the
# $TIMEOUT_S cap and is recorded TIMEOUT ⇒ flagged.  STRING ops that build their
# input via concatStringsSep are coupled to it (if concat is blown they all are).
MANIFEST=(
  "list   | wl_map             | 500000 1000000 2000000 | user | linear |"
  # XFAIL trio — BYTECODE-PRIMOP blowups found by this sweep (2026-06-07).
  # sort is proven (bc-vs-cpp A/B: C++ 452× faster @10k); filter/concatMap/
  # zipAttrsWith are the same class (bytecode impls; C++ counterparts O(N)/
  # O(N log N) per bytecode_primops.cc). Confirm each via bench/bc-vs-cpp.sh.
  # Mitigation: NIX_V3_NO_BC_{FILTER,CONCATMAP,ZIP_ATTRS_WITH,SORT}=1.
  "list   | wl_filter          | 250000 500000          | user | linear | xfail"
  "list   | wl_foldl           | 500000 1000000 2000000 | user | linear |"
  "list   | wl_concatMap       | 100000 250000          | user | linear | xfail"
  "list   | wl_concatLists     | 250000 500000 1000000  | user | linear |"
  "list   | wl_all             | 500000 1000000 2000000 | user | linear |"
  "list   | wl_any             | 500000 1000000 2000000 | user | linear |"
  "list   | wl_partition       | 250000 500000 1000000  | user | linear |"
  "list   | wl_groupBy         | 250000 500000 1000000  | user | linear |"
  "list   | wl_elemAt          | 250000 500000 1000000  | user | linear |"
  "list   | wl_elem            | 500000 1000000 2000000 | user | linear |"
  "list   | wl_tail            | 500000 1000000 2000000 | user | linear |"
  "list   | wl_length          | 500000 1000000 2000000 | user | linear |"
  "list   | wl_sort            | 1000 2000 4000         | user | nlogn  | xfail"
  "attrs  | wl_listToAttrs     | 50000 100000 200000    | user | nlogn  |"
  "attrs  | wl_attrNames       | 50000 100000 200000    | user | nlogn  |"
  "attrs  | wl_attrValues      | 50000 100000 200000    | user | nlogn  |"
  "attrs  | wl_mapAttrs        | 50000 100000 200000    | user | nlogn  |"
  "attrs  | wl_removeAttrs     | 50000 100000 200000    | user | nlogn  |"
  "attrs  | wl_intersectAttrs  | 50000 100000 200000    | user | nlogn  |"
  "attrs  | wl_hasAttr         | 50000 100000 200000    | user | nlogn  |"
  "attrs  | wl_getAttr         | 50000 100000 200000    | user | nlogn  |"
  "attrs  | wl_catAttrs        | 50000 100000 200000    | user | linear |"
  "attrs  | wl_update          | 50000 100000 200000    | user | nlogn  |"
  "attrs  | wl_zipAttrsWith    | 10000 20000 40000      | user | nlogn  | xfail"
  "string | wl_concatStringsSep| 100000 200000 400000   | user | linear |"
  "string | wl_toJSON          | 100000 200000 400000   | user | linear |"
  "string | wl_fromJSON        | 50000 100000 200000    | user | nlogn  |"
  "string | wl_replaceStrings  | 100000 200000 400000   | user | linear |"
  "string | wl_split           | 100000 200000 400000   | user | linear |"
  "string | wl_splitString     | 100000 200000 400000   | user | linear |"
  "string | wl_substring       | 100000 200000 400000   | user | linear |"
  "string | wl_stringLength    | 100000 200000 400000   | user | linear |"
  "string | wl_match           | 100000 200000 400000   | user | linear |"
  "string | wl_hashString      | 100000 200000 400000   | user | linear |"
  "deep   | wl_deepSeq         | 100000 200000 400000   | user | linear |"
)

# measure <wl-func> <size> <metric> → metric value (min over RUNS) | TIMEOUT | NOTENGAGED | NODATA
measure() {
  local wlfunc="$1" size="$2" metric="$3" best="" v i rc pid watcher
  "$wlfunc" | sed "s/__N__/$size/g" > "$WL"
  for ((i=0; i<RUNS; i++)); do
    : > "$TMP/m"
    /usr/bin/time -l env NIX_V3_DIRECT_EVAL=1 NIX_VM_STATS=1 "$NIX_BIN" \
        eval --extra-experimental-features 'nix-command flakes' --file "$WL" >/dev/null 2>"$TMP/m" &
    pid=$!
    ( sleep "$TIMEOUT_S"; kill -9 "$pid" 2>/dev/null; pkill -9 -f "$WL" 2>/dev/null ) & watcher=$!
    wait "$pid" 2>/dev/null; rc=$?
    kill "$watcher" 2>/dev/null; wait "$watcher" 2>/dev/null
    [[ $rc -eq 137 ]] && { echo TIMEOUT; return; }
    grep -q 'v3-direct' "$TMP/m" || { echo NOTENGAGED; return; }
    case "$metric" in
      user)  v=$(grep -oE '[0-9.]+ user' "$TMP/m" | grep -oE '^[0-9.]+' | head -1) ;;
      insns) v=$(grep -oE 'insns=[0-9]+' "$TMP/m" | grep -oE '[0-9]+' | tail -1) ;;
      rss)   v=$(grep 'maximum resident set size' "$TMP/m" | grep -oE '[0-9]+' | head -1) ;;
      attrs) v=$(grep -oE 'attrsets=[0-9]+' "$TMP/m" | grep -oE '[0-9]+' | tail -1) ;;
    esac
    [[ -z "$v" ]] && { echo NODATA; return; }
    [[ -z "$best" ]] && best="$v" || best=$(awk "BEGIN{print ($v<$best)?$v:$best}")
  done
  echo "$best"
}

echo "scaling-check ($FILTER) — host=$(hostname -s)  load=$(uptime | sed 's/.*averages*: //')  RUNS=$RUNS  TIMEOUT=${TIMEOUT_S}s"
echo "nix=$NIX_BIN"; echo

RESULTS="$TMP/results"; : > "$RESULTS"
for entry in "${MANIFEST[@]}"; do
  IFS='|' read -r cat wlfunc sizes metric class xfail <<<"$entry"
  cat="${cat// /}"; wlfunc="${wlfunc// /}"; metric="${metric// /}"; class="${class// /}"; xfail="${xfail// /}"
  [[ "$FILTER" != "all" && "$FILTER" != "$cat" ]] && continue
  name="${wlfunc#wl_}"
  vals=""
  for s in $sizes; do vals="$vals $(measure "$wlfunc" "$s" "$metric")"; done
  printf '%-14s|%s|%s|%s|%s|%s\n' "$name" "$metric" "$class" "$xfail" "$(echo $sizes)" "$(echo $vals)" >>"$RESULTS"
  # live line so a long run shows progress
  printf '  %-16s %s → %s\n' "$name" "$(echo $sizes)" "$(echo $vals)"
done
echo

python3 - "$RESULTS" <<'PY'
import sys, math
rows = [l.rstrip("\n") for l in open(sys.argv[1]) if l.strip()]
ACC = {"linear": 2.5, "nlogn": 2.8}
hard_fail = 0; xpass = 0
print(f'{"workload":<16}{"expect":>7}   sizes→values                                  ratio  class      verdict')
print("─"*108)
for r in rows:
    name, metric, klass, xfail, sizes, vals = r.split("|")
    name=name.strip()
    sl = sizes.split(); vl = vals.split()
    timed_out = "TIMEOUT" in vl
    nums = []
    for v in vl:
        try: nums.append(float(v))
        except ValueError: nums.append(None)
    good = [x for x in nums if x is not None and x > 0]
    if timed_out:
        # super-linear: it blew past the cap before n,2n,4n all completed
        measured = "TIMEOUT"; g = 99.0
    elif len(good) < 2:
        print(f'{name:<16}{klass:>7}   {vals.strip():<46} —      ERROR'); hard_fail += 1; continue
    else:
        ratios = [good[i+1]/good[i] for i in range(len(good)-1)]
        g = math.exp(sum(math.log(x) for x in ratios)/len(ratios))
        measured = "linear" if g <= 2.5 else ("nlogn" if g <= 2.8 else ("super" if g < 3.3 else "QUADRATIC"))
    ok = (not timed_out) and g <= ACC.get(klass, 2.8)
    if xfail == "xfail":
        verdict = "XPASS ⚠ promote!" if ok else "xfail (known)"
        if ok: xpass += 1
    else:
        verdict = "PASS" if ok else "FAIL ✗ BLOWUP"
        if not ok: hard_fail += 1
    sv = "  ".join(f"{int(float(s)/1000)}k:{(v if v is not None else 'T/O')}" for s, v in zip(sl, (nums+[None]*len(sl))))
    print(f'{name:<16}{klass:>7}   {sv:<46} {g:>4.2f}  {measured:<10} {verdict}')
print("─"*108)
if xpass:     print(f"⚠  {xpass} XPASS — promote (remove xfail; becomes a hard guard).")
if hard_fail: print(f"✗  {hard_fail} BLOWUP(S) — workload(s) that should be ≤O(n log n) went super-linear/TIMEOUT."); sys.exit(1)
print("✓  no regressions (xfail entries known/tracked).")
PY
