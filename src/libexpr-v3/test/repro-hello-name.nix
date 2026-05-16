# Minimal v3-direct repro of the hello.name hot-loop bug.
#
# Bisected 2026-05-16 per action plan principle "Bisect nixpkgs to
# find the unit you can falsify against".  See
# ./run-hello-name-repro.sh and project_583_memoization_loop.md.
#
# Status (under NIX_V3_DIRECT_EVAL=1):
#   - TW:      ~ 0.6 s
#   - v3:      TIMES OUT at 30+ s with 5.7M setType calls inside
#              parsedPlatform.check / flip / setTypes cycle
#
# Repro is "just `import <nixpkgs>{}` and look at it" — even
# `builtins.isAttrs` of the result hot-loops because the attrset's
# WHNF requires forcing the `boot stages` thunk in
# pkgs/top-level/default.nix, which triggers the cycle.
#
# Narrowed:
#   - lib.systems alone — works (lib/systems/parse.nix isolated, no loop)
#   - lib.systems.elaborate "aarch64-darwin" — works (0.2 s)
#   - lib.systems.parse.mkSystemFromString "aarch64-darwin" — works (0.1 s)
#   - lib.meta.availableOn ... — works (0.08 s)
#   - check-meta.assertValidity (direct call) — works (0.08 s)
#   - pkgs/top-level/default.nix invocation — HOT-LOOPS (44 s timeout)
#
# So the bug is in interactions inside `boot stages` (the stage
# builder), not in any lib-systems / check-meta primitive in
# isolation.  The cycle's stack signature (from V3_DBG_HOT_CALLEE):
#
#   validity @ make-derivation.nix:953
#    → invalid @ check-meta.nix:686
#      → attrs @ check-meta.nix:418
#        → pkg @ check-meta.nix:132
#          → list @ check-meta.nix:130
#            → <formals> @ parse.nix:953 (mkSystem)
#              → ... parsedPlatform.check ...
#                → cycle of (b @ flip → openKernel.check → setTypes)
#
# Hypothesis kept open: the cycle involves overlays applied to lib
# during pkgs construction (each stage re-applies overlays via
# `lib.extends` / `lib.composeExtensions`), and v3's lib.fix
# / extends intrinsic path interacts badly with parsedPlatform.check
# when called from inside the stage's lazy attrset.
#
# This expression alone is NOT a self-contained repro — it imports
# nixpkgs.  A truly self-contained synthetic repro could not be
# constructed during bisection (synthetic mapAttrs+attrValues+elem
# at N=100 took 50 ms on both TW and v3 — see project_583
# memoization_loop.md "falsification" section).  The bug requires
# the specific shape of nixpkgs stage construction.

builtins.isAttrs (import <nixpkgs> { })
