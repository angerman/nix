# Native v3-AST → IR lowering — phased execution plan (Stage 2 proper)

**Status:** the v3-native parser is functionally complete — it parses
547/547 nixpkgs files byte-equal to TW and **evaluates 143/143 lang
tests** via `NIX_V3_NATIVE_PARSER=1` (commits `46eab3b3f` +
`b65104815`).  Today's eval path is the **bridge**: v3 AST →
`nix::Expr` (`cli/v3-to-nixexpr.hh`) → TW `bindVars` → `lower.cc` → IR →
VM.  This plan retires the bridge by lowering the v3 AST → IR directly.

## Why this is multi-phase (measured, not speculated)

* `lower.cc` is **3,446 LoC** and resolves variables by `(level, displ)`
  — it requires a `bindVars`-style resolution pass to have run.
* It embeds hard-won correctness developed over ~1380 commits:
  - the `Fix` / `Extends` / `ComposeExtensions` intrinsic recognition
    (#495) that makes `lib.fix`/nixpkgs overlays evaluate,
  - rec-attrset slot semantics (`recAttrsVar` + `AttrSelect`),
  - the lexical `with`-chain (#530), `inheritFrom` displ + caching,
  - upvalue / freeVars closure capture.
* SHIP gate (PARSER_PROJECT_PLAN §6): byte-equal `drvPath` on
  hello / firefox / python3 / HNE / M5 (large nixpkgs evals).

So a correct native lowering = native `bindVars` + a 3,446-LoC-equivalent
lowerer + the SHIP-gate campaign.  Rushing eval-critical lowering risks
silent wrong-store-path bugs ([[680-silent-permissiveness]],
[[682-tofile-context]]) — the most serious bug class.  Hence: phased,
each phase eval-validated.

## Invariant for every phase (no carcasses, no mistakes)

* **Whole-program bridge fallback.**  `canLowerV3(ast)` returns true iff
  the WHOLE tree uses only natively-implemented constructs; otherwise the
  program takes the proven bridge path.  So every committed phase is
  COMPLETE (all programs evaluate) and VALIDATED (143/143 lang +
  byte-equal drvPath on the supported subset).
* **Eval-parity is the gate** (not IR-byte-equality — the native lowerer
  legitimately emits different VarId numbering / binding order than
  `lower.cc`; only the eval RESULT must match).
* Each phase grows `canLowerV3`'s accepted set; coverage rises monotonically
  until it accepts everything → bridge + `cli/v3-to-nixexpr.hh` retired.

**No separate native bindVars.**  The native lowerer resolves variables
BY NAME during the descent (its own name→VarId scope stack, like
lower.cc's `byName`), so it needs neither TW's bindVars nor pre-computed
`level`/`displ` — and avoids the displ-matching risk entirely.  Free
names resolve to: a lexical VarId, else a `with`-lookup (if any enclosing
`with`), else a primop ref by name, else an undefined-variable error
(matching TW).

## Phases

1. **Lowerer skeleton + atoms** (`parser/lower_v3.{hh,cc}`).
   `LowererV3` mirroring lower.cc's builder surface (Module/Block/Func,
   `addBinding`/`setReturn`, name-keyed scope stack, primop-ref emission).
   Implement: Int/Float/String/Path/Var(lexical+primop)/Pos/Call/BinOp/
   If/OpNot/ConcatStrings/List/Assert.  `canLowerV3(ast)` accepts trees
   using only these.  Validated: the lang subset using only these evals
   natively (rest bridges) → 143/143.

2. **Lambdas + Let + closures** — lowerLambda / lowerLet /
   call-with-closures (freeVars/upvalue capture).  Grow `canLowerV3`.

3. **Attrsets + Select + HasAttr + With** — rec slots (recAttrsVar +
   AttrSelect), the lexical with-chain (#530).  The intricate scope work.

4. **inheritFrom + intrinsic recognition** (#495 Fix/Extends/Compose) —
   the nixpkgs-critical piece; validate on lib.fix + overlay patterns.

5. **SHIP gate + flip.**  drvPath byte-equality on hello/firefox/
   python3/HNE/M5; 1-week soak under the flag; then `canLowerV3` accepts
   everything, default-flip, delete the bridge + lower.cc's nix::Expr
   path + the v3 AST→nix::Expr converter.

## Reuse vs duplicate (decision)

DUPLICATE (parallel `lower_v3.cc` reading v3 AST), not trait-genericize
`lower.cc`: genericizing touches all 3,446 lines and risks regressing
the production path; a parallel lowerer is isolated (the bridge path
stays the validated fallback throughout).  The duplicated logic is
deleted-by-replacement at phase 6 (net LoC neutral once the bridge +
nix::Expr path go).

Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
Input Output Group.  SPDX-License-Identifier: Apache-2.0
