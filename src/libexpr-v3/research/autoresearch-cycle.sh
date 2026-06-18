#!/usr/bin/env bash
# ════════════════════════════════════════════════════════════════════════════
#  autoresearch-cycle.sh — ONE deterministic v3-autoresearch experiment.
#
#  The gate-wrapped analog of autoresearch's "train 5 min → eval val_bpb → keep
#  or revert".  Given a candidate engine state (the checkout this script lives
#  in — run it from a git worktree for isolation), it rebuilds, measures v3 vs
#  TW on a target row, and emits a MACHINE-READABLE VERDICT.  It reuses the
#  existing measurement contract from bench/pin-seven-rows.sh (NIX_VM_STATS
#  v3_arena + /usr/bin/time -l user CPU) and the pinned baseline.
#
#  Design + rationale: lode/AUTORESEARCH_V3_DESIGN_2026-06-16.md.
#  Spec / constraints / bars: research/program.md.
#
#  VERDICTS (precedence high→low):
#    BUILD-FAIL          rebuild failed
#    ABORT-NOT-ENGAGED   v3 arm showed no v3-direct/v3_arena stats line
#                        (broke v3-direct routing, or we measured TW) — DISTRUST
#    REVERT-DIVERGENT    v3 result != TW result — CORRECTNESS, non-negotiable
#    REVERT-REGRESS      objective metric worse than baseline beyond tolerance,
#                        OR a hard-constraint failure
#    KEEP-CANDIDATE      objective improved ≥ KEEP_BAR, byte-identical, engaged
#    NEUTRAL             within noise (|Δ| < keep bar) — revert to avoid carcasses
#
#  Usage:
#    autoresearch-cycle.sh --row git --metric cpu
#    autoresearch-cycle.sh --row firefox --metric arena
#    autoresearch-cycle.sh --expr '1 + 1' --impure 0 --baseline-cpu 0.5 --metric cpu
#    autoresearch-cycle.sh --no-build --row fib --metric cpu     # measure current binary
#    autoresearch-cycle.sh --selftest                            # ~1s, no build, no mutation
#
#  Knobs (env):  NIX=…  RUNS=3  KEEP_BAR=0.97  CPU_REGRESS_TOL=1.03
#                ARENA_REGRESS_TOL=1.05  BUILD_CMD='nix develop -c ninja -C build src/nix/nix'
#                JOURNAL=research/run-journal.tsv  CN_PATH=…
#
#  Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
#  Input Output Group.  SPDX-License-Identifier: Apache-2.0
# ════════════════════════════════════════════════════════════════════════════
set -u

SELF_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SELF_DIR/../../.." && pwd)"          # repo root of THIS checkout/worktree
NIX="${NIX:-$ROOT/build/src/nix/nix}"
RUNS="${RUNS:-3}"
KEEP_BAR="${KEEP_BAR:-0.97}"                       # objective ≤ baseline×0.97 ⇒ ≥3% better
CPU_REGRESS_TOL="${CPU_REGRESS_TOL:-1.03}"
ARENA_REGRESS_TOL="${ARENA_REGRESS_TOL:-1.05}"
BUILD_CMD="${BUILD_CMD:-nix develop -c ninja -C build src/nix/nix}"
BASELINE_TSV="${BASELINE_TSV:-$ROOT/src/libexpr-v3/bench/baselines/seven-rows.tsv}"
JOURNAL="${JOURNAL:-$SELF_DIR/run-journal.tsv}"
CN_PATH="${CN_PATH:-/Users/angerman/Projects/iohk/cardano-node}"

ROW=""; EXPR=""; IMPURE="0"; METRIC="cpu"; NO_BUILD=0; SELFTEST=0; NO_DISK_CACHE=0
BASE_CPU=""; BASE_ARENA=""
while [[ $# -gt 0 ]]; do case "$1" in
  --row) ROW="$2"; shift 2;;
  --expr) EXPR="$2"; shift 2;;
  --impure) IMPURE="$2"; shift 2;;
  --metric) METRIC="$2"; shift 2;;
  --baseline-cpu) BASE_CPU="$2"; shift 2;;
  --baseline-arena) BASE_ARENA="$2"; shift 2;;
  --no-build) NO_BUILD=1; shift;;
  # --no-disk-cache: force a REAL eval by disabling v3's disk cache on BOTH arms.
  # MANDATORY for any drvPath CPU objective (git/hello/firefox/M5): cache-ON, the
  # drv is served from disk so the measured CPU is cache-hit time the VM cannot
  # move (git cache-on ≈1.45s partial vs cache-off ≈4.82s real eval, 2026-06-18).
  # Pair with a cache-off --baseline-cpu (the pinned seven-rows.tsv is cache-ON).
  --no-disk-cache) NO_DISK_CACHE=1; shift;;
  --selftest) SELFTEST=1; shift;;
  *) echo "autoresearch-cycle: unknown arg $1" >&2; exit 2;;
