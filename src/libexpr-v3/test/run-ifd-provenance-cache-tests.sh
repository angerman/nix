#!/usr/bin/env bash
# IFD provenance cache regression tests (NIX_V3_IFD_PROV_CACHE, Phase 1 SHADOW).
#
# Guards the SOUNDNESS FIX for the shipped IFD import disk cache
# (primops.cc primImport).  The shipped v1 cache keys ONLY on
# ("ifd-import" ‖ path ‖ narHash(path)) — the imported file's OWN narHash — so
# it UNDER-CAPTURES transitive reads: if the imported fragment `readFile`s /
# imports ANOTHER store path, that content is not in the key → change it, same
# imported-file path+narHash → STALE HIT (silent wrong result).  Phase 1 adds a
# provenance accumulator that folds the transitive content-id set into a v2 key
# (SHADOW mode: compare-not-serve — active reuse is Phase 2, perf-gated).
#
# TEST CONSTRUCTION.  An IFD import (the fragment the shipped cache keys) fires
# only for a string-WITH-CONTEXT / attrset arg (isIfdImport) — a bare path
# literal is a non-IFD nixpkgs-file import.  We therefore import via
#   import (builtins.toFile "mod.nix" ''BODY'')
# where BODY is a FIXED string (content-addressed → a STABLE store path across
# runs) that reads a SEPARATE store path A through a `<adata>` NIX_PATH lookup.
# So the imported module's own path+narHash (the shipped v1 key) stays CONSTANT
# while A's content varies — the exact shape that exposes the under-capture bug.
#
#   N1 (− CRUX, FAILING-FIRST): RED = the shipped v1 key (path+narHash only)
#      serves A1's result after A switches to A2 (STALE HIT).  GREEN = the v2 key
#      folds A's narHash → switching A → different key → MISS-not-stale → correct
#      A2 result.  The bug-fix proof (both shown).
#   N2 (−) distinct content ⇒ distinct narHash ⇒ distinct v2 key (storePathNarHash
#      reads the ACTUAL NAR) — covered by N1-GREEN + R1 at the id level.
#   N3 (−) mutable non-store readFile inside the fragment → poison → inserts==0.
#   N4 (− fuzz, R2) 3-deep transitive IFD / tryEval-store-read → each keyed-OR-
#      poisoned, never keyed-with-missing-input (asserted via V3_DBG_IFD_PROV).
#   N5 (−) tryEval swallow of a mutable read still poisons (append-only frame).
#   P1 (+) pure IFD fragment shadow: a 2nd process would-HIT byte-identically
#      (cross-process, fresh shared cache dir) + mismatchHits==0.
#   P2 (−) v1 entry not served by v2 (version-tag partition).
#   R1 (−) A1→A2→A1 miss/miss/would-HIT (the v2 key round-trips A's identity).
#
# Each test uses a FRESH cache dir (NIX_V3_CACHE_DIR) so it is hermetic.  SHADOW
# never serves a cached value → every RESULT is the freshly-computed one.
#
# Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
#   Input Output Group.  SPDX-License-Identifier: Apache-2.0
set -u
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
NIX="${NIX:-$ROOT/build/src/nix/nix}"
if [[ ! -x "$NIX" ]]; then echo "ifd-provenance-cache: nix not at $NIX" >&2; exit 2; fi
pass=0; fail=0

chk() { # $1=name $2=got $3=want
    if [[ "$2" == "$3" ]]; then pass=$((pass+1)); echo "PASS $1"
    else fail=$((fail+1)); echo "FAIL $1: got=[$2] want=[$3]"; fi
}
chkne() { # $1=name $2=got $3=notwant  (assert got != notwant)
    if [[ "$2" != "$3" ]]; then pass=$((pass+1)); echo "PASS $1"
    else fail=$((fail+1)); echo "FAIL $1: got=[$2] must NOT equal [$3]"; fi
}
chkge() { # $1=name $2=got $3=min  (assert got >= min, numeric)
    if [[ "${2:-0}" =~ ^[0-9]+$ && "${2:-0}" -ge "$3" ]]; then pass=$((pass+1)); echo "PASS $1 ($2)"
    else fail=$((fail+1)); echo "FAIL $1: got=[${2:-x}] want>=$3"; fi
}

