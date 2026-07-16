/// @file
/// v3 IR → bytecode emit.
///
/// Per Function:
///   - allocate stack slots for param + every reachable binding (lazily,
///     on first reference)
///   - emit the entry Block; emit any sub-Blocks (if branches, &&/||/->
///     rhs, with/assert bodies) in-line at their reference site
///   - terminate with OP_HALT (top-level) or OP_RETURN
///
/// Slot allocation is per-Function: a VarId resolves to a frame slot if it
/// is defined within the Function, or an upvalue index if it is a free
/// variable captured by the enclosing closure.
///
/// Each Block's TermReturn leaves the result Value on the operand stack.
/// The parent context (e.g., the surrounding If binding) consumes that
/// stack-top value, typically via OP_SET_LOCAL.
///
/// Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.
/// SPDX-License-Identifier: Apache-2.0

#include "v3/alloc.hh"   // Phase-1 capture-model emit-time counters (V3_STATS_BUMP)
#include "v3/bytecode.hh"
#include "v3/disasm.hh"
#include "v3/ir.hh"
#include "v3/primop.hh"
#include "v3/vm.hh"

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <deque>
#include <set>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

/// Build a `"lower.cc:NNN"` literal for an arbitrary integer line number
/// known only at runtime (e.g. one that came in via an ir::Force srcLine
/// field).  We intern these in a small static pool so the side-table can
/// hold a stable `const char *` without owning storage on every entry.
///
/// The pool deduplicates by line number — the ~14 lower.cc force sites
/// produce at most ~14 distinct strings across the entire process, so a
/// linear scan is fine.

namespace nix::v3 {

namespace {

/// Intern a `lower.cc:NNN` string for the given source line number.
/// Returns a stable `const char *` valid for the process lifetime.
///
/// Uses `std::deque` rather than `std::vector` so existing `c_str()`
/// pointers stored in `CompilationUnit::forceEmitSites` survive any
/// future appends — `std::vector<std::string>::emplace_back` may
/// reallocate the buffer and (for SSO-fitting short strings) move
/// the actual character storage too, invalidating prior `c_str()`s.
const char * internLowerCcSiteString(int line)
{
    // {line -> stable-cstr}.  Linear scan is fine — at most ~16
    // entries in practice (one per forceVal call site in lower.cc).
    static std::deque<std::pair<int, std::string>> pool;
    for (auto & p : pool)
        if (p.first == line) return p.second.c_str();
    char buf[32];
    std::snprintf(buf, sizeof buf, "lower.cc:%d", line);
    pool.emplace_back(line, std::string(buf));
    return pool.back().second.c_str();
}

/// Intern an arbitrary site label (used for emit.cc-internal force
/// sites that don't originate from lower.cc, such as `ir::With`'s
/// rec-attrset force or `ir::RecBindingSlotRef`'s pre-force).
/// Same `std::deque` rationale as above for pointer stability.
const char * internEmitSiteString(const char * label)
{
    static std::deque<std::string> pool;
    for (auto & s : pool)
        if (s == label) return s.c_str();
    pool.emplace_back(label);
    return pool.back().c_str();
}

struct Emitter
{
    const ir::Module & m;
    CompilationUnit unit;

    /// Phase-1 capture-model instrumentation (BEAT_TW_V3_PLAN §3, Counter 3).
    /// Set true while emitting the capture (lexicalWiths+freeVars) sequence of a
    /// Lambda/MkThunk, so emitVarRef can classify each emitted GET as a capture
    /// (and whether it forwards an upvalue) vs a body read.  Emit-time only.
    bool emittingCaptures_ = false;

    /// #542 — module-wide occurrence info, computed once per compile.
    /// Drives the per-binding "defer SET vs emit SET" decision: only
    /// OnceLinear bindings are safe to defer (single use → the
    /// consumer pops the value off the runtime stack exactly once).
    /// Many-use bindings need a real SET so subsequent GETs read from
    /// the slot.
    ir::OccMap occ;

    struct FuncCtx
    {
        ir::FuncId                          fid;
        std::unordered_map<ir::VarId, uint16_t> slot;
        std::unordered_map<ir::VarId, uint16_t> upvalue;
        uint16_t nextSlot = 0;
        uint16_t nLocals  = 0;

        /// #542 emit-time deferring stack.  Each entry corresponds to
        /// a runtime-stack value that has NOT yet been flushed to its
        /// slot (its binding's SET_LOCAL was elided).  pendingDefer.back()
        /// is the top of the runtime stack; pendingDefer[0] is the
        /// deepest deferred value.  Mirrors the runtime-stack region
        /// above the *previous* "stable" depth.
        ///
        /// Discipline:
        ///   - Push only OnceLinear bindings (count==1, single use).
        ///   - emitVarRef matches and consumes top (or flushes above
        ///     and consumes deeper).
        ///   - Binary-op fast path detects [lhs, rhs] at end of
        ///     pending and emits the OP without GETs.
        ///   - At sub-block boundary (emitBlock for then/else/rhs/
        ///     body), caller flushes via flushAllDeferred() before
        ///     descending — sub-blocks always run with empty pending.
        std::vector<ir::VarId> pendingDefer;
    };
    FuncCtx * ctx = nullptr;

    // SET_LOCAL_KEEP fusion (BYTECODE_NGRAM_ANALYSIS §7).  Per-function
    // bookkeeping for the adjacent same-slot `SET_LOCAL n; GET_LOCAL n` →
    // `SET_LOCAL_KEEP n` peephole; emitFunction is non-re-entrant (flat
    // loop in compile()), so plain members suffice.  Every jump is created
    // via emitJumpPlaceholder()+patchJump(); hooking those captures the
    // complete jump structure (insn positions + targets) with zero
    // bytecode scanning, so compactFuseSetGet() can re-base operands and
    // skip eliding a GET that is a branch-join target.
    std::vector<uint32_t>        fuseGetPositions_;
    std::vector<uint32_t>        jumpInsnPositions_;
    std::unordered_set<uint32_t> jumpTargetPositions_;
    // Lever 1B-lite robustness: every OP_GET_LOCAL emit position, so the
    // GET_LOCAL2 fusion in compactFuseSetGet pairs ONLY real adjacent
    // GET_LOCAL instructions — never a multi-word op's follow-up word that
    // happens to decode to OP_GET_LOCAL (R_PRIMOP2's dst/descAB, CALL_PRIMOP's
    // poIdx, …).  Reset per function alongside fuseGetPositions_.
    std::vector<uint32_t>        getLocalPositions_;

    // Register VM Phase 5: when ≥0, the block currently being emitted is an If
    // branch whose terminal value must land in this slot (the If's merge slot)
    // rather than on the operand stack.  Captured + reset at emitBlock entry so
    // nested blocks don't inherit it; set by emitBlockToSlot for each branch.
    int32_t                      blockResultSlot_ = -1;

    // §2(a) constant rematerialization (NEXT_STEPS_2026-06-05).  A-normal-
    // form lowering names every literal operand, so the emitter would spill
    // a pure immediate into a slot (`LIT_* ; SET_LOCAL s ; … ; GET_LOCAL s`)
    // instead of pushing it where used.  For immediates (LitInt/Float/Bool/
    // Null), re-emitting the LIT at each use site is order-independent and
    // ≤ the spill cost regardless of use count (LIT and GET are the same
    // width, and we drop the SET), so we never give them a slot — they are
    // rematerialized in emitVarRef.  constRemat_ is rebuilt per function in
    // preassignSlotsInBlock; it maps the binding's VarId to its Lit Expr
    // (stable: the module is not mutated during emit).  Vars captured as a
    // nested function's freeVar are EXCLUDED (the closure reads them from a
    // parent slot at MAKE_CLOSURE time) — see capturedFreeVars_.
    std::unordered_map<ir::VarId, const ir::Expr *> constRemat_;
    std::unordered_set<ir::VarId>                   capturedFreeVars_;
    bool                                            capturedFreeVarsBuilt_ = false;

    /// An immediate constant safe to rematerialize at each use: re-emitting
    /// the LIT is free (no heap, no context) and width-equal to a GET_LOCAL.
    /// LitString is deliberately excluded — re-emit would append a duplicate
    /// string-pool entry per use; strings are also not the hot operand.
    static bool isRematConst(const ir::Expr & e)
    {
        return std::holds_alternative<ir::LitInt>(e)
            || std::holds_alternative<ir::LitFloat>(e)
            || std::holds_alternative<ir::LitBool>(e)
            || std::holds_alternative<ir::LitNull>(e);
    }

    /// Module-wide union of every function's freeVars — the set of VarIds
    /// that some nested closure captures (and therefore must live in a
    /// parent slot).  Built once, lazily; the module is immutable here.
    void ensureCapturedFreeVars()
    {
        if (capturedFreeVarsBuilt_) return;
        capturedFreeVarsBuilt_ = true;
        for (const auto & fn : m.functions)
            for (ir::VarId fv : fn.freeVars)
                capturedFreeVars_.insert(fv);
    }

    /// #548c (2026-05-10): set by emitBlock when emitting the
    /// terminal-return binding of the function's entry block.  Non-
    /// rec AttrSets in this position become OP_ATTRS_REC_INIT
    /// (publishing), so the surrounding Black thunk's partial-
    /// bindings registry observes the rec-attrset's evolving
    /// Bindings — STG-style "selector thunk on Con cell" sharing
    /// for `with self;` mid-construction lookups.  AttrSets NOT in
    /// the function's tail return position use OP_ATTRS_LET_REC_INIT
    /// (non-publishing) so sub-expression Bindings don't pollute
    /// the registry (#495 fix preserved).
    bool emittingFunctionTailReturn = false;

    Emitter(const ir::Module & mod) : m(mod) {
        // #542: occurrence info drives the defer-vs-SET decision
        // per binding.  Cheap (~O(N) on module size); compute once.
        occ = ir::analyseOccurrence(m);
    }

    // -- #542 deferring helpers --------------------------------------------

    /// Flush all pending deferred values to their slots, top-down.
    /// After this returns, pending is empty and every previously-
    /// deferred binding's slot has been populated.
    void flushAllDeferred()
    {
        while (!ctx->pendingDefer.empty()) {
            ir::VarId v = ctx->pendingDefer.back();
            ctx->pendingDefer.pop_back();
            uint16_t slot = getOrAssignSlot(v);
            unit.code.push_back(encode(OP_SET_LOCAL, slot));
        }
    }

    /// Flush deferred values whose pendingDefer index is > `keepIdx`.
    /// Used when emitVarRef finds the target var deeper in the stack:
    /// flush everything ABOVE it so it ends up on top, then consume.
    void flushDeferAbove(size_t keepIdx)
    {
        while (ctx->pendingDefer.size() > keepIdx + 1) {
            ir::VarId v = ctx->pendingDefer.back();
            ctx->pendingDefer.pop_back();
            uint16_t slot = getOrAssignSlot(v);
            unit.code.push_back(encode(OP_SET_LOCAL, slot));
        }
    }

    /// Flush deferred entries that sit BELOW the runtime-stack TOP
    /// (the conditional value) before a divergent branch opcode runs.
    ///
    /// Why this exists (the #668 bug): tryFastPathUnary(cond) pops the
    /// cond from pendingDefer but leaves DEEPER pending entries in
    /// place.  Those entries' runtime values sit BELOW the cond on the
    /// value stack.  The branch opcode (OP_BRANCH_FALSE / OP_AND_BRANCH
    /// / OP_OR_BRANCH / OP_IMPL_BRANCH) then dispatches one of two
    /// disjoint paths.  Each path's first emitBlock() entry calls
    /// flushAllDeferred(), which emits OP_SET_LOCAL ops that pop the
    /// deferred runtime values to their slots.  But the SETs are
    /// emitted INSIDE whichever path the emitter visits FIRST; the
    /// other path (the JUMP target) sees no SETs and leaves the
    /// deferred values un-consumed on the value stack.  At the merge
    /// point both paths must have the same stack depth — they don't,
    /// and subsequent OP_GET_LOCAL reads return Tag::Uninitialized.
    ///
    /// Repro pinned in test/repro-668-defer-across-branch.nix:
    ///   with { a = "x"; }; "_${if a == "z" then "y" else "n"}_"
    /// Bytecode dump showed `OP_SET_LOCAL 4` emitted inside the THEN
    /// branch (from the inner flushAllDeferred) but absent in ELSE; at
    /// runtime ELSE path's later `OP_GET_LOCAL 4` read the never-set
    /// slot, surfacing as `STR_CONCAT cannot coerce type to string
    /// (tag=0)` and a SIGTRAP exception cascade on go.drvPath.
    ///
    /// Fix: stash cond into a scratch slot, flush remaining pending
    /// (their SETs now run UNCONDITIONALLY before the branch dispatch),
    /// then restore cond on top.  When pending is already empty (the
    /// fib `if k < 2 then ... else ...` fast path), this is a no-op
    /// and the branch op runs directly on the deferred cond as before.
    void flushBelowBranchCond()
    {
        if (ctx->pendingDefer.empty()) return;
        uint16_t scratchSlot = ctx->nextSlot++;
        if (scratchSlot + 1 > ctx->nLocals) ctx->nLocals = scratchSlot + 1;
        unit.code.push_back(encode(OP_SET_LOCAL, scratchSlot));
        flushAllDeferred();
        unit.code.push_back(encode(OP_GET_LOCAL, scratchSlot));
    }

    /// Try to consume a binary-op's [lhs, rhs] from the pending stack
    /// top.  If pending ends with [lhs, rhs] in order, pop both and
    /// return true (caller emits just the OP).  Else return false
    /// (caller falls back to emitVarRef path).
    bool tryFastPathBinary(ir::VarId lhs, ir::VarId rhs)
    {
        auto & p = ctx->pendingDefer;
        if (p.size() < 2) return false;
        if (p[p.size() - 2] != lhs) return false;
        if (p.back() != rhs) return false;
        p.pop_back();
        p.pop_back();
        return true;
    }

    /// Push `v` to pendingDefer if it's safe to defer (OnceLinear).
    /// Called by emitBlock instead of emitting OP_SET_LOCAL.  For
    /// non-OnceLinear bindings this falls through to a real SET so
    /// the slot is populated for multiple GETs.
    /// Returns true iff deferred (caller skipped SET).
    bool tryDefer(ir::VarId var)
    {
        // NIX_V3_NO_DEFER=1: A/B switch.  Disables deferring entirely
        // so a regression can be bisected to "v3 emit deferring
        // optimisation" vs "everything else."
        static const bool disabled =
            std::getenv("NIX_V3_NO_DEFER") != nullptr;
        if (disabled) return false;

        // Only OnceLinear bindings are safe to defer: by definition a
        // single use exists and the consumer pops the value off the
        // runtime stack.  Many / OnceCaptured / Param: not safe (the
        // value must persist in a slot for multiple/cross-frame uses).
        if (occ.lookup(var).kind != ir::OccKind::OnceLinear)
            return false;
        ctx->pendingDefer.push_back(var);
        return true;
    }

    // Helpers ---------------------------------------------------------------

    uint16_t getOrAssignSlot(ir::VarId v)
    {
        auto it = ctx->slot.find(v);
        if (it != ctx->slot.end()) return it->second;
        uint16_t s = ctx->nextSlot++;
        ctx->slot[v] = s;
        if (s + 1 > ctx->nLocals) ctx->nLocals = s + 1;
        return s;
    }

    /// Emit OP_GET_LOCAL, recording a SET_LOCAL_KEEP fusion candidate when
    /// the previous instruction is a same-slot SET_LOCAL (reserved local).
    /// Elision is deferred to compactFuseSetGet() at function end, where
    /// the complete jump-target set is known.  See BYTECODE_NGRAM_ANALYSIS §7.
    ///
    /// NIX_V3_NO_FUSE_SETGET=1 disables the fusion (mirrors NIX_V3_NO_DEFER):
    /// a regression-bisection / A-B switch, default-ON.  RETIREMENT: drop the
    /// switch once the fusion is subsumed by a future register-VM operand-fold
    /// pass, or if it is ever shown net-negative on wall (revert the whole
    /// peephole, not just the switch).
    void emitGetLocal(uint16_t slot)
    {
        static const bool noFuse =
            std::getenv("NIX_V3_NO_FUSE_SETGET") != nullptr;
        if (!noFuse && !unit.code.empty()
            && unit.code.back() == encode(OP_SET_LOCAL, slot)
            && slot < ctx->nLocals)
        {
            fuseGetPositions_.push_back(
                static_cast<uint32_t>(unit.code.size()));
        }
        getLocalPositions_.push_back(static_cast<uint32_t>(unit.code.size()));
        unit.code.push_back(encode(OP_GET_LOCAL, slot));
    }