esac; done

# Per-process scratch dir.  The loop runs arms in PARALLEL git worktrees, each
# its own process; a shared /tmp/arc.err + /tmp/arc.build would let one arm read
# another arm's stderr → cross-contaminated CPU/arena/engaged values, i.e. the
# verdict graded on the wrong measurement.  mktemp -d isolates per process.
TMPD="$(mktemp -d "${TMPDIR:-/tmp}/arc.XXXXXX")" || { echo "autoresearch-cycle: mktemp -d failed (TMPDIR unwritable/full?)" >&2; exit 1; }
# Guard: an empty TMPD would send redirects to /err and /build (root) and the
# trap would rm -rf "" — so refuse to proceed without a real scratch dir.
[[ -n "$TMPD" && -d "$TMPD" ]] || { echo "autoresearch-cycle: no scratch dir" >&2; exit 1; }
trap 'rm -rf "$TMPD"' EXIT

# --- the 7 pinned-row workloads (kept in sync with bench/pin-seven-rows.sh) --
row_expr() { case "$1" in
  attrNames) IMPURE=1; echo 'builtins.length (builtins.attrNames (import <nixpkgs> {}))';;
  fib)       IMPURE=0; echo 'let f = n: if n < 2 then n else f (n - 1) + f (n - 2); in f 33';;
  foldl)     IMPURE=0; echo "builtins.foldl' (a: o: a + builtins.length (builtins.attrNames o)) 0 (builtins.genList (i: builtins.listToAttrs (builtins.genList (j: { name = toString j; value = i + j; }) 500)) 2000)";;
  hello)     IMPURE=1; echo '(import <nixpkgs> {}).hello.drvPath';;
  git)       IMPURE=1; echo '(import <nixpkgs> {}).git.drvPath';;
  firefox)   IMPURE=1; echo '(import <nixpkgs> {}).firefox.drvPath';;
  M5)        IMPURE=1; echo "(builtins.getFlake \"path:$CN_PATH\").outputs.packages.aarch64-darwin.cardano-node.name";;
  *) return 1;;
esac; }

# --- run_arm <envprefix> : echoes "engaged cpu arena result"  (mirrors pin-seven-rows) ---
run_arm() {
  local envp="$1" best="" arena="" res="" engaged=0 i u a
  local -a IMP=(); [[ "$IMPURE" == 1 ]] && IMP=(--impure)
  # cache-off forces a real eval on BOTH arms (see --no-disk-cache above).
  [[ "$NO_DISK_CACHE" == 1 ]] && envp="$envp NIX_V3_NO_DISK_CACHE=1"
  for ((i=0;i<RUNS;i++)); do
    res="$(env $envp NIX_VM_STATS=1 NIX_V3_MAX_WALL_TIME=300s NIX_V3_MAX_HEAP=10G \
        /usr/bin/time -l "$NIX" eval "${IMP[@]}" --expr "$EXPR" 2>"$TMPD/err")"
    u="$(grep -oE '[0-9]+\.[0-9]+ user' "$TMPD/err" | grep -oE '^[0-9.]+' | head -1)"
    a="$(grep -oE 'v3_arena=[0-9.]+MB' "$TMPD/err" | grep -oE '[0-9.]+' | tail -1)"
    grep -q 'v3-direct' "$TMPD/err" && engaged=1
    [[ -n "$u" ]] && { [[ -z "$best" ]] && best="$u" || best="$(awk "BEGIN{print ($u<$best)?$u:$best}")"; }
    [[ -n "$a" ]] && arena="$a"
  done
  # engaged FIRST (fixed field) and res LAST: `read`'s last-variable-gets-the-
  # remainder rule then absorbs a result containing spaces without shifting the
  # engaged flag onto a result word (which would mis-read ENGAGED).
  echo "${engaged} ${best:-NA} ${arena:-NA} ${res}"
}

