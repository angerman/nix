#!/usr/bin/env bash
# WC-31 / WC-34 / WC-35 lazy-semantics regression suite.
#
# Each test in this file targets a SPECIFIC bug fixed during the
# pure-VM nixpkgs investigation.  The test exists to prevent a future
# refactor (or revert of an isolated commit) from silently re-breaking
# laziness.  Every entry is annotated with:
#   - the commit hash that made it pass
#   - what the bug looked like before the fix (so a regression is
#     identifiable from the test name alone)
#
# Usage:
#   ./run-wc-laziness-tests.sh                 # summary
#   V3_LAZINESS_VERBOSE=1 ./run-wc-laziness-tests.sh
#   V3_LAZINESS_PATTERN=WC-31 ./...            # only WC-31 cases
#
# Each test is a triple (id, expression, expected_output).  expected
# is what tree-walker / v3 should both produce.  A failure in v3 alone
# pinpoints WHICH fix regressed.

set -u

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
V3="${V3:-$ROOT/build/src/libexpr-v3/v3-eval}"
TW="${TW:-$ROOT/build/src/nix/nix-instantiate}"

if [[ ! -x "$V3" ]]; then
  echo "v3-eval not found at $V3" >&2
  exit 1
fi

verbose="${V3_LAZINESS_VERBOSE:-0}"
pattern="${V3_LAZINESS_PATTERN:-}"

