#!/usr/bin/env bash
#
# v3-vs-tw-gate.sh — a SELF-ASSERTING v3-vs-tree-walker measurement harness.
#
# Why this exists (see lode/MEASUREMENT_GATE_2026-06-07.md): every v3-vs-TW
# number that flipped under scrutiny was not an arithmetic error — it was
# "we measured something other than what we claimed": v3 never engaged
# (M5/cardano ran TW), arms did different work (firefox fake-store), the
# comparand was intra-v3 (register-VM 1.5×), the baseline was unpinned
# (fork-2.35 vs stock-2.34.7), or the path was non-production (fib --expr).
#
# This harness REFUSES TO EMIT A NUMBER until the preconditions that bind the
# number to a "v3 vs TW" claim are verified. Abort-on-unmet-precondition, not
# best-effort-report. Every assertion is printed so a human can sanity-check.
#
# Outcomes per workload (the honest decision tree):
#   1. v3 not engaged          -> ABORT (would be TW-vs-TW; refuse)
#   2. v3 engaged, incomplete  -> report "v3 INCOMPLETE" (the cardano case)
#   3. engaged, result differs -> "DIVERGENT" flag, refuse the parity claim
#   4. engaged + identical      -> clean v3-vs-TW row, all gates green
#
# Usage:
#   v3-vs-tw-gate.sh '<nix-expr>'        measure one workload (v3 vs TW)
#   v3-vs-tw-gate.sh --selftest          validate the engagement detector itself
#
# Env knobs (all optional):
#   NIX_BIN          path to the nix under test       (default: repo build)
#   RUNS=3           timed repetitions (min user-CPU)  load-insensitivity
#   V3_WALL=120s     v3 wall-time cap (clean timeout for pathological evals)
#   V3_HEAP=6G       v3 heap cap
#   ALLOW_DIVERGENT  set=1 to still print metrics on a DIVERGENT result
#
# NOTE: builds nothing, touches no engine source. Pure measurement.

set -uo pipefail   # deliberately NOT -e: a v3 timeout is DATA, not a crash.

# ── config ─────────────────────────────────────────────────────────────────
SELF_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(git -C "$SELF_DIR" rev-parse --show-toplevel 2>/dev/null || echo "$SELF_DIR/../../..")"
NIX_BIN="${NIX_BIN:-$REPO_ROOT/build/src/nix/nix}"
RUNS="${RUNS:-3}"
V3_WALL="${V3_WALL:-120s}"
V3_HEAP="${V3_HEAP:-6G}"
ALLOW_DIVERGENT="${ALLOW_DIVERGENT:-0}"

# ── pretty ─────────────────────────────────────────────────────────────────
if [[ -t 1 ]]; then B=$'\e[1m'; R=$'\e[0m'; GRN=$'\e[32m'; RED=$'\e[31m'; YEL=$'\e[33m'; CYN=$'\e[36m'
else B=''; R=''; GRN=''; RED=''; YEL=''; CYN=''; fi
pass() { printf '  %s✓%s %s\n' "$GRN" "$R" "$1"; }
fail() { printf '  %s✗%s %s\n' "$RED" "$R" "$1"; }
warn() { printf '  %s!%s %s\n' "$YEL" "$R" "$1"; }
hdr()  { printf '%s── %s ──%s\n' "$B" "$1" "$R"; }

TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT

# run_timed <tag> <env-prefix-or-empty> -- <nix args...>
#   captures stdout->$TMP/<tag>.out, stderr->$TMP/<tag>.err over RUNS reps,
#   writes min user-CPU to $TMP/<tag>.user, max maxRSS(bytes) to $TMP/<tag>.rss,
#   and the LAST exit code to $TMP/<tag>.rc.
run_timed() {
  local tag="$1"; shift
  local -a envp=(); while [[ "$1" != "--" ]]; do envp+=("$1"); shift; done; shift
  local min_user="" max_rss=0 rc=0 i
  for ((i=0; i<RUNS; i++)); do
    /usr/bin/env "${envp[@]}" /usr/bin/time -l "$NIX_BIN" "$@" \
        >"$TMP/$tag.out" 2>"$TMP/$tag.err"
    rc=$?
    local u r
    u="$(grep -oE '[0-9]+\.[0-9]+ user' "$TMP/$tag.err" | grep -oE '^[0-9.]+' | head -1)"
    r="$(grep -E 'maximum resident set size' "$TMP/$tag.err" | grep -oE '[0-9]+' | head -1)"
    [[ -n "$u" ]] && { [[ -z "$min_user" ]] || awk "BEGIN{exit !($u < $min_user)}" && min_user="$u"; }
    [[ -n "$r" && "$r" -gt "$max_rss" ]] && max_rss="$r"
  done
  echo "${min_user:-NA}" >"$TMP/$tag.user"
  echo "$max_rss"        >"$TMP/$tag.rss"
  echo "$rc"             >"$TMP/$tag.rc"
}

