/// @file
/// IR optimisation pass: redundant-Force elimination via local
/// strictness analysis.
///
/// Lower emits `forceVal(...)` (an `ir::Force` binding) defensively
/// at every place a strict context demands a value -- BinOp operands,
/// If conditions, AttrSelect roots, etc.  Force is a no-op at runtime
/// when its argument is already in WHNF, but the bytecode still
/// dispatches an OP_FORCE per call site.  On real workloads this is
/// a few percent of every loop iteration's instruction stream.
///
/// This pass identifies bindings whose RHS unconditionally produces
/// a WHNF value and rewrites every `Force{v}` over such a binding
/// as `VarRef{v}` -- a pure alias that the existing
/// `inlineTrivialBindings` pass folds away, after which `deadBindingElim`
/// removes the now-orphan binding.
///
/// "WHNF-producing" Expr kinds (the conservative whitelist):
///
///   - All literals (Lit{Int,Float,Bool,Null,String,Path}).
///   - Lambda (returns Tag::Closure -- a value, not a thunk).
///   - Force itself (Force's result is by definition forced).
///   - AttrSet / AttrSetDyn / Update / LetRec
///     (return Tag::Attrs; entries may be thunks but the *attrset*
///     value is WHNF).
///   - ListExpr / ConcatLists (return Tag::List).
///   - All primitive arithmetic / comparison / boolean / Not / HasAttr
///     (return Int / Float / Bool).
///   - ConcatStrings (returns String).
///   - LitPrimOp / LitBuiltins (return Tag::PrimOp / Tag::Attrs).
///
/// Deliberately NOT WHNF-guaranteed (left untouched):
///
///   - App: an over-applied call returns a thunk-shaped value sometimes.
///   - MkThunk: a thunk by construction.
///   - AttrSelect / AttrSelectDyn: stored entry values may be thunks.
///   - WithLookup: same.
///   - RecBindingSlotRef: returns Tag::Slot pointing at a potentially-
///     thunk Bindings entry.
///   - PrimOpCall: depends on the primop (most return WHNF, but
///     `__seq` / `__lazy`-style primops are pathological); conservative
///     skip avoids surprises.
///   - If / Assert / With: terminal value is in the sub-block, so a
///     cross-block analysis is needed -- future pass.
///
/// Block-local: only consults bindings defined in the same Block as
/// the Force, chasing VarRef chains within that block (mirrors
/// opt_const_fold's resolve pattern).  Outer-scope upvalues are
/// opaque -- their definition lives in another function.
///
/// Idempotent: each Force is examined once.  Run before
/// `inlineTrivialBindings` so the freshly-introduced VarRefs are
/// collapsed in the same pipeline pass.
///
/// Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.
/// SPDX-License-Identifier: Apache-2.0

#include "v3/ir.hh"
#include "v3/ir_scratch.hh"
#include "v3/primop.hh"
#include "v3/bytecode_primops.hh"  // T-8: isBytecodePrimopInstalled

#include <cstdio>
#include <cstdlib>
#include <string_view>
#include <unordered_set>

