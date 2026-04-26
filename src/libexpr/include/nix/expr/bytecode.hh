#pragma once
/// @file
/// Bytecode VM instruction encoding, opcode definitions, and compilation
/// unit structure for the Nix expression evaluator.
///
/// Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.
/// SPDX-License-Identifier: Apache-2.0

#include "nix/expr/eval-gc.hh"
#include "nix/expr/symbol-table.hh"
#include "nix/util/pos-idx.hh"

#include <algorithm>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace nix {

// Forward declarations for types used by pointer only.
struct Expr;
struct Formals;
struct ExprLambda;
struct SourcePath;
class EvalState;

} // namespace nix

namespace nix::bytecode {

// ---------------------------------------------------------------------------
// Instruction encoding
// ---------------------------------------------------------------------------
//
// Fixed-width 32-bit instructions:
//
//   31       24 23                    0
//  +----------+------------------------+
//  |  opcode  |       operand          |
//  | (8 bits) |       (24 bits)        |
//  +----------+------------------------+
//
// The operand is unsigned by default. For jump offsets, we sign-extend
// from 24 bits (range: -8388608 .. +8388607 instruction words).
//
// Two-operand instructions pack level:8 | displacement:16 into the
// 24-bit operand field.

using Instruction = uint32_t;

static constexpr uint32_t kOperandBits    = 24;
static constexpr uint32_t kOperandMask    = (1u << kOperandBits) - 1;
static constexpr int32_t  kSignedMax      = (1 << (kOperandBits - 1)) - 1;
static constexpr int32_t  kSignedMin      = -(1 << (kOperandBits - 1));

/// Encode an instruction from opcode + unsigned operand.
[[gnu::always_inline]]
inline constexpr Instruction encode(uint8_t op, uint32_t operand = 0) noexcept
{
    return (static_cast<uint32_t>(op) << kOperandBits) | (operand & kOperandMask);
}

/// Decode the 8-bit opcode from an instruction word.
[[gnu::always_inline]]
inline constexpr uint8_t decodeOp(Instruction instr) noexcept
{
    return static_cast<uint8_t>(instr >> kOperandBits);
}

/// Decode the 24-bit unsigned operand.
[[gnu::always_inline]]
inline constexpr uint32_t decodeOperand(Instruction instr) noexcept
{
    return instr & kOperandMask;
}

/// Decode the operand as a signed 24-bit value (for jump offsets).
[[gnu::always_inline]]
inline constexpr int32_t decodeSigned(Instruction instr) noexcept
{
    uint32_t raw = instr & kOperandMask;
    if (raw & (1u << (kOperandBits - 1)))
        raw |= ~kOperandMask; // sign-extend
    return static_cast<int32_t>(raw);
}

/// Pack (level:8, displacement:16) into a 24-bit operand.
[[gnu::always_inline]]
inline constexpr uint32_t packLevelDispl(uint8_t level, uint16_t displ) noexcept
{
    return (static_cast<uint32_t>(level) << 16) | displ;
}

/// Unpack level (high 8 bits) from a 24-bit operand.
[[gnu::always_inline]]
inline constexpr uint8_t unpackLevel(uint32_t operand) noexcept
{
    return static_cast<uint8_t>(operand >> 16);
}

/// Unpack displacement (low 16 bits) from a 24-bit operand.
[[gnu::always_inline]]
inline constexpr uint16_t unpackDispl(uint32_t operand) noexcept
{
    return static_cast<uint16_t>(operand & 0xFFFF);
}

/// Pack (arity:8, constIdx:16) for OP_CALL_PRIMOP.
[[gnu::always_inline]]
inline constexpr uint32_t packArityConst(uint8_t arity, uint16_t constIdx) noexcept
{
    return (static_cast<uint32_t>(arity) << 16) | constIdx;
}

/// Pack three 8-bit fields (dst, a, b) for register-based ops.
[[gnu::always_inline]]
inline constexpr uint32_t packABC(uint8_t dst, uint8_t a, uint8_t b) noexcept
{
    return (static_cast<uint32_t>(dst) << 16)
         | (static_cast<uint32_t>(a) << 8)
         | static_cast<uint32_t>(b);
}

/// Unpack 8-bit dst (high 8 bits of operand).
[[gnu::always_inline]]
inline constexpr uint8_t unpackDst(uint32_t operand) noexcept
{
    return static_cast<uint8_t>((operand >> 16) & 0xFF);
}

/// Unpack 8-bit a (middle 8 bits).
[[gnu::always_inline]]
inline constexpr uint8_t unpackA(uint32_t operand) noexcept
{
    return static_cast<uint8_t>((operand >> 8) & 0xFF);
}

/// Unpack 8-bit b (low 8 bits).
[[gnu::always_inline]]
inline constexpr uint8_t unpackB(uint32_t operand) noexcept
{
    return static_cast<uint8_t>(operand & 0xFF);
}


// ---------------------------------------------------------------------------
// Opcodes
// ---------------------------------------------------------------------------

enum Op : uint8_t {
    // -- Literals & constants --
    OP_NOP              = 0x00, // no-op (padding)
    OP_CONST            = 0x01, // [constIdx]     push constants[idx]
    OP_TRUE             = 0x02, //                push true
    OP_FALSE            = 0x03, //                push false
    OP_NULL             = 0x04, //                push null
    OP_INT              = 0x05, // [imm24]        push mkInt(imm24)

