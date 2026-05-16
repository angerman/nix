# `with` — analysis and prior art (2026-05-07)

`with E; body` is the feature JavaScript banned in strict mode for the same
reasons it makes Nix's evaluator hard.  This memo surveys the comparable
features in other languages, why most of them are tractable, and what
Nix can credibly do given that it can't take JavaScript's escape hatch.

The bottom line is up front: there is no clean efficiency story for
`with` in the abstract.  The cleanest path is shape-based PIC at the
use site, treating `with`-lookup as attrset-select against a stack of
shapes to try in turn.  This converges with the shape work that the
recent reviews (`REVIEW_2026-05-06.md` §S1, `OPTIMIZER_REPORT_2026-05-07.md`
§4.2 #1) keep flagging for unrelated reasons.

---

## 1. Why `with` is uniquely costly

`with E;` injects an arbitrary runtime value's keys into local scope.
Three properties combine to defeat the standard optimisation toolkit:

1. **Arbitrary E.**  Until E is forced, the name set is unknown.
2. **Untyped.**  No type signature closes the name set ahead of time.
3. **Lazy.**  E may not be forced until the first lookup actually fires
   (and that lookup may itself be inside a thunk captured under the
   `with`).

Nix gets one structural break that JavaScript doesn't: lexical names
always shadow `with`-injected ones, and lexical scope is fully resolved
at parse time (`(level, displ)`).  So the compiler statically knows
which references are `OP_GET_LOCAL` / `OP_GET_UPVALUE` and which fall
through to `OP_WITH_LOOKUP`.  Everything below builds on that
distinction.

---

## 2. Comparable features in other languages

Three families.  Two are tractable.  Nix landed on the third.

### 2.1 Static scope injection — cheap

The injected namespace's key set is known at compile time, from a record
type, a module signature, or a typed receiver.  The compiler resolves
`foo` to `R.foo` / `M.foo` at parse time.  Zero runtime cost; no special
VM machinery.

| Language | Construct | Resolution |
|---|---|---|
| Pascal | `with R do ...` | record type → static |
| Visual Basic | `With ... End With` | record type → static |
| OCaml | `let open M in ...`, `M.(...)` | module sig → static |
| Kotlin / Scala | `with(obj) { ... }` (typed receiver) | static receiver type |
| Haskell | `RecordWildCards` (`let X{..} = r in ...`) | record type → static |

These work because *types or modules close the name set before
evaluation runs.*  Nix has neither.

### 2.2 Principled dynamic scope — solvable

The compiler doesn't know the *value*, but it knows *which names are
dynamic* — they're declared up-front.  The set of dynamic names is
closed at parse time even though their values aren't.

- **Common Lisp** `special` variables (`defvar`, `defparameter`).
- **Scheme** parameter objects (SRFI-39).
- **Emacs Lisp** dynamic scope (the default, until `lexical-binding: t`).

Implementations use **shallow binding**: each special symbol has a
thread-local value cell.  Binding pushes the old value onto a stack;
unbinding pops it.  Lookup is O(1) — one indirection through the
symbol's cell.  This is genuinely fast; it's why Emacs Lisp's
performance survives despite being dynamically scoped.

The trick that makes this efficient — *closed set of dynamic names
known at parse time* — is the one Nix can't have.  `with E;` makes any
name potentially dynamic, depending on a value that hasn't been computed
yet.

### 2.3 Dynamic injection of an arbitrary value — the worst case

The injected namespace is an arbitrary runtime value; its key set isn't
known to the compiler; mutating the keys between executions is legal.
This is the worst case for static analysis.

| Language | Construct | Status |
|---|---|---|
| JavaScript | `with(obj) { ... }` | Banned in strict mode (ES5) |
| Lua | `setfenv` / `_ENV` | Tightened in 5.2+ via static `_ENV` rewrite |
| Ruby | `instance_eval { ... }` | Slow path; no IC caching |
| Nix | `with E; body` | Load-bearing in nixpkgs `lib`/`callPackage` |

JavaScript's resolution was to *ban it.*  ES5 strict mode prohibits
`with` entirely.  V8 routes the rare legacy `with`-block through a
generic-lookup path with no inline caches — the existence of a `with`
block in a function disables most optimisations *for that function*.
The community accepted this because `const { x, y, z } = obj`
(destructuring) covers 95 % of legitimate uses.

Lua 5.2+ kept `_ENV` but lets static analysis treat every free
identifier in a function as `_ENV.name`.  Most code never reassigns
`_ENV`, so the resolution path is monomorphic per function and ICs
work.  This is roughly what Nix already gets via parse-time
partitioning (§1) — the difference is that Lua's `_ENV` is
single-level, while Nix's `with` stacks.

---

## 3. Why Nix can't take JavaScript's escape hatch

Nixpkgs is built on `with self;` and `with pkgs;`.  The `lib/` calling
convention treats the rec-attrset as the namespace.  Retiring `with`
is not a language-design conversation — it's a "rewrite nixpkgs"
conversation.

So the strategy has to be *make `with` cheap*, not *deprecate it*.

The principled path (CL-style shallow binding) requires a closed set
of dynamic names; we don't have it.  The static path (Pascal / OCaml)
requires types or modules; we don't have those either.  Nix is in the
worst spot of the three families *and* can't ban its way out.  This
forces the engineering response: optimise per-call-site rather than
globally.

---

## 4. Concrete moves, ordered by leverage

### 4.1 Parse-time partition (already exploited)

Lexical references compile to `OP_GET_LOCAL` / `OP_GET_UPVALUE`; only
genuinely-free identifiers fall through to `OP_WITH_LOOKUP`.  v3
already does this — see `lower.cc` `emitVarRef` and the resolution
table built during `computeFreeVars` (`ir.cc:287–456`).

The cost surface for `with` is therefore exactly the set of lookups
that the parser couldn't statically resolve.  In typical nixpkgs
code that's a small minority of variable references, but they tend
to be *hot* (`callPackage`, `lib`, `pkgs.X`).

### 4.2 PIC on `OP_WITH_LOOKUP` — the cheapest unlanded win

Mirror the existing `OP_ATTRS_SELECT` 4-way polymorphic IC
(`bytecode.hh:297–308`, `vm.cc:2761–3080`) for `OP_WITH_LOOKUP`.
Each lookup site keeps a small cache of `(Bindings*, slot)` it has
seen; repeat hits skip the binary search and the with-stack walk.

Hit-rate prediction: high, for the same reason as the attribute-
select IC — call sites are typically monomorphic or low-polymorphic
on the with-attrset shape across calls.

Estimate: ~200 LOC, no semantic change.  Pairs trivially with the
existing IC framework.

### 4.3 Shapes (V8 hidden classes) — the structural win

Once attrsets carry a `Shape *` (an interned sorted-symbol vector),
`with`-lookup degenerates to "given `Shape *`, what's the slot for
this symbol?" — a hash-table lookup against the shape, not against
the attrset's content.  Polymorphic over the typical 1–4 shapes a
given site sees in practice.

This is the single biggest unlanded structural win the reviews keep
flagging (`REVIEW_2026-05-06.md` §S1, M2; `OPTIMIZER_REPORT_2026-05-07.md`
§4.2 #1).  `with` is one of three independent justifications for it;
the others are #455-class env-shape pathology and selector-thunk
specialisation.

References: Chambers, Ungar, Lee, *An efficient implementation of SELF*
(OOPSLA 1989); Hölzle, Chambers, Ungar, *Optimizing dynamically-typed
object-oriented languages with polymorphic inline caches* (ECOOP 1991).

### 4.4 Snapshot the with-stack — already landed, can be cheaper

v3 already snapshots the with-stack at `MAKE_CLOSURE`-time
(`Closure::capturedWiths`, `closure.hh:72`).  The body-level dispatcher
re-pushes them so `OP_WITH_LOOKUP` inside the body finds them.  This
is correct.

The cost is *carrying* the snapshot: every closure under a `with`
holds a `ListVec *` of full attrsets.  With shapes (§4.3), the
snapshot becomes "a list of `Shape *` plus pointers" — much cheaper
than the current full-attrset capture.

### 4.5 `Tag::Slot` for `with E;` over rec-attrset entries — landed

WC-38 forced the fix; the fix is also the optimisation.  When E
resolves to a rec-attrset entry, capture a stable `Value *` pointer
into `Bindings::entries[i].value` rather than snapshotting the
entry's value.  Mutating the entry in place propagates to every
observer.

This is the SECD DUM/RAP move (Landin 1964) and morally the same as
GHC's `stg_IND` indirection chain — the consumer derefs at force
time, the producer mutates in place.  See
`OP_REC_BINDING_SLOT_REF` (`bytecode.hh:182`) and the WC-38 memo.

The remaining gap is when E resolves to a *lambda parameter* slot
rather than a rec-attrset entry — `Phase 5` in
`OPTIMIZATION_PLAN.md:41–58`, partially landed.

---

## 5. Cross-references

- **Bug class.**  `with`-stack identity is one face of the recurring
  "rec / let / with / formals identity is unstable across capture
  boundaries" bug.  See `project_v1_v2_v3_comparison`,
  `project_wc38_with_blackhole`, `project_493_step3d_with_stack`,
  `project_498_always_thunkify_regression`.  Shapes (§4.3) is the
  single architectural fix every review converges on.

- **Performance.**  v3's with-stack handling already pays an
  acceptable cost for snapshot + walk; the headline regressions
  attributed to "`with`" in earlier benchmarks turned out to be
  identity bugs in disguise (e.g.  WC-38, #455).  The PIC (§4.2)
  is the next concrete perf win.

- **Lowering.**  The `with` on direct value-stack slots gap
  (`lower.cc:1683–1686`, REVIEW_2026-05-06 §1.3) is downstream of
  shapes — once attrset entries are slot-addressable by shape, the
  lambda-param case unifies with the rec-attrset case.

---

## 6. Verdict

`with` is a bad feature, and the language designers who could afford
to retire it (JavaScript ES5, Visual Basic .NET) did so.  Nix can't.

The principled dynamic-scope languages (Common Lisp, Scheme) make it
efficient by keeping the name set closed; Nix forfeits that.  The
static analogues (Pascal, OCaml, Kotlin) keep the value type closed;
Nix forfeits that too.  What's left is the V8 playbook: don't make
`with` cheap in general; make it cheap *at each particular use site*
by caching the shape transitions it sees.

That prescription is the same one that comes out of every recent
review for unrelated reasons — `with` just makes the case stronger.

---

Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.
SPDX-License-Identifier: Apache-2.0
