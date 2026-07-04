# LEVER-1 applied-cache fixture: an import-result application that THROWS.
# The cache inserts only at OP_RETURN, so a throw must never be cached — the
# second application must re-evaluate and throw again.
{ ... } @ args: throw "applied-cache-throwy"