# (id, description, expression, expected) — one per fix.
# Format: each block is 4 array entries.
TESTS=(
  # ----------------------------------------------------------------
  # WC-31 root-cause: ConcatStrings eager-eval breaks rec-attr laziness
  # Commit: 3d9228726
  # Pre-fix symptom:
  #   `rec { x = "a"; y = x + "b"; }.y` would not cycle (string concat
  #   is fine), BUT in attr-value position, ConcatStrings was treated
  #   as "trivial" by isTrivialForLazy → not thunkified → eagerly
  #   forced rec siblings during attrset construction.
  # Test pattern: a rec attrset where one entry's ConcatStrings RHS
  # references a sibling that itself depends on the result attrset.
  # Without the fix this blackholes; tree-walker accepts.
  # ----------------------------------------------------------------
  WC-31-rec-concat
  "rec-attr ConcatStrings RHS references rec sibling"
  'let x = rec { release = "0.1"; suffix = "-pre"; full = release + suffix; }; in x.full'
  '"0.1-pre"'

  # WC-31-inherit-from-rec: the actual nixpkgs/lib/trivial.nix shape.
  # `inherit (lib.trivial) X Y` inside lib.trivial — references must
  # resolve lazily, not at attrset-build time.
  WC-31-inherit-from-rec
  "inherit (rec.X) Y Z; uses Y in attr value"
  'let
     lib = rec {
       trivial = let inherit (lib.trivial) ver suf; in
                 { ver = "1.0"; suf = "-pre"; full = ver + suf; };
     };
   in lib.trivial.full'
  '"1.0-pre"'

  # ----------------------------------------------------------------
  # WC-34 / OP_RETURN App-chase reverted: force-on-receive at OP_CALL
  # Commit: 62d2309eb (reverts 63a8214e8)
  # Pre-fix symptom:
  #   A thunk wrapping `(mapAttrs f attrs).key` returns a Tag::App
  #   (deferred call from mapAttrs lazy entries).  The Tag::App
  #   escaped to OP_CALL which threw "callee is not a closure".
  # Test pattern: lazy mapAttrs result selected and CALLED.
  # ----------------------------------------------------------------
  WC-34-mapattrs-call
  "OP_CALL forces fun on Tag::App from mapAttrs entries"
  'let m = builtins.mapAttrs (n: v: x: v + x) { a = 10; b = 20; }; in m.a 5'
  '15'

  # ----------------------------------------------------------------
  # WC-34 toString on non-primitive types
  # Commit: 48aff0763
  # Pre-fix symptom:
  #   `toString [1 2 3]` and `toString { outPath = "/x"; }` threw
  #   "v3 toString: cannot stringify this type".
  # ----------------------------------------------------------------
  WC-34-tostring-list
  "toString joins list elements with spaces"
  'builtins.toString [ "a" "b" "c" ]'
  '"a b c"'

  WC-34-tostring-attrs-outpath
  "toString attrset falls through to outPath"
  'builtins.toString { outPath = "/nix/store/x"; type = "derivation"; }'
  '"/nix/store/x"'

  WC-34-tostring-int
  "toString integer (regression coverage)"
  'builtins.toString 42'
  '"42"'

  WC-34-tostring-path
  "toString path stringifies, does not store-copy"
  'builtins.toString /tmp/v3-test-path'
  '"/tmp/v3-test-path"'

  # ----------------------------------------------------------------
  # WC-34 callClosure forces fun arg
  # Commit: 62f9ac514
  # Pre-fix symptom:
  #   __functor recursion left an unforced Thunk on the second
  #   callClosure call → "callClosure: not callable".
  # Test pattern: an attrset with __functor that itself returns
  # an attrset with __functor (chained functor application).
  # ----------------------------------------------------------------
  WC-34-functor-chain
  "callClosure forces fun across __functor chain"
  'let f = { __functor = self: x: { __functor = s: y: x + y; }; }; in (f 10) 5'
  '15'

  # ----------------------------------------------------------------
  # WC-35 force-on-receive at OP_ATTRS_SELECT
  # Commit: 62d2309eb
  # Pre-fix symptom:
  #   A Tag::App (e.g., from mapAttrs) reaching OP_ATTRS_SELECT
  #   threw "OP_ATTRS_SELECT: not an attrset".
  # ----------------------------------------------------------------
  WC-35-attrselect-on-app
  "OP_ATTRS_SELECT forces target on Tag::App"
  'let m = builtins.mapAttrs (n: v: { x = v; }) { a = 1; b = 2; }; in m.a.x'
  '1'

  WC-35-attrselect-dyn-on-app
  "OP_ATTRS_SELECT_DYN forces target on Tag::App"
  'let m = builtins.mapAttrs (n: v: { x = v; }) { a = 1; }; n = "x"; in m.a.${n}'
  '1'

  # ----------------------------------------------------------------
  # WC-35 per-primop lazyArgs — addErrorContext arg 1 lazy
  # Commit: 1f42622ef
  # Pre-fix symptom:
  #   `addErrorContext "msg" (rec_sibling_being_built)` would force
  #   the rec sibling at primop entry (auto-pre-force in OP_CALL's
  #   primop branch).  Now the lazy bit on arg 1 lets the value flow
  #   through unforced; the body just returns args[1].
  # Test pattern: addErrorContext wrapping a thunk-of-int returns the
  # int when forced.  Negative test: in a rec-sibling cycle, the
  # value should still cycle if actually forced (this is the documented
  # behaviour — "if you get an infinite recursion here").  We test
  # the positive case below.
  # ----------------------------------------------------------------
  WC-35-addErrorContext-lazy-passthrough
  "addErrorContext arg 1 passes through unforced (returns the value)"
  'builtins.addErrorContext "ctx" 42'
  '42'

  WC-35-addErrorContext-thunk-passthrough
  "addErrorContext on a thunked expression doesn't pre-force"
  'let x = builtins.addErrorContext "ctx" (1 + 2); in x'
  '3'

  # ----------------------------------------------------------------
  # WC-31 thunkifyRecAttrSelect (Option A) — sibling references in
  # closure body should be deferrable.
  # Commit: d644125a0
  # Pre-fix symptom:
  #   `rec { f = x: x + n; n = 10; }.f 5` — the closure captures `n`
  #   via the rec-attrset slot.  Pre-fix could capture a Black thunk
  #   at closure creation time; thunkified rec-references defer the
  #   AttrSelect to access time.
  # ----------------------------------------------------------------
  WC-31-rec-closure-uses-sibling
  "closure inside rec captures + uses rec sibling"
  'let r = rec { n = 10; f = x: x + n; }; in r.f 5'
  '15'

  WC-31-rec-closure-mutual
  "mutually-recursive rec siblings resolve through closure body"
  'let r = rec {
       a = x: if x <= 0 then 0 else b (x - 1) + 1;
       b = x: if x <= 0 then 0 else a (x - 1) + 2;
     }; in r.a 4'
  '6'

  # ----------------------------------------------------------------
  # WC-31 isTrivialForLazy split (forArg vs forValue)
  # Commit: 3d9228726
  # Pre-fix arg path symptom:
  #   `f (n + 1) (n - 1)` — fib hot path.  Args were eagerly passed
  #   (no thunk wrapping for ConcatStrings/arith); regressing this
  #   would re-introduce the fib slowdown.  This test catches
  #   correctness regression but not perf — bench-v3-vs-tw.sh is the
  #   perf gate.
  # ----------------------------------------------------------------
  WC-31-fib-arg-laziness
  "fib-style recursion with arith args (correctness)"
  'let fib = n: if n < 2 then n else fib (n - 1) + fib (n - 2); in fib 10'
  '55'

  # ----------------------------------------------------------------
  # WC-35 ROOT-CAUSE: zipAttrsWith must build LAZY entries
  # Commit: <next>
  # Pre-fix symptom:
  #   v3's primZipAttrsWith eagerly called `fn name list` for EVERY
  #   name at zipAttrsWith time.  In nixpkgs lib/modules.nix's
  #   pushedDownDefinitionsByName = zipAttrsWith (n: concatLists)
  #   (map (mod: mapAttrs (n: v: ... pushDownProperties v) mod.config)
  #   modules), this forced each module's per-name config attribute
  #   value (e.g., pkgs/top-level/config.nix's `warnings = optionals
  #   config.warnUndeclaredOptions ...`) — which transitively forces
  #   the rec config sibling currently being computed.  Cycle.
  # Tree-walker's nix-level `zipAttrsWith` definition uses genAttrs
  # which builds entries via App-style lazy thunks.  v3 now matches
  # via Tag::App entry construction in primZipAttrsWith.
  # ----------------------------------------------------------------
  WC-35-zipAttrs-lazy-entries
  "zipAttrsWith builds lazy entries; only queried name's f runs"
  'let
     # Set up zipAttrsWith on attrsets where SOME entries would force
     # an out-of-order rec sibling if eagerly evaluated.
     r = rec {
       sets = [ { a = 1; b = forced; } { a = 10; } ];
       forced = builtins.length [ 1 2 3 ];
       z = builtins.zipAttrsWith (n: vs: vs) sets;
     };
   in builtins.length r.z.a'  # only queries .a, never .b — should be 2
  '2'

  WC-35-zipAttrs-throw-not-on-queried-name
  "zipAttrsWith does NOT fire f for names we do not query"
  'let z = builtins.zipAttrsWith (n: vs: throw "fired for ${n}")
       [ { ok = 1; } { ok = 2; bad = 3; } ];
   in z ? ok'  # only checks presence, no entry forced
  'true'

  # ----------------------------------------------------------------
  # WC-35 follow-up: builtins.map must build LAZY entries
  # Commit: <next>
  # Pre-fix symptom:
  #   v3's primMap eagerly called fun on each element.  Same shape as
  #   the zipAttrsWith bug — would force list elements that might be
  #   rec siblings being constructed.
  # ----------------------------------------------------------------
  WC-35-map-lazy-entries
  "map builds App entries; un-accessed positions never fire"
  'let xs = builtins.map (x: throw "elem ${toString x}") [ 1 2 3 ];
   in builtins.length xs'  # length doesn't force entries
  '3'

  WC-35-map-only-queried-element-fired
  "map's accessed element fires; others stay lazy"
  'let xs = builtins.map (x: x * x) [ 1 (throw "boom") 3 ];
   in builtins.elemAt xs 0'
  '1'

  # OP_STR_CONCAT must force Tag::App parts (e.g. from mapped lists).
  WC-35-strconcat-forces-app-parts
  "string interpolation of map result forces Tag::App element"
  'let xs = builtins.map (x: x + "!") [ "hi" "yo" ];
   in builtins.head xs'
  '"hi!"'

  # ----------------------------------------------------------------
  # WC-36: genList must build LAZY entries (matches tree-walker)
  # Commit: 117a31628
  # Pre-fix symptom:
  #   Eager genList forced (gen idx) for every i.  In nixpkgs
  #   `lib.lists.imap1 = f: list: genList (n: f (n+1) (elemAt list n))
  #   (length list)`, eager genList meant every list element was
  #   built up-front — surfaced bootstrap-stage closures into
  #   contexts expecting forced values.
  # ----------------------------------------------------------------
  WC-36-genList-lazy-entries
  "genList builds App entries; un-accessed positions never fire"
  'let xs = builtins.genList (i: throw "elem ${toString i}") 100;
   in builtins.length xs'
  '100'

  WC-36-genList-only-queried-element-fired
  "genList accessed element fires; others stay lazy"
  'let xs = builtins.genList (i: i * 10) 5;
   in (builtins.elemAt xs 0) + (builtins.elemAt xs 4)'
  '40'

  # ----------------------------------------------------------------
  # WC-36 dyn-attr ops force `name` arg
  # Commit: 888b56928
  # Pre-fix symptom:
  #   `attrs.${expr}` where expr resolves to a Tag::App / Tag::Thunk
  #   tripped "type error" or made HasAttr return false (which
  #   caused nixpkgs's `cpuTypes.${cpu} or throw "Unknown CPU"` to
  #   throw).
  # ----------------------------------------------------------------
  WC-36-dyn-select-with-app-name
  "AttrSelect_DYN forces lazy name (Tag::App from map)"
  'let names = builtins.map (x: x) [ "aarch64" ];
       attrs = { aarch64 = "OK"; };
   in attrs.${builtins.head names}'
  '"OK"'

  WC-36-dyn-has-with-thunk-name
  "AttrSelect_HAS_DYN forces lazy name (Tag::Thunk)"
  'let n = builtins.head [ "x" ]; attrs = { x = 1; }; in attrs ? ${n}'
  'true'

  # ----------------------------------------------------------------
  # WC-36 OP_LIST_CONCAT forces lazy operands
  # ----------------------------------------------------------------
  WC-36-list-concat-on-app
  "++ forces Tag::App lhs/rhs (e.g. from map)"
  'let xs = builtins.map (x: x) [ 1 2 ];
       ys = builtins.map (x: x) [ 3 4 ];
   in builtins.length (xs ++ ys)'
  '4'

  WC-36-list-concat-on-app-elemAt
  "++ result indexed forces individual entry"
  'let xs = builtins.map (x: x * 10) [ 1 2 ];
       ys = builtins.map (x: x * 100) [ 3 4 ];
       cat = xs ++ ys;
   in (builtins.elemAt cat 2) + (builtins.elemAt cat 0)'  # 300 + 10
  '310'

  # ----------------------------------------------------------------
  # WC-36 OP_ATTRS_UPDATE forces lazy operands
  # ----------------------------------------------------------------
  WC-36-attrs-update-on-app
  "// forces Tag::App lhs/rhs (e.g. from mapAttrs)"
  'let a = builtins.mapAttrs (n: v: v) { x = 1; };
       b = builtins.mapAttrs (n: v: v) { y = 2; };
   in (a // b).y'
  '2'

  # ----------------------------------------------------------------
  # WC-36 primConcatMap forces fn result
  # ----------------------------------------------------------------
  WC-36-concatMap-lazy-result
  "concatMap forces fn's return (may be lazy mapAttrs/map result)"
  'let xs = builtins.concatMap (x: builtins.map (y: y) [ x x ]) [ 1 2 ];
   in builtins.length xs'
  '4'

  # ----------------------------------------------------------------
  # WC-37 working cases (pin the patterns that DO work, since the
  # full nixpkgs case is still blocked by a deeper issue)
  # ----------------------------------------------------------------
  WC-37-formal-default-concat
  "formal preHook with default '' + string interpolation works"
  'let f = { name ? "default", preHook ? "", overrides ? (s: s: {}) }:
     preHook + " from " + name;
   in f { name = "test"; preHook = "hello"; }'
  '"hello from test"'

  WC-37-stage-foldl-chain
  "stage-list folded via foldl' produces final stage's result"
  'let
     stages = [
       (prevStage: { v = "1"; })
       (prevStage: { v = "2(${prevStage.v})"; })
       (prevStage: { v = "3(${prevStage.v})"; })
     ];
     run = builtins.foldl'"'"' (acc: stageFn: stageFn acc) { v = "init"; } stages;
   in run.v'
  '"3(2(1))"'

  WC-37-inherit-prevStage-pattern
  "(prevStage: { inherit (prevStage) X Y; }) — bytecode pattern WC-37 traced to"
  'let
     inheritor = prevStage: {
       inherit (prevStage) a b c;
     };
     stage = { a = 1; b = 2; c = 3; };
   in (inheritor stage).b'
  '2'

  # ----------------------------------------------------------------
  # WC-31 negative test: a TRUE infinite recursion still errors.
  # We must not have made the evaluator too lenient — `let x = x; in x`
  # should still throw, not loop forever.
  # ----------------------------------------------------------------
  WC-31-true-self-cycle-still-errors
  "let x = x; in x must still raise infinite-recursion"
  'let x = x; in x'
  '__ERROR__'

  WC-31-mutual-self-cycle-errors
  "let x = y; y = x; in x must still raise"
  'let x = y; y = x; in x'
  '__ERROR__'

  # ----------------------------------------------------------------
  # WC-37 root-cause: clearBlackMarksOnException leaves ghost frames
  # Commit: TBD
  # Pre-fix symptom: when forceValue's inner dispatchLoop throws and
  # is caught by tryEval, the inner pushed thunk frames remain on
  # vm.frames as "ghost frames".  A subsequent OP_RETURN in the
  # outer dispatchLoop pops the ghost frame, finds CFF_THUNK_RETURN
  # set, and stores its retVal into the ghost thunk's evaluated slot
  # — corrupting an unrelated thunk.  The corrupt thunk leaks into
  # OP_STR_CONCAT (got Closure where String was expected).
  # ----------------------------------------------------------------
  WC-37-tryeval-ghost-frame-corrupt
  "tryEval failure-path doesn't corrupt later thunks"
  'let
     foo = throw "nope";
     bar = "ok-${"value"}-end";
   in builtins.seq (builtins.tryEval foo).success bar'
  '"ok-value-end"'

  WC-37-double-tryeval-then-string
  "two tryEval failures then a +chain that must produce a string"
  'let
     foo = throw "boom";
     parts = "a" + "b" + "c";
   in
   builtins.seq (builtins.tryEval foo).success
     (builtins.seq (builtins.tryEval foo).success parts)'
  '"abc"'

  # ----------------------------------------------------------------
  # WC-38 positive: lib.fix-style fixed-point with `with self;` body
  # Pre-fix symptom (from nixpkgs eval):
  #   In `let x = f x; in x` where f returns `with self; { foo = bar; bar = "ok"; }`,
  #   sub-thunks captured `self` (= x).  v3's OP_RETURN-chain push at
  #   x's RETURN eagerly drives the chain through inner thunks while
  #   x is still Blackhole.  If sub-thunks fire during this deep
  #   eval AND look up names via `with self;`, they hit Blackhole.
  # This MINIMAL pattern works in v3 today because the body doesn't
  # trigger a deep stage chain.  We pin it to detect regressions.
  # ----------------------------------------------------------------
  WC-38-libfix-with-self
  "lib.fix-style: let x = f x; in x with `with self;` in body"
  'let
     fix = f: let x = f x; in x;
     pkgs = fix (self: with self; { foo = bar; bar = "ok"; });
   in pkgs.foo'
  '"ok"'

  WC-38-nested-libfix-overlay
  "fix + overlay: ensures captured-with self resolves through chain"
  'let
     fix = f: let x = f x; in x;
     extends = overlay: f: self: let prev = f self; in prev // overlay self prev;
     base = self: { a = 1; };
     ext = self: super: { b = self.a + 1; };
     pkgs = fix (extends ext base);
   in pkgs.b'
  '2'
)