    // -- Variable access --
    OP_GET_LOCAL_0      = 0x06, // [displ:24]     env->values[displ]
    OP_GET_LOCAL_1      = 0x07, // [displ:24]     env->up->values[displ]
    OP_GET_LOCAL_2      = 0x08, // [displ:24]     env->up->up->values[displ]
    OP_GET_LOCAL_3      = 0x09, // [displ:24]     env->up->up->up->values[displ]
    OP_GET_LOCAL        = 0x0A, // [lvl:8|dsp:16] general (level, displacement)
    OP_GET_WITH         = 0x0B, // [symIdx:24]    dynamic with-chain lookup

    // -- Attribute operations --
    OP_ATTR_SELECT      = 0x0C, // [symIdx:24]    pop attrs, push attrs.sym
    OP_ATTR_SELECT_OR   = 0x0D, // [symIdx:24]    select with or-default (see OP_JUMP)
    OP_HAS_ATTR         = 0x0E, // [symIdx:24]    pop v, push bool (v ? sym)
    OP_ATTR_INSERT      = 0x0F, // [symIdx:24]    pop v, insert into builder
    OP_ATTR_INSERT_DYN  = 0x10, //                pop name+v, insert into builder
    OP_ATTRS_INIT       = 0x11, // [capacity:24]  begin non-rec attrset
    OP_ATTRS_FINISH     = 0x12, //                finalize attrset, push Value
    OP_ATTRS_UPDATE     = 0x13, //                pop lhs+rhs, push lhs // rhs
    OP_REC_ATTRS_INIT   = 0x14, // [envSize:24]   begin rec attrset
    OP_REC_ATTRS_FINISH = 0x15, // [hasOverrides]  finalize rec attrset

    // -- List operations --
    OP_LIST_INIT        = 0x16, // [size:24]      begin list
    OP_LIST_ELEM        = 0x17, // [idx:24]       pop v, set list[idx]
    OP_LIST_FINISH      = 0x18, //                finalize list, push Value
    OP_LIST_CONCAT      = 0x19, //                pop l1+l2, push l1 ++ l2

    // -- String operations --
    OP_STR_CONCAT_INIT  = 0x1A, // [nParts:24]    begin string interpolation
    OP_STR_CONCAT_PART  = 0x1B, //                pop v, add to concat state
    OP_STR_CONCAT_FINISH= 0x1C, //                finalize, push result string
    OP_COERCE_TO_STRING = 0x1D, // [flags:24]     pop v, push coerced string

    // -- Control flow --
    OP_JUMP             = 0x1E, // [offset:s24]   unconditional jump
    OP_JUMP_IF_FALSE    = 0x1F, // [offset:s24]   pop, force bool, jump if false
    OP_JUMP_IF_TRUE     = 0x20, // [offset:s24]   pop, force bool, jump if true
    OP_JUMP_IF_NOT_ATTRS= 0x21, // [offset:s24]   force top; jump if not attrset (no pop)
    OP_JUMP_IF_NO_ATTR  = 0x22, // [symIdx:24]    jump if top attrs lacks sym (next word=offset)