    void emitVarRef(ir::VarId v)
    {
        // #542 deferring discipline: emitVarRef ALWAYS flushes any
        // pending deferred values to their slots before emitting the
        // GET.  This is safe and simple — the alternative (consuming
        // top-of-pending without flushing) requires the immediate
        // next emit step to be an OP that pops the consumed value,
        // and getting the consume/non-consume invariant right at
        // every emitVarRef call site is fragile.
        //
        // The actual win from deferring comes via op-level fast
        // paths (tryFastPathBinary / tryFastPathUnary) which check
        // pending BEFORE calling emitVarRef.  When the fast path
        // fires, both/all operands are popped from pending and only
        // the OP itself is emitted — no GETs.  When it doesn't fire,
        // we degrade gracefully to the standard flush+GET pattern,
        // matching the no-deferring baseline.
        flushAllDeferred();
        // §2(a): an immediate-constant binding has no slot — re-emit the LIT
        // here instead of GET_LOCAL.  A LIT pushes exactly one value (like a
        // GET), so the post-flush stack discipline is unchanged.
        if (auto cit = constRemat_.find(v); cit != constRemat_.end()) {
            // A const-remat'd capture emits a LIT (no dispatched GET), but it is
            // still a capture (counted for Counter-3's denominator).
            if (__builtin_expect(emittingCaptures_, 0))
                V3_STATS_BUMP(totalCapturesEmitted, 1);
            emitExpr(*cit->second);
            return;
        }
        if (auto it = ctx->slot.find(v); it != ctx->slot.end()) {
            if (__builtin_expect(emittingCaptures_, 0)) {
                V3_STATS_BUMP(totalCapturesEmitted, 1);
                V3_STATS_BUMP(captureGetsEmitted, 1);
            } else V3_STATS_BUMP(bodyGetsEmitted, 1);
            emitGetLocal(it->second);   // Step-2 fusion: records SET;GET candidates
            return;
        }
        if (auto uit = ctx->upvalue.find(v); uit != ctx->upvalue.end()) {
            // A capture that resolves to an UPVALUE of the creating frame is pure
            // forwarding — the transitive re-copy the env-pointer chain kills.
            if (__builtin_expect(emittingCaptures_, 0)) {
                V3_STATS_BUMP(totalCapturesEmitted, 1);
                V3_STATS_BUMP(captureGetsEmitted, 1);
                V3_STATS_BUMP(fwdCapturesEmitted, 1);
            } else V3_STATS_BUMP(bodyGetsEmitted, 1);
            unit.code.push_back(encode(OP_GET_UPVALUE, uit->second));
            return;
        }
        throw std::runtime_error("v3 emit: unbound VarId " + std::to_string(v));
    }

    /// #542 unary fast path: if `operand` is at top of pendingDefer,
    /// pop it and return true (caller emits just the OP, no GET).
    bool tryFastPathUnary(ir::VarId operand)
    {
        auto & p = ctx->pendingDefer;
        if (p.empty() || p.back() != operand) return false;
        p.pop_back();
        return true;
    }

    /// Register VM Phase 1 (REGISTER_VM_DESIGN_2026-06-05): try to emit a
    /// 3-address `OP_R_PRIMOP2` for a binary PrimOpCall binding whose operands
    /// are slot-resident (or small-int immediates) and whose result goes to a
    /// slot — collapsing `GET a; GET b; CALL_PRIMOP; SET v` to one dispatch
    /// with no operand-stack traffic.  CONSERVATIVE for Phase 1: fires only
    /// when no value is deferred (pendingDefer empty → arg slots are
    /// materialised and there is no stack state to preserve), both args are
    /// local slots / remat small-int consts (not upvalues, not deferred), and
    /// the primop has no deep-force-list arg.  Returns true iff emitted (the
    /// caller then skips the normal expr-emit + SET for this binding).
    bool tryEmitRPrimop2(const ir::Binding & bd, int32_t dstOverride = -1)
    {
        static const bool s_noRegPrimop2 =
            std::getenv("NIX_V3_NO_REG_PRIMOP2") != nullptr;
        if (s_noRegPrimop2) return false;
        auto * pc = std::get_if<ir::PrimOpCall>(&bd.expr);
        if (!pc || pc->args.size() != 2) return false;
        if (!pc->primop || pc->primop->deepForceList != 0) return false;
        uint32_t desc[2];
        for (int k = 0; k < 2; ++k) {
            ir::VarId av = pc->args[k];
            if (auto cit = constRemat_.find(av); cit != constRemat_.end()) {
                auto * li = std::get_if<ir::LitInt>(cit->second);
                if (!li || li->value < -16384 || li->value > 16383) return false;
                desc[k] = 0x8000u | (static_cast<uint32_t>(li->value) & 0x7FFFu);
                continue;
            }
            auto sit = ctx->slot.find(av);
            if (sit == ctx->slot.end() || sit->second > 0x7FFFu) return false;
            // A slot arg that is currently DEFERRED lives on the operand
            // stack, not (yet) in its slot — reading the slot would be stale.
            // Exclude such args; OTHER pending values are fine (R_PRIMOP2 is
            // stack-neutral and doesn't disturb them).
            if (std::find(ctx->pendingDefer.begin(), ctx->pendingDefer.end(), av)
                != ctx->pendingDefer.end())
                return false;
            desc[k] = sit->second & 0x7FFFu;          // bit15 clear ⇒ slot
        }
        uint32_t dst = dstOverride >= 0
            ? static_cast<uint32_t>(dstOverride) : getOrAssignSlot(bd.var);
        unit.code.push_back(encode(OP_R_PRIMOP2, internPrimOp(pc->primop)));
        unit.code.push_back(dst);                  // full word, no 12-bit cap
        unit.code.push_back((desc[0] << 16) | desc[1]);
        return true;
    }

    /// Register VM Phase 5: fuse a non-tail `vC = App(fun,arg)` that is
    /// immediately consumed by an OnceLinear `vF = Force(vC)` into ONE
    /// OP_R_CALL writing vF's slot — dropping the arg GET, the result
    /// operand-stack round-trip, and the separate FORCE.  This is the call
    /// analogue of tryEmitRPrimop2.  Returns the number of bindings consumed
    /// (0 = no match, 2 = the App + its Force).  Gate NIX_V3_NO_R_CALL.
    ///
    /// Each operand must be either slot-resident or the deferred operand-stack
    /// TOP (at most one can be the top in the canonical `RecBindingSlotRef; App`
    /// shape).  All feasibility/cap checks run in a PEEK pass that emits
    /// nothing, so a bail is a clean no-op; only the COMMIT pass emits.
    ///
    /// Correctness: the callee is forced in-VM (R_CALL Phase 1) and the result
    /// is written UNFORCED to the slot (Nix-lazy, more TW-like than the eager
    /// FORCE it replaces); every real consumer (the strict primop / STR_CONCAT
    /// that the Force fed) re-forces on use, so the observable value is
    /// unchanged.  ≤4095 slots (12-bit R_CALL fields).
    template <class SpineHeadMap>
    size_t tryEmitRCall(size_t i, const ir::Block & b, size_t nBd,
                        bool tailLast, const SpineHeadMap & spineHead,
                        int32_t dstOverride = -1)
    {
        static const bool s_no = std::getenv("NIX_V3_NO_R_CALL") != nullptr;
        if (s_no) return 0;
        const auto & bd = b.bindings[i];
        auto * app = std::get_if<ir::App>(&bd.expr);
        if (!app) return 0;
        if (spineHead.count(bd.var)) return 0;         // spine heads use CALL_N
        if (i + 1 >= nBd) return 0;
        const auto & nxt = b.bindings[i + 1];
        auto * frc = std::get_if<ir::Force>(&nxt.expr);
        if (!frc || frc->thunk != bd.var) return 0;    // next must be Force(vC)
        if (occ.lookup(bd.var).kind != ir::OccKind::OnceLinear) return 0;
        if (constRemat_.count(app->fun) || constRemat_.count(app->arg))
            return 0;
        uint32_t dst = dstOverride >= 0
            ? static_cast<uint32_t>(dstOverride) : getOrAssignSlot(nxt.var);
        if (dst > 0xFFFu) return 0;
        // PEEK: classify each operand as resident or deferred-top, resolving
        // its slot WITHOUT emitting.  Returns false (caller bails) for a remat
        // const, an upvalue/unbound, a buried deferred value, or a >12-bit slot.
        auto & p = ctx->pendingDefer;
        auto classify = [&](ir::VarId v, bool & isTop, uint16_t & slot) -> bool {
            bool deferred = std::find(p.begin(), p.end(), v) != p.end();
            if (!deferred) {
                auto it = ctx->slot.find(v);
                if (it == ctx->slot.end()) return false;       // upvalue/unbound
                isTop = false; slot = it->second;
            } else {
                if (p.back() != v) return false;               // buried
                isTop = true; slot = getOrAssignSlot(v);        // idempotent assign
            }
            return slot <= 0xFFFu;
        };
        bool funTop = false, argTop = false;
        uint16_t funSlot = 0, argSlot = 0;
        if (!classify(app->fun, funTop, funSlot)) return 0;
        if (!classify(app->arg, argTop, argSlot)) return 0;
        if (funTop && argTop) return 0;        // can't both be the single top
        // COMMIT: pop the deferred-top operand (if any) into its slot, then
        // emit the call.  At most one operand is on the stack top, so the
        // single SET_LOCAL is order-independent.
        if (funTop || argTop) {
            p.pop_back();
            unit.code.push_back(encode(OP_SET_LOCAL, funTop ? funSlot : argSlot));
        }
        unit.code.push_back(encode(OP_R_CALL, dst));
        unit.code.push_back((static_cast<uint32_t>(funSlot) << 12) | argSlot);
        // If the Force binding is the block tail, leave its value on the
        // operand stack for RETURN (mirrors tryEmitRPrimop2); the R_RETURN
        // peephole then fuses `GET_LOCAL dst; RETURN` → `R_RETURN dst`.  With a
        // dstOverride (register-mode If) the result is already in the merge
        // slot — leave nothing on the stack.
        if (tailLast && i + 2 == nBd && dstOverride < 0) emitGetLocal(dst);
        return 2;
    }

    /// Register VM Phase 5: a 2-part ConcatStrings binding (`a + b`) whose
    /// operands are slot-resident and whose result goes to a slot → one
    /// OP_R_STR_CONCAT2 (no GET_LOCAL2 + STR_CONCAT operand-stack round-trip).
    /// `dstOverride >= 0` writes that slot instead of the binding's own (used by
    /// register-mode If to land the branch result directly in the merge slot).
    /// Gate NIX_V3_NO_R_STRCONCAT2.
    bool tryEmitRStrConcat2(const ir::Binding & bd, int32_t dstOverride = -1)
    {
        static const bool s_no =
            std::getenv("NIX_V3_NO_R_STRCONCAT2") != nullptr;
        if (s_no) return false;
        auto * cs = std::get_if<ir::ConcatStrings>(&bd.expr);
        if (!cs || cs->parts.size() != 2) return false;
        uint16_t slots[2];
        for (int k = 0; k < 2; ++k) {
            ir::VarId v = cs->parts[k];
            if (constRemat_.count(v)) return false;       // const has no slot
            auto it = ctx->slot.find(v);
            if (it == ctx->slot.end() || it->second > 0xFFFu) return false;
            // A deferred operand lives on the stack, not its slot — exclude
            // (reading the slot would be stale), mirroring tryEmitRPrimop2.
            if (std::find(ctx->pendingDefer.begin(), ctx->pendingDefer.end(), v)
                != ctx->pendingDefer.end())
                return false;
            slots[k] = it->second;
        }
        uint32_t dst = dstOverride >= 0
            ? static_cast<uint32_t>(dstOverride) : getOrAssignSlot(bd.var);
        if (dst > 0xFFFFFFu) return false;        // dst is the 24-bit operand
        unit.code.push_back(encode(OP_R_STR_CONCAT2, dst));
        unit.code.push_back((cs->forceString ? (1u << 24) : 0u)
                            | (static_cast<uint32_t>(slots[0]) << 12) | slots[1]);
        return true;
    }

    /// Register VM Phase 5 (item 5a): for a RecBindingSlotRef binding whose var
    /// is used as a call callee (the fib self-resolution shape) and whose source
    /// is a captured rec-attrset upvalue, resolve straight into the binding's
    /// slot with OP_GET_UPVALUE_REC_BINDING_SLOT — dropping the GET_UPVALUE
    /// push + the materialising SET that tryEmitRCall would otherwise emit.
    /// Gate NIX_V3_NO_RBSR_SLOT.  Returns true iff emitted (caller skips the
    /// generic emit + defer/SET; the value is now slot-resident).
    bool tryEmitRecBindToSlot(const ir::Binding & bd,
                              const std::unordered_set<ir::VarId> & appFunVars)
    {
        static const bool s_no = std::getenv("NIX_V3_NO_RBSR_SLOT") != nullptr;
        if (s_no) return false;
        auto * e = std::get_if<ir::RecBindingSlotRef>(&bd.expr);
        if (!e) return false;
        if (!appFunVars.count(bd.var)) return false;       // only call-callee uses
        // Source must be a captured upvalue (the GET_UPVALUE_REC_BINDING form):
        // not the deferred stack top, not a local slot.
        if (!ctx->pendingDefer.empty() && ctx->pendingDefer.back() == e->attrs)
            return false;
        if (ctx->slot.find(e->attrs) != ctx->slot.end()) return false;
        auto uit = ctx->upvalue.find(e->attrs);
        if (uit == ctx->upvalue.end()) return false;
        uint32_t dst = getOrAssignSlot(bd.var);
        flushAllDeferred();                                // match emitVarRef discipline
        uint32_t icIdx = static_cast<uint32_t>(unit.rt.recSlotCache.size());
        unit.rt.recSlotCache.emplace_back();
        unit.code.push_back(encode(OP_GET_UPVALUE_REC_BINDING_SLOT, e->name));
        unit.code.push_back(dst);            // dst slot (process-independent)
        unit.code.push_back(uit->second);    // upvalIdx
        unit.code.push_back(icIdx);
        return true;
    }

    /// Register VM Phase 5: emit `bid` so its terminal value lands in slot `R`
    /// (no operand-stack value left).  Used per branch by register-mode If.
    void emitBlockToSlot(ir::BlockId bid, uint16_t R)
    {
        int32_t saved = blockResultSlot_;
        blockResultSlot_ = static_cast<int32_t>(R);
        emitBlock(bid);
        blockResultSlot_ = saved;
    }

    /// Register VM Phase 5: register-mode If — branch-result-to-slot.  When the
    /// If binding's cond is materialised in a slot (R_BRANCH_FALSE eligible),
    /// emit each branch so its terminal lands in the If's result slot R, with
    /// the merge holding the value in R and NOTHING on the operand stack — so a
    /// tail If becomes `… ; GET R ; RETURN` → `R_RETURN R` and the function
    /// runs with the operand stack dropped.  `resultHint >= 0` (this If is
    /// itself a branch tail) writes that slot directly instead of the binding's
    /// own.  Gate NIX_V3_NO_R_IF.  Returns true iff emitted.
    bool tryEmitRegisterIf(const ir::Binding & bd, bool isTail, int32_t resultHint)
    {
        static const bool s_no = std::getenv("NIX_V3_NO_R_IF") != nullptr;
        if (s_no) return false;
        auto * e = std::get_if<ir::If>(&bd.expr);
        if (!e) return false;
        // cond must be slot-resident + not deferred (same gate as the
        // R_BRANCH_FALSE path in emitOne(If)).
        auto sit = ctx->slot.find(e->cond);
        if (sit == ctx->slot.end() || sit->second > 0xFFFFFFu) return false;
        if (std::find(ctx->pendingDefer.begin(), ctx->pendingDefer.end(),
                      e->cond) != ctx->pendingDefer.end())
            return false;
        uint32_t R = resultHint >= 0
            ? static_cast<uint32_t>(resultHint) : getOrAssignSlot(bd.var);
        if (R > 0xFFFu) return false;   // R_MOVE / merge fields are 12-bit
        flushAllDeferred();
        uint32_t bf = emitJumpPlaceholder(OP_R_BRANCH_FALSE);
        unit.code.push_back(sit->second);              // cond_slot follow-up
        emitBlockToSlot(e->thenBlock, static_cast<uint16_t>(R));
        uint32_t je = emitJumpPlaceholder(OP_JUMP);
        patchJump(bf, static_cast<uint32_t>(unit.code.size()));
        emitBlockToSlot(e->elseBlock, static_cast<uint16_t>(R));
        patchJump(je, static_cast<uint32_t>(unit.code.size()));
        // R holds the result.  resultHint>=0 → leave it in the parent's merge
        // slot (caller marks tailToResult).  Else for a tail If, GET R so the
        // R_RETURN peephole fuses `GET R; RETURN`.  Else (non-tail) R is the
        // binding's own slot and later uses GET it.
        if (resultHint < 0 && isTail) emitGetLocal(static_cast<uint16_t>(R));
        return true;
    }

    uint32_t addIntConst(int64_t n)
    {
        unit.intConstants.push_back(n);
        return static_cast<uint32_t>(unit.intConstants.size() - 1);
    }
    uint32_t addFloatConst(double d)
    {
        unit.floatConstants.push_back(d);
        return static_cast<uint32_t>(unit.floatConstants.size() - 1);
    }
    uint32_t addStringConst(std::string_view s)
    {
        // M-10 (CODEBASE_REVIEW_2026-06-11): intern against the process-wide
        // pool so literals recurring across CUs are stored once.
        unit.stringConstants.push_back(internStringConstant(s));
        return static_cast<uint32_t>(unit.stringConstants.size() - 1);
    }

    // Patch helpers ---------------------------------------------------------

