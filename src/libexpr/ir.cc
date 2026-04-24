/// @file
/// IR lowering and free variable analysis implementation.
///
/// Translates the Nix AST into the flat, A-normal-form IR defined in
/// ir.hh.  The lowering pass desugars all syntactic sugar and computes
/// explicit free variable lists for closures and thunks.
///
/// Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
///   Input Output Group.
/// SPDX-License-Identifier: Apache-2.0

#include "nix/expr/ir.hh"
#include "nix/expr/nixexpr.hh"
#include "nix/expr/eval.hh"

#include <algorithm>
#include <cassert>
#include <unordered_map>
#include <unordered_set>

namespace nix::ir {

// ============================================================================
// FreeVars operations
// ============================================================================

void FreeVars::insert(VarId v)
{
    auto it = std::lower_bound(vars.begin(), vars.end(), v);
    if (it == vars.end() || *it != v)
        vars.insert(it, v);
}

void FreeVars::merge(const FreeVars & other)
{
    // Standard sorted merge (set union).
    std::vector<VarId> merged;
    merged.reserve(vars.size() + other.vars.size());
    std::set_union(
        vars.begin(), vars.end(),
        other.vars.begin(), other.vars.end(),
        std::back_inserter(merged));
    vars = std::move(merged);
}

void FreeVars::erase(VarId v)
{
    auto it = std::lower_bound(vars.begin(), vars.end(), v);
    if (it != vars.end() && *it == v)
        vars.erase(it);
}

void FreeVars::subtract(const FreeVars & bound)
{
    std::vector<VarId> result;
    result.reserve(vars.size());
    std::set_difference(
        vars.begin(), vars.end(),
        bound.vars.begin(), bound.vars.end(),
        std::back_inserter(result));
    vars = std::move(result);
}

bool FreeVars::contains(VarId v) const noexcept
{
    return std::binary_search(vars.begin(), vars.end(), v);
}


// ============================================================================
// IRModule helpers
// ============================================================================

BlockId IRModule::freshBlock(PosIdx pos)
{
    auto id = static_cast<BlockId>(blocks.size());
    blocks.push_back(IRBlock{
        .id = id,
        .params = {},
        .bindings = {},
        .terminal = TermReturn{.value = kInvalidVar, .pos = pos},
        .pos = pos,
    });
    return id;
}


// ============================================================================
// AST -> IR Lowering: state and helpers
// ============================================================================

/// Internal lowering state.  One instance per lower() call.
///
/// The lowerer walks the AST post-order, emitting IR bindings into the
/// "current block".  Sub-expressions that need their own scope (lambda
/// bodies, thunk bodies, if-branches) get their own blocks.
class Lowerer
{
    EvalState & state;
    IRModule & module;

    /// The block we are currently emitting bindings into.
    BlockId currentBlock;

    /// Map from AST variable coordinates (level, displacement) to IR VarIds.
    /// This mirrors the StaticEnv chain but using the flat IR namespace.
    ///
    /// Key: (level, displacement) pair packed into a uint64_t.
    /// Value: the IR VarId assigned to that binding.
    std::unordered_map<uint64_t, VarId> envMap;

    /// Stack of env map snapshots for scope push/pop.
    /// Each entry is a set of keys added in that scope, so we can
    /// remove them on scope exit.
    std::vector<std::vector<uint64_t>> scopeStack;

    /// Current nesting level (how many scopes deep we are).
    uint32_t currentLevel = 0;

    /// Extra level offset applied to ExprVar lookups.
    ///
    /// When lowering Inherited bindings in a let with inherit(expr), the
    /// AST binds the expression in the parent env (one level up from
    /// the let scope).  But the lowerer is currently inside the let
    /// scope (and possibly the extra inherit-from scope).  This offset
    /// compensates: it's added to the ExprVar's level during lookup so
    /// that the resolution matches the correct absolute level.
    ///
    /// Mirrors the v1 compiler's `levelOffset` field.
    uint32_t levelOffset = 0;

public:
    Lowerer(EvalState & state, IRModule & module)
        : state(state)
        , module(module)
        , currentBlock(0)
    {}

    /// Lower an expression, returning the VarId holding its result.
    VarId lowerExpr(Expr * expr);

private:
    // -- Scope management --
    static uint64_t packKey(uint32_t level, uint32_t displ)
    {
        return (static_cast<uint64_t>(level) << 32) | displ;
    }

    void pushScope()
    {
        scopeStack.push_back({});
        currentLevel++;
    }

    void popScope()
    {
        assert(!scopeStack.empty());
        for (auto key : scopeStack.back())
            envMap.erase(key);
        scopeStack.pop_back();
        currentLevel--;
    }

    /// Bind a variable at the given (relative level, displacement).
    /// The relative level is converted to an absolute level using
    /// currentLevel so that lookups from nested scopes resolve correctly.
    /// Most callers use level=0 ("bind in current scope").
    void bindVar(uint32_t level, uint32_t displ, VarId var)
    {
        uint32_t absLevel = currentLevel - level;
        auto key = packKey(absLevel, displ);
        envMap[key] = var;
        if (!scopeStack.empty())
            scopeStack.back().push_back(key);
    }

    /// Look up an AST variable by its (relative level, displacement).
    /// The relative level from the ExprVar is converted to an absolute
    /// level using currentLevel, matching the bindVar convention.
    /// Returns kInvalidVar if not found (external variable).
    VarId lookupVar(uint32_t level, uint32_t displ) const
    {
        uint32_t absLevel = currentLevel - level;
        auto it = envMap.find(packKey(absLevel, displ));
        if (it != envMap.end())
            return it->second;
        return kInvalidVar;
    }

    // -- Block management --
    IRBlock & curBlock() { return module.blocks[currentBlock]; }

    /// Emit a binding into the current block and return the result VarId.
    VarId emit(IRExpr && expr, PosIdx pos)
    {
        VarId v = module.freshVar();
        curBlock().bindings.push_back(Binding{
            .result = v,
            .expr = std::move(expr),
            .pos = pos,
        });
        return v;
    }

    // -- Per-expression lowering --
    VarId lowerVar(ExprVar * e);
    VarId lowerInt(ExprInt * e);
    VarId lowerFloat(ExprFloat * e);
    VarId lowerString(ExprString * e);
    VarId lowerPath(ExprPath * e);
    VarId lowerSelect(ExprSelect * e);
    VarId lowerHasAttr(ExprOpHasAttr * e);
    VarId lowerAttrs(ExprAttrs * e);
    VarId lowerList(ExprList * e);
    VarId lowerLambda(ExprLambda * e);
    VarId lowerCall(ExprCall * e);
    VarId lowerLet(ExprLet * e);
    VarId lowerWith(ExprWith * e);
    VarId lowerIf(ExprIf * e);
    VarId lowerAssert(ExprAssert * e);
    VarId lowerNot(ExprOpNot * e);
    VarId lowerConcatStrings(ExprConcatStrings * e);
    VarId lowerPos(ExprPos * e);

    // Binary operators
    VarId lowerBinOp(Expr * e1, Expr * e2, PosIdx pos,
                     auto makeExpr);

    /// Lower a sub-expression into a new block, returning the BlockId.
    /// Used for lambda bodies, thunk bodies, if-branches, etc.
    BlockId lowerIntoBlock(Expr * expr, PosIdx pos);