    // -- Thunks & closures --
    OP_MAKE_THUNK       = 0x23, // [thunkIdx:24]  push thunk(currentEnv, unit.thunks[idx])
    OP_MAKE_CLOSURE     = 0x24, // [lambdaIdx:24] push closure(currentEnv, unit.lambdas[idx])

    // -- Calling --
    OP_CALL             = 0x25, // [nArgs:24]     pop fun+args, call, push result
    OP_CALL_1           = 0x26, //                pop fun+arg, call, push result
    OP_TAIL_CALL        = 0x27, // [nArgs:24]     tail-call (reuses frame)

    // -- Force & return --
    OP_FORCE            = 0x28, //                force TOS in-place
    OP_RETURN           = 0x29, //                pop result, return to caller

    // -- Environment --
    OP_ENTER_LET        = 0x2A, // [envSize:24]   alloc Env, enter scope
    OP_LEAVE_SCOPE      = 0x2B, //                restore previous env
    OP_SET_ENV_SLOT     = 0x2C, // [displ:24]     pop v, store in currentEnv->values[displ]
    OP_PUSH_WITH        = 0x2D, //                pop attrs, enter with-scope

    // -- Comparison & logic --
    OP_EQ               = 0x2E, //                pop a+b, push deep equality
    OP_NEQ              = 0x2F, //                pop a+b, push !eq
    OP_NOT              = 0x30, //                pop v, push !v (force, assert bool)
    OP_IMPL             = 0x31, //                pop a+b, push !a || b

    // -- Arithmetic --
    OP_ADD              = 0x32, //                pop a+b, push a + b
    OP_SUB              = 0x33, //                pop a+b, push a - b
    OP_MUL              = 0x34, //                pop a+b, push a * b
    OP_DIV              = 0x35, //                pop a+b, push a / b
    OP_NEGATE           = 0x36, //                pop v, push -v
    OP_LESS_THAN        = 0x37, //                pop a+b, push a < b

    // -- Assertion --
    OP_ASSERT           = 0x38, //                pop cond, throw if false

    // -- Scope helpers --
    OP_INHERIT_FROM_INIT= 0x39, // [nExprs:24]    alloc inherit-from Env
    OP_INHERIT_FROM_SET = 0x3A, // [displ:24]     pop v, set in inheritEnv
    OP_SET_ENV_SLOT_UP  = 0x41, // [displ:24]     pop v, store in curEnv->up->values[displ]
    OP_ATTR_SELECT_DYN  = 0x42, //                pop nameVal+attrs, select attr by string name
    OP_HAS_ATTR_DYN     = 0x43, //                pop nameVal, peek attrs, push bool (dynamic has-attr)
    OP_ATTRS_DYN_INIT   = 0x44, // [nS:12|nD:12] build attrset with nS static + nD dynamic attrs

    // -- Superinstructions (fused hot-path sequences) --
    OP_GET_LOCAL_0_FORCE = 0x45, // [displ:24]   GET_LOCAL_0 + FORCE fused

    // -- Miscellaneous --
    OP_POS              = 0x3B, // [posIdx:24]    push __curPos attrset
    OP_DUP              = 0x3C, //                duplicate TOS
    OP_POP              = 0x3D, //                discard TOS
    OP_SWAP             = 0x3E, //                swap top two stack entries

    // -- Fallback --
    OP_EVAL_EXPR        = 0x3F, // [exprIdx:24]   fallback: eval exprPool[idx] in current env

    // -- Fused hot-path instructions --
    OP_SELECT_FORCE     = 0x40, // [symIdx:24]    select attr + force result

    // -- VM v2: upvalue-based closures (IR emitter) --
    //
    // These opcodes implement flat, upvalue-based closure capture for the
    // IR->bytecode path.  They coexist with the v1 env-chain opcodes above.
    //
    // Instead of walking a linked-list Env chain (v1), v2 closures store
    // captured variables in a flat GC-traced Value** array.  The body code
    // reads from this array via OP_GET_UPVALUE.  At the creation site, the
    // parent pushes captured values and then emits OP_MAKE_CLOSURE_V2 or
    // OP_MAKE_THUNK_V2 which pops them into the upvalue array.

