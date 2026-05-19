# Repro for #673 — baseNameOf / dirOf drop string context
#
# Found during the #672 follow-up context-propagation audit.  Both
# primops accept a string (often the interpolation of a derivation,
# carrying drv context) and return a derived string.  Pre-fix v3
# dropped the input's context; TW preserves it.
#
# Real-world impact: nixpkgs idioms like
#   `passthru.tests = { ... = mkSomething { name = baseNameOf "${drv}"; ...; }; }`
# would lose the drv reference from `inputDrvs`, cascade-diverging
# the consumer derivation's hash.  Same cascade family as #672.
#
# Tree-walker behaviour:
# - String input with context → output has SAME context.
# - Path input → output is a String (for baseNameOf) or Path (for
#   dirOf), no context (paths don't carry context).
#
# Expected (matches TW): non-empty context dict for the String case.
let
  pkgs = import <nixpkgs> {};
  lua = pkgs.lua;
in {
  baseNameOf_ctx = builtins.getContext (baseNameOf "${lua}");
  dirOf_ctx      = builtins.getContext (dirOf "${lua}");
  # Negative case: Path inputs have no context — must remain empty.
  baseNameOf_path_ctx = builtins.getContext (toString (baseNameOf ./.));
  dirOf_path_ctx      = builtins.getContext (toString (dirOf ./.));
}