    /// Emit a placeholder jump and return the index of the operand word
    /// (so the caller can patch in the absolute target later).
    uint32_t emitJumpPlaceholder(Op op)
    {
        uint32_t at = static_cast<uint32_t>(unit.code.size());
        unit.code.push_back(encode(op, 0));
        jumpInsnPositions_.push_back(at);   // Step-2 fusion: for operand re-base
        return at;
    }

    void patchJump(uint32_t at, uint32_t target)
    {
        // Preserve the opcode byte; replace the 24-bit operand.
        Instruction prev = unit.code[at];
        unit.code[at] = (prev & 0xFF000000u) | (target & 0x00FFFFFFu);
        jumpTargetPositions_.insert(target);  // Step-2 fusion: never elide a GET here
    }

    /// Record `(bytecode-offset, site-string)` for a force-flavoured
    /// opcode that is about to be appended to `unit.code` at the
    /// current end-of-stream offset.  Caller passes the site string
    /// (already interned to a process-stable c-string).  Entries are
    /// always appended in monotonically increasing offset order.
    void recordForceSite(const char * site)
    {
        uint32_t off = static_cast<uint32_t>(unit.code.size());
        unit.forceEmitSites.emplace_back(off, site);
    }

    /// Append OP_FORCE plus a side-table entry for the lower.cc line
    /// that synthesised the IR Force.  `srcLine == 0` means the IR
    /// node carried no annotation (e.g., the smoke-test build it
    /// directly), so we attribute it to "lower.cc:?".
    void emitForceFromIR(int srcLine)
    {
        const char * site = srcLine
            ? internLowerCcSiteString(srcLine)
            : internEmitSiteString("lower.cc:?");
        recordForceSite(site);
        unit.code.push_back(encode(OP_FORCE));
    }

    /// Check if `srcLine` is in the per-line force-skip list.  If
    /// `NIX_V3_SKIP_FORCE_LINES=305,805,984` is set, forceVal calls
    /// emitted from those lower.cc lines compile to non-forcing
    /// loads (OP_GET_LOCAL / OP_GET_UPVALUE) — used to bisect which
    /// emit-site causes WC-38 without breaking other lang tests.
    static bool skipForceAtLine(int srcLine)
    {
        if (srcLine == 0) return false;
        static const auto & skipSet = []() -> const std::set<int> & {
            static std::set<int> s;
            const char * env = std::getenv("NIX_V3_SKIP_FORCE_LINES");
            if (env) {
                std::string str(env);
                size_t pos = 0;
                while (pos < str.size()) {
                    size_t comma = str.find(',', pos);
                    std::string tok = str.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
                    if (!tok.empty()) {
                        int n = std::atoi(tok.c_str());
                        if (n > 0) s.insert(n);
                    }
                    if (comma == std::string::npos) break;
                    pos = comma + 1;
                }
            }
            return s;
        }();
        return skipSet.count(srcLine) > 0;
    }

    /// Append a fused OP_GET_LOCAL_FORCE / OP_GET_UPVALUE_FORCE while
    /// recording the eventual force back to its lower.cc line.  The
    /// recorded offset is the offset of the fused superinstruction
    /// itself — the runtime trace path keys on whichever instruction
    /// `ip - 1` points at.
    void emitGetLocalForceFromIR(uint16_t slot, int srcLine)
    {
        const char * site = srcLine
            ? internLowerCcSiteString(srcLine)
            : internEmitSiteString("lower.cc:?");
        recordForceSite(site);
        // WC-38 bisection: NIX_V3_SKIP_FORCE_LINES=N1,N2,... compiles
        // forces at those lower.cc lines as non-forcing loads.
        if (skipForceAtLine(srcLine))
            unit.code.push_back(encode(OP_GET_LOCAL, slot));
        else
            unit.code.push_back(encode(OP_GET_LOCAL_FORCE, slot));
    }
    void emitGetUpvalueForceFromIR(uint16_t idx, int srcLine)
    {
        const char * site = srcLine
            ? internLowerCcSiteString(srcLine)
            : internEmitSiteString("lower.cc:?");
        recordForceSite(site);
        if (skipForceAtLine(srcLine))
            unit.code.push_back(encode(OP_GET_UPVALUE, idx));
        else
            unit.code.push_back(encode(OP_GET_UPVALUE_FORCE, idx));
    }

    // Block emit ------------------------------------------------------------

    /// Emit a Block in-line within the current function.  After emission,
    /// the Block's TermReturn value is on top of the operand stack.
    void emitBlock(ir::BlockId bid)
    {
        const ir::Block & b = m.blocks[bid];
        const auto & ret = std::get<ir::TermReturn>(b.terminal);

        // #542: at every emitBlock entry, flushAllDeferred().  Any
        // pending entries from outside this block (the function's
        // entry block has none; sub-blocks may inherit outer pending)
        // get committed to slots BEFORE this block's bindings emit.
        // After the flush the runtime stack has no deferred values;
        // sub-block bindings build their own pending state from
        // scratch and clean it up before exit.
        //
        // This is the load-bearing safety property: emitBlock always
        // runs with pending=[] at entry and exits with pending=[].
        flushAllDeferred();

        // Register VM Phase 5: capture (and clear) the result-slot for THIS
        // block — when ≥0 the block's terminal value must land in `myResult`
        // (the enclosing If's merge slot), not on the operand stack.  Cleared
        // so nested blocks don't inherit it (each branch sets its own via
        // emitBlockToSlot).  `tailToResult` records that the tail binding's
        // register op already wrote myResult (so the terminal does nothing).
        int32_t myResult = blockResultSlot_;
        blockResultSlot_ = -1;
        bool tailToResult = false;

        // Optimisation: when the very last binding's VarId is the
        // block's TermReturn value, the binding's expression result
        // is already on top of the operand stack right after we
        // emit it.  Skip the SET + trailing GET round-trip.  Combined
        // with #542 deferring, we additionally need to flush any
        // mid-block pending BEFORE the tail binding emits (so the
        // tail's value sits cleanly on top with no deferred values
        // beneath it that would conflict with sub-block-exit
        // invariants).
        const size_t nBd = b.bindings.size();
        // §2(a): a const-remat var carries no slot and emits nothing here, so
        // it can't be the "value already on top" tail — exclude it so the
        // terminal below rematerialises ret.value via emitVarRef instead.
        bool tailLast = nBd > 0 && b.bindings.back().var == ret.value
                        && !constRemat_.count(b.bindings.back().var);

        // --- App-spine coalescing (eval/apply call-site, lever B) --------
        // Recognise maximal application spines App(App(…App(base,a0)…),
        // a_{n-1}) whose intermediate Apps are OnceLinear (their PAP result
        // is used ONLY by the next App in the spine) and emit ONE saturated
        // OP_CALL_N(n) instead of n curried OP_CALLs — eliminating one PAP
        // (Tag::App) pair per inner application, the dominant per-iteration
        // allocation in every bytecode fold/loop.  Inner nodes are skipped;
        // the head binding emits the whole spine via emitVarRef (base then
        // args), which OP_CALL_N pops in order.  NIX_V3_NO_CALL_N=1 disables
        // it (bisect handle; retire once shipped byte-identical on --core +
        // a nixpkgs sample across ≥10 runs).
        static const bool s_noCallN = std::getenv("NIX_V3_NO_CALL_N") != nullptr;
        std::unordered_map<ir::VarId, const ir::App *> appOf;
        std::unordered_set<ir::VarId> spineInner;
        std::unordered_map<ir::VarId,
            std::pair<ir::VarId, std::vector<ir::VarId>>> spineHead;

        // Register VM Phase 5 (item 5a): VarIds used as some App's callee.  A
        // RecBindingSlotRef bound to one of these is a call-callee candidate
        // (the fib self-resolution shape) → resolve it straight into its slot
        // with OP_GET_UPVALUE_REC_BINDING_SLOT (dropping the push + SET).
        std::unordered_set<ir::VarId> appFunVars;
        for (auto & bd : b.bindings)
            if (auto * a = std::get_if<ir::App>(&bd.expr))
                appFunVars.insert(a->fun);
        if (!s_noCallN) {
            for (auto & bd : b.bindings)
                if (auto * a = std::get_if<ir::App>(&bd.expr))
                    appOf.emplace(bd.var, a);
            // v is an inner spine node iff it is an App binding that is
            // OnceLinear (single use) — that single use being as some App's
            // .fun is what makes it part of a spine; we confirm the fun-use
            // by only ever reaching it while walking down from a head.
            auto innerApp = [&](ir::VarId v) -> bool {
                auto it = appOf.find(v);
                return it != appOf.end()
                    && occ.lookup(v).kind == ir::OccKind::OnceLinear;
            };
            for (auto & bd : b.bindings) {
                auto * a = std::get_if<ir::App>(&bd.expr);
                if (!a) continue;
                // A coalescable HEAD is an App whose .fun is itself an inner
                // App node (so the spine has ≥ 2 args).  A head that is ALSO
                // some longer spine's inner node is subsumed: it is reached
                // (and recorded in spineInner) while walking the outer head,
                // so by the time the loop emits it we skip it.
                if (!innerApp(a->fun)) continue;
                std::vector<ir::VarId> revArgs;
                std::vector<ir::VarId> walkedInner;
                const ir::App * cur = a;
                ir::VarId base = ir::kInvalid;
                while (true) {
                    revArgs.push_back(cur->arg);
                    ir::VarId f = cur->fun;
                    if (innerApp(f)) {
                        walkedInner.push_back(f);
                        cur = appOf[f];
                    } else { base = f; break; }
                }
                // Cap at the runtime argbuf (16); skip pathological spines.
                if (revArgs.size() < 2 || revArgs.size() > 16) continue;
                std::vector<ir::VarId> args(revArgs.rbegin(), revArgs.rend());
                spineHead.emplace(bd.var,
                    std::make_pair(base, std::move(args)));
                for (ir::VarId iv : walkedInner) spineInner.insert(iv);
            }
        }

        for (size_t i = 0; i < nBd; ++i) {
            auto & bd = b.bindings[i];
            bool isTail = tailLast && (i + 1 == nBd);

            // Subsumed inner spine node: the owning head emits it.  (An inner
            // node is OnceLinear and never the block's tail, so skipping it
            // affects neither slots nor the return value.)
            if (spineInner.count(bd.var)) continue;

            // §2(a): immediate-constant bindings are rematerialised at each
            // use (emitVarRef) and never spilled — emit nothing for them
            // here.  Excluded from tailLast above, so a const ret.value is
            // re-emitted by the terminal.  (Consts are Lits, never Apps, so
            // they never collide with the spine head/inner logic.)
            if (constRemat_.count(bd.var)) continue;

            // Register VM Phase 1 + 5: a binary primop with slot/const
            // operands and a slot result → one 3-address OP_R_PRIMOP2 (no
            // stack traffic; writes slot v).  For the TAIL binding, follow
            // with GET_LOCAL v so the value is on the operand stack for the
            // function RETURN — the Phase-5 R_RETURN peephole then fuses
            // `GET_LOCAL v; RETURN` → `R_RETURN v`, so a straight-line
            // arithmetic function (e.g. `map (x: x - 1)`'s lambda) runs with
            // ZERO operand-stack traffic (R_PRIMOP2; R_RETURN).
            // Register VM Phase 5: register-mode If (branch-result-to-slot).
            // When this If binding is the block tail (or a nested branch tail),
            // emit each branch to the result slot so the merge holds the value
            // in a register with nothing on the operand stack.
            if (tryEmitRegisterIf(bd, isTail,
                                  (isTail && myResult >= 0) ? myResult : -1)) {
                if (isTail && myResult >= 0) tailToResult = true;
                continue;
            }

            // Register VM Phase 5 (item 5a): a RecBindingSlotRef used as a call
            // callee resolves straight into its slot (no push + SET).  Value is
            // now slot-resident; skip defer/SET.  (A callee var is never the
            // block's return value, so no tail handling is needed.)
            if (tryEmitRecBindToSlot(bd, appFunVars))
                continue;

            // For a tail binding inside a register-mode If branch, the register
            // ops write the merge slot (`myResult`) directly — no GET_LOCAL,
            // nothing left on the stack (terminal then does nothing).
            int32_t tailDst = (isTail && myResult >= 0) ? myResult : -1;

            if (tryEmitRPrimop2(bd, tailDst)) {
                if (isTail) {
                    if (tailDst >= 0) tailToResult = true;
                    else emitGetLocal(getOrAssignSlot(bd.var));
                }
                continue;
            }

            // Register VM Phase 5: fuse `App(fun,arg); Force(...)` → OP_R_CALL
            // writing the Force's slot (drops arg GET + result round-trip +
            // FORCE).  Consumes 2 bindings; advance `i` past the Force.
            if (size_t consumed =
                    tryEmitRCall(i, b, nBd, tailLast, spineHead, tailDst)) {
                if (tailDst >= 0) tailToResult = true;
                i += consumed - 1;     // skip the Force; loop ++i moves past App
                continue;
            }

            // Register VM Phase 5: `a + b` (2-part ConcatStrings) with slot
            // operands → OP_R_STR_CONCAT2 writing a slot.  For the tail binding
            // emit GET_LOCAL after so the R_RETURN peephole can fuse it (a
            // straight-line concat tail then runs stack-free).
            if (tryEmitRStrConcat2(bd, tailDst)) {
                if (isTail) {
                    if (tailDst >= 0) tailToResult = true;
                    else emitGetLocal(getOrAssignSlot(bd.var));
                }
                continue;
            }

            if (auto sh = spineHead.find(bd.var); sh != spineHead.end()) {
                emitVarRef(sh->second.first);                  // base
                for (ir::VarId av : sh->second.second)         // a0..a_{n-1}
                    emitVarRef(av);
                unit.code.push_back(encode(OP_CALL_N,
                    static_cast<uint32_t>(sh->second.second.size())));
            } else {
                emitExpr(bd.expr);
            }

            if (isTail) {
                // #542: AFTER tail's emit, pending may still contain
                // entries that were deferred by earlier bindings AND
                // not consumed by tail's emit.  Their runtime values
                // sit BELOW tail's value on the stack.  We must flush
                // them so emitBlock exits with pending=[] and the
                // tail's value cleanly on top.  Use the binding's
                // slot as a scratch: SET tail_slot; flush; GET
                // tail_slot.  When pending is empty (binary fast
                // path consumed everything — the common case for
                // fib's `Less(force(k), 2)` shape), this branch is
                // skipped and we save the 3-op overhead.
                if (!ctx->pendingDefer.empty()) {
                    uint16_t slot = getOrAssignSlot(bd.var);
                    unit.code.push_back(encode(OP_SET_LOCAL, slot));
                    flushAllDeferred();
                    unit.code.push_back(encode(OP_GET_LOCAL, slot));
                }
                continue;
            }

            // #542: try to defer this binding's SET if its var is
            // OnceLinear.  Subsequent emitOne calls may consume it
            // via fast paths (binary/unary) or via emitVarRef-with-
            // top-match.  If not OnceLinear, fall back to the real
            // SET so the slot is populated for multiple GETs.
            if (!tryDefer(bd.var)) {
                uint16_t slot = getOrAssignSlot(bd.var);
                unit.code.push_back(encode(OP_SET_LOCAL, slot));
            }
        }

        // Register VM Phase 5: when this block must deliver its terminal to a
        // slot (`myResult` ≥ 0, an If branch), land the value in that slot with
        // nothing left on the operand stack.
        if (myResult >= 0) {
            uint16_t R = static_cast<uint16_t>(myResult);
            if (tailToResult) {
                // A tail register op (R_PRIMOP2 / R_CALL / R_STR_CONCAT2) or a
                // nested register-If already wrote R — nothing to do.
            } else if (tailLast) {
                // The tail binding left its value on the operand stack (a
                // non-register op) → commit it to R.
                unit.code.push_back(encode(OP_SET_LOCAL, R));
            } else if (ret.value != ir::kInvalid) {
                // A var/const terminal (e.g. `then: n`): move it into R.  A
                // slot-resident, non-deferred var uses R_MOVE (stack-free);
                // otherwise fall back to GET/emit + SET.
                auto it = ctx->slot.find(ret.value);
                bool resident = it != ctx->slot.end()
                    && it->second <= 0xFFFu
                    && !constRemat_.count(ret.value)
                    && std::find(ctx->pendingDefer.begin(),
                                 ctx->pendingDefer.end(), ret.value)
                           == ctx->pendingDefer.end();
                if (resident) {
                    unit.code.push_back(encode(OP_R_MOVE,
                        (static_cast<uint32_t>(R) << 12) | it->second));
                } else {
                    emitVarRef(ret.value);
                    unit.code.push_back(encode(OP_SET_LOCAL, R));
                }
            } else {
                unit.code.push_back(encode(OP_LIT_NULL));
                unit.code.push_back(encode(OP_SET_LOCAL, R));
            }
            return;   // pending is empty (flushed); nothing on the stack
        }

        // Terminal: TermReturn.  If tailLast, the value is already on
        // top — nothing to emit.  Else: emit the value via emitVarRef
        // (which flushes any remaining pending and emits GET).
        if (ret.value != ir::kInvalid && !tailLast)
            emitVarRef(ret.value);
        else if (ret.value == ir::kInvalid)
            unit.code.push_back(encode(OP_LIT_NULL));

        // Invariant: pending is empty here.  emitVarRef flushes; tail
        // path doesn't push to pending.  Mid-block pending was flushed
        // before tail emitted.
    }

    // Expr emit -------------------------------------------------------------