pass=0
fail=0
total=0
failed_cases=()

i=0
while [[ $i -lt ${#TESTS[@]} ]]; do
  id="${TESTS[$i]}"
  desc="${TESTS[$((i+1))]}"
  expr="${TESTS[$((i+2))]}"
  expected="${TESTS[$((i+3))]}"
  i=$((i+4))

  # Pattern filter (set V3_LAZINESS_PATTERN to e.g. "WC-31" to limit).
  if [[ -n "$pattern" && "$id" != *"$pattern"* ]]; then
    continue
  fi

  total=$((total+1))

  # Run v3-eval.
  if [[ "$expected" == "__ERROR__" ]]; then
    # Negative test: we expect a runtime error.
    if v3_out=$("$V3" --expr "$expr" 2>&1) && [[ -n "$v3_out" ]]; then
      # Got non-error output where we expected an error.
      fail=$((fail+1))
      failed_cases+=("$id (no error: got '$v3_out')")
      [[ "$verbose" == "1" ]] && echo "FAIL  $id — expected error, got: $v3_out"
    else
      pass=$((pass+1))
      [[ "$verbose" == "1" ]] && echo "OK    $id ($desc)"
    fi
  else
    # Positive test: must match expected output.
    if v3_out=$("$V3" --expr "$expr" 2>&1); then
      # Strip leading/trailing whitespace.
      v3_out_trim=$(echo -n "$v3_out" | sed 's/^[[:space:]]*//;s/[[:space:]]*$//')
      if [[ "$v3_out_trim" == "$expected" ]]; then
        pass=$((pass+1))
        [[ "$verbose" == "1" ]] && echo "OK    $id ($desc)"
      else
        fail=$((fail+1))
        failed_cases+=("$id: v3='$v3_out_trim' expected='$expected'")
        [[ "$verbose" == "1" ]] && echo "FAIL  $id — v3='$v3_out_trim' expected='$expected'"
      fi
    else
      fail=$((fail+1))
      failed_cases+=("$id (v3 errored: $v3_out)")
      [[ "$verbose" == "1" ]] && echo "ERR   $id — v3 errored: $v3_out"
    fi
  fi
done

echo ""
echo "=== WC-31/34/35 lazy-semantics test results ==="
echo "  total:    $total"
echo "  passing:  $pass"
echo "  failing:  $fail"
if [[ ${#failed_cases[@]} -gt 0 ]]; then
  echo ""
  echo "Failed cases:"
  for c in "${failed_cases[@]}"; do
    echo "  $c"
  done
fi
[[ $fail -eq 0 ]]