namespace nix::v3::ir {

namespace {

/// Primops whose body unconditionally writes a non-Thunk tag into the
/// result Value (so the immediately-following `Force` is a no-op).
/// Used by `producesWHNF` to elide Force on `PrimOpCall`.
///
/// Categories:
///   - Type predicates / hasContext / hasAttr / pathExists / elem etc.
///     (return Bool).
///   - Length ops (Int).
///   - Arithmetic, comparison (Int / Float / Bool).
///   - String / Path / Hash builders (fresh String/Path tag).
///   - List / Attrset constructors and transformers (fresh ListVec /
///     Bindings — outer container is WHNF; entries may be thunks but
///     the value itself is WHNF and the trailing Force is redundant).
///   - tryEval (wraps result into Attrs).
///
/// Deliberately EXCLUDED — primops that return an arg or list-element
/// verbatim, so the result tag depends on the caller's input:
///   - `head`, `elemAt`         — return list[i] as-is.
///   - `foldl'`                  — returns the accumulator as-is.
///   - `seq`, `deepSeq`          — return args[1] verbatim.
///   - `addErrorContext`         — returns args[1] verbatim.
///   - `getAttr`                 — returns attrset[name] verbatim.
///   - `import`, `scopedImport`  — return file top-level value verbatim.
///   - `genericClosure`          — returns a list whose entries are
///     forced via the op closure; entries may be thunks but the LIST
///     IS WHNF — could be whitelisted but `genericClosure`'s output
///     uses callbacks → keep conservative until reviewed.
///
/// Maintenance rule (mirrors opt_strict_call_unthunk pattern): when
/// adding a new primop in primops.cc, audit its body — if it writes
/// `Tag::X` for any X != Tag::Thunk regardless of args (including
/// not assigning args[i] verbatim), add the name here.
const std::unordered_set<std::string_view> & alwaysWHNFPrimOps()
{
    static const std::unordered_set<std::string_view> set = {
        // Type predicates / Bool reducers.
        "isAttrs", "isList", "isFunction", "isString", "isInt", "isBool",
        "isNull", "isFloat", "isPath",
        "__isAttrs", "__isList", "__isFunction", "__isString", "__isInt",
        "__isBool", "__isNull", "__isFloat", "__isPath",
        "hasContext",       "__hasContext",
        "hasAttr",          "__hasAttr",
        "pathExists",       "__pathExists",
        "elem",  "__elem",
        "all",   "__all",
        "any",   "__any",
        // Type query → String.
        "typeOf",           "__typeOf",
        // Length → Int.
        "length",           "__length",
        "stringLength",     "__stringLength",
        // Arithmetic.
        "add", "sub", "mul", "div",
        "bitAnd", "bitOr", "bitXor",
        "floor", "ceil", "parseInt",
        "__add", "__sub", "__mul", "__div",
        "__bitAnd", "__bitOr", "__bitXor",
        "__floor", "__ceil",
        // Comparison.
        "lessThan",         "__lessThan",
        "compareVersions",  "__compareVersions",
        // String builders / path coercion / hashes.
        "toString",         "__toString",
        "substring",        "__substring",
        "concatStringsSep", "__concatStringsSep",
        "replaceStrings",   "__replaceStrings",
        "stringReplace",
        "hashString",       "__hashString",
        "hashFile",         "__hashFile",
        "convertHash",      "__convertHash",
        "getEnv",           "__getEnv",
        "placeholder",      "__placeholder",
        "baseNameOf",       "__baseNameOf",
        "dirOf",            "__dirOf",
        "unsafeDiscardStringContext",
        "__unsafeDiscardStringContext",
        "unsafeDiscardOutputDependency",
        "__unsafeDiscardOutputDependency",
        "unsafeGetAttrPos", "__unsafeGetAttrPos",
        "appendContext",    "__appendContext",
        "addDrvOutputDependencies",
        "__addDrvOutputDependencies",
        "getContext",       "__getContext",
        "splitString",      "__splitString",
        // Constants / system info.
        "currentSystem",    "__currentSystem",
        "currentTime",      "__currentTime",
        "langVersion",      "__langVersion",
        "nixVersion",       "__nixVersion",
        "storeDir",         "__storeDir",
        "storePath",        "__storePath",
        "toPath",           "__toPath",
        // Serialization → String / Attrs.
        "toJSON",           "__toJSON",
        "toXML",            "__toXML",
        "toFile",           "__toFile",
        "fromJSON",         "__fromJSON",
        "fromTOML",         "__fromTOML",
        // Filesystem reads → String / Attrs.
        "readFile",         "__readFile",
        "readFileType",     "__readFileType",
        "readDir",          "__readDir",
        // List / Attrset constructors and transformers (Tag::List /
        // Tag::Attrs by construction).
        "tail",             "__tail",
        "attrNames",        "__attrNames",
        "attrValues",       "__attrValues",
        "catAttrs",         "__catAttrs",
        "intersectAttrs",   "__intersectAttrs",
        "removeAttrs",      "__removeAttrs",
        "listToAttrs",      "__listToAttrs",
        "zipAttrsWith",     "__zipAttrsWith",
        "concatLists",      "__concatLists",
        "concatMap",        "__concatMap",
        "filter",           "__filter",
        "map",              "__map",
        "mapAttrs",         "__mapAttrs",
        "groupBy",          "__groupBy",
        "partition",        "__partition",
        "sort",             "__sort",
        "genList",          "__genList",
        "splitVersion",     "__splitVersion",
        "parseDrvName",     "__parseDrvName",
        "match",            "__match",
        "split",            "__split",
        "functionArgs",     "__functionArgs",
        // tryEval wraps in Attrs.
        "tryEval",          "__tryEval",
        // FlakeRef ops return Attrs.
        "parseFlakeRef",    "__parseFlakeRef",
        "flakeRefToString", "__flakeRefToString",
    };
    return set;
}

/// True if `e` always evaluates to a WHNF value.  Whitelist; everything
/// else returns false (conservative).  Force itself is whitelisted so
/// `Force{Force{x}}` collapses to `Force{x}` after one pass + alias
/// fold.
bool producesWHNF(const Expr & e)
{
    // PrimOpCall: WHNF iff the primop is in the always-WHNF whitelist.
    if (const auto * pc = std::get_if<PrimOpCall>(&e)) {
        if (!pc->primop) return false;
        const auto & set = alwaysWHNFPrimOps();
        if (set.find(pc->primop->name) == set.end()) return false;
        // T-8 (CODEBASE_REVIEW_2026-06-11): the name-keyed always-WHNF whitelist
        // is only valid for the C implementation.  If this primop has been
        // swapped for a bytecode override (installBytecodePrimop), its impl may
        // return a non-WHNF tail — so eliding the Force after it would be
        // unsound.  Distrust the whitelist for overridden primops (conservative;
        // costs at most a redundant Force on an already-WHNF result).
        return !isBytecodePrimopInstalled(pc->primop->name);
    }
    return std::holds_alternative<LitInt>(e)
        || std::holds_alternative<LitFloat>(e)
        || std::holds_alternative<LitBool>(e)
        || std::holds_alternative<LitNull>(e)
        || std::holds_alternative<LitString>(e)
        || std::holds_alternative<LitPath>(e)
        || std::holds_alternative<Lambda>(e)
        || std::holds_alternative<Force>(e)
        || std::holds_alternative<AttrSet>(e)
        || std::holds_alternative<AttrSetSetInheritFrom>(e)
        || std::holds_alternative<AttrSetDyn>(e)
        || std::holds_alternative<Update>(e)
        || std::holds_alternative<LetRec>(e)
        || std::holds_alternative<ListExpr>(e)
        || std::holds_alternative<ConcatLists>(e)
        || std::holds_alternative<Add>(e)
        || std::holds_alternative<Sub>(e)
        || std::holds_alternative<Mul>(e)
        || std::holds_alternative<Div>(e)
        || std::holds_alternative<Eq>(e)
        || std::holds_alternative<NEq>(e)
        || std::holds_alternative<Less>(e)
        || std::holds_alternative<Not>(e)
        || std::holds_alternative<And>(e)
        || std::holds_alternative<Or>(e)
        || std::holds_alternative<Impl>(e)
        || std::holds_alternative<HasAttr>(e)
        || std::holds_alternative<HasAttrDyn>(e)
        || std::holds_alternative<ConcatStrings>(e)
        || std::holds_alternative<LitPrimOp>(e)
        || std::holds_alternative<LitBuiltins>(e);
}

/// Walk VarRef chain inside one block until we hit a non-VarRef Expr.
/// Returns nullptr if the chain leaves the block (the source is an
/// upvalue / param / cross-block reference -- opaque to us).
///
/// #766: uses `FlatBlockMap` (sorted vector) instead of a per-call
/// `std::unordered_map` so the per-block construction skips the
/// per-node malloc that dominated #765 profile data.
const Expr * resolve(VarId v, const FlatBlockMap & m)
{
    size_t hops = 0;
    const auto cap = m.size() + 1;
    while (hops++ < cap) {
        const Expr * e = m.find(v);
        if (!e) return nullptr;
        if (auto * vr = std::get_if<VarRef>(e)) {
            if (vr->var == kInvalid) return nullptr;
            v = vr->var;
            continue;
        }
        return e;
    }
    return nullptr;
}

/// Cross-block WHNF: resolve the Block's TermReturn target to its
/// defining Expr (chasing VarRef chains), then test WHNF.  Depth-
/// capped to bound recursion through nested If/Assert/With.  Returns
/// false on any opaque case (terminal references an upvalue, target
/// VarId not found, etc.).
bool blockTerminalIsWHNF(const Module & m, const Block & b, int depth);

bool producesWHNFDeep(const Expr & e, const Module & m, int depth)
{
    if (producesWHNF(e)) return true;
    if (depth <= 0) return false;
    // Cross-block forms: every reachable sub-block's terminal must be
    // WHNF for the carrier itself to be WHNF.
    if (const auto * i = std::get_if<If>(&e)) {
        if (i->thenBlock >= (BlockId)m.blocks.size()) return false;
        if (i->elseBlock >= (BlockId)m.blocks.size()) return false;
        return blockTerminalIsWHNF(m, m.blocks[i->thenBlock], depth - 1)
            && blockTerminalIsWHNF(m, m.blocks[i->elseBlock], depth - 1);
    }
    if (const auto * a = std::get_if<Assert>(&e)) {
        if (a->bodyBlock >= (BlockId)m.blocks.size()) return false;
        return blockTerminalIsWHNF(m, m.blocks[a->bodyBlock], depth - 1);
    }
    if (const auto * w = std::get_if<With>(&e)) {
        if (w->bodyBlock >= (BlockId)m.blocks.size()) return false;
        return blockTerminalIsWHNF(m, m.blocks[w->bodyBlock], depth - 1);
    }
    return false;
}

bool blockTerminalIsWHNF(const Module & m, const Block & b, int depth)
{
    const auto * tr = std::get_if<TermReturn>(&b.terminal);
    if (!tr || tr->value == kInvalid) return false;
    // Resolve TermReturn target through VarRef chain in this block.
    VarId target = tr->value;
    size_t hops = 0;
    const size_t cap = b.bindings.size() + 1;
    while (hops++ < cap) {
        const Expr * found = nullptr;
        for (const auto & bind : b.bindings) {
            if (bind.var == target) { found = &bind.expr; break; }
        }
        if (!found) return false; // terminal not defined in this block
        if (auto * vr = std::get_if<VarRef>(found)) {
            if (vr->var == kInvalid) return false;
            target = vr->var;
            continue;
        }
        return producesWHNFDeep(*found, m, depth);
    }
    return false;
}

} // namespace

/// Eliminate redundant `Force{v}` bindings: if `v` (chasing local
/// VarRef chains) resolves to a WHNF-producing Expr in the same
/// block, rewrite the Force as a VarRef.  Returns the number of
/// Force bindings rewritten.
size_t elimRedundantForce(Module & m)
{
    static const bool disabled =
        std::getenv("NIX_V3_NO_OPT_STRICT") != nullptr;
    if (disabled) return 0;

    static const char * dbgRaw = std::getenv("NIX_V3_DBG_OPT_STRICT");
    const int dbgLevel = dbgRaw ? std::atoi(dbgRaw) : 0;

    // Depth cap for cross-block WHNF probing (If/Assert/With).  4 is
    // deep enough to cover nested-If chains in nixpkgs (mkIf inside
    // optionalAttrs inside Assert) without runaway recursion.
    constexpr int kCrossBlockDepth = 4;

    size_t rewritten = 0;
    size_t forceTotal = 0;
    // Bucket residue Forces by source kind so we can prioritise the
    // next-WHNF-class extension.  Indexed by Expr::index() — variant
    // order in ir.hh determines slot.
    constexpr size_t kKindBuckets = 64;
    size_t residueByKind[kKindBuckets] = {0};
    size_t residueNullSrc = 0;
    size_t residueInvalidThunk = 0;
    auto & bm = FlatBlockMap::scratch();
    for (BlockId bid = 1; bid < (BlockId)m.blocks.size(); ++bid) {
        Block & block = m.blocks[bid];
        bm.rebuild(block);
        for (auto & bind : block.bindings) {
            auto * f = std::get_if<Force>(&bind.expr);
            if (!f) continue;
            ++forceTotal;
            if (f->thunk == kInvalid) { ++residueInvalidThunk; continue; }
            const Expr * src = resolve(f->thunk, bm);
            if (!src) { ++residueNullSrc; continue; }
            if (!producesWHNFDeep(*src, m, kCrossBlockDepth)) {
                size_t idx = src->index();
                if (idx < kKindBuckets) ++residueByKind[idx];
                continue;
            }
            // Rewrite in-place.  The BlockMap's pointer (`&bind.expr`)
            // remains valid — only the Expr's content changes.  A
            // later resolve() in the same block deref's the same
            // pointer and sees the new VarRef, so no map update needed.
            bind.expr = VarRef{f->thunk};
            ++rewritten;
        }
    }
    if (dbgLevel >= 1 && forceTotal > 0) {
        std::fprintf(stderr,
            "v3 opt strictness: %zu / %zu Force bindings rewritten\n",
            rewritten, forceTotal);
    }
    if (dbgLevel >= 2 && forceTotal > 0) {
        std::fprintf(stderr,
            "  residue: invalidThunk=%zu nullSrc=%zu byKind=[",
            residueInvalidThunk, residueNullSrc);
        for (size_t i = 0; i < kKindBuckets; ++i) {
            if (residueByKind[i] > 0)
                std::fprintf(stderr, "%zu:%zu ", i, residueByKind[i]);
        }
        std::fprintf(stderr, "]\n");
    }
    return rewritten;
}

} // namespace nix::v3::ir
