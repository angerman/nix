# NEGATIVE test 2: writeback must propagate exceptions correctly.
#
# When primElem iterates a list and forces an entry that throws,
# the exception must propagate AND the entry should remain in a
# state consistent with TW semantics (either un-forced or
# marked-with-error so future forces re-throw the same way).
#
# Setup: `mapAttrs` lambda throws on a specific name.  Call elem
# against a needle that won't match — forcing every entry until
# one throws.  Expected: throws "from-b" (when forcing the second
# entry; entries are sorted alphabetically).
#
# Both TW and v3 must throw the same error message.  v3 must not
# silently cache a bad state that would mask the error on retry.
let
  src = { a = 1; b = 2; c = 3; };
  mapped = builtins.mapAttrs
    (n: v: if n == "b" then throw "from-${n}" else v)
    src;
  values = builtins.attrValues mapped;
  needle = 999;
in
  if builtins.elem needle values then "found" else "not-found"
