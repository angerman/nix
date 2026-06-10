# Minimal reproducer for #455 (current, v3-native manifestation) — found 2026-06-10.
#
#   v3-eval:  SPINS  (non-terminating force loop, ~30 closures / 0 thunks; even
#             `builtins.isAttrs` of the result does not reach WHNF)
#   TW:       "x86_64-linux"  (instant)
#
# This was THE blocker for `(import <nixpkgs> {})` under pure v3-direct: the package
# set forces `stdenv.hostPlatform`, which elaborates the platform via
# `lib.systems.elaborate`.  Reaching this small unit (no full pkgset) is the key to
# fixing it.  Root cause: `elaborate` returns a self-referential rec-attrset `final`
# built as a `//` chain whose RHS operands are functions of `final`
# (`platforms.select final`, `mapAttrs (v: v final.parsed) inspect.predicates`, …);
# v3's `//` / APPLY_OVERRIDES WHNF order re-enters where TW's lazy merge terminates.
# See lode/RCA_455_VNATIVE_2026-06-10.md.
#
# Run: NIX_PATH=nixpkgs=<path> v3-eval --file repro-455-systems-elaborate.nix --strict
# (or substitute an explicit `import /path/to/nixpkgs/lib`).
((import <nixpkgs>/lib).systems.elaborate "x86_64-linux").system
