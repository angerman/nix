let
  s = {
    "quoted-key" = 1;
  };
  p = builtins.unsafeGetAttrPos "quoted-key" s;
in { inherit (p) column line; }