emit_verdict() {  # verdict ratio v3cpu twcpu v3arena reason
  local v="$1" ratio="$2" v3cpu="$3" twcpu="$4" arena="$5" reason="$6"
  local stamp; stamp="$(date -u +%Y-%m-%dT%H:%M:%SZ 2>/dev/null || echo NA)"
  printf 'VERDICT\t%s\trow=%s\tmetric=%s\tv3cpu=%s\ttwcpu=%s\tratio=%s\tarena=%s\t%s\n' \
    "$v" "${ROW:-custom}" "$METRIC" "$v3cpu" "$twcpu" "$ratio" "$arena" "$reason"
  mkdir -p "$(dirname "$JOURNAL")"
  [[ -f "$JOURNAL" ]] || printf '# ts\tverdict\trow\tmetric\tv3cpu\ttwcpu\tratio\tarena\treason\n' >"$JOURNAL"
  printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
    "$stamp" "$v" "${ROW:-custom}" "$METRIC" "$v3cpu" "$twcpu" "$ratio" "$arena" "$reason" >>"$JOURNAL"
}

# ── selftest: validate the pipeline on a sub-second workload, no build, no mutation ──
if [[ "$SELFTEST" == 1 ]]; then
  echo "autoresearch-cycle: SELFTEST (current binary, no build, no engine mutation)" >&2
  [[ -x "$NIX" ]] || { echo "selftest FAIL: no nix at $NIX"; exit 1; }
  EXPR='let f = n: if n < 2 then n else f (n - 1) + f (n - 2); in f 22'; IMPURE=0; ROW=selftest; METRIC=cpu
  RUNS=2
  read -r v3eng v3cpu v3arena v3res < <(run_arm "NIX_V3_DIRECT_EVAL=1")
  read -r tweng twcpu twarena twres < <(run_arm "")
  echo "  v3: cpu=$v3cpu arena=$v3arena engaged=$v3eng result=$v3res" >&2
  echo "  tw: cpu=$twcpu result=$twres" >&2
  fail=0
  [[ "$v3eng" == 1 ]] || { echo "  selftest FAIL: v3 not ENGAGED (no v3-direct stats line)"; fail=1; }
  [[ "$v3res" == "$twres" && -n "$v3res" ]] || { echo "  selftest FAIL: not byte-identical ($v3res vs $twres)"; fail=1; }
  # exercise the verdict logic with a synthetic 2× baseline → must be KEEP-CANDIDATE
  if [[ "$v3cpu" != "NA" ]]; then
    BASE_CPU="$(awk "BEGIN{printf \"%.4f\", $v3cpu*2}")"
    ratio="$(awk "BEGIN{printf \"%.2f\", $v3cpu/$twcpu}")"
    keep="$(awk "BEGIN{print ($v3cpu <= $BASE_CPU*$KEEP_BAR)?\"KEEP-CANDIDATE\":\"NEUTRAL\"}")"
    [[ "$keep" == "KEEP-CANDIDATE" ]] || { echo "  selftest FAIL: verdict logic ($keep, expected KEEP-CANDIDATE)"; fail=1; }
    echo "  verdict-logic: synthetic baseline=$BASE_CPU → $keep (ratio vs TW=$ratio)  ✓" >&2
  fi
  [[ "$fail" == 0 ]] && { echo "autoresearch-cycle: SELFTEST PASS"; exit 0; } || { echo "autoresearch-cycle: SELFTEST FAIL"; exit 1; }
fi

# ── resolve workload ──
if [[ -n "$ROW" ]]; then
  EXPR="$(row_expr "$ROW")" || { echo "autoresearch-cycle: unknown --row $ROW" >&2; exit 2; }
  # BUG FIX 2026-06-18 (found by the autoresearch L2 arm): row_expr sets IMPURE
  # *inside* the `$(...)` command-substitution subshell, so the assignment never
  # reaches this parent scope.  The impure rows (attrNames/hello/git/firefox/M5)
  # were silently run WITHOUT --impure → `import <nixpkgs>` fails in pure-eval →
  # empty result (cpu≈0.08s, arena≈16.8MB) → spurious REVERT-DIVERGENT, making
  # every real-row run ungradeable.  Re-derive IMPURE here in the parent.
  case "$ROW" in
    fib|foldl) IMPURE=0;;                 # pure: no <nixpkgs>/currentSystem
    *)         IMPURE=1;;                 # attrNames/hello/git/firefox/M5
  esac
