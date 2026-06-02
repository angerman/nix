/// @file
/// FFI plan migration step 2: skeletal Evaluator + EvalScope impl.
///
/// Provides minimal definitions for the framework types declared in
/// `include/v3/ffi.hh` so they can be linked against.  Full migration
/// (handle storage, scope-bound handle invalidation, GlobalClosureHandle
/// promotion semantics) lands incrementally per the plan.
///
/// **Status:**
///   - `Evaluator` is a thin shell -- holds a per-instance scope-list
///     pointer.  Future work will lift the existing v3 globals
///     (v3HookCache, v3SubExprCache, v3BridgeClosures, ...) into
///     Evaluator member fields so multiple Evaluators can coexist
///     (e.g., for sandboxed plugin evaluation).
///   - `EvalScope` chains scopes per Evaluator.  Handle issuance and
///     invalidation will land alongside the migration of
///     `v3FormalsLambdaBridges` (today's sentinel-Env side-table) into
///     the EvalScope handle accounting.
///   - `promoteToGlobal` / `releaseGlobal` are skeletal -- the stable
///     v3BridgeClosures table already provides the storage; the wrapper
///     just makes the lifetime contract explicit at the API boundary.
///
/// Per FFI_PLAN_2026-05-06b §A1, A7, A10 (highest-priority migration
/// step 2).
///
/// Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.
/// SPDX-License-Identifier: Apache-2.0

#include "v3/ffi.hh"

#include "v3/value.hh"
#include "v3/vm.hh"
#include "v3/primop.hh"

#include "nix/util/source-path.hh"
#include "nix/util/source-accessor.hh"
#include "nix/util/error.hh"
#include "nix/util/canon-path.hh"          // CanonPath (coercePathToStore)
#include "nix/util/users.hh"               // getHome (homeDir)
#include "nix/util/hash.hh"                // HashFormat / Hash (narHash SRI)
#include "nix/expr/eval.hh"   // EvalState — ffi.cc is the one TU that wraps it
#include "nix/expr/value/context.hh"       // NixStringContext(Elem) (path/ctx shims)
#include "nix/store/store-api.hh"          // Store::printStorePath / toStorePath
#include "nix/store/globals.hh"            // nix::settings.readOnlyMode
#include "nix/store/path-references.hh"    // PathRefScanSink (storeRefsContextFor)
#include "nix/store/content-address.hh"    // ContentAddressMethod / TextInfo
#include "nix/fetchers/fetch-to-store.hh"  // fetchToStore / FetchMode (pathFetchToStore)
#include "nix/util/serialise.hh"           // StringSource / FileSerialisationMethod (addTextToStore)
#include "nix/fetchers/fetchers.hh"        // fetchers::Input getters (readLockedFlake)
#include "nix/fetchers/registry.hh"        // lookupInRegistries (fetchTree)
#include "nix/fetchers/input-cache.hh"     // InputCache::getAccessor (fetchTree)
#include "nix/util/logging.hh"             // warn (fetchTree pure-eval narHash path)
#include "nix/util/url.hh"                 // fixGitURL
#include "nix/util/url-parts.hh"           // revRegex (fetchMercurial rev/ref classify)
#include "nix/util/file-system.hh"         // baseNameOf (fetchUrl name default)
#include "nix/fetchers/tarball.hh"         // downloadFile / downloadTarball (fetchUrl)
#include "nix/fetchers/attrs.hh"           // maybeGetStrAttr / maybeGetBoolAttr
#include "nix/flake/flake.hh"              // flake::LockedFlake / lockFlake / LockFlags
#include "nix/flake/flakeref.hh"           // parseFlakeRef (lockFlakeAndRead)
#include "nix/flake/settings.hh"           // flake::Settings::useRegistries
#include "nix/flake/lockfile.hh"           // flake::LockedNode

#include <atomic>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>
#include <regex>

