# #455 TIGHTEST regression repro (standalone — NO nixpkgs).
#
#   v3-eval:  SPINS (~5 closures, 0 thunks)
#   TW / inline: [ "xa" "xb" ]
#
# `map (<imported-rec-sibling> arg) list` — a sibling fn of a SEPARATELY-IMPORTED
# `rec` module, PARTIALLY APPLIED and passed to `map`. The PAP `(f "x")`'s body
# resolves the sibling `f` via OP_GET_UPVALUE_REC_BINDING_SLOT, which forces the
# captured imported rec-self and cycles (only cross-CU; inline works). This is the
# kernel of #455 — the nixpkgs lib/pkgset fixpoints hit it via
# lib.strings.splitString's `map (addContextFrom s) splits`.
# See lode/RCA_455_VNATIVE_2026-06-10.md.
(import ./repro-455-mod.nix { lib = { }; }).ss
