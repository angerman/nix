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
    /// Superinstruction (BYTECODE_NGRAM_ANALYSIS §7, shipped 2026-06-04):
    /// fuse an OP_SET_LOCAL immediately followed by a same-slot
    /// OP_GET_LOCAL.  Stores the operand-stack top into the slot WITHOUT
    /// popping (the elided GET would have re-pushed it) — net stack effect
    /// identical to SET_LOCAL;GET_LOCAL, one fewer dispatch + one fewer
    /// Value copy/pop.  The emit-time peephole only fuses RESERVED locals
    /// (slot < nLocals), which sit strictly below the top, so no auto-grow
    /// path is needed (unlike OP_SET_LOCAL).  Measured +1.4% wall on
    /// hello.drvPath (warm+cold), +2.4% on dispatch-heavy compute.
    OP_SET_LOCAL_KEEP = 0x58,  // [slot:24]   store top into slot, keep on stack

    /// eval/apply call-SITE optimization (lever B, 2026-06-05): saturated
    /// multi-arg call.  [n:24] = arg count (2..16).  Stack: fun then a0..a_{n-1}
    /// (a_{n-1} on top).  Pops n args + fun.  When fun is a closure of arity
    /// == n, enters the body with the n args in slots 0..n-1 DIRECTLY — no
    /// intermediate PAP (Tag::App) pair per partial application.  For any
    /// other shape (under/over-arity, PAP, primop, __functor, non-closure) it
    /// falls back to applying the args one at a time (callClosure), so it is
    /// always semantically identical to n curried OP_CALLs.  Emitted by the
    /// App-spine coalescer in emit.cc for spines of >= 2 args whose inner
    /// applications are OnceLinear.
    OP_CALL_N         = 0x59,  // [n:24]   saturated n-arg call (skip PAPs)
    /// Tail variant of OP_CALL_N: reuses the current frame when saturated
    /// (O(1) tail recursion, no PAP), else falls back to building the value
    /// and returning it.  See OP_TAIL_CALL.
    OP_TAIL_CALL_N    = 0x5a,  // [n:24]   saturated n-arg tail call
    /// §2(b) superinstruction (NEXT_STEPS_2026-06-05): fuse the common
    /// `OP_GET_UPVALUE idx ; OP_REC_BINDING_SLOT_REF sym ; <icIdx>` sequence
    /// (a recursive self-reference resolved through a captured rec-attrset)
    /// into one dispatch.  Reads the upvalue directly (no intermediate
    /// push/pop), forces it to WHNF if needed, then does the same
    /// recSlotCache IC lookup and pushes the Tag::Slot.  Encoding:
    ///   [sym:24]; data: [upvalIdx:32, icIdx:32]
    OP_GET_UPVALUE_REC_BINDING = 0x5b,
    /// Lever 1B-lite (WALL_OPTIMIZATION_PLAN §4): operand-folding super-
    /// instruction.  Fuses two adjacent different-slot `OP_GET_LOCAL a;
    /// OP_GET_LOCAL b` (the D1-confirmed dominant `GET_LOCAL;GET_LOCAL`
    /// bigram — prefix of `GL GL CALL_PRIMOP`, `GL GL MAKE_THUNK`, …) into a
    /// single dispatch that pushes slots a then b.  Encoding: operand 24-bit
    /// = (a << 12) | b — two 12-bit slot indices (slots ≥ 4096 fall back to
    /// the unfused pair; register-pressure p99 = 7, so this covers ~all).
    OP_GET_LOCAL2     = 0x5c,  // [a:12|b:12]  push local a, then local b
    /// Register VM Phase 1 (REGISTER_VM_DESIGN_2026-06-05): 3-address binary
    /// primop call.  `regs[dst] = po(arg0, arg1)` reading operands directly
    /// from local slots (or inline immediates) and writing the result to a
    /// dst slot — NO operand-stack round-trip; collapses
    /// `GET a; GET b; CALL_PRIMOP po; SET dst` (5 dispatches) to ONE.
    /// Encoding: `operand = poIdx`; word1 = `dst` (24-bit slot); word2 =
    /// `(descA << 16) | descB`, each desc = bit15 immediate-flag |
    /// 15-bit value (slot index, or signed int when immediate).  Strict
    /// non-WHNF slot args are forced via A8 writeback-to-slot, exactly like
    /// OP_CALL_PRIMOP.  Emitted only for primops with no deep-force-list arg.
    OP_R_PRIMOP2      = 0x5d,
    /// Register VM Phase 5 (REGISTER_VM_DESIGN_2026-06-05): return a register.
    /// `return regs[operand]` — reads the result directly from a local slot,
    /// dropping the `GET_LOCAL s; OP_RETURN` operand-stack round-trip.  Shares
    /// OP_RETURN's teardown (only the retVal source differs).  Emitted (post-
    /// RETURN peephole) only when no branch targets the RETURN — i.e. the
    /// return value is unconditionally in slot s, not left on the stack by a
    /// branch (the straight-line case; branchy functions need full register
    /// mode).  The first op that lets a whole (straight-line) function run
    /// with NO operand-stack traffic.
    OP_R_RETURN       = 0x5e,  // [slot:24]  return regs[slot]
    /// Register VM Phase 5: branch on a register.  `if !regs[cond] goto target`
    /// reading the condition directly from a local slot (force-writeback-to-
    /// slot if non-WHNF, like the other register ops), dropping the
    /// `GET_LOCAL cond; BRANCH_FALSE` round-trip.  `operand = target` (24-bit,
    /// so compactFuseSetGet's jump rebase applies) + 1 follow-up word
    /// `cond_slot`.
    OP_R_BRANCH_FALSE = 0x5f,

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
    /// #458 step 1/6 — heap-stable rec-attrset slot publish.
    ///
    /// Peeks the Tag::Attrs at top of operand stack (a freshly-built
    /// rec-attrset Bindings allocated by OP_ATTRS_REC_INIT), allocates
    /// a fresh heap-stable `Value*` slot via `Alloc::allocValue()`,
    /// writes the peeked Tag::Attrs INTO that slot (so the slot now
    /// holds a stable Tag::Attrs(Bindings*) referring to the same
    /// Bindings that subsequent OP_ATTRS_REC_SET will populate), and
    /// pushes a Tag::Slot Value pointing at *slot ON TOP of the
    /// existing Tag::Attrs.  Stack transition:
    ///
    ///     [..., Tag::Attrs]  →  [..., Tag::Attrs, Tag::Slot]
    ///
    /// Inner closures captured under this let-rec scope may capture
    /// the Tag::Slot as an upvalue (rather than the Tag::Attrs).
    /// Forcing a Tag::Slot deref's to the Bindings, which evolves as
    /// REC_SET writes happen — the slot is partial-bindings-safe.
    ///
    /// No operand; no extra data words.  The newly-allocated slot has
    /// no compile-time reference; closures capturing it keep it alive
    /// via Boehm GC reachability through their freeVars vector.
    OP_REC_SLOT_PUBLISH = 0x83,

    /// SECD DUM/RAP: push a Tag::Slot Value onto the operand stack
    /// pointing at the local slot referenced by [slot:24].  Used when

    /// SECD-style heap-stable slot reference: pop a Tag::Attrs (a
    /// rec-attrset's Bindings*), look up the entry by SymbolId, and
    /// push a Tag::Slot Value pointing at `&entries[i].value` —
    /// stable as long as the Bindings is alive.  Used by `with E;`
    /// when E resolves to a rec-attrset entry: sub-thunks captured
    /// in the with-body see the entry's live mutated value through
    /// the slot, including the memoized resolved value once forceValue
    /// has run on the entry once.  Mirrors tree-walker's `Value *`
    /// slot pointer into the Env block.
    ///
    /// Schema 10 (#779, 2026-05-23): adds a 1-word IC follow-up
    /// (`[ic_idx:32]`) indexing `CompilationUnit::recSlotCache`.  On
    /// hit, the cached (Bindings*, slot) is used to skip the binary
    /// search by SymbolId; on miss, the search runs and the cache is
    /// installed.  Lifts ~30 ns / call on hello.drvPath (9.05 % of
    /// 15 M dispatches).
    OP_REC_BINDING_SLOT_REF = 0x84, // [sym:24], data: [ic_idx:32]

    /// STG-14b (#516/#517): pop a Tag::Thunk from the operand stack,
    /// allocate a heap-stable Value cell, store the thunk into the
    /// cell, set `thunk->cell = cell`, and write a Tag::Slot{cell}
    /// into the local at [slot:24].  Equivalent to:
    ///   OP_MAKE_THUNK fid; OP_SET_LOCAL slot
    /// PLUS attaching the slot's storage as the thunk's cell so the
    /// thunk's eventual OP_RETURN's cell-update at vm.cc:3003 fires.
    /// Used by emit.cc:594-601 for hidden-from-expr thunks (the
    /// `inherit (X // Y) ...` lowering's inheritFromExpr thunks),
    /// which previously had `cell == nullptr` -- causing per-attr
    /// thunks that captured the hidden slot via emitVarRef to see a
    /// stale Black thunk pointer when the hidden thunk was forced
    /// mid-construction under STG_KEEP_HOOKS=1 (#517 root cause).
    ///
    /// The heap-stable cell is necessary because value-stack
    /// addresses are not stable across `valueStack.resize()`; using
    /// `&valueStack[stackBase+slot]` directly would dangle on the
    /// next OP_CALL/OP_MAKE_THUNK that grew the stack.
    OP_THUNK_SET_LOCAL_THROUGH_CELL = 0x85, // [slot:24]

    /// Bytecode-identical to OP_ATTRS_REC_INIT (allocates a placeholder
    /// rec-attrset Bindings of size [n:24], with n trailing (SymbolId,
    /// PosIdx) pairs in the same layout) BUT does NOT call
    /// publishToNearestBlackThunkFrame.  Emitted by the lowerer for
    /// `let ... in body` shapes (lowerLet -> lowerLetRecCapture with
    /// hasBody=true) where the rec-attrset is INTERMEDIATE state, not
    /// the surrounding thunk's eventual return value.
    ///
    /// The publish-to-Black-thunk behaviour of OP_ATTRS_REC_INIT only
    /// makes sense for bare `rec { ... }` literals, where the rec-
    /// attrset IS the surrounding thunk's eventual return value (so
    /// publishing it as the thunk's `evaluated` field is correct, and
    /// lets self-references like `rec { x = 1; y = self.x; }` resolve
    /// without forcing the wrap thunk).  For `let prev = ...; in
    /// body`, the thunk's eventual return value is `body`, NOT the
    /// `{prev}` recAttrs — publishing `{prev}` corrupts the thunk's
    /// state with a wrong-shape value.  Surfaced as the v3-direct
    /// callPackage-with-scope bug (lib.extends / lib.fix interaction
    /// in nixpkgs hello.name; see lode/CALLPACKAGE_BUG_2026-05-09.md).
    OP_ATTRS_LET_REC_INIT = 0x86, // [n:24]; data: 2n (name, pos) pairs

    /// Bytecode-identical to OP_ATTRS_REC_INIT (allocates a placeholder
    /// rec-attrset Bindings of size [n:24], with n trailing (SymbolId,
    /// PosIdx) pairs in the same layout) BUT publishes the partial
    /// Bindings to EVERY thunk frame on the call stack — Black AND
    /// Suspended — using FIRST-WINS semantics.
    ///
    /// Emitted by the lowerer when this AttrSet is the function's
    /// tail-return value (i.e. the value the surrounding thunk will
    /// eventually evaluate to).  Outer thunks on the call stack are
    /// transitively waiting for THIS AttrSet's value, so registering
    /// it as their partial Bindings lets `with self;`-style lookups
    /// resolve through any of them via the partial-Bindings peek path
    /// (vm.cc:withLookup).
    ///
    /// Sub-attrsets (let-bindings, function args, intermediate
    /// expressions) emit OP_ATTRS_REC_INIT instead — they don't
    /// represent the function's eventual return value, so registering
    /// them with outer thunks would falsely advertise sub-expression
    /// shapes to consumers expecting the function's return.
    ///
    /// FIRST-WINS: when an outer thunk already has a registry entry
    /// (e.g. from an earlier function's tail-return AttrSet that's
    /// still on the call stack), we leave the existing entry in
    /// place.  This preserves the outermost-tail-return property:
    /// once super's body's REC_INIT_TAIL fires and registers super
    /// with the surrounding lib.fix chain's thunks, later inner
    /// function calls (e.g. helper thunks spawned during super body)
    /// don't displace super's registration on those outer thunks.
    /// They get to register on their OWN thunk (which super hasn't
    /// touched), but not on x_thunk / prev_thunk / etc.
    ///
    /// See ir::AttrSet::isFunctionReturn for the lower-time tagging
    /// that drives this opcode emission.
    OP_ATTRS_REC_INIT_TAIL = 0x87, // [n:24]; data: 2n (name, pos) pairs

    /// #558 (2026-05-10) tail-return // operation.  Bytecode-identical
    /// to OP_ATTRS_UPDATE (pops 2 operands, pushes merged Bindings)
    /// BUT additionally publishes the merged Bindings to every
    /// THUNK_RETURN frame on the call stack via
    /// publishToAllThunkFrames.
    ///
    /// STG analog: when a function's tail expression is a constructor
    /// allocation (// produces a new Bindings cell), the cell IS the
    /// function's WHNF.  STG's update-frame chain propagates this
    /// constructor up through tail-call ancestors.  For Nix's
    /// lib.fix's `let x = f x; in x` with f producing a // chain in
    /// tail position, the // result IS x's WHNF — registering it with
    /// x's chain gives consumers (e.g. `with self;` lookups, select
    /// thunks) a more-correct WHNF approximation than any
    /// intermediate REC_INIT_TAIL sub-AttrSet.
    ///
    /// See ir::Update::isFunctionReturn for the lower-time tagging.
    OP_ATTRS_UPDATE_TAIL = 0x88,

    // --- Strings --------------------------------------------------------
    OP_STR_CONCAT     = 0x90,  // [n:24] forceString stored in low bit of n; pops n parts

    // --- Assert / pos ---------------------------------------------------
    OP_ASSERT         = 0xA0,  // pop bool; raise if false
    // OP_POS: reserved; lowerExpr skips ExprPos in v3 (positions are
    // recovered from the side table at error time).  Dispatch removed.
    OP_POS            = 0xA1,  // [posIdx:24]  push pos attrset

    // --- #736 IFD probe (S5 from IFD_DEEP_DIVE_2026-05-21.md) ----------
    /// Compile-time-emitted marker for primops that may trigger
    /// Import-From-Derivation.  Operand encodes the IfdProbeKind
    /// (low 8 bits).  Zero stack effect: the probe observes that an
    /// IFD-class primop is about to be invoked but does not consume
    /// or transform its arguments.
    ///
    /// Default runtime cost: one branch + (when active) one
    /// uint64_t++ into `allocStats().ifdProbeCount[kind]`.  No
    /// allocation, no force, no FFI cross.  Future S2 (batching) /
    /// S4 (content-addressed eval cache) will dispatch additional
    /// behavior from this opcode without each primop reaching into
    /// its own special-case logic.
    OP_IFD_PROBE      = 0xA2,  // [kind:24]   IFD-class primop about to fire
    //
    // Probe kinds — keep in sync with `emit.cc::ifdProbeKindForPrimop`
    // and `allocStats().ifdProbeCount[]` (which sizes to 16 slots).
    // The 1..N numbering leaves slot 0 as the kIfdNone sentinel so a
    // zero in the counter table unambiguously means "kind not used."
    //
    // Categorization rationale:
    //   - Imports / file reads of (possibly-context) paths trigger
    //     classic Nix IFD.
    //   - Network fetches don't trigger IFD per se but block eval the
    //     same way and benefit from the same batching/cache layers,
    //     so they're tracked as separate kinds.
    //   - filterSource realises a path but doesn't go through TW's
    //     realisePath bridge today; tracked for completeness.

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

