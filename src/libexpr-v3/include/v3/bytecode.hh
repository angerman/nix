#pragma once
/// @file
/// v3 bytecode — opcodes + CompilationUnit.
///
/// Encoding: a single 32-bit Instruction.  Top 8 bits = opcode; low 24 bits =
/// operand (or 16+8 packed; opcode-specific).  Multi-word instructions
/// (e.g. CALL with N args, MAKE_CLOSURE with nUpvalues, JUMP with offset)
/// follow with extra 32-bit data words.
///
/// Per-frame stack model:
///   [params...]         provided by caller
///   [locals...]         allocated up-front from LambdaDescriptor::nLocals
///   [operand stack]     grows after locals; ephemeral
/// All slot indices in opcodes (OP_GET_LOCAL etc.) are FRAME-RELATIVE —
/// indexing into the locals array starting at stackBaseOffset.
///
/// JUMPS use absolute code offsets (not deltas) for easier debugging; the
/// emitter patches them at the second pass.
///
/// Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.
/// SPDX-License-Identifier: Apache-2.0

#include "v3/value.hh"
#include "v3/closure.hh"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

/// Stringify-then-paste helper used by EMIT_FORCE_AT below.
#ifndef V3_STRINGIFY
#define V3_STRINGIFY_INNER(x) #x
#define V3_STRINGIFY(x) V3_STRINGIFY_INNER(x)
#endif

namespace nix::v3 {

struct PrimOp;
struct Bindings;

using Instruction = uint32_t;

enum Op : uint8_t
{
    // 0x00 was OP_NOP — never emitted, removed in the review-cleanup
    // pass.  Reserved (don't reuse in case old disk caches are still
    // around in the wild).

    // --- Literals -------------------------------------------------------
    OP_LIT_INT        = 0x01,  // [imm:24]   small signed int
    OP_LIT_INT_BIG    = 0x02,  // [const:24] from intConstants
    OP_LIT_FLOAT      = 0x03,  // [const:24] from floatConstants
    OP_LIT_STR        = 0x04,  // [const:24] from stringConstants
    OP_LIT_PATH       = 0x05,  // [const:24] from stringConstants (with accessor table)
    OP_LIT_TRUE       = 0x06,
    OP_LIT_FALSE      = 0x07,
    OP_LIT_NULL       = 0x08,

    // --- Locals / upvalues ----------------------------------------------
    OP_GET_LOCAL      = 0x10,  // [slot:24]
    OP_SET_LOCAL      = 0x11,  // [slot:24]   pop into slot
    OP_GET_UPVALUE    = 0x12,  // [idx:24]    push closure->upvalues[idx]
    OP_DUP            = 0x13,
    // OP_POP / OP_SWAP: reserved opcode bytes -- no current emit path,
    // dispatch removed in vm.cc.  Don't reuse the values for new ops
    // until disk-cache schema bumps past kSchemaVersion=2.
    OP_POP            = 0x14,
    OP_SWAP           = 0x15,

    // --- Arithmetic ------------------------------------------------------
    OP_ADD            = 0x20,
    OP_SUB            = 0x21,
    OP_MUL            = 0x22,
    OP_DIV            = 0x23,
    // OP_NEGATE: reserved; lowered as `0 - x` via OP_SUB.  Dispatch
    // removed (see OP_POP / OP_SWAP above).
    OP_NEGATE         = 0x24,

    // --- Comparison -----------------------------------------------------
    OP_EQ             = 0x30,
    OP_NEQ            = 0x31,
    OP_LESS           = 0x32,

    // --- Boolean / control ----------------------------------------------
    OP_NOT            = 0x40,
    /// Short-circuit AND: if top is false, jump (leaving false on stack);
    /// otherwise pop and continue (the rhs's value will be the result).
    OP_AND_BRANCH     = 0x41,  // [target:24] — jump-if-false-keep
    OP_OR_BRANCH      = 0x42,  // [target:24] — jump-if-true-keep
    /// Implication: if top is false, jump (push true and leave on stack);
    /// otherwise pop and continue.
    OP_IMPL_BRANCH    = 0x43,  // [target:24]