    /// Read an upvalue from the current closure/thunk's upvalue array.
    /// Operand: upvalue index (0-based).
    /// Stack effect: pushes upvalues[idx].
    OP_GET_UPVALUE       = 0x46, // [idx:24]       push upvalues[idx]

    /// Create a v2 closure.  Operand: lambdaIdx into unit.lambdas.
    /// The preceding N data words (encoded as OP_NOP) give the upvalue
    /// count.  The N Values to capture are on the stack (first pushed =
    /// upvalue 0).  Pops N values, allocates a flat upvalue array, and
    /// pushes the resulting closure Value.
    OP_MAKE_CLOSURE_V2   = 0x47, // [lambdaIdx:24] pop N upvalues, push closure

    /// Create a v2 thunk.  Operand: thunkIdx into unit.thunks.
    /// Same capture convention as OP_MAKE_CLOSURE_V2.
    OP_MAKE_THUNK_V2     = 0x48, // [thunkIdx:24]  pop N upvalues, push thunk

    /// Read a stack slot (local variable) by absolute frame-relative index.
    /// Used by the IR emitter where VarIds map to fixed stack positions.
    /// Operand: slot index from frame base.
    /// Stack effect: pushes stack[frameBase + slot].
    OP_GET_STACK_SLOT    = 0x49, // [slot:24]       push stack[base+slot]

    /// Write a value into a stack slot.
    /// Pop TOS, store into stack[frameBase + slot].
    OP_SET_STACK_SLOT    = 0x4A, // [slot:24]       pop v, store to stack[base+slot]

    // -- v2 recursive binding support --

    /// Allocate a fresh Value* (via state.allocValue()) and push it.
    /// Used to pre-allocate stack slot Value* objects for recursive
    /// let bindings, so that thunks capturing forward references get
    /// a stable pointer that will be updated in-place when the thunk
    /// is later written.
    OP_ALLOC_VALUE       = 0x4B, //                push allocValue()

    /// Copy the contents of TOS into the Value* at a stack slot.
    /// Pop the source Value*, memcpy its data into stack[base+slot].
    /// Unlike OP_SET_STACK_SLOT which overwrites the pointer in the
    /// slot, this copies the VALUE DATA into the existing pointer,
    /// preserving the address for any captured upvalues that already
    /// reference it.
    OP_COPY_TO_SLOT      = 0x4C, // [slot:24]       pop src, copy *src into *stack[base+slot]
    OP_CELL_GET          = 0x4D, // [cell_uv:8|idx:16] push cell[idx] via upvalues[cell_uv]
    OP_CELL_SET          = 0x4E, // [cell_slot:8|idx:16] pop val, cell[idx] = val (cell from stack[base+cell_slot])
    OP_ALLOC_CELL        = 0x4F, // [size:24]           push GC_MALLOC'd Value*[size] (as reinterpret_cast'd Value*)

    /// Direct saturated primop call.
    /// Operand encoding: arity:8 | constIdx:16
    ///   - arity (high 8 bits):   number of arguments (1..8)
    ///   - constIdx (low 16 bits): index into constants[] for the PrimOp Value*
    /// Stack effect: pops `arity` Values, calls primOp->impl, pushes result.
    OP_CALL_PRIMOP       = 0x50, // [arity:8|constIdx:16]  direct primop call

    // -- Superinstructions (fused common patterns) --
    OP_GET_SLOT_FORCE    = 0x51, // [slot:24]            GET_STACK_SLOT + FORCE
    OP_GET_SLOT_RETURN   = 0x52, // [slot:24]            GET_STACK_SLOT + RETURN (direct to resultSlot)
    OP_GET_UV_FORCE      = 0x53, // [idx:24]             GET_UPVALUE + FORCE
    OP_SLOT_SLOT_CALL1   = 0x54, // [funcSlot:12|argSlot:12] GET_STACK_SLOT(f) + GET_STACK_SLOT(a) + CALL_1

    /// Inline-cached attribute select.
    /// Operand: index into CompilationUnit::attrCaches.
    /// On a cache hit (same Bindings* as last execution), the binary
    /// search over Bindings is skipped — direct cached load.
    OP_ATTR_SELECT_CACHED = 0x55, // [cacheIdx:24]  cached attr select

