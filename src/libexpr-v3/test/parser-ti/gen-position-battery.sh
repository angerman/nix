#!/usr/bin/env bash
# v3 parser TI.3 — position-info golden battery generator.
#
# Per PARSER_PROJECT_PLAN_2026-06-01.md §4 (TI.3):
#   "≥20 position-info golden fixtures — the existing 6 are anemic;
#    need positions for if/let/lambda/with/assert/tryEval."
# Per NATIVE_PARSER_FEASIBILITY §3.3 gap #7:
#   "Position info — only 6 unsafeGetAttrPos/__curPos tests.  None
#    cover positions inside if/let/lambda/with/assert/tryEval."
#
# Positions are PARSER output (every AST node carries a PosIdx).
# `--parse` doesn't print them, so we validate via eval:
# `builtins.unsafeGetAttrPos "name" attrs` returns {file,line,column}.
# We strip `file` (environment-dependent) and keep {column,line} so
# goldens are reproducible across machines/checkout-paths.
#
# These are EVAL fixtures (parse→lower→eval).  Under the v3-parser
# path (Stage 1), the v3 parser must record byte-identical line+column
# so these goldens stay green.  TW is the oracle today.
#
# Fixtures are deliberately MULTI-LINE to exercise line + column
# tracking (the lexer's offset→line/col recompute).
#
# Run once to (re)generate; commit fixtures + goldens.
#
# Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
# Input Output Group.  SPDX-License-Identifier: Apache-2.0

set -eu

ROOT="$(cd "$(dirname "$0")/../../../.." && pwd)"
NIX_INST="${NIX_INST:-$ROOT/builddir/src/nix/nix-instantiate}"
FIXDIR="$(cd "$(dirname "$0")" && pwd)/fixtures/position"

mkdir -p "$FIXDIR"

if [[ ! -x "$NIX_INST" ]]; then
    echo "gen-position-battery: nix-instantiate not found at $NIX_INST" >&2
    exit 2
fi

# Each fixture is a complete multi-line .nix expression that extracts
# the position of some attribute via unsafeGetAttrPos and projects to
# {column, line} (dropping the env-dependent file field).  The author
# comment notes which syntactic context the position lives in.
emit() {
    local name="$1"; shift
    local body="$1"
    printf '%s' "$body" > "$FIXDIR/$name.nix"
    if ! out=$("$NIX_INST" --eval --strict "$FIXDIR/$name.nix" 2>/dev/null); then
        echo "GEN ERROR: $name failed to eval" >&2
        rm -f "$FIXDIR/$name.nix"
        return
    fi
    printf '%s\n' "$out" > "$FIXDIR/$name.exp"
    GEN_COUNT=$((GEN_COUNT + 1))
}

GEN_COUNT=0

# --- top-level attrset, attr on a single line ---
emit pos-toplevel-line1 'let p = builtins.unsafeGetAttrPos "x" { x = 1; }; in { inherit (p) column line; }'

emit pos-attr-line2 "$(printf 'let p = builtins.unsafeGetAttrPos "x" {\n  x = 1;\n}; in { inherit (p) column line; }\n')"

emit pos-attr-line3-col5 "$(printf 'let\n  s = {\n    x = 1;\n  };\n  p = builtins.unsafeGetAttrPos "x" s;\nin { inherit (p) column line; }\n')"

# --- rec attrset ---
emit pos-rec-attr "$(printf 'let\n  s = rec {\n    a = 1;\n    b = a;\n  };\n  p = builtins.unsafeGetAttrPos "b" s;\nin { inherit (p) column line; }\n')"

# --- nested attrset (position of inner attr) ---
emit pos-nested-inner "$(printf 'let\n  s = {\n    outer = {\n      inner = 42;\n    };\n  };\n  p = builtins.unsafeGetAttrPos "inner" s.outer;\nin { inherit (p) column line; }\n')"

# --- attr produced from a let binding ---
emit pos-let-derived "$(printf 'let\n  mk = v: { y = v; };\n  s = mk 7;\n  p = builtins.unsafeGetAttrPos "y" s;\nin { inherit (p) column line; }\n')"

# --- attr inside a lambda body ---
emit pos-lambda-body "$(printf 'let\n  f = arg: {\n    z = arg;\n  };\n  p = builtins.unsafeGetAttrPos "z" (f 1);\nin { inherit (p) column line; }\n')"