fi
[[ -n "$EXPR" ]] || { echo "autoresearch-cycle: need --row NAME or --expr EXPR" >&2; exit 2; }

# ── baseline (from the pinned TSV unless overridden) ──
if [[ -z "$BASE_CPU$BASE_ARENA" && -n "$ROW" && -f "$BASELINE_TSV" ]]; then
  brow="$(awk -F'\t' -v n="$ROW" '$1==n{print; exit}' "$BASELINE_TSV")"
  [[ -n "$brow" ]] && { BASE_CPU="$(cut -f2 <<<"$brow")"; BASE_ARENA="$(cut -f5 <<<"$brow")"; }
fi

# ── rebuild the candidate (stale-binary trap) ──
if [[ "$NO_BUILD" != 1 ]]; then
  echo "autoresearch-cycle: building ($BUILD_CMD)…" >&2
  ( cd "$ROOT" && eval "$BUILD_CMD" ) >"$TMPD/build" 2>&1 \
    || { tail -15 "$TMPD/build" >&2; emit_verdict BUILD-FAIL NA NA NA NA "rebuild failed"; exit 1; }
fi

# ── measure ──
read -r v3eng v3cpu v3arena v3res < <(run_arm "NIX_V3_DIRECT_EVAL=1")
read -r tweng twcpu twarena twres < <(run_arm "")
ratio="$(awk "BEGIN{ if(\"$twcpu\"==\"NA\"||\"$v3cpu\"==\"NA\"||$twcpu==0) print \"NA\"; else printf \"%.2f\", $v3cpu/$twcpu }")"

# ── verdict (precedence high→low) ──
if [[ "$v3eng" != 1 ]]; then
  emit_verdict ABORT-NOT-ENGAGED "$ratio" "$v3cpu" "$twcpu" "$v3arena" "no v3-direct stats line — distrust"; exit 0
fi
# An empty result means the eval produced no stdout — a crash/OOM/wall-timeout
# (possibly AFTER the v3-direct stats line, so engaged==1).  Two empty strings
# compare EQUAL, which would otherwise sail past the byte-identity check and let
# a double-crash be graded as a win.  Treat any empty arm as a hard revert.
if [[ -z "$v3res" || -z "$twres" ]]; then
  emit_verdict REVERT-DIVERGENT "$ratio" "$v3cpu" "$twcpu" "$v3arena" "empty result (crash/timeout) — cannot establish byte-identity"; exit 0
fi
if [[ "$v3res" != "$twres" ]]; then
  emit_verdict REVERT-DIVERGENT "$ratio" "$v3cpu" "$twcpu" "$v3arena" "v3 result != TW (correctness)"; exit 0
fi
# objective current vs baseline
cur="$v3cpu"; base="$BASE_CPU"; regtol="$CPU_REGRESS_TOL"
[[ "$METRIC" == arena ]] && { cur="$v3arena"; base="$BASE_ARENA"; regtol="$ARENA_REGRESS_TOL"; }
if [[ "$cur" == "NA" || -z "$base" || "$base" == "NA" ]]; then
  emit_verdict NEUTRAL "$ratio" "$v3cpu" "$twcpu" "$v3arena" "no baseline for $METRIC — measured only"; exit 0
fi
if awk "BEGIN{exit !($cur > $base*$regtol)}"; then
  emit_verdict REVERT-REGRESS "$ratio" "$v3cpu" "$twcpu" "$v3arena" "$METRIC $cur > baseline $base ×$regtol"; exit 0
fi
if awk "BEGIN{exit !($cur <= $base*$KEEP_BAR)}"; then
  emit_verdict KEEP-CANDIDATE "$ratio" "$v3cpu" "$twcpu" "$v3arena" "$METRIC $cur ≤ baseline $base ×$KEEP_BAR (promote: full gate + verify + soak)"; exit 0
fi
emit_verdict NEUTRAL "$ratio" "$v3cpu" "$twcpu" "$v3arena" "$METRIC $cur within noise of baseline $base"; exit 0