    /// Fused cached attr select + force.  The most common attribute
    /// access pattern (e.g., `pkg.meta.description`) is select-then-force.
    /// One dispatch instead of two.
    OP_ATTR_SELECT_FORCE_CACHED = 0x56, // [cacheIdx:24]

    /// Slot-to-slot copy (mini register-based op).
    /// Operand: [srcSlot:12|dstSlot:12].  Equivalent to GET_STACK_SLOT(src)
    /// + SET_STACK_SLOT(dst) in one dispatch.  Saves the operand stack
    /// round-trip for simple alias bindings (`let a = b; in ...`).
    OP_MOV_SLOTS = 0x57, // [srcSlot:12|dstSlot:12]

    // -- Phase 1: Register-form ops --
    //
    // Per the agent research: the v2 VM is already register-architectured
    // at the memory model level — slots are registers, the IR is SSA.
    // The 52% push/pop overhead comes from using the operand stack as
    // a bus to deliver operands to opcodes.  Register-form ops read
    // from a source slot and write to a destination slot directly,
    // bypassing the operand stack entirely.
    //
    // Encoding: [dst:8|src:16] — dst slot up to 256, src slot/idx up to 65536

    /// Force value at slot src, write result to slot dst.
    /// Replaces GET_SLOT_FORCE + SET_STACK_SLOT (2 dispatches → 1).
    OP_RFORCE_FROM = 0x58, // [dst:8|src:16]

    /// Read upvalue at idx, write to slot dst.
    /// Replaces GET_UPVALUE + SET_STACK_SLOT (2 dispatches → 1).
    OP_RGET_UV_TO = 0x59, // [dst:8|uvIdx:16]

    /// Read upvalue at idx, force it, write result to slot dst.
    /// Replaces GET_UV_FORCE + SET_STACK_SLOT (2 dispatches → 1).
    OP_RUVF_TO = 0x5A, // [dst:8|uvIdx:16]

    // -- Phase 2: Three-address register ops --
    //
    // Encoding: [dst:8|a:8|b:8] — three 8-bit slot operands packed into
    // the 24-bit operand.  Limits: 256 slots per frame (sufficient for
    // virtually all Nix functions; the largest lambda body in nixpkgs
    // has ~80 IR vars).
    //
    // Replaces stack-based patterns:
    //   GET a; GET b; OP; SET dst   →   ROP dst, a, b   (4 → 1 dispatch)

    /// dst = call(*a, *b).  Direct slot-to-slot function application.
    OP_RCALL1_R = 0x5B, // [dst:8|funcSlot:8|argSlot:8]

    /// dst = *a + *b (integer add, with tagged-int fast path).
    OP_RADD_R   = 0x5C, // [dst:8|lhs:8|rhs:8]
    /// dst = *a - *b
    OP_RSUB_R   = 0x5D,
    /// dst = *a * *b
    OP_RMUL_R   = 0x5E,
    /// dst = *a < *b (boolean result)
    OP_RLESS_R  = 0x5F,
    /// dst = *a == *b
    OP_REQ_R    = 0x60,

    /// Cached attr select + force, slot-to-slot.
    /// Operand format: [dst:8|attrsSlot:8|cacheIdxLow:8] +
    /// data word [cacheIdxHigh:24] for full 32-bit cache index.
    /// Simpler: [dst:8|attrsSlot:8|cacheIdxLow:8] with cacheIdx limited to 256.
    /// Falls back to two-instruction form if cacheIdx exceeds 256.
    OP_RATTR_SELF_R = 0x61, // [dst:8|attrsSlot:8|cacheIdxLow:8]