# v3 engaged iff its stderr carries a 'v3-direct' line (empirically verified
# discriminator: TW emits none). This is the assertion the M5 run lacked.
v3_engaged() { grep -q 'v3-direct' "$1"; }
v3_insns()   { grep -oE 'insns=[0-9]+' "$1" | grep -oE '[0-9]+' | tail -1; }

mb() { awk "BEGIN{printf \"%.1f\", $1/1048576}"; }
ratio() { awk "BEGIN{ if ($2+0==0||\"$1\"==\"NA\"||\"$2\"==\"NA\") print \"NA\"; else printf \"%.2f\", $1/$2 }"; }

# ── self-test: prove the detector distinguishes v3 from TW ───────────────────
if [[ "${1:-}" == "--selftest" ]]; then
  hdr "SELFTEST — does the engagement detector actually work?"
  EXPR='1 + 1'
  NIX_V3_DIRECT_EVAL=1 NIX_VM_STATS=1 "$NIX_BIN" eval --expr "$EXPR" >/dev/null 2>"$TMP/eng.err" || true
  NIX_VM_STATS=1                       "$NIX_BIN" eval --expr "$EXPR" >/dev/null 2>"$TMP/tw.err"  || true
  ok=1
  if v3_engaged "$TMP/eng.err"; then pass "DIRECT=1 run detected as v3-ENGAGED (insns=$(v3_insns "$TMP/eng.err"))"
  else fail "DIRECT=1 run NOT detected as engaged — detector is broken"; ok=0; fi
  if v3_engaged "$TMP/tw.err"; then fail "plain run FALSELY detected as v3 — detector false-positives"; ok=0
  else pass "plain run detected as TW (not engaged) — no false positive"; fi
  echo
  if [[ "$ok" == 1 ]]; then printf '%sSELFTEST PASS%s — the gate can tell v3 from TW.\n' "$GRN" "$R"; exit 0
  else printf '%sSELFTEST FAIL%s — do NOT trust this harness until fixed.\n' "$RED" "$R"; exit 1; fi
fi

