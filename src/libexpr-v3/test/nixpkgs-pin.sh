# Pin <nixpkgs> for v3 golden tests + measurements — stop channel drift.
#
# The brute-audit golden (hello/git/firefox versions) and the cross-host CPU/
# arena numbers were breaking whenever a host's `<nixpkgs>` channel moved:
# laptop resolved hello-2.12.3, darwin-4 hello-2.12.1, the flake hello-2.12.2.
# Pin `<nixpkgs>` to the repo's OWN flake.lock nixpkgs so every host and every
# checkpoint resolves the SAME tree → the golden never drifts and firefox-on-
# darwin-4 == firefox-on-laptop.
#
# Single source of truth = flake.lock (this derives the rev from it, so the pin
# auto-tracks).  To bump: `nix flake update nixpkgs`, THEN re-derive the golden
# versions in run-brute-audit.sh (eval the cases against the new rev).
#
# Sourced by run-brute-audit.sh + bench/profile-at-scale.sh; ad-hoc evals may
# `source` it too.  Sets NIX_PATH so the existing `import <nixpkgs>` exprs
# resolve to the pinned tree (a rev-immutable github archive; fetched + cached).
#
# Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
# Input Output Group.  SPDX-License-Identifier: Apache-2.0

# Resolve this file's own location robustly whether sourced via a relative or
# absolute path (BASH_SOURCE may be relative to the sourcer's PWD).
_pin_self="${BASH_SOURCE[0]:-$0}"
case "$_pin_self" in /*) ;; *) _pin_self="$PWD/$_pin_self" ;; esac
_pin_root="$(cd "$(dirname "$_pin_self")/../../.." 2>/dev/null && pwd)"
V3_NIXPKGS_REV="$(python3 -c "import json,sys; print(json.load(open('$_pin_root/flake.lock'))['nodes']['nixpkgs']['locked']['rev'])" 2>/dev/null)"
if [ -n "${V3_NIXPKGS_REV:-}" ]; then
    export V3_NIXPKGS_REV
    export V3_NIXPKGS_PIN="https://github.com/NixOS/nixpkgs/archive/${V3_NIXPKGS_REV}.tar.gz"
    # Prepend so the pinned entry wins over any ambient channel `nixpkgs=`.
    export NIX_PATH="nixpkgs=${V3_NIXPKGS_PIN}${NIX_PATH:+:${NIX_PATH}}"
else
    echo "nixpkgs-pin: could not read nixpkgs rev from $_pin_root/flake.lock — using ambient <nixpkgs> (golden may drift)" >&2
fi
unset _pin_root _pin_self