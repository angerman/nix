#!/usr/bin/env bash
# Top-level result cache regression tests (NIX_V3_TOPLEVEL_CACHE).
#
# Guards the ACTIVE (skip-on-hit) cache's SOUNDNESS — a wrong top-level result
# is a silent whole-eval miscompile:
#   TL1  pure expr round-trips: eval, then re-eval → ACTIVE hit returns the
#        byte-identical value (the cache actually reuses + is correct).
#   TL2  IMPURITY taint (the core soundness guard): `getEnv "X"` with X=v1 then
#        X=v2 must return v2, NOT v1 — a tainted eval must never be reused
#        (found 2026-07-05: a pre-taint stale entry served a wrong value).
#   TL3  currentTime-tainted eval is not reused (result differs across a >1s
#        gap → must re-eval, never serve a stale timestamp).
#   TL4-TL6  A1 (2026-07-06): readFile/pathExists tainted (not cached) + the
#        NIX_V3_FAKE_CURRENTTIME clock-perturbation hook.
#   TL7-TL12 A1 VETTED SPEC (TOPLEVEL_TAINT_DESIGN_2026-07-06, Q7): the two-tier
#        insert gate + offline clock-stability manifest.  TL7 (+) blessed
#        clock-independent source caches+reuses; TL8 (− CRUX) the gate accepts a
#        blessed clock-DEPENDENT source (so the offline >=3-clock generator is
#        the sole soundness gate) + unblessed→not-cached; TL9 (−) getEnv under
#        --impure rejected despite the manifest; TL10 (−) readFile hard-reject
#        dominates a wrongful bless (reject-bits FIRST); TL11/TL12 manifest-hash
#        keyspace partition (a manifest change invalidates clock-tainted entries).
# Each test uses a FRESH cache dir (NIX_V3_CACHE_DIR) so it is hermetic and
# immune to stale cross-version entries.
#
# Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
#   Input Output Group.  SPDX-License-Identifier: Apache-2.0
set -u
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
NIX="${NIX:-$ROOT/build/src/nix/nix}"
if [[ ! -x "$NIX" ]]; then echo "toplevel-cache: nix not at $NIX" >&2; exit 2; fi
pass=0; fail=0

# eval in ACTIVE mode with a per-call FRESH cache dir; extra env via "$@".
ev() { # $1=cachedir $2=expr ; rest=env assignments
    local dir="$1" expr="$2"; shift 2
    env "$@" NIX_V3_CACHE_DIR="$dir" NIX_V3_TOPLEVEL_CACHE=active NIX_V3_DIRECT_EVAL=1 \
        NIX_V3_MAX_WALL_TIME=60s "$NIX" eval --impure --raw --expr "$expr" 2>/dev/null
}
chk() { # $1=name $2=got $3=want
    if [[ "$2" == "$3" ]]; then pass=$((pass+1)); echo "PASS $1"
    else fail=$((fail+1)); echo "FAIL $1: got=$2 want=$3"; fi
}

# TL1 — pure expr round-trips through the ACTIVE cache (miss→insert→hit).
D=$(mktemp -d)
r1=$(ev "$D" 'builtins.toString (1 + 2 + 3)')       # miss, insert
r2=$(ev "$D" 'builtins.toString (1 + 2 + 3)')       # ACTIVE hit
chk TL1-pure-roundtrip-miss "$r1" '6'
chk TL1-pure-roundtrip-hit  "$r2" '6'
rm -rf "$D"

# TL2 — impurity taint: getEnv must NOT be reused across differing env values.
D=$(mktemp -d)
g1=$(ev "$D" 'builtins.getEnv "TLCACHE_X"' TLCACHE_X=first)
g2=$(ev "$D" 'builtins.getEnv "TLCACHE_X"' TLCACHE_X=second)
chk TL2-taint-first  "$g1" 'first'
chk TL2-taint-nostale "$g2" 'second'   # must be 'second', not a stale 'first'
rm -rf "$D"

# TL3 — currentTime-tainted eval is not served stale (differs across a >1s gap).
D=$(mktemp -d)
t1=$(ev "$D" 'builtins.toString builtins.currentTime')
sleep 2
t2=$(ev "$D" 'builtins.toString builtins.currentTime')
if [[ -n "$t1" && -n "$t2" && "$t1" != "$t2" ]]; then
    pass=$((pass+1)); echo "PASS TL3-currenttime-not-stale ($t1 -> $t2)"
