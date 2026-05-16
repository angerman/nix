# Attribute-set semantics audit: TW vs v3 (bytecode)

Scope: `ExprAttrs`, `OP_ATTRS_*`, `OP_SELECT*`, `OP_HAS_ATTR*`, `OP_UPDATE*`, dynamic attrs, inherit-from, `__overrides`, `__functor`, attrName ordering.

Reference files:

- TW: `/Users/angerman/Projects/iohk/nix/src/libexpr/eval.cc` (`ExprAttrs::eval` at L1525, `ExprSelect::eval` at L1690, `ExprOpHasAttr::eval` at L1770, `ExprOpUpdate::eval`/`evalForUpdate` at L2339/L2420/L2470, `buildInheritFromEnv` at L1513).
- TW header: `/Users/angerman/Projects/iohk/nix/src/libexpr/include/nix/expr/nixexpr.hh` (`AttrDef::Kind` at L428, `ExprAttrs` at L421, `maybeThunk` overrides at L220/233/259/275/313/522).
- v3 lower: `/Users/angerman/Projects/iohk/nix/src/libexpr-v3/lower.cc` (`lowerAttrs` at L2113, `lowerLetRecCapture` at L2394, `pushInheritFromCache` at L1686, `lowerInheritFrom` at L1645, `lowerSelect`/`emitSelectChain` at L2702/L2731, `lowerHasAttr` at L2779, `isTrivialForLazy` at L1514, `thunkifyForAttr` at L1556).
- v3 emit: `/Users/angerman/Projects/iohk/nix/src/libexpr-v3/emit.cc` (`emitOne(AttrSet)` at L702, `emitOne(AttrSetSetInheritFrom)` at L838, `emitOne(AttrSetDyn)` at L856, `emitOne(Update)` at L932, `emitOne(LetRec)` at L949).
- v3 vm: `/Users/angerman/Projects/iohk/nix/src/libexpr-v3/vm.cc` (`OP_ATTRS_INIT` at L4875, `OP_ATTRS_INIT_DYN` at L4936, `OP_ATTRS_REC_INIT` at L5003, `OP_ATTRS_LET_REC_INIT` at L5076, `OP_ATTRS_REC_INIT_TAIL` at L5119, `OP_APPLY_OVERRIDES` at L5158, `OP_ATTRS_SELECT` at L5210, `OP_ATTRS_SELECT_DYN` at L5753, `OP_ATTRS_HAS` at L5786, `OP_ATTRS_HAS_DYN` at L5813, `OP_ATTRS_UPDATE` at L5846, `OP_ATTRS_UPDATE_TAIL` at L5887, `OP_ATTRS_REC_SET` at L6947, `publishToNearestBlackThunkFrame` at L1316, `publishToAllThunkFrames` at L1527, `mergeBindings` at L666, `lookupInPartialChain` at L202).

---

## Summary

- **Three confirmed divergences** with observable semantics:
  1. `(throw "lhs") // (throw "rhs")` evaluates LHS first in v3 but RHS first in TW (eval-order in `//`).
  2. v3 emits `OP_ATTRS_REC_INIT` for **every non-empty attrset** (rec or non-rec), which publishes the partial Bindings to the surrounding Black thunk's side-table — TW has no analog and never lets an outer thunk observe a sub-attrset's mid-construction shape. This is the architecture behind the `#455` / `#496` / `#498` / `#546` / `#558` bug cluster; it can return entries from the partial chain that are `mkNull` placeholders or that belong to a sibling sub-expression.
  3. Inherit-from from-expressions are **unconditionally thunkified** in TW (`from->maybeThunk(state, up)` at eval.cc:1520) but only **heuristically** thunkified in v3 (`pushInheritFromCache` at lower.cc:1686, gated behind `s_thunkifyAll` which is only set when `NIX_V3_INHERIT_FROM_THUNK_ALL` or `NIX_V3_STG` env vars are set — **opt-in**, distinct from the runtime `NIX_V3_NO_STG` opt-out gate).
