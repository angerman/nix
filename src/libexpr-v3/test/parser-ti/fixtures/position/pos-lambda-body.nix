let
  f = arg: {
    z = arg;
  };
  p = builtins.unsafeGetAttrPos "z" (f 1);
in { inherit (p) column line; }