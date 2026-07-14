# WS-6 M2.0 reclaim-timing probe workload.
#
# Allocates 200 transient 3M-element lists (each ~24 MiB → a single huge arena
# block, > kHugeCutoff), each dead immediately after `length`. Used to observe,
# on Linux, that v3's own freeHugeBlock reclaims dead huge blocks (blocksFreed /
# bytesFreed under NIX_VM_STATS) — and that PEAK RSS is bounded only if the
# gen-major fires mid-eval. Here it does NOT (the foldl' callback runs at nested
# dispatch depth, so the exitDepth==0 gen-major defers → dead blocks accumulate
# → peak ~= total). Contrast a real broad eval (firefox) where the gen-major
# fires at the many top-level points. See lode/M2_LINUX_RECLAIM_CHARACTERIZATION_2026-07-14.md.
#
# Run (Linux, peak via /proc VmHWM):
#   NIX_V3_DIRECT_EVAL=1 NIX_VM_STATS=1 v3-eval --file huge-block-transient.nix
#
# Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.
# SPDX-License-Identifier: Apache-2.0
builtins.foldl' (a: i: a + builtins.length (builtins.genList (j: j) 3000000)) 0 (builtins.genList (i: i) 200)
