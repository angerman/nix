#!/usr/bin/env bash
# bench/profile-at-scale.sh — REPRODUCIBLE "where does the v3 VM spend CPU at
# scale" profile.  Run this at every checkpoint to see the on-CPU phase
# breakdown + the deterministic VM dynamics, and to diff against prior runs.
#
# This is the committed, runnable form of the methodology written up in
# lode/PROFILE_AT_SCALE_2026-06-21.md.  Keeping the categorizer + workload defs
# HERE (not in prose) is what makes the profile repeatable + comparable.
#
# MUST run on a QUIET host (darwin-4) — CPU sampling is meaningless under load.
# macOS `sample` is required for the on-CPU breakdown; without it (e.g. Linux)
# the script still emits the deterministic counters (host-independent).
#
# Usage
# -----
#   bench/profile-at-scale.sh                 # profile firefox M5 HNE, print report
#   bench/profile-at-scale.sh firefox         # one workload
#   bench/profile-at-scale.sh --git-note      # also attach the summary to HEAD
#                                             #   as a git note (refs/notes/commits)
#   DUR_M5=15 bench/profile-at-scale.sh       # override a sample duration
#
# Always appends one commit-stamped summary line to
#   bench/samples/profile-ledger.tsv   (git_hash \t date \t per-workload metrics)
# so a "regressed at scale" claim always has a reproducible reference point.
#
# Output per workload:
#   - on-CPU category % (ALLOC / BINDINGS / PARSE / DISPATCH / LOWER / TLS / GC /
#     FORCE / CALL / OTHER) — idle/wait samples excluded
#   - thunk churn (allocated vs forced), nursery hit-rate, top opcodes
#
# Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
# Input Output Group.  SPDX-License-Identifier: Apache-2.0
set -u

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
NIX="${NIX:-$ROOT/build/src/nix/nix}"
CN_PATH="${CN_PATH:-/Users/angerman/Projects/iohk/cardano-node}"
HNE_PATH="${HNE_PATH:-/Users/angerman/Projects/iohk/haskell-nix-example}"
LEDGER="${LEDGER:-$ROOT/src/libexpr-v3/bench/samples/profile-ledger.tsv}"
GIT_NOTE=0
WORKLOADS=()
for a in "$@"; do
  case "$a" in
    --git-note) GIT_NOTE=1 ;;
    *) WORKLOADS+=("$a") ;;
  esac
done
[ ${#WORKLOADS[@]} -eq 0 ] && WORKLOADS=(firefox M5 HNE)

# Per-workload sample durations (seconds): a bit above the cache-off eval time so
# `sample` captures the whole run (it stops early when the process exits).
DUR_firefox="${DUR_firefox:-8}"; DUR_M5="${DUR_M5:-13}"; DUR_HNE="${DUR_HNE:-9}"

V3="NIX_V3_DIRECT_EVAL=1 NIX_V3_NO_DISK_CACHE=1"
V3F="$V3 NIX_V3_NO_NATIVE_CALL_FLAKE=1"   # flake workloads

# workload -> (env, expr).  Match pin-seven-rows.sh; cache-OFF exposes the full
# parse+lower+eval pipeline (the disk cache amortizes parse/lower in real evals).
wl_env() { case "$1" in firefox) echo "$V3";; M5|HNE) echo "$V3F";; esac; }
wl_expr() {
  case "$1" in
    firefox) echo '(import <nixpkgs> { config.allowUnfree = true; }).firefox.drvPath' ;;
    M5)  echo "(builtins.getFlake \"path:$CN_PATH\").outputs.packages.aarch64-darwin.cardano-node.name" ;;
    HNE) echo "(builtins.getFlake \"path:$HNE_PATH\").packages.aarch64-darwin.hello.drvPath" ;;
  esac
}
wl_dur() { local v="DUR_$1"; echo "${!v:-10}"; }

