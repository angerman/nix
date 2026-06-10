# PAP recognition — REGRESSION (functionArgs over a __functor / PAP).
#
# nixpkgs `lib.functionArgs` recurses `builtins.functionArgs (f.__functor f)`
# when `f` is a functor; `f.__functor f` is a collapsed-curry PAP.  Before the
# fix `builtins.functionArgs` typeError'd ("functionArgs: expected lambda") on
# the Tag::App PAP — used by `lib.makeOverridable`'s
# `setFunctionArgs result (functionArgs result)` branch.
#
# MUST evaluate to `{ }` (the next unbound param of the PAP is positional).
let
  functionArgs =
    f: if f ? __functor then f.__functionArgs or (builtins.functionArgs (f.__functor f))
       else builtins.functionArgs f;
in
functionArgs { __functor = self: x: x; }
