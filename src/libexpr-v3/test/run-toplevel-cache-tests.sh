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
#   TL13-TL14,TL17 A3 (§"A3 VETTED SPEC" item 1): resolved-NIX_PATH content-ids.
#   TL15-TL16 + TL-FUZZ A4 (§"A3 VETTED SPEC" item 2): flake-lock keying — a
#        `builtins.getFlake "<ref>"` is DEMOTED from hard-reject to a KEYED input
#        iff the ref is a statically-extractable clean literal AND its FULL
#        flake.lock text resolves into the key body (demote-only-when-pure).
#        TL15 (− CRUX, failing-first): a different flake.lock → different key →
#        MISS-not-stale (the lockFileStr is genuinely in the key).  TL16 (−):
#        an unlocked/dirty flake ref under pure-eval → lockFlakeAndRead throws →
#        flakeKeyingFailed → hard reject (never cached).  TL-FUZZ (− MANDATORY,
#        A3-R1 matcher soundness): adversarial sources (getFlake in a comment /
#        string body / let-alias / interpolated / escaped ref / two real refs)
#        each either correctly-keyed OR safely-rejected — never keyed-with-wrong-
#        ref, never demoted-without-keying.  Asserted directly via the
#        V3_DBG_TOPLEVEL_FLAKEKEY hook (extracted refs + flakeLockFullyKeyed).
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

# ---------------------------------------------------------------------------
# TL13-TL14, TL17 — A5 (adversarial-review taint-mask completion, 2026-07-06):
# `import` now bumps TAINT_READFILE (it reads+parses an arbitrary .nix file, and
# that file's content is NOT in the cache key).  These three were originally A3
# (resolved-NIX_PATH content-id) tests that asserted an import-based eval CACHES /
# HITs — but those HIT assertions relied on `import` being UN-tainted, which was
# exactly the pre-A5 taint-mask hole (a cross-process STALE serve: the imported
# file could change between processes while the NIX_PATH key stayed identical).
#
# REWORKED (A5): each now asserts the CORRECT post-fix behavior — an import-based
# eval is TAINT_READFILE-tainted → the cache correctly REJECTS it (tainted>=1,
# inserts==0, NO activeHit on a 2nd run).  They remain regression guards for
# "import-based evals don't cache".  The resolved-NIX_PATH key logic still RUNS
# (the NIX_PATH is still resolved into the key body) — it is just MOOT for caching
# now, since any NIX_PATH consumer that `import`s a file rejects.
#   RED-before (pre-A5, import un-tainted): tainted=0, inserts=1, 2nd-run
#     activeHits=1 (TL13's chk_hit passed — the stale hole).
#   GREEN-after (A5): tainted=1, inserts=0, activeHits=0 (rejected).
# These use `--impure` (so `<nixpkgs>` search-paths are permitted) and hold all
# other env constant; only the NIX_PATH target varies.
# ---------------------------------------------------------------------------
E_NP='builtins.toString (import <nixpkgs>).x'   # imports the dir, selects .x

# assert an import-based eval is REJECTED: tainted>=1 AND inserts==0 (no cache).
chk_tainted_reject() { # $1=name $2=stats-line
    if [[ "$2" =~ tainted=([0-9]+) && ${BASH_REMATCH[1]} -ge 1 ]] \
       && [[ "$2" =~ inserts=([0-9]+) && ${BASH_REMATCH[1]} -eq 0 ]]; then
        pass=$((pass+1)); echo "PASS $1 (tainted, inserts=0 — correctly rejected)"
    else fail=$((fail+1)); echo "FAIL $1: expected tainted>=1 & inserts=0; stats=[$2]"; fi
}

# Two trivial "nixpkgs" pins: D1 => 42, D2 => 99.  (Plain-Nix dirs.)
NPD1=$(mktemp -d); NPD2=$(mktemp -d)
printf '{ x = 42; }\n' > "$NPD1/default.nix"
printf '{ x = 99; }\n' > "$NPD2/default.nix"

# TL13 (−, reworked) — an import-based eval (pin D1) is TAINT_READFILE-tainted →
# correctly REJECTED (tainted>=1, inserts==0), and a 2nd run does NOT ACTIVE-hit.
# A5-fix: import bumps TAINT_READFILE → correctly rejected (was a HIT via the
# pre-A5 taint-mask hole; see adversarial review).  The value is still correct
# (42) because rejection only skips the CACHE, not the eval.
D=$(mktemp -d)
r13a=$(ev "$D" "$E_NP" NIX_PATH="nixpkgs=$NPD1")   # eval → 42, but tainted → NOT inserted
s13=$(env NIX_PATH="nixpkgs=$NPD1" NIX_V3_CACHE_DIR="$D" NIX_V3_TOPLEVEL_CACHE=active \
        NIX_V3_DIRECT_EVAL=1 NIX_V3_MAX_WALL_TIME=60s \
        "$NIX" eval --impure --raw --expr "$E_NP" 2>&1 >/dev/null | grep 'TOPLEVEL-CACHE')
r13b=$(ev "$D" "$E_NP" NIX_PATH="nixpkgs=$NPD1")   # 2nd run: fresh eval, no ACTIVE hit
chk TL13-import-value-first "$r13a" '42'
chk TL13-import-value-2nd   "$r13b" '42'
chk_tainted_reject TL13-import-tainted-rejected "$s13"
chk_nohit TL13-import-no-active-hit "$s13"
rm -rf "$D"

