#pragma once
/// @file
/// v3 FFI surface — clean-room boundary between the v3 evaluator and
/// the rest of nix (parser, store, fetchers, settings, logger).
///
/// Per `lode/FFI_PLAN_2026-05-06.md` and the review-correction companion
/// `lode/FFI_PLAN_2026-05-06b.md`.  Migration step 1: header pack
/// declaring the full surface (~59 functions, ~20 types, 13 categories).
///
/// This is a SKELETON.  Most types are forward-declared / opaque; many
/// signatures reference framework types (Fallible, BlockingFFI,
/// EvalScope) that are defined here but not yet wired into the existing
/// evaluator.  The migration plan (FFI_PLAN_2026-05-06b §"Migration
/// path") brings each category in over multiple steps; this header is
/// the durable contract that all the steps build toward.
///
/// **Categories:**
///   A. Parser                      (5 entries + 1 type)
///   B. Symbol & position tables    (6 entries + 2 types)
///   C. Filesystem I/O              (readFile, readDir, pathExists, …)
///   D. Network fetchers            (fetchurl, fetchTree, …)
///   E. Store operations            (10+ entries + types)
///   F. Derivation construction     (DerivationDescriptor, expanded)
///   G. Closure / handle ABI        (EvalScope + ClosureHandle)
///   H. Errors                      (EvalError, structured trace)
///   I. Settings snapshot
///   J. Logger / activity
///   K. Sandbox / pure-eval gating  (PrimOp flags, see primop.hh)
///   L. String context              (4-variant ContextElem)
///   M. Latency-class wrappers      (BlockingFFI<T>)
///
/// **Status (as of 2026-05-07):**
///   - Category K (PrimOp flags): #486 done, see primop.hh PrimOpFlags.
///   - Category L (string context): #487 done, plan corrected to match
///     `nix::ContextElem`'s 3-variant {Opaque, DrvDeep, Built}.
///   - Categories A–J, M: skeleton declarations only; impl in later steps.
///
/// **NOT in this header:**
///   - The internal `nix::v3::PrimOp` struct (in primop.hh) -- v3-internal,
///     not part of the FFI surface.
///   - The `Value` representation, bytecode dispatch, Closure layout --
///     v3 owns these end-to-end; FFI doesn't expose them.
///
/// Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.
/// SPDX-License-Identifier: Apache-2.0

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

// FFI surface depends on a few small types from libnixutil / libnixstore.
// The plan's long-term goal is to minimise these dependencies (v3 should
// link against libnixstore + a parser, not full libnixexpr).  These
// includes track the CURRENT pinch points; future steps may opaque-wrap
// them.  Heavy headers (eval.hh, value.hh) are NOT pulled in here.
#include "nix/util/pos-idx.hh"
#include "nix/util/experimental-features.hh"
#include "nix/util/source-path.hh"
#include "nix/store/path.hh"

// Forward declarations for types where opaque-by-pointer is sufficient.
namespace nix {
    struct Pos;
    struct StaticEnv;
    class  Store;
    class  Logger;
}

namespace nix::v3 {

class Value; // v3 16-byte tagged value (declared in value.hh)

// =========================================================================
// Framework: lifetime + error + latency-class wrappers
// =========================================================================

/// **Category H — EvalError** (FFI_PLAN_2026-05-06b §A5).
///
/// V8-style: separate the unformatted message + structural trace from
/// rendering.  v3 supplies positions and a HintFmt-compatible message;
/// the host renders.  Width / colour / terminal-vs-JSON is the
/// renderer's concern, not v3's.
struct TraceFrame
{
    nix::PosIdx pos;        ///< 1-based index into shared PosTable; 0 = unknown.
    std::string hint;       ///< HintFmt-compatible (the renderer formats).
};

struct Suggestion
{
    std::string  text;       ///< suggested replacement / fix-up.
    unsigned int distance;   ///< Levenshtein distance from the bad input.
};

struct EvalError
{
    std::string             msg;          ///< unformatted; HintFmt-style.
    nix::PosIdx             primaryPos;   ///< 1-based; 0 = no position.
    std::vector<TraceFrame> trace;        ///< innermost-first.
    unsigned                exitStatus = 1;
    std::vector<Suggestion> suggestions;
};

/// **Category H — Fallible<T>** (FFI_PLAN_2026-05-06b §A7).
///
/// Boundary-only -- internally v3 throws C++ exceptions; the FFI shim
/// catches at the crossing and constructs the variant.  Symmetrically,
/// errors *returned* into the FFI by the host are re-thrown inside v3.
/// Avoids GraalVM's "every boundary is typed" overhead while preserving
/// ABI clarity at the crossing.
template <typename T>
struct Fallible
{
    std::variant<T, EvalError> data;