    OP_JUMP           = 0x44,  // [target:24]
    OP_BRANCH_FALSE   = 0x45,  // [target:24]   pop, jump if false
    // OP_BRANCH_TRUE: reserved; the lowerer always emits OP_BRANCH_FALSE
    // with a negated condition.  Dispatch removed.
    OP_BRANCH_TRUE    = 0x46,  // [target:24]   pop, jump if true

    // --- Closures / calls / thunks --------------------------------------
    OP_MAKE_CLOSURE   = 0x50,  // [funcIdx:24]; data: nUpvalues; pops nUpvalues
    OP_MAKE_THUNK     = 0x51,  // [funcIdx:24]; data: nUpvalues; pops nUpvalues
    OP_CALL           = 0x52,  // single-arg call: pop arg, pop fun, push result
    OP_RETURN         = 0x53,  // pop result, return to caller
    OP_FORCE          = 0x54,  // pop, force (run if thunk), push WHNF value
    /// Superinstructions: fuse OP_GET_LOCAL/OP_GET_UPVALUE with OP_FORCE.
    /// Saves a dispatch + push+force on the hot pattern emitted by
    /// every IR `Force(VarRef)` — i.e., almost every variable reference
    /// in the current AST→IR lowering.
    OP_GET_LOCAL_FORCE   = 0x55,  // [slot:24]
    OP_GET_UPVALUE_FORCE = 0x56,  // [idx:24]
    /// Tail call: like OP_CALL, but reuses the current frame instead
    /// of pushing a new one.  Emitted at function tail position when
    /// the last instruction before OP_RETURN was OP_CALL — the
    /// callee's eventual OP_RETURN pops the (modified) current frame
    /// so the result lands at our caller.
    OP_TAIL_CALL      = 0x57,

    // --- Lists ----------------------------------------------------------
    OP_LIST_INIT      = 0x60,  // [n:24]   pop n elems, push list
    OP_LIST_CONCAT    = 0x61,  // pop b, pop a, push a ++ b

    // --- Attrsets -------------------------------------------------------
    OP_ATTRS_INIT     = 0x70,  // [n:24]   pop n values; data: n SymbolIds; build sorted attrset
    OP_ATTRS_INIT_DYN = 0x71,  // [nStatic:12, nDyn:12] then static syms then values then dyn name+value pairs (REVIEW B-14: was [16,8] in stale comment)
    OP_ATTRS_REC_INIT = 0x72,  // [n:24]   data: n SymbolIds — allocate placeholder Bindings,
                                //          push it on op stack with placeholders; entries are filled in by
                                //          subsequent OP_ATTRS_REC_SET ops
    OP_ATTRS_REC_SET  = 0x78,  // [i:24]   pop top (the entry value), peek bindings, write into entries[i].value
    OP_ATTRS_SELECT   = 0x73,  // [sym:24] pop attrs, push attrs[sym]
    OP_ATTRS_SELECT_DYN = 0x74, // pop name, pop attrs, push attrs[name]
    OP_ATTRS_HAS      = 0x75,  // [sym:24] pop attrs, push bool
    OP_ATTRS_HAS_DYN  = 0x76,
    OP_ATTRS_UPDATE   = 0x77,
    /// __overrides: if the attrset on top of the operand stack contains
    /// a `__overrides` attribute, force it (must be an attrset) and for
    /// each (name, value) in it replace the corresponding entry in the
    /// rec attrset.  No-op if `__overrides` is absent.  Result: the
    /// possibly-updated attrset stays on top of the stack.
    OP_APPLY_OVERRIDES = 0x79,

