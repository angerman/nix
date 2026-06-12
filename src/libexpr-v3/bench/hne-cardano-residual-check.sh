#!/usr/bin/env bash
# HNE + cardano haskell.nix residual check (CODEBASE_REVIEW follow-up).
#
# Question: now that the convergent firefox/ghc98 Slot-deref residual is fixed,
# do the two remaining drvPath residuals noted in memory — HNE "hackage-package
# missing" and cardano "cabalProject' missing" — also resolve, OR is a given
# attr unevaluable on the TW *reference* itself (env/IFD), in which case it is a
# SKIP (like vlc in the nixpkgs sweep), not a v3 divergence.
#
# For each (flake, attr) we record, side by side, TW and v3-direct results PLUS
# the TW stderr — because an empty TW result means the reference itself failed
# (IFD/network/eval error) and the comparison is moot.
set -u

N=/Users/angerman/Projects/iohk/nix/build/src/nix/nix
HNE=path:/Users/angerman/Projects/iohk/haskell-nix-example
CN=path:/Users/angerman/Projects/iohk/cardano-node
OUT=/tmp/hne-cardano-residual.out
: > "$OUT"

log() { echo "$@" | tee -a "$OUT"; }

# eval_one <label> <expr> <extra-eval-flags...>
# Runs TW then v3-direct on the SAME expr; captures stdout+stderr for both.
eval_one() {
  local label="$1"; shift
  local expr="$1"; shift
  log "=================================================================="
  log "### $label"
  log "--- expr: $expr"

  # TW reference
  "$N" eval --impure "$@" --expr "$expr" >/tmp/hcr.tw.out 2>/tmp/hcr.tw.err
  local tw_rc=$?
  local tw_out; tw_out=$(cat /tmp/hcr.tw.out)
  log "TW   rc=$tw_rc out=[$tw_out]"
  if [ -s /tmp/hcr.tw.err ]; then
    log "TW   stderr (last 4 lines):"
    tail -4 /tmp/hcr.tw.err | sed 's/^/  TW| /' | tee -a "$OUT"
  fi

  # v3-direct
  NIX_V3_DIRECT_EVAL=1 NIX_V3_SKIP_INSTALLABLE_PREEVAL=1 \
  NIX_V3_MAX_WALL_TIME=600s NIX_V3_MAX_HEAP=12G \
    "$N" eval --impure "$@" --expr "$expr" >/tmp/hcr.v3.out 2>/tmp/hcr.v3.err
  local v3_rc=$?
  local v3_out; v3_out=$(cat /tmp/hcr.v3.out)
  log "v3   rc=$v3_rc out=[$v3_out]"
  if [ -s /tmp/hcr.v3.err ]; then
    log "v3   stderr (last 4 lines):"
    tail -4 /tmp/hcr.v3.err | sed 's/^/  v3| /' | tee -a "$OUT"
  fi

  # Verdict
  if [ "$tw_rc" -ne 0 ] || [ -z "$tw_out" ]; then
    log ">>> VERDICT: SKIP (TW reference itself failed — env/IFD, not a v3 divergence)"
  elif [ "$tw_out" = "$v3_out" ]; then
    log ">>> VERDICT: PASS (byte-identical)"
  else
    log ">>> VERDICT: DIVERGE (TW ok, v3 differs)"
  fi
}

log "########## HNE ##########"
eval_one "HNE attrNames packages.aarch64-darwin" \
  "builtins.attrNames (builtins.getFlake \"$HNE\").packages.aarch64-darwin"
eval_one "HNE hello typeOf" \
  "builtins.typeOf (builtins.getFlake \"$HNE\").packages.aarch64-darwin.hello"
eval_one "HNE hello.drvPath" \
  "(builtins.getFlake \"$HNE\").packages.aarch64-darwin.hello.drvPath" --raw

log "########## cardano-node ##########"
eval_one "CN attrNames packages.aarch64-darwin" \
  "builtins.attrNames (builtins.getFlake \"$CN\").packages.aarch64-darwin"

log "HCR_DONE"
