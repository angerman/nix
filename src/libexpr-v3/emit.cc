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

    struct FuncCtx
    {
        ir::FuncId                          fid;
        std::unordered_map<ir::VarId, uint16_t> slot;
        std::unordered_map<ir::VarId, uint16_t> upvalue;
        uint16_t nextSlot = 0;
        uint16_t nLocals  = 0;
    };
    FuncCtx * ctx = nullptr;

    Emitter(const ir::Module & mod) : m(mod) {}

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

        // Optimisation: when the very last binding's VarId is the
        // block's TermReturn value, the binding's expression result
        // is already on top of the operand stack right after we
        // emit it.  Emit the trailing SET_LOCAL only if a slot was
        // previously assigned (someone else might reference this
        // var), but skip the SET+GET round-trip and leave the value
        // on the stack — the function-tail OP_CALL→OP_TAIL_CALL
        // peephole then has a chance to fire.
        const size_t nBd = b.bindings.size();
        bool tailLast = nBd > 0 && b.bindings.back().var == ret.value;

        // Params have already been bound by the caller (e.g., function
        // prologue assigned param-VarId -> slot 0).
        for (size_t i = 0; i < nBd; ++i) {
            auto & bd = b.bindings[i];
            emitExpr(bd.expr);
            // For the tail binding (last binding == term value), the
            // emitted bytecode already left the value on the operand
            // stack.  Skip the SET + trailing GET round-trip — slots
            // are pre-assigned by preassignSlotsInBlock but only
            // referenced when something explicitly emits OP_GET_LOCAL
            // for them; nothing in this block does.  Other blocks
            // can't reach this var (it's bound only here).
            bool isTail = tailLast && (i + 1 == nBd);
            if (isTail) continue;
            uint16_t slot = getOrAssignSlot(bd.var);
            unit.code.push_back(encode(OP_SET_LOCAL, slot));
        }

        // Terminal: TermReturn.  If we elided the SET for the tail
        // binding, the value is already on top — skip the trailing
        // emitVarRef.
        if (ret.value != ir::kInvalid && !tailLast)
            emitVarRef(ret.value);
        else if (ret.value == ir::kInvalid)
            unit.code.push_back(encode(OP_LIT_NULL));
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
        for (auto fv : e.freeVars) emitVarRef(fv);
        unit.code.push_back(encode(OP_MAKE_CLOSURE, e.funcIdx));
        unit.code.push_back(static_cast<uint32_t>(e.freeVars.size()));
    }
    void emitOne(const ir::MkThunk & e)
    {
        for (auto fv : e.freeVars) emitVarRef(fv);
        unit.code.push_back(encode(OP_MAKE_THUNK, e.funcIdx));
        unit.code.push_back(static_cast<uint32_t>(e.freeVars.size()));
    }
    void emitOne(const ir::App & e)
    {
        emitVarRef(e.fun); emitVarRef(e.arg);
        unit.code.push_back(encode(OP_CALL));
    }
    void emitOne(const ir::Force & e)
    {
        // Fuse `Force(VarRef)` into a single superinstruction: every
        // variable reference in the AST→IR lowering goes through this
        // path, so this is the most common bytecode pair (~25-40% of
        // instructions on benchmarks like fib).
        //
        // Each emitted force-flavoured opcode also records a side-
        // table entry mapping the bytecode offset back to the
        // lower.cc line that produced this `ir::Force`.  Zero runtime
        // cost when V3_DBG_FORCE_SITE is unset (the table is read
        // only by that env-gated trace path in vm.cc).
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
    void emitOne(const ir::Add & e)  { emitVarRef(e.lhs); emitVarRef(e.rhs); unit.code.push_back(encode(OP_ADD)); }
    void emitOne(const ir::Sub & e)  { emitVarRef(e.lhs); emitVarRef(e.rhs); unit.code.push_back(encode(OP_SUB)); }
    void emitOne(const ir::Mul & e)  { emitVarRef(e.lhs); emitVarRef(e.rhs); unit.code.push_back(encode(OP_MUL)); }
    void emitOne(const ir::Div & e)  { emitVarRef(e.lhs); emitVarRef(e.rhs); unit.code.push_back(encode(OP_DIV)); }
    void emitOne(const ir::Eq  & e)  { emitVarRef(e.lhs); emitVarRef(e.rhs); unit.code.push_back(encode(OP_EQ));  }
    void emitOne(const ir::NEq & e)  { emitVarRef(e.lhs); emitVarRef(e.rhs); unit.code.push_back(encode(OP_NEQ)); }
    void emitOne(const ir::Less& e)  { emitVarRef(e.lhs); emitVarRef(e.rhs); unit.code.push_back(encode(OP_LESS));}
    void emitOne(const ir::Not & e)  { emitVarRef(e.operand); unit.code.push_back(encode(OP_NOT)); }

    // -- Short-circuit
    void emitOne(const ir::And & e)
    {
        emitVarRef(e.lhs);
        uint32_t at = emitJumpPlaceholder(OP_AND_BRANCH);
        emitBlock(e.rhsBlock);
        patchJump(at, static_cast<uint32_t>(unit.code.size()));
    }
    void emitOne(const ir::Or & e)
    {
        emitVarRef(e.lhs);
        uint32_t at = emitJumpPlaceholder(OP_OR_BRANCH);
        emitBlock(e.rhsBlock);
        patchJump(at, static_cast<uint32_t>(unit.code.size()));
    }
    void emitOne(const ir::Impl & e)
    {
        emitVarRef(e.lhs);
        uint32_t at = emitJumpPlaceholder(OP_IMPL_BRANCH);
        emitBlock(e.rhsBlock);
        patchJump(at, static_cast<uint32_t>(unit.code.size()));
    }

    // -- If
    void emitOne(const ir::If & e)
    {
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
        emitVarRef(e.lhs); emitVarRef(e.rhs);
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
        for (auto & en : e.entries) emitVarRef(en.value);
        unit.code.push_back(encode(OP_ATTRS_INIT, static_cast<uint32_t>(e.entries.size())));
        for (auto & en : e.entries) {
            unit.code.push_back(en.name);
            unit.code.push_back(en.pos);
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
        emitVarRef(e.attrs); emitVarRef(e.nameVar);
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
        emitVarRef(e.attrs);
        unit.code.push_back(encode(OP_REC_BINDING_SLOT_REF, e.name));
    }
    void emitOne(const ir::HasAttr & e)
    {
        emitVarRef(e.attrs);
        unit.code.push_back(encode(OP_ATTRS_HAS, e.name));
    }
    void emitOne(const ir::HasAttrDyn & e)
    {
        emitVarRef(e.attrs); emitVarRef(e.nameVar);
        unit.code.push_back(encode(OP_ATTRS_HAS_DYN));
    }
    void emitOne(const ir::Update & e)
    {
        emitVarRef(e.lhs); emitVarRef(e.rhs);
        unit.code.push_back(encode(OP_ATTRS_UPDATE));
    }

    // -- Recursive let / rec attrset
    void emitOne(const ir::LetRec & e)
    {
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

        // 1. OP_ATTRS_REC_INIT[n] + n sorted (SymbolId, PosIdx) pairs:
        //    push placeholder rec Bindings on operand stack.
        unit.code.push_back(encode(OP_ATTRS_REC_INIT, n));
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
        for (auto & he : e.hiddenEntries) {
            const auto & ff = m.functions[he.thunkBody].freeVars;
            for (auto fv : ff) emitVarRef(fv);
            unit.code.push_back(encode(OP_MAKE_THUNK, he.thunkBody));
            unit.code.push_back(static_cast<uint32_t>(ff.size()));
            uint16_t hiddenSlot = getOrAssignSlot(he.hiddenVar);
            unit.code.push_back(encode(OP_SET_LOCAL, hiddenSlot));
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
            for (auto fv : ff) emitVarRef(fv);
            unit.code.push_back(encode(OP_MAKE_THUNK, en.thunkBody));
            unit.code.push_back(static_cast<uint32_t>(ff.size()));
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
