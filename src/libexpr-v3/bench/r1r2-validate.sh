#!/usr/bin/env bash
# R1/R2 fix validation (darwin-4). Builds nix + v3-eval with the two fixes
# (R1 force-before-coerce in OP_STR_CONCAT; R2 over-application saturate-then-
# reapply at the OP_CALL/OP_TAIL_CALL throw sites) and validates:
#   - lang 143 + drv-parity 25 (no regression)
#   - R2 synthetic regression test (positive+negative)
#   - R1: HNE typeOf .hello  (v3 == TW == "set"; R1-RESIDUAL must NOT print)
#   - R2: cardano attrNames .packages count (v3 == TW)
#   - full nixpkgs drvPath sweep (no regression vs prior 63/63)
set -u
REPO=/Users/angerman/Projects/iohk/nix
N=$REPO/build/src/nix/nix
OUT=/tmp/r1r2-validate.out
: > "$OUT"
log() { echo "$@" | tee -a "$OUT"; }

log "=== BUILD nix + v3-eval ==="
if ! ninja -C "$REPO/build" src/nix/nix src/libexpr-v3/v3-eval >>"$OUT" 2>&1; then
  log "BUILD FAILED"; log R1R2VAL_DONE; exit 1
fi
log "BUILD OK"

log ""; log "=== lang tests ==="
bash "$REPO/src/libexpr-v3/test/run-lang-tests.sh" 2>&1 | tail -5 | tee -a "$OUT"
log ""; log "=== drv-parity ==="
bash "$REPO/src/libexpr-v3/test/run-drv-parity.sh" 2>&1 | tail -4 | tee -a "$OUT"
log ""; log "=== R2 synthetic regression ==="
bash "$REPO/src/libexpr-v3/test/run-r2-over-application-tests.sh" 2>&1 | tail -4 | tee -a "$OUT"

HNE=path:/Users/angerman/Projects/iohk/haskell-nix-example
CN=path:/Users/angerman/Projects/iohk/cardano-node
strip() { grep -vE 'stack size|setrlimit|search path|does not exist, ignoring'; }

log ""; log "=== R1: HNE typeOf .hello ==="
tw=$("$N" eval --impure --raw --expr "builtins.typeOf (builtins.getFlake \"$HNE\").packages.aarch64-darwin.hello" 2>/dev/null)
v3=$(NIX_V3_DIRECT_EVAL=1 NIX_V3_MAX_WALL_TIME=600s NIX_V3_MAX_HEAP=12G \
     "$N" eval --impure --raw --expr "builtins.typeOf (builtins.getFlake \"$HNE\").packages.aarch64-darwin.hello" 2>/tmp/r1.err)
log "  TW=[$tw]  v3=[$v3]"
if grep -q R1-RESIDUAL /tmp/r1.err; then log "  R1-RESIDUAL diag FIRED:"; grep R1-RESIDUAL /tmp/r1.err | tee -a "$OUT"; fi
grep -E "error:" /tmp/r1.err | tail -2 | sed 's/^/  v3err| /' | tee -a "$OUT"
[ -n "$tw" ] && [ "$tw" = "$v3" ] && log "  R1 VERDICT: PASS" || log "  R1 VERDICT: CHECK (tw=[$tw] v3=[$v3])"

log ""; log "=== R2: cardano attrNames .packages (count) ==="
tw=$("$N" eval --impure --expr "builtins.length (builtins.attrNames (builtins.getFlake \"$CN\").packages.aarch64-darwin)" 2>/dev/null)
v3=$(NIX_V3_DIRECT_EVAL=1 NIX_V3_MAX_WALL_TIME=600s NIX_V3_MAX_HEAP=12G \
     "$N" eval --impure --expr "builtins.length (builtins.attrNames (builtins.getFlake \"$CN\").packages.aarch64-darwin)" 2>/tmp/r2.err)
log "  TW=[$tw]  v3=[$v3]"
grep -E "error:" /tmp/r2.err | tail -2 | sed 's/^/  v3err| /' | tee -a "$OUT"
[ -n "$tw" ] && [ "$tw" = "$v3" ] && log "  R2 VERDICT: PASS" || log "  R2 VERDICT: CHECK (tw=[$tw] v3=[$v3])"

log ""; log "=== full nixpkgs drvPath sweep (regression) ==="
bash "$REPO/src/libexpr-v3/test/run-759-nixpkgs-drvpath-sweep.sh" 2>&1 | tail -6 | tee -a "$OUT"

log R1R2VAL_DONE
