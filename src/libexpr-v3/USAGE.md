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

Primops (registered in v3's own registry):

  - `length`, `head`, `tail`, `elemAt`
  - `attrNames`, `attrValues`
  - `isAttrs`/`isList`/`isFunction`/`isString`/`isInt`/`isBool`/
    `isNull`/`isFloat`/`isPath`
  - `toString`, `typeOf`, `stringLength`
  - `add`, `sub`, `mul`, `div`, `lessThan`, `throw`
  - `concatLists`, `concatStringsSep`, `substring`
  - `map`, `filter`, `foldl'`, `genList`, `all`, `any`
    (these re-enter the VM via `callClosure` to invoke their function
    arg on each element)
  - All accessible as `__name` (parser uses these for `*`, `-`, etc.)
    AND `builtins.name`.

The lowerer detects `builtins.<name>` and `__<name>` patterns at the
ExprCall callee site and emits a direct `OP_CALL_PRIMOP` (no Closure
allocation, no PrimOpApp).

## Known limitations



Partial application of multi-arg primops:
  ```
  builtins.foldl' (a: b: a + b) 0
  ```
  fails because v3 doesn't yet build PrimOpApp values for partially
  applied primops.  Workaround: add a wrapper lambda
  (`xs: builtins.foldl' (a: b: a + b) 0 xs`).

`builtins` as a standalone value: rejected.  Only `builtins.foo` is
recognized at the lowerer.

Dynamic attrs (`{ ${name} = value; }`): rejected at lowering.

Path SourceAccessor: parsed and stored as string; the accessor
pointer is null (file system operations on paths not yet wired).

Coercion in interpolation: only int/float/bool/null/string/path can
appear in `"${...}"`; attrsets and lists need an explicit
`builtins.toString` call (or attrs may have `__toString`, not yet
supported).

Nix's full primop set: ~150 primops; v3 has 26.  Adding more is
mostly mechanical (register the function on v3::Value).

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