    /// Tail-call: pop fun + arg, replace the CURRENT call frame's
    /// unit/ip/env/upvalues with the callee's body, push arg as slot 0.
    /// Caller's resultSlot / resultStoreSlot are preserved so OP_RETURN
    /// in the callee delivers to the original consumer.  Only works
    /// when fun is a v2 bytecode-proxy lambda; otherwise the emitter
    /// keeps the OP_CALL_1 + OP_RETURN sequence (no tail call).
    /// No operand.
    OP_TAIL_CALL_1 = 0x62,
};


// ---------------------------------------------------------------------------
// Source position table entry (sparse)
// ---------------------------------------------------------------------------

/// Maps an instruction offset to its source position.
/// The position table is sorted by instrOffset for binary search.
struct PosEntry
{
    uint32_t instrOffset;
    PosIdx pos;
};


// ---------------------------------------------------------------------------
// Thunk and Lambda descriptors
// ---------------------------------------------------------------------------

/// Identifies a lazy sub-expression (thunk body) within a CompilationUnit.
struct ThunkDescriptor
{
    uint32_t codeOffset; // Instruction index into CompilationUnit::code
    PosIdx   pos;        // Source position for error messages
    Expr *   sourceExpr = nullptr; // Original AST expression (for isTrivial() compat)
    uint16_t nUpvalues = 0; // Number of upvalues captured (v2 thunks)

    /// Maximum stack-slot index used by this thunk's body.  Filled in
    /// at emit time (max nextSlot reached during sub-block emission).
    /// Frame-push paths use this for a single VMState::ensureCapacity
    /// call instead of growing the stack one slot at a time across
    /// the register-form opcode handlers.
    uint16_t maxSlot = 0;

    /// Pre-allocated ExprBytecodeThunk for this descriptor.
    /// Created once during compilation (emitFromIR), reused by every
    /// OP_MAKE_THUNK_V2 execution.  Eliminates 681K+ runtime Expr
    /// allocations for typical nixpkgs evaluation.
    Expr * cachedExpr = nullptr;
};

/// Identifies a function body within a CompilationUnit.
struct LambdaDescriptor
{
    uint32_t codeOffset; // Instruction index for the lambda body
    PosIdx   pos;        // Source position of the lambda definition
    Symbol   name;       // Lambda name (may be empty for anonymous)
    Symbol   arg;        // Simple parameter name (empty if formals-only)

    /// Formals metadata. nullptr if the lambda takes a single positional
    /// argument (x: ...) rather than a pattern ({ a, b, ... }: ...).
    /// Points into BumpMemoryResource (shared with AST Formals).
    Formals * formals = nullptr;

    /// Number of slots required in the Env for this lambda's body (v1).
    uint16_t envSize = 0;

    /// Number of upvalues captured by this closure (v2).
    uint16_t nUpvalues = 0;

    /// Back-pointer to the source ExprLambda for profiling compatibility.
    ExprLambda * sourceExpr = nullptr;

    /// Index into CompilationUnit::thunks for the lambda body's
    /// ThunkDescriptor. Pre-allocated at compile time to avoid
    /// runtime vector modifications.  Points to bodyStart (after
    /// the formals-binding prologue), used by ExprBytecodeThunk
    /// when callFunction dispatches via lambda.body->eval().
    uint32_t bodyThunkIdx = 0;

    /// Code offset for the formals-binding prologue.
    /// OP_CALL_1 trampoline jumps here so it runs the bytecoded
    /// formal parameter unpacking before the body.  For simple
    /// lambdas (no formals), prologueOffset == body code offset.
    uint32_t prologueOffset = 0;

    /// Pre-allocated ExprLambdaBytecode for this descriptor.
    /// Created once during compilation, reused by every
    /// OP_MAKE_CLOSURE_V2 execution.
    Expr * cachedExpr = nullptr;
};


/// 8-way polymorphic inline cache for OP_ATTR_SELECT_CACHED.
///
/// Each call site stores up to 8 (Bindings*, Value*) pairs.  On lookup,
/// linearly scan the slots; on miss, evict the oldest entry (round-robin).
///
/// Nix attribute access is more polymorphic than monomorphic interpreters
/// like Luau — `map (p: p.meta) packages` sees many different Bindings*
/// at the same call site.  Profiling against nixpkgs#hello.name showed
/// the previous 4-way PIC running at 33.9% hit rate (essentially noise),
/// suggesting the hot polymorphic sites had degree > 4.  Doubling to
/// 8-way nearly doubles per-site memory (32B → 128B) but catches the
/// real-world polymorphism without falling back to Bindings::get()
/// binary search on every lookup.
struct AttrCache
{
    Symbol name;
    static constexpr int kEntries = 8;
    static constexpr int kEvictMask = kEntries - 1;

