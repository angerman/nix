let
  s = (builtins.tryEval { e = 1; }).value;
  p = builtins.unsafeGetAttrPos "e" s;
in { inherit (p) column line; }