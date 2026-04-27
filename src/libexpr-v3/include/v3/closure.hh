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

struct Closure
{
    const LambdaDescriptor * desc;  // shared blueprint
    Env *                    withEnv; // null if not created inside a with
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
            Env * withEnv;
        } suspended;
        // ThunkState::Evaluated — the cached value.
        Value evaluated;
        // ThunkState::Native — primop wrapper.
        struct {
            const PrimOp * fn;
            // args follow as FAM
        } native;
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

    /// Formal parameters (`{a, b ? def}: body`).  Each entry is the
    /// SymbolId of the formal name plus a flag indicating whether it
    /// has a default value.  Used by `builtins.functionArgs` to
    /// introspect the function's parameter list.  Empty for simple
    /// `x: ...` lambdas.
    std::vector<std::pair<uint32_t, bool>> formals;
};

struct ThunkDescriptor
{
    uint32_t codeOffset;
    uint16_t nUpvalues;
    uint16_t nLocals;
};

} // namespace nix::v3