    void emitExpr(const ir::Expr & expr)
    {
        std::visit([&](auto const & e) { emitOne(e); }, expr);
    }

    // -- Literals
    void emitOne(const ir::LitInt & e)
    {
        if (e.value >= -(1 << 23) && e.value < (1 << 23)) {
            unit.code.push_back(encode(OP_LIT_INT, static_cast<uint32_t>(e.value) & 0x00FFFFFF));
        } else {
            unit.code.push_back(encode(OP_LIT_INT_BIG, addIntConst(e.value)));
        }
    }
    void emitOne(const ir::LitFloat & e)
    {
        unit.code.push_back(encode(OP_LIT_FLOAT, addFloatConst(e.value)));
    }
    void emitOne(const ir::LitBool & e)
    {
        unit.code.push_back(encode(e.value ? OP_LIT_TRUE : OP_LIT_FALSE));
    }
    void emitOne(const ir::LitNull &) { unit.code.push_back(encode(OP_LIT_NULL)); }
    void emitOne(const ir::LitString & e)
    {
        unit.code.push_back(encode(OP_LIT_STR, addStringConst(e.value)));
    }
    void emitOne(const ir::LitPath & e)
    {
        // For now: store path string in stringConstants; accessor table TBD.
        unit.code.push_back(encode(OP_LIT_PATH, addStringConst(e.path)));
    }
    // -- Variable / scoping
    void emitOne(const ir::VarRef & e) { emitVarRef(e.var); }
    void emitOne(const ir::WithLookup & e)
    {
        unit.code.push_back(encode(OP_WITH_LOOKUP, e.name));
    }

    // -- Functions
    void emitOne(const ir::Lambda & e)
    {
        // #530 lexical-with chain — push with-target VarIds FIRST so
        // they sit BELOW the upvalue block on the value stack.
        // OP_MAKE_CLOSURE pops nUpvalues then nWithTargets in that
        // order (top-down).
        emittingCaptures_ = true;
        for (auto wv : e.lexicalWiths) emitVarRef(wv);
        uint32_t nUp = 0;
        for (auto fv : e.freeVars) {
            emitVarRef(fv); ++nUp;
        }
        emittingCaptures_ = false;
        unit.code.push_back(encode(OP_MAKE_CLOSURE, e.funcIdx));
        unit.code.push_back(nUp);
        unit.code.push_back(static_cast<uint32_t>(e.lexicalWiths.size()));
        // Mirror count into LambdaDescriptor::nWithTargets.  This
        // function emit may run before the descriptor is built (the
        // function-emit loop populates descriptors in a later pass);
        // we populate the descriptor separately in `compile()` from
        // `Function::lexicalWiths` after free-var convergence.  Here
        // we only encode the count into the bytecode stream.
    }
    void emitOne(const ir::MkThunk & e)
    {
        // Same push order as ir::Lambda — see comment there.
        emittingCaptures_ = true;
        for (auto wv : e.lexicalWiths) emitVarRef(wv);
        uint32_t nUp = 0;
        for (auto fv : e.freeVars) {
            emitVarRef(fv); ++nUp;
        }
        emittingCaptures_ = false;
        // P2.1-a (NIX_V3_RAW_FORMALS, default-off): prefix a no-default demoted
        // formal wrapper's MkThunk with OP_RAW_FORMAL(formalSym), so the runtime
        // raw-binds the formal (plain arg) instead of allocating the wrapper.
        // Guarded to nUp==1 (a `param.X` wrapper's only free var is param); the
        // captured lexical withs (nWiths, common under `with lib;`) are DEAD for
        // a `param.X` body (no with-lookup), so the handler drops them on the
        // raw path — no nWiths guard needed.
        static const bool s_rawFormals =
            std::getenv("NIX_V3_RAW_FORMALS") != nullptr;
        if (__builtin_expect(s_rawFormals, 0)
            && e.funcIdx < m.functions.size()
            && m.functions[e.funcIdx].rawFormalEligible
            && e.freeVars.size() == 1) {
            unit.code.push_back(encode(OP_RAW_FORMAL,
                m.functions[e.funcIdx].formalSym));
        }
        unit.code.push_back(encode(OP_MAKE_THUNK, e.funcIdx));
        unit.code.push_back(nUp);
        unit.code.push_back(static_cast<uint32_t>(e.lexicalWiths.size()));
    }
    void emitOne(const ir::App & e)
    {
        // #542: fast path when [fun, arg] are both OnceLinear and
        // pending in order.  Saves the SET+GET on each.
        if (tryFastPathBinary(e.fun, e.arg)) {
            unit.code.push_back(encode(OP_CALL));
            return;
        }
        emitVarRef(e.fun); emitVarRef(e.arg);
        unit.code.push_back(encode(OP_CALL));
    }
    void emitOne(const ir::Force & e)
    {
        // #542 unary fast path: if e.thunk's value is already on top
        // of the runtime stack (its binding's SET was deferred), skip
        // the GET entirely — emit just OP_FORCE which pops top, forces,
        // pushes.  Saves the SET (deferred) + GET (we don't emit) =
        // 2 ops, AND collapses to a single dispatch even though we
        // still go through OP_FORCE rather than the
        // GET_LOCAL_FORCE / GET_UPVALUE_FORCE superinstruction.
        if (tryFastPathUnary(e.thunk)) {
            emitForceFromIR(e.srcLine);
            return;
        }

        // Otherwise fall through to the existing superinstruction
        // path.  emitGet*ForceFromIR fuses `Force(VarRef)` into a
        // single opcode: every variable reference in the AST→IR
        // lowering goes through this path, so this is the most
        // common bytecode pair (~25-40% of instructions on
        // benchmarks like fib).
        //
        // The superinstruction emit doesn't go through emitVarRef,
        // so flushAllDeferred isn't called automatically — call
        // explicitly to commit any pending bindings to slots before
        // we read from the slot.
        flushAllDeferred();
        if (auto it = ctx->slot.find(e.thunk); it != ctx->slot.end()) {
            emitGetLocalForceFromIR(it->second, e.srcLine);
            return;
        }
        if (auto uit = ctx->upvalue.find(e.thunk); uit != ctx->upvalue.end()) {
            emitGetUpvalueForceFromIR(uit->second, e.srcLine);
            return;
        }
        // Fallback: var was neither slot nor upvalue (shouldn't happen
        // for a well-formed module; emitVarRef will throw).
        emitVarRef(e.thunk);
        emitForceFromIR(e.srcLine);
    }

    // -- Arithmetic / comparison / logical
    //
    // #542 binary fast path: when pendingDefer ends with [lhs, rhs]
    // — i.e., both operands' bindings were OnceLinear and emitted
    // immediately before this binary op — pop both from pending and
    // emit just the OP.  Saves the 2 SETs (deferred) plus 2 GETs
    // (we don't emit) — 4 ops per binary op when both operands are
    // OnceLinear and adjacent.
    //
    // For fib's body `Less(force(k), 2)`, the IR has bindings
    // T_force_k = Force(k); T_lit2 = LitInt 2; T_less = Less(T_force_k,
    // T_lit2).  All OnceLinear.  Pending = [T_force_k, T_lit2] when
    // T_less.emit fires; fast path matches; emit OP_LESS; pending =
    // [T_less] (deferred for the next consumer, the If).
#define V3_EMIT_BINARY(IRType, OP) \
    void emitOne(const ir::IRType & e) { \
        if (tryFastPathBinary(e.lhs, e.rhs)) { \
            unit.code.push_back(encode(OP)); \
            return; \
        } \
        /* §2(a): cooperate with const-remat.  When lhs is deferred (pending */ \
        /* top) and rhs is a remat-const, the two-operand fast path above */ \
        /* can't fire (the const was never deferred), which would force lhs */ \
        /* to flush+reload.  Instead consume lhs from pending and emit the */ \
        /* const inline — preserving the no-SET shape `<lhs>; LIT; OP`. */ \
        if (auto cit = constRemat_.find(e.rhs); \
            cit != constRemat_.end() && tryFastPathUnary(e.lhs)) { \
            emitExpr(*cit->second); \
            unit.code.push_back(encode(OP)); \
            return; \
        } \
        emitVarRef(e.lhs); \
        emitVarRef(e.rhs); \
        unit.code.push_back(encode(OP)); \
    }
    V3_EMIT_BINARY(Add,  OP_ADD)
    V3_EMIT_BINARY(Sub,  OP_SUB)
    V3_EMIT_BINARY(Mul,  OP_MUL)
    V3_EMIT_BINARY(Div,  OP_DIV)
    V3_EMIT_BINARY(Eq,   OP_EQ)
    V3_EMIT_BINARY(NEq,  OP_NEQ)
    V3_EMIT_BINARY(Less, OP_LESS)
#undef V3_EMIT_BINARY

    void emitOne(const ir::Not & e) {
        if (tryFastPathUnary(e.operand)) {
            unit.code.push_back(encode(OP_NOT));
            return;
        }
        emitVarRef(e.operand);
        unit.code.push_back(encode(OP_NOT));
    }

    // -- Short-circuit
    void emitOne(const ir::And & e)
    {
        // #542 unary fast path: lhs may be deferred at top of pending.
        if (!tryFastPathUnary(e.lhs))
            emitVarRef(e.lhs);
        // #668: flush deferred values below the cond (see comment on
        // flushBelowBranchCond).  Both OP_AND_BRANCH paths must see
        // identical stack depth at the merge.
        flushBelowBranchCond();
        uint32_t at = emitJumpPlaceholder(OP_AND_BRANCH);
        emitBlock(e.rhsBlock);
        patchJump(at, static_cast<uint32_t>(unit.code.size()));
    }
    void emitOne(const ir::Or & e)
    {
        if (!tryFastPathUnary(e.lhs))
            emitVarRef(e.lhs);
        flushBelowBranchCond();  // #668
        uint32_t at = emitJumpPlaceholder(OP_OR_BRANCH);
        emitBlock(e.rhsBlock);
        patchJump(at, static_cast<uint32_t>(unit.code.size()));
    }
    void emitOne(const ir::Impl & e)
    {
        if (!tryFastPathUnary(e.lhs))
            emitVarRef(e.lhs);
        flushBelowBranchCond();  // #668
        uint32_t at = emitJumpPlaceholder(OP_IMPL_BRANCH);
        emitBlock(e.rhsBlock);
        patchJump(at, static_cast<uint32_t>(unit.code.size()));
    }

    // -- If
    void emitOne(const ir::If & e)
    {
        // reg-VM Phase 5: when the cond is MATERIALISED in a slot (not deferred
        // on the stack, not a remat const), branch on it directly with
        // OP_R_BRANCH_FALSE (reads regs[cond_slot]) — dropping the GET_LOCAL
        // the fast path / emitVarRef would emit.  The cond isn't on the stack,
        // so flushAllDeferred (commit pending to slots for branch consistency)
        // replaces flushBelowBranchCond's stash-the-cond dance.  This is what
        // makes a branch's condition register-addressed (e.g. fib's R_PRIMOP2
        // `n < 2` result feeding the If).  Gate NIX_V3_NO_R_BRANCH.
        static const bool s_noRBranch =
            std::getenv("NIX_V3_NO_R_BRANCH") != nullptr;
        if (!s_noRBranch) {
            auto sit = ctx->slot.find(e.cond);
            if (sit != ctx->slot.end() && sit->second <= 0xFFFFFFu
                && std::find(ctx->pendingDefer.begin(),
                             ctx->pendingDefer.end(), e.cond)
                       == ctx->pendingDefer.end()) {
                flushAllDeferred();
                uint32_t bf = emitJumpPlaceholder(OP_R_BRANCH_FALSE);
                unit.code.push_back(sit->second);   // cond_slot follow-up
                emitBlock(e.thenBlock);
                uint32_t je = emitJumpPlaceholder(OP_JUMP);
                patchJump(bf, static_cast<uint32_t>(unit.code.size()));
                emitBlock(e.elseBlock);
                patchJump(je, static_cast<uint32_t>(unit.code.size()));
                return;
            }
        }

        // #542 unary fast path: when the cond binding is OnceLinear
        // and was deferred (its value is on top of stack), skip the
        // GET — OP_BRANCH_FALSE pops top.  This is the canonical
        // shape for fib's `if k < 2 then ... else ...`: the LESS
        // result is consumed by If, both bindings are tail-adjacent
        // OnceLinear, and the binary fast path on Less + this unary
        // fast path on If together collapse `<expr-cond>; SET; GET;
        // BRANCH_FALSE` to `<expr-cond>; BRANCH_FALSE`.
        if (!tryFastPathUnary(e.cond))
            emitVarRef(e.cond);
        // #668: when tryFastPathUnary popped only the cond from a
        // non-singleton pending list, the deeper deferred values sit
        // BELOW the cond on the runtime stack.  Without this flush
        // thenBlock's emitBlock would commit them to slots ONLY in the
        // then path, leaving elseBlock with the deeper values still on
        // stack and later OP_GET_LOCAL reads landing on Uninitialized
        // slots.  See flushBelowBranchCond for the full reasoning.
        flushBelowBranchCond();
        uint32_t bf = emitJumpPlaceholder(OP_BRANCH_FALSE);
        emitBlock(e.thenBlock);
        uint32_t je = emitJumpPlaceholder(OP_JUMP);
        patchJump(bf, static_cast<uint32_t>(unit.code.size()));
        emitBlock(e.elseBlock);
        patchJump(je, static_cast<uint32_t>(unit.code.size()));
    }

    // -- Lists / strings
    void emitOne(const ir::ListExpr & e)
    {
        for (auto v : e.elems) emitVarRef(v);
        unit.code.push_back(encode(OP_LIST_INIT, static_cast<uint32_t>(e.elems.size())));
    }
    void emitOne(const ir::ConcatLists & e)
    {
        // #542 binary fast path.
        if (!tryFastPathBinary(e.lhs, e.rhs)) {
            emitVarRef(e.lhs);
            emitVarRef(e.rhs);
        }
        unit.code.push_back(encode(OP_LIST_CONCAT));
    }
    void emitOne(const ir::ConcatStrings & e)
    {
        for (auto v : e.parts) emitVarRef(v);
        // pack forceString as the LSB of the count operand
        uint32_t opnd = (static_cast<uint32_t>(e.parts.size()) << 1) | (e.forceString ? 1u : 0u);
        unit.code.push_back(encode(OP_STR_CONCAT, opnd));
    }