    bool ok() const noexcept { return data.index() == 0; }
    T &        unwrap() &       { return std::get<0>(data); }
    const T &  unwrap() const & { return std::get<0>(data); }
    T &&       unwrap() &&      { return std::move(std::get<0>(data)); }
    const EvalError & error() const & { return std::get<1>(data); }
};

/// **Category M — BlockingFFI<T>** (FFI_PLAN_2026-05-06b §A10).
///
/// Erlang NIF/Port distinction: tag potentially-slow FFI calls
/// (fetchurl, addToStore, realiseDerivation) at the type level.  Wrapper
/// is a no-op semantically -- documents the latency class and lets
/// future scheduling / profiling hooks differentiate Port-class
/// (potentially-slow) from NIF-class (must-return-fast) calls.
///
/// Usage:
///   BlockingFFI<FetchResult> fetchurl(URL, …);          // Port-class
///   Fallible<StorePath>      parseStorePath(string_view); // NIF-class
template <typename T>
struct BlockingFFI : Fallible<T>
{
    using Fallible<T>::Fallible;
};

/// **Category G — EvalScope** (FFI_PLAN_2026-05-06b §A1, highest priority).
///
/// RAII handle lifetime.  Mirrors V8's HandleScope, OCaml's CAMLparam,
/// JNI's local refs.  ClosureHandle is valid only within an enclosing
/// EvalScope; GlobalClosureHandle escapes the scope and is released
/// explicitly.  Exposing INCREF/DECREF to the host (CPython's mistake)
/// is universally regretted; this hides it behind RAII.
///
/// The literature is unanimous: every embedded-VM FFI gets handle
/// lifetime as a discipline; the current plan's bare ClosureHandle
/// re-derives CPython's bugs.
class Evaluator; // forward (impl class for the v3 evaluator instance)

class EvalScope
{
public:
    explicit EvalScope(Evaluator & e);
    ~EvalScope(); ///< invalidates all ClosureHandles created within.