    /// Interleaved (bindings, value) pairs to maximize cache-line locality
    /// — the hit path touches both fields in lockstep.  The hit branch
    /// promotes the matched entry to slot 0 (LRU-on-hit), so the most
    /// recently used Bindings* is always the first compare.
    struct Entry
    {
        const Bindings * bindings = nullptr;
        Value * value = nullptr;
    };
    Entry entries[kEntries];
    uint8_t nextEvict = 0;  ///< Insertion index for cold misses.
};


// ---------------------------------------------------------------------------
// CompilationUnit
// ---------------------------------------------------------------------------

/// One per parsed file or top-level eval. Owns a single flat bytecode
/// buffer plus metadata arrays. Thunks and lambdas are byte ranges
/// within this buffer -- no per-thunk heap allocations.
///
/// GC-allocated so Boehm traces all Value* pointers in the constants pool.
struct CompilationUnit : gc
{
    // -- Bytecode --
    // Flat array of 32-bit instructions.
    std::vector<Instruction, traceable_allocator<Instruction>> code;

    // -- Constants pool --
    // GC-traced array of Value* pointers. Indexed by OP_CONST operand.
    std::vector<Value *, traceable_allocator<Value *>> constants;

    // -- Symbol pool --
    // Attribute names used by OP_ATTR_SELECT, OP_ATTR_INSERT, etc.
    // References into the global EvalState::symbols table.
    std::vector<Symbol> symbols;

    // -- Thunk descriptors --
    // Indexed by OP_MAKE_THUNK operand.
    std::vector<ThunkDescriptor> thunks;

    // -- Lambda descriptors --
    // Indexed by OP_MAKE_CLOSURE operand.
    std::vector<LambdaDescriptor> lambdas;

    // -- PosIdx pool --
    // Source positions used by OP_ATTRS_INIT for attribute positions.
    // Indexed by operand of data words following OP_ATTRS_INIT.
    std::vector<PosIdx> posPool;

    // -- Expr fallback pool --
    // Expr* pointers for expressions that are not yet compiled to bytecode.
    // Indexed by OP_EVAL_EXPR operand.  The VM calls expr->eval() on these.
    std::vector<Expr *> exprPool;

    // -- Position table (sparse, sorted by instrOffset) --
    std::vector<PosEntry> positions;

    // -- Inline caches for OP_ATTR_SELECT / OP_SELECT_FORCE / OP_HAS_ATTR --
    // One slot per call site, indexed by the operand of the instruction.
    // mutable because the VM updates these at runtime on cache miss.
    mutable std::vector<AttrCache> attrCaches;

    // -- Source path for cache keying and error messages --
    // May be nullptr for string-evaluated expressions.
    const SourcePath * sourcePath = nullptr;

    /// Look up the source position for a given instruction offset.
    /// Uses binary search over the sparse position table.
    PosIdx posForOffset(uint32_t instrOffset) const noexcept
    {
        auto it = std::upper_bound(
            positions.begin(), positions.end(), instrOffset,
            [](uint32_t offset, const PosEntry & entry) {
                return offset < entry.instrOffset;
            });
        if (it == positions.begin())
            return noPos;
        --it;
        return it->pos;
    }

    /// Append an instruction, returning its index (for patching jumps).
    uint32_t emit(uint8_t op, uint32_t operand = 0)
    {
        uint32_t idx = static_cast<uint32_t>(code.size());
        code.push_back(encode(op, operand));
        return idx;
    }

    /// Patch a previously emitted jump instruction to target the current
    /// code offset. The offset is relative: target - source - 1.
    void patchJump(uint32_t jumpInstrIdx)
    {
        uint32_t target = static_cast<uint32_t>(code.size());
        int32_t offset = static_cast<int32_t>(target) - static_cast<int32_t>(jumpInstrIdx) - 1;
        uint8_t op = decodeOp(code[jumpInstrIdx]);
        code[jumpInstrIdx] = encode(op, static_cast<uint32_t>(offset) & kOperandMask);
    }

