#!/usr/bin/env bash
# Tier 3 real-world validation — broad-coverage sample of nixpkgs
# workloads to confirm v3-direct still produces byte-identical
# drvPaths to TW after the latest session work.
#
# This is a SAMPLING harness, not exhaustive: it picks ~30 packages
# across categories (basics, compilers, ecosystems, cross-compile,
# overlays, generators, writers, lib functions, module eval, stress)
# that together exercise the major v3 code paths.
#
# Run with `V3_TIER3_VERBOSE=1` to see per-shape output.  Otherwise
# only summary + divergences are shown.
#
# Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
# Input Output Group.  SPDX-License-Identifier: Apache-2.0
set -u

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
NIX="${NIX:-$ROOT/build/src/nix/nix}"
VERBOSE="${V3_TIER3_VERBOSE:-0}"

if [[ ! -x "$NIX" ]]; then
  echo "run-tier3: nix not executable at $NIX" >&2
  exit 2
fi

total=0
match=0
diverge=0
no_tw=0
divergent_list=""

check_drv() {
  local label="$1" expr="$2"
  total=$((total + 1))
  local tw v3
  tw="$("$NIX" eval --impure --expr "$expr" 2>/dev/null || true)"
  v3="$(NIX_V3_DIRECT_EVAL=1 NIX_V3_SKIP_INSTALLABLE_PREEVAL=1 \
        NIX_V3_MAX_WALL_TIME=120s NIX_V3_MAX_HEAP=8G \
        "$NIX" eval --impure --expr "$expr" 2>/dev/null || true)"
  if [[ -z "$tw" ]]; then
    no_tw=$((no_tw + 1))
    [[ "$VERBOSE" == "1" ]] && printf "  ?-empty  %s\n" "$label"
  elif [[ "$tw" == "$v3" ]]; then
    match=$((match + 1))
    [[ "$VERBOSE" == "1" ]] && printf "  MATCH    %-50s %s\n" "$label" "${tw:0:50}"
  else
    diverge=$((diverge + 1))
    divergent_list="$divergent_list $label"
    printf "  DIVERGE  %-50s\n    TW: %s\n    V3: %s\n" "$label" "$tw" "$v3"
  fi
}

# Battery A — basic packages
check_drv "hello"           '(import <nixpkgs>{}).hello.drvPath'
check_drv "ripgrep"         '(import <nixpkgs>{}).ripgrep.drvPath'
check_drv "jq"              '(import <nixpkgs>{}).jq.drvPath'
check_drv "curl"            '(import <nixpkgs>{}).curl.drvPath'
check_drv "git"             '(import <nixpkgs>{}).git.drvPath'
check_drv "python3"         '(import <nixpkgs>{}).python3.drvPath'
check_drv "nodejs"          '(import <nixpkgs>{}).nodejs.drvPath'
check_drv "rustc"           '(import <nixpkgs>{}).rustc.drvPath'

# Battery D — heavy packages (firefox, emacs, qemu)
check_drv "firefox"         '(import <nixpkgs>{}).firefox.drvPath'
check_drv "emacs"           '(import <nixpkgs>{}).emacs.drvPath'
check_drv "qemu"            '(import <nixpkgs>{}).qemu.drvPath'

# Cross-compile + static + overlay
check_drv "cross-aarch64-hello" \
  '(import <nixpkgs>{}).pkgsCross.aarch64-multiplatform.hello.drvPath'
check_drv "pkgsStatic-hello" \
  '((import <nixpkgs>{}).pkgsStatic.hello.drvPath or "skip")'
check_drv "overlay-rename" \
  '(import <nixpkgs>{ overlays = [(self: super: { hello = super.hello.overrideAttrs(_:{ name = "custom-hello"; }); })]; }).hello.name'

# Compositions
check_drv "python-withPkgs" \
  'let p = import <nixpkgs>{}; in (p.python3.withPackages (ps: [ps.requests ps.numpy])).drvPath'
check_drv "ghc-withPkgs" \
  'let p = import <nixpkgs>{}; in (p.haskellPackages.ghcWithPackages (ps: [ps.text])).drvPath'
check_drv "symlinkJoin" \
  '((import <nixpkgs>{}).symlinkJoin { name = "joined"; paths = [(import <nixpkgs>{}).hello]; }).drvPath'
check_drv "buildPackages-hello" \
  '((import <nixpkgs>{}).buildPackages.hello).drvPath'
check_drv "runCommand" \
  '((import <nixpkgs>{}).runCommand "test" {} "echo hi > $out").drvPath'
check_drv "writeShellApp" \
  '((import <nixpkgs>{}).writeShellApplication { name = "myapp"; text = "echo hi"; }).drvPath'

# lib generators
check_drv "generators-toJSON" \
  '(import <nixpkgs>{}).lib.generators.toJSON {} { x = 1; y = [1 2]; z = "ok"; }'
check_drv "generators-toINI" \
  '(import <nixpkgs>{}).lib.generators.toINI {} { section1 = { x = 1; y = "hello"; }; }'
check_drv "generators-toLua" \
  '(import <nixpkgs>{}).lib.generators.toLua {} { x = 1; }'

# lib computational
check_drv "lib-recursiveUpdate" \
  'with (import <nixpkgs>{}).lib; attrsets.recursiveUpdate { a.b.c = 1; d = 2; } { a.b.x = 99; d = 3; }'
check_drv "lib-versionSort" \
  'with (import <nixpkgs>{}).lib; lists.sort (a: b: versionOlder a b) ["1.0.0" "0.9" "1.0.0-rc1" "2.0"]'
check_drv "lib-makeBinPath" \
  'with (import <nixpkgs>{}); lib.makeBinPath [hello jq]'

# Module eval
check_drv "evalModules-basic" \
  'with (import <nixpkgs>{}).lib; (evalModules { modules = [{ options.x = mkOption { type = types.int; default = 1; }; }]; }).config.x'
check_drv "evalModules-submodule" \
  'with (import <nixpkgs>{}).lib; (evalModules { modules = [{ options.service = mkOption { type = types.submodule { options.port = mkOption { type = types.int; default = 80; }; }; default = {}; }; }]; }).config.service.port'

# Stress
check_drv "stress-big-list" \
  'builtins.length (builtins.genList (i: i) 10000)'
check_drv "stress-mutual-rec" \
  'let even = n: if n == 0 then true else odd (n - 1); odd = n: if n == 0 then false else even (n - 1); in even 100'
check_drv "stress-fib-20" \
  'let fib = n: if n < 2 then n else fib (n - 1) + fib (n - 2); in fib 20'

echo
echo "=== Tier 3 validation summary ==="
echo "  Total checked: $total"
echo "  MATCH:         $match"
echo "  DIVERGE:       $diverge"
echo "  No TW value:   $no_tw  (platform-N/A / removed packages)"
[[ -n "$divergent_list" ]] && echo "  Divergent:    $divergent_list"

# Exit 0 if no DIVERGEs (no_tw is not a v3 problem).
[[ "$diverge" -eq 0 ]]
