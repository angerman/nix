/// @file
/// IR Phase H — static genList unrolling (2026-05-18).
///
/// Recognises `PrimOpCall("genList", [fVar, nVar])` where `nVar`
/// resolves (via same-block VarRef chase) to a `LitInt` with value
/// 1..kMaxUnrollN.  Rewrites to:
///
///   v_litN-1 = LitInt N-1
///   v_appN-1 = App fVar v_litN-1
///   v_thunkN-1 = MkThunk fT_{N-1}  freeVars=[fVar]
///   ... (N MkThunks total)
///   v_list = ListExpr [v_thunk0, ..., v_thunkN-1]
///   v_genlist = VarRef v_list
///
/// Where each `fT_i` is a fresh Function whose entryBlock contains:
///   v_lit_i = LitInt i
///   v_app_i = App captured_f v_lit_i
///   return v_app_i
///
/// Hypothesis killed (Rule 0): "`genList f n` always builds an
/// N-element list via a runtime primop call."  After this pass,
/// small-N calls are statically expanded — each element becomes a
/// separate MkThunk binding, subject to Phase A inlining when `f`
/// is a known lambda.
///
/// Laziness preserved: each unrolled element is a MkThunk wrapping
/// `App(f, i)`, mirroring tree-walker's `genList` which builds a
/// Tag::App per element.  Forcing the i-th element calls f(i)
/// on demand; forcing element j has no effect on element k.
///
/// Threshold: kMaxUnrollN = 8 (per
/// IR_OPTIMIZATION_PLAN_2026-05-18.md §2.5 Phase H).  Each unrolled
/// call adds N new Function entries + N new Block entries to the
/// module, so the threshold caps per-call cost at a small constant.
/// Larger genList calls fall through to the original PrimOpCall
/// path.
///
/// Unconditional (the NIX_V3_NO_GENLIST_UNROLL A/B opt-out was retired
/// once it shipped byte-identical).
///
/// Dependency: Phase B (primOpFold).  The `n` argument must already
/// resolve to a LitInt — primOpFold collapses chains like
/// `builtins.length [a b c d]` → LitInt 4 that this pass then
/// consumes.
///
/// Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
///   Input Output Group.
/// SPDX-License-Identifier: Apache-2.0

#include "v3/ir.hh"
#include "v3/ir_util.hh"
#include "v3/primop.hh"

#include <cstdio>
#include <cstdlib>
#include <optional>
#include <unordered_map>
#include <variant>
#include <vector>

