# Repro for #674 — primops that should reject contexted-string inputs
# (parseDrvName, splitVersion, getEnv, compareVersions) PLUS toXML
# context propagation.
#
# TW uses `forceStringNoCtx` for primops that semantically must operate
# on context-free strings.  Pre-fix v3 silently accepted contexted
# strings and produced (possibly wrong) results.
#
# Positive case: no-context strings still work.  (The rejection path
# is validated separately because the error string contains
# nondeterministic store-path hashes.  v3's error message now matches
# TW byte-for-byte after this commit.)
{
  parseDrvName_accepts    = builtins.parseDrvName "name-1.0";
  splitVersion_accepts    = builtins.splitVersion "1.2.3-alpha";
  compareVersions_accepts = builtins.compareVersions "1.0" "2.0";
  # toXML on no-context inputs is unaffected.
  toXML_accepts           = builtins.toXML { a = "x"; b = 42; };
}