    /// Add a Value* to the constants pool, returning its index.
    uint32_t addConstant(Value * v)
    {
        uint32_t idx = static_cast<uint32_t>(constants.size());
        constants.push_back(v);
        return idx;
    }

    /// Add a PosIdx to the pos pool, returning its index.
    uint32_t addPos(PosIdx pos)
    {
        uint32_t idx = static_cast<uint32_t>(posPool.size());
        posPool.push_back(pos);
        return idx;
    }

    /// Add an Expr* to the expr fallback pool, returning its index.
    /// Used for expressions not yet compiled to bytecode (OP_EVAL_EXPR).
    uint32_t addExpr(Expr * e)
    {
        uint32_t idx = static_cast<uint32_t>(exprPool.size());
        exprPool.push_back(e);
        return idx;
    }

    /// Add a Symbol to the symbol pool, returning its index.
    /// Deduplicates via hash map (O(1) instead of linear scan).
    /// Defined in bytecode.cc (requires complete Symbol type).
    uint32_t addSymbol(Symbol sym);

    /// Allocate an inline cache slot for an attribute access.
    /// Each OP_ATTR_SELECT_CACHED call site gets its own 8-way PIC slot.
    /// Also registers the symbol in the symbol pool so that disk-cache
    /// serialization can recover the AttrCache by symbol-pool index.
    uint32_t addAttrCache(Symbol name)
    {
        // Force-register the symbol in the pool so symbolIndex is
        // complete for serialization (Phase 3.2 disk cache).
        addSymbol(name);
        uint32_t idx = static_cast<uint32_t>(attrCaches.size());
        AttrCache c;
        c.name = name;
        attrCaches.push_back(c);
        return idx;
    }

    /// Hash map for O(1) symbol deduplication in addSymbol.
    std::unordered_map<Symbol, uint32_t> symbolIndex;

    /// Record a source position for the current instruction offset.
    /// Only emits if the position differs from the previous entry.
    void emitPos(PosIdx pos)
    {
        uint32_t offset = static_cast<uint32_t>(code.size());
        if (!positions.empty() && positions.back().pos == pos)
            return; // deduplicate consecutive identical positions
        positions.push_back({offset, pos});
    }
};

/// Return the mnemonic string for an opcode.
const char * opName(uint8_t op);

/// Disassemble a CompilationUnit into a human-readable string.
/// If `state` is non-null, prints constant values and symbol names.
std::string disassemble(const CompilationUnit & unit, const EvalState * state = nullptr);

/// Reasons why a CompilationUnit cannot be safely serialized to disk.
/// Used by the persistent CompilationUnit cache (Phase 3.2) to skip
/// uncacheable units rather than corrupting the cache.
enum class UncacheableReason : uint8_t {
    Cacheable = 0,
    /// The unit references AST Expr* via OP_EVAL_EXPR.  Tree-walker
    /// fallbacks for IR coverage gaps make this CU process-bound.
    HasExprPool,
    /// The constant pool contains a non-leaf Value (attrset, list,
    /// lambda, thunk, app, external).  These hold nested heap pointers
    /// that can't be re-materialized from a binary blob.
    NonLeafConstant,
    /// A LambdaDescriptor has formals (pattern-match parameter list).
    /// Formals point into the AST BumpMemoryResource and would require
    /// deep-copy serialization.  Skip until that lands.
    HasFormalsLambda,
    /// The descriptor's sourceExpr is non-null AND the unit isn't
    /// going through Phase 3.1 lite's lazy pattern.  Cached CUs from
    /// another process don't have valid AST pointers; downstream
    /// lambdaBodyCache lookups would miss harmlessly, but flag for
    /// hygiene during the schema bring-up.
    UnsupportedFeature,
};

/// Returns Cacheable if the unit can be safely serialized, otherwise
/// the first reason encountered.  Designed to short-circuit on the
/// most common rejection (HasExprPool).
UncacheableReason cacheabilityCheck(const CompilationUnit & unit);

inline bool isCacheable(const CompilationUnit & unit)
{
    return cacheabilityCheck(unit) == UncacheableReason::Cacheable;
}

/// Human-readable name for the rejection reason; used in stats and
/// optional debug logging when a CU is skipped.
const char * uncacheableReasonName(UncacheableReason r);

} // namespace nix::bytecode
