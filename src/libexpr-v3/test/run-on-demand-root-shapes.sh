#!/usr/bin/env bash
# v3 #451 / #455: targeted positive shape coverage for on-demand-root.
#
# Hand-crafted .nix expressions that exercise the call-hook patterns
# on-demand-root populates: rec-attrset captures, `with self;` over
# rec attrsets, formals lambdas, multi-file imports, NixOS-module-
# shape (config/options) fixed-points.  Each runs through TW first
# to capture the expected output, then runs through v3 in each
# on-demand-root mode and asserts the same output.
#
# This is a *positive* test for the shapes that already work today,
# acting as a regression guard so future on-demand-root changes don't
# silently break them.  The known-failing real-world cardano-node
# shape (#455) is *not* included here -- that's a separate regression
# tracked by the task itself, not by this suite.
#
# Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.
# SPDX-License-Identifier: Apache-2.0

set -u

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
NIX_BIN="${NIX_BIN:-$ROOT/build/src/nix/nix}"

if [[ ! -x "$NIX_BIN" ]]; then
  echo "nix not found at $NIX_BIN" >&2
  exit 1
fi

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

# Each test case is a (name, body) pair.  `body` is a Nix expression
# evaluated at the current scope; it must produce a string-coercible
# value (number, string, bool, attrset/list of those).
declare -a CASES=(
  "lit:1 + 2"
  "lambda:(x: x * 2) 21"
  "rec-self:let s = rec { a = 1; b = 2; sum = a + b; }; in s.sum"
  "with-self:let s = rec { a = 10; f = x: with s; x + a; }; in s.f 5"
  "with-self-nested:let s = rec { a = 1; b = 2; f = x: with s; x + a + b; g = x: with s; f x; }; in s.g 100"
  "formals:let f = { a, b ? 10 }: a + b; in f { a = 5; }"
  "formals-rec:let f = { a, b ? a * 2 }: a + b; in f { a = 7; }"
  "module-shape:let cfg = { config, options ? {}, ... }@args: config.x or 0; in cfg { config = { x = 42; }; }"
  "let-rec:let rec1 = { x = 1; y = rec1.x + 1; }; in rec1.y"
  "fix:let fix = f: let x = f x; in x; addOne = self: { a = 0; b = self.a + 1; }; r = fix addOne; in r.b"
  "deep-let-rec:let outer = rec { x = 1; inner = rec { y = x + 1; z = inner.y + 1; }; }; in outer.inner.z"
  "list-of-lambdas:let fs = [ (x: x + 1) (x: x * 2) (x: x - 1) ]; in (builtins.elemAt fs 1) 10"
  "map-rec:let s = rec { items = [1 2 3]; doubled = map (x: x * 2) items; }; in builtins.elemAt s.doubled 1"
)

declare -a MODES=(
  "default::NIX_USE_V3=1"
  "od-safe:NIX_V3_ON_DEMAND_ROOT=1:NIX_USE_V3=1 NIX_V3_ON_DEMAND_ROOT=1"
  "od-unsafe:NIX_V3_ON_DEMAND_ROOT_UNSAFE=1:NIX_USE_V3=1 NIX_V3_ON_DEMAND_ROOT=1 NIX_V3_ON_DEMAND_ROOT_UNSAFE=1"
  # #454 Phase E: INVERT_EVAL skips willReturnClosure short-circuit
  # and lets v3 own closure-producing top-level Exprs, bridging the
  # Tag::Closure result back via __v3_call_bridge_1.
  "invert:NIX_V3_INVERT_EVAL=1:NIX_USE_V3=1 NIX_V3_INVERT_EVAL=1"
  "invert+od:NIX_V3_INVERT_EVAL=1+OD:NIX_USE_V3=1 NIX_V3_INVERT_EVAL=1 NIX_V3_ON_DEMAND_ROOT=1"
)

ok=0
fail=0
declare -a fail_names=()

for case_spec in "${CASES[@]}"; do
  IFS=':' read -r case_name case_body <<< "$case_spec"
  case_file="$TMP/${case_name}.nix"
  echo "$case_body" > "$case_file"

  # Capture the TW expected output.
  tw_out=$("$NIX_BIN" eval --no-eval-cache --json -f "$case_file" 2>/dev/null) || tw_out=""
  if [[ -z "$tw_out" ]]; then
    fail=$((fail + 1))
    fail_names+=("[tw-eval-failed] $case_name")
    continue
  fi

  for mode_spec in "${MODES[@]}"; do
    IFS=':' read -r mode_name _flag mode_env <<< "$mode_spec"
    v3_out=$(env $mode_env "$NIX_BIN" eval --no-eval-cache --json -f "$case_file" 2>/dev/null) || v3_out="<v3-eval-failed>"
    if [[ "$v3_out" == "$tw_out" ]]; then
      ok=$((ok + 1))
    else
      fail=$((fail + 1))
      fail_names+=("[$mode_name] $case_name: tw=$tw_out v3=$v3_out")
    fi
  done