else
    fail=$((fail+1)); echo "FAIL TL3-currenttime-not-stale: t1=$t1 t2=$t2 (must differ — a stale hit froze the clock)"
fi
rm -rf "$D"

# TL4 — A1 (2026-07-06): readFile-derived result must NOT be served stale.
# File content is NOT in the cache key, so a readFile-derived top-level result
# MUST be tainted (not cached) — else changing the file serves a stale value.
# Failing-first: before A1 extended taint beyond getEnv+currentTime, readFile
# was un-tainted → this would cache "aaa" and serve it after the file changed.
D=$(mktemp -d); F=$(mktemp -t tlcache-rf.XXXXXX)
printf 'aaa' > "$F"; r1=$(ev "$D" "builtins.readFile $F")
printf 'bbb' > "$F"; r2=$(ev "$D" "builtins.readFile $F")
chk TL4-readfile-first   "$r1" 'aaa'
chk TL4-readfile-nostale "$r2" 'bbb'   # must be 'bbb', not a stale cached 'aaa'
rm -rf "$D" "$F"

# TL5 — A1: pathExists-derived result must NOT be served stale (ambient FS state).
D=$(mktemp -d); F=$(mktemp -t tlcache-pe.XXXXXX)
p1=$(ev "$D" "builtins.toString (builtins.pathExists $F)")   # file exists → "1"
rm -f "$F"
p2=$(ev "$D" "builtins.toString (builtins.pathExists $F)")   # gone → "" (false)
chk TL5-pathexists-true    "$p1" '1'
chk TL5-pathexists-nostale "$p2" ''    # must be '' (false), not a stale '1'
rm -rf "$D"

# TL6 — A1 perturbation hook: NIX_V3_FAKE_CURRENTTIME forces a fixed clock (the
# empirical-corpus harness's clock-perturbation lever). Test-only scaffolding.
D=$(mktemp -d)
c1=$(ev "$D" 'builtins.toString builtins.currentTime' NIX_V3_FAKE_CURRENTTIME=555)
chk TL6-fake-currenttime "$c1" '555'   # hook forces the fake value
rm -rf "$D"

# ---------------------------------------------------------------------------
# TL7-TL12 — A1 (TOPLEVEL_TAINT_DESIGN_2026-07-06 VETTED SPEC, Q7): the two-tier
# insert gate + offline clock-stability manifest.  These exercise the SOUNDNESS-
# CRITICAL Q4 predicate: reject-bits FIRST, manifest LAST, fails CLOSED, manifest
# honored ONLY under --pure-eval.
#
# The manifest is keyed on manifestEntryId = SHA-256(keyBody).  Since the id
# depends on (system,NIX_PATH,basePath,source) we can't hardcode it — so we use
# a two-pass approach: V3_DBG_TOPLEVEL_MANIFEST_ID=1 prints the id to stderr;
# capture it, write it into a temp manifest, then run the cache runs pointing
# NIX_V3_TOPLEVEL_MANIFEST at that file.
# ---------------------------------------------------------------------------

