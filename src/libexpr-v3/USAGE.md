# v3 evaluator — usage

The v3 evaluator is a clean-room replacement for the existing nix
tree-walker / v2 bytecode VM, with a single bytecode pipeline (no
runtime fallback to the tree-walker), 16-byte tagged Values, FAM
upvalues for Closures, and a re-entrant dispatcher.

This file documents what works **today** so we don't have to guess.
Update it as coverage grows.

## Building

```bash
# from src/libexpr-v3
nix develop -c make help     # show all targets
nix develop -c make check    # build + run hand-built IR smoke tests
nix develop -c make check-debug  # same with ASan + UBSan

# Top-level meson build (also produces v3-eval CLI)
nix develop -c ninja -C build src/libexpr-v3/v3-eval
nix develop -c ninja -C build src/libexpr-v3/v3-smoke
./build/src/libexpr-v3/v3-eval '<expression>'
```

## v3-eval CLI

`v3-eval EXPR` parses EXPR through nix's parser, runs `bindVars`,
lowers to v3 IR, compiles to bytecode, and runs through the v3 VM.

Exits with status 0 on success; prints the resulting value on stdout.
On unsupported AST shapes or runtime errors, prints `v3-eval error: …`
on stderr and exits 1.

## What the lowerer supports

Literals: `Int`, `Float`, `Bool`, `Null`, `String`, `Path`.

Operators:
  - Arithmetic: `+`, `-`, `*`, `/` (mixed Int/Float promoted)
  - Comparison: `==`, `!=`, `<`, `>`, `<=`, `>=` (via primops + lowering)
  - Logical: `&&`, `||`, `->`, `!` (short-circuit branches via inline
    sub-blocks)
  - String/numeric coercion via `+` (the parser desugars to
    `ConcatStrings`; the VM dispatches on operand types)
  - Attrset update `//`
  - List concat `++`

Control flow:
  - `if-then-else`
  - `with attrs; body` (runtime with-stack)
  - `assert cond; body`

Bindings (full mutual recursion):
  - `let x = a; y = b; in body` — env-carrier letrec.  References
    inside thunk bodies and the body itself resolve via
    AttrSelect+Force on the rec attrset, so mutual recursion works.
  - `inherit a b c;`
  - `inherit (from) a b;`  (for both rec and non-rec attrsets)
  - `rec { ... }` — same env-carrier path as let.

Functions:
  - Plain lambdas: `x: body`
  - Formals: `{a, b ? def, ...}: body` and `x@{a, b}: body`
    (defaults supported; `...` accepted)
  - Application (curried via repeated OP_CALL)

Attrsets / lists / select:
  - `{ a = 1; b = 2; }` (non-recursive)
  - `rec { ... }` (recursive)
  - `attrs.a.b.c` (chained static select with implicit Force)
  - `attrs.a.b.c or default` (any path length; default applies if any
    segment is missing — short-circuit chain of HasAttr)
  - `attrs ? a` (single-element hasAttr)
  - List literals; `++` concat

