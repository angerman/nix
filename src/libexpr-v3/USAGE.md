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

## NIX_USE_V3 cutover

Setting `NIX_USE_V3=1` routes the regular `nix` CLI's evaluator
through v3:

    NIX_USE_V3=1 ./build/src/nix/nix eval --expr '1 + 2'    # 3
    NIX_USE_V3=1 ./build/src/nix/nix eval --expr 'fib 30 ...'

The hook is wired in via an explicit `nix::v3::installEvalHook()`
call from `nix::mainWrapped`.  This serves two purposes: it
populates the function pointer that `EvalState::eval` consults, and
it provides a strong symbol reference so macOS's
`-dead_strip_dylibs` can't remove libnixexprv3 from the binary.

Real-world behaviour today (single-invocation runs):

  - **fib30**: v3 cutover at 0.37s user, slight win over
    tree-walker's 0.38s.
  - **`(import <nixpkgs> {}).hello.name`**: v3 cutover at 0.25s user,
    tree-walker at 0.25s — **parity**.
  - **`attrNames pkgs.haskellPackages` count**: v3 0.45s vs
    tree-walker 0.45s — parity.

How parity was reached:

  - **Static short-circuits** route Expr kinds where v3's
    lower+compile+run cycle is net-negative versus tree-walker
    directly to tree-walker.  Currently short-circuited:
      * Literals (Int / Float / String / Path / Bool-singleton)
      * Var, Pos
      * Lambda (would return Closure → bridge can't hand back)
      * Attrs, List (file-toplevel; tree-walker constructs lazy
        thunks faster than v3's eager eval + recursive bridge)
  - **`willReturnClosure` predicate** walks the AST through Let /
    With / Assert / If-with-both-Lambda-branches and short-circuits
    when the result is statically a closure.

Per-phase profile (V3_TIMING=1, hello.name):

    v3 hook timing (ms):  lower=2.9  compile=0.43  run=0.08  bridge=0.002

(Was 21.8 ms total before the short-circuits landed.)

Set `NIX_VM_STATS=1` to see hook invocation counts; `V3_TIMING=1`
adds per-phase timing (lower / compile / run / bridge):

    NIX_VM_STATS=1 V3_TIMING=1 NIX_USE_V3=1 nix eval --json --expr '...'

## Benchmark harness

`src/libexpr-v3/test/bench-v3-vs-tw.sh` times v3 vs. tree-walker
across a fixed set of workloads.  Use it to attribute every "perf
win" commit to a measurable delta.

    # Default: 3 runs/cell, table output.  Synthetic workloads only.
    bash src/libexpr-v3/test/bench-v3-vs-tw.sh

    # Add nixpkgs workloads.
    NPK=$HOME/src/nixpkgs bash src/libexpr-v3/test/bench-v3-vs-tw.sh

    # Higher run count for tighter stats.
    N=10 bash src/libexpr-v3/test/bench-v3-vs-tw.sh

    # Subset by workload name.
    ONLY=fib35,letrec-fix bash src/libexpr-v3/test/bench-v3-vs-tw.sh

    # Raw CSV (one row per run) for graphing.
    FORMAT=csv N=20 bash src/libexpr-v3/test/bench-v3-vs-tw.sh \
        > /tmp/bench-$(date +%Y%m%d-%H%M).csv

Workloads cover compute-bound (fib35, ackermann), attrset path
chains (path-deep, letrec-fix), and -- when nixpkgs is available --
the nixpkgs-cold-path queries dominant in real-world use
(hello-name, git-name, drv3, attr-pkgs, attr-hask).

## CO-2 / CO-3: forceValue cutover (opt-in)

Set `NIX_USE_V3_FORCE=1` (in addition to `NIX_USE_V3=1`) to enable
the forceValue cutover hook.  When the lowered+compiled v3 module
recorded an Expr* match for a given thunk-body, force-time
dispatch to that v3 function instead of tree-walker's expr->eval.

Stats with the force hook on:

    v3 force stats: forceEntries=N forceHits=H forceMisses=M skippedNeedsUpvalues=S

What the counters mean:
  - `forceEntries`: every forceValue call where the Expr* lookup
    fired; dominated by sub-Expr forces.
  - `forceHits`: cache hit + actually ran in v3.
  - `forceMisses`: cache miss; fell through to expr->eval.
  - `skippedNeedsUpvalues`: cache hit, but the function needs
    upvalues we can't yet translate from tree-walker's env.
    Phase B (task #278) unblocks these.

Currently the force hook is a small *regression* on real-world
workloads (~8% slower on hello.name) because the per-call hash-map
lookup outweighs the few hits that don't need upvalues.  Phase B
lifts the upvalue restriction, at which point most thunks can
flow through v3 and the trade-off should reverse.

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

Store / derivation primops:
  - derivationStrict / builtins.path: bridged to tree-walker under
    settings.readOnlyMode = true, so they produce deterministic
    `/nix/store/<32-char-hash>-name` paths.  The bridge fails for
    inputs containing a v3 closure (e.g. `builtins.path { filter = ...; }`)
    and falls back to a fake `/v3-fake-store/<16-hex-hash>-name`.
  - scopedImport: works (synthesises `__scope__: let foo = __scope__.foo;
    bar = __scope__.bar; ... in (<imported file>)` to shadow base-env
    primops the same way tree-walker stacks a new StaticEnv).
  - findFile / __findFile (NIX_PATH + corepkgs `<nix/...>` lookup): works.
  - Path interpolation `${./file}` produces the proper
    `/nix/store/<32-hash>-name` form via tree-walker's copyPathToStore.
  - exec, filterSource, importNative, outputOf, toFile, fetchurl,
    fetchTarball — not implemented.
  - String contexts: tracked via a side-table keyed by Tag::String
    payload pointer.  Path interpolation `${./file}` tags the result
    with an Opaque context entry; tree-walker strings (e.g. drvPath,
    outPath via the derivation bridge) preserve their context across
    the bridge.  All context primops work: getContext, hasContext,
    unsafeDiscardStringContext, unsafeDiscardOutputDependency,
    addDrvOutputDependencies, appendContext.

Lazy evaluation:
  - Mutually-circular formal defaults like `{ a ? b, b ? a }: ...`
    when only one side is provided.  Tree-walker uses per-default
    thunks; v3 currently reads sibling slots eagerly.  Forward refs
    (`b ? a + 1`) and backward refs (`a ? b - 1`) work fine.
  - Delayed `with` works: with-stack entries are pushed unforced and
    forced lazily on lookup.  Blackholes that bubble out from a
    deeper force are skipped so outer scopes still get a chance.

Coercion in interpolation: int/float/bool/null/string/path/attrset-
with-__toString-or-outPath are supported; lists still need explicit
conversion.

Position tracking: `__curPos` materializes {file, line, column} for
the call site.  `unsafeGetAttrPos` and `functionArgs`-derived
positions both work via the per-attr position side-table populated
by OP_ATTRS_INIT[_DYN] / OP_ATTRS_REC_INIT.

Performance: v3 is at parity with the tree-walker on compute-bound
benchmarks and on real-world nixpkgs evaluation, and significantly
faster on attrset-heavy workloads.

  fib30:  tree-walker 0.37s user, v3 0.35s user  (slightly faster
                                                    after TCO + slot elision)
  fib32:  tree-walker 0.93s user, v3 0.94s user  (~1% gap)
  fib34:  tree-walker 2.41s user, v3 2.42s user  (~0.5% gap)
  attrs10k (10000 // merges + foldl' over attrNames):
          tree-walker 0.34s user, v3 0.10s user  (3.4× faster)
  haskellPackages attrNames length:
          tree-walker 0.44s user, v3 0.44s user  (matched)
  pkgs.stdenv attrNames length:
          tree-walker 0.24s user, v3 0.23s user  (matched)
  pkgs.hello.meta.description:
          tree-walker 0.23s user, v3 0.23s user  (matched)
  cardano-node flake evaluation (warm cache):
          tree-walker 0.43s user / 3.65s real,
                v3 0.43s user / 3.61s real  (matched)

Real-package instantiation through `nix-instantiate --dry-run`
(produces a /nix/store/...drv path).  v3 produces byte-identical
drvPaths to tree-walker and is consistently a few % faster:

  hello: tree-walker 0.28s user, v3 0.26s user (~7% faster)
  vim:   tree-walker 0.28s user, v3 0.27s user (~3% faster)
  git:   tree-walker 0.39s user, v3 0.37s user (~5% faster)

Heavy nixpkgs scan — filter+count all 25 070 top-level package
attrsets (`builtins.length (builtins.filter (n: builtins.isAttrs
(tryEval pkgs.${n}).value) (attrNames pkgs))`).  Both produce 25070:
  tree-walker: 9.06s user / 10.68s real
  v3:          8.95s user /  7.78s real  (~1% less CPU, ~27% less wall)

NixOS module evaluation — full sample config with grub / firewall /
ssh / nginx / postgresql / users / packages.  Both produce 59
systemd services:
  tree-walker: 0.59s user / 0.73s real
  v3:          0.59s user / 0.75s real  (matched)

Tail-call optimization: 100,000 recursive tail calls
(`let f = n: if n == 100000 then n else f (n + 1); in f 0`) now
runs in O(1) frame stack space.  Bounded against true infinite
recursion (`(x: x x) (x: x x)`) by a 10⁷ tail-call iteration
counter that resets on any non-tail call/return.

Recent perf wins (in-VM hot path):
  - OP_FORCE peek-fast-path: skip pop+push when top is already WHNF.
  - OP_SET_LOCAL fast-path: skip the grow loop when slot is in range.
  - Superinstructions OP_GET_LOCAL_FORCE / OP_GET_UPVALUE_FORCE: fuse
    the var-load+force pair (the most common bytecode pair).
  - OP_STR_CONCAT 2-int fast-path: every `a + b` over ints sums
    in-place on the operand stack without allocating.
  - OP_EQ / OP_NEQ / OP_LESS int-int fast paths: every numeric
    predicate inlines the comparison without a helper call.

Still queued for the larger wins: NaN-boxing, computed-goto dispatch,
Bindings polymorphism, OP_ATTRS_SELECT inline cache (already done for
the simple case).

## Test status

The official `tests/functional/lang/eval-okay-*.nix` lang suite:
**142 / 142 passing** (one test is `.exp-disabled` upstream).  Run via:

    bash src/libexpr-v3/test/run-lang-tests.sh

The official `tests/functional/lang/eval-fail-*.nix` lang suite:
**103 / 109 raise the expected error**, zero crashes.  The 6 remaining
silent passes are intentional divergences:

- 4 require tree-walker-specific lint flags
  (`--lint-absolute-path-literals fatal` etc.): abs-path-fatal,
  home-path-fatal, short-path-literal, url-literal.  These test
  tree-walker features v3 does not implement.
- 2 expect a stack-overflow trap on deep recursion: toJSON-stack-overflow,
  derivation-structuredAttrs-stack-overflow.  v3's iterative toJSON
  succeeds where tree-walker overflows — divergence is by design.

Run via:

    bash src/libexpr-v3/test/run-fail-tests.sh

The 77-case v3-vs-tree-walker regression suite at
`src/libexpr-v3/test/run-v3-tests.sh`: 76/77 passing (1 display-only).

End-to-end cutover validation via the regular `nix-instantiate` CLI
with `NIX_USE_V3=1` set — exercises the libnixexpr → libnixexprv3
hook + result-conversion shim:

    bash src/libexpr-v3/test/run-cutover-tests.sh

**142 / 142 passing** (all upstream eval-okay tests, including
those with `.flags` files like `--lint-absolute-path-literals`,
`--extra-experimental-features parse-toml-timestamps`, and
`-I` lookup-path entries).

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
