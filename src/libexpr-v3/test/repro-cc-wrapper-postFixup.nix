# 2026-05-18: minimal reproducer for the cc-wrapper postFixup
# OP_ATTRS_SELECT-on-null failure documented in
# project_cc_wrapper_op_attrs_select_2026-05-18.md.
#
# Forces `stdenv.cc.drvAttrs.postFixup` (the cc-wrapper derivation's
# postFixup shell script) to a string.  This SUCCEEDS in TW
# (returns ~3200-char shell script) but FAILS in v3 with:
#
#   v3 OP_ATTRS_SELECT: not an attrset (tag=4) looking up 'isGNU'
#
# at `pkgs/top-level/all-packages.nix:5536:5` — the `gccForLibs`
# definition's `targetPackages.stdenv.cc.isGNU` access.
#
# At the top level, `pkgs.targetPackages.stdenv.cc.isGNU` evaluates
# correctly to `false` in BOTH v3 and TW.  The failure is
# context-dependent: only when `cc.drvAttrs.postFixup` is forced
# does v3 evaluate `targetPackages.stdenv.cc` to null.
#
# The failure pattern is system-class:
# - aarch64-darwin / x86_64-darwin: OP_ATTRS_SELECT (isGNU on null)
# - aarch64-linux: OP_WITH_LOOKUP (name 'isAarch32' not found in with-scope)
# - x86_64-linux: OP_ATTRS_SELECT (attribute 'toFile' not found)
#
# Same bug CLASS — v3-vs-TW eval-order divergence inside cc-wrapper
# string-interpolation forcing — distinct manifestation per system.
#
# This fixture is for darwin only; mirror copies should be added
# for linux variants when those bug classes are reduced.
#
# Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
# Input Output Group.  SPDX-License-Identifier: Apache-2.0.

(import <nixpkgs> { system = "aarch64-darwin"; }).stdenv.cc.drvAttrs.postFixup