Primops (81 registered in v3's own registry):

Lists / collections:
  length, head, tail, elemAt, concatLists, concatMap, partition, sort,
  map, filter, foldl', genList, all, any, elem, splitString,
  concatStringsSep

Attrsets:
  attrNames, attrValues, getAttr, hasAttr, removeAttrs, intersectAttrs,
  mapAttrs, listToAttrs, catAttrs, groupBy, genericClosure,
  functionArgs

Type predicates / coercion:
  isAttrs, isList, isFunction, isString, isInt, isBool, isNull,
  isFloat, isPath, typeOf, toString, stringLength, parseInt

Arithmetic / numeric:
  add, sub, mul, div, lessThan, bitAnd, bitOr, bitXor, floor, ceil,
  compareVersions, parseDrvName

Strings:
  substring, replaceStrings, match, split, hashString (FNV stub)

I/O / files:
  readFile, readDir, pathExists, baseNameOf, dirOf, import

JSON:
  toJSON, fromJSON

Evaluation control:
  throw, abort, seq, deepSeq, tryEval

System info (0-arity):
  currentSystem, currentTime, nixVersion, getEnv

Internals (parser desugaring):
  __add, __sub, __mul, __div, __lessThan

The lowerer detects `builtins.<name>` and `__<name>` patterns at the
ExprCall callee site and emits a direct `OP_CALL_PRIMOP` (no Closure
allocation, no PrimOpApp).  `builtins.<name>` for arity-0 primops is
auto-invoked at access time; for higher arity it produces a
Tag::PrimOp value that participates in PrimOpApp partial application.

## Known limitations

Store / derivation primops (need real Nix store integration):
  - derivation, derivationStrict (stub returns input attrs as-is)
  - exec, filterSource, importNative, outputOf
  - path, toFile, fetchurl, fetchTarball
  - findFile / __findFile (NIX_PATH lookup)
  - scopedImport
  - String-context primops are no-context stubs:
    unsafeDiscardStringContext, hasContext, getContext,
    unsafeDiscardOutputDependency

Lazy evaluation gaps:
  - Mutually-circular formal defaults like `{ a ? b, b ? a }: ...`
    when only one side is provided.  Tree-walker uses per-default
    thunks; v3 currently reads sibling slots eagerly.  Forward refs
    (`b ? a + 1`) and backward refs (`a ? b - 1`) work fine.
  - `rec { ${dyn} = ...; }` — recursive attrset with dynamic attr
    names lowers to "unsupported AST node".

Coercion in interpolation: only int/float/bool/null/string/path can
appear in `"${...}"`; attrset-with-`__toString` and lists need
explicit conversion.

Position tracking: `__curPos`, `unsafeGetAttrPos`, builtins.unsafeGetAttrPos
return null since v3 doesn't track parser positions yet.

Performance: v3 is roughly on par with the tree-walker (within ~30%)
on compute-bound benchmarks.  Bigger wins are queued (NaN-boxing,
computed-goto dispatch, Bindings polymorphism, etc).

## Test status

The official `tests/functional/lang/eval-okay-*.nix` lang suite:
**116 / 143 passing** (81%).  Remaining failures are concentrated in:
  - flake / store primops not yet wired (parseFlakeRef,
    flakeRefToString, builtins.path with `path = ./.`, fromTOML,
    toxml/toxml2)
  - rec attrsets with dynamic attr names (3 tests)
  - position tracking primops (`__curPos`, `unsafeGetAttrPos`)
  - scopedImport, __findFile, NIX_PATH lookup (`<x>` syntax)
  - tests that need a real `derivation` definition (delayed-with,
    eq-derivations, context*) — v3 aliases `derivation` to
    `derivationStrict` as a stub
  - `--xml` output format (eval-okay-xml, eval-okay-autoargs)

The 77-case v3-vs-tree-walker regression suite at
`src/libexpr-v3/test/run-v3-tests.sh` is fully passing
(byte-identical output for 67, display-only diff for 10).

## Recent feature additions

`builtins` as a standalone value: produces an attrset of all
registered primops on demand (used by `with builtins; …` and
`inherit (builtins) substring;`).

Cross-CU calls: closures returned from `builtins.import` now carry
their own CompilationUnit so cross-file `(import lib.nix) attrs`
patterns work.  primImport caches CUs in a process-global ImportCache
so the bytecode they own outlives any closure that points into it.

Lazy attrset values: every non-trivial attribute value is wrapped in
a thunk at lowering time, so building `{a = 1; b = throw "x";}`
doesn't fire the throw until `b` is forced.  Trivial values
(literals, var refs, lambdas) skip the wrapper.

Lazy function args: non-primop calls thunkify their non-trivial
arguments so `f (throw "x")` only throws when `f` actually demands
the value.

Dynamic attr names in nested select: `attrs.${k}.deeper` works.
Dynamic attrs (`{ ${name} = value; }`) work as the top-level
attrset; rec + dynamic combination is still rejected.

Mutually-recursive lambda formals (one-direction): `{a, b ? a + 1}`,
`{a ? b - 1, b ? 10}` and `{a ? 1, b ? a}` all lower correctly.
Symmetric circular defaults `{a ? b, b ? a}` still fail eagerly.

App / lazy-callback primops: `mapAttrs` builds Tag::App entries that
defer the `fn name value` call until the entry is forced — matches
tree-walker laziness, fixes `intersectAttrs` against
`mapAttrs throw alphabet`.

Multi-arg primop callbacks via callClosure: builds PrimOpApp on
under-application and walks a PrimOpApp chain to invoke once the
arity is reached.  Fixes `sort builtins.lessThan list-of-lists`.

## Tested examples

```
$ v3-eval '1 + 2 * 3'                                        → 7
$ v3-eval '(x: y: x * y) 6 7'                                → 42
$ v3-eval 'if 5 < 10 then "small" else "big"'                → "small"
$ v3-eval '({a, b ? 100}: a + b) { a = 10; }'                → 110
$ v3-eval 'let fact = n: if n == 0 then 1
                          else n * fact (n - 1); in fact 8'  → 40320
$ v3-eval 'let isEven = n: if n == 0 then true
                            else isOdd (n - 1);
              isOdd  = n: if n == 0 then false
                            else isEven (n - 1);
            in isEven 10'                                     → true
$ v3-eval 'rec { a = 1; b = a + 1; c = b + 1; }.c'           → 3
$ v3-eval 'let foo = { x = 100; }; bar = { y = 200; };
            in { inherit (foo) x; inherit (bar) y; }.y'      → 200
$ v3-eval 'builtins.foldl'"'"' (a: b: a + b) 0
            (builtins.map (x: x * x)
              (builtins.genList (i: i + 1) 5))'              → 55
$ v3-eval 'let pkgs = rec {
              name = "v3"; version = "1.0";
              fullname = "${name}-${version}";
              config = { enableFoo = true;
                          buildInputs = [ "gcc" "make" ]; };
            };
            in pkgs.fullname + " with "
                + builtins.toString
                    (builtins.length pkgs.config.buildInputs)
                + " inputs"'                                  → "v3-1.0 with 2 inputs"
```