    // -- Attrsets
    //
    // Static-attr layout in bytecode: after OP_ATTRS_INIT[n] we emit
    // 2*n words — `name, pos, name, pos, ...` so the VM can populate
    // both the Bindings and the per-attr position side-table.  Older
    // call sites that read just SymbolIds need to bump their `ip` by
    // 2*n instead of n.
    void emitOne(const ir::AttrSet & e)
    {
        // foldl lever (2026-06-16): demote a NON-recursive static attrset to
        // the cheap OP_ATTRS_INIT (pop-N build) instead of the
        // OP_ATTRS_REC_INIT + per-entry OP_ATTRS_REC_SET + slot/publish/
        // cell-update protocol.  A non-rec `{...}` literal can never
        // self-reference or `with self;` during construction (both require
        // `rec`), so that machinery is dead weight.  Profiled: the FOLD 7-row
        // workload's 3.5× v3-vs-TW gap is per-element record construction; this
        // removes the 2 REC_SET + 2 slot stores per record.  Values are pushed
        // BEFORE the opcode (like OP_LIST_INIT) so no deferred-flush dance is
        // needed (unlike REC_INIT which pushes the Bindings first).
        // DEFAULT-ON 2026-06-16 (opt-out NIX_V3_NO_NONREC_ATTRS_INIT=1).
        // Validated byte-identical: --core lang 21/21, hello/git/firefox
        // drvPath, and a 59-package laptop drvPath sweep (demote-diverge=0,
        // tw-diverge=0) — plus the semantic argument (a non-rec literal can't
        // self-ref/with-self, so ATTRS_INIT ≡ REC_INIT modulo the value-
        // irrelevant cell-update).  The opt-out is the A/B baseline + emergency
        // mitigation.  RETIREMENT: drop the opt-out (hard-true) after a full
        // darwin-4 nixpkgs byte-equality sweep, mirroring the nursery-flip gate.
        static const bool s_nonRecAttrsInit =
            std::getenv("NIX_V3_NO_NONREC_ATTRS_INIT") == nullptr;
        // `!e.isFunctionReturn`: the demotion must NOT win over the
        // isFunctionReturn → OP_ATTRS_REC_INIT_TAIL selection below (emit.cc:1465),
        // which publishes the partial Bindings to outer mid-force thunks (the
        // #495/#498 `with self;` family).  No pass currently sets isFunctionReturn
        // on a non-rec set (markTailReturnAttrSets is retired), so this is a no-op
        // today — kept as a guard so reviving tail-return tagging can't silently
        // drop the TAIL publish for a demoted set with no compile error.
        if (s_nonRecAttrsInit && e.nonRecursive && !e.isFunctionReturn) {
            const size_t nn = e.entries.size();
            if (nn == 0) {
                unit.code.push_back(encode(OP_ATTRS_INIT, 0));
                return;
            }
            // P3.3/§3.4 ATTEMPTED + REVERTED (2026-07-02): moving the runtime
            // entry sort here (emit the trailer + value pushes SymbolId-sorted,
            // let the VM straight-fill) is UNSAFE for the cross-process disk
            // cache.  OP_ATTRS_INIT pushes its values POSITIONALLY on the stack
            // (in trailer order), with no per-value operand.  On a cross-process
            // cache hit, remapAllSymbols translates the writer's SymbolIds to
            // the reader's — an order-changing permutation in general (see the
            // #814 formals re-sort in deserialize) — but it cannot reorder the
            // already-emitted value-push instructions to match a re-sorted
            // trailer.  So the trailer MUST be sorted at RUNTIME, by the reader's
            // local SymbolIds, AFTER remap.  OP_ATTRS_REC_INIT escapes this only
            // because its values are SLOT-indexed (OP_ATTRS_REC_SET) and
            // remapAllSymbols rewrites the slots (serialize.cc:576-614).  Keep
            // the source-order emit; the VM sorts.  (The sort is small-n and its
            // cost is below the darwin-4 noise floor — the cold-only win from a
            // clear-flag-on-deserialize variant did not justify the 7-site
            // operand-masking footgun.)  Full write-up: DEFECT_AUDIT §3.4 note.
            for (const auto & en : e.entries) emitVarRef(en.value);
            unit.code.push_back(encode(OP_ATTRS_INIT, static_cast<uint32_t>(nn)));
            for (const auto & en : e.entries) {
                unit.code.push_back(en.name);
                unit.code.push_back(en.pos);
            }
            return;
        }
        // STG-style early-alloc (#548c, 2026-05-10): allocate the
        // Bindings UPFRONT via OP_ATTRS_REC_INIT and fill entries via
        // per-entry OP_ATTRS_REC_SET.  This mirrors GHC's allocate-Con-
        // first-fill-fields-later pattern and gives each entry's thunk
        // value a heap-stable cell on entries[i].value, which OP_RETURN
        // updates at force time (cell-update protocol from REC_SET's
        // own logic at vm.cc:5704+).
        //
        // Why not OP_ATTRS_INIT: the legacy form pops N values from
        // stack at OP_ATTRS_INIT time (after all entries have been
        // computed).  That's incompatible with `with self;` lookups
        // that fire DURING entry computation — pkgs is mid-Black, the
        // Bindings doesn't exist yet, the lookup throws cycle.  With
        // REC_INIT firing first, the Bindings is allocated and (under
        // Phase B) registered as the outer thunk's partial Bindings;
        // earlier-set entries become observable to later entries'
        // sub-expressions via the partial-bindings peek in withLookup.
        //
        // Why OP_ATTRS_REC_INIT (publishes) and not LET_REC_INIT (no
        // publish): the publish is what registers the partial Bindings
        // with the outer Black thunk's side-table.  The publish itself
        // is gated by STG + isRecInit=true (vm.cc:1095) so non-rec
        // attrsets only register the side-table — they never overwrite
        // an outer thunk's `evaluated` field.  This is the
        // architecturally-correct choice (sub-expression Bindings are
        // never confused with the outer thunk's value; #495 stays
        // fixed).
        //
        // Layout: REC_INIT requires the trailer to be (name, pos) in
        // SymbolId-sorted order.  The slot operand of REC_SET indexes
        // into the trailer's sorted positions.  We sort entry indices
        // by name here (vs. their textual order in the source), then
        // emit per-entry value-push + REC_SET in sort order so the
        // first SET fills sorted-slot 0, etc.
        const size_t n = e.entries.size();
        if (n == 0) {
            // Empty attrset: keep the OP_ATTRS_INIT fast path.
            // OP_ATTRS_INIT with n=0 has its own dispatch shortcut
            // (vm.cc:4017) that pushes the singleton vEmptyAttrs.
            unit.code.push_back(encode(OP_ATTRS_INIT, 0));
            return;
        }

        // Build a permutation `sortedIdx` such that
        //   e.entries[sortedIdx[k]].name is the k-th in sorted order.
        std::vector<uint32_t> sortedIdx(n);
        for (uint32_t i = 0; i < n; ++i) sortedIdx[i] = i;
        std::sort(sortedIdx.begin(), sortedIdx.end(),
            [&](uint32_t a, uint32_t b) {
                return e.entries[a].name < e.entries[b].name;
            });
        // P3.6/§3.14 (2026-07-02): removed a dead duplicate-detection loop
        // here — it scanned the sorted entries for an adjacent duplicate and
        // then merely `break`'d (the throw was deferred to the runtime
        // OP_ATTRS_REC/INIT dup-check for message-identity), so it computed
        // and discarded.  Pure scan ⇒ BI-neutral removal; the runtime check
        // still rejects duplicate attrs with the identical message.
        // Flush any pending deferred values to their slots BEFORE we
        // push the Bindings.  The deferring optimisation (#542) keeps
        // recently-computed OnceLinear values on the runtime stack
        // expecting the next emit step to consume them; if we push the
        // Bindings on top of those, subsequent emitVarRef calls would
        // flush-and-spill them with the WRONG slot mapping (top of
        // stack is now the Bindings, not the deferred value).  Empty
        // pending after this means our REC_INIT/REC_SET sequence has
        // a clean stack to work on.
        flushAllDeferred();
        // Emit REC_INIT with sorted (name, pos) trailer.
        //
        // #558: choose between OP_ATTRS_REC_INIT_TAIL (publishes to
        // every thunk frame on the call stack — Black + Suspended,
        // first-wins) and OP_ATTRS_REC_INIT (publishes only to the
        // innermost Black thunk frame).  The TAIL variant fires when
        // this AttrSet is the function's tail-return value — i.e.
        // forcing the surrounding thunk produces THIS AttrSet's
        // Bindings.  Outer thunks waiting for that thunk benefit
        // from seeing our partial Bindings via the partial-Bindings
        // peek path in withLookup.
        //
        // The non-tail variant is conservative: it preserves the
        // original innermost-Black-only registration so sub-attrsets
        // (let-bindings, function args) don't pollute outer thunks'
        // registry entries with intermediate sub-expression shapes.
        const Op initOp =
            e.isFunctionReturn ? OP_ATTRS_REC_INIT_TAIL
                               : OP_ATTRS_REC_INIT;
        unit.code.push_back(encode(initOp, static_cast<uint32_t>(n)));
        for (uint32_t k = 0; k < n; ++k) {
            const auto & en = e.entries[sortedIdx[k]];
            unit.code.push_back(en.name);
            unit.code.push_back(en.pos);
        }
        // Emit per-entry value-push + REC_SET <slot>.
        //
        // R1 trigger fix (2026-05-26, post-Schema-14): DECOUPLE emit
        // visit order from runtime slot order.
        //
        // Old approach: iterate `sortedIdx[k]` (SymbolId-sorted), emit
        // REC_SET to k.  This made the EMIT VISIT ORDER process-local
        // (SymbolId values vary cross-process), so `getOrAssignSlot`
        // assigned different local slots to the same source binding —
        // observable as OP_GET_LOCAL operand drift across the residual
        // 4 R1 DIFFs (perl, all-packages, python-packages, lua-5).
        //
        // New approach: iterate `e.entries` in canonical (entries-
        // vector, i.e. string-sorted post my lower.cc:2586 fix) order.
        // The REC_SET operand is `symRank[i]` — the SymbolId-sort
        // position of entries[i].  This keeps:
        //   * Runtime invariant intact (REC_INIT trailer is still
        //     SymbolId-sorted at line 854-858; Bindings::lookup's
        //     binary search works as before).
        //   * Cross-process emit order canonical (both writer and
        //     reader walk the same canonical entries vector) → slot
        //     allocations match → OP_GET_LOCAL operands match.
        //   * Existing `remapSymbolsInBytecode` REC_SET permutation
        //     logic still applies (it tracks `pending.oldToNew` over
        //     setsRemaining=n REC_SETs, regardless of their emit
        //     order).
        //
        // #558 emit-order restructure: entries with `isInheritFrom=true`
        // are SKIPPED here.  Their REC_SETs are emitted by a trailing
        // `AttrSetSetInheritFrom` binding lowered AFTER the from-expr
        // cache + IF entry value bindings.
        std::vector<uint32_t> symRank(n);
        for (uint32_t k = 0; k < n; ++k)
            symRank[sortedIdx[k]] = k;
        for (uint32_t i = 0; i < n; ++i) {
            const auto & en = e.entries[i];  // canonical vector order
            if (en.isInheritFrom) continue;
            emitVarRef(en.value);
            unit.code.push_back(encode(OP_ATTRS_REC_SET, symRank[i]));
        }
    }

    /// #558 emit-order restructure: emit the per-entry REC_SETs for
    /// the inherit-from entries of an attrset built by an earlier
    /// `AttrSet` binding.  Pushes the attrset on the runtime stack,
    /// then for each (slot, valueVar): pushes value, OP_ATTRS_REC_SET k.
    /// The attrset stays on top of the stack at the end — emitBlock
    /// flushes/spills it to a slot like any other binding result.  The
    /// var that this binding produces is a discardable alias of the
    /// attrset (same heap Bindings*).
    void emitOne(const ir::AttrSetSetInheritFrom & e)
    {
        // Match emitOne(AttrSet)'s flush discipline: deferred values on
        // the runtime stack would interleave wrong with the
        // attrset-on-top + value-push + REC_SET sequence.
        flushAllDeferred();
        emitVarRef(e.attrSetVar);
        for (const auto & en : e.entries) {
            emitVarRef(en.valueVar);
            unit.code.push_back(encode(OP_ATTRS_REC_SET, en.sortedSlot));
        }
        // Attrset stays on top of stack — caller (emitBlock) handles
        // SET_LOCAL / pendingDefer for our binding's var.  The var is
        // typically dead (no GET_LOCAL on it elsewhere) but we still
        // assign a slot so `tryDefer`'s OnceLinear classification has
        // something to work with.
    }

    void emitOne(const ir::AttrSetDyn & e)
    {
        // Emit static values in order, then dynamic name+value pairs.
        for (auto & en : e.statics)  emitVarRef(en.value);
        for (auto & en : e.dynamics) { emitVarRef(en.nameVar); emitVarRef(en.value); }
        uint32_t packed = (static_cast<uint32_t>(e.statics.size()) << 12)
                        | (static_cast<uint32_t>(e.dynamics.size()) & 0xFFFu);
        unit.code.push_back(encode(OP_ATTRS_INIT_DYN, packed));
        for (auto & en : e.statics) {
            unit.code.push_back(en.name);
            unit.code.push_back(en.pos);
        }
        // Dynamic-name positions follow the static block, one per
        // dynamic entry (positions for static names then dyn names).
        for (auto & en : e.dynamics) unit.code.push_back(en.pos);
    }
    void emitOne(const ir::AttrSelect & e)
    {
        // #542 unary fast path.
        if (!tryFastPathUnary(e.attrs))
            emitVarRef(e.attrs);
        unit.code.push_back(encode(OP_ATTRS_SELECT, e.name));
        // Reserve an inline-cache slot.  At runtime the VM will write
        // the most recently seen (Bindings*, slot) tuple here so a
        // repeat access on the same attrset shape skips the binary
        // search.  Slot index is stored as the next code word.
        uint32_t icIdx = static_cast<uint32_t>(unit.rt.attrSelectCache.size());
        unit.rt.attrSelectCache.emplace_back();
        unit.code.push_back(icIdx);
    }
    void emitOne(const ir::AttrSelectDyn & e)
    {
        // #542 binary fast path.
        if (!tryFastPathBinary(e.attrs, e.nameVar)) {
            emitVarRef(e.attrs);
            emitVarRef(e.nameVar);
        }
        unit.code.push_back(encode(OP_ATTRS_SELECT_DYN));
    }
    /// SECD-style heap-stable slot reference: push the rec-attrset
    /// and let OP_REC_BINDING_SLOT_REF look up the entry, pushing a
    /// Tag::Slot Value pointing into Bindings::entries[i].value.
    /// See ir.hh + vm.cc OP_REC_BINDING_SLOT_REF for rationale.
    ///
    /// #458 step 5/6: dropped the emit-time OP_FORCE that previously
    /// preceded OP_REC_BINDING_SLOT_REF.  The runtime handler already
    /// forces / Tag::Slot-derefs its source on the fast path, and
    /// the emit-time prefix added a redundant dispatch (mirrors the
    /// REVIEW-COMP §8.6 MED-5 cleanup that removed OP_FORCE before
    /// OP_WITH_PUSH).  In slot-capture mode the source is Tag::Slot
    /// and an explicit OP_FORCE would chain a deref-then-no-op;
    /// dropping it lets the slot deref happen in one place inside
    /// the OP_REC_BINDING_SLOT_REF handler.
    void emitOne(const ir::RecBindingSlotRef & e)
    {
        // §2(b) superinstruction (NEXT_STEPS_2026-06-05): the recursive
        // self-reference resolves the rec-attrset through a CAPTURED upvalue
        // (the common shape — e.g. fib resolving `fib` twice per call), so
        // the source is `OP_GET_UPVALUE idx`.  Fuse `GET_UPVALUE ; RBSR` into
        // OP_GET_UPVALUE_REC_BINDING: read the upvalue directly, dropping one
        // dispatch and the intermediate push/pop, while keeping the same IC.
        // Only fires when the source is a plain upvalue that the #542 defer
        // pipeline didn't capture (pending top) — upvalues are never deferred
        // (defer is for OnceLinear bindings), so this is the steady case.
        // NIX_V3_NO_FUSE_RECBIND=1 disables it (default-ON A/B bisect switch,
        // mirrors NO_DEFER/NO_CALL_N; retire once shipped byte-identical on
        // --core + a nixpkgs sample, or revert the feature if net-negative).
        static const bool s_noFuseRecBind =
            std::getenv("NIX_V3_NO_FUSE_RECBIND") != nullptr;
        if (!s_noFuseRecBind
            && (ctx->pendingDefer.empty() || ctx->pendingDefer.back() != e.attrs)
            && ctx->slot.find(e.attrs) == ctx->slot.end()) {
            if (auto uit = ctx->upvalue.find(e.attrs); uit != ctx->upvalue.end()) {
                flushAllDeferred();  // match emitVarRef discipline
                uint32_t icIdx = static_cast<uint32_t>(unit.rt.recSlotCache.size());
                unit.rt.recSlotCache.emplace_back();
                unit.code.push_back(encode(OP_GET_UPVALUE_REC_BINDING, e.name));
                unit.code.push_back(uit->second);   // upvalIdx
                unit.code.push_back(icIdx);
                return;
            }
        }

        // #542 unary fast path.
        if (!tryFastPathUnary(e.attrs))
            emitVarRef(e.attrs);
        unit.code.push_back(encode(OP_REC_BINDING_SLOT_REF, e.name));
        // #779 (2026-05-23) Schema 10: 1-word IC follow-up.  Each
        // OP_REC_BINDING_SLOT_REF instance gets a fresh slot in
        // unit.rt.recSlotCache, indexed by the icIdx that we emit
        // here.  Entries default-initialize to (bindings=null,
        // slot=0); first miss installs.
        uint32_t icIdx = static_cast<uint32_t>(unit.rt.recSlotCache.size());
        unit.rt.recSlotCache.emplace_back();
        unit.code.push_back(icIdx);
    }
    void emitOne(const ir::HasAttr & e)
    {
        // #542 unary fast path.
        if (!tryFastPathUnary(e.attrs))
            emitVarRef(e.attrs);
        unit.code.push_back(encode(OP_ATTRS_HAS, e.name));
    }
    void emitOne(const ir::HasAttrDyn & e)
    {
        // #542 binary fast path.
        if (!tryFastPathBinary(e.attrs, e.nameVar)) {
            emitVarRef(e.attrs);
            emitVarRef(e.nameVar);
        }
        unit.code.push_back(encode(OP_ATTRS_HAS_DYN));
    }
    void emitOne(const ir::Update & e)
    {
        // #542 binary fast path.
        if (!tryFastPathBinary(e.lhs, e.rhs)) {
            emitVarRef(e.lhs);
            emitVarRef(e.rhs);
        }
        // #558 (2026-05-10) tail-return // emits OP_ATTRS_UPDATE_TAIL
        // which additionally publishes the merged Bindings to all
        // THUNK_RETURN frames (publishToAllThunkFrames).  This is the
        // STG analog of "constructor reaches WHNF" for // results.
        // See ir::Update::isFunctionReturn doc.
        unit.code.push_back(encode(
            e.isFunctionReturn ? OP_ATTRS_UPDATE_TAIL : OP_ATTRS_UPDATE));
    }

