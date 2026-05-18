# Regression test for commit bb3e31b87:
#   v3 primDerivationStrictNative: scope structured-attrs coerce to
#   special fields only
#
# Pre-fix: under __structuredAttrs the native derivation path
# iterated args and called `valueToJsonWithContext` (JSON-encode)
# AND `v3CoerceToString` (string-coerce, discard result) on each
# attr.  The v3CoerceToString throw on attrset-without-outPath-or-
# __toString sent 63 darwin-clang-wrapper-touching derivations on
# `hello.drvPath` through the TW bridge unnecessarily (V3_DRV_DEBUG
# trace + project_bridge_primop_status_2026-05-18.md for the data).
#
# Fix: gate the coerce on the actual special-field set
# (builder/system/outputHash*); the JSON encoding remains
# unconditional and handles arbitrary attrset values correctly.
#
# Positive test: a structured-attrs derivation with an env attribute
# whose value is a NESTED ATTRSET (which `v3CoerceToString` cannot
# coerce because it has neither `__toString` nor `outPath`).
# Both v3 and TW must produce the same byte-identical drvPath.
#
# Why this catches a regression: pre-fix v3 threw OP_ATTRS_SELECT
# on the nested attrset during primDerivationStrictNative's loop.
# Re-introducing the unconditional v3CoerceToString call would
# resurface that throw, making this fixture diverge from TW.
#
# Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
# Input Output Group.  SPDX-License-Identifier: Apache-2.0.

(derivation {
  name = "structured-attrs-nested-env-test";
  system = "x86_64-linux";
  builder = "/bin/sh";
  __structuredAttrs = true;

  # The triggering shape: an env attr that's a nested attrset.
  # Under __structuredAttrs this is valid (gets JSON-encoded into
  # the .drv's __json field).  Without the fix, v3 throws because
  # the discard-result v3CoerceToString call on this value fails.
  SOME_CONFIG = {
    inner = "value1";
    nested = {
      deep = "value2";
    };
  };

  # Another nested-attrset env attr for double coverage.
  ANOTHER_ATTR = {
    key1 = "v1";
    key2 = "v2";
  };
}).drvPath
