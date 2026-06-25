#pragma once
/// @file
/// Stage 5 of the precise-root foundation (lode/
/// GC_PRECISE_ROOT_FOUNDATION_2026-05-27.md) — RAII-style C++-stack
/// root registration.
///
/// ## Purpose
///
/// The Stage 3 root walker (`walkAllV3Roots`) covers:
///   * VMState stacks + frames
///   * standalone cells / FFI bridge tables / import cache
///
/// What it does NOT cover today: transient v3 Value variables held
/// on the C++ stack of helper functions (primops, bridge helpers,
/// fix-point intrinsics, etc.).  Under the current Phase D/E nursery
/// design these are SAFE because the scavenger only fires at
/// `exitDepth == 0` (between bytecode opcodes) — primop body C++
/// frames are unwound before any scavenge can run.
///
/// Stage 6 production precise GC will run at additional safe-points
/// (e.g., periodic during long primop bodies, or at allocation
/// thresholds).  At those moments, C++-frame Values must be
/// registered or risk being treated as unreachable.
///
/// ## Design
///
/// Thread-local `std::vector<Value *>` of root pointers.  Two RAII
/// helpers:
///
///   `GcRoot(Value & v)`          — registers `&v` on construct,
///                                   unregisters on destruct.
///   `GcRoot(Value * p)`          — same but for already-pointer
///                                   variables.
///
/// Macro `V3_GC_ROOT(varname)` for the typical `Value local;` shape.
///
/// `walkAllV3Roots` visits the registry in addition to its existing
/// sources.  Order is irrelevant — the visitor handles dedup.
///
/// ## Cost
///
/// Per-frame: 1 push + 1 pop on a thread-local vector.  ~10 ns total
/// each.  Production primop bodies typically use 0-3 such locals,
/// so the added overhead is bounded by ~30 ns/primop.
///
/// ## Retirement criterion
///
/// When Stage 6 production precise GC is mature AND the safe-point
/// model has been audited to NOT fire from inside primop bodies
/// (i.e., only at fully-quiescent bytecode dispatch states), this
/// infrastructure becomes redundant for safety.  At that point it
/// could remain as defense-in-depth or be retired.
///
/// Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
///   Input Output Group.
/// SPDX-License-Identifier: Apache-2.0

#include <vector>
#include <functional>  // walkEvalScopeRoots adapter (M-5)

namespace nix::v3 {

struct Value;
struct RootVisitor;

/// Returns a reference to the calling thread's GC-root stack.
///
/// Each entry is a pointer to a Value living on the C++ stack of
/// some helper function.  Entries are LIFO via `GcRoot`'s RAII
/// scope — readers walk left-to-right which is equivalent to
/// bottom-to-top of the call chain.
std::vector<Value *> & gcRootStack() noexcept;

/// Walk the C++-stack root registry calling `visitor.visitValue(*p)`
/// for each registered entry whose pointer is non-null.
///
/// Used by `walkAllV3Roots` to extend root coverage beyond the
/// bytecode-frame walker.  External use is fine but rare —
/// the typical pattern is the `GcRoot` RAII helper below.
void walkCppStackRoots(RootVisitor & visitor) noexcept;

/// M-5 (CODEBASE_REVIEW_2026-06-11): walk the FFI EvalScope handle table as GC
/// roots.  Each valid HandleSlot::payload is a `Value *` (the v3 value an
/// embedder registered via allocClosureHandle); without this walk the major GC
/// would sweep an EvalScope-pinned-only Value (non-moving) and evacuation would
/// relocate its referent without rewriting the slot (moving) → dangle.  Dormant
/// today (only test paths use allocClosureHandle) but load-bearing before the
/// FFI opens to production embedders (#485).  Defined in ffi.cc (owner of the
/// g_liveScopes table).  Adapter form to match the global-root call site.
void walkEvalScopeRoots(const std::function<void(Value &)> & visit);

/// RAII helper: register a Value as a GC root on construct,
/// unregister on destruct.  Stack-allocate within the helper
/// function that owns the underlying Value.
///
/// Usage:
///   Value local;
///   V3_GC_ROOT(local);              // registers &local
///   // ... operations that may run a GC safe-point ...
///   // (destructor at scope exit unregisters)
///
/// Non-copyable + non-movable to prevent accidental duplicated
/// registrations.
class GcRoot
{
public:
    explicit GcRoot(Value & v) noexcept;
    explicit GcRoot(Value * p) noexcept;
    ~GcRoot() noexcept;

    GcRoot(const GcRoot &) = delete;
    GcRoot & operator=(const GcRoot &) = delete;
    GcRoot(GcRoot &&) = delete;
    GcRoot & operator=(GcRoot &&) = delete;
};

/// S1.2: root a CONTIGUOUS range of Values [data, data+n).  The canonical use is
/// a primop's C++-local `Value args[]` array — popped off the value-stack into a
/// stack copy (vm.cc OP_CALL_PRIMOP), so NOT otherwise a precise root — held
/// across a re-entrant callClosure/forceValue.  Each slot is walked AND rewritten
/// in place by walkCppStackRoots exactly like GcRoot, so the args follow a
/// mid-eval relocation instead of dangling once the conservative C-stack scan is
/// removed.  RAII: pushes n entries on construct, pops n on destruct (LIFO).
/// Non-copyable/movable.
class GcRootRange
{
    size_t n_;
public:
    GcRootRange(Value * data, size_t n) noexcept;
    ~GcRootRange() noexcept;

    GcRootRange(const GcRootRange &) = delete;
    GcRootRange & operator=(const GcRootRange &) = delete;
    GcRootRange(GcRootRange &&) = delete;
    GcRootRange & operator=(GcRootRange &&) = delete;
};

/// S1.2 Rule-4 (compute-style): root a GROWING `std::vector<Value>` accumulator whose
/// elements are COMPUTED (not source indices — so the cheap index trick used by
/// filter/partition doesn't apply, e.g. concatMap/groupBy) and held across re-entrant
/// callbacks.  Unlike GcRootRange (fixed data+n at construct), this registers the VECTOR
/// OBJECT; walkCppStackRoots reads its CURRENT data()/size() each GC, so it is
/// REALLOC-SAFE (push_back may move the buffer) and covers elements added after
/// construction.  Each element is walked + rewritten in place.  RAII register/unregister.
class GcRootVec
{
public:
    explicit GcRootVec(std::vector<Value> & v) noexcept;
    ~GcRootVec() noexcept;

    GcRootVec(const GcRootVec &) = delete;
    GcRootVec & operator=(const GcRootVec &) = delete;
    GcRootVec(GcRootVec &&) = delete;
    GcRootVec & operator=(GcRootVec &&) = delete;
};

/// Convenience: register the given Value-typed local for GC root
/// scanning until the enclosing scope exits.  Uses `__COUNTER__` to
/// allow multiple V3_GC_ROOT(...) calls in the same scope without
/// macro-hygiene collisions.
#define V3_GC_ROOT_CONCAT_INNER(a, b) a##b
#define V3_GC_ROOT_CONCAT(a, b) V3_GC_ROOT_CONCAT_INNER(a, b)
#define V3_GC_ROOT(varname) \
    ::nix::v3::GcRoot V3_GC_ROOT_CONCAT(_v3_gcroot_, __COUNTER__)(varname)

} // namespace nix::v3