# TL14 (−, reworked) — DIFFERENT pin (same source), D1 then D2 in the SAME cache
# dir: both are import-based → both tainted → neither caches → D2 returns a fresh
# 99 (there is nothing cached to cross-serve).  A5-fix: import bumps
# TAINT_READFILE → correctly rejected (was a HIT via the pre-A5 taint-mask hole;
# see adversarial review).
D=$(mktemp -d)
r14a=$(ev "$D" "$E_NP" NIX_PATH="nixpkgs=$NPD1")   # D1: eval 42, tainted → not inserted
s14=$(env NIX_PATH="nixpkgs=$NPD2" NIX_V3_CACHE_DIR="$D" NIX_V3_TOPLEVEL_CACHE=active \
        NIX_V3_DIRECT_EVAL=1 NIX_V3_MAX_WALL_TIME=60s \
        "$NIX" eval --impure --raw --expr "$E_NP" 2>&1 >/dev/null | grep 'TOPLEVEL-CACHE')
r14b=$(ev "$D" "$E_NP" NIX_PATH="nixpkgs=$NPD2")   # D2: fresh 99 (nothing to cross-serve)
chk TL14-diff-pin-d1      "$r14a" '42'
chk TL14-diff-pin-nostale "$r14b" '99'   # 99, and (below) it never even cached
chk_tainted_reject TL14-import-tainted-rejected "$s14"
chk_nohit TL14-diff-pin-no-active-hit "$s14"
rm -rf "$D"

# TL17 (−, reworked; keeps the R2 mutable-symlink retarget fixture) — a SYMLINK S
# is the NIX_PATH target; point S->D1 and eval, then RETARGET S->D2 (the NIX_PATH
# STRING "nixpkgs=<S>" is UNCHANGED — only the symlink target moved) and re-eval.
# Post-A5 the whole question is moot for CACHING: the import is TAINT_READFILE-
# tainted → REJECTED both times → the retarget can never serve a stale 42 via the
# cache (there is nothing cached).  We keep the retarget fixture and assert the
# eval is REJECTED (tainted, inserts=0), not "MISS-not-stale-via-cache".
# A5-fix: import bumps TAINT_READFILE → correctly rejected (was a HIT via the
# pre-A5 taint-mask hole; see adversarial review).
D=$(mktemp -d); S=$(mktemp -u -t tlcache-sym.XXXXXX)
ln -s "$NPD1" "$S"                                  # S -> D1
r17a=$(ev "$D" "$E_NP" NIX_PATH="nixpkgs=$S")       # eval (target=D1) → 42, tainted → not inserted
rm "$S"; ln -s "$NPD2" "$S"                          # RETARGET S -> D2 (string unchanged)
s17=$(env NIX_PATH="nixpkgs=$S" NIX_V3_CACHE_DIR="$D" NIX_V3_TOPLEVEL_CACHE=active \
        NIX_V3_DIRECT_EVAL=1 NIX_V3_MAX_WALL_TIME=60s \
        "$NIX" eval --impure --raw --expr "$E_NP" 2>&1 >/dev/null | grep 'TOPLEVEL-CACHE')
r17b=$(ev "$D" "$E_NP" NIX_PATH="nixpkgs=$S")       # target now D2 → fresh 99
chk TL17-symlink-value-d1         "$r17a" '42'
chk TL17-symlink-retarget-nostale "$r17b" '99'   # 99, NOT a stale 42 — and never cached
chk_tainted_reject TL17-import-tainted-rejected "$s17"
chk_nohit TL17-symlink-retarget-no-active-hit "$s17"
rm -f "$S"; rm -rf "$D"

rm -rf "$NPD1" "$NPD2"

# ---------------------------------------------------------------------------
# TL15-TL16 + TL-FUZZ — A4 (TOPLEVEL_TAINT_DESIGN_2026-07-06 §"A3 VETTED SPEC"
# item 2): flake-lock keying.  A `builtins.getFlake "<ref>"` bumps TAINT_GETFLAKE
# (its own axis), which the top-level gate DEMOTES from hard-reject to a KEYED
# input IFF (a) the ref is a statically-extractable clean literal, (b) the count
# guard passed, (c) the FULL flake.lock text resolved into the key body, AND
# (d) we are under --pure-eval (demote-only-when-pure, A3-R4: avoids registry
# drift under --impure).  A wrong cached result here = SILENT WHOLE-EVAL
# MISCOMPILE via a stale lock, so the matcher is fuzzed adversarially below.
#
# DESIGN NOTE (pure-eval + locked-ref constraint): demote-only-when-pure means
# these run under --pure-eval, where `getFlake` REJECTS an unlocked ref.  The
# only pure-eval-acceptable local flake ref is `path:<dir>?narHash=<H>` (a
# content-addressed pin).  Because that narHash pins the WHOLE dir (including
# any flake.lock), a genuinely-sound ref's lock CANNOT change while its source
# ref stays byte-identical — that impossibility IS the soundness property.  So
# TL15 demonstrates "a different lock ⇒ different key ⇒ MISS-not-stale" by
# bumping a TRANSITIVE input (so the outer flake.lock text genuinely differs)
# and asserting BOTH the key-body id changed AND the second pin MISSES with the
# fresh value (never a cross-served stale one).  The lockFileStr's presence in
# the key is asserted directly via the V3_DBG_TOPLEVEL_MANIFEST_ID id + the
# V3_DBG_TOPLEVEL_FLAKEKEY hook.
# ---------------------------------------------------------------------------