# --- the categorizer: the STABLE phase buckets (the heart of the methodology) ---
# Reads a macOS `sample` file, sums the "Sort by top of stack" self-time leaves
# into phases, EXCLUDING idle/wait (kernel cvwait/sigwait/workq/etc.).
CATAWK="$(mktemp -t catawk.XXXX)"
cat >"$CATAWK" <<'AWK'
/Sort by top of stack/{f=1; next}
/Binary Images|^Total number/{f=0}
f && /\(in / {
  n=$NF+0
  line=$0; sub(/[[:space:]]*\(in .*/,"",line); gsub(/^[[:space:]]+/,"",line); sym=line
  sub(/^DYLD-STUB\$\$/,"",sym)          # a PLT stub IS the underlying call
  cat="OTHER"
  if (sym ~ /__psynch|__sigwait|__workq|kevent|^poll$|mach_msg|__getdirentries|^__open$|^read$|^write$|semaphore|__commpage/) cat="WAIT"
  else if (sym ~ /yylex|Parser::parse|ParserState|addAttrLeaf|parser::|ParserLoc/) cat="PARSE"
  else if (sym ~ /globalInternSymbol|inlineTrivial|nix::v3::ir::|optimi|opt_|::lower|Module&|emit[A-Z]|Emitter|Lowerer/) cat="LOWER"
  else if (sym ~ /dispatchLoop/) cat="DISPATCH"
  else if (sym ~ /forceValue/) cat="FORCE"
  else if (sym ~ /callClosure/) cat="CALL"
  else if (sym ~ /mergeBindings|countDistinct|Bindings::|allocBindings|Cursor/) cat="BINDINGS"
  else if (sym ~ /tryMark|visitValue|Nursery|RootVisitor|[Ss]cavenge|MarkSweep|Marker|markRoots|walkV3|walkAll/) cat="GC"
  else if (sym ~ /strlen|printString|StringContext|stringContext|^memcmp$|basic_string|coerceToString/) cat="STRING"
  else if (sym ~ /sha256|blake|murmur|cityhash|hash_table|__emplace|__hash/) cat="HASH"
  else if (sym ~ /malloc|free|xzm|_platform_mem|operator new|operator delete|nanov2|tiny_|__bzero|alloc[A-Z]|traceable_allocator|::__append/) cat="ALLOC"
  else if (sym ~ /PosTable|PosIdx|LinesIterator/) cat="POS"
  else if (sym ~ /_tlv_get_addr|tlv/) cat="TLS"
  sum[cat]+=n; if(cat!="WAIT") oncpu+=n
}
END{
  if(oncpu==0){print "  (no on-CPU samples — sample missing or process too short)"; exit}
  split("ALLOC BINDINGS PARSE DISPATCH LOWER HASH STRING TLS GC FORCE CALL POS OTHER",order," ")
  for(i=1;i<=13;i++){c=order[i]; if(sum[c]>0) printf "  %-9s %6.1f%%  (%d)\n",c,100.0*sum[c]/oncpu,sum[c]}
  printf "  onCPU=%d  wait_excluded=%d  (OTHER = unresolved ??? + foreign libstore/getenv)\n",oncpu,sum["WAIT"]
}
AWK

# Commit stamp.  NOTE: when run on darwin-4 the source checkout may lag its
# (rsync'd) binary, so `git rev-parse` there is the WRONG commit — pass
# COMMIT=<canonical-hash> (the laptop HEAD the binary was built from), and attach
# the git note from the canonical checkout, not darwin-4.
GITHASH="${COMMIT:-$(cd "$ROOT" && git rev-parse --short HEAD 2>/dev/null || echo unknown)}"
DATESTR="$(date +%Y-%m-%d)"
REPORT="$(mktemp -t profrep.XXXX)"
LEDGERLINE="$GITHASH	$DATESTR"

{
  echo "================================================================"
  echo "v3 PROFILE AT SCALE — commit $GITHASH — $DATESTR — host $(hostname -s)"
  echo "  (cache-off, v3-direct; on-CPU excludes idle/wait; quiet host required)"
  echo "================================================================"
} | tee "$REPORT"

have_sample=1; command -v sample >/dev/null 2>&1 || have_sample=0
[ $have_sample -eq 0 ] && echo "WARNING: macOS \`sample\` absent — deterministic counters only." | tee -a "$REPORT"

for w in "${WORKLOADS[@]}"; do
  env_w="$(wl_env "$w")"; expr_w="$(wl_expr "$w")"; dur_w="$(wl_dur "$w")"
  [ -z "$expr_w" ] && { echo "unknown workload: $w" | tee -a "$REPORT"; continue; }
  echo | tee -a "$REPORT"; echo "#### $w ####" | tee -a "$REPORT"

  # (1) on-CPU category breakdown via `sample`
  if [ $have_sample -eq 1 ]; then
    env $env_w "$NIX" eval --impure --option allow-import-from-derivation true \
      --raw --expr "$expr_w" >/dev/null 2>&1 &
    pid=$!
    sample "$pid" "$dur_w" -file "$REPORT.$w.sample" >/dev/null 2>&1
    wait "$pid" 2>/dev/null
    echo "-- on-CPU category breakdown --" | tee -a "$REPORT"
    awk -f "$CATAWK" "$REPORT.$w.sample" | tee -a "$REPORT"
    rm -f "$REPORT.$w.sample"
  fi

  # (2) deterministic VM dynamics (host-independent counts)
  env $env_w NIX_VM_OPCOUNTS=1 NIX_VM_STATS=1 "$NIX" eval --impure \
    --option allow-import-from-derivation true --raw --expr "$expr_w" \
    >/dev/null 2>"$REPORT.$w.dyn"
  maxt="$(grep 'opcounts: total=' "$REPORT.$w.dyn" 2>/dev/null | sed -E 's/.*total=([0-9]+).*/\1/' | sort -rn | head -1)"
  churn="$(grep 'v3-direct alloc:' "$REPORT.$w.dyn" | awk '{th=0;tf=0;for(i=1;i<=NF;i++){if($i~/^thunks=/){split($i,a,"=");th=a[2]+0}if($i~/^thunksForced=/){split($i,b,"=");tf=b[2]+0}}if(th>mx){mx=th;mt=tf}}END{printf "thunks_alloc=%d forced=%d unforced=%.1f%%",mx,mt,(mx>0?100.0*(mx-mt)/mx:0)}')"
  nurs="$(grep 'nursery routing' "$REPORT.$w.dyn" | grep -oE 'hit_rate=[0-9.]+%' | tail -1)"
  {
    echo "-- dynamics: ops=$maxt  $churn  nursery_$nurs --"
    echo "-- top opcodes --"
    grep -A13 "opcounts: total=$maxt " "$REPORT.$w.dyn" 2>/dev/null | grep -E "OP_" | head -8 | sed 's/^/  /'
  } | tee -a "$REPORT"
  rm -f "$REPORT.$w.dyn"
  LEDGERLINE="$LEDGERLINE	$w:$churn,$nurs"
done

# Ledger (always) — commit-stamped, append-only.
mkdir -p "$(dirname "$LEDGER")"
[ -f "$LEDGER" ] || echo "# git_hash	date	per-workload: thunk-churn + nursery hit-rate" >"$LEDGER"
echo "$LEDGERLINE" >>"$LEDGER"
echo | tee -a "$REPORT"; echo "ledger += $LEDGER" | tee -a "$REPORT"

# Optional git note on the measured commit.
if [ $GIT_NOTE -eq 1 ]; then
  (cd "$ROOT" && git notes append HEAD -F "$REPORT") \
    && echo "git note attached to $GITHASH (git notes show $GITHASH)" | tee -a "$REPORT"
fi

rm -f "$CATAWK" "$REPORT"
