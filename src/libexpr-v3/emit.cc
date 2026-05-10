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
        if (auto it = ctx->slot.find(v); it != ctx->slot.end()) {
            unit.code.push_back(encode(OP_GET_LOCAL, it->second));
            return;
        }
        if (auto uit = ctx->upvalue.find(v); uit != ctx->upvalue.end()) {
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
        unit.stringConstants.emplace_back(s);
        return static_cast<uint32_t>(unit.stringConstants.size() - 1);
    }

    // Patch helpers ---------------------------------------------------------

    /// Emit a placeholder jump and return the index of the operand word
    /// (so the caller can patch in the absolute target later).
    uint32_t emitJumpPlaceholder(Op op)
    {
        unit.code.push_back(encode(op, 0));
        return static_cast<uint32_t>(unit.code.size() - 1);
    }

    void patchJump(uint32_t at, uint32_t target)
    {
        // Preserve the opcode byte; replace the 24-bit operand.
        Instruction prev = unit.code[at];
        unit.code[at] = (prev & 0xFF000000u) | (target & 0x00FFFFFFu);
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
        bool tailLast = nBd > 0 && b.bindings.back().var == ret.value;

        for (size_t i = 0; i < nBd; ++i) {
            auto & bd = b.bindings[i];
            bool isTail = tailLast && (i + 1 == nBd);

            emitExpr(bd.expr);

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
        for (auto wv : e.lexicalWiths) emitVarRef(wv);
        for (auto fv : e.freeVars) emitVarRef(fv);
        unit.code.push_back(encode(OP_MAKE_CLOSURE, e.funcIdx));
        unit.code.push_back(static_cast<uint32_t>(e.freeVars.size()));
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
        for (auto wv : e.lexicalWiths) emitVarRef(wv);
        for (auto fv : e.freeVars) emitVarRef(fv);
        unit.code.push_back(encode(OP_MAKE_THUNK, e.funcIdx));
        unit.code.push_back(static_cast<uint32_t>(e.freeVars.size()));
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
        uint32_t at = emitJumpPlaceholder(OP_AND_BRANCH);
        emitBlock(e.rhsBlock);
        patchJump(at, static_cast<uint32_t>(unit.code.size()));
    }
    void emitOne(const ir::Or & e)
    {
        if (!tryFastPathUnary(e.lhs))
            emitVarRef(e.lhs);
        uint32_t at = emitJumpPlaceholder(OP_OR_BRANCH);
        emitBlock(e.rhsBlock);
        patchJump(at, static_cast<uint32_t>(unit.code.size()));
    }
    void emitOne(const ir::Impl & e)
    {
        if (!tryFastPathUnary(e.lhs))
            emitVarRef(e.lhs);
        uint32_t at = emitJumpPlaceholder(OP_IMPL_BRANCH);
        emitBlock(e.rhsBlock);
        patchJump(at, static_cast<uint32_t>(unit.code.size()));
    }

    // -- If
    void emitOne(const ir::If & e)
    {
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
        // Detect duplicates at lower-time so we reject earlier than
        // OP_ATTRS_INIT's runtime dup check would (matches the prior
        // OP_ATTRS_INIT path which was an emit of [push N values];
        // OP_ATTRS_INIT N).
        for (size_t k = 1; k < n; ++k) {
            if (e.entries[sortedIdx[k]].name
                == e.entries[sortedIdx[k - 1]].name)
            {
                // Defer the throw to runtime so the error message is
                // identical to the OP_ATTRS_INIT path's.  Just allow
                // the duplicate trailer here; OP_ATTRS_REC_SET to the
                // same slot twice is harmless (last write wins).
                break;
            }
        }
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
        unit.code.push_back(encode(OP_ATTRS_REC_INIT, static_cast<uint32_t>(n)));
        for (uint32_t k = 0; k < n; ++k) {
            const auto & en = e.entries[sortedIdx[k]];
            unit.code.push_back(en.name);
            unit.code.push_back(en.pos);
        }
        // Emit per-entry value-push + REC_SET <sorted_slot>.  We emit
        // in SORT order so values are pushed and consumed adjacently
        // (no transient stack ordering issues).
        for (uint32_t k = 0; k < n; ++k) {
            const auto & en = e.entries[sortedIdx[k]];
            emitVarRef(en.value);
            unit.code.push_back(encode(OP_ATTRS_REC_SET, k));
        }
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
        uint32_t icIdx = static_cast<uint32_t>(unit.attrSelectCache.size());
        unit.attrSelectCache.emplace_back();
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
        // #542 unary fast path.
        if (!tryFastPathUnary(e.attrs))
            emitVarRef(e.attrs);
        unit.code.push_back(encode(OP_REC_BINDING_SLOT_REF, e.name));
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
        unit.code.push_back(encode(OP_ATTRS_UPDATE));
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

    void emitOne(const ir::PrimOpCall & e)
    {
        for (auto v : e.args) emitVarRef(v);
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
        if (!visited.insert(bid).second) return;
        const ir::Block & b = m.blocks[bid];
        for (auto & bd : b.bindings) {
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
        // Upvalue order = freeVars.
        for (uint16_t i = 0; i < f.freeVars.size(); ++i)
            fc.upvalue[f.freeVars[i]] = i;
        // #498: trace freeVars for body emit of "res" with 4 freeVars.
        if (std::getenv("V3_DBG_RES_FREEVARS")
            && f.name == "res" && f.freeVars.size() == 4) {
            std::fprintf(stderr,
                "v3 emit body res: fid=%u freeVars=[",
                (unsigned)fid);
            for (auto fv : f.freeVars) std::fprintf(stderr, "%u,", (unsigned)fv);
            std::fprintf(stderr, "]\n");
        }
        // Pre-assign slots for every VarId reachable from the entry
        // block (including in sub-blocks: if-branches, with bodies, etc.)
        // so forward references resolve at emit time.
        if (f.entryBlock != ir::kInvalidBlock) {
            std::unordered_set<ir::BlockId> visited;
            preassignSlotsInBlock(fc, f.entryBlock, visited);
        }

        ctx = &fc;
        uint32_t codeStart = static_cast<uint32_t>(unit.code.size());

        if (f.entryBlock != ir::kInvalidBlock)
            emitBlock(f.entryBlock);
        else
            unit.code.push_back(encode(OP_LIT_NULL));

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
            // Case (a): last instruction is OP_CALL.
            if (decodeOp(unit.code.back()) == OP_CALL)
                unit.code.back() = encode(OP_TAIL_CALL);
            // Cases (b) + (c): walk function body for OP_CALL
            // followed by OP_JUMP-to-codeEnd.  OP_JUMP encodes its
            // absolute 24-bit target in the operand (see vm.cc
            // OP_JUMP dispatch: `ip = operand`).  When the target
            // equals codeEnd (the slot the OP_RETURN we're about to
            // emit will occupy), the OP_CALL is in tail position.
            for (uint32_t ip = codeStart; ip + 1 < unit.code.size(); ++ip) {
                if (decodeOp(unit.code[ip]) != OP_CALL) continue;
                uint32_t nip = ip + 1;
                if (decodeOp(unit.code[nip]) != OP_JUMP) continue;
                uint32_t target = decodeOperand(unit.code[nip]);
                if (target == codeEnd)
                    unit.code[ip] = encode(OP_TAIL_CALL);
            }
        }
        unit.code.push_back(encode(fid == 0 ? OP_HALT : OP_RETURN));

        if (unit.lambdas.size() <= fid)         unit.lambdas.resize(fid + 1);
        if (unit.lambdaCodeOffsets.size() <= fid) unit.lambdaCodeOffsets.resize(fid + 1);
        unit.lambdas[fid] = LambdaDescriptor{
            .codeOffset     = codeStart,
            .prologueOffset = codeStart,
            .nUpvalues      = static_cast<uint16_t>(f.freeVars.size()),
            .nLocals        = fc.nLocals,
            .arity          = static_cast<uint8_t>(f.argName != ir::kInvalidSymbol ? 1 : (f.hasFormals ? 1 : 0)),
            .hasFormals     = static_cast<uint8_t>(f.hasFormals ? 1 : 0),
            .ellipsis       = static_cast<uint8_t>(f.ellipsis ? 1 : 0),
            // #530 lexical-with chain — mirror the count from
            // ir::Function (populated by the lowerer).  Runtime uses
            // this to consume the with-target block before the
            // upvalue block at OP_MAKE_CLOSURE / OP_MAKE_THUNK.
            .nWithTargets   = f.nWithTargets,
            .formals        = {},
            .name           = f.name,
            .posHandle      = f.posHandle,
            // #495: native-intrinsic kind (0=None, 1=Fix, 2=Extends, ...)
            // -- when set, OP_CALL on a closure with this descriptor
            // dispatches to a v3-native impl that evaluates the entire
            // fix-point machinery without TW round-trips.  Carried
            // through from ir::Function which lower.cc structurally-
            // matched at lower-time.  (Ordered before .astLambda to
            // match LambdaDescriptor's field declaration order --
            // designated-initializer requirement under -Wreorder-init-list.)
            .intrinsicKind  = static_cast<LambdaDescriptor::Intrinsic>(f.intrinsicKind),
            // STG-13b (#509/#511): upvalue indices for ExtendsBody /
            // ComposeBody native dispatch -- populated below after the
            // designated initializer (depend on freeVars search).
            .intrinsicVar0  = -1,
            .intrinsicVar1  = -1,
            .intrinsicVar2  = -1,
            // #493: original ExprLambda* (or nullptr for synthesised
            // thunks).  Used by v3ToTreeWalker to construct TW Tag::tLambda
            // when bridging a formals closure back -- preserves
            // autoCallFunction's formals introspection through the bridge.
            // Disk-cache-loaded descriptors get nullptr (AST is gone after
            // lowering); the bridge falls back to refusal in that case.
            .astLambda      = f.astLambda,
        };
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
            auto & desc = unit.lambdas[fid];
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
            auto & desc = unit.lambdas[fid];
            desc.formals.reserve(f.formals.size());
            for (auto & fm : f.formals)
                desc.formals.push_back({fm.name, fm.hasDefault, fm.pos});
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
        // Detection gated by NIX_V3_SELECTOR_LAMBDA=1 while we
        // shake out shape-mismatch false positives.  Once stable, flip
        // default ON.
        static const bool selectorLambda =
            std::getenv("NIX_V3_SELECTOR_LAMBDA") != nullptr;
        if (selectorLambda
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
                    unit.lambdas[fid].selectorSym = sym;
            }
        }

        unit.lambdaCodeOffsets[fid] = codeStart;

        ctx = nullptr;

        if (fid == 0)
            unit.entryOffset = codeStart;
    }

    void emitAll()
    {
        // Mirror the IR symbol table into the CompilationUnit so the VM
        // can use SymbolId at runtime without round-tripping to strings.
        // Copy the global symbol table so SymbolIds in this CU map to
        // the same names that any other CU in the process uses.
        unit.symbolTable = ir::globalSymbolTable();

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
    return std::move(e.unit);
}

} // namespace nix::v3
