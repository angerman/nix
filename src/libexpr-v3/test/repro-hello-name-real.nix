# Regression / smoke: hello.name from real nixpkgs.
#
# This is the Phase 1 exit-criterion fixture from the action plan
# (`hello.name evaluates without C-stack overflow`).  It's also the
# overall correctness guard that protects the whole Option 4 hybrid:
# if this regresses, something in the wrapper / FFI leaf / iterative
# force / Tag::App memo broke.
#
# Wall-time target: ≤2× TW (TW ~0.5s; v3 ~0.6s as of 2026-05-18).
#
# Run:
#   NIX_V3_DIRECT_EVAL=1 NIX_V3_SKIP_INSTALLABLE_PREEVAL=1 \
#     nix eval --impure -f repro-hello-name-real.nix
#
# Expected output (both TW and v3):
#   "hello-2.12.3"
#
# (Version tracks whatever the user's <nixpkgs> channel points at; the
# test driver should compare to TW output, not a hard-coded string.)
#
# Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
# Input Output Group.  SPDX-License-Identifier: Apache-2.0

(import <nixpkgs> {}).hello.name
