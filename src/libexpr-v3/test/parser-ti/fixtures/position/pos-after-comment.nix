let
  s = {
    # line comment
    /* block
       comment */
    c = 1;
  };
  p = builtins.unsafeGetAttrPos "c" s;
in { inherit (p) column line; }