namespace nix::v3::ir {

namespace {

constexpr int64_t kMaxUnrollN = 8;

// chaseInBlock() is shared across opt_*.cc passes — see v3/ir_util.hh.

std::optional<int64_t> resolveLitInt(VarId v,
                                     const std::unordered_map<VarId, const Expr *> & defs)
{
    const Expr * e = chaseInBlock(v, defs);
    if (!e) return std::nullopt;
    if (const auto * li = std::get_if<LitInt>(e))
        return li->value;
    return std::nullopt;
}

/// Pattern test: is `e` a saturated `PrimOpCall("genList", [f, n])`?
/// Returns (fVar, nVar) on match, std::nullopt otherwise.
std::optional<std::pair<VarId, VarId>> matchGenList(const Expr & e)
{
    const auto * pc = std::get_if<PrimOpCall>(&e);
    if (!pc || !pc->primop) return std::nullopt;
    if (std::string_view(pc->primop->name) != "genList") return std::nullopt;
    if (pc->args.size() != 2) return std::nullopt;
    return std::make_pair(pc->args[0], pc->args[1]);
}

/// Build a single per-element thunk Function + Block.  Returns the
/// FuncId; caller wires up the MkThunk binding.
///
/// IMPORTANT: `m.freshBlock()` and `m.functions.emplace_back()` BOTH
/// can reallocate the underlying `m.blocks` / `m.functions` vectors.
/// References into either are invalidated.  Callers MUST NOT hold a
/// `Block &` or `Function &` reference across a call to this
/// function.  We commit field writes via index every time.
FuncId buildElementThunk(Module & m, VarId fVar, int64_t i)
{
    FuncId fid = static_cast<FuncId>(m.functions.size());
    m.functions.emplace_back();
    BlockId bid = m.freshBlock();

    // VarId allocation does NOT invalidate vectors (it just bumps a
    // counter), but the writes that follow must use index-based
    // access — `freshBlock` may have reallocated `m.blocks`.
    VarId litVar  = m.freshVar();
    VarId appVar  = m.freshVar();

    m.functions[fid].entryBlock = bid;
    m.functions[fid].paramVar   = kInvalid;
    m.functions[fid].argName    = kInvalidSymbol;
    m.functions[fid].name       = "<genListThunk>";

    m.blocks[bid].bindings.clear();
    m.blocks[bid].bindings.push_back({ litVar, LitInt{ i } });
    m.blocks[bid].bindings.push_back({ appVar, App{ fVar, litVar } });
    m.blocks[bid].terminal = TermReturn{ appVar };

    return fid;
}

} // namespace

size_t genListUnroll(Module & m)
{
    static const bool s_dbg =
        std::getenv("V3_DBG_GENLIST_UNROLL") != nullptr;

    size_t unrolled = 0;

    // Snapshot the block count UP FRONT — we'll be appending new
    // blocks during the rewrite (via freshBlock + buildElementThunk),
    // and we MUST NOT iterate into those newly-appended blocks (they
    // belong to thunk bodies and have their own pattern semantics).
    const BlockId nOrigBlocks = static_cast<BlockId>(m.blocks.size());

    for (BlockId bid = 1; bid < nOrigBlocks; ++bid) {
        // IMPORTANT: do NOT hold a `Block &` reference across calls
        // to `buildElementThunk` — `freshBlock()` reallocs
        // `m.blocks`.  Index-based access (`m.blocks[bid]`) is
        // safe because the BlockId never moves, even when the
        // backing vector grows.

        // Build defs map once per block from the original bindings
        // (we look up VarIds that existed BEFORE this pass ran).
        std::unordered_map<VarId, const Expr *> defs;
        defs.reserve(m.blocks[bid].bindings.size());
        for (const auto & bd : m.blocks[bid].bindings)
            defs.emplace(bd.var, &bd.expr);

        // First pass — scan-without-mutating.  Detect any foldable
        // genList; only commit if at least one applies.  Same
        // two-pass discipline as opt_if_fold.cc (commit bc346f9a5
        // memo: scan first, then commit — avoids the
        // std::move-from-iterating-vector foot-gun).
        bool anyFold = false;
        for (const auto & bd : m.blocks[bid].bindings) {
            if (auto pair = matchGenList(bd.expr)) {
                if (auto n = resolveLitInt(pair->second, defs)) {
                    if (*n >= 0 && *n <= kMaxUnrollN) {
                        anyFold = true;
                        break;
                    }
                }
            }
        }
        if (!anyFold) continue;

        // Second pass — commit the rewrite.  Walk the binding list,
        // when we hit a matching genList, splice in the new bindings
        // (per-element thunks + ListExpr) and rewrite this binding's
        // expr to VarRef(list_var).  Copy the bindings out of
        // `m.blocks[bid]` first so we can safely call
        // `buildElementThunk` (which may realloc `m.blocks`).
        std::vector<Binding> origBindings = std::move(m.blocks[bid].bindings);
        std::vector<Binding> out;
        out.reserve(origBindings.size() + kMaxUnrollN * 2);
        for (auto & bd : origBindings) {
            auto pair = matchGenList(bd.expr);
            if (!pair) {
                out.push_back(std::move(bd));
                continue;
            }
            auto n = resolveLitInt(pair->second, defs);
            if (!n || *n < 0 || *n > kMaxUnrollN) {
                out.push_back(std::move(bd));
                continue;
            }
            VarId fVar = pair->first;
            int64_t N = *n;

            // Emit N thunk bindings.  Note: buildElementThunk MAY
            // realloc m.blocks via freshBlock — that's fine here
            // because we already moved blk's bindings into a local
            // `origBindings` vector + we use `m.blocks[bid]` index
            // access for the final commit, not a cached reference.
            std::vector<VarId> elementVars;
            elementVars.reserve(static_cast<size_t>(N));
            for (int64_t i = 0; i < N; ++i) {
                FuncId tFid = buildElementThunk(m, fVar, i);
                VarId tVar = m.freshVar();
                // freeVars and lexicalWiths are left empty here —
                // computeFreeVars (run AFTER optimise) populates
                // these from the body block's actual VarRef usage.
                out.push_back({ tVar, MkThunk{ tFid, /*freeVars*/ {},
                                                 /*lexicalWiths*/ {} } });
                elementVars.push_back(tVar);
            }

            // Emit the aggregating ListExpr binding.
            VarId listVar = m.freshVar();
            out.push_back({ listVar, ListExpr{ std::move(elementVars) } });

            // Rewrite this binding's expr to VarRef the list.
            bd.expr = VarRef{ listVar };
            out.push_back(std::move(bd));

            ++unrolled;
            if (s_dbg) {
                std::fprintf(stderr,
                    "v3 genListUnroll: B%u: unrolled N=%lld "
                    "(N new funcs + N new blocks + 1 ListExpr binding)\n",
                    (unsigned)bid, (long long)N);
            }
        }
        m.blocks[bid].bindings = std::move(out);
    }

    return unrolled;
}

} // namespace nix::v3::ir
