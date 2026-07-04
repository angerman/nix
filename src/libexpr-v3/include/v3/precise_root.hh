#pragma once
/// @file
/// Precise-root enumeration for the v3 VM — Stage 3 of the precise-root
/// foundation (`lode/GC_PRECISE_ROOT_FOUNDATION_2026-05-27.md`).
///
/// `walkAllV3Roots(VMState &, RootVisitor &)` enumerates every v3-heap
/// pointer reachable from a root source — VM stacks, call frames,
/// registered global roots, FFI bridge tables.  The visitor decides
/// what to do with each pointer (observe, count, rewrite for moving
/// GC, etc.).
///
/// This is the foundation for replacing Boehm's conservative scan of
/// the v3 arena.  It is NOT a tracing GC — it walks ROOTS only.
/// Transitive walk (mark phase) is the consumer's responsibility.
///
/// What counts as a root:
///   1. `VMState::frames`           — call-frame closure / thunk /
///                                    forceWriteTarget
///   2. `VMState::valueStack`       — operand + local stack
///   3. `VMState::withStack`        — `with`-expression scope chain
///   4. `activeVMStack()`           — secondary VMStates (under
///                                    nested runFunctionWithUpvalues)
///   5. `standaloneCellRoots()`     — manually-registered Values
///                                    (singletons, transient cells)
///   6. `walkV3BridgeRoots()`       — v3BridgeClosures / Attrs /
///                                    Lists FFI handle tables
///   7. `walkImportCacheRoots()`    — in-memory primImport result
///                                    cache
///
/// NOT root sources (audited and intentionally excluded):
///   * `cellOwnerTable()` — METADATA only; every Thunk* in the
///     table is also reachable via the owning cell (sources 1, 5,
///     dirty list).  Walking would double-count.
///   * `drvHashCacheMap()` — holds SERIALIZED BYTES (std::string),
///     not live v3 Value pointers.  No v3-heap pointer reaches the
///     cache.
///
/// Visitor pattern: subclasses override the per-pointer-type
/// callbacks for type-specific logic (e.g., moving GC uses different
/// forward functions per pointee type).  `visitValue(Value &)` is
/// provided as the dispatch hub — scalar tags are no-ops, pointer-
/// bearing tags call the appropriate typed callback.
///
/// Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
///   Input Output Group.
/// SPDX-License-Identifier: Apache-2.0

#include "v3/value.hh"
#include "v3/closure.hh"

namespace nix::v3 {

struct VMState;

/// Memory-bucket classification of a root source.  `walkAllV3Roots`
/// announces, via `RootVisitor::enterRootSource`, which class of root
/// it is about to enumerate.  A *bucketing* visitor (the memory-bucket
/// accounting in live_trace.cc) uses this to attribute each
/// first-touch-marked cell to a live-bytes bucket:
///
///   * `Eval`    — the genuine working set: VMState stacks/frames,
///                 `with` scopes, standalone cells, builtins, callFlake
///                 + deepForce temporaries, C++-stack GcRoots.
///   * `CuCache` — Values pinned by the in-memory `import` result cache
///                 (the bytecode + eval-result cache for imported
///                 `.nix` files; see primops.cc ImportCache).
///   * `Ffi`     — Values held by FFI bridge tables.  Retired post-F4
///                 (TW_VALUE_ERADICATION_GOAL_2026-06-02) — currently
///                 emitted by no walk, kept for completeness / future
///                 FFI-leaf root sources.
///
/// The eval-first ordering (`Eval` before `CuCache`) makes a cell that
/// is reachable from BOTH eval and a cache attribute to `Eval`, so a
/// cache's bucket reports only the bytes it pins *beyond* the working
/// set (marginal-retention semantics).
enum class RootSource { Eval, CuCache, Ffi };

/// Visitor base class for `walkAllV3Roots`.  Each callback receives
/// a REFERENCE to the slot containing the pointer so a moving-GC
/// visitor can rewrite the slot in place.
///
/// Subclass + override the callbacks you care about.  By default
/// `visitValue` dispatches on `tagIsPointer` (centralised in
/// value.hh) — overriding `visitValue` directly is rarely needed.
struct RootVisitor
{
    virtual ~RootVisitor() = default;