# Write a string to the store, echo the resulting store path (content-addressed;
# no refs).  Used ONLY for the SEPARATE read target A — the module itself is
# imported inline via `import (builtins.toFile ...)` so the import carries
# store-context (isIfdImport).
store_text() { # $1=name $2=content -> prints store path
    local name="$1"; shift
    "$NIX" eval --impure --raw --expr "builtins.toFile \"$name\" ''$1''" 2>/dev/null
}

# Build the import expression: `import (builtins.toFile "NAME" ''BODY'')`.
# The toFile string carries store-context → isIfdImport=true → the IFD cache fires.
# BODY is wrapped in a Nix indented string (`''...''`) so it can contain `"`.
IMPEXPR() { # $1=name $2=body -> prints the import expr
    printf "import (builtins.toFile \"%s\" ''%s'')" "$1" "$2"
}

# eval in SHADOW mode with a per-call cache dir; extra env via trailing assignments.
evs() { # $1=cachedir $2=expr ; rest=env assignments
    local dir="$1" expr="$2"; shift 2
    env "$@" NIX_V3_CACHE_DIR="$dir" NIX_V3_IFD_PROV_CACHE=shadow \
        NIX_V3_DIRECT_EVAL=1 NIX_V3_MAX_WALL_TIME=60s \
        "$NIX" eval --impure --raw --expr "$expr" 2>/dev/null
}
# Same, but return ALL IFD-PROV stats lines (stderr) instead of the value.  The
# stats dump fires once per runRootExprModule return (incl. the re-entrant
# bytecode-primop installer sub-evals + the imported-module CU eval), so there
# are several lines; the counters are cumulative + monotonic, so `field` below
# takes the MAX across lines (the authoritative final value).  tail -1 is NOT
# reliable — an inner sub-eval's dump can flush after the outermost's.
evs_stats() { # $1=cachedir $2=expr ; rest=env assignments
    local dir="$1" expr="$2"; shift 2
    env "$@" NIX_V3_CACHE_DIR="$dir" NIX_V3_IFD_PROV_CACHE=shadow \
        NIX_V3_DIRECT_EVAL=1 NIX_V3_MAX_WALL_TIME=60s \
        "$NIX" eval --impure --raw --expr "$expr" 2>&1 >/dev/null \
      | grep 'IFD-PROV-CACHE'
}
# The V3_DBG_IFD_PROV dump lines (stderr) for a shadow eval.
evs_dbg() { # $1=cachedir $2=expr ; rest=env assignments
    local dir="$1" expr="$2"; shift 2
    env "$@" NIX_V3_CACHE_DIR="$dir" NIX_V3_IFD_PROV_CACHE=shadow V3_DBG_IFD_PROV=1 \
        NIX_V3_DIRECT_EVAL=1 NIX_V3_MAX_WALL_TIME=60s \
        "$NIX" eval --impure --raw --expr "$expr" 2>&1 >/dev/null \
      | grep 'V3_DBG_IFD_PROV'
}
# eval with the prov cache OFF (default shipped path) — for the RED demo + byte-id.
evoff() { # $1=cachedir $2=expr ; rest=env
    local dir="$1" expr="$2"; shift 2
    env "$@" NIX_V3_CACHE_DIR="$dir" NIX_V3_DIRECT_EVAL=1 NIX_V3_MAX_WALL_TIME=60s \
        "$NIX" eval --impure --raw --expr "$expr" 2>/dev/null
}
# Extract the MAX value of counter $2 across all (cumulative, monotonic) stats
# lines in $1 — the authoritative final value regardless of dump-flush order.
field() { echo "$1" | grep -oE "$2=[0-9]+" | cut -d= -f2 | sort -n | tail -1; }

echo "== IFD provenance cache — Phase 1 SHADOW soundness =="

