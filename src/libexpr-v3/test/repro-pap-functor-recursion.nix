# PAP recognition — REGRESSION (the haskell.nix / cardano v3-direct spin).
#
# This is the minimal kernel of the 2026-06-10 non-termination: nixpkgs
# `lib.isFunction` recurses through a `__functor` whose body is a (collapsed)
# curried lambda.  `f.__functor f` is a Tag::App PAP; before the fix the
# `OP_IS_FUNCTION` macro treated the WHNF PAP as non-WHNF, sent it to
# op_force_slow (a no-op on a PAP), then re-tested isAppLike() → infinite
# force-retry loop.  Surfaced at scale in `import nixpkgs { overlays = [
# haskellNix.overlay ]; }` (makeOverridable / setFunctionArgs build & test
# PAPs everywhere) → `getFlake haskell-nix-example` spun at ~5612 closures.
#
# MUST evaluate to `true` (and terminate).  See lode/RCA_HASKELLNIX_SPIN_2026-06-10.md.
let
  # The exact nixpkgs lib.trivial definition.
  isFunction = f: builtins.isFunction f || (f ? __functor && isFunction (f.__functor f));
in
isFunction { __functor = self: (x: x); }