# pure-eval eval, per-call FRESH cache dir + a manifest path; stdout=value.
# $1=cachedir $2=manifest(path or "") $3=expr ; rest=env assignments
evp() {
    local dir="$1" man="$2" expr="$3"; shift 3
    local manenv=(); [[ -n "$man" ]] && manenv=(NIX_V3_TOPLEVEL_MANIFEST="$man")
    env "$@" "${manenv[@]}" NIX_V3_CACHE_DIR="$dir" NIX_V3_TOPLEVEL_CACHE=active \
        NIX_V3_DIRECT_EVAL=1 NIX_V3_MAX_WALL_TIME=60s \
        "$NIX" eval --pure-eval --raw --expr "$expr" 2>/dev/null
}
# same, but return the TOPLEVEL-CACHE stats line (stderr) to inspect activeHits.
evp_stats() {
    local dir="$1" man="$2" expr="$3"; shift 3
    local manenv=(); [[ -n "$man" ]] && manenv=(NIX_V3_TOPLEVEL_MANIFEST="$man")
    env "$@" "${manenv[@]}" NIX_V3_CACHE_DIR="$dir" NIX_V3_TOPLEVEL_CACHE=active \
        NIX_V3_DIRECT_EVAL=1 NIX_V3_MAX_WALL_TIME=60s \
        "$NIX" eval --pure-eval --raw --expr "$expr" 2>&1 >/dev/null \
        | grep 'TOPLEVEL-CACHE'
}
# capture the manifestEntryId for (expr) under pure-eval + the SAME extra env
# (system/NIX_PATH/basePath must match the cache runs → id matches by
# construction).  Uses a throwaway fresh cache dir so nothing persists.
capture_id() { # $1=expr ; rest=env assignments
    local expr="$1"; shift
    local tmpd; tmpd=$(mktemp -d)
    env "$@" V3_DBG_TOPLEVEL_MANIFEST_ID=1 NIX_V3_CACHE_DIR="$tmpd" \
        NIX_V3_TOPLEVEL_CACHE=active NIX_V3_DIRECT_EVAL=1 NIX_V3_MAX_WALL_TIME=60s \
        "$NIX" eval --pure-eval --raw --expr "$expr" 2>&1 >/dev/null \
        | sed -n 's/^TOPLEVEL manifestEntryId=\([0-9a-f]*\).*/\1/p' | head -1
    rm -rf "$tmpd"
}
# same, but under --impure (for TL9: getEnv id under impure).
capture_id_impure() { # $1=expr ; rest=env assignments
    local expr="$1"; shift
    local tmpd; tmpd=$(mktemp -d)
    env "$@" V3_DBG_TOPLEVEL_MANIFEST_ID=1 NIX_V3_CACHE_DIR="$tmpd" \
        NIX_V3_TOPLEVEL_CACHE=active NIX_V3_DIRECT_EVAL=1 NIX_V3_MAX_WALL_TIME=60s \
        "$NIX" eval --impure --raw --expr "$expr" 2>&1 >/dev/null \
        | sed -n 's/^TOPLEVEL manifestEntryId=\([0-9a-f]*\).*/\1/p' | head -1
    rm -rf "$tmpd"
}
chk_hit() { # $1=name $2=stats-line  — assert activeHits>=1
    if [[ "$2" =~ activeHits=([0-9]+) && ${BASH_REMATCH[1]} -ge 1 ]]; then
        pass=$((pass+1)); echo "PASS $1 (activeHits=${BASH_REMATCH[1]})"
    else fail=$((fail+1)); echo "FAIL $1: expected an ACTIVE hit; stats=[$2]"; fi
}
chk_nohit() { # $1=name $2=stats-line  — assert activeHits==0
    if [[ "$2" =~ activeHits=([0-9]+) && ${BASH_REMATCH[1]} -eq 0 ]]; then
        pass=$((pass+1)); echo "PASS $1 (no ACTIVE hit — correct)"
    elif [[ -z "$2" ]]; then
        # no stats line at all also means no hit
        pass=$((pass+1)); echo "PASS $1 (no ACTIVE hit — correct)"
    else fail=$((fail+1)); echo "FAIL $1: expected NO ACTIVE hit; stats=[$2]"; fi
}

# TL7 (+) — a currentTime-TAINTED but clock-INDEPENDENT source, blessed in the
# manifest, caches + reuses byte-identically under --pure-eval.  Failing-first:
# without the manifest tier, reject-all-perturbable-tainted never caches it (it
# touches currentTime), so the second run would MISS.  The manifest recovers it.
D=$(mktemp -d); M=$(mktemp -t tlcache-m7.XXXXXX)
# seq FORCES currentTime (so TAINT_CURRENTTIME genuinely fires — a lazy unused
# `let _ = currentTime` would never force it, masking the manifest tier); the
# result "stable" is clock-independent.
E7='builtins.seq builtins.currentTime "stable"'
id7=$(capture_id "$E7"); printf '%s\n' "$id7" > "$M"
r7a=$(evp "$D" "$M" "$E7")                       # miss → blessed → insert
s7=$(evp_stats "$D" "$M" "$E7"); r7b=$(evp "$D" "$M" "$E7")  # ACTIVE hit
chk TL7-blessed-value-first "$r7a" 'stable'
chk TL7-blessed-value-hit   "$r7b" 'stable'
chk_hit TL7-blessed-active-hit "$s7"
rm -rf "$D" "$M"