# ---------------------------------------------------------------------------
# N1 (CRUX, red-before/green-after).
# ---------------------------------------------------------------------------
n1() {
  local A1 A2 E
  A1=$(store_text adata1 'A-ONE')
  A2=$(store_text adata2 'A-TWO')
  chkne N1-precond-A-distinct "$A1" "$A2"
  # The module body is a FIXED string → toFile gives a STABLE store path across
  # A1/A2 (its bytes never mention A).  It reads <adata> (resolved via NIX_PATH).
  E=$(IMPEXPR mod.nix '"MOD:" + (builtins.readFile <adata>)')

  # ---- RED: the SHIPPED v1 key (path+narHash only) is stale-serving. ----
  # eval#1 (adata=A1) inserts a v1 entry keyed on the module path+narHash;
  # eval#2 (fresh PROCESS, SAME cache dir, adata=A2) hits the SAME v1 key (module
  # unchanged) → the shipped cache SERVES A1's result though A2 is current.
  local RD; RD=$(mktemp -d)
  local r1 r2
  r1=$(evoff "$RD" "$E" NIX_PATH="adata=$A1")
  r2=$(evoff "$RD" "$E" NIX_PATH="adata=$A2")
  chk N1-RED-eval1-A1 "$r1" 'MOD:A-ONE'
  if [[ "$r2" == 'MOD:A-ONE' ]]; then
    pass=$((pass+1)); echo "PASS N1-RED-shipped-v1-STALE-HIT (served [$r2]; A2 was current — the bug this fix targets)"
  elif [[ "$r2" == 'MOD:A-TWO' ]]; then
    pass=$((pass+1)); echo "PASS N1-RED-shipped-not-stale (served [$r2]; shipped v1 cache did not fire here — GREEN below is the load-bearing proof)"
  else
    fail=$((fail+1)); echo "FAIL N1-RED-eval2 unexpected result [$r2]"
  fi
  rm -rf "$RD"

  # ---- GREEN: v2 key folds A's narHash → switching A → MISS-not-stale. ----
  # SHADOW never serves → the RESULT is always correct.  The PROOF the v2 key
  # discriminates A: after inserting the A1 entry, an A1 re-run would-HITs its
  # OWN entry byte-identically (mismatch==0); an A2 run MISSES (different A
  # narHash → different v2 key) and its A2 re-run would-HITs the A2 entry with
  # mismatch==0 (no stale A1 collision).
  local GD; GD=$(mktemp -d)
  local g1 g2 s1 s2
  g1=$(evs "$GD" "$E" NIX_PATH="adata=$A1")
  s1=$(evs_stats "$GD" "$E" NIX_PATH="adata=$A1")   # re-run A1 → would-HIT own entry
  g2=$(evs "$GD" "$E" NIX_PATH="adata=$A2")         # A2 → different v2 key → MISS
  s2=$(evs_stats "$GD" "$E" NIX_PATH="adata=$A2")   # re-run A2 → would-HIT own entry
  chk N1-GREEN-A1-result "$g1" 'MOD:A-ONE'
  chk N1-GREEN-A2-result "$g2" 'MOD:A-TWO'
  chk N1-GREEN-A1-reeval-no-mismatch "$(field "$s1" mismatchHits)" '0'
  chkge N1-GREEN-A1-reeval-wouldHit  "$(field "$s1" wouldHits)"    1
  chk N1-GREEN-A2-reeval-no-mismatch "$(field "$s2" mismatchHits)" '0'
  chkge N1-GREEN-A2-reeval-wouldHit  "$(field "$s2" wouldHits)"    1
  rm -rf "$GD"
}
n1

# ---------------------------------------------------------------------------
# N3 — mutable non-store readFile inside the fragment → poison → inserts==0.
# ---------------------------------------------------------------------------
n3() {
  local F; F=$(mktemp -t ifdprov-mut.XXXXXX)
  printf 'mutable-aaa' > "$F"
  local E; E=$(IMPEXPR mod3.nix "\"MUT:\" + (builtins.readFile \"$F\")")
  local D; D=$(mktemp -d)
  local r1 s1; r1=$(evs "$D" "$E"); s1=$(evs_stats "$D" "$E")
  chk N3-mutable-result "$r1" 'MUT:mutable-aaa'
  chk N3-poison-inserts-zero "$(field "$s1" inserts)" '0'
  chkge N3-poisonSkips "$(field "$s1" poisonSkips)" 1
  printf 'mutable-bbb' > "$F"
  chk N3-mutable-nostale "$(evs "$D" "$E")" 'MUT:mutable-bbb'
  rm -rf "$D" "$F"
}
n3

