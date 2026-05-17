# NEGATIVE test 1: writeback must preserve mapAttrs laziness.
#
# Bug to guard against: writeback eagerly forces ALL list entries
# (not just the ones primElem actually iterates).  That would
# violate the lazy `attrValues (mapAttrs throw _)` contract.
#
# Construction: `mapAttrs throw_with_name` over a 3-entry attrset.
# Then call `elem "found" valuesList` where elements are pure
# strings (not the mapped results) — should NOT force the mapped
# entries.  Or rather, only the inspected entries.
#
# Stronger: `attrValues (mapAttrs (n: v: throw n) attrs)` should
# NOT throw on construction; throwing only fires when an entry is
# forced.
#
# Test: build the list, return its length without touching elements.
# Expected: TW returns 3 (no throw).  v3 + fix: 3 (no throw).
# v3 without writeback: 3 (no throw, but only by accident).
#
# This guards against an over-eager fix where someone "forces all
# entries on attrValues construction" — that would throw here and
# diverge from TW.
let
  attrs = { a = 1; b = 2; c = 3; };
  mapped = builtins.mapAttrs (n: _: throw "should-not-fire-${n}") attrs;
  values = builtins.attrValues mapped;
in
  builtins.length values