    // -- Recursive let / rec attrset
    void emitOne(const ir::LetRec & e)
    {
        // #542: LetRec's emit does multiple direct-push ops
        // (OP_ATTRS_REC_INIT, intermediate OP_DUP / OP_SET_LOCALs,
        // OP_MAKE_THUNK loops with REC_SET writes) where the runtime
        // stack mid-emit is in a complex state — `bindings` on top
        // with various intermediates above it, then a thunk pushed,
        // then REC_SET pops the thunk back into the bindings entry,
        // etc.  In this state, our deferring tracker would mis-
        // attribute the runtime stack top: pending says "var X is on
        // top" but actually `bindings` (or a thunk) is on top.
        // flushAllDeferred() at LetRec entry commits any prior
        // pending to slots BEFORE we begin the rec construction,
        // ensuring the runtime stack is in sync with the LetRec
        // emit's expected state.  After this, pending = [] and the
        // construction proceeds on a clean foundation.
        flushAllDeferred();

        // Sort entries by SymbolId so the resulting Bindings are valid
        // (Bindings::lookup uses binary search on the sorted array).
        // Each REC_SET's operand becomes the entry's slot in the
        // sorted Bindings.
        const uint32_t n = static_cast<uint32_t>(e.entries.size());
        std::vector<uint32_t> sortedOrder(n);
        for (uint32_t i = 0; i < n; ++i) sortedOrder[i] = i;
        std::sort(sortedOrder.begin(), sortedOrder.end(),
            [&](uint32_t a, uint32_t b) {
                return e.entries[a].name < e.entries[b].name;
            });
        std::vector<uint32_t> entryToSlot(n);
        for (uint32_t slot = 0; slot < n; ++slot)
            entryToSlot[sortedOrder[slot]] = slot;

        // 1. OP_ATTRS_REC_INIT or OP_ATTRS_LET_REC_INIT (based on
        //    hasBody) + n sorted (SymbolId, PosIdx) pairs: push
        //    placeholder rec Bindings on operand stack.
        //
        // OP_ATTRS_REC_INIT additionally publishes the rec-attrs to
        // the nearest Black thunk frame's `evaluated` field (legitimate
        // for `rec { ... }` literals where the rec-attrs IS the
        // surrounding thunk's eventual return value).  OP_ATTRS_LET_REC
        // _INIT skips that publish (correct for `let ... in body`
        // where the thunk's return value is `body`, not the recAttrs;
        // publishing the let's intermediate `{prev}` etc. corrupts the
        // surrounding thunk's state -- the v3-direct callPackage with-
        // scope bug; see CALLPACKAGE_BUG_2026-05-09.md).
        const Op initOp = e.hasBody ? OP_ATTRS_LET_REC_INIT
                                    : OP_ATTRS_REC_INIT;
        unit.code.push_back(encode(initOp, n));
        for (uint32_t slot = 0; slot < n; ++slot) {
            unit.code.push_back(e.entries[sortedOrder[slot]].name);
            unit.code.push_back(e.entries[sortedOrder[slot]].pos);
        }

        // 1.5  #458 step 1/6 — heap-stable rec-slot publish.
        //
        // If the lowerer registered a `recSlotVar` for this LetRec
        // (`Module::recVarToSlotVar`), allocate a heap-stable Value*
        // slot and publish the just-built rec Bindings into it.
        // Stack transition pre/post:
        //
        //     pre:  [..., Tag::Attrs]
        //     OP_REC_SLOT_PUBLISH
        //     post: [..., Tag::Attrs, Tag::Slot]
        //     OP_SET_LOCAL recSlotLocal
        //     post: [..., Tag::Attrs]
        //
        // The Tag::Slot in the local outlives the let-rec frame as
        // long as any closure referencing it stays reachable (Boehm
        // GC tracks the slot through the closure's freeVars vector).
        //
        // No-op for callers that haven't enabled the slot path:
        // recVarToSlotVar is empty, getOrAssignSlot is never called.
        if (auto sit = m.recVarToSlotVar.find(e.recVar);
            sit != m.recVarToSlotVar.end())
        {
            uint16_t recSlotLocal = getOrAssignSlot(sit->second);
            unit.code.push_back(encode(OP_REC_SLOT_PUBLISH));
            unit.code.push_back(encode(OP_SET_LOCAL, recSlotLocal));
        }

        // 2. Spill the rec_attrs (currently on top of the operand
        //    stack) into a frame slot so each entry's thunk-body
        //    upvalue list can reference it via plain emitVarRef.  We
        //    can't rely on OP_DUP-in-loop because freeVars is sorted
        //    by VarId — recVar may not be the last one pushed before
        //    OP_MAKE_THUNK, so a DUP at that point would copy the
        //    wrong value.
        uint16_t recSlot = getOrAssignSlot(e.recVar);
        unit.code.push_back(encode(OP_DUP));
        unit.code.push_back(encode(OP_SET_LOCAL, recSlot));

        // 3. REVIEW HIGH-4 follow-up: emit hidden from-expr thunks
        //    BEFORE the regular per-attr thunks, so per-attr bodies
        //    that reference a hidden thunk via upvalue capture see
        //    the bound slot at MAKE_THUNK time.  Each hidden thunk
        //    captures recVar (already bound) + any other free vars,
        //    and writes its result Value into the hiddenVar's local
        //    slot.
        //
        // STG-14b (#516/#517): use OP_THUNK_SET_LOCAL_THROUGH_CELL
        // instead of OP_SET_LOCAL.  The new opcode wraps the thunk
        // in a heap-stable cell and attaches the cell as the thunk's
        // OP_RETURN-update target, then writes Tag::Slot{cell} into
        // the local.  Per-attr thunks capturing the slot deref
        // through the cell -- so when the hidden thunk's body
        // completes via OP_RETURN, captures see the Evaluated value
        // instead of the stale Black thunk pointer (the previous
        // Tag::Thunk by-value capture broke under STG_KEEP_HOOKS
        // because cell-update at OP_RETURN never fired without a
        // cell attached -- audit memo lode/CELL_UPDATE_AUDIT_2026-
        // 05-08.md).
        for (auto & he : e.hiddenEntries) {
            const auto & ff = m.functions[he.thunkBody].freeVars;
            // #530 lexical-with chain — push with-target VarIds FIRST
            // so they sit BELOW the upvalue block.
            for (auto wv : he.lexicalWiths) emitVarRef(wv);
            for (auto fv : ff) emitVarRef(fv);
            unit.code.push_back(encode(OP_MAKE_THUNK, he.thunkBody));
            unit.code.push_back(static_cast<uint32_t>(ff.size()));
            unit.code.push_back(static_cast<uint32_t>(he.lexicalWiths.size()));
            uint16_t hiddenSlot = getOrAssignSlot(he.hiddenVar);
            unit.code.push_back(encode(OP_THUNK_SET_LOCAL_THROUGH_CELL, hiddenSlot));
        }

        // 4. For each IR entry, build a Thunk capturing whatever
        //    upvalues its body needs and write it into the sorted slot.
        //    Each thunk body's freeVars list (sorted by VarId,
        //    populated by computeFreeVars) IS the upvalue layout: the
        //    body references upvalues[i] = freeVars[i].  We push them
        //    in that order so OP_MAKE_THUNK pops in reverse and
        //    upvalues[i] ends up correct.
        for (uint32_t i = 0; i < n; ++i) {
            auto & en = e.entries[i];
            const auto & ff = m.functions[en.thunkBody].freeVars;
            // #498: trace LetRec entry's freeVars for thunks named "res"
            // with size==4 — the all-packages.nix invocation we're
            // root-causing.
            if (std::getenv("V3_DBG_RES_FREEVARS")
                && m.functions[en.thunkBody].name == "res"
                && ff.size() == 4) {
                std::fprintf(stderr,
                    "v3 emit LetRec res: fid=%u freeVars=[",
                    (unsigned)en.thunkBody);
                for (auto fv : ff) std::fprintf(stderr, "%u,", (unsigned)fv);
                std::fprintf(stderr, "]\n");
                // Print where each freeVar resolves in the OUTER ctx.
                std::fprintf(stderr, "  outer ctx resolution:\n");
                for (auto fv : ff) {
                    if (auto it = ctx->slot.find(fv); it != ctx->slot.end())
                        std::fprintf(stderr,
                            "    var=%u -> outer slot %u\n",
                            (unsigned)fv, (unsigned)it->second);
                    else if (auto uit = ctx->upvalue.find(fv); uit != ctx->upvalue.end())
                        std::fprintf(stderr,
                            "    var=%u -> outer upvalue %u\n",
                            (unsigned)fv, (unsigned)uit->second);
                    else
                        std::fprintf(stderr,
                            "    var=%u -> UNBOUND\n", (unsigned)fv);
                }
            }
            // #530 lexical-with chain — push with-target VarIds FIRST.
            for (auto wv : en.lexicalWiths) emitVarRef(wv);
            for (auto fv : ff) emitVarRef(fv);
            unit.code.push_back(encode(OP_MAKE_THUNK, en.thunkBody));
            unit.code.push_back(static_cast<uint32_t>(ff.size()));
            unit.code.push_back(static_cast<uint32_t>(en.lexicalWiths.size()));
            unit.code.push_back(encode(OP_ATTRS_REC_SET, entryToSlot[i]));
        }
        // After all SETs, apply __overrides if the rec contains it —
        // rewrites the matching entries' thunk values so subsequent
        // OP_ATTRS_SELECT inside the rec body sees the overridden value.
        unit.code.push_back(encode(OP_APPLY_OVERRIDES));
        // After all SETs, rec attrs is on top of the operand stack —
        // becomes the value of the LetRec binding.
    }

    // -- Primop direct call
    uint32_t internPrimOp(const PrimOp * po)
    {
        for (uint32_t i = 0; i < unit.primops.size(); ++i)
            if (unit.primops[i] == po) return i;
        unit.primops.push_back(po);
        return static_cast<uint32_t>(unit.primops.size() - 1);
    }
    /// #428: targeted-primop -> fast-path opcode mapping.  Returns 0
    /// when the primop isn't one of the inlined ones; otherwise the
    /// matching opcode.  Lazily caches the canonical PrimOp pointers
    /// at first lookup so subsequent calls are pointer-compares.
    static Op fastPathOpcodeFor(const PrimOp * po, uint32_t nArgs)
    {
        struct Cache {
            const PrimOp * isNull = nullptr, * isBool = nullptr;
            const PrimOp * isInt = nullptr, * isFloat = nullptr;
            const PrimOp * isString = nullptr, * isPath = nullptr;
            const PrimOp * isList = nullptr, * isAttrs = nullptr;
            const PrimOp * isFunction = nullptr;
            const PrimOp * head = nullptr, * tail = nullptr;
            const PrimOp * length = nullptr, * elemAt = nullptr;
            Cache() {
                isNull     = findPrimOp("isNull");
                isBool     = findPrimOp("isBool");
                isInt      = findPrimOp("isInt");
                isFloat    = findPrimOp("isFloat");
                isString   = findPrimOp("isString");
                isPath     = findPrimOp("isPath");
                isList     = findPrimOp("isList");
                isAttrs    = findPrimOp("isAttrs");
                isFunction = findPrimOp("isFunction");
                head       = findPrimOp("head");
                tail       = findPrimOp("tail");
                length     = findPrimOp("length");
                elemAt     = findPrimOp("elemAt");
            }
        };
        static const Cache c;
        if (nArgs == 1) {
            if (po == c.isNull)     return OP_IS_NULL;
            if (po == c.isBool)     return OP_IS_BOOL;
            if (po == c.isInt)      return OP_IS_INT;
            if (po == c.isFloat)    return OP_IS_FLOAT;
            if (po == c.isString)   return OP_IS_STRING;
            if (po == c.isPath)     return OP_IS_PATH;
            if (po == c.isList)     return OP_IS_LIST;
            if (po == c.isAttrs)    return OP_IS_ATTRS;
            if (po == c.isFunction) return OP_IS_FUNCTION;
            if (po == c.head)       return OP_HEAD;
            if (po == c.tail)       return OP_TAIL;
            if (po == c.length)     return OP_LENGTH;
        } else if (nArgs == 2) {
            if (po == c.elemAt)     return OP_ELEM_AT;
        }
        return static_cast<Op>(0);
    }

    /// #736 IFD-class primop detector.  Returns one of the
    /// `kIfd*` constants for primops that may trigger Import-From-
    /// Derivation (or block-eval network fetches that benefit from
    /// the same batching/cache layers).  Returns `kIfdNone` for any
    /// primop that does not require an OP_IFD_PROBE marker.
    ///
    /// Lazily caches canonical PrimOp pointers at first call so
    /// subsequent dispatch is a string-free pointer compare.  Both
    /// the un-prefixed and `__`-prefixed names are matched because
    /// nixpkgs uses both spellings interchangeably.
    static uint8_t ifdProbeKindForPrimop(const PrimOp * po)
    {
        struct Cache {
            const PrimOp * import = nullptr;         const PrimOp * iimport = nullptr;
            const PrimOp * readFile = nullptr;       const PrimOp * ireadFile = nullptr;
            const PrimOp * readDir = nullptr;        const PrimOp * ireadDir = nullptr;
            const PrimOp * pathExists = nullptr;     const PrimOp * ipathExists = nullptr;
            const PrimOp * readFileType = nullptr;   const PrimOp * ireadFileType = nullptr;
            const PrimOp * findFile = nullptr;       const PrimOp * ifindFile = nullptr;
            const PrimOp * fetchurl = nullptr;       const PrimOp * ifetchurl = nullptr;
            const PrimOp * fetchTarball = nullptr;
            const PrimOp * fetchTree = nullptr;
            const PrimOp * fetchGit = nullptr;
            const PrimOp * fetchMercurial = nullptr;
            const PrimOp * filterSource = nullptr;
            Cache() {
                import         = findPrimOp("import");         iimport         = findPrimOp("__import");
                readFile       = findPrimOp("readFile");       ireadFile       = findPrimOp("__readFile");
                readDir        = findPrimOp("readDir");        ireadDir        = findPrimOp("__readDir");
                pathExists     = findPrimOp("pathExists");     ipathExists     = findPrimOp("__pathExists");
                readFileType   = findPrimOp("readFileType");   ireadFileType   = findPrimOp("__readFileType");
                findFile       = findPrimOp("findFile");       ifindFile       = findPrimOp("__findFile");
                fetchurl       = findPrimOp("fetchurl");       ifetchurl       = findPrimOp("__fetchurl");
                fetchTarball   = findPrimOp("fetchTarball");
                fetchTree      = findPrimOp("fetchTree");
                fetchGit       = findPrimOp("fetchGit");
                fetchMercurial = findPrimOp("fetchMercurial");
                filterSource   = findPrimOp("filterSource");
            }
        };
        static const Cache c;
        if (po == c.import         || po == c.iimport)         return kIfdImport;
        if (po == c.readFile       || po == c.ireadFile)       return kIfdReadFile;
        if (po == c.readDir        || po == c.ireadDir)        return kIfdReadDir;
        if (po == c.pathExists     || po == c.ipathExists)     return kIfdPathExists;
        if (po == c.readFileType   || po == c.ireadFileType)   return kIfdReadFileType;
        if (po == c.findFile       || po == c.ifindFile)       return kIfdFindFile;
        if (po == c.fetchurl       || po == c.ifetchurl)       return kIfdFetchurl;
        if (po == c.fetchTarball)                              return kIfdFetchTarball;
        if (po == c.fetchTree)                                 return kIfdFetchTree;
        if (po == c.fetchGit)                                  return kIfdFetchGit;
        if (po == c.fetchMercurial)                            return kIfdFetchMercurial;
        if (po == c.filterSource)                              return kIfdFilterSource;
        return kIfdNone;
    }

    void emitOne(const ir::PrimOpCall & e)
    {
        for (auto v : e.args) emitVarRef(v);
        // #736 IFD probe: bytecode-level marker emitted right before
        // an IFD-class primop call.  Zero stack effect; identifies
        // the call site as a future S2 (batching) / S4 (eval cache)
        // dispatch point.  Cost when no consumer is active: a single
        // dispatched opcode that increments a counter.
        if (uint8_t kind = ifdProbeKindForPrimop(e.primop); kind != kIfdNone) {
            unit.code.push_back(encode(OP_IFD_PROBE, kind));
        }
        // #428: fast-path inline if this is one of the targeted primops.
        // Args are already on the stack; the inline opcode pops them.
        if (Op op = fastPathOpcodeFor(e.primop, static_cast<uint32_t>(e.args.size()));
            op != static_cast<Op>(0)) {
            unit.code.push_back(encode(op));
            return;
        }
        unit.code.push_back(encode(OP_CALL_PRIMOP, static_cast<uint32_t>(e.args.size())));
        unit.code.push_back(internPrimOp(e.primop));
    }
    void emitOne(const ir::LitPrimOp & e)
    {
        unit.code.push_back(encode(OP_LIT_PRIMOP, internPrimOp(e.primop)));
    }
    void emitOne(const ir::LitBuiltins &)
    {
        unit.code.push_back(encode(OP_LIT_BUILTINS));
    }