    EvalScope(const EvalScope &) = delete;
    EvalScope & operator=(const EvalScope &) = delete;

private:
    Evaluator & m_ev;
    void *      m_state; ///< opaque per-scope state (frame pointer / handle list)
};

/// Local handle: valid within the enclosing EvalScope; auto-released
/// at scope exit.  Use this for transient handles (most FFI calls).
struct ClosureHandle
{
    uint64_t opaque;  ///< implementation-defined; do not interpret.
};

/// Global handle: outlives the enclosing scope; explicitly released.
/// Use for callbacks that must survive a single FFI call (e.g., a
/// PathFilter used during a long copyPathToStore).
struct GlobalClosureHandle
{
    uint64_t opaque;
};

GlobalClosureHandle promoteToGlobal(EvalScope &, ClosureHandle);
void                releaseGlobal  (GlobalClosureHandle);

// =========================================================================
// Category A: Parser (5 entries + 1 type)
// =========================================================================
//
// Per FFI_PLAN_2026-05-06b §A8: the parser currently lives in libnixexpr
// alongside the tree-walker.  Extracting it into a standalone shared
// library is a deferred milestone; this declares the v3-side contract
// the eventual extraction must satisfy.

struct Expr; // opaque AST root; v3 walks once at lower-time.

Expr *           parseExprFromFile  (nix::SourcePath path);
Expr *           parseExprFromString(std::string_view src, nix::SourcePath origin);
void             bindVars           (Expr * e, const nix::StaticEnv & env);
nix::SourcePath  canonicalisePath   (std::string_view raw, nix::SourcePath cwd);
const nix::Pos & lookupPos          (nix::PosIdx);

// =========================================================================
// Category B: Symbol & position tables (6 entries + 2 types)
// =========================================================================

struct SymbolTable;
struct PosTable;

using SymbolId = uint32_t;

SymbolId         intern (SymbolTable &, std::string_view name);
std::string_view str    (const SymbolTable &, SymbolId);
nix::PosIdx      addPos (PosTable &, nix::Pos);
nix::Pos         lookup (const PosTable &, nix::PosIdx);

/// **§A6**: position threading lookup helper.  Per-call `PosIdx caller`
/// stays in FFI signatures (documents intent + lets primops pass an
/// explicit override), but the IMPLEMENTATION reads from
/// `currentPos(vm)` at the boundary rather than maintaining a per-
/// instruction field.  CallFrame has no position field today.
struct VMState; // forward (vm.hh)
nix::PosIdx currentPos(const VMState &);

// =========================================================================
// Category C: Filesystem I/O
// =========================================================================
//
// Disambiguated from "store I/O" per the original plan's hole #1.
// These are pure-filesystem reads -- no store daemon involvement.
// Subject to restricted-eval / pure-eval gating at the dispatch layer
// (PrimOp flags, category K).

Fallible<std::string>            readFile  (nix::SourcePath);
Fallible<std::map<std::string, std::string>> readDir(nix::SourcePath); ///< name -> file-type
bool                              pathExists(nix::SourcePath);
Fallible<nix::SourcePath>         findFile  (std::string_view name);

// =========================================================================
// Category D: Network fetchers
// =========================================================================
//
// All Port-class (BlockingFFI<T>) -- network calls can take seconds.

struct FetchResult
{
    nix::StorePath path;
    std::map<std::string, std::string> attrs;
};

BlockingFFI<FetchResult> fetchurl   (std::string_view url);
BlockingFFI<FetchResult> fetchTree  (const std::map<std::string, std::string> & input);
BlockingFFI<FetchResult> fetchTarball(std::string_view url);

// =========================================================================
// Category E: Store operations (10+ + types)
// =========================================================================
//
// Per FFI_PLAN_2026-05-06b §A12: addMultipleToStore, computeFSClosure,
// PathFilter callback type added (count goes 10 -> 13).

using PathFilter = std::function<bool(std::string_view path)>;

struct StoreHandle; // opaque

BlockingFFI<nix::StorePath>      addToStore        (StoreHandle &, nix::SourcePath, PathFilter = {});
BlockingFFI<std::vector<nix::StorePath>>
                                  addMultipleToStore(StoreHandle &, std::vector<nix::SourcePath>);
BlockingFFI<nix::StorePath>      addTextToStore    (StoreHandle &, std::string_view name, std::string_view text);
BlockingFFI<bool>                isValidPath       (StoreHandle &, nix::StorePath);
BlockingFFI<std::set<nix::StorePath>>
                                  computeFSClosure  (StoreHandle &, nix::StorePath, bool flipDirection = false);
Fallible<nix::StorePath>         parseStorePath    (StoreHandle &, std::string_view);
Fallible<std::string>            printStorePath    (StoreHandle &, nix::StorePath);
BlockingFFI<void>                ensurePath        (StoreHandle &, nix::StorePath);
BlockingFFI<void>                copyPathToStore   (StoreHandle &, nix::StorePath, StoreHandle & dst);

// =========================================================================
// Category F: Derivation construction (DerivationDescriptor + factory)
// =========================================================================
//
// Per FFI_PLAN_2026-05-06b §A3: 10 missing fields + nested OutputChecks.
// derivationStrict is a SUBSYSTEM, not a single primop -- handles fixed-
// output, content-addressed, structuredAttrs, passAsFile, impure,
// allowedReferences, multiple outputs, placeholder synthesis, .drv file
// writing.
//
// **TODO (#488):** populate this struct fully.  Today, derivationStrict
// has hand-rolled checks across primops.cc; this descriptor lifts them
// to one place.

struct DerivationDescriptor
{
    // Required identity
    std::string                name;
    std::string                system;
    std::string                builder;
    std::vector<std::string>   args;

