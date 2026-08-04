# Beta-reduce positive-impact guard.
#
# A pattern that the pass SHOULD inline: many single-use single-arg
# lambdas in a let-binding chain.  When inlined, each lambda body's
# arithmetic folds into a single sequence of binary ops; the
# constant-fold pass (run twice in the pipeline post-beta-reduce)
# collapses the literal arithmetic into a single LitInt.
#
# In effect: this should evaluate to a LitInt at compile time, with
# no runtime computation at all.  With beta-reduce (now unconditional)
# the fixture allocates ~0 thunks beyond startup overhead.
#
# A future bench-harness test could track NIX_VM_STATS allocs on this
# fixture as a perf guardrail; this fixture is the input.
#
# Run:
#   nix eval --impure -f repro-beta-reduce-perf.nix
#   NIX_V3_DIRECT_EVAL=1 \
#     nix eval --impure -f repro-beta-reduce-perf.nix
#
# Expected output (both TW and v3, with or without gate):
#   45
#
# Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
# Input Output Group.  SPDX-License-Identifier: Apache-2.0

let
  a = (x: x + 1) 0;
  b = (x: x + 2) a;
  c = (x: x + 3) b;
  d = (x: x + 4) c;
  e = (x: x + 5) d;
  f = (x: x + 6) e;
  g = (x: x + 7) f;
  h = (x: x + 8) g;
  i = (x: x + 9) h;
in
  i  # 0+1+2+3+4+5+6+7+8+9 = 45