namespace nix::v3 {

// Defined in primops.cc — the v3↔TW value bridge.  ffi shims that build a
// TW value and hand it back to v3 (addTextToStore) call it here; forward-
// declared rather than headered (it's part of the not-yet-relocated bridge
// subsystem) — same pattern v3_call_flake.cc used pre-decoupling.
Value treeWalkerToV3Public(nix::EvalState & nixState, nix::Value & nv);

// EvalState shims (audit §3.4) — out-of-line wrappers; see ffi.hh.
namespace ffi {

void forceValue(nix::EvalState & state, nix::Value & v)
{
    state.forceValue(v, nix::noPos);
}

void forceValue(nix::EvalState & state, nix::Value & v, nix::PosIdx pos)
{
    state.forceValue(v, pos);
}

void setTreeWalkerBuiltin(nix::EvalState & state, const std::string & name, nix::Value * value)
{
    state.getBuiltin(name) = *value;  // throws if `name` isn't a builtin
}

const nix::SymbolTable & symbols(nix::EvalState & state) { return state.symbols; }
nix::PosTable &          positions(nix::EvalState & state) { return state.positions; }

// --- TW value-graph probe + bridge round-trip (audit Phase 2/3) ---------

TwType valueType(const nix::Value * v)
{
    // type<true>(): an invalid/blackholed cell maps to nThunk instead of
    // asserting — matches the `type<true>()` call sites we replaced in vm.cc.
    switch (v->type<true>()) {
        case nix::nNull:     return TwType::Null;
        case nix::nBool:     return TwType::Bool;
        case nix::nInt:      return TwType::Int;
        case nix::nFloat:    return TwType::Float;
        case nix::nString:   return TwType::String;
        case nix::nPath:     return TwType::Path;
        case nix::nList:     return TwType::List;
        case nix::nAttrs:    return TwType::Attrs;
        case nix::nFunction: return TwType::Function;
        case nix::nThunk:    return TwType::Thunk;
        case nix::nExternal: return TwType::External;
        case nix::nFailed:   return TwType::Other;  // evaluation-failed sentinel
    }
    return TwType::Other;  // unreachable; satisfies the non-void contract.
}

nix::Value * allocValue(nix::EvalState & state)
{
    return state.allocValue();
}

void callFunction(nix::EvalState & state, nix::Value & fun, nix::Value & arg, nix::Value & out)
{
    state.callFunction(fun, arg, out, nix::noPos);
}

void callFunction(nix::EvalState & state, nix::Value & fun, nix::Value & arg, nix::Value & out, nix::PosIdx pos)
{
    state.callFunction(fun, arg, out, pos);
}

std::string coercePathToStore(nix::EvalState & state, const std::string & path)
{
    // The local context is filled by copyPathToStore but discarded here;
    // the v3 caller re-records the Opaque entry keyed on the store path
    // (see vm.cc OP_STR_CONCAT).  Let exceptions propagate — TW raises on
    // a missing path during interpolation and v3 must match.
    nix::NixStringContext ctx;
    nix::SourcePath sp(state.rootFS, nix::CanonPath(path));
    auto storePath = state.copyPathToStore(ctx, sp);
    return state.store->printStorePath(storePath);
}

std::string coercePathToStoreName(nix::EvalState & state, const std::string & path)
{
    nix::NixStringContext ctx;
    nix::SourcePath sp(state.rootFS, nix::CanonPath(path));
    auto storePath = state.copyPathToStore(ctx, sp);
    return std::string(storePath.to_string());
}

std::string displayContextElem(nix::EvalState & state, const std::string & raw)
{
    try {
        auto elem = nix::NixStringContextElem::parse(raw);
        return elem.display(*state.store);
    } catch (...) {
        return raw;  // keep the raw form on a parse failure.
    }
}

std::vector<std::string> storeRefsContextFor(nix::EvalState & state,
                                             const std::string & path,
                                             const std::string & content)
{
    std::vector<std::string> ctx;
    try {
        if (state.store->isInStore(path)) {
            nix::StorePathSet refs;
            try {
                auto [storePath, _sub] = state.store->toStorePath(path);
                refs = state.store->queryPathInfo(storePath)->references;
            } catch (const nix::Error &) { /* unknown path; no refs */ }
            if (!refs.empty()) {
                auto refsSink = nix::PathRefScanSink::fromPaths(refs);
                refsSink << content;
                refs = refsSink.getResultPaths();
            }
            ctx.reserve(refs.size());
            for (auto & p : refs) {
                nix::NixStringContextElem elem = nix::NixStringContextElem::Opaque{ .path = p };
                ctx.push_back(elem.to_string());
            }
        }
    } catch (...) { /* best-effort context attribution */ }
    return ctx;
}

// --- Flake / fetcher marshalling (audit Phase 4) ------------------------

std::string homeDir()
{
    return nix::getHome().string();
}

bool readOnlyMode()
{
    return nix::settings.readOnlyMode;
}

bool pureEval(nix::EvalState & state)
{
    return state.settings.pureEval;
}

std::string nixVersion()
{
    return nix::nixVersion;
}

namespace {

/// Read one `fetchers::Input` + its `StorePath` into a plain TreeAttrsInfo.
/// This is the EXACT field-for-field set of reads the pre-extraction
/// `v3EmitTreeAttrs` performed (callFlakeV3 always passed
/// emptyRevFallback=false, so that fallback branch is intentionally
/// dropped — it never executed in the flake path).
TreeAttrsInfo readTreeAttrs(nix::EvalState & state,
                            const nix::StorePath & storePath,
                            const nix::fetchers::Input & input,
                            bool forceDirty,
                            bool emptyRevFallback)
{
    TreeAttrsInfo info;

    info.printedStorePath = state.store->printStorePath(storePath);
    nix::NixStringContextElem elem = nix::NixStringContextElem::Opaque{ .path = storePath };
    info.opaqueContextElem = elem.to_string();

    if (auto narHash = input.getNarHash())
        info.narHash = narHash->to_string(nix::HashFormat::SRI, /*includeAlgo=*/true);

    info.isGit = input.getType() == "git";
    if (info.isGit)
        info.submodules = nix::fetchers::maybeGetBoolAttr(input.attrs, "submodules").value_or(false);

    if (!forceDirty) {
        if (auto rev = input.getRev()) {
            info.rev      = rev->gitRev();
            info.shortRev = rev->gitShortRev();
        } else if (emptyRevFallback) {
            // Backwards-compat for builtins.fetchGit on a dirty repo: TW's
            // emitTreeAttrs emits an empty sha1 as rev (fetchTree.cc).
            const auto emptyHash = nix::Hash(nix::HashAlgorithm::SHA1);
            info.rev      = emptyHash.gitRev();
            info.shortRev = emptyHash.gitShortRev();
        }
        if (auto revCount = input.getRevCount())
            info.revCount = static_cast<int64_t>(*revCount);
        else if (emptyRevFallback)
            info.revCount = 0;
    }

    if (auto dirtyRev = nix::fetchers::maybeGetStrAttr(input.attrs, "dirtyRev")) {
        info.dirtyRev = *dirtyRev;
        if (auto dirtyShortRev = nix::fetchers::maybeGetStrAttr(input.attrs, "dirtyShortRev"))
            info.dirtyShortRev = *dirtyShortRev;
    }

    if (auto lastModified = input.getLastModified())
        info.lastModified = static_cast<int64_t>(*lastModified);

    return info;
}

}  // namespace

std::string fixGitURL(const std::string & url)
{
    return nix::fixGitURL(url).to_string();
}

FetchMercurialResult fetchMercurial(nix::EvalState & state, const std::string & url,
                                    const std::optional<std::string> & revOrRef,
                                    const std::string & name)
{
    std::optional<nix::Hash> rev;
    std::optional<std::string> ref;
    if (revOrRef) {
        // Like prim_fetchMercurial: a 40-hex value is a rev, else a ref.
        if (std::regex_match(revOrRef->begin(), revOrRef->end(), nix::revRegex))
            rev = nix::Hash::parseAny(*revOrRef, nix::HashAlgorithm::SHA1);
        else
            ref = *revOrRef;
    }

    state.checkURI(url);
    if (state.settings.pureEval && !rev)
        throw nix::Error("in pure evaluation mode, 'fetchMercurial' requires a Mercurial revision");

    nix::fetchers::Attrs attrs;
    attrs.insert_or_assign("type", std::string("hg"));
    attrs.insert_or_assign("url", url.find("://") != std::string::npos ? url : "file://" + url);
    attrs.insert_or_assign("name", name);
    if (ref) attrs.insert_or_assign("ref", *ref);
    if (rev) attrs.insert_or_assign("rev", rev->gitRev());
    auto input = nix::fetchers::Input::fromAttrs(state.fetchSettings, std::move(attrs));

    auto [storePath, input2] = input.fetchToStore(state.fetchSettings, *state.store);
    state.allowPath(storePath);

    FetchMercurialResult r;
    r.outPath = state.store->printStorePath(storePath);
    r.opaqueContextElem =
        nix::NixStringContextElem{nix::NixStringContextElem::Opaque{.path = storePath}}.to_string();
    if (input2.getRef()) r.branch = *input2.getRef();
    // Backward-compat: dirty tree → 0000…0000 sha1 rev (matches TW).
    r.rev = input2.getRev().value_or(nix::Hash(nix::HashAlgorithm::SHA1)).gitRev();
    if (auto rc = input2.getRevCount()) r.revCount = static_cast<int64_t>(*rc);
    return r;
}

FetchUrlResult fetchUrl(nix::EvalState & state, const std::string & urlArg,
                        const std::optional<std::string> & sha256,
                        std::string name, bool unpack, const std::string & who)
{
    std::string url = urlArg;
    std::optional<nix::Hash> expectedHash;
    if (sha256)
        expectedHash = nix::newHashAllowEmpty(*sha256, nix::HashAlgorithm::SHA256);

    if (who == "fetchTarball")
        url = state.settings.resolvePseudoUrl(url);

    state.checkURI(url);

    if (name.empty())
        name = std::string(nix::baseNameOf(url));
    nix::checkName(name);  // throws BadStorePathName on an invalid store name

    if (state.settings.pureEval && !expectedHash)
        throw nix::Error("in pure evaluation mode, '%s' requires a 'sha256' argument", who);

    auto opaque = [&](const nix::StorePath & p) -> FetchUrlResult {
        return {state.store->printStorePath(p),
                nix::NixStringContextElem{nix::NixStringContextElem::Opaque{.path = p}}.to_string()};
    };

    // Early exit if pinned + already substitutable.
    if (expectedHash && expectedHash->algo == nix::HashAlgorithm::SHA256) {
        auto expectedPath = state.store->makeFixedOutputPath(
            name,
            nix::FixedOutputInfo{
                .method = unpack ? nix::FileIngestionMethod::NixArchive : nix::FileIngestionMethod::Flat,
                .hash = *expectedHash,
                .references = {}});
        try {
            state.store->ensurePath(expectedPath);
            state.allowPath(expectedPath);
            return opaque(expectedPath);
        } catch (nix::Error &) { /* fall through to download */ }
    }

    nix::StorePath storePath = unpack
        ? nix::fetchToStore(
              state.fetchSettings, *state.store,
              nix::fetchers::downloadTarball(*state.store, state.fetchSettings, url),
              nix::FetchMode::Copy, name)
        : nix::fetchers::downloadFile(*state.store, state.fetchSettings, url, name).storePath;

    if (expectedHash) {
        auto hash = unpack
            ? state.store->queryPathInfo(storePath)->narHash
            : nix::hashPath({state.store->requireStoreObjectAccessor(storePath)},
                            nix::FileSerialisationMethod::Flat, nix::HashAlgorithm::SHA256).hash;
        if (hash != *expectedHash)
            throw nix::Error(
                "hash mismatch in file downloaded from '%s':\n  specified: %s\n  got:       %s",
                url, expectedHash->to_string(nix::HashFormat::Nix32, true),
                hash.to_string(nix::HashFormat::Nix32, true));
    }

    state.allowPath(storePath);
    return opaque(storePath);
}

TreeAttrsInfo fetchTree(nix::EvalState & state, const FetchTreeInput & in,
                        bool emptyRevFallback, bool isFinal)
{
    // (a) Build the fetchers::Input from plain data (mirrors fetchTree.cc's
    //     fromURL / fromAttrs split; the v3 caller already did the per-
    //     builtin arg normalization).
    nix::fetchers::Input input{};
    if (in.url) {
        input = nix::fetchers::Input::fromURL(state.fetchSettings, *in.url);
    } else {
        nix::fetchers::Attrs attrs;
        for (auto & a : in.attrs) {
            if (std::holds_alternative<std::string>(a.value))
                attrs.emplace(a.name, std::get<std::string>(a.value));
            else if (std::holds_alternative<int64_t>(a.value))
                attrs.emplace(a.name, uint64_t(std::get<int64_t>(a.value)));
            else
                attrs.emplace(a.name, nix::Explicit<bool>{std::get<bool>(a.value)});
        }
        input = nix::fetchers::Input::fromAttrs(state.fetchSettings, std::move(attrs));
    }

    // (b) registry / pure-eval / checkURI / __final — verbatim from TW's
    //     fetchTree helper (fetchTree.cc:196-220).
    if (!state.settings.pureEval && !input.isDirect()
        && nix::experimentalFeatureSettings.isEnabled(nix::Xp::Flakes))
        input = nix::fetchers::lookupInRegistries(state.fetchSettings, *state.store, input,
                    nix::fetchers::UseRegistries::Limited).first;

    if (state.settings.pureEval && !input.isLocked(state.fetchSettings)) {
        if (input.getNarHash())
            nix::warn(
                "Input '%s' is unlocked (e.g. lacks a Git revision) but is checked by NAR hash. "
                "This is not reproducible and will break after garbage collection or when shared.",
                input.to_string());
        else
            throw nix::Error(
                "in pure evaluation mode, '%s' doesn't fetch unlocked input '%s'",
                in.fetcherName, input.to_string());
    }

    state.checkURI(input.toURLString());

    if (isFinal)
        input.attrs.insert_or_assign("__final", nix::Explicit<bool>(true));
    else if (input.isFinal())
        throw nix::Error("input '%s' is not allowed to use the '__final' attribute", input.to_string());

    // (c) the actual fetch + path mount, then read the result V3-NATIVE.
    auto cachedInput = state.inputCache->getAccessor(
        state.fetchSettings, *state.store, input, nix::fetchers::UseRegistries::No);
    auto storePath = state.mountInput(cachedInput.lockedInput, input, cachedInput.accessor);
    return readTreeAttrs(state, storePath, cachedInput.lockedInput,
                         /*forceDirty=*/false, emptyRevFallback);
}

LockedFlakeInfo readLockedFlake(nix::EvalState & state, const void * lockedFlakePtr)
{
    const auto & lockedFlake =
        *static_cast<const nix::flake::LockedFlake *>(lockedFlakePtr);

    LockedFlakeInfo out;
    auto [lockFileStr, keyMap] = lockedFlake.lockFile.to_string();
    out.lockFileStr = std::move(lockFileStr);

    out.nodes.reserve(lockedFlake.nodePaths.size());
    for (auto & [node, sourcePath] : lockedFlake.nodePaths) {
        auto lockedNode = node.dynamic_pointer_cast<const nix::flake::LockedNode>();
        auto [storePath, subdir] = state.store->toStorePath(sourcePath.path.abs());

        const auto & inputForNode =
            lockedNode ? lockedNode->lockedRef.input
                       : lockedFlake.flake.lockedRef.input;
        const bool forceDirty = !lockedNode && lockedFlake.flake.forceDirty;

        FlakeNodeInfo n;
        n.sourceInfo = readTreeAttrs(state, storePath, inputForNode, forceDirty,
                                     /*emptyRevFallback=*/false);
        n.dir = std::string(nix::CanonPath(subdir).rel());

        auto key = keyMap.find(node);
        if (key == keyMap.end())
            throw std::runtime_error(
                "v3::readLockedFlake: node missing from lockfile keyMap");
        n.key = key->second;

        out.nodes.push_back(std::move(n));
    }
    return out;
}

nix::StorePath pathFetchToStore(nix::EvalState & state,
                                const nix::SourcePath & path,
                                const std::string & name,
                                const nix::ContentAddressMethod & method,
                                bool readOnly)
{
    return nix::fetchToStore(
        state.fetchSettings,
        *state.store,
        path.resolveSymlinks(),
        readOnly ? nix::FetchMode::DryRun : nix::FetchMode::Copy,
        name,
        method,
        nullptr,
        state.repair);
}

Value addTextToStore(nix::EvalState & state, const std::string & name,
                     const std::string & contents,
                     nix::StorePathSet refs, bool readOnly)
{
    auto storePath = readOnly
        ? state.store->makeFixedOutputPathFromCA(
            name,
            nix::TextInfo{
                .hash = nix::hashString(nix::HashAlgorithm::SHA256, contents),
                .references = std::move(refs),
            })
        : ({
            nix::StringSource s{contents};
            state.store->addToStoreFromDump(
                s, name,
                nix::FileSerialisationMethod::Flat,
                nix::ContentAddressMethod::Raw::Text,
                nix::HashAlgorithm::SHA256, refs, state.repair);
        });
    nix::Value tw;
    state.allowAndSetStorePathString(storePath, tw);
    return nix::v3::treeWalkerToV3Public(state, tw);
}

LockedFlakeInfo lockFlakeAndRead(nix::EvalState & state,
                                 const std::string & flakeRefStr,
                                 bool pureEval)
{
    const nix::flake::Settings * flakeSettings = nix::v3::getFlakeSettings();
    if (!flakeSettings)
        throw std::runtime_error(
            "v3 builtins.getFlake: flake::Settings not wired — the v3 host "
            "must call nix::v3::setFlakeSettings() at startup (libcmd's "
            "common-eval-args.cc does this for the `nix` CLI; v3-eval too)");

    auto flakeRef = nix::parseFlakeRef(state.fetchSettings, flakeRefStr, {}, true);
    if (pureEval && !flakeRef.input.isLocked(state.fetchSettings))
        throw nix::Error(
            "cannot call 'getFlake' on unlocked flake reference '%s' (use --impure to override)",
            flakeRefStr);

    auto lockedFlake = nix::flake::lockFlake(
        *flakeSettings, state, flakeRef,
        nix::flake::LockFlags{
            .updateLockFile = false,
            .writeLockFile  = false,
            .useRegistries  = !pureEval && flakeSettings->useRegistries,
            .allowUnlocked  = !pureEval,
        });
    return readLockedFlake(state, &lockedFlake);
}

}  // namespace ffi

// ---------------------------------------------------------------------------
// Evaluator
// ---------------------------------------------------------------------------

/// Per-EvalScope handle storage.  Each entry holds an opaque payload
/// (whatever v3-internal value the host registers) and a `valid` flag
/// flipped to false when the enclosing scope is destroyed.  The valid
/// flag persists in the table after scope destruction (until the next
/// gen rollover) so isValid() correctly returns false for stale handles.
///
/// GC_AUDIT_ROUND_2 N6 (LATENT, documented 2026-05-21): `payload` is
/// cast to `Value *` by `applyClosure` (this file ~line 271).
/// Production FFI consumers (the embedding host API) would store v3
/// `Closure *` / `Bindings *` / `ListVec *` here, any of which can be
/// nursery-resident.  Today only test code paths use
/// `allocClosureHandle`, so the absence of a scavenger walk is not
/// active.  Before opening the FFI to production embedders, add
/// `walkEvalScopeRoots(visit)` that iterates every live `ScopeNode`
/// (via `g_topScope` chain) and calls `visit(*reinterpret_cast<Value *>(&slot.payload))`
/// for each valid slot — then call it from `gc.cc::Scavenger::run()`
/// and from `postScavengeAudit`.  See
/// `lode/GC_AUDIT_ROUND_2_2026-05-21.md` §2.4 N6.
struct HandleSlot
{
    void *   payload;
    bool     valid;
};

/// Per-Evaluator scope chain head.  Each EvalScope ctor pushes a new
/// node; dtor pops.  Threadlocal -- one logical Evaluator per thread
/// today; future work may need lock-free per-instance lists.
struct ScopeNode
{
    ScopeNode * prev;
    /// Generation token for handles allocated in this scope.  Encoded
    /// in the upper 32 bits of ClosureHandle::opaque so a handle whose
    /// scope has been destroyed (and whose generation has been removed
    /// from g_liveScopes) fails the lookup -- ABA defence works because
    /// each new scope gets a fresh generation from the global counter.
    uint32_t generation;
    /// Slot vector owned by this scope; entries are flipped to valid=false
    /// in the dtor before the table is freed.
    std::vector<HandleSlot> slots;
};

namespace {
thread_local ScopeNode * g_topScope = nullptr;

/// Scope generation counter.  Atomic so concurrent threads issue
/// distinct generations even if their EvalScopes never interact.
/// Starts at 1 (0 reserved for "uninitialised handle").
std::atomic<uint32_t> g_nextScopeGen{1};

/// Live-scope index: maps generation -> ScopeNode*.  Populated on ctor,
/// erased on dtor.  Lookup-by-generation drives O(1) handle resolution.
/// Lock guards both the map and per-slot access (writers + readers).
std::mutex                                  g_scopeLock;
std::unordered_map<uint32_t, ScopeNode *>   g_liveScopes;

constexpr uint32_t kInvalidGen = 0;

/// Pack/unpack helpers for the 64-bit handle opaque.
struct PackedHandle
{
    uint32_t generation;
    uint32_t slotIdx;
};
inline uint64_t packHandle(uint32_t gen, uint32_t slot) {
    return (uint64_t(gen) << 32) | uint64_t(slot);
}
inline PackedHandle unpackHandle(uint64_t opaque) {
    return PackedHandle{
        .generation = uint32_t(opaque >> 32),
        .slotIdx    = uint32_t(opaque & 0xFFFFFFFFu),
    };
}
}

class Evaluator
{
public:
    Evaluator() = default;
    Evaluator(const Evaluator &) = delete;
    Evaluator & operator=(const Evaluator &) = delete;
};

// ---------------------------------------------------------------------------
// EvalScope
// ---------------------------------------------------------------------------

EvalScope::EvalScope(Evaluator & e)
    : m_ev(e)
{
    uint32_t gen = g_nextScopeGen.fetch_add(1, std::memory_order_relaxed);
    // Avoid handing out gen=0 (reserved as kInvalidGen).  In practice
    // this only matters at the 4-billion-scope rollover; bias once.
    if (gen == kInvalidGen)
        gen = g_nextScopeGen.fetch_add(1, std::memory_order_relaxed);

    auto * node = new ScopeNode{
        .prev       = g_topScope,
        .generation = gen,
        .slots      = {},
    };
    {
        std::lock_guard<std::mutex> lk(g_scopeLock);
        g_liveScopes.emplace(gen, node);
    }
    g_topScope = node;
    m_state    = node;
}

EvalScope::~EvalScope()
{
    auto * node = static_cast<ScopeNode *>(m_state);
    if (!node) return;

    // Invalidate every handle issued by this scope.  Marking the slots
    // (rather than just dropping the table) means a stale ClosureHandle
    // copied out by the host still resolves cleanly to "invalid" via
    // lookupClosureHandle / isValid -- they re-check the slot's valid
    // bit on every call.  After the slot vector is freed, generation
    // removal from g_liveScopes makes future lookups short-circuit.
    {
        std::lock_guard<std::mutex> lk(g_scopeLock);
        for (auto & s : node->slots) s.valid = false;
        g_liveScopes.erase(node->generation);
    }

    if (node == g_topScope) {
        g_topScope = node->prev;
        delete node;
    } else {
        // Mismatch (scope dtor running out of stack order) is a programmer
        // error.  Leak the node so subsequent dtors find their state.
        // In a debug build, an assert would fire.
    }
}

ClosureHandle allocClosureHandle(EvalScope & /*scope*/, void * payload)
{
    // EvalScope is non-copyable + RAII, so the scope passed in MUST be
    // the topmost (otherwise the caller has a stack-order bug).  Read
    // g_topScope rather than poking at EvalScope::m_state -- the API
    // contract guarantees they match.
    ScopeNode * top = g_topScope;
    if (!top) return ClosureHandle{0};

    HandleSlot newSlot{payload, true};
    uint32_t slotIdx;
    {
        std::lock_guard<std::mutex> lk(g_scopeLock);
        slotIdx = static_cast<uint32_t>(top->slots.size());
        top->slots.push_back(newSlot);
    }
    return ClosureHandle{packHandle(top->generation, slotIdx)};
}

bool isValid(ClosureHandle h)
{
    auto p = unpackHandle(h.opaque);
    if (p.generation == kInvalidGen) return false;

    std::lock_guard<std::mutex> lk(g_scopeLock);
    auto it = g_liveScopes.find(p.generation);
    if (it == g_liveScopes.end()) return false;

    ScopeNode * node = it->second;
    if (p.slotIdx >= node->slots.size()) return false;
    return node->slots[p.slotIdx].valid;
}

void * lookupClosureHandle(ClosureHandle h)
{
    auto p = unpackHandle(h.opaque);
    if (p.generation == kInvalidGen) return nullptr;

    std::lock_guard<std::mutex> lk(g_scopeLock);
    auto it = g_liveScopes.find(p.generation);
    if (it == g_liveScopes.end()) return nullptr;

    ScopeNode * node = it->second;
    if (p.slotIdx >= node->slots.size()) return nullptr;
    auto & slot = node->slots[p.slotIdx];
    return slot.valid ? slot.payload : nullptr;
}

// ---------------------------------------------------------------------------
// Global handle promotion
// ---------------------------------------------------------------------------
//
// Today: GlobalClosureHandle wraps the same storage as ClosureHandle
// (the v3BridgeClosures table -- always-rooted via traceable_allocator).
// The lifetime distinction is contract-only; the table itself never
// shrinks (per existing MED-14 bounding work).  Once we migrate
// v3FormalsLambdaBridges to use ClosureHandle, releaseGlobal will
// actually free the slot.

GlobalClosureHandle promoteToGlobal(EvalScope & /*scope*/, ClosureHandle h)
{
    GlobalClosureHandle g;
    g.opaque = h.opaque;
    return g;
}

void releaseGlobal(GlobalClosureHandle /*h*/)
{
    // No-op for now -- the underlying v3BridgeClosures table is grow-
    // only.  When we migrate to a slot-recycling registry, this will
    // free the slot and any captured values.
}

// ---------------------------------------------------------------------------
// applyClosure (Sprint priority 1, FFI plan §G)
// ---------------------------------------------------------------------------
//
// Calls a v3 closure (registered via allocClosureHandle whose payload is a
// `Value *` pointing at the closure) with the given argument.  Returns
// `Fallible<Value>`: success carries the result; failure carries a
// v3::EvalError translated from a thrown C++ exception.
//
// **Threading / VMState:** allocates a fresh VMState per call.  This is
// the same pattern bridge1 (`primops.cc:3138`) already uses for TW→v3
// transitions.  Deeper integration (one VMState per EvalScope, fiber
// hand-off, BlockingFFI<T> wrapping for Port-class blocking calls)
// lands incrementally per the FFI plan.

Fallible<Value> applyClosure(EvalScope & scope, ClosureHandle h, Value arg)
{
    void * payload = lookupClosureHandle(h);
    if (!payload) {
        EvalError err;
        err.msg = "applyClosure: invalid handle (scope expired or never existed)";
        return Fallible<Value>{err};
    }
    (void)scope;  // EvalScope ownership is the validity check above.

    Value * funPtr = static_cast<Value *>(payload);

    // Allocate a fresh VMState.  Reserve sizes mirror bridge1's
    // (primops.cc:3138-3141): enough for a typical closure call without
    // re-allocation, but heap-bounded.
    VMState vm;
    vm.valueStack.reserve(64 * 1024);
    vm.frames.reserve(4096);
    vm.withStack.reserve(64);

    try {
        Value result = callClosure(vm, *funPtr, arg);
        return Fallible<Value>{result};
    } catch (const std::exception & e) {
        EvalError err;
        err.msg = e.what();
        // primaryPos / trace / suggestions left empty for now;
        // FFI plan §A5 boundary shim catches the structured nix::EvalError
        // (when v3 throws it) and copies position + trace.  See #489.
        return Fallible<Value>{err};
    }
}

// ---------------------------------------------------------------------------
// Category C: Filesystem I/O (Sprint priority 2, FFI plan §A12)
// ---------------------------------------------------------------------------
//
// Pure-filesystem reads -- no store daemon involvement.  Each function is a
// thin wrapper around the corresponding `nix::SourcePath` method, with
// exceptions translated to v3::EvalError at the boundary.  Sandbox /
// pure-eval gating is the dispatcher's concern (PrimOp flags, Cat. K).

namespace {

/// Map a SourceAccessor::Type enumerator to a stable lower-case string.
/// Matches the names builtins.readFileType uses ("regular", "directory",
/// "symlink", "unknown").  "unknown" subsumes char/block/socket/fifo so
/// the FFI surface is small; consumers that need the precise sub-kind
/// can call lstat directly.
const char * typeToString(nix::SourceAccessor::Type t)
{
    using T = nix::SourceAccessor::Type;
    switch (t) {
        case T::tRegular:   return "regular";
        case T::tDirectory: return "directory";
        case T::tSymlink:   return "symlink";
        case T::tChar:      return "unknown";
        case T::tBlock:     return "unknown";
        case T::tSocket:    return "unknown";
        case T::tFifo:      return "unknown";
        case T::tUnknown:   return "unknown";
    }
    return "unknown";
}

/// Wrap a thrown C++ exception as an EvalError at the FFI boundary.
/// Position / trace / suggestions stay empty until the §A5 structured
/// nix::EvalError catch lands (see #489).
EvalError exceptionToEvalError(const std::exception & e)
{
    EvalError err;
    err.msg = e.what();
    return err;
}

} // namespace

Fallible<std::string> readFile(nix::SourcePath path)
{
    try {
        return Fallible<std::string>{path.readFile()};
    } catch (const std::exception & e) {
        return Fallible<std::string>{exceptionToEvalError(e)};
    }
}

Fallible<std::map<std::string, std::string>> readDir(nix::SourcePath path)
{
    try {
        auto entries = path.readDirectory();
        std::map<std::string, std::string> out;
        for (auto & [name, optType] : entries) {
            // Unknown / lazy-resolution entries (some FS layers skip the
            // type lookup) are reported as "unknown" so consumers know
            // to call lstat for the precise kind.  Mirrors TW
            // primReadDir's behaviour at primops.cc:2549-2569.
            out.emplace(name,
                optType ? typeToString(*optType) : "unknown");
        }
        return Fallible<std::map<std::string, std::string>>{std::move(out)};
    } catch (const std::exception & e) {
        return Fallible<std::map<std::string, std::string>>{
            exceptionToEvalError(e)};
    }
}

bool pathExists(nix::SourcePath path)
{
    // No Fallible -- pathExists in nix:: itself returns bool and does not
    // throw on missing-path; only on permission / I/O errors which
    // surface as exceptions.  Match that contract: false on any throw.
    try {
        return path.pathExists();
    } catch (...) {
        return false;
    }
}

// findFile is a host-environment lookup (NIX_PATH search-path resolution)
// rather than a pure-filesystem op.  Implementing it requires either
// EvalState's searchPath or an injected lookup function.  Deferred to
// the EvaluatorSettings wiring (sprint priority 3); declared in ffi.hh
// for the eventual host migration.

} // namespace nix::v3
