#!/usr/bin/env bash
# PLAN_BEAT_TW_V2 QG-4 — ratchet guard.
#
# Re-measures the 7 gate rows and compares against the committed
# baselines/seven-rows.tsv.  FAILS (exit 1) if any row regresses
# >3% v3 CPU or >5% v3 arena vs the pinned value, or flips to DIVERGENT.
#
# To legitimately move a row the wrong way, re-pin the TSV
# (`make pin-seven-rows`) in a commit whose body justifies the regression
# — the re-pin commit IS the justification trail QG-4 asks for.
#
# Env: ALLOW_MISSING_M5=1 skips the M5 row if the cardano flake is absent.
#
# Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
# Input Output Group.  SPDX-License-Identifier: Apache-2.0
set -u
SELF_DIR="$(cd "$(dirname "$0")" && pwd)"
BASE="$SELF_DIR/baselines/seven-rows.tsv"
CPU_TOL="${CPU_TOL:-1.03}"   # +3% CPU allowed
RSS_TOL="${RSS_TOL:-1.05}"   # +5% arena allowed

[[ -f "$BASE" ]] || { echo "ratchet-check: no baseline at $BASE — run 'make pin-seven-rows' first" >&2; exit 2; }

echo "ratchet-check: re-measuring 7 rows vs $BASE (CPU tol +3%, arena tol +5%)"
fresh="$(mktemp)"; bash "$SELF_DIR/pin-seven-rows.sh" > "$fresh" || { echo "measurement failed" >&2; exit 2; }

fail=0
printf "%-10s %-22s %-22s %s\n" "row" "CPU pinned→fresh" "arena pinned→fresh" "verdict"
while IFS=$'\t' read -r name v3cpu twcpu ratio arena ident; do
  [[ "$name" == \#* ]] && continue
  # find matching fresh row
  frow="$(awk -F'\t' -v n="$name" '$1==n{print; exit}' "$fresh")"
  [[ -z "$frow" ]] && { printf "%-10s MISSING in fresh run\n" "$name"; fail=1; continue; }
  IFS=$'\t' read -r _ fcpu ftw fratio farena fident <<<"$frow"
  local_fail=""
  awk "BEGIN{exit !(\"$fcpu\"!=\"NA\" && \"$v3cpu\"!=\"NA\" && $fcpu > $v3cpu*$CPU_TOL)}" && local_fail+="CPU "
  awk "BEGIN{exit !(\"$farena\"!=\"NA\" && \"$arena\"!=\"NA\" && $farena > $arena*$RSS_TOL)}" && local_fail+="ARENA "
  [[ "$fident" == "DIVERGENT" ]] && local_fail+="DIVERGENT "
  if [[ -n "$local_fail" ]]; then verdict="FAIL($local_fail)"; fail=1; else verdict="ok"; fi
  printf "%-10s %-22s %-22s %s\n" "$name" "${v3cpu}→${fcpu}" "${arena}→${farena}" "$verdict"
done < "$BASE"
rm -f "$fresh"

if [[ "$fail" == 1 ]]; then
  echo "ratchet-check: FAIL — a row regressed beyond tolerance (re-pin with justification to move it)"; exit 1
fi
echo "ratchet-check: PASS — no row regressed beyond tolerance"

# Freshness guard for the time-series tooling (perf-trace.py / plot-v3-memory.py
# / trace.sh): a fast 1-run smoke that regenerates the overlays on a synthetic
# workload.  Keeps the graphing infra from silently bit-rotting (its prior
# decay — last SVG 2026-05-20 — is what prompted this).  SKIP_TRACE_SMOKE=1
# opts out; the smoke itself skips cleanly if matplotlib/psutil are absent so
# CI never hard-fails on optional plotting libs — it only FAILS if the tooling
# errors when the deps ARE present.
if [[ "${SKIP_TRACE_SMOKE:-0}" != 1 ]]; then
  echo "ratchet-check: trace-smoke (graphing-infra freshness)…"
  if ! SMOKE=1 OUT="$(mktemp -d)/trace-smoke" bash "$SELF_DIR/trace.sh"; then
    echo "ratchet-check: FAIL — trace tooling broke (perf-trace.py/plot-v3-memory.py); SKIP_TRACE_SMOKE=1 to bypass"; exit 1
  fi
fi