    // -- With / assert
    void emitOne(const ir::With & e)
    {
        // WC-38 SECD-style slot aliasing.
        //
        // Preferred path (heap-stable): when the source resolves to a
        // rec-attrset entry, push a Tag::Slot pointing into the
        // Bindings::entries[i].value memory (allocated on the v3 heap,
        // stable for the lifetime of the bindings).  Sub-thunks
        // captured in the with-body see the entry's live mutated /
        // memoized value through the slot.
        //
        // Fallback (snapshot): for non-rec-attrset sources, emit the
        // legacy OP_GET_LOCAL/UPVALUE + OP_WITH_PUSH path.
        bool emittedSlotRef = false;
        if (e.recAttrsVar != ir::kInvalid && e.recAttrsName != ir::kInvalidSymbol) {
            // Push the rec-attrset value, then OP_REC_BINDING_SLOT_REF
            // looks up the entry and pushes Tag::Slot.
            //
            // REVIEW-COMP §8.6 + MED-5 follow-on: the prior emit-time
            // OP_FORCE here was redundant -- OP_REC_BINDING_SLOT_REF
            // already forces its source on the runtime fast path
            // (vm.cc).  The NIX_V3_NO_WITH_FORCE A/B gate is removed;
            // the no-force path is the verified-correct default.
            //
            // #542: do NOT use unary fast path here.  emitOne(With)'s
            // body block emits ITS OWN bindings using the with-stack,
            // and OP_WITH_PUSH happens BETWEEN the operand push and
            // the body emit.  Consuming pending top here would leave
            // pending non-empty; the subsequent OP_WITH_PUSH and
            // emitBlock(body) sequence assumes runtime stack matches
            // pending exactly, which it wouldn't.  Stay safe: just
            // emitVarRef (flushes everything before pushing the
            // attrs).
            emitVarRef(e.recAttrsVar);
            unit.code.push_back(encode(OP_REC_BINDING_SLOT_REF, e.recAttrsName));
            // #779 Schema 10 IC follow-up (see emitOne(RecBindingSlotRef)).
            {
                uint32_t icIdx = static_cast<uint32_t>(unit.rt.recSlotCache.size());
                unit.rt.recSlotCache.emplace_back();
                unit.code.push_back(icIdx);
            }
            unit.code.push_back(encode(OP_WITH_PUSH));
            emittedSlotRef = true;
        }
        if (!emittedSlotRef) {
            emitVarRef(e.attrs);
            unit.code.push_back(encode(OP_WITH_PUSH));
        }
        emitBlock(e.bodyBlock);
        unit.code.push_back(encode(OP_WITH_POP));
    }
    void emitOne(const ir::Assert & e)
    {
        // #542: Assert's cond is consumed by OP_ASSERT (pops top).
        // tryFastPathUnary is sound here.  Skip for now — limited
        // win and Assert is uncommon.
        emitVarRef(e.cond);
        unit.code.push_back(encode(OP_ASSERT));
        emitBlock(e.bodyBlock);
    }

    // Function emit ---------------------------------------------------------

    /// Recursively walk all bindings reachable from `bid` (within the
    /// same function — sub-blocks for if-branches, with-bodies, etc.)
    /// and assign each binding's VarId a frame slot.  This pre-pass
    /// allows references to forward bindings (e.g., let-rec in lambdas)
    /// to resolve at emit time without needing two-pass slot resolution
    /// later.
    void preassignSlotsInBlock(FuncCtx & fc, ir::BlockId bid,
                               std::unordered_set<ir::BlockId> & visited)
    {
        // §2(a) A/B bisect switch — default-ON.  NIX_V3_NO_CONST_REMAT=1
        // disables constant rematerialization so a regression can be
        // bisected to "v3 const-remat" vs everything else (mirrors
        // NIX_V3_NO_DEFER / NIX_V3_NO_CALL_N / NIX_V3_NO_FUSE_SETGET).
        // RETIREMENT: drop the switch once const-remat ships byte-identical
        // on --core + a nixpkgs sample over ≥10 runs, or if it is ever shown
        // net-negative on wall (then revert the whole feature, not the gate).
        static const bool s_noConstRemat =
            std::getenv("NIX_V3_NO_CONST_REMAT") != nullptr;

        if (!visited.insert(bid).second) return;
        const ir::Block & b = m.blocks[bid];
        for (auto & bd : b.bindings) {
            // §2(a): single-use immediate constants get no slot — recorded
            // for remat at use.  Restricted to OnceLinear: a multi-use const
            // is left on the normal materialize path (LIT;SET_LOCAL_KEEP) so
            // the defer mechanism's many-use contract is unchanged, and the
            // win (all hot recursion/loop literals are single-use) is kept.
            // Vars captured by a nested closure are excluded (they must live
            // in a parent slot for MAKE_CLOSURE) and fall through to a slot.
            if (!s_noConstRemat && isRematConst(bd.expr)
                && !capturedFreeVars_.count(bd.var)
                && occ.lookup(bd.var).kind == ir::OccKind::OnceLinear)
                constRemat_[bd.var] = &bd.expr;
            else
                (void)getOrAssignSlot(fc, bd.var);
            std::vector<ir::BlockId> subs;
            std::visit([&](auto const & e) {
                using T = std::decay_t<decltype(e)>;
                if constexpr (std::is_same_v<T, ir::If>) {
                    subs.push_back(e.thenBlock); subs.push_back(e.elseBlock);
                } else if constexpr (std::is_same_v<T, ir::With>  ||
                                     std::is_same_v<T, ir::Assert>) {
                    subs.push_back(e.bodyBlock);
                } else if constexpr (std::is_same_v<T, ir::And> ||
                                     std::is_same_v<T, ir::Or>  ||
                                     std::is_same_v<T, ir::Impl>) {
                    subs.push_back(e.rhsBlock);
                }
            }, bd.expr);
            for (auto sb : subs) preassignSlotsInBlock(fc, sb, visited);
        }
    }

    uint16_t getOrAssignSlot(FuncCtx & fc, ir::VarId v)
    {
        auto it = fc.slot.find(v);
        if (it != fc.slot.end()) return it->second;
        uint16_t s = fc.nextSlot++;
        fc.slot[v] = s;
        if (s + 1 > fc.nLocals) fc.nLocals = s + 1;
        return s;
    }

    /// Step-2 fusion: collapse adjacent same-slot SET_LOCAL;GET_LOCAL in
    /// [codeStart, end) into SET_LOCAL_KEEP.  In-place compaction over
    /// THIS function's body only (O(function), prefix untouched); re-bases
    /// this function's jump operands + diagnostic force-emit offsets.
    /// Skips any GET that is a branch-join target (jumpTargetPositions_).
    void compactFuseSetGet(uint32_t codeStart)
    {
        // Two compacting peepholes in ONE slide+rebase pass:
        //   (A) SET_LOCAL n; GET_LOCAL n (same slot) → SET_LOCAL_KEEP n + drop
        //       the GET  (the original #542 / BYTECODE_NGRAM_ANALYSIS §7 fuse).
        //   (B) Lever 1B-lite: GET_LOCAL a; GET_LOCAL b (adjacent, slots < 4096)
        //       → OP_GET_LOCAL2 (a,b) + drop the second GET.
        // Both rewrite a SURVIVING instruction (the one BEFORE the dropped op)
        // and add the dropped op's position to `rem`; a single slide removes
        // all dropped positions and one rebase fixes jump operands.  Only the
        // DROPPED position may not be a jump target (a jump to the kept first
        // op still executes identically).
        static const bool s_noGet2 =
            std::getenv("NIX_V3_NO_GET_LOCAL2") != nullptr;

        std::vector<uint32_t> rem;
        std::unordered_set<uint32_t> keepInvolved;  // positions (A) touches
        rem.reserve(fuseGetPositions_.size());
        for (uint32_t getPos : fuseGetPositions_) {
            if (jumpTargetPositions_.count(getPos)) continue;
            if (getPos == 0 || getPos >= unit.code.size()) continue;
            Instruction setI = unit.code[getPos - 1];
            Instruction getI = unit.code[getPos];
            if (decodeOp(setI) != OP_SET_LOCAL) continue;
            if (decodeOp(getI) != OP_GET_LOCAL) continue;
            if (decodeOperand(setI) != decodeOperand(getI)) continue;
            rem.push_back(getPos);
            keepInvolved.insert(getPos - 1);
            keepInvolved.insert(getPos);
        }

        // (B) GET_LOCAL a; GET_LOCAL b → OP_GET_LOCAL2.  Drive off the RECORDED
        // GET_LOCAL emit positions (getLocalPositions_) — never decoding raw
        // words — so a multi-word op's follow-up word that happens to decode to
        // OP_GET_LOCAL is never mis-fused.  A pair is two recorded positions p,
        // p+1 (consecutive, since GET_LOCAL is one word); fuse the first of each
        // pair and skip the second (so `GL GL GL` fuses one pair).  The second
        // (removed) GET must not be a jump target; neither may overlap an (A)
        // site.
        if (!s_noGet2 && getLocalPositions_.size() >= 2) {
            std::unordered_set<uint32_t> getSet(getLocalPositions_.begin(),
                                                getLocalPositions_.end());
            std::vector<uint32_t> gp(getLocalPositions_.begin(),
                                     getLocalPositions_.end());
            std::sort(gp.begin(), gp.end());
            uint32_t prevFused = UINT32_MAX;
            for (uint32_t p : gp) {
                uint32_t q = p + 1;
                if (p == prevFused) continue;          // p was a consumed second
                if (!getSet.count(q)) continue;        // q not an adjacent GET
                if (jumpTargetPositions_.count(q)) continue;
                if (keepInvolved.count(p) || keepInvolved.count(q)) continue;
                uint32_t sa = decodeOperand(unit.code[p]);
                uint32_t sb = decodeOperand(unit.code[q]);
                if (sa >= 4096 || sb >= 4096) continue;
                unit.code[p] = encode(OP_GET_LOCAL2, (sa << 12) | sb);
                rem.push_back(q);
                prevFused = q;                          // don't start a pair at q
            }
        }

        if (rem.empty()) return;
        std::sort(rem.begin(), rem.end());

        const uint32_t end = static_cast<uint32_t>(unit.code.size());
        auto shift = [&](uint32_t o) -> uint32_t {
            return static_cast<uint32_t>(
                std::lower_bound(rem.begin(), rem.end(), o) - rem.begin());
        };

        // (A) sites only: rewrite the kept SET_LOCAL → SET_LOCAL_KEEP.  (B)
        // sites already have OP_GET_LOCAL2 at getPos-1 (rewritten inline) —
        // the SET_LOCAL guard leaves them untouched.
        for (uint32_t getPos : rem) {
            Instruction setI = unit.code[getPos - 1];
            if (decodeOp(setI) != OP_SET_LOCAL) continue;
            unit.code[getPos - 1] =
                encode(OP_SET_LOCAL_KEEP, decodeOperand(setI));
        }

        // In-place compact [codeStart, end): slide survivors down.
        size_t w = codeStart;
        size_t ri = 0;
        for (uint32_t o = codeStart; o < end; ++o) {
            if (ri < rem.size() && o == rem[ri]) { ++ri; continue; }
            unit.code[w++] = unit.code[o];
        }
        unit.code.resize(w);

        // Re-base this function's jump operands.
        for (uint32_t at : jumpInsnPositions_) {
            uint32_t newAt = at - shift(at);
            Instruction ins = unit.code[newAt];
            unit.code[newAt] = encode(decodeOp(ins),
                                decodeOperand(ins) - shift(decodeOperand(ins)));
        }

        // Re-base this function's force-emit-site offsets (contiguous tail).
        for (auto it = unit.forceEmitSites.rbegin();
             it != unit.forceEmitSites.rend() && it->first >= codeStart; ++it)
            it->first -= shift(it->first);
    }