done

echo "=== on-demand-root shapes: ok=$ok fail=$fail ==="
if [[ $fail -gt 0 ]]; then
  echo "Failures:"
  for n in "${fail_names[@]}"; do echo "  $n"; done
  exit 1
fi

# v3 #455 POSITIVE test: minimal reproducer for the on-demand-root
# infinite-recursion was *fixed* by the call-hook auto-eager-bridge
# (commit pending).  Assert the fix sticks.
repro_455="$ROOT/src/libexpr-v3/test/repro-455.nix"
if [[ -e "$repro_455" ]]; then
  tw_out=$("$NIX_BIN" eval --no-eval-cache --json -f "$repro_455" 2>/dev/null)
  v3_out=$(NIX_USE_V3=1 "$NIX_BIN" eval --no-eval-cache --json -f "$repro_455" 2>/dev/null)
  od_out=$(NIX_USE_V3=1 NIX_V3_ON_DEMAND_ROOT=1 NIX_V3_SKIP_THRESHOLD=0 "$NIX_BIN" \
            eval --no-eval-cache --json -f "$repro_455" 2>/dev/null) || od_out="<failed>"

  if [[ "$tw_out" != "$v3_out" ]]; then
    echo "=== #455 baseline regression: TW != v3 default ($tw_out vs $v3_out) ==="
    exit 1
  fi
  if [[ "$od_out" != "$tw_out" ]]; then
    echo
    echo "=== #455 minimal repro POSITIVE regression: FAILED ==="
    echo "  TW:                                                       $tw_out"
    echo "  v3 default:                                               $v3_out"
    echo "  v3 + ON_DEMAND_ROOT + SKIP_THRESHOLD=0:                   '$od_out'"
    echo "  The auto-eager-bridge fix for #455 lazy-bridge cycle has"
    echo "  regressed.  Check the call hook's ScopedEagerBridge guard."
    exit 1
  fi
  echo "=== #455 minimal repro positive: passes (auto-eager fix intact) ==="

  # #455 also asserts the env-var override knob works.
  eb_out=$(NIX_USE_V3=1 NIX_V3_ON_DEMAND_ROOT=1 NIX_V3_SKIP_THRESHOLD=0 \
            NIX_V3_EAGER_BRIDGE_MAX=10000 "$NIX_BIN" \
            eval --no-eval-cache --json -f "$repro_455" 2>/dev/null) \
    || eb_out="<failed>"
  if [[ "$eb_out" != "$tw_out" ]]; then
    echo
    echo "=== #455 EAGER_BRIDGE_MAX positive regression: FAILED ==="
    echo "  TW: $tw_out"
    echo "  ON_DEMAND_ROOT + EAGER_BRIDGE_MAX=10000: '$eb_out'"
    exit 1
  fi
  echo "=== #455 EAGER_BRIDGE_MAX=10000 override knob: passes ==="

  # Negative-of-positive: with NIX_V3_NO_CALL_HOOK_EAGER=1 (disable
  # the auto-eager fix) the minimal repro should fail again.  This
  # proves the auto-eager-bridge guard is what's actually closing
  # the cycle.  If this assertion FLIPS (no-eager produces correct
  # output), it means the lazy-bridge cycle was fixed elsewhere
  # and the auto-eager guard is now redundant.
  no_eager_out=$(NIX_USE_V3=1 NIX_V3_ON_DEMAND_ROOT=1 NIX_V3_SKIP_THRESHOLD=0 \
            NIX_V3_NO_CALL_HOOK_EAGER=1 "$NIX_BIN" \
            eval --no-eval-cache --json -f "$repro_455" 2>/dev/null) \
    || no_eager_out="<failed>"
  if [[ "$no_eager_out" == "$tw_out" ]]; then
    echo
    echo "=== #455 NEGATIVE-OF-POSITIVE has FLIPPED ==="
    echo "  NIX_V3_NO_CALL_HOOK_EAGER=1 now produces correct output."
    echo "  The auto-eager guard may be redundant -- check if a deeper"
    echo "  fix landed and remove the guard to simplify."
  else
    echo "=== #455 NIX_V3_NO_CALL_HOOK_EAGER=1 still fails (auto-eager doing the work) ==="
  fi
fi
exit 0
