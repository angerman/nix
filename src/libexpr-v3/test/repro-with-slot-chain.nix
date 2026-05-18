# Regression test for commit 8e5fd7614:
#   v3 withLookup: chase multi-level Slot indirections with cycle detect
#
# Pre-fix: v3's OP_WITH_LOOKUP did a SINGLE Slot deref then checked
# `isAttrs`.  nixpkgs's `lib.fix` / `extends` / `callPackage` can
# produce chains of Slot indirections (Slot → Slot → Slot → ... → Attrs)
# where the single deref landed on the next Slot, fell through
# `!isAttrs()`, and SKIPPED the with-entry silently — even though the
# chain resolves to an attrset containing the looked-up name.
#
# This synthetic fixture builds a 4-deep let-rec chain that produces
# the same Slot indirection shape, then uses `with` to look up a name
# inside the chain.  Pre-fix: throws OP_WITH_LOOKUP miss.  Post-fix:
# resolves correctly, matching TW.
#
# Cycle detection (also in commit 8e5fd7614): on a Slot pointer revisit
# during chase, withLookup bails to "blackholed" semantics (skips the
# scope, tries outer).  The chain-depth backstop NIX_V3_WITH_CHAIN_LIMIT
# (default 64) throws a clear error with the chain dump if exceeded —
# tested manually since fixture chains stay shallow.
#
# Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
# Input Output Group.  SPDX-License-Identifier: Apache-2.0.

let
  # Build a 4-level let-rec chain.  Each layer is a let-rec whose
  # body is itself another let-rec.  The innermost layer defines the
  # actual `targetAttrset` containing the test name.
  layer4 = rec {
    targetAttrset = { isAarch32 = false; isLinux = true; magic = "ok"; };
  };

  layer3 = rec {
    inherit (layer4) targetAttrset;
  };

  layer2 = rec {
    inherit (layer3) targetAttrset;
  };

  layer1 = rec {
    inherit (layer2) targetAttrset;
  };

  # Now look up `magic` via with-scope on the chained attrset.
  #
  # Pre-fix: this fails with "OP_WITH_LOOKUP: name 'magic' not found
  # in with-scope" because the chain of let-rec slots produces multiple
  # Slot indirections that the single-level deref couldn't follow.
  # Post-fix: chase walks the chain to the final attrset, finds `magic`.
  viaWithLookup = with layer1.targetAttrset; magic;

  # Cross-checks: confirm the chain DOES resolve correctly via direct
  # attr-access (which has always worked because OP_ATTRS_SELECT chases
  # internally).
  viaDirectAccess = layer1.targetAttrset.magic;

  # And via `inherit` propagation: layer1.targetAttrset === layer4.targetAttrset.
  sameRef = layer1.targetAttrset == layer4.targetAttrset;

in
{
  inherit viaWithLookup viaDirectAccess sameRef;
}
