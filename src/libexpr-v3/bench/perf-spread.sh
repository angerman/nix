#!/usr/bin/env bash
# v3-vs-TW perf spread (darwin-4): run the self-asserting gate across a
# representative workload set and collect the (user-CPU, peak-RSS) ratio rows.
# Every row is ENGAGED + byte-IDENTICAL gated by the harness itself.
set -u
REPO=/Users/angerman/Projects/iohk/nix
GATE="$REPO/src/libexpr-v3/bench/v3-vs-tw-gate.sh"
OUT=/tmp/perf-spread.out
: > "$OUT"
log() { echo "$@" | tee -a "$OUT"; }

log "=== BUILD (HEAD vm.cc) ==="
ninja -C "$REPO/build" src/nix/nix >>"$OUT" 2>&1 || { log "BUILD FAILED"; log SPREAD_DONE; exit 1; }
log "BUILD OK"; log ""

log "=== gate selftest ==="
bash "$GATE" --selftest 2>&1 | tee -a "$OUT" | tail -1
log ""

# one row per workload: label | expr | RUNS
run_wl() {
  local label="$1" expr="$2" runs="$3"
  log "######################## $label ########################"
  RUNS="$runs" V3_WALL=600s V3_HEAP=14G bash "$GATE" "$expr" 2>&1 \
    | grep -E "ENGAGED|IDENTICAL|DIVERGENT|INCOMPLETE|user-CPU|peak-RSS|VERDICT|insns=" \
    | tee -a "$OUT"
  log ""
}

run_wl "hello.drvPath (real drv)"      '(import <nixpkgs> {}).hello.drvPath' 3
run_wl "git.drvPath (bigger drv)"      '(import <nixpkgs> {}).git.drvPath' 3
run_wl "firefox.drvPath (mem-heavy)"   '(import <nixpkgs> { config.allowUnfree = true; }).firefox.drvPath' 2
run_wl "fib 30 (pure compute/dispatch)" 'let f = n: if n < 2 then n else f (n - 1) + f (n - 2); in f 30' 3
run_wl "foldl 1e6 (list iteration)"    "builtins.foldl' (a: b: a + b) 0 (builtins.genList (x: x) 1000000)" 3
run_wl "map+length 1e6 (list build)"   'builtins.length (builtins.map (x: x + 1) (builtins.genList (x: x) 1000000))' 3
run_wl "attrNames nixpkgs (broad eval)" 'builtins.length (builtins.attrNames (import <nixpkgs> {}))' 3

log SPREAD_DONE
