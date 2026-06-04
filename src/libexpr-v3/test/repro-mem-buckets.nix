# Regression fixture for the LIVE MEMORY BUCKETS report
# (dumpV3MemoryBuckets, gated NIX_V3_MEM_BUCKETS=1).
#
# It (a) imports a sibling file so the CU + BC caches are populated,
# and (b) allocates a wide attrset (20000 entries) so the arena bump
# clears the report's object-count floor and the report fires on the
# real workload pass (not the tiny bytecode-primop install passes).
#
# Returns a small int so the harness can assert the eval result
# byte-for-byte while the report goes to stderr.
#
# Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
# Input Output Group.  SPDX-License-Identifier: Apache-2.0
let lib = import ./repro-mem-buckets-lib.nix;
in builtins.length (builtins.attrNames (lib.mk 20000))
