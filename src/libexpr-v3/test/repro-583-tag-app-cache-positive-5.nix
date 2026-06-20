# POSITIVE test 5: valueEqual over CHAIN mapAttrs results forces the MAPPED
# value, not the stored source — the chain sibling of POS-3 (which covers the
# flat mapAttrs case).  mapAttrs is applied over a chain (`base // ext`), so
# valueEqual walks the chain path (Bindings::Cursor, realizeMapAttrs=true).
#
# Without realizing, `==` would force the source `v` and skip the mapper's
# builtins.trace side effect (0 traces); with it, each mapped entry is realized
# once per side, matching TW.
#
# Trace assertion: 6 traces (3 entries x 2 sides), matches TW.
let
  base = { a = 1; b = 2; };
  ext  = { c = 3; };
  mkSide = tag: builtins.mapAttrs
    (n: v: builtins.trace "${tag}-${n}" v)
    (base // ext);
in
  if mkSide "P" == mkSide "Q" then "equal" else "not-equal"