/// #736 IFD-probe kinds.  Encoded in OP_IFD_PROBE's 24-bit operand;
/// see vm.cc OP_IFD_PROBE dispatch + allocStats().ifdProbeCount[].
enum IfdProbeKind : uint8_t
{
    kIfdNone           = 0,   // sentinel — never emitted
    kIfdImport         = 1,   // import / __import
    kIfdReadFile       = 2,   // readFile / __readFile
    kIfdReadDir        = 3,   // readDir / __readDir
    kIfdPathExists     = 4,   // pathExists / __pathExists
    kIfdReadFileType   = 5,   // readFileType / __readFileType
    kIfdFindFile       = 6,   // findFile / __findFile (also: <nixpkgs> via NIX_PATH)
    kIfdFetchurl       = 7,   // fetchurl / __fetchurl
    kIfdFetchTarball   = 8,   // fetchTarball
    kIfdFetchTree      = 9,   // fetchTree
    kIfdFetchGit       = 10,  // fetchGit
    kIfdFetchMercurial = 11,  // fetchMercurial
    kIfdFilterSource   = 12,  // filterSource / path
    kIfdProbeKindCount = 13,
};

/// Render a probe kind as a stable short string (for stats banners).
/// Returns "?" for out-of-range values.
inline const char * ifdProbeKindName(uint8_t k) noexcept
{
    switch (k) {
        case kIfdImport:         return "import";
        case kIfdReadFile:       return "readFile";
        case kIfdReadDir:        return "readDir";
        case kIfdPathExists:     return "pathExists";
        case kIfdReadFileType:   return "readFileType";
        case kIfdFindFile:       return "findFile";
        case kIfdFetchurl:       return "fetchurl";
        case kIfdFetchTarball:   return "fetchTarball";
        case kIfdFetchTree:      return "fetchTree";
        case kIfdFetchGit:       return "fetchGit";
        case kIfdFetchMercurial: return "fetchMercurial";
        case kIfdFilterSource:   return "filterSource";
        default:                 return "?";
    }
}

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

    /// #779 (2026-05-23) per-call-site IC for OP_REC_BINDING_SLOT_REF.
    /// Monomorphic 1-way: LetRec sites are shape-stable (the same
    /// rec attrset's Bindings* recurs across calls to the lambda
    /// containing the slot ref).  Cached (Bindings*, slot) skips
    /// the binary search by SymbolId on the hot path.  Entries are
    /// zeroed on construction; first hit installs.  Cache size is
    /// determined by the count of OP_REC_BINDING_SLOT_REF instances
    /// in the bytecode, indexed via the 1-word follow-up after
    /// each OP_REC_BINDING_SLOT_REF.
    struct RecSlotIC {
        const Bindings * bindings = nullptr;
        uint32_t slot = 0;
    };
    mutable std::vector<RecSlotIC> recSlotCache;

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

    /// Approximate in-memory byte footprint of this CU's owned
    /// containers.  These live in libc-malloc'd `std::vector`s (NOT the
    /// v3 arena), so they are invisible to the arena's `bytesAllocated`
    /// counter AND to the precise arena mark — yet the `import` cache
    /// retains one CU per imported `.nix` file for the whole process,
    /// which is a material slice of resident RSS on nixpkgs-scale evals
    /// (~700 MB on HNE per HNE_BUCKET_DECOMP_2026-05-27).  This accessor
    /// lets the memory-bucket report size the "CU cache (bytecode)"
    /// bucket honestly.
    ///
    /// Counts `capacity()` (the resident allocation), not `size()`, plus
    /// the string bodies of the two string-bearing pools.  Inline
    /// caches (attrSelect / recSlot) are sized at compile time, so their
    /// `capacity()` is the live footprint.
    size_t approxBytesUsed() const noexcept
    {
        size_t b = sizeof(CompilationUnit);
        b += code.capacity()           * sizeof(Instruction);
        b += intConstants.capacity()   * sizeof(int64_t);
        b += floatConstants.capacity() * sizeof(double);
        b += stringConstants.capacity() * sizeof(std::string);
        for (const auto & s : stringConstants) b += s.capacity();
        b += symbolTable.capacity()    * sizeof(std::string);
        for (const auto & s : symbolTable) b += s.capacity();
        b += lambdas.capacity()           * sizeof(LambdaDescriptor);
        b += lambdaCodeOffsets.capacity() * sizeof(uint32_t);
        b += primops.capacity()           * sizeof(const PrimOp *);
        b += attrSelectCache.capacity()   * sizeof(AttrSelectIC);
        b += recSlotCache.capacity()      * sizeof(RecSlotIC);
        b += forceEmitSites.capacity()
             * sizeof(std::pair<uint32_t, const char *>);
        return b;
    }
};

} // namespace nix::v3