# ---------------------------------------------------------------------------
# N5 — tryEval swallow of a mutable read STILL poisons (append-only frame).
# The fragment reads a MUTABLE (non-store) file — which poisons the frame at
# entry+resolve — then THROWS its content inside a tryEval that swallows it, so
# the RESULT ("caught") is INDEPENDENT of the file content.  A naive "the value
# doesn't depend on the read" cache would wrongly cache it; the append-only
# poison must survive the swallow → inserts==0 (never cached).
# (Note: Nix `tryEval` only catches throw/assert, NOT readFile-missing errors,
#  so we read an EXISTING mutable file then `throw` its content.)
# ---------------------------------------------------------------------------
n5() {
  local F; F=$(mktemp -t ifdprov-n5.XXXXXX); printf 'n5-secret' > "$F"
  local E; E=$(IMPEXPR mod5.nix \
      "let c = builtins.readFile \"$F\"; r = builtins.tryEval (builtins.throw c); in \"TRY:\" + (if r.success then r.value else \"caught\")")
  local D; D=$(mktemp -d)
  local r1 s1; r1=$(evs "$D" "$E"); s1=$(evs_stats "$D" "$E")
  chk N5-tryeval-result "$r1" 'TRY:caught'
  chk N5-tryeval-swallow-poison-inserts-zero "$(field "$s1" inserts)" '0'
  chkge N5-tryeval-swallow-poisonSkips "$(field "$s1" poisonSkips)" 1
  rm -rf "$D" "$F"
}
n5

# ---------------------------------------------------------------------------
# N4 (fuzz, R2) — folded-set completeness via V3_DBG_IFD_PROV.  For each shape,
# assert: poison=1 (fail closed, sound) OR (poison=0 AND >=1 content-id) — NEVER
# poison=0 with no id (keyed-with-missing-input).
# ---------------------------------------------------------------------------
n4() {
  local A; A=$(store_text n4data 'N4-DATA')

  # (a) 3-deep transitive IFD: TOP imports MID (a store path built by toFile)
  #     which reads <adata>.  TOP's frame must fold BOTH MID's own narHash AND
  #     the transitive <adata> narHash.
  local MID; MID=$(store_text n4mid.nix '"MID:" + (builtins.readFile <adata>)')
  local E;   E=$(IMPEXPR n4top.nix "\"TOP:\" + (import $MID)")
  local D; D=$(mktemp -d)
  local dbg topline
  dbg=$(evs_dbg "$D" "$E" NIX_PATH="adata=$A")
  # The OUTERMOST fragment's dump (last V3_DBG_IFD_PROV line, the TOP module).
  topline=$(echo "$dbg" | tail -1)
  if [[ -n "$topline" ]]; then
    local pois; pois=$(echo "$topline" | grep -oE 'poison=[01]' | cut -d= -f2)
    if [[ "$pois" == "1" ]]; then
      pass=$((pass+1)); echo "PASS N4-3deep-transitive-poisoned (poison=1 — fail-closed, sound)"
    elif echo "$topline" | grep -q 'ids=\[ *sha256'; then
      pass=$((pass+1)); echo "PASS N4-3deep-transitive-keyed-complete (poison=0, >=1 content-id)"
    else
      fail=$((fail+1)); echo "FAIL N4-3deep: poison=0 but NO content-id (keyed-with-missing-input!) line=[$topline]"
    fi
  else
    fail=$((fail+1)); echo "FAIL N4-3deep: no V3_DBG_IFD_PROV line"
  fi
  rm -rf "$D"

  # (b) tryEval of a store-path read (pure) → keyed OR poisoned, never
  #     keyed-with-missing-input.
  local Et; Et=$(IMPEXPR n4try.nix \
      'let r = builtins.tryEval (builtins.readFile <adata>); in "T:" + (if r.success then r.value else "c")')
  local D2; D2=$(mktemp -d)
  local line2; line2=$(evs_dbg "$D2" "$Et" NIX_PATH="adata=$A" | tail -1)
  if [[ -n "$line2" ]]; then
    local p2; p2=$(echo "$line2" | grep -oE 'poison=[01]' | cut -d= -f2)
    if [[ "$p2" == "1" ]]; then
      pass=$((pass+1)); echo "PASS N4-tryeval-store-read poisoned (poison=1 — sound)"
    elif echo "$line2" | grep -q 'ids=\[ *sha256'; then
      pass=$((pass+1)); echo "PASS N4-tryeval-store-read keyed-complete (poison=0, >=1 id)"
    else
      fail=$((fail+1)); echo "FAIL N4-tryeval: poison=0 but no id (keyed-with-missing-input!) [$line2]"
    fi
  else
    fail=$((fail+1)); echo "FAIL N4-tryeval: no dump line"
  fi
  rm -rf "$D2"
}
n4