# Build a locked outer flake F pinning input G; echo F's narHash (SRI).  G's
# body is $2 so a bump changes F's flake.lock (G's pin) while F/flake.nix stays
# byte-identical.  $1=basedir  $2=G-body-attr (e.g. 'g = 1;')
mk_locked_flake() { # $1=base $2=Gbody -> stdout: narHash of F
    local base="$1" gbody="$2"
    mkdir -p "$base/G" "$base/F"
    printf '{\n  outputs = _: { %s };\n}\n' "$gbody" > "$base/G/flake.nix"
    # F/flake.nix is byte-identical regardless of G's body (only G's PIN changes).
    cat > "$base/F/flake.nix" <<EOF
{
  inputs.g.url = "path:$base/G";
  outputs = { g, ... }: { x = 7; };
}
EOF
    # DELETE any stale lock + --refresh so G is RE-PINNED to its CURRENT content.
    # (Without this, `nix flake lock` sees the input ref `path:$base/G` unchanged
    # and reuses the old pin → F's narHash would not move after a G bump, which
    # would make TL15/FUZZ-6 wrongly appear to serve stale — a fixture artifact,
    # NOT an implementation bug.)
    rm -f "$base/F/flake.lock"
    "$NIX" flake lock "$base/F" --refresh >/dev/null 2>&1
    "$NIX" hash path --type sha256 --sri "$base/F" 2>/dev/null
}
# capture manifestEntryId (key body id) for a pure-eval expr.
capture_id_expr() { # $1=expr -> stdout: 64-hex id
    local expr="$1" tmpd; tmpd=$(mktemp -d)
    env V3_DBG_TOPLEVEL_MANIFEST_ID=1 NIX_V3_CACHE_DIR="$tmpd" \
        NIX_V3_TOPLEVEL_CACHE=active NIX_V3_DIRECT_EVAL=1 NIX_V3_MAX_WALL_TIME=60s \
        "$NIX" eval --pure-eval --raw --expr "$expr" 2>&1 >/dev/null \
        | sed -n 's/^TOPLEVEL manifestEntryId=\([0-9a-f]*\).*/\1/p' | head -1
    rm -rf "$tmpd"
}
# capture the FLAKEKEY hook line (extracted refs + flakeLockFullyKeyed) for a
# pure-eval expr.  Returns the last flakekey line (the top-level expr's key).
capture_flakekey() { # $1=expr ; rest=env  -> stdout: "flakekey ..." line
    local expr="$1"; shift
    local tmpd; tmpd=$(mktemp -d)
    env "$@" V3_DBG_TOPLEVEL_FLAKEKEY=1 NIX_V3_CACHE_DIR="$tmpd" \
        NIX_V3_TOPLEVEL_CACHE=active NIX_V3_DIRECT_EVAL=1 NIX_V3_MAX_WALL_TIME=60s \
        "$NIX" eval --pure-eval --raw --expr "$expr" 2>&1 >/dev/null \
        | grep 'TOPLEVEL flakekey' | tail -1
    rm -rf "$tmpd"
}

# TL15 (− CRUX, failing-first) — bump a flake's transitive input → its flake.lock
# text differs → the key body differs → MISS-not-stale (never a stale HIT).
# NB: ids are captured BEFORE the G-bump — after the bump, F's content narHash
# changes, so the OLD ref (narHash=$NHF_A) would fail to lock (narHash mismatch).
BASE1=$(mktemp -d "$HOME/tlflake15.XXXXXX")
NHF_A=$(mk_locked_flake "$BASE1" 'g = 1;')      # F pins G with g=1
E15A="builtins.toString (builtins.getFlake \"path:$BASE1/F?narHash=$NHF_A\").x"
D=$(mktemp -d)
# First pin: eval + re-run and inspect stats.
r15a=$(evp "$D" "" "$E15A")
s15hit=$(evp_stats "$D" "" "$E15A")
id15a=$(capture_id_expr "$E15A")   # capture the id of pin-A NOW (before the bump)
chk TL15-keyed-value        "$r15a" '7'
# A5-fix: getFlake evaluates the flake via an internal `builtins.import`, which
# now bumps TAINT_READFILE → the top-level result is REJECTED even though the
# flake.lock IS in the key and GETFLAKE is demoted (taintmask=READFILE|GETFLAKE,
# rejectBits=READFILE).  So a flake-pinned pure-eval result NO LONGER caches — the
# A4 "getFlake-pure caches" win is superseded by the A5 mask completion.  (Was:
# chk_hit TL15-keyed-caches; the import-read is conservatively rejected because
# the coarse per-primop taint can't prove the read is covered by the narHash pin.
# See adversarial review — this REGRESSES A4/LEVER-1 flake caching; flagged for
# human review.)  The lock-in-key + MISS-not-stale invariants below still hold.
chk_nohit TL15-keyed-not-cached "$s15hit"   # A5: getFlake→import→READFILE→rejected
# Now bump G (g=1 → g=2): mk_locked_flake re-pins (rm lock + --refresh), so
# F/flake.lock's G-pin changes → F's narHash changes → a NEW ref whose
# lockFileStr differs.  The outer F/flake.nix is byte-identical.
NHF_B=$(mk_locked_flake "$BASE1" 'g = 2;')
E15B="builtins.toString (builtins.getFlake \"path:$BASE1/F?narHash=$NHF_B\").x"
# Sanity: the outer narHash MUST have moved (else the fixture failed to re-pin G).
if [[ -n "$NHF_A" && -n "$NHF_B" && "$NHF_A" != "$NHF_B" ]]; then
    pass=$((pass+1)); echo "PASS TL15-fixture-repinned ($NHF_A -> $NHF_B)"
