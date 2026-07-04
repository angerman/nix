# LEVER-1 applied-cache fixture: the CU-key collision regression shape.
# `inner` is a formals-lambda DEFINED IN the same imported file as the
# import-result lambda.  Under the (killed) CU-keyed cache identity, applying
# `inner {}` collided with the entry for `import ./lib.nix {}` (same CU, same
# argsHash) and returned the WRONG value.  The desc+provenance identity only
# caches the import-RESULT closure's applications.
{ ... } @ args:
{
  v = 1 + builtins.length (builtins.attrNames args);
  inner = { ... } @ a2: { v = 100 + builtins.length (builtins.attrNames a2); };
}
