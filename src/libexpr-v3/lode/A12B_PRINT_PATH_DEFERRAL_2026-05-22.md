# A12b Print-path Conversion — Deferred (2026-05-22)

## Status

**DEFERRED with rationale.**  `#761` (iterative conversion of
`printNixValue`, `printNixValueRich`, and `toJsonValue`) is
formally closed in this session.  The decision is not "we won't
do it" — it is "we won't do it AT THIS TIME, until ONE OF THESE
TRIGGERS fires."

## Background

The A12b sweep (`012c0e38f` + `b2ed05a19`,
[`project_a12b_sweep_2026-05-22.md`]) converted three force-chain
helpers from C-recursive to explicit work-stacks:

  * `vm.cc::valueEqual`     — used by `==` operator.
  * `primops.cc::valueEqual` — used by `builtins.filter`/`elem`/
    `all`/`primConcatMap`.
  * `print.cc::forceDeep`   — walks transitive container graph
    forcing every value.

These conversions close v3's C-stack overflow hazard on deeply-
nested force chains (stdenv assertion chains, deepSeq on big
trees, `==` on deep structures, RCA_FAMILY_DIVERGENCE_A7
territory).

The fourth candidate site — recursive printing in `printNixValue`,
`printNixValueRich`, and `toJsonValue` — was deferred for the
reasons below.

## Why deferred

### 1. The hazard is symmetric with TW

TW's `print.cc` `printAttrs` / `printList` and
`value-to-json.cc::printValueAsJSON` recurse through container
children with the same shape v3's does.  TW's default
`print-options.hh` has

  ```c++
  size_t maxDepth = std::numeric_limits<size_t>::max();
  ```

— unlimited.  There is no user-controllable depth cap on either
evaluator's print path.

Empirically: any Nix expression that would overflow v3's C-stack
on print would also overflow TW's.  The user-facing experience is
identical (SIGSEGV / "Stack overflow" on the same input).  Closing
v3's exposure unilaterally is asymmetric work for a shared
user-experience problem.

A second, more subtle concern is **the optics of asymmetric
robustness**.  A v3-only success on input that TW crashes on would
be reported by default as "v3 returns wrong output" (because TW is
the reference oracle), not as "v3 is more robust."  Without
spec'd semantics for a depth cap, an asymmetric fix flips the
parity story.

### 2. The A12b motivation was the force chain

`RCA_FAMILY_DIVERGENCE_A7` and follow-ons identified deep
recursion through `forceValue` as the production hazard — stdenv
assertion chains, nested `mkOption` defaults, lib.fix-style
fixpoints.  Users **force** deep values constantly during build
evaluation (every drvPath query traverses a deep transitive
input-tree).  Users almost never **print** deep values:

  * `nix eval pkg.drvPath` returns one string.
  * `nix eval --json pkg.meta` returns a small attrset.
  * The repl normally prints with depth-limited `:show`.

The closed three sites cover the practical hazard.  The fourth
is theoretical.

### 3. Lang-test golden byte-identical risk

`printNixValue` output bytes drive dozens of lang-test goldens
(under `tests/functional/lang/`).  A subtle conversion bug —
trailing space before `]`, attr-key sort order, separator
emission between nested closures — would silently break tests in
a way that diffs as "wrong byte at position N" rather than "wrong
shape."  Recovering from such a regression is painful: bisecting
which conversion site introduced the byte drift, then re-running
the entire lang-test suite per attempt.

The conversion itself is well-trodden (the `forceDeep` sweep
showed the pattern).  The risk is **implementation**: emitting
events from the iterative loop in exactly the order the recursive
version did, with the same string padding and separator state.
Doable with care, but expensive to validate.

### 4. Low practical user demand

We have not received a report of deep-print SIGSEGV from any v3
workload to date.  Workloads sampled in the past three weeks:

  * 143 nixpkgs lang tests        — all PASS.
  * 64 nixpkgs drvPaths           — 63 byte-identical (#759 sweep).
  * 15 cardano-node callFlake queries — 15 byte-identical (#758).
  * 20 derivation-parity scenarios   — 20 PASS.
  * cardano-node M5 end-to-end       — byte-identical to TW.
  * 119 v3-direct workloads          — byte-identical.

None of these hit print-path recursion limits.  The risk model is
"some future workload might."

## What would un-defer this

Any of the following triggers should promote #761 from deferred
to active:

  1. **A user reports a deep-print SIGSEGV on v3 that does NOT
     also reproduce on TW.**  Asymmetric exposure is the smoking
     gun — the symmetry argument breaks and the conversion
     becomes a real defect-fix.

  2. **A Stage 5 (hidden classes) or later phase introduces a v3
     `maxDepth` setting for prints.**  Once v3 has a controllable
     depth, converting becomes part of the depth-cap
     implementation — the iterative loop can emit `"..."` at
     depth N rather than recursing further.  The two pieces of
     work line up.

  3. **A nixpkgs-eval workload trips a TW print SIGSEGV that
     becomes a public bug + TW gets a `maxDepth` fix.**  v3
     should then match TW's depth cap; the conversion is the
     v3-side implementation.

## What this kills (Rule 0)

Falsifies "every A12b force-chain conversion implies the
symmetric print-chain conversion must also land in the same
closure."  The print chain is a **separate** hazard with
**separate** triggers; bundling them artificially conflates the
practical force-chain risk with the theoretical print-chain risk.

The deferral is itself the kill: a commit body that says "we
considered this, we measured the symmetry argument, we explicitly
chose not to do it now."  The investigation closes — it does not
sit half-open with `// TODO: convert print path` strewn across
the source.

## Operating rule

When a conversion pass touches multiple call sites with different
practical exposure profiles, **close each site separately on its
own merits**.  Bundling "we should convert all four" forces
conversion of theoretical sites just because actual sites are
being addressed.

Deferred-with-rationale is the correct exit when:

  * The site has no observed defect.
  * The asymmetric work argument cuts against doing it now.
  * A measurable trigger criterion exists for future re-opening.

This is the Rule 0 falsification rule's "rename to a fresh
top-level investigation" branch.  The conversion can be opened
later as `#761b print-path A12b (re-opened: trigger X fired)` —
the deferral document gives that future session everything it
needs.

## Cross-references

  * [`lode/RCA_FAMILY_DIVERGENCE_A7_2026-05-11.md`] — canonical A7
    write-up identifying the C-stack force-chain class.
  * `ddb8faef0` (Round1 #7) — GC-stale-pointer fix in forceDeep
    (predecessor to the A12b print sweep).
  * `012c0e38f` + `b2ed05a19` — A12b sweep landing for the three
    closed sites.
  * [`memory/project_a12b_sweep_2026-05-22.md`] — A12b sweep
    memory note (this commit updates it to mark print-path
    closed-deferred).
  * `b28030bdc` (#762) — opt_strictness operating rule on
    "measure first."  Applies here too: deferred is the right
    answer until a measurement shows otherwise.

Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
Input Output Group.  SPDX-License-Identifier: Apache-2.0