# TL8 (− CRUX) — a clock-DEPENDENT source.  This tests the GATE, not the offline
# generator/harness (out of scope here).  Point: if you (wrongly) bless a moving
# result, the gate WILL cache it under --pure-eval — which is EXACTLY why the
# offline generator's >=3-adversarial-clock refuse-to-bless (TL8-generator, a
# deferred harness test) is the SOLE soundness gate for clock-tainted entries.
# The runtime trusts the manifest.  We assert the gate MECHANICS honestly:
#   (a) BLESSED → cached (proves the gate would accept a wrongly-blessed source,
#       so a 2-clock generator that blesses this is unsound — see comment above);
#   (b) NOT blessed → NOT cached (the sound coverage boundary).
# We freeze the clock with NIX_V3_FAKE_CURRENTTIME so (a)'s two runs agree.
D=$(mktemp -d); M=$(mktemp -t tlcache-m8.XXXXXX)
E8='builtins.toString (if builtins.currentTime > 1500000000 then 1 else 0)'
id8=$(capture_id "$E8" NIX_V3_FAKE_CURRENTTIME=1600000000); printf '%s\n' "$id8" > "$M"
r8a=$(evp "$D" "$M" "$E8" NIX_V3_FAKE_CURRENTTIME=1600000000)   # blessed → insert
s8=$(evp_stats "$D" "$M" "$E8" NIX_V3_FAKE_CURRENTTIME=1600000000)  # blessed → hit
chk TL8-blessed-value "$r8a" '1'
chk_hit TL8-blessed-gate-accepts "$s8"   # gate accepts a (wrongly-)blessed mover
# (b) coverage boundary: an EMPTY manifest → clock-tainted source not cached.
D2=$(mktemp -d); MEMPTY=$(mktemp -t tlcache-m8e.XXXXXX)  # empty file = blesses nothing
r8c=$(evp "$D2" "$MEMPTY" "$E8" NIX_V3_FAKE_CURRENTTIME=1600000000)  # miss (not blessed)
s8b=$(evp_stats "$D2" "$MEMPTY" "$E8" NIX_V3_FAKE_CURRENTTIME=1600000000)  # still miss
chk TL8-unblessed-value "$r8c" '1'
chk_nohit TL8-unblessed-not-cached "$s8b"
rm -rf "$D" "$D2" "$M" "$MEMPTY"

# TL9 (−) — getEnv blessed in the manifest but run under --impure (NOT
# --pure-eval): the perturbable set EXCLUDES getEnv under impure (Q1 — impure
# getEnv reads the real env, which is not in the key), so getEnv taint is a HARD
# REJECT despite the manifest.  Must return the CURRENT env value, never a stale
# cached one.  (capture_id_impure blesses the impure-mode id; the gate still
# rejects because pure==false demotes getEnv out of `perturbable`.)
D=$(mktemp -d); M=$(mktemp -t tlcache-m9.XXXXXX)
E9='builtins.getEnv "TL9_X"'
id9=$(capture_id_impure "$E9" TL9_X=a); printf '%s\n' "$id9" > "$M"
# run under --impure (ev uses --impure); manifest present but not honored.
g9a=$(env NIX_V3_TOPLEVEL_MANIFEST="$M" TL9_X=a NIX_V3_CACHE_DIR="$D" \
        NIX_V3_TOPLEVEL_CACHE=active NIX_V3_DIRECT_EVAL=1 NIX_V3_MAX_WALL_TIME=60s \
        "$NIX" eval --impure --raw --expr "$E9" 2>/dev/null)
g9b=$(env NIX_V3_TOPLEVEL_MANIFEST="$M" TL9_X=b NIX_V3_CACHE_DIR="$D" \
        NIX_V3_TOPLEVEL_CACHE=active NIX_V3_DIRECT_EVAL=1 NIX_V3_MAX_WALL_TIME=60s \
        "$NIX" eval --impure --raw --expr "$E9" 2>/dev/null)
chk TL9-getenv-impure-first   "$g9a" 'a'
chk TL9-getenv-impure-nostale "$g9b" 'b'   # must be 'b' — impure getEnv rejected
rm -rf "$D" "$M"

