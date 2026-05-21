#!/usr/bin/env bash
# Walk progressively more aggressive cardano-node targets, capped
# at 4G/180s.  Reports outcome per step.  Safe to re-run.
set -u
NIX=build/src/nix/nix
CN=path:/Users/angerman/Projects/iohk/cardano-node
CAP=4G
WALL=180s

run_v3() {
    local name="$1"
    local expr="$2"
    local out_f=/tmp/bisect-${name}.out
    local err_f=/tmp/bisect-${name}.err
    printf "\n=== v3 %s ===\n" "$name"
    /usr/bin/time -p env \
        NIX_V3_DIRECT_EVAL=1 NIX_V3_SKIP_INSTALLABLE_PREEVAL=1 \
        NIX_V3_MAX_HEAP=$CAP NIX_V3_MAX_WALL_TIME=$WALL \
        "$NIX" eval --impure --expr "$expr" >"$out_f" 2>"$err_f"
    local rc=$?
    echo "exit: $rc"
    echo "stdout (head 3):"
    head -3 "$out_f" 2>/dev/null
    echo "stderr (tail 6):"
    tail -6 "$err_f" 2>/dev/null
}

# Cheap baseline.
run_v3 "01-outputs-lib" \
  "(builtins.getFlake \"$CN\").outputs.lib or 1"

# Force the input flake (nixpkgs).
run_v3 "02-outputs-inputs-nixpkgs-rev" \
  "(builtins.getFlake \"$CN\").outputs.inputs.nixpkgs.rev or null"

# Package set — just the attribute, no force.
run_v3 "03-packages-attr" \
  "let s = (builtins.getFlake \"$CN\").outputs.packages.aarch64-darwin; in builtins.typeOf s"

# Length of attrNames.
run_v3 "04-packages-length" \
  "builtins.length (builtins.attrNames (builtins.getFlake \"$CN\").outputs.packages.aarch64-darwin)"

# Smallest package — bech32 is alphabetically first.
run_v3 "05-bech32-attr" \
  "let s = (builtins.getFlake \"$CN\").outputs.packages.aarch64-darwin.bech32; in builtins.typeOf s"

# Its name.
run_v3 "06-bech32-name" \
  "(builtins.getFlake \"$CN\").outputs.packages.aarch64-darwin.bech32.name"

# M5 target.
run_v3 "07-cardano-node-name" \
  "(builtins.getFlake \"$CN\").outputs.packages.aarch64-darwin.cardano-node.name"
