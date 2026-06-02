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
#include "nix/util/hash.hh"        // Hash / HashAlgorithm — shared store/drv domain type
#include "nix/store/path.hh"
// Layer-0 util re-export: eval-trace.hh is a STANDALONE tracing facility
// (it #includes only <cstdio>/<atomic>/<string>/… — zero coupling to the
// tree-walker evaluator).  Re-exporting it here lets v3 TUs reach
// `nix::evalTrace::{enabled,enterForce,leaveWhnf,formatPos,…}` INLINE
// (the hot force/dispatch guards stay zero-cost) without each TU carrying
// a direct `nix/util/eval-trace.hh` include the FFI lint would flag.  This
// is the same pattern as hash.hh above — a shared Layer-0 util, NOT an
// evaluator coupling, so it is not "include laundering".
#include "nix/util/eval-trace.hh"

// Forward declarations for types where opaque-by-pointer is sufficient.
namespace nix {
    struct Pos;
    struct StaticEnv;
    struct ContentAddressMethod;   // store/content-address.hh (by-ref shim param)
    class  Store;
    class  Logger;
    struct Value;       // TW value (distinct from nix::v3::Value)
    class  EvalState;   // host evaluator — opacity Level 1 (methods only)
    class  SymbolTable;
    class  PosTable;
}