    /// Announced by `walkAllV3Roots` immediately before it enumerates
    /// each class of root source.  Default no-op: marking / scavenging
    /// / counting visitors (MarkVisitor, Scavenger, DumpVisitor, …) are
    /// unaffected and pay nothing.  Only the memory-bucket tracer
    /// overrides this — to drain the worklist under the OLD source label
    /// then switch, giving exact eval-first first-touch attribution.
    virtual void enterRootSource(RootSource) noexcept {}

    // Per-pointer-type callbacks.  Pure virtual to force the consumer
    // to think about each pointer type.  No-op implementations are
    // available via the `NopRootVisitor` helper below.
    virtual void visitClosure  (Closure   * & slot) = 0;
    virtual void visitThunk    (Thunk     * & slot) = 0;
    virtual void visitBindings (Bindings  * & slot) = 0;
    virtual void visitList     (ListVec   * & slot) = 0;
    virtual void visitPair     (ValuePair * & slot) = 0;
    /// `Tag::Slot` — slot is a `Value*` into a tenured cell; the
    /// pointed-to Value may carry its own payload that needs walking.
    virtual void visitSlot     (Value     * & slot) = 0;

    /// Stage 6 Day 4: arena-allocated raw bytes (Alloc::allocChars).
    /// Tag::String + Tag::Path carry a `const char *` to a buffer in
    /// the v3 arena.  For non-moving visitors these are no-ops;
    /// MajorScavenger forwards each buffer to its NEW backup-resident
    /// address (with string-context side-table re-keying).
    ///
    /// Default no-op so existing visitors (LiveTracer, DumpVisitor,
    /// TagCountVisitor, etc.) need no change — they don't care
    /// about string forwarding.  Pure virtual would force every
    /// visitor to implement; default-impl trades pure-virtual safety
    /// for migration simplicity.
    virtual void visitString   (const char * & s) noexcept { (void)s; }
    virtual void visitPath     (const char * & s) noexcept { (void)s; }

    /// A shared frame `Env *` root — the
    /// CallFrame::defEnv holding a frame's escaping locals, reachable ONLY via
    /// the frame register until a child captures it, so it must be walked as a
    /// distinct root class (the upvalEnv walkers do NOT cover it).  DEFAULT: walk
    /// each Env's values + its parent chain via visitValue — correct for the
    /// EVAC visitor (rewrites the value pointers; the Env cell is non-moving) and
    /// for tracers.  A MARKING visitor MUST OVERRIDE to also set the Env cell's
    /// mark bit (else sweep frees a live Env whose only root is the frame
    /// register → UAF).  Env parent chains are acyclic (lexical nesting), so the
    /// plain walk terminates.  Default no-op-ish today: CallFrame::defEnv is null
    /// until W2 emission, so walkAllV3Roots never calls this on a real Env.
    virtual void visitEnv      (Env * & e)
    {
        for (Env * cur = e; cur; cur = cur->parent)
            for (uint16_t i = 0; i < cur->nValues; ++i)
                visitValue(cur->values[i]);
    }