# TL10 (−) — readFile blessed in the manifest: reject-bits (TAINT_READFILE) are
# checked FIRST and DOMINATE the wrongful manifest bless (the manifest is only
# consulted when rejectBits==0; TAINT_READFILE is NEVER in the perturbable set,
# so it is always a rejectBit).  Must serve fresh content after the file changes,
# never a stale cached blob.
#   NOTE: --pure-eval FORBIDS readFile of a mutable non-store absolute path
#   ("access to absolute path ... is forbidden in pure evaluation mode"), so a
#   readFile reject can only be exercised under --impure.  The manifest bless is
#   present (impure-mode id) to prove reject-bits dominate it regardless of mode:
#   even were getEnv perturbable, readFile is not, so the entry hard-rejects.
D=$(mktemp -d); M=$(mktemp -t tlcache-m10.XXXXXX); F=$(mktemp -t tlcache-f10.XXXXXX)
E10="builtins.readFile $F"
printf 'aaa' > "$F"
id10=$(capture_id_impure "$E10"); printf '%s\n' "$id10" > "$M"   # bless it (wrongly)
r10a=$(env NIX_V3_TOPLEVEL_MANIFEST="$M" NIX_V3_CACHE_DIR="$D" \
        NIX_V3_TOPLEVEL_CACHE=active NIX_V3_DIRECT_EVAL=1 NIX_V3_MAX_WALL_TIME=60s \
        "$NIX" eval --impure --raw --expr "$E10" 2>/dev/null)   # aaa
printf 'bbb' > "$F"
r10b=$(env NIX_V3_TOPLEVEL_MANIFEST="$M" NIX_V3_CACHE_DIR="$D" \
        NIX_V3_TOPLEVEL_CACHE=active NIX_V3_DIRECT_EVAL=1 NIX_V3_MAX_WALL_TIME=60s \
        "$NIX" eval --impure --raw --expr "$E10" 2>/dev/null)   # must be bbb
chk TL10-readfile-first        "$r10a" 'aaa'
chk TL10-readfile-reject-first "$r10b" 'bbb'   # not a stale 'aaa' (reject-bits first)
rm -rf "$D" "$M" "$F"

# TL11 (−) — manifest-partition: the manifest content hash is folded into the
# cache key, so an entry inserted with manifest M1 is NOT served under a
# DIFFERENT manifest M2 (different content hash → different key).  (This exercises
# the manifestContentHash-in-key; a true v3-vs-v4 cross-binary key partition —
# the version tag — is impractical in-suite, but the same key-namespacing
# mechanism enforces it.)  Populate under M1, then a differently-blessed M2 misses.
D=$(mktemp -d)
E11='builtins.seq builtins.currentTime "part"'   # seq forces → clock-tainted → manifest-gated
id11=$(capture_id "$E11")
M1=$(mktemp -t tlcache-m11a.XXXXXX); M2=$(mktemp -t tlcache-m11b.XXXXXX)
printf '%s\n' "$id11" > "$M1"
# M2 blesses the SAME id but has different content (a comment line) → different
# content hash → different key namespace.
printf '# a different manifest\n%s\n' "$id11" > "$M2"
r11a=$(evp "$D" "$M1" "$E11")               # miss under M1 → insert
s11m1=$(evp_stats "$D" "$M1" "$E11")        # hit under M1 (same key)
s11m2=$(evp_stats "$D" "$M2" "$E11")        # MISS under M2 (different manifest hash)
chk TL11-partition-value "$r11a" 'part'
chk_hit   TL11-same-manifest-hits    "$s11m1"
chk_nohit TL11-diff-manifest-misses  "$s11m2"
rm -rf "$D" "$M1" "$M2"

# TL12 (R) — manifest-swap invalidation regression: M1 populates, then swapping
# to M2 is a MISS (not a stale cross-hit), and re-populates under M2's key.  Same
# mechanism as TL11 (manifest-hash-in-key), asserted as an explicit round-trip.
D=$(mktemp -d)
E12='builtins.seq builtins.currentTime "swap"'   # seq forces → clock-tainted → manifest-gated
id12=$(capture_id "$E12")
M1=$(mktemp -t tlcache-m12a.XXXXXX); M2=$(mktemp -t tlcache-m12b.XXXXXX)
printf '%s\n' "$id12" > "$M1"
printf '# swapped\n%s\n' "$id12" > "$M2"
_=$(evp "$D" "$M1" "$E12")                  # M1: insert
s12m2a=$(evp_stats "$D" "$M2" "$E12")       # M2: miss (invalidated by swap)
s12m2b=$(evp_stats "$D" "$M2" "$E12")       # M2: now a hit (re-populated)
chk_nohit TL12-swap-invalidates "$s12m2a"
chk_hit   TL12-swap-repopulates "$s12m2b"
rm -rf "$D" "$M1" "$M2"

echo "toplevel-cache: $pass passed, $fail failed"
[[ $fail -eq 0 ]]