    // --- With -----------------------------------------------------------
    OP_WITH_PUSH      = 0x80,  // pop attrset, push it on with-stack
    OP_WITH_POP       = 0x81,
    OP_WITH_LOOKUP    = 0x82,  // [sym:24]; data: depth (0=innermost)
    /// SECD DUM/RAP: push a Tag::Slot Value onto the operand stack
    /// pointing at the local slot referenced by [slot:24].  Used when
    // 0x83 was OP_LOAD_SLOT_REF — Phase-3 scaffolding for a planned
    // `with E;`-on-let-rec-slot path that the WC-31/Phase-5 redesign
    // (RecBindingSlotRef + Tag::Slot deref in forceValue) made
    // unnecessary.  Never emitted by the compiler in the landed
    // pipeline; removed in the review-cleanup pass.

    /// SECD-style heap-stable slot reference: pop a Tag::Attrs (a
    /// rec-attrset's Bindings*), look up the entry by SymbolId, and
    /// push a Tag::Slot Value pointing at `&entries[i].value` —
    /// stable as long as the Bindings is alive.  Used by `with E;`
    /// when E resolves to a rec-attrset entry: sub-thunks captured
    /// in the with-body see the entry's live mutated value through
    /// the slot, including the memoized resolved value once forceValue
    /// has run on the entry once.  Mirrors tree-walker's `Value *`
    /// slot pointer into the Env block.
    OP_REC_BINDING_SLOT_REF = 0x84, // [sym:24]

    // --- Strings --------------------------------------------------------
    OP_STR_CONCAT     = 0x90,  // [n:24] forceString stored in low bit of n; pops n parts

    // --- Assert / pos ---------------------------------------------------
    OP_ASSERT         = 0xA0,  // pop bool; raise if false
    // OP_POS: reserved; lowerExpr skips ExprPos in v3 (positions are
    // recovered from the side table at error time).  Dispatch removed.
    OP_POS            = 0xA1,  // [posIdx:24]  push pos attrset

    /// Direct primop call.  [nArgs:24]; data: primop-table index.
    /// Pops nArgs from stack (in argument order: arg0, arg1, ...) and
    /// pushes the primop's result.
    OP_CALL_PRIMOP    = 0xB0,
    /// Push a Tag::PrimOp value pointing to cu->primops[idx].
    OP_LIT_PRIMOP     = 0xB1,
    /// Push the singleton `vBuiltins` Tag::Attrs value containing every
    /// registered primop.  Lazily materialised on first execution and
    /// reused across every reference for the lifetime of the process.
    OP_LIT_BUILTINS   = 0xB2,

    // --- #428 fast-path primop opcodes (Smalltalk primitiveFailed
    // pattern).  Each opcode is bug-compatible with the corresponding
    // C primop -- same forcing, same throws, same return shape -- it
    // just inlines the hot path into the dispatch loop, saving the
    // OP_CALL_PRIMOP indirection (~30 cycles -> 1-2 cycles for type
    // predicates).  Emitted in lieu of OP_CALL_PRIMOP when the lowerer
    // recognises the targeted primop pointer; the primop itself stays
    // registered for first-class uses (`map builtins.isAttrs xs`).
    //
    // Operand format: bare opcode (no operand bits).  Pops the args
    // off the operand stack, pushes the result.

    // Type predicates: pop one arg, force, push bool result.
    OP_IS_NULL        = 0xC0,
    OP_IS_BOOL        = 0xC1,
    OP_IS_INT         = 0xC2,
    OP_IS_FLOAT       = 0xC3,
    OP_IS_STRING      = 0xC4,
    OP_IS_PATH        = 0xC5,
    OP_IS_LIST        = 0xC6,
    OP_IS_ATTRS       = 0xC7,
    OP_IS_FUNCTION    = 0xC8,

    // List selectors: pop list (and index for OP_ELEM_AT), force,
    // do the selector, push.  Throw with the same error messages as
    // the primop on type/range failure.
    OP_HEAD           = 0xD0,
    OP_TAIL           = 0xD1,
    OP_LENGTH         = 0xD2,  // also handles strings (matches primLength)
    OP_ELEM_AT        = 0xD3,

