# #455 POSITIVE test — the splitString LOGIC is correct in v3.
#
# A standalone fixpoint-lib of the EXACT nixpkgs shape (module gets `lib = self`,
# splitString captures sibling rec-members `escapeRegex`/`addContextFrom` as upvalues).
# This MUST evaluate to `[ "x86_64" "linux" ]` instantly — it does today, and the #455
# fix (which corrects upvalue resolution for the function as compiled inside the *real*
# nixpkgs lib) MUST NOT regress it.  Pure (no <nixpkgs>), so always runnable.
let
  stringsModule =
    { lib }:
    rec {
      stringToCharacters = s: builtins.genList (i: builtins.substring i 1 s) (builtins.stringLength s);
      escape = list: builtins.replaceStrings list (map (c: "\\${c}") list);
      escapeRegex = escape (stringToCharacters "\\[{()^$?*+|.");
      addContextFrom = a: b: builtins.substring 0 0 a + b;
      splitString =
        sep: s:
        let
          splits = builtins.filter builtins.isString (
            builtins.split (escapeRegex (toString sep)) (toString s)
          );
        in
        map (addContextFrom s) splits;
    };
  lib = { strings = stringsModule { inherit lib; }; };
in
lib.strings.splitString "-" "x86_64-linux"