    void emitFunction(ir::FuncId fid)
    {
        const ir::Function & f = m.functions[fid];

        FuncCtx fc;
        fc.fid = fid;
        // Param goes in slot 0 if present.  For `{a, b}: ...` formals
        // without an @arg name, the param VarId still represents the
        // attrset arg passed at call time.
        if (f.paramVar != ir::kInvalid &&
            (f.argName != ir::kInvalidSymbol || f.hasFormals))
            (void)getOrAssignSlot(fc, f.paramVar);
        // eval/apply (#3): extra params of an uncurried arity-N function
        // occupy slots 1..N-1, right after paramVar (slot 0).  A saturated
        // OP_CALL_N writes the N args into these slots before entering.
        for (ir::VarId ep : f.extraParams)
            (void)getOrAssignSlot(fc, ep);
        // Upvalue order = freeVars; the index order here MUST match the
        // maker's push order in emitOne(Lambda/MkThunk).
        {
            uint16_t ui = 0;
            for (ir::VarId fv : f.freeVars)
                fc.upvalue[fv] = ui++;
        }
        // #498: trace freeVars for body emit of "res" with 4 freeVars.
        if (std::getenv("V3_DBG_RES_FREEVARS")
            && f.name == "res" && f.freeVars.size() == 4) {
            std::fprintf(stderr,
                "v3 emit body res: fid=%u freeVars=[",
                (unsigned)fid);
            for (auto fv : f.freeVars) std::fprintf(stderr, "%u,", (unsigned)fv);
            std::fprintf(stderr, "]\n");
        }
        // §2(a): build the per-function const-remat set (needs the module-
        // wide captured-freeVar set) BEFORE slot pre-assignment, which both
        // populates constRemat_ and skips slots for its members.
        ensureCapturedFreeVars();
        constRemat_.clear();

        // Pre-assign slots for every VarId reachable from the entry
        // block (including in sub-blocks: if-branches, with bodies, etc.)
        // so forward references resolve at emit time.
        if (f.entryBlock != ir::kInvalidBlock) {
            std::unordered_set<ir::BlockId> visited;
            preassignSlotsInBlock(fc, f.entryBlock, visited);
        }

        ctx = &fc;
        uint32_t codeStart = static_cast<uint32_t>(unit.code.size());

        // Step-2 fusion: reset per-function bookkeeping.
        fuseGetPositions_.clear();
        getLocalPositions_.clear();
        jumpInsnPositions_.clear();
        jumpTargetPositions_.clear();

        if (f.entryBlock != ir::kInvalidBlock)
            emitBlock(f.entryBlock);
        else
            unit.code.push_back(encode(OP_LIT_NULL));

        // Step-2 fusion: compact adjacent same-slot SET;GET (before the
        // tail-call / selector peepholes; no-op when no safe candidate).
        compactFuseSetGet(codeStart);

        // #498: dump bytecode + slot map for any function named "final"
        // or "prev" (extends's `final:` lambda + its inner LetRec thunk).
        // Use V3_DBG_DUMP_FINAL=1.
        if (std::getenv("V3_DBG_DUMP_FINAL") && (f.name == "final" || f.name == "prev")) {
            std::fprintf(stderr,
                "v3 emit dump: fid=%u name='final' nUp=%u paramVar=%u argName=%u\n",
                (unsigned)fid, (unsigned)f.freeVars.size(),
                (unsigned)f.paramVar,
                (unsigned)f.argName);
            std::fprintf(stderr, "  freeVars=[");
            for (auto fv : f.freeVars) std::fprintf(stderr, "%u,", (unsigned)fv);
            std::fprintf(stderr, "]\n");
            std::fprintf(stderr, "  slot map (varId → slot):\n");
            for (auto & [vid, slot] : fc.slot)
                std::fprintf(stderr, "    var=%u → slot %u\n",
                    (unsigned)vid, (unsigned)slot);
            // Disasm the body we just emitted
            uint32_t codeEnd = static_cast<uint32_t>(unit.code.size());
            std::fprintf(stderr, "  bytecode [%u..%u):\n", codeStart, codeEnd);
            disassembleWindow(stderr, unit, codeStart, codeEnd);
        }

        // Tail-call peephole: rewrite OP_CALL → OP_TAIL_CALL whenever
        // the call's result IS this function's return value.  Three
        // shapes need to be caught:
        //
        //   (a) `body = f x`            — last instruction is OP_CALL.
        //   (b) `if c then result       — elseBlock's tail is OP_CALL;
        //        else f x`                with the trailing OP_JUMP
        //                                  target just before OP_RETURN.
        //   (c) `if c then f x          — thenBlock's tail is OP_CALL,
        //        else result`             then OP_JUMP to past the
        //                                  elseBlock to the OP_RETURN.
        //
        // Pre-fix only (a)+(b) fired (the back of unit.code is
        // elseBlock's last instruction).  The thenBlock's OP_CALL is
        // followed by an OP_JUMP and never gets the rewrite, so
        // recursion of the form `if cond then f x else result` leaks
        // C-stack on a common idiom (REVIEW MED-2).
        //
        // Strategy: after the function body is emitted but before
        // the trailing OP_RETURN, walk every OP_CALL in this
        // function's range and rewrite to OP_TAIL_CALL when:
        //   - it's the back of the code (case a), OR
        //   - the immediately following instruction is OP_JUMP whose
        //     target == codeEnd (the OP_RETURN slot we're about to
        //     emit).
        if (fid != 0 && unit.code.size() > codeStart) {
            uint32_t codeEnd = static_cast<uint32_t>(unit.code.size());
            // Case (a): last instruction is OP_CALL / OP_CALL_N.  For
            // OP_CALL_N the arg-count operand must be preserved.
            if (decodeOp(unit.code.back()) == OP_CALL)
                unit.code.back() = encode(OP_TAIL_CALL);
            else if (decodeOp(unit.code.back()) == OP_CALL_N)
                unit.code.back() = encode(OP_TAIL_CALL_N,
                                          decodeOperand(unit.code.back()));
            // Cases (b) + (c): walk function body for OP_CALL / OP_CALL_N
            // followed by OP_JUMP-to-codeEnd.  OP_JUMP encodes its
            // absolute 24-bit target in the operand (see vm.cc
            // OP_JUMP dispatch: `ip = operand`).  When the target
            // equals codeEnd (the slot the OP_RETURN we're about to
            // emit will occupy), the call is in tail position.
            for (uint32_t ip = codeStart; ip + 1 < unit.code.size(); ++ip) {
                Op cop = decodeOp(unit.code[ip]);
                if (cop != OP_CALL && cop != OP_CALL_N) continue;
                uint32_t nip = ip + 1;
                if (decodeOp(unit.code[nip]) != OP_JUMP) continue;
                uint32_t target = decodeOperand(unit.code[nip]);
                if (target == codeEnd)
                    unit.code[ip] = cop == OP_CALL
                        ? encode(OP_TAIL_CALL)
                        : encode(OP_TAIL_CALL_N, decodeOperand(unit.code[ip]));
            }
        }
        unit.code.push_back(encode(fid == 0 ? OP_HALT : OP_RETURN));

        // reg-VM Phase 5: if the function ends `GET_LOCAL s; OP_RETURN` and NO
        // branch targets the OP_RETURN, the return value is unconditionally in
        // slot s (not left on the operand stack by a branch) → fuse to
        // OP_R_RETURN s.  A straight-line function then runs with NO operand-
        // stack traffic.  A branch to the GET_LOCAL is fine (R_RETURN s returns
        // slot s identically); only a branch to the RETURN (a path that left
        // its value on the stack) blocks the rewrite.  Gate NIX_V3_NO_R_RETURN.
        {
            static const bool s_noRReturn =
                std::getenv("NIX_V3_NO_R_RETURN") != nullptr;
            uint32_t retPos = static_cast<uint32_t>(unit.code.size()) - 1;
            if (!s_noRReturn && fid != 0 && retPos >= codeStart + 1
                && decodeOp(unit.code[retPos]) == OP_RETURN
                && decodeOp(unit.code[retPos - 1]) == OP_GET_LOCAL) {
                bool retIsTarget = false;
                for (uint32_t ip = codeStart; ip < retPos && !retIsTarget; ++ip) {
                    Op cop = decodeOp(unit.code[ip]);
                    if ((cop == OP_JUMP || cop == OP_BRANCH_FALSE
                         || cop == OP_BRANCH_TRUE || cop == OP_AND_BRANCH
                         || cop == OP_OR_BRANCH || cop == OP_IMPL_BRANCH)
                        && decodeOperand(unit.code[ip]) == retPos)
                        retIsTarget = true;
                }
                if (!retIsTarget) {
                    unit.code[retPos - 1] =
                        encode(OP_R_RETURN, decodeOperand(unit.code[retPos - 1]));
                    unit.code.pop_back();   // drop the now-redundant OP_RETURN
                }
            }
        }

        // WS5-B2 (D2b): build into the LambdaTable's heap-owning STAGING form
        // (`LambdaBuild`).  `compile()` calls `unit.lambdas.finalize()` after
        // emitAll to pack all staging entries into the flat, borrowable block.
        // `lb` is stable for this emitFunction call (only this fid is built
        // here; other fids resize build_ in their own calls).
        LambdaBuild & lb = unit.lambdas.buildAt(fid);
        if (unit.lambdaCodeOffsets.size() <= fid) unit.lambdaCodeOffsets.resize(fid + 1);
        lb.codeOffset     = codeStart;
        lb.prologueOffset = codeStart;
        lb.nUpvalues      = static_cast<uint16_t>(f.freeVars.size());
        lb.nLocals        = fc.nLocals;
        lb.arity          = static_cast<uint8_t>((f.argName != ir::kInvalidSymbol ? 1 : (f.hasFormals ? 1 : 0)) + f.extraParams.size());
        lb.hasFormals     = static_cast<uint8_t>(f.hasFormals ? 1 : 0);
        lb.ellipsis       = static_cast<uint8_t>(f.ellipsis ? 1 : 0);
        // #530 lexical-with chain — mirror the count from ir::Function
        // (populated by the lowerer).  Runtime uses this to consume the
        // with-target block before the upvalue block at OP_MAKE_CLOSURE /
        // OP_MAKE_THUNK.
        lb.nWithTargets   = f.nWithTargets;
        lb.name           = f.name;
        lb.contextualName = f.contextualName;
        lb.posHandle      = f.posHandle;
        // #495: native-intrinsic kind (0=None, 1=Fix, 2=Extends, ...) -- when
        // set, OP_CALL on a closure with this descriptor dispatches to a
        // v3-native impl.  Carried through from ir::Function which lower.cc
        // structurally-matched at lower-time.  (WS5-B2: the retired
        // `astLambda` field is no longer copied — it has no readers.)
        lb.intrinsicKind  = static_cast<LambdaDescriptor::Intrinsic>(f.intrinsicKind);
        // STG-13b (#509/#511): upvalue indices for ExtendsBody / ComposeBody
        // native dispatch -- populated below (depend on freeVars search).
        lb.intrinsicVar0  = -1;
        lb.intrinsicVar1  = -1;
        lb.intrinsicVar2  = -1;
        // P2.1 step-0 measure (2026-07-02, TEMPORARY): carry the formal-
        // wrapper tag from ir::Function into the descriptor.
        lb.isFormalWrapper = f.isFormalWrapper;
        // P2.3 step-0 measure (2026-07-02, TEMPORARY): same for the §4.3 classes.
        lb.isOrDefault = f.isOrDefault;
        lb.isInheritWrapper = f.isInheritWrapper;
        // STG-13b (#509/#511): for ExtendsBody / ComposeBody dispatch,
        // find the upvalue index of each captured VarId by searching
        // freeVars.  Linear search is fine -- freeVars typically has 2
        // (Extends) or 3 (Compose) entries for these intrinsics.
        if (f.intrinsicKind == 5 /*ExtendsBody*/
            || f.intrinsicKind == 6 /*ComposeBody*/) {
            auto findIdx = [&](ir::VarId v) -> int8_t {
                if (v == ir::kInvalid) return -1;
                for (size_t i = 0; i < f.freeVars.size(); ++i)
                    if (f.freeVars[i] == v) return static_cast<int8_t>(i);
                return -1;
            };
            auto & desc = lb;
            desc.intrinsicVar0 = findIdx(f.intrinsicVar0);
            desc.intrinsicVar1 = findIdx(f.intrinsicVar1);
            desc.intrinsicVar2 = findIdx(f.intrinsicVar2);
            // If any required var didn't make it into freeVars (could
            // happen if optimization rewrites the body and elides the
            // capture), demote to None so we fall back to bytecode.
            // Native dispatch requires ALL named captures to be
            // resolvable; partial info would mis-index.
            bool ok = (f.intrinsicKind == 5)
                ? (desc.intrinsicVar0 >= 0 && desc.intrinsicVar1 >= 0)
                : (desc.intrinsicVar0 >= 0 && desc.intrinsicVar1 >= 0
                   && desc.intrinsicVar2 >= 0);
            if (!ok) {
                static const bool s_dbg =
                    std::getenv("V3_DBG_INTRINSIC") != nullptr;
                if (s_dbg) std::fprintf(stderr,
                    "v3 emit: demoting intrinsic kind=%u for fid=%u "
                    "name='%s' (capture not in freeVars: var0=%d var1=%d var2=%d)\n",
                    (unsigned)f.intrinsicKind, (unsigned)fid,
                    f.name.c_str(),
                    (int)desc.intrinsicVar0, (int)desc.intrinsicVar1,
                    (int)desc.intrinsicVar2);
                desc.intrinsicKind = LambdaDescriptor::Intrinsic::None;
                desc.intrinsicVar0 = desc.intrinsicVar1 = desc.intrinsicVar2 = -1;
            }
        }
        if (f.hasFormals) {
            auto & desc = lb;
            desc.formals.reserve(f.formals.size());
            for (auto & fm : f.formals)
                desc.formals.push_back({fm.name, fm.hasDefault, fm.pos});
            if (desc.formals.size() > 1) {
                std::sort(desc.formals.begin(), desc.formals.end(),
                    [](const LambdaDescriptor::Formal & a,
                       const LambdaDescriptor::Formal & b) {
                        return a.name < b.name;
                    });
            }
        }

        // #424: selector lambda specialisation peephole.  Detect the
        // canonical bytecode shape for `\x: x.f`:
        //
        //   OP_GET_LOCAL 0            (paramVar; OP_ATTRS_SELECT
        //                              forces internally)
        //   OP_ATTRS_SELECT [sym]
        //   [icIdx]                   (uint32_t follow word)
        //   OP_RETURN
        //
        // and record the projected SymbolId on the descriptor.  OP_CALL
        // takes a fast path on these (force arg, project, push) without
        // allocating a frame.  Only fires for arity-1 simple-arg lambdas
        // (no formals) with no upvalues -- matches `(p: p.name)` and
        // similar map/filter callbacks that dominate nixpkgs.
        //
        // OP_GET_LOCAL_FORCE 0 is also accepted for the same shape:
        // when the IR has an explicit Force around the paramVar (rare
        // but possible if the lowerer adds it for some path).
        //
        // IR Phase E (2026-05-18): default-on selector-lambda
        // recognition.  Previously gated by NIX_V3_SELECTOR_LAMBDA=1
        // ("Once stable, flip default ON" — the recognition has now
        // soaked through the full v3 test matrix without false
        // positives).  The opt-out gate `NIX_V3_NO_SELECTOR_LAMBDA=1`
        // exists for A/B perf measurement only; correctness is
        // guaranteed by the structural peephole (4-instruction body:
        // OP_GET_LOCAL[_FORCE] 0, OP_ATTRS_SELECT [sym], [icIdx],
        // OP_RETURN — no nested control flow, no captures).
        static const bool noSelectorLambda =
            std::getenv("NIX_V3_NO_SELECTOR_LAMBDA") != nullptr;
        if (!noSelectorLambda
            && fid != 0
            && f.argName != ir::kInvalidSymbol
            && !f.hasFormals
            && f.freeVars.empty()
            && unit.code.size() == codeStart + 4)
        {
            const Instruction i0 = unit.code[codeStart];
            const Instruction i1 = unit.code[codeStart + 1];
            // i2 is the IC slot index (uint32_t follow word)
            const Instruction i3 = unit.code[codeStart + 3];
            const Op op0 = decodeOp(i0);
            if ((op0 == OP_GET_LOCAL || op0 == OP_GET_LOCAL_FORCE)
                && decodeOperand(i0) == 0
                && decodeOp(i1) == OP_ATTRS_SELECT
                && decodeOp(i3) == OP_RETURN)
            {
                uint32_t sym = decodeOperand(i1);
                // SymbolId 0 is kInvalidSymbol -- never a real attr
                // name, so reserved as the "not a selector" sentinel.
                if (sym != 0)
                    lb.selectorSym = sym;
            }
        }

        // Phase 1.2 identity-lambda detection: body is exactly
        //   OP_GET_LOCAL[_FORCE] 0
        //   OP_RETURN
        // Two instructions, no frame setup needed at call time —
        // arg substitutes directly.  Critical for deep App spines
        // like `id (id (id ... 0))` that otherwise push 5000 frames
        // and hit kMaxCallDepth.  Same gating conditions as the
        // selectorSym detection above (non-top-level, single param,
        // no formals, no captures) to keep the recognition narrow.
        if (fid != 0
            && f.argName != ir::kInvalidSymbol
            && !f.hasFormals
            && f.freeVars.empty()
            && unit.code.size() == codeStart + 2)
        {
            const Instruction i0 = unit.code[codeStart];
            const Instruction i1 = unit.code[codeStart + 1];
            const Op op0 = decodeOp(i0);
            if ((op0 == OP_GET_LOCAL || op0 == OP_GET_LOCAL_FORCE)
                && decodeOperand(i0) == 0
                && decodeOp(i1) == OP_RETURN)
            {
                lb.identityLambda = true;
            }
        }

        // Phase 3.1 (PLAN_BEAT_TW 2026-06-12): the R_RETURN peephole above
        // (line ~2337) has ALREADY fused the canonical identity body
        //   OP_GET_LOCAL 0 ; OP_RETURN
        // into the single instruction `OP_R_RETURN 0` by the time the 2-insn
        // detector above runs — so it never matched a real `x: x` / `lib.id` /
        // genList-gen callback, and every such call paid a private nested
        // dispatchLoop re-entry (measured: 1 of foldl's 2 per-element re-entries).
        // Detect the post-peephole 1-insn form too.  `OP_R_RETURN 0` as the sole
        // body instruction, with a single param + no formals + no captures, can
        // ONLY have come from `GET_LOCAL 0 ; RETURN` (the peephole's precondition)
        // → it is exactly the identity lambda; no false positives.  Restores the
        // no-frame identity fast paths (vm.cc OP_CALL / op_force_slow / valueEq).
        // (The peephole only fuses OP_GET_LOCAL, not OP_GET_LOCAL_FORCE, so the
        // forced-arg identity body is still caught by the 2-insn detector above.)
        if (fid != 0
            && f.argName != ir::kInvalidSymbol
            && !f.hasFormals
            && f.freeVars.empty()
            && unit.code.size() == codeStart + 1)
        {
            const Instruction i0 = unit.code[codeStart];
            if (decodeOp(i0) == OP_R_RETURN && decodeOperand(i0) == 0)
                lb.identityLambda = true;
        }

        // mapAttrs identity-value callback detection.  The common
        // `name: value: value` mapper is an arity-2 closure after eval/apply
        // collapse and compiles to a direct return of slot 1.  Mark it so
        // primMapAttrs can return the source attrset directly without copying
        // the binding table or installing one lazy App3/MapAttrs cell per entry.
        if (fid != 0
            && f.argName != ir::kInvalidSymbol
            && f.extraParams.size() == 1
            && !f.hasFormals
            && f.freeVars.empty()
            && unit.code.size() == codeStart + 2)
        {
            const Instruction i0 = unit.code[codeStart];
            const Instruction i1 = unit.code[codeStart + 1];
            if (decodeOp(i0) == OP_GET_LOCAL
                && decodeOperand(i0) == 1
                && decodeOp(i1) == OP_RETURN)
            {
                lb.secondArgIdentityLambda = true;
            }
        }
        if (fid != 0
            && f.argName != ir::kInvalidSymbol
            && f.extraParams.size() == 1
            && !f.hasFormals
            && f.freeVars.empty()
            && unit.code.size() == codeStart + 1)
        {
            const Instruction i0 = unit.code[codeStart];
            if (decodeOp(i0) == OP_R_RETURN && decodeOperand(i0) == 1)
                lb.secondArgIdentityLambda = true;
        }

        unit.lambdaCodeOffsets[fid] = codeStart;

        ctx = nullptr;

        if (fid == 0)
            unit.entryOffset = codeStart;
    }

    void emitAll()
    {
        // #781b (2026-05-23): no longer mirror the global table into
        // unit.symbolTable.  Audit shows ZERO callers index into
        // cu.symbolTable post-compile; serialize::serializeCU reads
        // names directly from ir::globalSymbolTable() (with cu.
        // symbolTable as a fallback only if it happens to be set
        // by an older caller).  Dropping the copy saves ~1 MB of
        // std::string allocations per CU on a process where the
        // global table has grown to 50 K symbols — meaningful on
        // hello.drvPath's 269 import compiles (~270 MB of compile-
        // time alloc churn).  Keep the field for ABI compatibility
        // with any code that constructs a CU outside emit.

        // Emit inner functions first so their descriptors and code are
        // available before the top-level (which references them via
        // OP_MAKE_CLOSURE / OP_MAKE_THUNK).  Top-level is functions[0].
        for (ir::FuncId i = 1; i < m.functions.size(); ++i)
            emitFunction(i);
        emitFunction(0);
    }
};

} // namespace

CompilationUnit compile(const ir::Module & m)
{
    Emitter e(m);
    e.emitAll();
    // WS5-B2 (D2b): pack the per-function LambdaBuild staging into the flat,
    // borrowable descriptor block.  Must run before any read of e.unit.lambdas
    // (the disasm dump below + serialize + the VM).
    e.unit.lambdas.finalize();

    // Observability: full-CU bytecode disassembly dump for STATIC opcode /
    // n-gram analysis (consumed by `bench/analyze-bytecode.py`).  Gated by
    // NIX_V3_EMIT_BYTECODE; optional NIX_V3_EMIT_BYTECODE_OUT=<path> appends
    // to a file (default stderr).  This is the single chokepoint every CU
    // passes through — top-level AND each imported module — so one hook
    // covers the whole compiled corpus.  For full static coverage run with
    // NIX_V3_NO_DISK_CACHE=1, since a warm cache deserializes CUs and skips
    // compile() entirely.  The `=== v3-bytecode CU ... ===` delimiter line
    // is the CU boundary `bench/analyze-bytecode.py` keys on (CU_RE) — keep
    // its exact format.  `disassembleModule` then emits the per-function
    // framing + resolved operands + jump labels (its extra `; func` / `L:` /
    // `; module` lines don't match the analyzer's INSN/CU regexes, so they
    // are skipped).  `v3-eval --emit-bytecode` shares disassembleModule.
    if (std::getenv("NIX_V3_EMIT_BYTECODE")) {
        std::FILE * out = stderr;
        bool closeOut = false;
        if (const char * p = std::getenv("NIX_V3_EMIT_BYTECODE_OUT")) {
            if (std::FILE * f = std::fopen(p, "a")) { out = f; closeOut = true; }
        }
        std::fprintf(out, "=== v3-bytecode CU functions=%zu code=%zu ===\n",
                     e.unit.lambdas.size(), e.unit.code.size());
        disassembleModule(out, e.unit);
        if (closeOut) std::fclose(out);
    }

    return std::move(e.unit);
}

} // namespace nix::v3
