#pragma once
/// @file
/// v3 Closure / Thunk / Env representation.
///
/// Design (per doc/v3-design/v3-design.md §3.2-§3.5):
///
///   Closure {
///     LambdaDescriptor * desc;     // shared blueprint (code, formals, etc.)
///     Env *              withEnv;  // null when no enclosing `with`
///     Value              upvalues[FAM];
///   }
///
///   No carrier Env around upvalues.  Closures with nUpvalues == 0 and no
///   enclosing `with` are 16 bytes total — one alloc per closure.
///
///   Thunk {
///     State        state;          // Suspended / Blackhole / Evaluated / Native
///     ...
///   }
///
///   Env (only for `let` / `with` scopes — NOT for closure upvalues):
///     parent + values[FAM]
///
/// Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.
/// SPDX-License-Identifier: Apache-2.0

#include "v3/value.hh"

#include <cstdint>
#include <cstddef>
#include <utility>
#include <vector>

namespace nix::v3 {

struct LambdaDescriptor;
struct ThunkDescriptor;
struct PrimOp;

// ---------------------------------------------------------------------------
// Env: only for `let`/`with` scopes, NOT for closure upvalues.
// ---------------------------------------------------------------------------

/// Env is allocated by OP_ENTER_LET / OP_PUSH_WITH / OP_INHERIT_FROM_INIT.
/// Closures use Closure directly (FAM upvalues); thunks use Thunk directly.
/// This dramatically reduces the env-allocation count vs v2.
struct Env
{
    Env *  parent;        // outer scope, or nullptr at the base.
    bool   isWithEnv;     // true if values[0] holds a with-attrset.
    uint16_t nValues;     // size of FAM (slot count).
    Value  values[];      // FAM
};

// ---------------------------------------------------------------------------
// Closure
// ---------------------------------------------------------------------------

struct CompilationUnit;

struct Closure
{
    const LambdaDescriptor * desc;        // shared blueprint
    /// CompilationUnit owning desc + the bytecode it points into.
    /// Required for cross-CU calls (e.g., closures returned by
    /// `builtins.import` from another file).  When null, the dispatch
    /// loop uses the caller's CU — fine for intra-CU calls.
    const CompilationUnit *  cu;
    /// Snapshot of the `with`-stack visible at MAKE_CLOSURE.  null when
    /// no enclosing `with` is in scope at definition time.  When the
    /// closure is invoked, the dispatcher re-pushes these onto the
    /// runtime with-stack so OP_WITH_LOOKUP inside the body finds them.
    ListVec *                capturedWiths;
    uint16_t                 nUpvalues;
    uint16_t                 _pad;
    Value                    upvalues[]; // FAM
};

// ---------------------------------------------------------------------------
// Thunk
// ---------------------------------------------------------------------------

enum class ThunkState : uint8_t {
    Suspended = 0,
    Blackhole = 1,
    Evaluated = 2,
    Native    = 3,
    /// WC-10 (Option 1): a thunk that, when forced, calls back into
    /// tree-walker for a single nix::Value*, then bridges the
    /// already-forced result to a v3 Value via treeWalkerToV3.
    /// Used by the rec-attrset materialisation path: instead of
    /// eagerly bridging every rec entry's body up-front, we
    /// allocate a Bridge thunk per entry — only entries the v3
    /// thunk's body actually accesses pay the bridge cost.
    Bridge    = 4,
};

struct Thunk
{
    ThunkState state;
    uint8_t    _pad0;
    uint16_t   nUpvalues;   // for Suspended state
    uint32_t   _pad1;

    union {
        // ThunkState::Suspended
        struct {
            const ThunkDescriptor * desc;
            /// Same semantics as Closure::capturedWiths.
            ListVec * capturedWiths;
            /// Same semantics as Closure::cu.
            const CompilationUnit * cu;
        } suspended;
        // ThunkState::Evaluated — the cached value.
        Value evaluated;
        // ThunkState::Native — primop wrapper.
        struct {
            const PrimOp * fn;
            // args follow as FAM
        } native;
        // ThunkState::Bridge — pointer to a tree-walker nix::Value
        // that the OP_FORCE handler will forceValue + bridge on
        // access.  Stored as `void *` so closure.hh stays free of
        // nix:: includes; cast to nix::Value * at the use site.
        void * bridgeSrc;
    };

    // FAM: upvalues[nUpvalues] for Suspended; args[fn->arity] for Native.
    Value tail[];
};

// ---------------------------------------------------------------------------
// LambdaDescriptor / ThunkDescriptor (shared blueprints)
// ---------------------------------------------------------------------------

struct LambdaDescriptor
{
    uint32_t codeOffset;    // start of the body in CompilationUnit::code
    uint32_t prologueOffset; // for formals; same as codeOffset for simple lambdas
    uint16_t nUpvalues;     // count of upvalues this closure captures
    uint16_t nLocals;       // stack slots needed in the body's frame
    uint8_t  arity;         // 1 for simple `x: ...`; >1 for currying (later)
    uint8_t  hasFormals;    // 0 = simple arg, 1 = formals attrset
    uint8_t  ellipsis;      // formals with `...` accept extra args; otherwise reject

    /// Formal parameters (`{a, b ? def}: body`).  Each entry is
    /// (name SymbolId, hasDefault, posHandle).  posHandle is an index
    /// into the global posSnapshotPool; 0 means unknown.  Used by
    /// `builtins.functionArgs` (the bool drives the result value, the
    /// pos feeds the per-attr side-table so `unsafeGetAttrPos` works).
    struct Formal {
        uint32_t name;
        bool     hasDefault;
        uint32_t pos;
    };
    std::vector<Formal> formals;
    /// WC-17.1 diagnostic name (lambda or rec-attrset attr name).
    /// Mirrors ir::Function::name; populated by compile().  Used by
    /// V3_DBG_OPCYCLE / disassembler dumps to map LambdaDescriptor
    /// pointers back to the original AST scope.
    std::string name;
};

struct ThunkDescriptor
{
    uint32_t codeOffset;
    uint16_t nUpvalues;
    uint16_t nLocals;
};

} // namespace nix::v3