    /// Lower a sub-expression as a thunk if it is non-trivial, or
    /// eagerly if trivial (mirrors Expr::maybeThunk logic).
    VarId lowerAsThunkOrEager(Expr * expr, PosIdx pos);
};


// ============================================================================
// Expression lowering implementations
// ============================================================================

VarId Lowerer::lowerExpr(Expr * expr)
{
    // Dispatch by dynamic type, same order as the bytecode compiler
    // (most common first).

    if (auto * e = dynamic_cast<ExprVar *>(expr))
        return lowerVar(e);
    if (auto * e = dynamic_cast<ExprSelect *>(expr))
        return lowerSelect(e);
    if (auto * e = dynamic_cast<ExprCall *>(expr))
        return lowerCall(e);
    if (auto * e = dynamic_cast<ExprAttrs *>(expr))
        return lowerAttrs(e);
    if (auto * e = dynamic_cast<ExprLet *>(expr))
        return lowerLet(e);
    if (auto * e = dynamic_cast<ExprIf *>(expr))
        return lowerIf(e);
    if (auto * e = dynamic_cast<ExprLambda *>(expr))
        return lowerLambda(e);
    if (auto * e = dynamic_cast<ExprList *>(expr))
        return lowerList(e);

    // Literals
    if (auto * e = dynamic_cast<ExprInt *>(expr))
        return lowerInt(e);
    if (auto * e = dynamic_cast<ExprFloat *>(expr))
        return lowerFloat(e);
    if (auto * e = dynamic_cast<ExprString *>(expr))
        return lowerString(e);
    if (auto * e = dynamic_cast<ExprPath *>(expr))
        return lowerPath(e);

    // Short-circuit operators: the rhs must be lowered into a separate
    // block so it's only evaluated when the lhs permits.  In A-normal
    // form, all bindings in the same block are executed sequentially,
    // so lowering the rhs into the current block would eagerly evaluate
    // it even when the short-circuit should skip it.
    if (auto * e = dynamic_cast<ExprOpAnd *>(expr)) {
        VarId lhs = lowerExpr(e->e1);
        BlockId rhsBlk = lowerIntoBlock(e->e2, e->pos);
        return emit(IRAnd{lhs, rhsBlk}, e->pos);
    }
    if (auto * e = dynamic_cast<ExprOpOr *>(expr)) {
        VarId lhs = lowerExpr(e->e1);
        BlockId rhsBlk = lowerIntoBlock(e->e2, e->pos);
        return emit(IROr{lhs, rhsBlk}, e->pos);
    }
    if (auto * e = dynamic_cast<ExprOpEq *>(expr))
        return lowerBinOp(e->e1, e->e2, e->pos,
            [](VarId l, VarId r) -> IRExpr { return IREq{l, r}; });
    if (auto * e = dynamic_cast<ExprOpNEq *>(expr))
        return lowerBinOp(e->e1, e->e2, e->pos,
            [](VarId l, VarId r) -> IRExpr { return IRNEq{l, r}; });
    if (auto * e = dynamic_cast<ExprOpNot *>(expr))
        return lowerNot(e);
    if (auto * e = dynamic_cast<ExprOpImpl *>(expr)) {
        VarId lhs = lowerExpr(e->e1);
        BlockId rhsBlk = lowerIntoBlock(e->e2, e->pos);
        return emit(IRImpl{lhs, rhsBlk}, e->pos);
    }
    if (auto * e = dynamic_cast<ExprOpUpdate *>(expr))
        return lowerBinOp(e->e1, e->e2, e->pos,
            [](VarId l, VarId r) -> IRExpr { return IRUpdate{l, r}; });
    if (auto * e = dynamic_cast<ExprOpConcatLists *>(expr))
        return lowerBinOp(e->e1, e->e2, e->pos,
            [](VarId l, VarId r) -> IRExpr { return IRConcatLists{l, r}; });
    if (auto * e = dynamic_cast<ExprOpHasAttr *>(expr))
        return lowerHasAttr(e);

    // String interpolation
    if (auto * e = dynamic_cast<ExprConcatStrings *>(expr))
        return lowerConcatStrings(e);

    // Remaining
    if (auto * e = dynamic_cast<ExprWith *>(expr))
        return lowerWith(e);
    if (auto * e = dynamic_cast<ExprAssert *>(expr))
        return lowerAssert(e);
    if (auto * e = dynamic_cast<ExprPos *>(expr))
        return lowerPos(e);

    throw Error("IR lowering: unhandled expression type at %s",
        state.positions[expr->getPos()]);
}


// ---------------------------------------------------------------------------
// Literals
// ---------------------------------------------------------------------------

VarId Lowerer::lowerInt(ExprInt * e)
{
    return emit(IRLitInt{.value = e->v.integer().value}, noPos);
}

VarId Lowerer::lowerFloat(ExprFloat * e)
{
    return emit(IRLitFloat{.value = e->v.fpoint()}, noPos);
}

VarId Lowerer::lowerString(ExprString * e)
{
    return emit(IRLitString{.value = e->v.string_view()}, noPos);
}

VarId Lowerer::lowerPath(ExprPath * e)
{
    return emit(IRLitPath{
        .path = e->v.pathStrView(),
        .accessor = e->v.pathAccessor(),
    }, noPos);
}


// ---------------------------------------------------------------------------
// Variable reference
// ---------------------------------------------------------------------------

VarId Lowerer::lowerVar(ExprVar * e)
{
    // Check if this is a with-scope variable (resolved dynamically).
    if (e->fromWith != nullptr) {
        return emit(IRWithLookup{
            .name = e->name,
            .pos = e->pos,
            .sourceVar = e,
        }, e->pos);
    }

    // Normal lexically-scoped variable.  Look up by (level, displacement).
    // Apply levelOffset for Inherited bindings (see lowerLet/lowerAttrs).
    uint32_t level = e->level + levelOffset;
    VarId v = lookupVar(level, e->displ);
    if (v != kInvalidVar) {
        // Return a reference to the existing binding.
        return emit(IRVarRef{.var = v}, e->pos);
    }

    // Fallback: variable not found in our env map.  This can happen for
    // variables bound in outer scopes that we haven't lowered (e.g.,
    // top-level builtins).  Emit a placeholder reference that the
    // free variable analysis will pick up.
    //
    // Note: for variables from the runtime base environment, the AST
    // level (relative to the current nested scope) must be adjusted
    // to be relative to the entry block's env (baseEnv).  Variables
    // with level < currentLevel are internal to the IR module; they
    // are references that should have been found in the envMap.  If
    // not found, this indicates a forward reference in a recursive
    // let that the lowerer didn't handle -- which is a bug.  However,
    // we gracefully handle it by treating it as an external reference
    // only when the level is >= currentLevel.
    VarId placeholder = module.freshVar();
    module.varNames[placeholder] = state.symbols[e->name];
    // Record the mapping so subsequent references resolve consistently.
    bindVar(level, e->displ, placeholder);

    if (level >= currentLevel) {
        // True external variable (baseEnv builtins, etc.).
        module.externalVars[placeholder] = ExternalVarRef{
            .level = level - currentLevel,
            .displacement = static_cast<uint32_t>(e->displ),
        };
    }
    // else: internal forward reference that wasn't found.  This can
    // happen during recursive-let lowering when the thunk body is
    // lowered in a sub-block but references a let-bound variable.
    // The placeholder VarId will be picked up by free variable
    // analysis and captured as an upvalue.  (The externalVars map
    // is only consulted by the entry block emitter.)

    return emit(IRVarRef{.var = placeholder}, e->pos);
}


// ---------------------------------------------------------------------------
// Attribute select: a.b.c or default
// ---------------------------------------------------------------------------

VarId Lowerer::lowerSelect(ExprSelect * e)
{
    VarId base = lowerExpr(e->e);
    auto attrPath = e->getAttrPath();

    if (e->def == nullptr) {
        // Simple select chain: a.b.c or a."${name}".c
        // Desugar into a chain of IRAttrSelect / IRAttrSelectDynamic operations.
        VarId current = base;
        for (auto & an : attrPath) {
            if (an.expr) {
                // Dynamic attribute name: lower the name expression,
                // emit a dynamic select.
                VarId nameVar = lowerExpr(an.expr);
                current = emit(IRAttrSelectDynamic{
                    .attrs = current,
                    .nameVar = nameVar,
                }, e->pos);
            } else {
                current = emit(IRAttrSelect{
                    .attrs = current,
                    .name = an.symbol,
                }, e->pos);
            }
        }
        return current;
    }

    // Select with default: a.b.c or default
    //
    // Desugar:
    //   a.b.c or default
    // into:
    //   let _1 = a in
    //   if _1 ? b then
    //     let _2 = _1.b in
    //     if _2 ? c then _2.c
    //     else default
    //   else default
    //
    // This chains of if-has-attr checks, one per path component.

    // Note: the default expression is lowered inside each else branch
    // (not eagerly here) because it may have side effects (e.g., throw)
    // that should only execute when the attribute is actually missing.
    VarId current = base;

    for (size_t i = 0; i < attrPath.size(); ++i) {
        auto & an = attrPath[i];

        // Emit: has-attr check (static or dynamic).
        VarId hasIt;
        if (an.expr) {
            VarId nameVar = lowerExpr(an.expr);
            hasIt = emit(IRHasAttrDynamic{
                .attrs = current,
                .nameVar = nameVar,
            }, e->pos);
        } else {
            hasIt = emit(IRHasAttr{
                .attrs = current,
                .name = an.symbol,
            }, e->pos);
        }

        if (i == attrPath.size() - 1) {
            // Last component: if has, select; else default.
            // Lower both branches into blocks.
            BlockId thenBlk = module.freshBlock(e->pos);
            BlockId elseBlk = module.freshBlock(e->pos);

            // Then branch: select the attribute.
            {
                auto saved = currentBlock;
                currentBlock = thenBlk;
                VarId selected;
                if (an.expr) {
                    VarId nameVar2 = lowerExpr(an.expr);
                    selected = emit(IRAttrSelectDynamic{
                        .attrs = current,
                        .nameVar = nameVar2,
                    }, e->pos);
                } else {
                    selected = emit(IRAttrSelect{
                        .attrs = current,
                        .name = an.symbol,
                    }, e->pos);
                }
                curBlock().terminal = TermReturn{.value = selected, .pos = e->pos};
                currentBlock = saved;
            }

            // Else branch: evaluate and return default.
            {
                auto saved = currentBlock;
                currentBlock = elseBlk;
                VarId defResult = lowerExpr(e->def);
                curBlock().terminal = TermReturn{.value = defResult, .pos = e->pos};
                currentBlock = saved;
            }

            return emit(IRIf{
                .cond = hasIt,
                .thenBlock = thenBlk,
                .elseBlock = elseBlk,
            }, e->pos);
        } else {
            // Intermediate component: if has, select and continue;
            // else short-circuit to default.
            BlockId thenBlk = module.freshBlock(e->pos);
            BlockId elseBlk = module.freshBlock(e->pos);

            // Else: evaluate and return default
            {
                auto saved = currentBlock;
                currentBlock = elseBlk;
                VarId defResult = lowerExpr(e->def);
                curBlock().terminal = TermReturn{.value = defResult, .pos = e->pos};
                currentBlock = saved;
            }

            // Then: select this component, continue in the then block.
            // The remaining path components will be lowered in the
            // then block's continuation.
            {
                (void) currentBlock;  // saved implicitly
                currentBlock = thenBlk;
                VarId selected;
                if (an.expr) {
                    VarId nameVar2 = lowerExpr(an.expr);
                    selected = emit(IRAttrSelectDynamic{
                        .attrs = current,
                        .nameVar = nameVar2,
                    }, e->pos);
                } else {
                    selected = emit(IRAttrSelect{
                        .attrs = current,
                        .name = an.symbol,
                    }, e->pos);
                }

                // Continue lowering remaining path in this block.
                current = selected;
                // We recursively handle the rest inside this block.
                // To avoid complex control flow, emit the if node and
                // continue with `current` set to the if result.
                // Actually, for intermediate nodes we need to thread
                // through the then-block, so just continue the loop.
                // The else block returns default; the then block
                // continues to the next iteration.
                // For proper A-normal form, we continue in the current
                // (then) block.  The outer block gets the if result.
                // This is a simplification -- a real implementation
                // would need phi-nodes or block arguments.  For now,
                // we continue in the then block and rely on the
                // terminal being set at the final step.
            }
        }
    }

    // Should not reach here.
    return current;
}


// ---------------------------------------------------------------------------
// has-attr: expr ? attrpath
// ---------------------------------------------------------------------------

VarId Lowerer::lowerHasAttr(ExprOpHasAttr * e)
{
    VarId base = lowerExpr(e->e);

    // Desugar multi-component has-attr: `a ? b.c.d`
    // into: `(a ? b) && (a.b ? c) && (a.b.c ? d)`
    //
    // Each step: check has-attr, short-circuit to false if missing,
    // select and continue if present.
    VarId current = base;
    VarId result = kInvalidVar;

    for (size_t i = 0; i < e->attrPath.size(); ++i) {
        auto & an = e->attrPath[i];

        VarId hasIt;
        if (an.expr) {
            VarId nameVar = lowerExpr(an.expr);
            hasIt = emit(IRHasAttrDynamic{
                .attrs = current,
                .nameVar = nameVar,
            }, e->getPos());
        } else {
            hasIt = emit(IRHasAttr{
                .attrs = current,
                .name = an.symbol,
            }, e->getPos());
        }

        if (i == e->attrPath.size() - 1) {
            // Last component: the result is the has-attr check.
            result = hasIt;
        } else {
            // Intermediate: AND with the next check.
            // Select the current attr to use as base for the next check.
            // Short-circuit: if !hasIt, result is false.
            VarId selected;
            if (an.expr) {
                VarId nameVar2 = lowerExpr(an.expr);
                selected = emit(IRAttrSelectDynamic{
                    .attrs = current,
                    .nameVar = nameVar2,
                }, e->getPos());
            } else {
                selected = emit(IRAttrSelect{
                    .attrs = current,
                    .name = an.symbol,
                }, e->getPos());
            }
            current = selected;

            // Combine with AND (short-circuit false if missing).
            if (result == kInvalidVar) {
                result = hasIt;
            } else {
                // Wrap hasIt in a trivial block for IRAnd's rhs.
                BlockId rhsBlk = module.freshBlock(e->getPos());
                {
                    auto saved = currentBlock;
                    currentBlock = rhsBlk;
                    VarId ref = emit(IRVarRef{.var = hasIt}, e->getPos());
                    curBlock().terminal = TermReturn{.value = ref, .pos = e->getPos()};
                    currentBlock = saved;
                }
                result = emit(IRAnd{.lhs = result, .rhsBlock = rhsBlk}, e->getPos());
            }
        }
    }

    return result;
}


// ---------------------------------------------------------------------------
// Attribute set construction
// ---------------------------------------------------------------------------

VarId Lowerer::lowerAttrs(ExprAttrs * e)
{
    if (e->recursive) {
        // Recursive attribute set: rec { a = e1; b = e2; inherit (src) c; }
        //
        // Desugaring:
        //   1. Allocate a VarId for the self-reference.
        //   2. Lower each attribute value as a thunk that captures self.
        //   3. For InheritedFrom bindings, build thunks that evaluate
        //      the source and select the attribute (same as for let).
        //   4. Construct IRRecAttrSet with self-reference.
        VarId selfVar = module.freshVar();
        pushScope();

        bool hasInheritFrom = e->inheritFromExprs
            && !e->inheritFromExprs->empty();

        std::vector<IRRecAttrSet::Entry> recEntries;

        // First, bind all attribute names to fresh VarIds so
        // forward references resolve.
        std::vector<std::pair<Symbol, VarId>> attrVars;
        if (e->attrs) {
            uint32_t displ = 0;
            for (auto & [name, def] : *e->attrs) {
                VarId av = module.freshVar();
                module.varNames[av] = "rec:" + state.symbols[name];
                bindVar(0, displ, av);
                attrVars.push_back({name, av});
                displ++;
            }
        }

        // Now lower each attribute value.
        // For each attribute, create a binding that links the pre-assigned
        // VarId (used for forward references during lowering) to the
        // lowered value.  This ensures the pre-assigned VarId gets a stack
        // slot so thunks can capture it as an upvalue.
        if (e->attrs) {
            size_t i = 0;
            for (auto & [name, def] : *e->attrs) {
                VarId valueVar;
                if (hasInheritFrom
                    && def.kind == ExprAttrs::AttrDef::Kind::InheritedFrom)
                {
                    // Desugar: inherit (src) x -> thunk { force(src).x }
                    auto * sel = dynamic_cast<ExprSelect *>(def.e);
                    auto * from = dynamic_cast<ExprInheritFrom *>(sel->e);
                    assert(sel && from && "InheritedFrom must be ExprSelect(ExprInheritFrom, ...)");

                    Expr * srcExpr = (*e->inheritFromExprs)[from->displ];
                    Symbol attrName = sel->getAttrPath()[0].symbol;

                    BlockId bodyBlk = module.freshBlock(def.pos);
                    auto savedBlock = currentBlock;
                    currentBlock = bodyBlk;

                    VarId srcVar = lowerExpr(srcExpr);
                    VarId selResult = emit(IRAttrSelect{
                        .attrs = srcVar,
                        .name = attrName,
                    }, def.pos);
                    curBlock().terminal = TermReturn{.value = selResult, .pos = def.pos};

                    currentBlock = savedBlock;

                    valueVar = emit(IRMkThunk{
                        .freeVars = {},   // Populated by computeFreeVars().
                        .bodyBlock = bodyBlk,
                        .pos = def.pos,
                    }, def.pos);
                } else if (def.kind == ExprAttrs::AttrDef::Kind::Inherited) {
                    // Inherited bindings (plain `inherit x;`) are bound
                    // in the parent env.  Apply levelOffset=1 so the
                    // ExprVar lookup skips the rec scope.
                    levelOffset = 1;
                    valueVar = lowerExpr(def.e);
                    levelOffset = 0;
                } else {
                    valueVar = lowerAsThunkOrEager(def.e, def.pos);
                }

                // Link the pre-assigned VarId to the lowered value so it
                // has a defining binding (needed for free variable capture).
                VarId av = attrVars[i].second;
                curBlock().bindings.push_back(Binding{
                    .result = av,
                    .expr = IRVarRef{.var = valueVar},
                    .pos = def.pos,
                });

                recEntries.push_back(IRRecAttrSet::Entry{
                    .name = name,
                    .value = av,
                    .pos = def.pos,
                });
                i++;
            }
        }

        popScope();

        return emit(IRRecAttrSet{
            .selfVar = selfVar,
            .entries = std::move(recEntries),
        }, e->pos);
    }

    // Non-recursive attribute set.
    bool hasDynamic = e->dynamicAttrs && !e->dynamicAttrs->empty();
    bool hasInheritFrom = e->inheritFromExprs && !e->inheritFromExprs->empty();

    // Helper: desugar an InheritedFrom binding.
    //
    // The AST represents `inherit (src) x` as:
    //   AttrDef { kind=InheritedFrom, e=ExprSelect(ExprInheritFrom(displ), x) }
    // where ExprInheritFrom(displ) indexes into inheritFromExprs.
    //
    // We desugar to a thunk whose body evaluates the source expression
    // and selects the attribute:
    //   IRMkThunk { body = force(src).x }
    //
    // This avoids the env-chain model that ExprInheritFrom relies on.
    auto lowerInheritedFrom = [&](const ExprAttrs::AttrDef & def) -> VarId {
        auto * sel = dynamic_cast<ExprSelect *>(def.e);
        auto * from = dynamic_cast<ExprInheritFrom *>(sel->e);
        assert(sel && from && "InheritedFrom binding must be ExprSelect(ExprInheritFrom, ...)");

        // Get the source expression from inheritFromExprs.
        Expr * srcExpr = (*e->inheritFromExprs)[from->displ];
        Symbol attrName = sel->getAttrPath()[0].symbol;

        // Build a thunk body: evaluate srcExpr, force, select attrName.
        BlockId bodyBlk = module.freshBlock(def.pos);
        auto savedBlock = currentBlock;
        currentBlock = bodyBlk;

        VarId srcVar = lowerExpr(srcExpr);
        VarId result = emit(IRAttrSelect{
            .attrs = srcVar,
            .name = attrName,
        }, def.pos);
        curBlock().terminal = TermReturn{.value = result, .pos = def.pos};

        currentBlock = savedBlock;

        return emit(IRMkThunk{
            .freeVars = {},   // Populated by computeFreeVars().
            .bodyBlock = bodyBlk,
            .pos = def.pos,
        }, def.pos);
    };

    if (!hasDynamic) {
        // Pure static attrset.
        IRAttrSet attrSet;

        if (e->attrs) {
            for (auto & [name, def] : *e->attrs) {
                VarId val;
                if (hasInheritFrom
                    && def.kind == ExprAttrs::AttrDef::Kind::InheritedFrom)
                {
                    // Desugar: inherit (src) x -> thunk { force(src).x }
                    val = lowerInheritedFrom(def);
                } else {
                    val = lowerAsThunkOrEager(def.e, def.pos);
                }
                attrSet.entries.push_back(IRAttrSet::Entry{
                    .name = name,
                    .value = val,
                    .pos = def.pos,
                });
            }
        }

        // Entries are already sorted by Symbol (pmr::map is ordered).
        return emit(std::move(attrSet), e->pos);
    }

    // Mixed static + dynamic attributes.
    IRAttrSetDynamic dynSet;

    if (e->attrs) {
        for (auto & [name, def] : *e->attrs) {
            VarId val;
            if (hasInheritFrom
                && def.kind == ExprAttrs::AttrDef::Kind::InheritedFrom)
            {
                val = lowerInheritedFrom(def);
            } else {
                val = lowerAsThunkOrEager(def.e, def.pos);
            }
            dynSet.staticEntries.push_back(IRAttrSetDynamic::StaticEntry{
                .name = name,
                .value = val,
                .pos = def.pos,
            });
        }
    }

    for (auto & dd : *e->dynamicAttrs) {
        VarId nameVar = lowerExpr(dd.nameExpr);
        VarId valVar = lowerAsThunkOrEager(dd.valueExpr, dd.pos);
        dynSet.dynamicEntries.push_back(IRAttrSetDynamic::DynamicEntry{
            .nameVar = nameVar,
            .value = valVar,
            .pos = dd.pos,
        });
    }

    return emit(std::move(dynSet), e->pos);
}


// ---------------------------------------------------------------------------
// List construction
// ---------------------------------------------------------------------------

VarId Lowerer::lowerList(ExprList * e)
{
    IRList list;
    list.elems.reserve(e->elems.size());
    for (auto * elem : e->elems) {
        list.elems.push_back(lowerAsThunkOrEager(elem, elem->getPos()));
    }
    return emit(std::move(list), e->getPos());
}


// ---------------------------------------------------------------------------
// Lambda
// ---------------------------------------------------------------------------

VarId Lowerer::lowerLambda(ExprLambda * e)
{
    // Lower the lambda body into a new block.
    BlockId bodyBlk = module.freshBlock(e->pos);

    // Set up parameter bindings in the body block's scope.
    auto savedBlock = currentBlock;
    currentBlock = bodyBlk;
    pushScope();

    IRFormals params;
    params.arg = e->arg;

    auto formals = e->getFormals();
    if (formals) {
        params.ellipsis = formals->ellipsis;
        uint32_t displ = 0;

        // If there's a whole-argument binding (`x@{...}`), bind it.
        if (e->arg) {
            VarId argVar = module.freshVar();
            module.varNames[argVar] = "@:" + state.symbols[e->arg];
            bindVar(0, displ, argVar);
            curBlock().params.push_back(argVar);
            displ++;
        }

        // Two-pass approach for formals (mirrors lowerLet):
        //   Pass 1: bind ALL formal VarIds so they're visible in the
        //           envMap.  Default expressions may reference sibling
        //           formals (e.g., `{ a, b ? a }: ...`), and the AST
        //           sorts formals alphabetically, so a later formal
        //           may be referenced by an earlier formal's default.
        //   Pass 2: lower default expressions, which can now look up
        //           any formal in the envMap.
        std::vector<VarId> formalVarIds;
        formalVarIds.reserve(formals->formals.size());
        for (auto & f : formals->formals) {
            VarId fVar = module.freshVar();
            module.varNames[fVar] = "formal:" + state.symbols[f.name];
            bindVar(0, displ, fVar);
            curBlock().params.push_back(fVar);
            formalVarIds.push_back(fVar);
            displ++;
        }

        // Pass 2: lower default expressions now that all formals are bound.
        //
        // Default blocks are emitted as thunks (not inline), so the
        // thunk body must fully evaluate the result.  In particular,
        // if the default is a variable reference to a sibling formal,
        // the reference must be forced (matching ExprVar::eval, which
        // always forces).  Without this force, the thunk would return
        // a raw thunk/value from the sibling's slot without resolving
        // thunk chains.
        for (size_t i = 0; i < formals->formals.size(); ++i) {
            auto & f = formals->formals[i];
            BlockId defBlock = kInvalidBlock;
            if (f.def) {
                BlockId blk = module.freshBlock(f.pos);
                auto savedBlock = currentBlock;
                currentBlock = blk;

                VarId result = lowerExpr(f.def);
                // Wrap the result in a force so the thunk body fully
                // evaluates the default expression (resolves thunk chains).
                VarId forced = emit(IRForce{.thunk = result}, f.pos);
                curBlock().terminal = TermReturn{.value = forced, .pos = f.pos};

                currentBlock = savedBlock;
                defBlock = blk;
            }
            params.formals.push_back(IRFormal{
                .name = f.name,
                .defaultBody = defBlock,
                .pos = f.pos,
            });
        }
    } else {
        // Simple lambda: `x: body`
        if (e->arg) {
            VarId argVar = module.freshVar();
            module.varNames[argVar] = "arg:" + state.symbols[e->arg];
            bindVar(0, 0, argVar);
            curBlock().params.push_back(argVar);
        }
    }

    // Lower the body.
    VarId bodyResult = lowerExpr(e->body);
    curBlock().terminal = TermReturn{.value = bodyResult, .pos = e->pos};

    popScope();
    currentBlock = savedBlock;

    // FreeVars will be computed in a separate pass (computeFreeVars).
    return emit(IRLambda{
        .freeVars = {},  // Populated by computeFreeVars().
        .params = std::move(params),
        .bodyBlock = bodyBlk,
        .name = e->name,
        .pos = e->pos,
        .sourceExpr = e,  // For callFunction compatibility.
    }, e->pos);
}


// ---------------------------------------------------------------------------
// Function call
// ---------------------------------------------------------------------------

VarId Lowerer::lowerCall(ExprCall * e)
{
    VarId func = lowerExpr(e->fun);

    // Lower all arguments left-to-right, then emit sequential applications.
    // Nix functions are curried: `f a b` = `(f a) b`.
    VarId current = func;
    if (e->args) {
        for (auto * argExpr : *e->args) {
            VarId arg = lowerExpr(argExpr);
            current = emit(IRApp{
                .func = current,
                .arg = arg,
            }, e->pos);
        }
    }
    return current;
}


// ---------------------------------------------------------------------------
// Let
// ---------------------------------------------------------------------------

VarId Lowerer::lowerLet(ExprLet * e)
{
    // `let` is just an ExprAttrs (with possible inherit-from) + body.
    //
    // Desugaring:
    //   let a = e1; inherit (src) x; in body
    // ->
    //   let a = e1 in
    //   let x = thunk { force(src).x } in
    //   body
    //
    // For InheritedFrom bindings, the AST gives us:
    //   AttrDef { kind=InheritedFrom, e=ExprSelect(ExprInheritFrom(displ), x) }
    // where ExprInheritFrom(displ) indexes into inheritFromExprs.
    //
    // We desugar to a thunk that directly evaluates the source
    // expression and selects the attribute, avoiding the env-chain
    // model that ExprInheritFrom relies on.

    pushScope();

    auto * attrs = e->attrs;
    bool hasInheritFrom = attrs->inheritFromExprs
        && !attrs->inheritFromExprs->empty();

    // Bind all let-bound variables.
    //
    // Nix `let` is ALWAYS recursive: all bindings are mutually visible
    // regardless of definition order.  (The `attrs->recursive` flag
    // only distinguishes `rec { }` from `{ }` for attrset construction;
    // ExprLet semantics are always recursive -- see ExprLet::eval.)
    //
    // Two-pass strategy:
    //   Pass 1: pre-assign VarIds for all binding names so that
    //           forward references from thunk/lambda bodies resolve.
    //   Pass 2: lower each binding's expression (which may reference
    //           sibling bindings via the VarIds assigned in pass 1).
    if (attrs->attrs) {
        uint32_t displ = 0;
        // Pass 1: assign VarIds so all names are in scope.
        for (auto & [name, def] : *attrs->attrs) {
            VarId v = module.freshVar();
            module.varNames[v] = "let:" + state.symbols[name];
            bindVar(0, displ, v);
            displ++;
        }
        // Pass 2: lower expressions (may reference each other).
        displ = 0;
        for (auto & [name, def] : *attrs->attrs) {
            VarId valueVar;

            if (hasInheritFrom
                && def.kind == ExprAttrs::AttrDef::Kind::InheritedFrom)
            {
                // Desugar: inherit (src) x -> thunk { force(src).x }
                //
                // Extract the source expression from inheritFromExprs
                // and the attribute name from the ExprSelect wrapper.
                auto * sel = dynamic_cast<ExprSelect *>(def.e);
                auto * from = dynamic_cast<ExprInheritFrom *>(sel->e);
                assert(sel && from && "InheritedFrom binding must be ExprSelect(ExprInheritFrom, ...)");

                Expr * srcExpr = (*attrs->inheritFromExprs)[from->displ];
                Symbol attrName = sel->getAttrPath()[0].symbol;

                // Build a thunk body: evaluate srcExpr, force, select attrName.
                // The thunk must be lazy because the let env may not be
                // fully initialized (nix lets are recursive).
                BlockId bodyBlk = module.freshBlock(def.pos);
                auto savedBlock = currentBlock;
                currentBlock = bodyBlk;

                VarId srcVar = lowerExpr(srcExpr);
                VarId selResult = emit(IRAttrSelect{
                    .attrs = srcVar,
                    .name = attrName,
                }, def.pos);
                curBlock().terminal = TermReturn{.value = selResult, .pos = def.pos};

                currentBlock = savedBlock;

                valueVar = emit(IRMkThunk{
                    .freeVars = {},   // Populated by computeFreeVars().
                    .bodyBlock = bodyBlk,
                    .pos = def.pos,
                }, def.pos);
            } else {
                // For Inherited bindings (plain `inherit x;`), the
                // ExprVar was bound in the parent env (one level above
                // the let scope).  Apply levelOffset=1 so that the
                // var lookup skips the let scope to find the correct
                // binding in the enclosing scope.
                if (def.kind == ExprAttrs::AttrDef::Kind::Inherited)
                    levelOffset = 1;
                valueVar = lowerAsThunkOrEager(def.e, def.pos);
                if (def.kind == ExprAttrs::AttrDef::Kind::Inherited)
                    levelOffset = 0;
            }

            // The actual binding was already assigned in pass 1.
            // Emit a linking binding that connects the pre-assigned
            // VarId to the lowered value.
            VarId bound = lookupVar(0, displ);
            curBlock().bindings.push_back(Binding{
                .result = bound,
                .expr = IRVarRef{.var = valueVar},
                .pos = def.pos,
            });
            displ++;
        }
    }

    VarId bodyResult = lowerExpr(e->body);

    popScope();
    return bodyResult;
}


// ---------------------------------------------------------------------------
// With
// ---------------------------------------------------------------------------

VarId Lowerer::lowerWith(ExprWith * e)
{
    VarId attrs = lowerExpr(e->attrs);

    // Push a scope to match the StaticEnv level added by ExprWith::bindVars.
    // The with's body is bindVars'd with a new StaticEnv (isWith = this),
    // so ExprVar levels inside the body are relative to that env.
    // We need to push a scope here so that lookupVar's absolute-level
    // calculation matches the AST's level numbering.
    pushScope();
    BlockId bodyBlk = lowerIntoBlock(e->body, e->pos);
    popScope();

    return emit(IRWith{
        .attrs = attrs,
        .bodyBlock = bodyBlk,
        .pos = e->pos,
    }, e->pos);
}


// ---------------------------------------------------------------------------
// If
// ---------------------------------------------------------------------------

VarId Lowerer::lowerIf(ExprIf * e)
{
    VarId cond = lowerExpr(e->cond);
    BlockId thenBlk = lowerIntoBlock(e->then, e->pos);
    BlockId elseBlk = lowerIntoBlock(e->else_, e->pos);

    return emit(IRIf{
        .cond = cond,
        .thenBlock = thenBlk,
        .elseBlock = elseBlk,
    }, e->pos);
}


// ---------------------------------------------------------------------------
// Assert
// ---------------------------------------------------------------------------

VarId Lowerer::lowerAssert(ExprAssert * e)
{
    // The condition and body must be in SEPARATE evaluation contexts.
    // If both are in the same block, the body's bindings are created
    // eagerly alongside the condition's bindings.  Inline blocks
    // emitted during condition evaluation (e.g., select-or branches)
    // can allocate stack slots in the shared parent context.  When
    // the body reads a variable (e.g., a recursive let binding), it
    // may get a stale or wrong value because the condition's inline
    // blocks shifted stack positions.
    //
    // Fix: lower the body into a separate inline block that's only
    // emitted after OP_ASSERT passes.
    VarId cond = lowerExpr(e->cond);
    BlockId bodyBlk = lowerIntoBlock(e->body, e->pos);
    return emit(IRAssert{
        .cond = cond,
        .bodyBlock = bodyBlk,
    }, e->pos);
}


// ---------------------------------------------------------------------------
// Not
// ---------------------------------------------------------------------------

VarId Lowerer::lowerNot(ExprOpNot * e)
{
    VarId operand = lowerExpr(e->e);
    return emit(IRNot{.operand = operand}, e->getPos());
}


// ---------------------------------------------------------------------------
// String interpolation
// ---------------------------------------------------------------------------

VarId Lowerer::lowerConcatStrings(ExprConcatStrings * e)
{
    IRConcatStrings concat;
    concat.forceString = e->forceString;
    concat.parts.reserve(e->es.size());
    for (auto & [partPos, partExpr] : e->es) {
        concat.parts.push_back(lowerExpr(partExpr));
    }
    return emit(std::move(concat), e->pos);
}


// ---------------------------------------------------------------------------
// __curPos
// ---------------------------------------------------------------------------

VarId Lowerer::lowerPos(ExprPos * e)
{
    return emit(IRPos{.pos = e->pos}, e->pos);
}


// ---------------------------------------------------------------------------
// Binary operator helper
// ---------------------------------------------------------------------------

VarId Lowerer::lowerBinOp(Expr * e1, Expr * e2, PosIdx pos,
                           auto makeExpr)
{
    VarId lhs = lowerExpr(e1);
    VarId rhs = lowerExpr(e2);
    return emit(makeExpr(lhs, rhs), pos);
}


// ---------------------------------------------------------------------------
// Sub-block lowering
// ---------------------------------------------------------------------------

BlockId Lowerer::lowerIntoBlock(Expr * expr, PosIdx pos)
{
    BlockId blk = module.freshBlock(pos);
    auto savedBlock = currentBlock;
    currentBlock = blk;

    VarId result = lowerExpr(expr);
    curBlock().terminal = TermReturn{.value = result, .pos = pos};

    currentBlock = savedBlock;
    return blk;
}


// ---------------------------------------------------------------------------
// Thunk-or-eager lowering
// ---------------------------------------------------------------------------

VarId Lowerer::lowerAsThunkOrEager(Expr * expr, PosIdx pos)
{
    // Mirror the AST's maybeThunk() logic: trivial expressions (literals,
    // variables) are lowered eagerly.  Non-trivial ones get wrapped in
    // a thunk (IRMkThunk) for lazy evaluation.
    //
    // Trivial: ExprInt, ExprFloat, ExprString, ExprPath, ExprVar (non-with).

    if (dynamic_cast<ExprInt *>(expr)
        || dynamic_cast<ExprFloat *>(expr)
        || dynamic_cast<ExprString *>(expr)
        || dynamic_cast<ExprPath *>(expr))
    {
        return lowerExpr(expr);
    }

    if (auto * var = dynamic_cast<ExprVar *>(expr)) {
        if (var->fromWith == nullptr) {
            return lowerExpr(expr);
        }
    }

    // Non-trivial: wrap in a thunk.
    BlockId bodyBlk = lowerIntoBlock(expr, pos);
    return emit(IRMkThunk{
        .freeVars = {},   // Populated by computeFreeVars().
        .bodyBlock = bodyBlk,
        .pos = pos,
    }, pos);
}


// ============================================================================
// Top-level lower() entry point
// ============================================================================

IRModule lower(EvalState & state, Expr * expr)
{
    IRModule module;

    // Create the entry block (index 0).
    module.freshBlock(expr->getPos());

    Lowerer lowerer(state, module);
    VarId result = lowerer.lowerExpr(expr);
    module.entryBlock().terminal = TermReturn{
        .value = result,
        .pos = expr->getPos(),
    };

    // Compute free variable sets for all Lambda and MkThunk nodes.
    computeFreeVars(module);

    return module;
}


// ============================================================================
// Free variable analysis
// ============================================================================

namespace {

/// Collect all VarIds referenced by an IRExpr (its operands).
void collectRefs(const IRExpr & expr, FreeVars & refs)
{
    std::visit([&](const auto & e) {
        using T = std::decay_t<decltype(e)>;

        // Literals -- no references.
        if constexpr (std::is_same_v<T, IRLitInt>
                    || std::is_same_v<T, IRLitFloat>
                    || std::is_same_v<T, IRLitString>
                    || std::is_same_v<T, IRLitPath>
                    || std::is_same_v<T, IRLitBool>
                    || std::is_same_v<T, IRLitNull>
                    || std::is_same_v<T, IRPos>) {
            // No variable references.
        }
        else if constexpr (std::is_same_v<T, IRVarRef>) {
            refs.insert(e.var);
        }
        else if constexpr (std::is_same_v<T, IRLambda>) {
            // Lambda free vars are computed separately; the enclosing
            // scope sees the freeVars of the lambda (once computed).
            // At collection time, we don't add anything here because
            // the lambda body is a separate block.
        }
        else if constexpr (std::is_same_v<T, IRApp>) {
            refs.insert(e.func);
            refs.insert(e.arg);
        }
        else if constexpr (std::is_same_v<T, IRForce>) {
            refs.insert(e.thunk);
        }
        else if constexpr (std::is_same_v<T, IRMkThunk>) {
            // Like lambda: free vars computed separately.
        }
        else if constexpr (std::is_same_v<T, IRAttrSelect>) {
            refs.insert(e.attrs);
        }
        else if constexpr (std::is_same_v<T, IRAttrSelectDynamic>) {
            refs.insert(e.attrs);
            refs.insert(e.nameVar);
        }
        else if constexpr (std::is_same_v<T, IRHasAttr>) {
            refs.insert(e.attrs);
        }
        else if constexpr (std::is_same_v<T, IRHasAttrDynamic>) {
            refs.insert(e.attrs);
            refs.insert(e.nameVar);
        }
        else if constexpr (std::is_same_v<T, IRAttrSet>) {
            for (auto & entry : e.entries)
                refs.insert(entry.value);
        }
        else if constexpr (std::is_same_v<T, IRAttrSetDynamic>) {
            for (auto & entry : e.staticEntries)
                refs.insert(entry.value);
            for (auto & entry : e.dynamicEntries) {
                refs.insert(entry.nameVar);
                refs.insert(entry.value);
            }
        }
        else if constexpr (std::is_same_v<T, IRRecAttrSet>) {
            // Note: selfVar is NOT a runtime variable reference.
            // It's a conceptual marker for the self-referencing env
            // created by OP_ENTER_LET.  The emitter handles it via
            // OP_ENTER_LET, not via emitVarRef.  Including it in refs
            // would incorrectly mark it as a free variable when the
            // rec attrset is inside a thunk or lambda body.
            for (auto & entry : e.entries)
                refs.insert(entry.value);
        }
        else if constexpr (std::is_same_v<T, IRList>) {
            for (auto v : e.elems)
                refs.insert(v);
        }
        else if constexpr (std::is_same_v<T, IRIf>) {
            refs.insert(e.cond);
            // Branch blocks are analyzed separately.
        }
        else if constexpr (std::is_same_v<T, IRPrimOpCall>) {
            for (auto v : e.args)
                refs.insert(v);
        }
        else if constexpr (std::is_same_v<T, IRWith>) {
            refs.insert(e.attrs);
        }
        else if constexpr (std::is_same_v<T, IRWithLookup>) {
            // Dynamic: no static VarId reference.
        }
        else if constexpr (std::is_same_v<T, IRConcatStrings>) {
            for (auto v : e.parts)
                refs.insert(v);
        }
        else if constexpr (std::is_same_v<T, IRAssert>) {
            refs.insert(e.cond);
            // bodyBlock's free vars are handled via subBlockFreeVars.
        }
        else if constexpr (std::is_same_v<T, IRNot>) {
            refs.insert(e.operand);
        }
        // Binary operators (all have lhs/rhs pattern).
        else if constexpr (std::is_same_v<T, IRAdd>
                        || std::is_same_v<T, IRSub>
                        || std::is_same_v<T, IRMul>
                        || std::is_same_v<T, IRDiv>
                        || std::is_same_v<T, IREq>
                        || std::is_same_v<T, IRNEq>
                        || std::is_same_v<T, IRLess>
                        || std::is_same_v<T, IRUpdate>
                        || std::is_same_v<T, IRConcatLists>) {
            refs.insert(e.lhs);
            refs.insert(e.rhs);
        }
        // Short-circuit operators: lhs is a VarId, rhs is a block.
        else if constexpr (std::is_same_v<T, IRAnd>
                        || std::is_same_v<T, IROr>
                        || std::is_same_v<T, IRImpl>) {
            refs.insert(e.lhs);
            // rhsBlock's free vars are handled via subBlockFreeVars.
        }
        else if constexpr (std::is_same_v<T, IRNegate>) {
            refs.insert(e.operand);
        }
        else {
            // Compile-time exhaustiveness: if we add a new variant
            // and forget to handle it here, this static_assert fires.
            static_assert(!std::is_same_v<T, T>,
                "collectRefs: unhandled IRExpr variant");
        }
    }, expr);
}

/// Collect VarIds referenced by a terminal.
void collectTerminalRefs(const Terminal & term, FreeVars & refs)
{
    std::visit([&](const auto & t) {
        using T = std::decay_t<decltype(t)>;

        if constexpr (std::is_same_v<T, TermReturn>) {
            if (t.value != kInvalidVar)
                refs.insert(t.value);
        }
        else if constexpr (std::is_same_v<T, TermTailCall>) {
            refs.insert(t.func);
            refs.insert(t.arg);
        }
        else if constexpr (std::is_same_v<T, TermBranch>) {
            refs.insert(t.cond);
        }
    }, term);
}

/// Compute the free variables of a single block.
///
/// Returns the set of VarIds that are used in the block but not
/// defined by any binding within it (and not in its params).
FreeVars blockFreeVars(
    const IRBlock & block,
    const std::unordered_map<BlockId, FreeVars> & subBlockFreeVars,
    const std::vector<IRBlock> & allBlocks)
{
    // Start with all VarIds defined by params.
    FreeVars defined;
    for (auto p : block.params)
        defined.insert(p);

    // All VarIds referenced in this block.
    FreeVars allRefs;

    for (auto & binding : block.bindings) {
        // Collect references from this binding's expression.
        FreeVars exprRefs;
        collectRefs(binding.expr, exprRefs);

        // For Lambda and MkThunk, add the sub-block's free vars
        // (these are the variables the closure/thunk needs from
        // the enclosing scope).
        std::visit([&](const auto & e) {
            using T = std::decay_t<decltype(e)>;
            if constexpr (std::is_same_v<T, IRLambda>) {
                auto it = subBlockFreeVars.find(e.bodyBlock);
                if (it != subBlockFreeVars.end())
                    exprRefs.merge(it->second);

                // Default-value blocks execute inline in the lambda
                // frame, but any external variables they reference
                // must be available in the lambda's enclosing scope
                // (so the enclosing thunk/lambda captures them).
                //
                // Subtract the lambda body's params + bindings first:
                // references to sibling formals are lambda-internal
                // and don't need to be captured by the enclosing scope.
                FreeVars bodyDefined;
                const auto & bodyBlock = allBlocks[e.bodyBlock];
                for (auto p : bodyBlock.params)
                    bodyDefined.insert(p);
                for (auto & b : bodyBlock.bindings)
                    bodyDefined.insert(b.result);

                for (auto & f : e.params.formals) {
                    if (f.defaultBody != kInvalidBlock) {
                        auto dit = subBlockFreeVars.find(f.defaultBody);
                        if (dit != subBlockFreeVars.end()) {
                            FreeVars external = dit->second;
                            external.subtract(bodyDefined);
                            exprRefs.merge(external);
                        }
                    }
                }
            }
            else if constexpr (std::is_same_v<T, IRMkThunk>) {
                auto it = subBlockFreeVars.find(e.bodyBlock);
                if (it != subBlockFreeVars.end())
                    exprRefs.merge(it->second);
            }
            else if constexpr (std::is_same_v<T, IRIf>) {
                // Both branches' free vars are needed here.
                auto it1 = subBlockFreeVars.find(e.thenBlock);
                if (it1 != subBlockFreeVars.end())
                    exprRefs.merge(it1->second);
                auto it2 = subBlockFreeVars.find(e.elseBlock);
                if (it2 != subBlockFreeVars.end())
                    exprRefs.merge(it2->second);
            }
            // Short-circuit operators: rhs is a block.
            else if constexpr (std::is_same_v<T, IRAnd>
                            || std::is_same_v<T, IROr>
                            || std::is_same_v<T, IRImpl>) {
                auto it = subBlockFreeVars.find(e.rhsBlock);
                if (it != subBlockFreeVars.end())
                    exprRefs.merge(it->second);
            }
            else if constexpr (std::is_same_v<T, IRWith>) {
                auto it = subBlockFreeVars.find(e.bodyBlock);
                if (it != subBlockFreeVars.end())
                    exprRefs.merge(it->second);
            }
            else if constexpr (std::is_same_v<T, IRAssert>) {
                auto it = subBlockFreeVars.find(e.bodyBlock);
                if (it != subBlockFreeVars.end())
                    exprRefs.merge(it->second);
            }
        }, binding.expr);

        allRefs.merge(exprRefs);

        // This binding defines its result.
        defined.insert(binding.result);
    }

    // Terminal references.
    collectTerminalRefs(block.terminal, allRefs);

    // Free vars = referenced - defined.
    allRefs.subtract(defined);
    return allRefs;
}

/// Build a reverse dependency graph: for each block, which blocks
/// reference it (as a sub-block)?  This lets us process leaf blocks
/// first in the bottom-up pass.
void buildBlockDeps(
    const IRModule & module,
    std::unordered_map<BlockId, std::vector<BlockId>> & childToParent,
    std::unordered_map<BlockId, std::vector<BlockId>> & parentToChildren)
{
    for (auto & block : module.blocks) {
        for (auto & binding : block.bindings) {
            std::visit([&](const auto & e) {
                using T = std::decay_t<decltype(e)>;
                if constexpr (std::is_same_v<T, IRLambda>) {
                    parentToChildren[block.id].push_back(e.bodyBlock);
                    childToParent[e.bodyBlock].push_back(block.id);
                    // Also track default-value blocks in formals.
                    for (auto & f : e.params.formals) {
                        if (f.defaultBody != kInvalidBlock) {
                            parentToChildren[block.id].push_back(f.defaultBody);
                            childToParent[f.defaultBody].push_back(block.id);
                        }
                    }
                }
                else if constexpr (std::is_same_v<T, IRMkThunk>) {
                    parentToChildren[block.id].push_back(e.bodyBlock);
                    childToParent[e.bodyBlock].push_back(block.id);
                }
                else if constexpr (std::is_same_v<T, IRIf>) {
                    parentToChildren[block.id].push_back(e.thenBlock);
                    parentToChildren[block.id].push_back(e.elseBlock);
                    childToParent[e.thenBlock].push_back(block.id);
                    childToParent[e.elseBlock].push_back(block.id);
                }
                else if constexpr (std::is_same_v<T, IRAnd>
                                || std::is_same_v<T, IROr>
                                || std::is_same_v<T, IRImpl>) {
                    parentToChildren[block.id].push_back(e.rhsBlock);
                    childToParent[e.rhsBlock].push_back(block.id);
                }
                else if constexpr (std::is_same_v<T, IRWith>) {
                    parentToChildren[block.id].push_back(e.bodyBlock);
                    childToParent[e.bodyBlock].push_back(block.id);
                }
                else if constexpr (std::is_same_v<T, IRAssert>) {
                    parentToChildren[block.id].push_back(e.bodyBlock);
                    childToParent[e.bodyBlock].push_back(block.id);
                }
            }, binding.expr);
        }
    }
}

} // anonymous namespace


void computeFreeVars(IRModule & module)
{
    // Build the block dependency graph.
    std::unordered_map<BlockId, std::vector<BlockId>> childToParent;
    std::unordered_map<BlockId, std::vector<BlockId>> parentToChildren;
    buildBlockDeps(module, childToParent, parentToChildren);

    // Topological sort: process leaf blocks (no children) first,
    // then their parents, bottom-up.
    std::unordered_map<BlockId, FreeVars> blockFrees;
    std::unordered_map<BlockId, uint32_t> childCount;

    for (auto & block : module.blocks) {
        auto it = parentToChildren.find(block.id);
        childCount[block.id] = (it != parentToChildren.end())
            ? static_cast<uint32_t>(it->second.size()) : 0;
    }

    // Seed the work queue with leaf blocks.
    std::vector<BlockId> queue;
    for (auto & [id, count] : childCount) {
        if (count == 0)
            queue.push_back(id);
    }

    while (!queue.empty()) {
        BlockId bid = queue.back();
        queue.pop_back();

        auto & block = module.blocks[bid];
        blockFrees[bid] = blockFreeVars(block, blockFrees, module.blocks);

        // Notify parents.
        auto pit = childToParent.find(bid);
        if (pit != childToParent.end()) {
            for (auto parentId : pit->second) {
                if (--childCount[parentId] == 0)
                    queue.push_back(parentId);
            }
        }
    }

    // Write computed free vars back onto Lambda and MkThunk nodes.
    //
    // For lambdas with formals that have default-value blocks, we must
    // also merge the default blocks' free vars into the lambda's freeVars.
    // Default blocks execute inline in the lambda frame (via emitInlineBlock
    // in emitSubBlockWithFormals), so any variable they reference must be
    // available as an upvalue of the lambda closure.
    for (auto & block : module.blocks) {
        for (auto & binding : block.bindings) {
            std::visit([&](auto & e) {
                using T = std::decay_t<decltype(e)>;
                if constexpr (std::is_same_v<T, IRLambda>) {
                    auto it = blockFrees.find(e.bodyBlock);
                    if (it != blockFrees.end())
                        e.freeVars = it->second;

                    // Merge free vars from default-value blocks.
                    //
                    // Default blocks execute inline in the lambda frame
                    // (via emitInlineBlock in emitSubBlockWithFormals).
                    // Any variable they reference that isn't a param or
                    // binding of the lambda body must be captured as an
                    // upvalue of the lambda closure.
                    //
                    // However, references to other formals (e.g.,
                    //   `{ a, b ? a }: ...`
                    // where b's default references a) are lambda-body
                    // params and must NOT be added to the lambda's
                    // upvalue capture set.
                    {
                        auto & bodyBlock = module.blocks[e.bodyBlock];
                        FreeVars bodyDefined;
                        for (auto p : bodyBlock.params)
                            bodyDefined.insert(p);
                        for (auto & b : bodyBlock.bindings)
                            bodyDefined.insert(b.result);

                        for (auto & f : e.params.formals) {
                            if (f.defaultBody != kInvalidBlock) {
                                auto dit = blockFrees.find(f.defaultBody);
                                if (dit != blockFrees.end()) {
                                    // Store the default block's full free vars
                                    // on the formal for thunk emission.
                                    f.defaultFreeVars = dit->second;

                                    // Take only the vars that are NOT
                                    // defined by the lambda body itself.
                                    FreeVars external = dit->second;
                                    external.subtract(bodyDefined);
                                    e.freeVars.merge(external);
                                }
                            }
                        }
                    }
                }
                else if constexpr (std::is_same_v<T, IRMkThunk>) {
                    auto it = blockFrees.find(e.bodyBlock);
                    if (it != blockFrees.end())
                        e.freeVars = it->second;
                }
            }, binding.expr);
        }
    }
}

} // namespace nix::ir