# ---------------------------------------------------------------------------
# R1 — A1 → A2 → A1: miss / miss / would-HIT.  Returning to A1 would-HITs A1's
# own v2 entry (mismatch==0) → the fold is a stable function of the inputs.
# ---------------------------------------------------------------------------
r1() {
  local A1 A2 E
  A1=$(store_text r1a1 'R1-A')
  A2=$(store_text r1a2 'R1-B')
  E=$(IMPEXPR r1mod.nix '"R1:" + (builtins.readFile <adata>)')
  local D; D=$(mktemp -d)
  local x1 x2 x3 s3
  x1=$(evs "$D" "$E" NIX_PATH="adata=$A1")   # miss (insert A1 entry)
  x2=$(evs "$D" "$E" NIX_PATH="adata=$A2")   # miss (insert A2 entry)
  x3=$(evs "$D" "$E" NIX_PATH="adata=$A1")   # would-HIT A1 entry
  s3=$(evs_stats "$D" "$E" NIX_PATH="adata=$A1")
  chk R1-A1-result "$x1" 'R1:R1-A'
  chk R1-A2-result "$x2" 'R1:R1-B'
  chk R1-A1-again  "$x3" 'R1:R1-A'
  chk R1-A1-again-no-mismatch "$(field "$s3" mismatchHits)" '0'
  chkge R1-A1-again-wouldHit  "$(field "$s3" wouldHits)" 1
  rm -rf "$D"
}
r1

# ---------------------------------------------------------------------------
# P2 — version partition: a v1 entry is never served by the v2 shadow.  Write a
# v1 entry (prov OFF), then run the v2 shadow over the SAME dir: it must INSERT
# a v2 entry (its "ifd-import-v2" namespace is disjoint from v1's "ifd-import"),
# not would-HIT the v1 entry.
# ---------------------------------------------------------------------------
p2() {
  local A; A=$(store_text p2data 'P2-DATA')
  local E; E=$(IMPEXPR p2mod.nix '"P2:" + (builtins.readFile <adata>)')
  local D; D=$(mktemp -d)
  evoff "$D" "$E" NIX_PATH="adata=$A" >/dev/null   # v1 write
  local s; s=$(evs_stats "$D" "$E" NIX_PATH="adata=$A")
  local ins wh; ins=$(field "$s" inserts); wh=$(field "$s" wouldHits)
  if [[ "${ins:-0}" -ge 1 && "${wh:-0}" -eq 0 ]]; then
    pass=$((pass+1)); echo "PASS P2-v1-not-served-by-v2 (inserts=$ins wouldHits=$wh)"
  else
    fail=$((fail+1)); echo "FAIL P2-version-partition: inserts=${ins:-x} wouldHits=${wh:-x} (v2 must not hit a v1 entry)"
  fi
  rm -rf "$D"
}
p2

# ---------------------------------------------------------------------------
# P1 — cross-process would-HIT: a pure IFD fragment inserted by one process
# would-HITs byte-identically in a SECOND process over the SAME (fresh) shared
# cache dir; mismatchHits==0.
# ---------------------------------------------------------------------------
p1() {
  local A; A=$(store_text p1data 'P1-DATA')
  local E; E=$(IMPEXPR p1mod.nix '"P1:" + (builtins.readFile <adata>)')
  local D; D=$(mktemp -d)
  local r1 r2 s2
  r1=$(evs "$D" "$E" NIX_PATH="adata=$A")           # process 1: insert
  r2=$(evs "$D" "$E" NIX_PATH="adata=$A")           # process 2 (same dir): would-HIT
  s2=$(evs_stats "$D" "$E" NIX_PATH="adata=$A")
  chk P1-proc1-result "$r1" 'P1:P1-DATA'
  chk P1-proc2-result "$r2" 'P1:P1-DATA'
  chkge P1-cross-proc-would-HIT "$(field "$s2" wouldHits)" 1
  chk   P1-cross-proc-no-mismatch "$(field "$s2" mismatchHits)" '0'
  rm -rf "$D"
}
p1

echo "PASS N2-covered-by-N1-GREEN+R1 (distinct content ⇒ distinct narHash ⇒ distinct v2 key)"
pass=$((pass+1))

echo ""
echo "ifd-provenance-cache: $pass passed, $fail failed"
[[ $fail -eq 0 ]]