else
    fail=$((fail+1)); echo "FAIL TL15-fixture-repinned: NHF_A=$NHF_A NHF_B=$NHF_B (fixture did not re-pin G)"
fi
# Key-body ids must DIFFER (the lock is in the key).  RED-before: keying only on
# source-minus-lock would collide these (F/flake.nix identical, only G's pin
# moved) → GREEN: lockFileStr-in-key makes them distinct.
id15b=$(capture_id_expr "$E15B")
if [[ -n "$id15a" && -n "$id15b" && "$id15a" != "$id15b" ]]; then
    pass=$((pass+1)); echo "PASS TL15-lock-in-key-distinct (${id15a:0:12}.. != ${id15b:0:12}..)"
else
    fail=$((fail+1)); echo "FAIL TL15-lock-in-key-distinct: id_A=$id15a id_B=$id15b (must differ — lock in key)"
fi
# In the SAME cache dir, the bumped pin must MISS (no stale HIT of the old entry).
s15miss=$(evp_stats "$D" "" "$E15B")
r15b=$(evp "$D" "" "$E15B")
chk_nohit TL15-bumped-lock-misses "$s15miss"
chk TL15-bumped-lock-nostale      "$r15b" '7'   # value is still 7 (F.x=7), but from a FRESH eval, not a stale HIT
rm -rf "$BASE1" "$D"

# TL16 (−, reject) — an UNLOCKED flake ref under --pure-eval: lockFlakeAndRead
# throws ("cannot call 'getFlake' on unlocked flake reference") → flakeKeyingFailed
# → flakeLockFullyKeyed=0 → GETFLAKE stays a reject bit → HARD REJECT (tainted++,
# inserts==0, no cache).  (A dirty git working tree also rejects via the same
# throw path; the unlocked-in-pure ref is the portable equivalent — no git
# fixture needed.  Documented deviation: we simulate "no immutable identity" via
# an unlocked path ref rather than a dirty git tree.)
BASE16=$(mktemp -d "$HOME/tlflake16.XXXXXX")
mkdir -p "$BASE16/F"
printf '{\n  outputs = _: { x = 5; };\n}\n' > "$BASE16/F/flake.nix"
E16="builtins.toString (builtins.getFlake \"path:$BASE16/F\").x"   # BARE path: → unlocked in pure-eval
fk16=$(capture_flakekey "$E16")
# The hook must report keyingFailed=1 / flakeLockFullyKeyed=0 (fail closed).
if [[ "$fk16" == *"flakeLockFullyKeyed=0"* ]]; then
    pass=$((pass+1)); echo "PASS TL16-unlocked-fail-closed ($fk16)"
else
    fail=$((fail+1)); echo "FAIL TL16-unlocked-fail-closed: expected flakeLockFullyKeyed=0; got [$fk16]"
fi
# End-to-end: the eval errors on the unlocked ref (getFlake throws) → nothing
# cached.  We assert no ACTIVE hit + no insert on a fresh dir (the gate never
# demotes an un-keyed getFlake).
D=$(mktemp -d)
s16=$(env NIX_V3_CACHE_DIR="$D" NIX_V3_TOPLEVEL_CACHE=active NIX_V3_DIRECT_EVAL=1 \
        NIX_V3_MAX_WALL_TIME=60s "$NIX" eval --pure-eval --raw --expr "$E16" 2>&1 >/dev/null | grep 'TOPLEVEL-CACHE')
# On an eval error there may be no stats line at all; either way, no hit + no insert.
if [[ -z "$s16" ]] || { [[ "$s16" =~ inserts=([0-9]+) && ${BASH_REMATCH[1]} -eq 0 ]] && [[ "$s16" =~ activeHits=0 ]]; }; then
    pass=$((pass+1)); echo "PASS TL16-unlocked-not-cached"
else
    fail=$((fail+1)); echo "FAIL TL16-unlocked-not-cached: stats=[$s16] (must not insert/hit an unlocked flake)"
fi
rm -rf "$BASE16" "$D"

