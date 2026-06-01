let
  s = {
    doc = ''
      multi
      line
    '';
    after = 1;
  };
  p = builtins.unsafeGetAttrPos "after" s;
in { inherit (p) column line; }
