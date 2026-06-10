# Minimal reproducers for #455 (current, v3-native manifestation) — found 2026-06-10.
#
# v3-eval SPINS (non-terminating force loop, 0 thunks allocated); TW completes instantly.
# This was THE blocker for `(import <nixpkgs> {})` under pure v3-direct.
#
# Narrowing chain (each spins in v3, instant in TW):
#   (import nixpkgs {}).lib.version                              ~71 closures  (pkgset fixpoint)
#   → lib.systems.elaborate "x86_64-linux"                       ~30 closures  (platform elaborate)
#   → lib.systems.parse.mkSystemFromString "x86_64-linux"        ~16 closures
#   → lib.strings.splitString "-" "x86_64-linux"                 ~10 closures  ← TIGHTEST
#
# ROOT CAUSE (localized, not yet fixed): the *logic* is fine — splitString inline, a
# standalone `rec` lib, and a standalone fixpoint lib ALL evaluate correctly; only the
# nixpkgs `lib.strings.splitString` (the function as COMPILED inside the full nixpkgs
# lib) spins. Its `splits` thunk captures upvalue0 (= `escapeRegex`) + sep + s; forcing
# it loops — consistent with a mis-resolved upvalue in the compiled lib function (the
# #495 "double-lowering / stale-upvalue-arity" class). `builtins.split`/`filter`/`map`
# and every piece work in isolation. See lode/RCA_455_VNATIVE_2026-06-10.md.
#
# Run: NIX_PATH=nixpkgs=<path> v3-eval --file repro-455-systems-elaborate.nix --strict
#
# TIGHTEST repro (this file): lib.strings.splitString.  (elaborate/mkSystemFromString
# are the same root cause one layer up — see the RCA.)
(import <nixpkgs>/lib).strings.splitString "-" "x86_64-linux"