# ── main ─────────────────────────────────────────────────────────────────────
[[ $# -ge 1 ]] || { echo "usage: $0 '<nix-expr>'   |   $0 --selftest" >&2; exit 2; }
EXPR="$1"
[[ -x "$NIX_BIN" ]] || { echo "nix binary not found/executable: $NIX_BIN (set NIX_BIN=)" >&2; exit 2; }

printf '%sworkload%s : %s\n' "$B" "$R" "$EXPR"
printf '%snix     %s : %s\n' "$B" "$R" "$NIX_BIN"
printf '%sruns    %s : %s (report min user-CPU, max peak-RSS)\n\n' "$B" "$R" "$RUNS"

# PINNED-REF: record exactly which TW we compare against.
hdr "tw-ref (PINNED-REF assertion)"
TW_VER="$("$NIX_BIN" --version 2>/dev/null | head -1)"
pass "TW baseline = ${TW_VER:-unknown}  @ $NIX_BIN"
echo

# TW arm: NO DIRECT → tree-walker. v3 arm: DIRECT + stats + clean caps.
run_timed tw  "NIX_VM_STATS=1" -- eval --impure --expr "$EXPR"
run_timed v3  "NIX_V3_DIRECT_EVAL=1" "NIX_VM_STATS=1" \
              "NIX_V3_MAX_WALL_TIME=$V3_WALL" "NIX_V3_MAX_HEAP=$V3_HEAP" \
              -- eval --impure --expr "$EXPR"

TW_OUT="$(cat "$TMP/tw.out")"; V3_OUT="$(cat "$TMP/v3.out")"
TW_RC="$(cat "$TMP/tw.rc")";   V3_RC="$(cat "$TMP/v3.rc")"

# ── the gate ─────────────────────────────────────────────────────────────────
hdr "preconditions"

# (1) ENGAGED — the assertion the M5 measurement lacked.
if v3_engaged "$TMP/v3.err"; then
  pass "ENGAGED: v3 ran (v3-direct stats present, insns=$(v3_insns "$TMP/v3.err"))"
else
  fail "ENGAGED: v3 did NOT run — no v3-direct stats. This would be TW-vs-TW."
  printf '\n%sABORT%s — refusing to emit a v3-vs-TW number when v3 never engaged.\n' "$RED" "$R"
  printf '       (likely a flake installable; NIX_V3_DIRECT_EVAL only hooks --expr.)\n'
  exit 3
fi

# COMPARAND is structurally enforced: tw arm has no DIRECT, v3 arm does.
if v3_engaged "$TMP/tw.err"; then warn "COMPARAND: TW control unexpectedly shows v3 stats — investigate"
else pass "COMPARAND: v3-vs-TW (TW arm is genuinely the tree-walker)"; fi

# (2) COMPLETION — a v3 timeout/error is honest data, not a number.
if [[ "$V3_RC" != "0" ]]; then
  warn "COMPLETION: v3 did NOT complete (exit $V3_RC) — see below"
  echo
  hdr "result: v3 INCOMPLETE"
  printf '  TW completed in %ss user, %s MB peak.\n' "$(cat "$TMP/tw.user")" "$(mb "$(cat "$TMP/tw.rss")")"
  printf '  v3 failed: %s\n' "$(grep -iE 'error|exceeded|timeout' "$TMP/v3.err" | head -1)"
  printf '  %sVERDICT%s: v3 cannot complete this workload — no ratio (this is the result).\n' "$B" "$R"
  exit 0
fi

# (3) IDENTICAL — catches fake-store / divergent-work asymmetry.
DIVERGENT=0
if [[ "$TW_OUT" == "$V3_OUT" ]]; then
  pass "IDENTICAL: v3 result == TW result (byte-for-byte) — correctness verified"
else
  fail "IDENTICAL: results DIFFER — arms did different work (fake-store? divergence?)"
  printf '      TW: %s\n      v3: %s\n' "$TW_OUT" "$V3_OUT"
  DIVERGENT=1
fi

# ── report ───────────────────────────────────────────────────────────────────
echo
TW_U="$(cat "$TMP/tw.user")"; V3_U="$(cat "$TMP/v3.user")"
TW_M="$(cat "$TMP/tw.rss")";  V3_M="$(cat "$TMP/v3.rss")"

if [[ "$DIVERGENT" == 1 && "$ALLOW_DIVERGENT" != 1 ]]; then
  hdr "result: DIVERGENT — parity claim REFUSED"
  printf '  Metrics withheld: a ratio between non-identical results is meaningless.\n'
  printf '  Re-run with ALLOW_DIVERGENT=1 only if you understand why they differ\n'
  printf '  (e.g. v3 /v3-fake-store/ drvPath: memory ratio valid, correctness NOT).\n'
  exit 0
fi

[[ "$DIVERGENT" == 1 ]] && hdr "result: v3 vs TW  ${YEL}(DIVERGENT — NOT correctness-verified)${R}" \
                        || hdr "result: v3 vs TW  (all gates green)"
printf '  %-14s %12s %12s   %s\n' "metric" "TW" "v3" "v3 vs TW"
printf '  %-14s %12s %12s   %sx\n' "user-CPU (s)" "$TW_U" "$V3_U" "$(ratio "$V3_U" "$TW_U")"
printf '  %-14s %11s %11s   %sx\n' "peak-RSS (MB)" "$(mb "$TW_M")" "$(mb "$V3_M")" "$(ratio "$V3_M" "$TW_M")"
echo
printf '  %sNote%s: user-CPU is load-insensitive vs wall; ratios >1 = v3 slower.\n' "$CYN" "$R"
