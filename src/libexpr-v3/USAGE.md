# v3 evaluator — usage

The v3 evaluator is a clean-room replacement for the existing nix
tree-walker / v2 bytecode VM, with a single bytecode pipeline (no
runtime fallback to the tree-walker), 16-byte tagged Values, FAM
upvalues for Closures, and a dispatcher with sub-block emit.

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
  - Comparison: `==`, `!=`, `<`
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

Bindings:
  - `let x = a; y = b; in body` — bindings see prior siblings AND
    all siblings inside lambda/thunk bodies (via prealloc + slot
    pre-pass).  Linear declaration order is required for correct
    eager-value cross-reference.
  - `inherit a b c;` — value comes from parent env (lowered before
    the new let scope is pushed)

Functions:
  - Plain lambdas: `x: body`
  - Formals: `{a, b ? def, ...}: body` and `x@{a, b}: body`
    (defaults supported; `...` accepted)
  - Application (curried via repeated OP_CALL)

Attrsets / lists / select:
  - `{ a = 1; b = 2; }` (non-recursive)
  - `attrs.a.b.c` (chained static select)
  - `attrs.a or default` (single-element path with default)
  - `attrs ? a` (single-element hasAttr)
  - List literals; `++` concat

Primops (registered in v3's own registry on EvalState init):
  - `length`, `head`, `tail`, `elemAt`
  - `attrNames`, `attrValues`
  - `isAttrs`/`isList`/`isFunction`/`isString`/`isInt`/`isBool`/
    `isNull`/`isFloat`/`isPath`
  - `toString`, `typeOf`, `stringLength`
  - `add`, `sub`, `mul`, `div`, `lessThan`, `throw`
  - All of the above accessible as both `__name` (parser uses these
    for `*`, `-`, etc.) and `builtins.name`.

The lowerer detects `builtins.<name>` and `__<name>` patterns at the
ExprCall callee site and emits a direct `OP_CALL_PRIMOP` (no Closure
allocation, no PrimOpApp).

## Known limitations (next-up)

Mutual recursion at construction time:
  ```
  let g = m: f m; f = n: g (n - 1); in g 5
  ```
  fails — g's closure captures f's slot value at MAKE_CLOSURE time
  before f is bound.  Needs an env carrier for proper let-rec.
  Workaround: declare the dependency before the dependent.

Recursive attrsets `rec { ... }`: rejected.  Same env-carrier work
items as let-rec.

Multi-element select with default: `a.b.c or X` rejected.  Single
element works.  Needs short-circuit chaining of HasAttr.

`inherit (from) a b`: rejected.  Needs evaluation of `from` then
attribute selects.

Dynamic attrs (`{ ${name} = value; }`): rejected at lowering.

String interpolation (`"${expr}"`): the parser produces ExprConcatStrings
with `forceString=true`, which lowers correctly to `OP_STR_CONCAT`.
But coercion of attrsets / lists to strings is not yet implemented.

Primops requiring callbacks into the VM (map, filter, foldl', genList):
deferred — would need re-entrant VM invocation from primop bodies.

Path values: parsed and stored; SourceAccessor* is null (file system
operations on paths not yet wired).

Lazy semantics: most bindings are evaluated strictly.  Real laziness
requires thunked lowering for non-trivial RHS expressions.

## Tested examples

```
$ v3-eval '1 + 2 * 3'                               → 7
$ v3-eval '(x: y: x * y) 6 7'                       → 42
$ v3-eval 'if 5 < 10 then "small" else "big"'       → "small"
$ v3-eval '({a, b ? 100}: a + b) { a = 10; }'       → 110
$ v3-eval 'let x = 10; in { inherit x; y = 20; }.x' → 10
$ v3-eval 'let f = n: n + 1; g = m: f m; in g 41'   → 42
$ v3-eval 'let fibImpl = self: n: if n < 2 then n
              else self self (n-1) + self self (n-2);
              fib = fibImpl fibImpl;
            in fib 25'                              → 75025
```