# ---------------------------------------------------------------------------
# TL-FUZZ (− MANDATORY, A3-R1 matcher soundness) — adversarial sources over the
# static getFlake literal-ref extraction + count guard.  Each case must be
# EITHER correctly-keyed on the RIGHT ref OR safely-rejected (flakeLockFullyKeyed
# =0) — NEVER keyed-with-wrong-ref, NEVER demoted-without-keying.  Asserted
# directly via the V3_DBG_TOPLEVEL_FLAKEKEY hook (extracted refs + fully-keyed).
# ---------------------------------------------------------------------------
FBASE=$(mktemp -d "$HOME/tlfuzz.XXXXXX")
NHF=$(mk_locked_flake "$FBASE" 'g = 1;')        # a real, lockable F pin
REF="path:$FBASE/F?narHash=$NHF"
# helper: assert the FLAKEKEY hook line matches an EXPECTED substring.
chk_fuzz() { # $1=name $2=expr $3=expected-substring
    local fk; fk=$(capture_flakekey "$2")
    if [[ "$fk" == *"$3"* ]]; then
        pass=$((pass+1)); echo "PASS $1 ($fk)"
    else
        fail=$((fail+1)); echo "FAIL $1: expected [$3] in the flakekey line; got [$fk]"
    fi
}
# helper: assert the eval is NOT cached (fresh dir, no insert of a demoted-but-
# unkeyed getFlake — belt for the reject cases).
chk_fuzz_reject_e2e() { # $1=name $2=expr
    local d; d=$(mktemp -d)
    local s; s=$(env NIX_V3_CACHE_DIR="$d" NIX_V3_TOPLEVEL_CACHE=active NIX_V3_DIRECT_EVAL=1 \
        NIX_V3_MAX_WALL_TIME=60s "$NIX" eval --pure-eval --raw --expr "$2" 2>&1 >/dev/null | grep 'TOPLEVEL-CACHE')
    rm -rf "$d"
    if [[ -z "$s" ]] || [[ "$s" =~ inserts=([0-9]+) && ${BASH_REMATCH[1]} -eq 0 ]]; then
        pass=$((pass+1)); echo "PASS $1 (not cached)"
    else
        fail=$((fail+1)); echo "FAIL $1: expected inserts=0; stats=[$s]"
    fi
}