namespace nix::v3 {

struct Value; // v3 16-byte tagged value (declared in value.hh)

// =========================================================================
// EvalState shims (Option A — typed link-time; audit §3.3/§3.4)
// =========================================================================
//
// Thin non-template wrappers over the `nix::EvalState` methods v3's FFI
// leaves call, so a consumer can forward-declare `nix::EvalState` /
// `nix::Value` and route through these instead of pulling the heavy
// `nix/expr/eval.hh` (+ its inline bodies).  Implemented in ffi.cc (the
// one TU that legitimately includes eval.hh).
//
// PERF (audit R1/R4): these are out-of-line, so the inline-`forceValue`
// fast path is lost — only adopt them on COLD/bridge paths until the
// `ffi-inline.h` private-header trick lands for the hot sites.
namespace ffi {

/// Force a TW value to WHNF via the host evaluator (at noPos).  Cold/
/// bridge-path convenience over `state.forceValue(v, nix::noPos)`.
void forceValue(nix::EvalState & state, nix::Value & v);

/// As above, threading an explicit position for error attribution
/// (`state.forceValue(v, pos)`).  Cold (FFI-leaf primop arg forcing).
void forceValue(nix::EvalState & state, nix::Value & v, nix::PosIdx pos);

/// Overwrite the host `builtins.<name>` slot with `*value` (the
/// bytecode-primop installer's path-1: make TW dispatch see v3's wrapper).
/// Throws (like `state.getBuiltin`) when `name` isn't a registered
/// builtin.  Startup-only / cold.
void setTreeWalkerBuiltin(nix::EvalState & state, const std::string & name, nix::Value * value);

/// Field accessors for the host's symbol + position tables (opacity
/// Level 1 — v3 reaches these via accessors, never the raw fields).  Used
/// by the parse→lower→run entry to feed lowerV3Ast / addOrigin.  Cold
/// (once per eval), so out-of-line is perf-fine.
const nix::SymbolTable & symbols(nix::EvalState & state);
nix::PosTable &          positions(nix::EvalState & state);

// -------------------------------------------------------------------------
// TW value-graph probe + bridge round-trip primitives (audit Phase 2/3).
//
// These wrap the `nix::EvalState` / `nix::Value` operations vm.cc performs
// ONLY on the FFI-leaf bridge paths (calling a tree-walker-bridged closure
// from v3) and on store/path coercion.  The v3-native hot dispatch path
// (OP_FORCE, GET_LOCAL, native OP_CALL) touches NONE of these — verified by
// the eval.hh-removal probe (2026-06-02): all 9 nix::EvalState uses in
// vm.cc are cold store-coercion or TW-bridge sites.  Each shim wraps an
// already-expensive TW operation, so the out-of-line call is noise.
// -------------------------------------------------------------------------

/// v3-owned mirror of `nix::ValueType` so a consumer can branch on a TW
/// value's type without naming `nix::ValueType` / the `nix::n*` enumerators
/// (which live in value.hh).  `Other` covers invalid / external / unknown.
enum class TwType { Null, Bool, Int, Float, String, Path, List, Attrs, Function, Thunk, External, Other };

/// Type of a (possibly-unforced) TW value, via `Value::type<true>()` so an
/// invalid/blackholed cell maps to `Thunk` rather than asserting.  Cold —
/// only the v3↔TW bridge inspects TW value types.
TwType valueType(const nix::Value * v);

/// `state.allocValue()` — a heap TW value cell (the bridge needs a stable
/// heap address TW updates in place; see vm.cc #484).  Cold/bridge only.
nix::Value * allocValue(nix::EvalState & state);

/// `state.callFunction(fun, arg, out, nix::noPos)` — invoke a TW function
/// value.  Cold/bridge only (a v3 closure call never routes here).
void callFunction(nix::EvalState & state, nix::Value & fun, nix::Value & arg, nix::Value & out);

/// As above with an explicit position (`state.callFunction(fun, arg, out,
/// pos)`).  Cold (FFI-leaf primop bridging to a TW function).
void callFunction(nix::EvalState & state, nix::Value & fun, nix::Value & arg, nix::Value & out, nix::PosIdx pos);

/// Coerce a filesystem path into the store and return its printed store
/// path: `printStorePath(copyPathToStore(SourcePath(rootFS, CanonPath(p))))`.
/// Keeps NixStringContext / SourcePath / CanonPath inside ffi.cc.  Cold —
/// string-interpolation of a path literal (FFI leaf: store).
std::string coercePathToStore(nix::EvalState & state, const std::string & path);

/// Same store copy, but return the StorePath's `to_string()` basename
/// (`<hash>-<name>`) — the encoded form an Opaque string-context entry
/// uses (NixStringContextElem round-trips it).  Used by OP_STR_CONCAT to
/// record the context for an interpolated path literal.  Cold (FFI leaf:
/// store).
std::string coercePathToStoreName(nix::EvalState & state, const std::string & path);

/// Render a single raw string-context entry for an error message:
/// `NixStringContextElem::parse(raw).display(*store)`.  Falls back to `raw`
/// on a parse failure.  Cold — only the "not allowed to refer to a store
/// path" diagnostic uses it.
std::string displayContextElem(nix::EvalState & state, const std::string & raw);

/// Realise a path string with its string-context (store-path / drv refs →
/// builds the IFD derivations the context names) to its resolved absolute
/// store path.  Plain-data in: the path + its context-elem strings (a TW
/// string is built from them INTERNALLY and realised — this is NOT a
/// v3-graph bridge / no bridge-table entry).  THE realisePath FFI leaf
/// (builtins.import / builtins.readDir).
std::string realisePath(nix::EvalState & state, const std::string & path,
                        const std::vector<std::string> & contextElems);

/// builtins.readFile store-ref context attribution: if `path` is in the
/// store, return the Opaque string-context entries for the declared
/// references (`queryPathInfo`) whose hash physically appears in `content`
/// (filtered via PathRefScanSink) — byte-for-byte matching TW.  Returns
/// empty when `path` isn't in the store or has no matching refs.  Keeps
/// PathRefScanSink / queryPathInfo inside ffi.cc.  Cold.
std::vector<std::string> storeRefsContextFor(nix::EvalState & state,
                                             const std::string & path,
                                             const std::string & content);

// -------------------------------------------------------------------------
// Flake / fetcher marshalling (audit Phase 4 — Category D, the flake-
// loading FFI leaf).
//
// `v3_call_flake.cc` builds the `getFlake` result V3-NATIVE (v3 Bindings /
// strings, run on the v3 VM), but the INPUTS it reads — the locked-flake's
// per-node `fetchers::Input` + store paths + lockfile text — are
// libflake / libfetchers / libstore types.  These plain-data structs +
// `readLockedFlake()` keep ALL of that type access inside ffi.cc, so
// v3_call_flake.cc names no `nix/...` flake/fetcher type and the v3 LIBRARY
// stops pulling libflake/libfetchers headers.  The extracted fields mirror
// the pre-extraction inline reads (callFlakeV3 + v3EmitTreeAttrs) field-
// for-field — the sourceInfo attrs feed downstream drvPaths, so byte parity
// is mandatory.
// -------------------------------------------------------------------------

/// Per-node `sourceInfo` source data, pre-read from a `fetchers::Input` +
/// `StorePath` (emitTreeAttrs's inputs).  `std::nullopt` ⇒ the attr is
/// absent (v3 side omits it).  `emptyRevFallback` is fixed false in the
/// flake path, so the dirty-fetchGit fallback is not represented here.
struct TreeAttrsInfo
{
    std::string                printedStorePath;   ///< store->printStorePath(sp)
    std::string                opaqueContextElem;  ///< Opaque{path=sp}.to_string()
    std::optional<std::string> narHash;            ///< getNarHash()→SRI(+algo)
    bool                       isGit = false;      ///< getType()=="git" (gates `submodules`)
    bool                       submodules = false; ///< maybeGetBoolAttr(attrs,"submodules")
    std::optional<std::string> rev;                ///< getRev()→gitRev()
    std::optional<std::string> shortRev;           ///< getRev()→gitShortRev()
    std::optional<int64_t>     revCount;           ///< getRevCount()
    std::optional<std::string> dirtyRev;           ///< attrs["dirtyRev"]
    std::optional<std::string> dirtyShortRev;      ///< attrs["dirtyShortRev"]
    std::optional<int64_t>     lastModified;       ///< getLastModified()
};

/// One locked-flake node, ready for callFlakeV3's `overrides` attrset.
struct FlakeNodeInfo
{
    std::string   key;          ///< lockfile keyMap entry for this node
    std::string   dir;          ///< CanonPath(subdir).rel()
    TreeAttrsInfo sourceInfo;
};

/// Everything callFlakeV3 reads out of a `nix::flake::LockedFlake`.
struct LockedFlakeInfo
{
    std::string                lockFileStr;   ///< lockFile.to_string().first
    std::vector<FlakeNodeInfo> nodes;         ///< nodePaths order (v3 side re-sorts)
};

/// One fetcher input attribute, extracted v3-native (string/int/bool), to
/// be rebuilt into `fetchers::Attrs` inside ffi.cc (audit Phase 4 / TW-value
/// eradication F1: keeps fetchers types out of the consumer TU).
struct FetchAttr
{
    std::string                                 name;
    std::variant<std::string, int64_t, bool>    value;
};

/// A fetcher invocation, plain-data: either a URL (→ `Input::fromURL`, the
/// fetchTree string form) or an attrs list (→ `Input::fromAttrs`).  The v3
/// caller does the arg normalization (type/url-fix/defaults) per TW's
/// fetchTree helper; ffi.cc does the fetch.
struct FetchTreeInput
{
    std::optional<std::string> url;    ///< set ⇒ fromURL; else fromAttrs(attrs)
    std::vector<FetchAttr>     attrs;
    std::string                fetcherName;  ///< "fetchGit"/"fetchTree" (error text)
};

/// THE fetcher FFI leaf (eradicates the `v3ToTreeWalker → callFunction(
/// builtins.fetchTree) → treeWalkerToV3` round-trip).  Mirrors TW's
/// `fetchTree` helper tail (fetchTree.cc:196-227): registry lookup +
/// pure-eval unlocked gating + checkURI + `__final` + inputCache getAccessor
/// + mountInput, then `readTreeAttrs` → plain `TreeAttrsInfo` for v3-native
/// `v3EmitTreeAttrs`.  No `nix::Value` crosses.  `emptyRevFallback` matches
/// the per-builtin flag (true for fetchGit).
TreeAttrsInfo fetchTree(nix::EvalState & state, const FetchTreeInput & in,
                        bool emptyRevFallback, bool isFinal);

/// `nix::fixGitURL(url).to_string()` — the fetchGit URL normalization (keeps
/// `nix/util/url.hh` in ffi.cc).  Cold (once per fetchGit call).
std::string fixGitURL(const std::string & url);

/// Result of a fetchurl/fetchTarball fetch: the printed store path + its
/// Opaque string-context entry (so v3 builds the result string V3-NATIVE,
/// matching TW's `mkStorePathString` = printStorePath + Opaque-context).
struct FetchUrlResult
{
    std::string printedStorePath;
    std::string opaqueContextElem;
};

/// THE fetchurl/fetchTarball FFI leaf (eradicates the bridgeBuiltin round-
/// trip).  Mirrors TW's `fetch` helper (fetchTree.cc:416-507): pseudo-url
/// resolve (tarball) + checkURI + name default + checkName + pure-eval
/// sha256 requirement + makeFixedOutputPath substitute early-exit +
/// downloadFile / downloadTarball+fetchToStore + hash-mismatch check +
/// allowPath.  `name` may be empty ("" → baseNameOf(url)); `unpack` selects
/// tarball.  No `nix::Value` crosses.
FetchUrlResult fetchUrl(nix::EvalState & state, const std::string & url,
                        const std::optional<std::string> & sha256,
                        std::string name, bool unpack, const std::string & who);

/// Result of builtins.fetchMercurial (its OWN attrset shape — distinct from
/// emitTreeAttrs: branch + 12-char shortRev, no narHash/lastModified).
struct FetchMercurialResult
{
    std::string                outPath;            ///< printed store path
    std::string                opaqueContextElem;  ///< Opaque{outPath}
    std::optional<std::string> branch;             ///< input.getRef()
    std::string                rev;                ///< gitRev (empty-sha1 if dirty)
    std::optional<int64_t>     revCount;
};

/// THE fetchMercurial FFI leaf (eradicates its bridgeBuiltin round-trip).
/// Mirrors prim_fetchMercurial (fetchMercurial.cc): build the hg input
/// (type=hg, url with file:// prefix, name, ref/rev — `revOrRef` is regex-
/// classified here into rev-vs-ref), `Input::fromAttrs` + `fetchToStore`,
/// then read outPath/branch/rev/revCount + allowPath.  No `nix::Value`.
FetchMercurialResult fetchMercurial(nix::EvalState & state, const std::string & url,
                                    const std::optional<std::string> & revOrRef,
                                    const std::string & name);

/// THE fetchClosure FFI leaf.  Mirrors prim_fetchClosure (fetchClosure.cc):
/// validate the fromStore URL (http(s), or file:// under _NIX_IN_TEST),
/// openStore, then dispatch — `toPath` present → rewrite via
/// makeContentAddressed; `inputAddressed` → copyClosure + require input-
/// addressed; else → copyClosure + require content-addressed.  allowClosure
/// + return the printed result path + Opaque context.  `toPath` nullopt =
/// absent; "" = gap ("report the CA path"); else a store-path string.
FetchUrlResult fetchClosure(nix::EvalState & state, const std::string & fromStoreUrl,
    const std::string & fromPathStr, const std::optional<std::string> & toPath,
    bool inputAddressed);

/// THE filtered-path-copy FFI leaf (builtins.filterSource — F3: VM re-entry).
/// Mirrors TW addPath's no-refs path (primops.cc:2961): builds a libstore
/// PathFilter that, per entry, lstats it for the file-type string and calls
/// back into v3 via `v3filter(absPath, type) -> keep?`, then
/// `fetchToStore(resolveSymlinks, Copy, baseName, NixArchive, filter)` +
/// allowPath.  `name` defaults to the source path's baseName.  Returns the
/// printed store path + Opaque context.  No `nix::Value` crosses (the filter
/// is a plain C++ callback the v3 caller wires to callClosure).
FetchUrlResult addPathFiltered(nix::EvalState & state, const std::string & srcPath,
    const std::function<bool(const std::string & absPath, const std::string & type)> & v3filter);

/// Read a `nix::flake::LockedFlake` (passed opaquely as `const void *`;
/// ffi.cc casts it back) into plain data: the lockfile text + per-node
/// store-path / fetcher-input fields.  Performs `lockFile.to_string`, the
/// per-node `store->toStorePath` + `dynamic_pointer_cast<LockedNode>` +
/// `emitTreeAttrs`-equivalent field reads (emptyRevFallback = false,
/// forceDirty per node).  THE flake-loading FFI leaf.
LockedFlakeInfo readLockedFlake(nix::EvalState & state, const void * lockedFlakePtr);

/// builtins.path's store fetch: `fetchToStore(path.resolveSymlinks(), name,
/// method)` under DryRun (when `readOnly`) else Copy, with no path filter
/// (the v3 caller bails to the bridge when a filter is present).  Returns
/// the resulting StorePath.  Keeps `fetchToStore` + `FetchMode` in ffi.cc.
/// Cold (FFI leaf: store).
nix::StorePath pathFetchToStore(nix::EvalState & state,
                                const nix::SourcePath & path,
                                const std::string & name,
                                const nix::ContentAddressMethod & method,
                                bool readOnly);

/// builtins.toFile's store add: under `readOnly` compute the path via
/// `makeFixedOutputPathFromCA(TextInfo{sha256(contents), refs})`, else
/// `addToStoreFromDump(StringSource, Flat, Raw::Text, sha256)`; then
/// allowPath + return the printed store path + its Opaque context (the v3
/// caller builds the result string V3-NATIVE — no bridge).  Keeps
/// StringSource / FileSerialisationMethod / TextInfo in ffi.cc.  `refs` is
/// taken by value (moved into TextInfo).  Cold.
FetchUrlResult addTextToStore(nix::EvalState & state, const std::string & name,
                              const std::string & contents,
                              nix::StorePathSet refs, bool readOnly);

/// builtins.getFlake's full flake-loading FFI leaf in one call: parseFlakeRef
/// + the unlocked-in-pure-eval guard + lockFlake (no lockfile write/update,
/// registries gated on !pureEval) + readLockedFlake.  Uses the process flake
/// Settings wired via setFlakeSettings (throws if unset).  Keeps FlakeRef /
/// LockFlags / lockFlake / flake::Settings entirely inside ffi.cc.
LockedFlakeInfo lockFlakeAndRead(nix::EvalState & state,
                                 const std::string & flakeRefStr,
                                 bool pureEval);

/// `nix::getHome().string()` — the synthetic homePath for parsing the
/// in-memory call-flake.nix source.  Cold (once, lazily).
std::string homeDir();

// -------------------------------------------------------------------------
// Settings reads (audit Phase 2 — Category I).  v3 reads a few host
// settings at FFI-leaf primops; these shims keep `nix::settings` /
// `EvalSettings` out of the consumer TU.
// -------------------------------------------------------------------------

/// `nix::settings.readOnlyMode` — when set, store paths are COMPUTED via
/// the derivation-hash protocol rather than written (derivationStrict /
/// builtins.path / fetchToStore branch on it).  Cold.
bool readOnlyMode();

/// `state.settings.pureEval` — the pure-eval gate (getFlake locking).
bool pureEval(nix::EvalState & state);

/// `nix::nixVersion` — the host Nix version string (builtins.nixVersion;
/// nixpkgs compares it against a minimum).  Cold.
std::string nixVersion();

}  // namespace ffi

// =========================================================================
// Framework: lifetime + error + latency-class wrappers
// =========================================================================

/// **Category H — EvalError** (FFI_PLAN_2026-05-06b §A5).
///
/// V8-style: separate the unformatted message + structural trace from
/// rendering.  v3 supplies positions and a HintFmt-compatible message;
/// the host renders.  Width / colour / terminal-vs-JSON is the
/// renderer's concern, not v3's.
///
/// **#489 status (2026-05-07):** type-level shape complete -- matches
/// `nix::EvalError`'s Suggestions + trace-with-positions structure.
/// The FFI boundary shim that catches a `nix::EvalError` C++ exception
/// and constructs a v3::EvalError variant is part of the per-category
/// migration steps (each Fallible<T>-returning FFI entry-point will
/// carry the catch-and-translate logic at its body).
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

/// Allocate a ClosureHandle bound to the current EvalScope.  The handle
/// becomes invalid when `scope`'s destructor runs (and all later inner
/// scopes get torn down too).  `payload` is an opaque pointer the host
/// owns -- v3 internals stash a Closure or arbitrary tagged value there.
///
/// Returns a handle whose opaque bits encode (scopeGeneration, slotIdx)
/// so a stale handle survives only as far as its scope is alive.
ClosureHandle allocClosureHandle(EvalScope & scope, void * payload);

/// True iff `h`'s scope is still alive AND the slot generation matches.
/// O(1).  Safe to call with arbitrary uint64_t opaques (returns false
/// for any value that doesn't decode to a known scope/slot pair).
bool isValid(ClosureHandle h);

/// Resolve a ClosureHandle to its payload.  Returns nullptr if the
/// handle has been invalidated (scope dtor ran) or never existed.
/// Read-only -- the payload pointer is owned by whoever allocated it.
void * lookupClosureHandle(ClosureHandle h);

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
//
// **#490 status (2026-05-07):** type-level shape complete -- all 13
// store-op declarations + PathFilter type are present.  Wiring v3
// internals (today libnixstore is reached directly via the global
// `getStore()`) onto these typed entry points is a separate
// migration step; tracked under the broader category-E body work in
// the FFI plan migration path.

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
// **#488 status (2026-05-07):** all 10 A3 fields declared + nested
// OutputChecks + structuredAttrsJSON; added `contentAddressed` and
// `impure` flags consulted by derivationStrictInternal
// (primops.cc:1709-1720) which the original A3 enumeration omitted.
//
// **Next step:** add a builder function `DerivationDescriptor
// fromAttrs(const v3::Bindings &)` that walks an attrset and populates
// this struct one place, replacing the hand-rolled checks scattered
// across primops.cc's derivationStrictInternal.  Out of scope for the
// type-level task; tracked separately.

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

    // CA / impure flags (#488 follow-on -- not strictly part of the
    // A3 enumeration but consulted by derivationStrictInternal at
    // primops.cc:1709-1720; without them the descriptor is missing
    // information the actual primop branches on).
    bool                       contentAddressed = false; ///< __contentAddressed (CaDerivations xp-feat)
    bool                       impure           = false; ///< __impure (ImpureDerivations xp-feat)

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
// `lower.cc`).
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
