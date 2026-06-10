# #455 minimal-repro module (imported by repro-455-import-papmap.nix).
# A separately-imported `rec` module (its own compilation unit). `ss` maps a
# PARTIALLY-APPLIED sibling (`f "x"`) over a list. Forcing `ss` cycles in v3
# (the PAP's OP_GET_UPVALUE_REC_BINDING_SLOT re-forces the imported rec-self),
# while the IDENTICAL module inlined (`let m = rec {…}; in m.ss`) works.
{ lib }:
rec {
  f = a: b: a + b;
  ss = map (f "x") [ "a" "b" ];
}