    /// Convenience: visit a Value slot.  Dispatches on `tag()` and
    /// calls the appropriate typed callback.  Scalar tags are
    /// no-ops.  Non-virtual to give the compiler full visibility for
    /// inlining when called from the walker.
    void visitValue(Value & v) noexcept
    {
        // visitX() may MUTATE the pointer (a moving walker rewrites the
        // field).  L0 accessors return by value (no stable lvalue inside an
        // 8B word), so read-into-local → visit → write back via the
        // tag-preserving mkX() setter.  (A read-only walker leaves the local
        // unchanged, so the write-back is a no-op.)
        switch (v.tag()) {
        case Tag::Closure:
            { auto p = v.asClosure(); visitClosure(p); v.mkClosure(p); }
            break;
        case Tag::Thunk:
            { auto p = v.asThunk(); visitThunk(p); v.mkThunk(p); }
            break;
        case Tag::Attrs:
            { auto p = v.asAttrs(); visitBindings(p); v.mkAttrs(p); }
            break;
        case Tag::List:
            { auto p = v.asList(); visitList(p); v.mkList(p); }
            break;
        case Tag::App:
        case Tag::App3:
        case Tag::PrimOpApp:
            { auto p = v.asPair(); visitPair(p); v.mkPair(v.tag(), p); }
            break;
        case Tag::Slot:
            { auto p = v.asSlot(); visitSlot(p); v.mkSlot(p); }
            break;
        case Tag::String:
            { auto s = v.asString(); visitString(s); v.mkString(s); }
            break;
        case Tag::Path:
            { auto s = v.asPath(); visitPath(s); v.mkPath(s); }
            break;
        // Scalar / external tags: no v3-heap pointer to walk.
        case Tag::Uninitialized:
        case Tag::Int:
        case Tag::Float:
        case Tag::Bool:
        case Tag::Null:
        case Tag::PrimOp:
        case Tag::Blackhole:
        case Tag::External:
            break;
        }
    }
};

/// No-op visitor base: implements every callback as `void` so
/// subclasses can override only what they need.  Use as a starting
/// point for diagnostic / measurement visitors that don't need
/// type-specific behavior.
struct NopRootVisitor : RootVisitor
{
    void visitClosure  (Closure   * &) override {}
    void visitThunk    (Thunk     * &) override {}
    void visitBindings (Bindings  * &) override {}
    void visitList     (ListVec   * &) override {}
    void visitPair     (ValuePair * &) override {}
    void visitSlot     (Value     * &) override {}
};

/// Walk every v3-heap root reachable from a root source.
///
/// Stage 3 coverage (complete as of 2026-05-27):
///   * `vm.frames`       — closure, thunk, forceWriteTarget
///   * `vm.valueStack`
///   * `vm.withStack`
///   * `activeVMStack()` — other VMStates
///   * `standaloneCellRoots()` — registered Value cells
///   * `walkV3BridgeRoots()` — FFI bridge tables (closures + attrs + lists)
///   * `walkImportCacheRoots()` — primImport result cache
///
/// The visitor receives every pointer slot — including null pointers,
/// which it can ignore.  No transitive walk: the visitor decides
/// whether to follow pointers (e.g., a moving-GC visitor follows;
/// a counting visitor does not).
void walkAllV3Roots(VMState & vm, RootVisitor & visitor) noexcept;

/// Walk ONLY the global root sources (standalone cells, bridge
/// tables, import cache).  Does NOT walk any VMState — useful at
/// end-of-run when no VMState is active but persistent global roots
/// still hold the residual live set.
///
/// Subset of `walkAllV3Roots`'s coverage:
///   * `standaloneCellRoots()`
///   * `walkV3BridgeRoots()`
///   * `walkImportCacheRoots()`
///
/// Mid-eval callers should prefer `walkAllV3Roots(vm, ...)` since it
/// includes the active VMState's stacks + frames.  End-of-run callers
/// (atexit, post-teardown) get an honest residual measurement here.
void walkGlobalV3Roots(RootVisitor & visitor) noexcept;

/// Diagnostic: under `V3_DBG_ROOT_DUMP=1`, called once at end of run
/// to print the root set.  Useful for cross-checking against Boehm's
/// view in subsequent V3_DBG_ROOT_PARITY work.
///
/// Looks up the active VMState via `activeVMStack()`.  If no VMState
/// is active when called (e.g., after eval has fully completed and
/// teared down), prints a "no active vm" line instead.  Caller does
/// not need VMState in hand.
void dumpAllV3Roots() noexcept;

} // namespace nix::v3