    OP_HALT           = 0xFF,
};

constexpr inline Op decodeOp(Instruction i) noexcept
{
    return static_cast<Op>((i >> 24) & 0xFF);
}

constexpr inline uint32_t decodeOperand(Instruction i) noexcept
{
    return i & 0x00FFFFFF;
}

constexpr inline int32_t decodeSignedOperand(Instruction i) noexcept
{
    int32_t op = static_cast<int32_t>(i & 0x00FFFFFF);
    if (op & 0x00800000) op |= 0xFF000000;
    return op;
}

constexpr inline Instruction encode(Op op, uint32_t operand = 0) noexcept
{
    return (static_cast<uint32_t>(op) << 24) | (operand & 0x00FFFFFF);
}

// ---------------------------------------------------------------------------
// CompilationUnit
// ---------------------------------------------------------------------------

struct CompilationUnit
{
    /// Flat instruction stream.
    std::vector<Instruction> code;

    /// Constants pools.
    std::vector<int64_t>     intConstants;
    std::vector<double>      floatConstants;
    std::vector<std::string> stringConstants;

    /// Per-symbol-id (v3 IR SymbolId space) → string.  Mirrors the IR
    /// symbol table for runtime use (with-lookup, attr-name display).
    std::vector<std::string> symbolTable;

    /// Lambda descriptors, indexed by IR FuncId.  function 0 = top-level.
    std::vector<LambdaDescriptor> lambdas;
    std::vector<uint32_t>          lambdaCodeOffsets;

    /// Primops referenced by OP_CALL_PRIMOP, indexed by primop-table index.
    std::vector<const PrimOp *> primops;

    /// Inline cache slots for OP_ATTRS_SELECT.  Each OP_ATTRS_SELECT
    /// reserves an index here; the entry caches up to kWays recently
    /// observed (Bindings*, slot-in-Bindings) pairs so a repeat
    /// access skips the binary search.  Mutated at runtime; sized at
    /// compile time so slot indices are stable.
    ///
    /// EVAL-COMP §8.1: 4-way polymorphic IC.  Pre-fix had a single
    /// (Bindings*, slot) entry per call site; polymorphic sites like
    /// `map (p: p.name) [foo bar]` thrashed on every call.  4 ways
    /// catches the common shape-polymorphism patterns in nixpkgs
    /// (`mapAttrs` etc.) at modest memory cost (64 B per call site
    /// vs. 16 B previously).
    struct AttrSelectIC {
        static constexpr int kWays = 4;
        struct Entry {
            const Bindings * bindings = nullptr;
            uint32_t slot = 0;
        };
        Entry entries[kWays] = {};
        /// Round-robin replacement: index of the next slot to evict.
        /// Cheap (one byte, no LRU bookkeeping).  Real LRU would buy
        /// a few percent on adversarial workloads but adds complexity.
        uint8_t evictIdx = 0;
    };
    mutable std::vector<AttrSelectIC> attrSelectCache;

    /// Top-level entry offset.
    uint32_t entryOffset = 0;

    /// Force-emit-site side-table.
    ///
    /// Maps each emitted force-flavoured opcode (OP_FORCE,
    /// OP_GET_LOCAL_FORCE, OP_GET_UPVALUE_FORCE) bytecode offset to a
    /// short string literal naming the emit site
    /// (e.g. "lower.cc:792" for an `ir::Force` whose annotation came
    /// from line 792 of lower.cc, or "emit.cc:N" for emit-internal
    /// forces such as OP_REC_BINDING_SLOT_REF's helper push).
    ///
    /// Sorted by ascending bytecode offset (entries are appended in
    /// emit order, which is monotonically increasing).  Lookup is
    /// done by `std::lower_bound` from the runtime trace path; in
    /// the default build this table is read by nothing on the hot
    /// path so it has zero runtime cost.
    ///
    /// The string pointer is a `const char *` to a string literal;
    /// no ownership / lifetime concerns.
    std::vector<std::pair<uint32_t, const char *>> forceEmitSites;
};

} // namespace nix::v3