- **Multiple subtle bridge/peek behaviors** in v3 (`tryBridgeAttrLookup`, `lookupInPartialChain` largest-layer-wins, registry-wide miss recovery) silently return values that TW would throw `BlackholeError` on. Most matter only with the bridge thunk type (TW values bridged into v3); the partial-chain peek is reachable in pure-v3 mode.
- **Most error messages diverge** ("v3 OP_ATTRS_SELECT: attribute not found" vs TW's "attribute '%1%' missing", with TW providing suggestions, source position, and frame info). Tests that match against error strings will break.

---

## Confirmed divergences

### D1. `//` evaluates operands in opposite order

- **TW**: `ExprOpUpdate::eval` (eval.cc:2420) calls `evalForUpdate` (eval.cc:2470), which evaluates `e2` first (line 2487: `e2->evalForUpdate(...)`), then walks the left spine. Net effect: the **rightmost** operand of a `// // // ...` chain is forced first.
- **v3**: `lowerBinOp` (lower.cc:2058) emits `forceVal(lowerExpr(e->e1))` then `forceVal(lowerExpr(e->e2))`. emit pushes lhs then rhs (emit.cc:932-946). LHS is forced first.
- **Repro**: `(throw "lhs") // (throw "rhs")` — TW throws "rhs", v3 throws "lhs". Also `(throw "x") // {}` — TW throws (forces `(throw "x")`); v3 also throws (forces it first); but `{} // (throw "x")` — TW throws "x"; v3 also throws "x". The bisecting case is when both sides throw.
- **Severity**: Observable but rare in practice (most code doesn't have two simultaneous throws). Could break a test or two if any exists for `//` error precedence.

### D2. `OP_ATTRS_REC_INIT` publishes partial Bindings for ALL non-empty attrsets

- **v3 emit**: `emitOne(ir::AttrSet)` (emit.cc:702) always emits `OP_ATTRS_REC_INIT` (or `_TAIL`) for non-empty attrsets — note emit.cc:712-721 explicitly comments "Why not OP_ATTRS_INIT". The legacy `OP_ATTRS_INIT` opcode survives only as the n=0 fast path (emit.cc:740-744) and for dynamic-attr construction (`OP_ATTRS_INIT_DYN`, emit.cc:856-871).
- **v3 vm**: `OP_ATTRS_REC_INIT` calls `publishToNearestBlackThunkFrame(vm, v, isRecInit=true)` (vm.cc:5072). Under STG mode (default-on; vm.cc:1361 `if (s_stgMode) { if (!isRecInit) return; ...; chain.push_back(v.payload.bindings); }`), this **registers the partial Bindings in the innermost Black thunk frame's partial-bindings registry**. Later `OP_WITH_LOOKUP` and `OP_ATTRS_SELECT` on the Black thunk go through `lookupInPartialChain` (vm.cc:202) which walks the chain and returns the first match (largest-layer-wins).
- **TW**: no analog. `ExprAttrs::eval` (eval.cc:1525) constructs the Bindings local-only; the outer environment is never updated mid-construction.
- **Hazards**:
  - Non-rec subexpressions inside a thunk's body register themselves with that thunk's partial chain (so a `with` reference can find names that belong to a strict sub-expression rather than the thunk's eventual value).
  - For `inherit (e) x y;` clauses, the `OP_ATTRS_REC_INIT` publishes a Bindings with **null placeholders** in the IF slots (lower.cc:2257-2266, vm.cc:5060 `b->entries[i].value.mkNull()`). The IF entries get their actual values via a trailing `OP_ATTRS_REC_SET` issued from `emitOne(AttrSetSetInheritFrom)` (emit.cc:838-854). Between the publish and the IF SET, the partial chain can return the null. The known mitigations (`OP_ATTRS_LET_REC_INIT` for let-in-body, lower.cc:1311 + emit.cc:995) only cover `let ... in body`, not naked `{ a; inherit (e) y; }`.
  - The "registry-wide chain peek" in `OP_ATTRS_SELECT` (vm.cc:5654-5682) walks **every** entry in the registry, finds chains that contain the source Bindings, then runs `lookupInPartialChain` on those chains. This can return a value from a completely unrelated thunk's chain on a miss — purely a v3 invention.
- **Repro shape**: `lib.systems.elaborate` from nixpkgs lib (see vm.cc:1301-1306 comment on the cluster). Minimal: `rec { a = { b = let prev = a; in prev // { c = 2; }; }.b; }.a` — the `let prev = a` chain inside `b`'s thunk publishes `{prev}` to the partial registry, which a future select on `a` could observe.
- **Severity**: This is the architecture-level divergence that drives the recurring nixpkgs-elaborate bug class. Documented in `CALLPACKAGE_BUG_2026-05-09.md` and the `#455 root cause` memory.

### D3. inherit-from from-exprs heuristically (not always) thunkified

- **TW**: `ExprAttrs::buildInheritFromEnv` (eval.cc:1513) — every from-expr is `from->maybeThunk(state, up)` (line 1520). For non-trivial from-exprs (anything other than Int/Float/String/Path/Var), `maybeThunk` builds a `Thunk(env, this)` — strictly lazy. This is also what happens for the rec case (line 1537).
- **v3**: `pushInheritFromCache` (lower.cc:1686) is gated on:
  - `useThunkBlanket = (s_lambdaSkip || s_thunkifyAll) && !s_noThunkify` (line 1743) where `s_thunkifyAll` requires `NIX_V3_INHERIT_FROM_THUNK_ALL` **OR** the **lower-time** `NIX_V3_STG` env var (line 1735-1742). This is distinct from the runtime STG mode (`NIX_V3_NO_STG`, vm.cc:1332).
  - Without the blanket flag, v3 falls back to two heuristics: `isSelfDotPattern` (lower.cc:1808-1821, recognizes `lambda_param.X` shape, gated by `NIX_V3_SELF_DOT_MAX_LEVEL`) and `isComplexFromExpr` (lower.cc:1847-1945, recognizes `OpUpdate`, `Call` with Var head, `Select` on Var head, and `Var fromWith`).
  - When neither heuristic fires, v3 emits `lowerExpr(fx)` directly (line 2046) — **eager evaluation at attrset construction time**. TW would defer.
- **Repro**: any from-expr that v3's heuristics don't match. The committed comment at lower.cc:1825-1858 explicitly notes the heuristic-gap caused `lib/systems/default.nix`'s `inherit ({...} // platforms.select final) ...` to misbehave.
- **Severity**: The default-mode v3 is observably less lazy than TW for inherit-from. With `NIX_V3_INHERIT_FROM_THUNK_ALL=1` v3 matches TW (modulo the `OP_ATTRS_REC_INIT` registration issue above), but per the comment at lower.cc:1727-1734 enabling that broke other lowering paths.

### D4. `attrNames`/`attrValues` re-sort lexicographically; v3 stores names in interning order

- **TW**: `prim_attrNames` (primops.cc:3186) iterates `*args[0]->attrs()` (Symbol-sorted, i.e., interning-id sorted), then re-sorts the resulting list with `std::sort` on `string_view < string_view` (lexicographic). Net result: lexicographic.
- **v3**: `primAttrNames` (primops.cc:480) iterates Bindings (SymbolId-sorted, same as TW), then re-sorts with `string_view < string_view`. Match.
- **Internal storage**: v3 Bindings store entries sorted by `SymbolId` (interning order; emit.cc:751-754 sorts on `e.entries[a].name < e.entries[b].name`). TW Bindings are sorted by `Symbol < Symbol` (numeric `<=>` on `Symbol::id`; symbol-table.hh:74). Both are interning-order; the names are interned in the same order by the parser (single SymbolTable). **No divergence**.
- **HOWEVER**, this lexicographic re-sort means `attrNames` and `attrValues` produce the public-facing alphabetic ordering. **`builtins.mapAttrs` and iteration via `foldl' / iterating with builtins.attrValues` rely on this**. Both implementations sort, so user code is fine. **Match**.

### D5. `__functor` error path

- **TW** (eval.cc:2141-2167): if `vCur` is `nAttrs` and has `__functor`, calls `callFunction(*functor->value, args2, vCur, functor->pos)` where `args2 = [vCur, args[0]]`. If `vCur` is `nAttrs` with no `__functor`, falls through to the generic "attempt to call something which is not a function" TypeError.
- **v3** (vm.cc:2941-2955): if `fun.isAttrs()`, **assumes** `__functor` is present; throws "v3 OP_CALL: callee is an attrset without __functor" if not. Different error message and different code path.
- **Calling sequence**: TW's `callFunction` re-enters with `args = [self, arg]` and processes them as ordinary args via the curried loop. v3 does two explicit `callClosure(vm, forced, fun)` then `callClosure(vm, firstStep, arg)`. The forced functor must already be a closure (or primop/another functor) by the time of the call. If `__functor` is a thunk that hasn't forced yet, `forceValue(vm, *fn)` runs first (vm.cc:2948). TW's `callFunction` recursion would force via its own re-entry. Match on result for the happy path; minor error-message divergence.

---

## Suspect areas (uncertain but subtly different)

### S1. v3 publishes ALL attrset shapes via `OP_ATTRS_REC_INIT`, even ones built inside lambda bodies

Looking at lower.cc:2113 (`lowerAttrs`) — the non-rec / non-dyn path emits a single `ir::AttrSet` binding (line 2300-2301). emit.cc:702 turns that into `OP_ATTRS_REC_INIT`. The `isFunctionReturn` markers (line 558 `markTailReturnAttrSets`) only flip the emitted opcode from `OP_ATTRS_REC_INIT` to `OP_ATTRS_REC_INIT_TAIL`, neither of which avoids publication. There's no path for "this attrset is genuinely a non-rec local intermediate, please don't publish" — all attrsets, regardless of lexical position, register with the innermost Black thunk's partial bindings.

This is the architecture of #495/#496 and the rationale at vm.cc:1283-1315 acknowledges the trade-off: "Restricting publishing to OP_ATTRS_REC_INIT preserves the legitimate use case... while eliminating the cross-expression contamination." But "OP_ATTRS_REC_INIT" used to mean "from a `rec { }`"; today it means "any non-empty attrset". The fix in #498 was to introduce `OP_ATTRS_LET_REC_INIT` for `let-in-body`; the publish hazard for **plain non-rec attrset inside a Black thunk** remains live.

### S2. `OP_ATTRS_SELECT` chain-peek can return placeholder values

Per #D2 above, the partial-bindings chain can contain Bindings where IF slots are still null. `OP_ATTRS_SELECT` at vm.cc:5286-5294 (after the deferred-chase) returns `*v` directly without checking for `mkNull` placeholder. If a `with` from-expr inside the same attrset block runs `OP_WITH_LOOKUP` on an inherit-from name before the trailing `AttrSetSetInheritFrom` binding fires, the lookup returns `nullValue` and the user sees "attribute is null" instead of the actual value. The IF entries are populated only after the entire from-expr cache block finishes (emit.cc:843-848).

The `markTailReturnAttrSets` (lower.cc:558) marks the tail-return attrset to publish to ALL frames (lower.cc:598 sets `isFunctionReturn = true`, emit.cc:796-799 selects `OP_ATTRS_REC_INIT_TAIL`). This **expands** the surface for the placeholder-leak.

### S3. `OP_APPLY_OVERRIDES` mutates the rec attrset in place

`OP_APPLY_OVERRIDES` (vm.cc:5158-5208) overwrites entries via `const_cast<Value &>(*existing) = src->entries[i].value` (line 5187) when an `__overrides` name shadows an existing entry. The same Bindings was published to the partial-bindings registry at `OP_ATTRS_REC_INIT` time (vm.cc:5072). After the in-place overwrite, any cached `attrSelectCache` IC entry (vm.cc:5594-5604) still points at the old slot — but the slot now holds the new value. This is actually safe (IC reads from the bindings directly at access time). Comment at vm.cc:5426-5431 acknowledges this is the contract.

But for entries that are **new** (`toAdd` at line 5189), `OP_APPLY_OVERRIDES` calls `Alloc::allocBindings(dst->size + toAdd.size())` and **replaces `top.payload.bindings`** with the new pointer (line 5206). This invalidates any partial-chain registry entry that points at the old pointer. The registry was keyed by `Thunk *`, not Bindings, so the chain entries still reference the OLD Bindings — `lookupInPartialChain` will see the old shape. TW: `bindings.grow(...)` (eval.cc:1571) and rewrites `(*bindings.bindings)[j->second.displ] = i;` (line 1575) — also a swap. No analog publishToBlack path to be stale, so harmless in TW.

### S4. v3 sentinel placeholder for `OP_ATTRS_REC_INIT` entries

vm.cc:5060 writes `b->entries[i].value.mkNull()` for every slot at `OP_ATTRS_REC_INIT`. Subsequent `OP_ATTRS_REC_SET` (vm.cc:6947) overwrites with the actual value. If `OP_ATTRS_SELECT` runs **before** the `OP_ATTRS_REC_SET` for that slot (only possible via the partial-chain peek path, since the normal stack-based protocol guarantees REC_INIT-then-SETs are adjacent), the user sees `null`. **Not** a divergence under the normal protocol; **is** a divergence under partial-chain peek. Already covered by S2.

### S5. `OP_ATTRS_SELECT_DYN` does not raise duplicate-key error

`OP_ATTRS_INIT_DYN` does duplicate-key detection at construction time (vm.cc:4980-4988). But `OP_ATTRS_INIT_DYN` is **only emitted** by the dynamic-attr branch of `lowerAttrs` (lower.cc:2188-2228). The dynamic-attr emit uses `internSym(kv.first)` for static names and runtime intern for dynamic names, both unified in the same sort step. Looks correct.

Subtle: TW's dup check (eval.cc:1603) tests `bindings.bindings->get(nameSym)` — which after sort returns the **earliest** insertion. Since TW iterates dynamic attrs in source order AFTER inserting statics, the error message points at the static (if there is one) or the earlier dynamic. v3's adjacency check (vm.cc:4980-4988) reports the **second** duplicate seen during sort iteration. Different "first defined at..." position. Doesn't affect semantics but error-message divergence.

### S6. v3 `tryBridgeAttrLookup` / `lookupInPartialChain` largest-layer-wins

`lookupInPartialChain` (vm.cc:202-228) defaults to **largest-layer-wins** (line 215-228) rather than back-to-front (latest-wins). The `NIX_V3_NO_LARGEST_PEEK` env var restores latest-wins. TW has no chain at all, so no analog. For multi-layer cases (nested fix-points), the layer-size heuristic decides which "shape" the surrounding consumers see — different answers for different layers. **Pure v3 semantics**, not directly observable from outside, but the chain that's returned can drive divergent computation downstream.

### S7. `__overrides` non-attrset value handling

- **TW**: `state.forceAttrs(*vOverrides, ..., "while evaluating the `__overrides` attribute")` (eval.cc:1567-1570) — throws if not an attrset, with the contextual error message.
- **v3**: `OP_APPLY_OVERRIDES` at vm.cc:5174-5175 throws "v3 OP_APPLY_OVERRIDES: __overrides must be an attrset". Match in semantics, divergent error message.

### S8. `OP_APPLY_OVERRIDES` fires unconditionally; TW's gate is at parse-time

- **TW** (eval.cc:1539-1540): `AttrDefs::iterator overrides = attrs->find(state.s.overrides); bool hasOverrides = overrides != attrs->end();` — at AST-eval time, if there's no `__overrides` literal **lexically**, the entire override block is skipped.
- **v3** (emit.cc:1119-1122): `OP_APPLY_OVERRIDES` is emitted **unconditionally** at the end of every `LetRec` body. The opcode at vm.cc:5158-5170 short-circuits if the attrset has no `__overrides` name. Semantically equivalent but pays a Bindings lookup per LetRec.

A subtler issue: TW only checks AST-level presence of `__overrides`. If a rec attrset has `inherit (e) __overrides;`, TW sees the AttrDef and treats the rec as having overrides. v3 sees the same and emits `OP_APPLY_OVERRIDES`. The opcode then does a runtime lookup which sees the (already-set or partially-resolved-via-IF) attr. Match.

---

## Matches

The following pieces line up between TW and v3:

### M1. Rec-binding visibility order

TW (`ExprAttrs::eval` for `recursive=true`, eval.cc:1546-1555) creates all `env2.values[displ++]` thunks BEFORE evaluating any binding's body. v3 (`lowerLetRecCapture`, lower.cc:2602-2627) emits `OP_ATTRS_REC_INIT` once at top, then `OP_ATTRS_REC_SET` per binding; the Bindings* is allocated first and patched per-entry. Each thunk's body captures the rec attrset pointer, so all bindings see each other. Match.

### M2. Non-rec scoping for `Inherited` bindings

TW (eval.cc:1546-1553 for rec, 1588 for non-rec) uses `chooseByKind(&env2, &env, inheritEnv)` — `Inherited` always picks the outer env, regardless of rec/non-rec. v3 (lower.cc:2618-2623) — `Inherited`'s `def.e` is an ExprVar lowered against parent scope without pushing recScope. Match.

### M3. Dynamic-attr name evaluation skips `null`

TW (eval.cc:1596-1597): `if (nameVal.type() == nNull) continue;`. v3 (vm.cc:4969): `if (nameV.isNull()) continue;`. Match.

### M4. Dynamic-attr name type-check (must be string)

TW (eval.cc:1598): `state.forceStringNoCtx(nameVal, i.pos, ...)`. v3 (vm.cc:4970-4971): `if (!nameV.isString()) throw...`. Match.

### M5. Duplicate-name detection in attrsets

TW: `bindings.bindings->get(nameSym)` (eval.cc:1603) for dynamic dup vs prior insert. v3: `OP_ATTRS_INIT` (vm.cc:4913-4920) and `OP_ATTRS_INIT_DYN` (vm.cc:4980-4988) check adjacent-equal after sort. Both throw. Error-message divergence noted in S5 but semantics match.

### M6. Has-attr does not force the resulting value

TW (eval.cc:1770-1789) forces each intermediate `vAttrs` but only checks `mkBool(true|false)` for the final step (no force). v3 (lower.cc:2779-2823 + vm.cc:5786 `OP_ATTRS_HAS`) — `OP_ATTRS_HAS` forces its source attrs but never forces the resulting value. Match.

### M7. Selector path stops at missing attr (no default case)

TW (eval.cc:1718-1730) raises "attribute '%1%' missing" with suggestions. v3 (vm.cc:5732) raises "v3 OP_ATTRS_SELECT: attribute not found" (after the registry-peek miss). Both stop at the first miss. Error-message divergence noted but semantic match.

### M8. Selector with `or` short-circuits

TW (eval.cc:1712-1717): `if (def) { state.forceValue(*vAttrs, pos); if (vAttrs->type() != nAttrs || !(j = vAttrs->attrs()->get(name))) { def->eval(state, env, v); return; } }`. v3 (lower.cc:2750-2771): emits `If hasIt then AttrSelect ... else default`. Both short-circuit on missing. Match.

### M9. `//` merge semantics: RHS wins, sorted-merge preserves order

TW (eval.cc:2390-2412) sorted-merge with j-wins. v3 `mergeBindings` (vm.cc:666-703) sorted-merge with b-wins (where b = rhs). Match including position forwarding.

### M10. `__functor` happy path

TW (eval.cc:2141-2158) and v3 (vm.cc:2941-2955) both: force the `__functor` value, call as `f self arg`. Match (modulo error-message divergence noted in D5).

### M11. List position (empty list) avoids fresh thunk

TW: `ExprList::maybeThunk` returns `&Value::vEmptyList` for empty (eval.cc:1654-1660). v3 emits `OP_ATTRS_INIT 0` → pushes `Value::vEmptyAttrs` singleton (vm.cc:4881-4884) for empty attrsets. Both avoid allocation in the empty case. Match.

### M12. Sort by Symbol id

TW iterates `*attrs` (`std::pmr::map<Symbol, AttrDef>`, sorted by `Symbol::id`) and inserts into Bindings (eval.cc:1554, 1587). v3 sorts entries by SymbolId at emit time (emit.cc:751-754) and at OP_ATTRS_INIT_DYN runtime (vm.cc:4977-4978). Both Bindings layouts are interning-id-sorted; per M12, `builtins.attrNames` re-sorts lexicographically. Match.

### M13. `attrNames`/`attrValues` lexicographic output

Per D4 above. Both implementations sort `string_view` lexicographically.

### M14. `inherit (e) x y;` shares one evaluation of `e`

TW: `buildInheritFromEnv` (eval.cc:1513) builds **one** `inheritEnv` per `ExprAttrs`, and each `InheritedFrom` AttrDef indexes into it via `chooseByKind(&env2, &env, inheritEnv)`. The `from->maybeThunk(state, up)` (line 1520) returns the same thunk pointer for repeated displ. v3: `pushInheritFromCache` populates `inheritFromCacheStack.back()` with one VarId per displ (lower.cc:2046); `lowerInheritFrom` returns the cached VarId (line 1659). Match.

### M15. Selector forces the result at the end

TW (eval.cc:1738): `state.forceValue(*vAttrs, ...)` at the bottom of the loop. v3 (vm.cc:5613-5620, 5741-5748): `OP_ATTRS_SELECT` writes-back the Tag::App force-result (mapAttrs memoization). Both leave a forced value on the stack. Match.

### M16. Rec attrset and `__overrides`

TW (eval.cc:1565-1581): on `__overrides`, forces the override attrset, grows bindings, replaces matching names in both `env2.values` AND `bindings.bindings`. v3 (vm.cc:5158-5208): mutates entries in place, allocates a larger Bindings* if new names are added. Semantic match.

---

## Notes for future investigation

- The `OP_ATTRS_LET_REC_INIT` / `OP_ATTRS_REC_INIT` / `OP_ATTRS_REC_INIT_TAIL` opcode triplet is **load-bearing for correctness**. Any future emit change that consolidates them or alters the publishing rules can re-open the `#495` / `#498` / `#546` family of bugs. The lowering decision at lower.cc:2300 + emit.cc:796 (`isFunctionReturn`) needs paranoid testing against nixpkgs `lib.systems.elaborate`, `lib.fix`, `lib.extends` whenever it's touched.
- The partial-bindings registry + chain-peek mechanism is a **v3-only invention** that adds semantic behaviors with no TW counterpart. The largest-layer-wins heuristic (vm.cc:202-228) is empirical, not derived. A more principled approach would be to drop the chain entirely and rely on STG cell-update at `OP_RETURN` — the cell-update mechanism (vm.cc:6973-6979 + the cell field at `Thunk::cell`) is already in place and is the architecturally-correct replacement (see vm.cc:1316-1330 commentary). The chain peek is the legacy of incomplete cell-update wiring.
- D3 (inherit-from heuristic thunkify) plus D2 (always-publish via REC_INIT) interact: when an inherit-from from-expr is **not** thunkified, it runs eagerly during attrset construction, and its `OP_ATTRS_REC_INIT` (if it's an attrset) publishes to the partial chain. The downstream effect is unpredictable and is the source of nested-fix-point divergence symptoms.
- The v3-specific `attrSelectCache` IC (vm.cc:5421-5432, 5594-5604) keys on `(Bindings*, slot)` — safe because `Bindings::entries` is a FAM. Any opcode that mutates a Bindings's entry count (e.g., `OP_APPLY_OVERRIDES`'s grow path) allocates a new Bindings, so the cache entry naturally invalidates. **Safe today**; would break if a future opcode resizes entries in place. Worth a static assert when adding new opcodes.
