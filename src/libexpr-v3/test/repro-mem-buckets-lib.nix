# Helper imported by repro-mem-buckets.nix.  The `import` makes the
# run populate the in-memory CU cache (one CompilationUnit for this
# file) and the on-disk BC cache (SQLite) — so the NIX_V3_MEM_BUCKETS
# report's "CU cache (bytecode)" and "BC cache (SQLite)" buckets are
# both exercised.
#
# Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
# Input Output Group.  SPDX-License-Identifier: Apache-2.0
{
  mk = n:
    builtins.listToAttrs
      (builtins.genList (i: { name = "k_" + toString i; value = i * i; }) n);
}