# --- attr inside a with-expression scope ---
emit pos-with-scope "$(printf 'let\n  env = { base = { w = 9; }; };\n  s = with env; base;\n  p = builtins.unsafeGetAttrPos "w" s;\nin { inherit (p) column line; }\n')"

# --- attr inside an if/then/else branch ---
emit pos-if-branch "$(printf 'let\n  s = if true\n      then { t = 1; }\n      else { t = 2; };\n  p = builtins.unsafeGetAttrPos "t" s;\nin { inherit (p) column line; }\n')"

# --- attr guarded by assert ---
emit pos-assert-guarded "$(printf 'let\n  s = assert true;\n    { g = 5; };\n  p = builtins.unsafeGetAttrPos "g" s;\nin { inherit (p) column line; }\n')"

# --- attr surfaced through tryEval ---
emit pos-tryeval "$(printf 'let\n  s = (builtins.tryEval { e = 1; }).value;\n  p = builtins.unsafeGetAttrPos "e" s;\nin { inherit (p) column line; }\n')"

# --- attr in deeply-indented context (column tracking) ---
emit pos-deep-indent "$(printf 'let\n  s = {\n            deep = 1;\n  };\n  p = builtins.unsafeGetAttrPos "deep" s;\nin { inherit (p) column line; }\n')"

# --- attr after a multi-line indented string (line tracking across
#     the lexer's IND_STRING state, which must count embedded newlines).
#     Written via heredoc because the Nix '' delimiters are awkward to
#     embed in a single-quoted printf. ---
cat > "$FIXDIR/pos-after-string.nix" <<'NIXEOF'
let
  s = {
    doc = ''
      multi
      line
    '';
    after = 1;
  };
  p = builtins.unsafeGetAttrPos "after" s;
in { inherit (p) column line; }
NIXEOF
if out=$("$NIX_INST" --eval --strict "$FIXDIR/pos-after-string.nix" 2>/dev/null); then
    printf '%s\n' "$out" > "$FIXDIR/pos-after-string.exp"
    GEN_COUNT=$((GEN_COUNT + 1))
else
    echo "GEN ERROR: pos-after-string failed to eval" >&2
    rm -f "$FIXDIR/pos-after-string.nix"
fi

# --- attr after a comment block (line tracking across comments) ---
emit pos-after-comment "$(printf 'let\n  s = {\n    # line comment\n    /* block\n       comment */\n    c = 1;\n  };\n  p = builtins.unsafeGetAttrPos "c" s;\nin { inherit (p) column line; }\n')"

# --- attr in a // merged attrset (which side wins position) ---
emit pos-update-merge "$(printf 'let\n  a = { m = 1; };\n  b = {\n    m = 2;\n  };\n  s = a // b;\n  p = builtins.unsafeGetAttrPos "m" s;\nin { inherit (p) column line; }\n')"

# --- attr defined via inherit ---
emit pos-inherit "$(printf 'let\n  src = { i = 1; };\n  s = {\n    inherit (src) i;\n  };\n  p = builtins.unsafeGetAttrPos "i" s;\nin { inherit (p) column line; }\n')"

# --- attr with dynamic-looking but static key ---
emit pos-quoted-key "$(printf 'let\n  s = {\n    "quoted-key" = 1;\n  };\n  p = builtins.unsafeGetAttrPos "quoted-key" s;\nin { inherit (p) column line; }\n')"

# --- nested let, attr several levels in ---
emit pos-nested-let "$(printf 'let\n  outer =\n    let\n      mid = {\n        leaf = 1;\n      };\n    in mid;\n  p = builtins.unsafeGetAttrPos "leaf" outer;\nin { inherit (p) column line; }\n')"

# --- attrset spanning many lines, last attr ---
emit pos-many-lines-last "$(printf 'let\n  s = {\n    a1 = 1;\n    a2 = 2;\n    a3 = 3;\n    a4 = 4;\n    a5 = 5;\n  };\n  p = builtins.unsafeGetAttrPos "a5" s;\nin { inherit (p) column line; }\n')"

echo "Generated $GEN_COUNT position fixtures + goldens in $FIXDIR"