    // Outputs
    std::vector<std::string>   outputs;

    // Fixed-output / content-addressed
    std::optional<std::string> outputHash;
    std::optional<std::string> outputHashAlgo;
    std::optional<std::string> outputHashMode;

    // Sandbox / platform (§A3 first batch)
    bool                       noChroot                   = false; ///< __noChroot
    std::optional<std::string> sandboxProfile;                     ///< __sandboxProfile (darwin)
    bool                       darwinAllowLocalNetworking = false;
    std::set<std::string>      impureHostDeps;                     ///< __impureHostDeps
    std::set<std::string>      impureEnvVars;

    // Scheduling (§A3 second batch)
    std::set<std::string>      requiredSystemFeatures;
    bool                       preferLocalBuild = false;
    bool                       allowSubstitutes = true;

    // Reference graph export (§A3 third batch)
    std::map<std::string, nix::StorePath> exportReferencesGraph;

    // Lowering hint
    bool                       ignoreNulls      = false; ///< __ignoreNulls

    // Per-output checks (§A3 -- NESTED, not flat).  derivation-options.hh
    // lets each output have its own checks; flattening loses information.
    struct OutputChecks
    {
        std::optional<std::set<nix::StorePath>> allowedReferences;
        std::optional<std::set<nix::StorePath>> allowedRequisites;
        std::optional<std::set<nix::StorePath>> disallowedReferences;
        std::optional<std::set<nix::StorePath>> disallowedRequisites;
        std::optional<uint64_t>                  maxSize;
        std::optional<uint64_t>                  maxClosureSize;
    };
    std::map<std::string /*outputName*/, OutputChecks> outputChecks;