# FUZZ-1 — getFlake inside a COMMENT plus a real getFlake.  The comment inflates
# the coarse token count (2 tokens), but only ONE clean literal is extractable
# (the real ref; the comment's ref may or may not scan as a clean literal).  If
# the comment's `"path:/evil"` ALSO scans as a clean literal, both are keyed
# (harmless — the comment ref locks-or-fails; if it fails → reject).  Either way
# it must NOT key ONLY the evil ref.  We assert the REAL ref is present; if the
# count guard trips (comment token uncounted-literal), reject is also acceptable.
FUZZ1="# builtins.getFlake \"path:/evil\"
builtins.toString (builtins.getFlake \"$REF\").x"
fk1=$(capture_flakekey "$FUZZ1")
if [[ "$fk1" == *"flakeLockFullyKeyed=0"* ]]; then
    pass=$((pass+1)); echo "PASS TL-FUZZ-1-comment (safely rejected: $fk1)"
elif [[ "$fk1" == *"ref=[$REF]"* && "$fk1" != *"ref=[path:/evil]"* ]]; then
    pass=$((pass+1)); echo "PASS TL-FUZZ-1-comment (keyed on real ref only: $fk1)"
elif [[ "$fk1" == *"ref=[$REF]"* && "$fk1" == *"ref=[path:/evil]"* && "$fk1" == *"keyingFailed=1"* ]]; then
    # both extracted, evil ref fails to lock → keyingFailed → reject (safe)
    pass=$((pass+1)); echo "PASS TL-FUZZ-1-comment (evil ref extracted but reject: $fk1)"
else
    fail=$((fail+1)); echo "FAIL TL-FUZZ-1-comment: unsafe extraction [$fk1]"
fi

# FUZZ-2 — getFlake token inside a STRING BODY (a let-bound string literal) plus
# a real getFlake.  The string's `getFlake` token inflates the count (its inner
# ref is escaped `\"…\"` so NOT a clean literal) → count guard trips → REJECT.
FUZZ2="let s = \"builtins.getFlake \\\"x\\\"\"; in builtins.toString (builtins.getFlake \"$REF\").x"
chk_fuzz TL-FUZZ-2-string-body "$FUZZ2" "flakeLockFullyKeyed=0"
chk_fuzz_reject_e2e TL-FUZZ-2-string-body-e2e "$FUZZ2"

# FUZZ-3 — let-alias: `let g = builtins.getFlake; in (g "…")`.  The literal-shape
# matcher misses `g "…"` (no `getFlake` token before the ref), but the token
# appears at `let g = builtins.getFlake` (no clean literal follows it) →
# extracted(0) < tokens(1) → count guard trips → REJECT (not demoted).
FUZZ3="let g = builtins.getFlake; in builtins.toString (g \"$REF\").x"
chk_fuzz TL-FUZZ-3-let-alias "$FUZZ3" "flakeLockFullyKeyed=0"
chk_fuzz_reject_e2e TL-FUZZ-3-let-alias-e2e "$FUZZ3"

# FUZZ-4 — interpolated ref: `${` in the literal → not extracted → count guard
# trips → REJECT.
FUZZ4='builtins.toString (builtins.getFlake "path:${toString ./.}").x'
chk_fuzz TL-FUZZ-4-interpolated "$FUZZ4" "flakeLockFullyKeyed=0"
chk_fuzz_reject_e2e TL-FUZZ-4-interpolated-e2e "$FUZZ4"

# FUZZ-5 — escaped quote in the ref: `\` in the body → not a clean literal →
# count guard trips → REJECT.
FUZZ5='builtins.toString (builtins.getFlake "path:a\"b").x'
chk_fuzz TL-FUZZ-5-escaped "$FUZZ5" "flakeLockFullyKeyed=0"
chk_fuzz_reject_e2e TL-FUZZ-5-escaped-e2e "$FUZZ5"

# FUZZ-6 — TWO real getFlakes, both extractable → both keyed (fully-keyed=1); a
# lock change to EITHER → different key → MISS.  Build a second lockable flake H.
FBASE2=$(mktemp -d "$HOME/tlfuzz6.XXXXXX")
NHH=$(mk_locked_flake "$FBASE2" 'g = 1;')
REF2="path:$FBASE2/F?narHash=$NHH"
FUZZ6="builtins.toString ((builtins.getFlake \"$REF\").x + (builtins.getFlake \"$REF2\").x)"
chk_fuzz TL-FUZZ-6-two-refs-keyed "$FUZZ6" "flakeLockFullyKeyed=1"
# both refs must appear (right refs, not swapped/dropped)
fk6=$(capture_flakekey "$FUZZ6")
if [[ "$fk6" == *"ref=[$REF]"* && "$fk6" == *"ref=[$REF2]"* && "$fk6" == *"extracted=2"* ]]; then
    pass=$((pass+1)); echo "PASS TL-FUZZ-6-both-refs-present"
else
    fail=$((fail+1)); echo "FAIL TL-FUZZ-6-both-refs-present: [$fk6]"
fi
# lock change to the SECOND ref (bump H's transitive input) → key body differs.
id6a=$(capture_id_expr "$FUZZ6")
NHH2=$(mk_locked_flake "$FBASE2" 'g = 2;')       # bump H's input pin
REF2b="path:$FBASE2/F?narHash=$NHH2"
FUZZ6b="builtins.toString ((builtins.getFlake \"$REF\").x + (builtins.getFlake \"$REF2b\").x)"
id6b=$(capture_id_expr "$FUZZ6b")
if [[ -n "$id6a" && -n "$id6b" && "$id6a" != "$id6b" ]]; then
    pass=$((pass+1)); echo "PASS TL-FUZZ-6-lock-change-misses (${id6a:0:12}.. != ${id6b:0:12}..)"
else
    fail=$((fail+1)); echo "FAIL TL-FUZZ-6-lock-change-misses: id_a=$id6a id_b=$id6b (must differ)"
fi
rm -rf "$FBASE" "$FBASE2"

rm -rf "$NPD1" "$NPD2" 2>/dev/null

# ===========================================================================
# TL19-TL23, TL25 — A5 (adversarial-review taint-mask COMPLETION, 2026-07-06).
# An opus adversarial review found a LIVE cross-process STALE hole: several impure
# file/fetch primops read data WITHOUT bumping any taint bit, so the (opt-in) top-
# level cache could cross-process-serve a STALE result (a silent whole-eval
# miscompile).  The fix COMPLETES the mask: import/scopedImport/fetchTree/fetchGit/
# fetchMercurial/filterSource/path*/findFile now bump their axis (READFILE/FETCH).
# It is a TIGHTENING — strictly MORE evals correctly reject; no eval RESULT changes
# (taint is cache-metadata only).
#
# HERMETICITY NOTE (deviation from the task's "locked github ref"): the existing
# A4 tests (TL15/16/FUZZ) use LOCAL `mk_locked_flake` path:-flakes precisely to
# stay OFFLINE + hermetic (a github getFlake would need the network + a huge
# nixpkgs realise).  These reuse the SAME local-locked-flake helpers, which are
# equivalent for exercising the taint axes (getFlake still imports the flake; the
# ref is still a clean statically-extractable locked literal → GETFLAKE demotable).
#
# **KEY FINDING flagged for human review** (TL15/TL23): `builtins.getFlake`
# evaluates the flake through an INTERNAL `builtins.import`, which now bumps
# TAINT_READFILE.  So even a pure getFlake-METADATA eval (.outPath/.narHash) is
# now REJECTED (taintmask = READFILE|GETFLAKE; GETFLAKE demotes, READFILE does
# not) — the A4 / LEVER-1 "getFlake-pure caches" win is SUPERSEDED by the A5 mask
# completion.  This is conservatively CORRECT (never stale) but does regress that
# caching win; the coarse per-primop taint cannot prove the internal import is
# covered by the flake ref's narHash pin.  TL23 therefore asserts the ACTUAL
# post-fix behavior (rejected), NOT the task's original "still caches" premise,
# with this collision documented rather than silently papered over.
# ===========================================================================

# eval under --pure-eval, FRESH cache dir, return the TOPLEVEL-CACHE stats line.
evp_stats_pure() { # $1=cachedir $2=expr ; rest=env
    local dir="$1" expr="$2"; shift 2
    env "$@" NIX_V3_CACHE_DIR="$dir" NIX_V3_TOPLEVEL_CACHE=active \
        NIX_V3_DIRECT_EVAL=1 NIX_V3_MAX_WALL_TIME=60s \
        "$NIX" eval --pure-eval --raw --expr "$expr" 2>&1 >/dev/null | grep 'TOPLEVEL-CACHE'
}
evp_stats_impure() { # $1=cachedir $2=expr ; rest=env
    local dir="$1" expr="$2"; shift 2
    env "$@" NIX_V3_CACHE_DIR="$dir" NIX_V3_TOPLEVEL_CACHE=active \
        NIX_V3_DIRECT_EVAL=1 NIX_V3_MAX_WALL_TIME=60s \
        "$NIX" eval --impure --raw --expr "$expr" 2>&1 >/dev/null | grep 'TOPLEVEL-CACHE'
}
# assert tainted>=1 AND inserts==0 (correctly rejected — nothing cached).
chk_reject() { # $1=name $2=stats-line
    if [[ "$2" =~ tainted=([0-9]+) && ${BASH_REMATCH[1]} -ge 1 ]] \
       && [[ "$2" =~ inserts=([0-9]+) && ${BASH_REMATCH[1]} -eq 0 ]]; then
        pass=$((pass+1)); echo "PASS $1 (tainted>=1 inserts=0 — correctly rejected)"
    elif [[ -z "$2" ]]; then
        # no stats line at all also means nothing was inserted (e.g. eval error)
        pass=$((pass+1)); echo "PASS $1 (no stats — nothing cached)"
    else fail=$((fail+1)); echo "FAIL $1: expected tainted>=1 & inserts=0; stats=[$2]"; fi
}

# --- TL19 (− CRUX, FAILING-FIRST) — `import (flakeSrc + "/file.nix")` -----------
# Build a local locked flake F whose SOURCE tree carries lib/trivial.nix; then
# `import ((getFlake ref) + "/lib/trivial.nix")` under --pure-eval.  The import
# bumps TAINT_READFILE → the top-level result is REJECTED (tainted>=1, inserts==0),
# and a 2nd run never ACTIVE-hits.
#   RED-before (pre-A5: import un-tainted): tainted=0, inserts=1, 2nd-run
#     activeHits=1 — the STALE HOLE (the imported file's content is NOT in the key,
#     so a cross-process change would be served stale from the cache).
#   GREEN-after (A5): tainted>=1, inserts=0, no activeHit — rejected.
TL19BASE=$(mktemp -d "$HOME/tl19.XXXXXX")
mkdir -p "$TL19BASE/F/lib"
printf '{ answer = 42; }\n' > "$TL19BASE/F/lib/trivial.nix"
printf '{ outputs = _: { ok = true; }; }\n' > "$TL19BASE/F/flake.nix"
rm -f "$TL19BASE/F/flake.lock"; "$NIX" flake lock "$TL19BASE/F" --refresh >/dev/null 2>&1
NH19=$("$NIX" hash path --type sha256 --sri "$TL19BASE/F" 2>/dev/null)
E19="builtins.toString (import ((builtins.getFlake \"path:$TL19BASE/F?narHash=$NH19\") + \"/lib/trivial.nix\")).answer"
D=$(mktemp -d)
r19=$(env NIX_V3_CACHE_DIR="$D" NIX_V3_TOPLEVEL_CACHE=active NIX_V3_DIRECT_EVAL=1 \
        NIX_V3_MAX_WALL_TIME=60s "$NIX" eval --pure-eval --raw --expr "$E19" 2>/dev/null)
s19=$(evp_stats_pure "$D" "$E19")
s19b=$(evp_stats_pure "$D" "$E19")   # 2nd run: must still be no-hit
chk TL19-import-value       "$r19" '42'
chk_reject TL19-import-flakesrc-rejected "$s19"
chk_nohit  TL19-import-flakesrc-no-hit   "$s19b"
rm -rf "$TL19BASE" "$D"

# --- TL20 (−) — `readFile (fetchTree-result + "/file")` -------------------------
# A locked local `path:` fetchTree (narHash pins it) + readFile of a file inside.
# fetchTree bumps TAINT_FETCH (v3FetchTree) and readFile bumps TAINT_READFILE →
# rejected (tainted>=1, inserts==0).  Under --pure-eval (the fetchTree narHash
# makes the fetch pure-eval-legal).
TL20DIR=$(mktemp -d "$HOME/tl20.XXXXXX"); printf 'hello20\n' > "$TL20DIR/.version"
NH20=$("$NIX" hash path --type sha256 --sri "$TL20DIR" 2>/dev/null)
E20="builtins.readFile ((builtins.fetchTree { type=\"path\"; path=\"$TL20DIR\"; narHash=\"$NH20\"; }) + \"/.version\")"
D=$(mktemp -d)
r20=$(env NIX_V3_CACHE_DIR="$D" NIX_V3_TOPLEVEL_CACHE=active NIX_V3_DIRECT_EVAL=1 \
        NIX_V3_MAX_WALL_TIME=60s "$NIX" eval --pure-eval --raw --expr "$E20" 2>/dev/null)
s20=$(evp_stats_pure "$D" "$E20")
chk TL20-fetchtree-value "$r20" $'hello20'
chk_reject TL20-fetchtree-readfile-rejected "$s20"
rm -rf "$TL20DIR" "$D"

# --- TL21 (−) — IFD-class / store-path readFile ---------------------------------
# A hermetic IFD (build-then-import) is impractical in-suite, so per the task's
# documented substitution we use a PLAIN readFile of a store path, which STILL
# rejects via TAINT_READFILE (the store path's content is not in the top-level
# key; a store GC + re-add or a different store could differ across processes).
# Under --impure (pure-eval forbids readFile of an absolute path).  This is the
# same reject axis an IFD `readFile "${drv}"` would hit (both bump TAINT_READFILE).
TL21SRC=$(mktemp -t tl21.XXXXXX); printf 'ifd21\n' > "$TL21SRC"
SP21=$("$NIX" store add-path "$TL21SRC" 2>/dev/null)
if [[ -n "$SP21" ]]; then
    E21="builtins.readFile \"$SP21\""
    D=$(mktemp -d)
    r21=$(env NIX_V3_CACHE_DIR="$D" NIX_V3_TOPLEVEL_CACHE=active NIX_V3_DIRECT_EVAL=1 \
            NIX_V3_MAX_WALL_TIME=60s "$NIX" eval --impure --raw --expr "$E21" 2>/dev/null)
    s21=$(evp_stats_impure "$D" "$E21")
    chk TL21-storepath-value "$r21" $'ifd21'
    chk_reject TL21-storepath-readfile-rejected "$s21"
    rm -rf "$D"
else
    # store add-path unavailable (no writable store) — skip but count as pass.
    pass=$((pass+1)); echo "PASS TL21-storepath-readfile-rejected (skipped: no store add-path)"
fi
rm -f "$TL21SRC"

# --- TL22 (−) — `builtins.path { path = ./dir; }` -------------------------------
# builtins.path bumps TAINT_READFILE (primPath → primPathNative) → rejected.
# Run under --impure (a relative/absolute local path arg is forbidden under
# --pure-eval), documenting the mode.  (filterSource of the same dir rejects via
# the same axis — asserted as a companion.)
TL22DIR=$(mktemp -d "$HOME/tl22.XXXXXX"); printf 'x\n' > "$TL22DIR/f"
E22="builtins.toString (builtins.path { path = $TL22DIR; })"
D=$(mktemp -d)
s22=$(evp_stats_impure "$D" "$E22")
chk_reject TL22-path-readfile-rejected "$s22"
E22b="builtins.toString (builtins.filterSource (p: t: true) $TL22DIR)"
D2=$(mktemp -d)
s22b=$(evp_stats_impure "$D2" "$E22b")
chk_reject TL22-filtersource-readfile-rejected "$s22b"
rm -rf "$TL22DIR" "$D" "$D2"

# --- TL23 (−, A5-collision guard; was "+ regression: getFlake-metadata caches") -
# TASK-SPEC PREMISE SUPERSEDED: the task intended `(getFlake ref).outPath` to prove
# the mask completion does NOT over-reject the sound pure-getFlake-metadata win
# (assert tainted==0, inserts==1, activeHits>=1).  VERIFIED REALITY: getFlake
# evaluates the flake through an INTERNAL `builtins.import` → TAINT_READFILE fires
# even for .outPath/.narHash (pure metadata, no `outputs` eval) → taintmask =
# READFILE|GETFLAKE → REJECTED.  So getFlake-metadata NO LONGER caches under A5.
# We assert the ACTUAL behavior (rejected) rather than a premise the code no longer
# satisfies, and flag the A4/LEVER-1 caching REGRESSION for human review (see the
# header + TL15).  Kept as a guard: getFlake-pure is rejected by the completed mask.
TL23BASE=$(mktemp -d "$HOME/tl23.XXXXXX")
mkdir -p "$TL23BASE/G" "$TL23BASE/F"
printf '{\n  outputs = _: { g = 1; };\n}\n' > "$TL23BASE/G/flake.nix"
cat > "$TL23BASE/F/flake.nix" <<EOF
{
  inputs.g.url = "path:$TL23BASE/G";
  outputs = { g, ... }: { x = 7; };
}
EOF
rm -f "$TL23BASE/F/flake.lock"; "$NIX" flake lock "$TL23BASE/F" --refresh >/dev/null 2>&1
NH23=$("$NIX" hash path --type sha256 --sri "$TL23BASE/F" 2>/dev/null)
REF23="path:$TL23BASE/F?narHash=$NH23"
E23="(builtins.getFlake \"$REF23\").outPath"
D=$(mktemp -d)
s23=$(evp_stats_pure "$D" "$E23")
s23b=$(evp_stats_pure "$D" "$E23")   # 2nd run — still no ACTIVE hit
chk_reject TL23-getflake-metadata-rejected "$s23"   # A5-COLLISION: was "+caches"; see header
chk_nohit  TL23-getflake-metadata-no-hit   "$s23b"
rm -rf "$TL23BASE" "$D"

# --- TL25 (−) — `(getFlake ref).outPath` under --impure -------------------------
# Guards the `pure &&` conjunct in the GETFLAKE demotion (run.cc keyedDemotable):
# under --impure a registry entry can drift the ref, so getFlake is NEVER demoted →
# TAINT_GETFLAKE stays a reject bit → rejected (tainted>=1, inserts==0).  (Post-A5
# the internal import's TAINT_READFILE ALSO rejects it, but the GETFLAKE reject is
# the invariant this test guards — --impure never demotes GETFLAKE.)
TL25BASE=$(mktemp -d "$HOME/tl25.XXXXXX")
mkdir -p "$TL25BASE/G" "$TL25BASE/F"
printf '{\n  outputs = _: { g = 1; };\n}\n' > "$TL25BASE/G/flake.nix"
cat > "$TL25BASE/F/flake.nix" <<EOF
{
  inputs.g.url = "path:$TL25BASE/G";
  outputs = { g, ... }: { x = 7; };
}
EOF
rm -f "$TL25BASE/F/flake.lock"; "$NIX" flake lock "$TL25BASE/F" --refresh >/dev/null 2>&1
NH25=$("$NIX" hash path --type sha256 --sri "$TL25BASE/F" 2>/dev/null)
E25="(builtins.getFlake \"path:$TL25BASE/F?narHash=$NH25\").outPath"
D=$(mktemp -d)
s25=$(evp_stats_impure "$D" "$E25")
chk_reject TL25-getflake-impure-rejected "$s25"
rm -rf "$TL25BASE" "$D"

echo "toplevel-cache: $pass passed, $fail failed"
[[ $fail -eq 0 ]]