    // structuredAttrs JSON-blob (when __structuredAttrs is set).
    std::optional<std::string> structuredAttrsJSON;
};

BlockingFFI<nix::StorePath> realiseDerivation(StoreHandle &, const DerivationDescriptor &);

// =========================================================================
// Category G: Closure / handle ABI (defined above)
// =========================================================================
//
// applyClosure: invoke a v3 closure via its handle.  NIF-class because
// the body runs in v3 (no I/O); but body may itself trigger Port-class
// calls (fetchers).  Wrapping at this level would force every closure
// call to be Port-class, which would hide the cost of inner blocking
// calls.  Keep as Fallible; let inner BlockingFFI bubble up.
Fallible<Value> applyClosure(EvalScope &, ClosureHandle, Value arg);
Fallible<Value> applyClosureN(EvalScope &, ClosureHandle, std::vector<Value> args);

// Closure introspection (for builtins.functionArgs, autoCallFunction).
struct ClosureFormals
{
    struct Formal
    {
        SymbolId    name;
        bool        hasDefault;
        nix::PosIdx pos;
    };
    std::vector<Formal> formals;
    bool                ellipsis;
};

Fallible<std::optional<ClosureFormals>> getClosureFormals(EvalScope &, ClosureHandle);

// =========================================================================
// Category I: Settings snapshot
// =========================================================================
//
// Per the original plan's hole #6: v3 reads pure-eval, restricted-eval,
// allowed-uris, substituters, NIX_PATH, current-system, plus a few
// flags.  Snapshot at scope start (not threaded per-call) -- mid-eval
// settings change is undefined behaviour.

struct EvalSettings
{
    bool                       pureEval        = false;
    bool                       restrictedEval  = false;
    bool                       readOnlyMode    = false;
    std::set<std::string>      allowedUris;
    std::vector<std::string>   substituters;
    std::vector<nix::SourcePath> nixPath;
    std::string                currentSystem;
    std::set<nix::ExperimentalFeature> experimentalFeatures;
};

const EvalSettings & currentSettings(const VMState &);

// =========================================================================
// Category J: Logger / activity (stateful observer)
// =========================================================================
//
// Per the original plan's hole #11: build progress, copying paths.
// Not a simple settings field -- needs an interface so the host can
// observe stages of long-running operations.

class Logger; // opaque (impl in libnixutil)

void setLogger(Logger *);

// =========================================================================
// Category K: Sandbox / pure-eval gating
// =========================================================================
//
// Already done (#486 / FFI A13): see `nix::v3::PrimOpFlags` in primop.hh.
// Dispatch at OP_CALL_PRIMOP entry consults the flags before running the
// body.  Removes per-primop boilerplate.

// =========================================================================
// Category L: String context (4-variant ContextElem)
// =========================================================================
//
// Already documented (#487 / FFI A4): see lib/value/context.hh.
// Plan was wrong about variant count; actual type uses 3-way variant
// {Opaque, DrvDeep, Built} where DrvDeep ("any output of drv") is
// orthogonal to Built ("specific named output").
//
// Cross-boundary representation -- forward-declared here for FFI sigs
// that need to thread context through.

struct ContextElem; // defined in libnixexpr's value/context.hh
using StringContext = std::set<ContextElem>;

// =========================================================================
// Category M: Latency-class wrappers (defined above)
// =========================================================================

// =========================================================================
// Eval entry points
// =========================================================================
//
// The "outermost" v3 surface: the host hands an Expr (parsed) and gets
// back a Value (or an error).  All other FFI calls are reachable from
// inside the v3 evaluator while it's running this Expr.

Fallible<Value> evalExpr(Evaluator &, Expr *);
Fallible<Value> evalFile(Evaluator &, nix::SourcePath);

// =========================================================================
// Plugin ABI (FFI_PLAN_2026-05-06b §A9)
// =========================================================================
//
// **Decision:** Option B (compat shim) during transition, Option A
// (hard cut) after a deprecation window.  See plan §A9 for rationale.
//
// **Option B (active during transition):** old plugins continue to use
// `RegisterPrimOp` (libnixexpr's primops.hh).  At plugin-load time,
// v3's hook intercepts each registration and wraps the old `PrimOp`
// into a `v3::PrimOp` whose `fn` marshals v3 Values to TW Values
// before calling the old `impl`, then bridges the TW result back.
// Slow-path -- one extra round-trip per plugin call -- but no
// compatibility break.  No FFI declarations needed for Option B; the
// shim lives entirely inside v3 (registerPrimOp interception in
// `lower.cc` / `v3_hook.cc`).
//
// **Option A (post-deprecation):** new plugins use the v3-native ABI:
//
//   extern "C" void nix_plugin_v3(::nix::v3::PrimOpRegistry &);
//
// The plugin's entry point registers v3-native PrimOp values directly,
// skipping the marshaling round-trip.  Deprecation window: until at
// least one full Nix release after Option A is announced; older
// plugins fall through Option B's shim.

class PrimOpRegistry; // forward (impl in primop_registry.hh, future)

/// Plugin entry-point signature for Option A (the post-deprecation
/// v3-native plugin ABI).  Not yet wired -- the registry type is
/// pending design.  Plugins that don't define this symbol are
/// loaded via the Option B compat shim.
extern "C" using V3PluginEntry = void (*)(PrimOpRegistry &);

} // namespace nix::v3
