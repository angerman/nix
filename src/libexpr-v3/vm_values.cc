/// @file
/// v3 VM value-manipulation helpers, extracted from vm.cc (step 4 of the vm.cc
/// split).  PURE MOVE: every definition here is byte-for-byte the one that used
/// to live in vm.cc; only the linkage of the cross-TU entry points changed
/// (anonymous-namespace `inline` -> external, declared in v3/vm_internal.hh) so
/// vm.cc's dispatch / error / apply paths can still reach them.
///
/// Moved here — the LARGER, cold-ish value helpers:
///   - valueRepr                       recursive TW-ValuePrinter error printer
///   - coerceToString                  `+` / `${…}` string coercion (String+Path)
///   - requireNoStringContextRuntime   dynamic-attr forceStringNoCtx mirror
///   - mergeBindings                   attrset `//` merge (the #1 Bindings producer)
///
/// Deliberately LEFT in vm.cc — the small, extremely-hot helpers whose out-of-
/// lining could regress the interpreter loop: isTrueValue (every OP_IF) and
/// valueLess (tight sort/compare loops).  --brute measures correctness only, so
/// a perf regression would pass unseen; conservatism wins.
///
/// Two shared symbols moved with the cluster (their main users are here, but
/// vm.cc still references them, so they gained external linkage via
/// v3/vm_internal.hh):
///   - kMaxIndirectionChase  now in v3/vm_internal.hh (a shared constexpr)
///   - g_sharedWbDetect + chainChildCount()  the WS-A chain-writeback detector,
///     defined here (mergeBindings is its chief producer); vm.cc's OP_* writeback
///     sites + the barrier reporter read them through the header decls.
/// The MergeBindingsSite enum went to the header (not here) because vm.cc's `//`
/// / extends / compose call sites still name MergeBindingsSite::… as the merge
/// site argument.
///
/// Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.
/// SPDX-License-Identifier: Apache-2.0

#include "v3/vm_internal.hh"      // moved-helper decls; MergeBindingsSite; brings v3/vm.hh
#include "v3/alloc.hh"            // Bindings/Alloc/allocStats/V3_STATS_BLOCK/lookupStringContextEntries
#include "v3/barrier.hh"          // bindingsPostConstructBarrier (Phase D batch barrier)
#include "v3/primop.hh"           // getNixEvalState
#include "v3/ffi.hh"              // ffi::coercePathToStore / ffi::displayContextElem
#include "v3/ir.hh"               // ir::globalSymbolTable (valueRepr attr names); SymbolId

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>

namespace nix::v3 {

// --- WS-A shared-writeback detector -----------------------------------------
// PLAN_BEAT_TW_V2 workstream A: gate + per-chain-parent child counter.  Moved
// from vm.cc (file-local statics) to external linkage — mergeBindings (below) is
// the chief producer; vm.cc's OP_* writeback sites + the barrier reporter read
// them via v3/vm_internal.hh.  Cost is nil when V3_DBG_SHARED_WB is unset.
const bool g_sharedWbDetect = std::getenv("V3_DBG_SHARED_WB") != nullptr;

// Child-count per chain parent (detector-only; populated solely when
// g_sharedWbDetect).  Function-local static so it costs nothing when the gate
// is off (never touched).  Pointers are stable (flat MS does not move cells);
// run the detector with a high NIX_V3_MAJOR_GC_THRESHOLD_MB to avoid a freed
// parent's address being recycled under a stale count.
std::unordered_map<const Bindings *, uint32_t> & chainChildCount() noexcept
{
    static std::unordered_map<const Bindings *, uint32_t> m;
    return m;
}

/// #691 — TW `ValuePrinter` mirror for error-message value rendering.
/// Matches TW's `errorPrintOptions` defaults: maxDepth=10, maxAttrs=10,
/// maxListItems=10, force=false (i.e., don't force lazy values — they
/// might be the source of the very error we're rendering).
///
/// This is the helper that closes the PREFIX-class gap in many v3
/// error messages where v3 currently emits truncated placeholders
/// like `{ ... }` / `[ ... ]` while TW shows the actual values.
/// Used by `coerceToString`, `valueLess`, and the formals-validation
/// error paths in OP_CALL / OP_TAIL_CALL.
///
/// IMPORTANT: this function does NOT force lazy values.  Tag::Thunk /
/// Tag::App / Tag::Slot render as `«…»` placeholders.  Forcing during
/// error rendering risks recursing into the very error we're trying
/// to report — TW's `force=false` discipline is correctness-load-
/// bearing here.
std::string valueRepr(const Value & v, int depth)
{
    constexpr int kMaxDepth = 10;
    constexpr int kMaxItems = 10;
    constexpr size_t kMaxStrLen = 1024;
    Tag t = v.tag();
    if (depth > kMaxDepth) return "«…»";
    if (t == Tag::Int) {
        char buf[24];
        std::snprintf(buf, sizeof buf, "%lld", (long long)v.asInt());
        return buf;
    }
    if (t == Tag::Float) {
        // Match TW's `output << double` — default ostream formatting
        // (not %f's fixed 6-decimal).
        std::ostringstream os; os << v.asFloat();
        return os.str();
    }
    if (t == Tag::Bool)   return v.asInt() == 1 ? "true" : "false";
    if (t == Tag::Null)   return "null";
    if (t == Tag::String) {
        std::string out = "\"";
        std::string_view sv = v.asString() ? std::string_view(v.asString())
                                            : std::string_view{};
        size_t n = sv.size();
        size_t lim = n > kMaxStrLen ? kMaxStrLen : n;
        for (size_t i = 0; i < lim; ++i) {
            char c = sv[i];
            switch (c) {
                case '"':  out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\n': out += "\\n"; break;
                case '\r': out += "\\r"; break;
                case '\t': out += "\\t"; break;
                case '$':
                    if (i + 1 < lim && sv[i + 1] == '{') {
                        out += "\\$";
                    } else {
                        out += c;
                    }
                    break;
                default: out += c;
            }
        }
        if (n > kMaxStrLen) out += "«…elided…»";
        out += "\"";
        return out;
    }
    if (t == Tag::Path)   return v.asPath() ? v.asPath() : "/";
    if (t == Tag::List) {
        if (!v.asList() || v.asList()->size == 0) return "[ ]";
        std::string out = "[ ";
        uint32_t n = v.asList()->size;
        uint32_t lim = n > kMaxItems ? kMaxItems : n;
        for (uint32_t i = 0; i < lim; ++i) {
            out += valueRepr(v.asList()->elems[i], depth + 1);
            out += ' ';
        }
        if (n > kMaxItems) {
            out += "«…";
            out += std::to_string(n - kMaxItems);
            out += " items elided…» ";
        }
        out += ']';
        return out;
    }
    if (t == Tag::Attrs) {
        if (!v.asAttrs() || v.asAttrs()->size == 0) return "{ }";
        std::string out = "{ ";
        auto * b = v.asAttrs();
        const auto & symTab = ir::globalSymbolTable();
        uint32_t n = b->size;
        uint32_t lim = n > kMaxItems ? kMaxItems : n;
        for (uint32_t i = 0; i < lim; ++i) {
            uint32_t nameIdx = b->entries[i].name;
            std::string nm = (nameIdx < symTab.size())
                ? symTab[nameIdx]
                : std::string("<sym?>");
            out += nm;
            out += " = ";
            out += valueRepr(b->entries[i].value, depth + 1);
            out += "; ";
        }
        if (n > kMaxItems) {
            out += "«…";
            out += std::to_string(n - kMaxItems);
            out += " attrs elided…» ";
        }
        out += '}';
        return out;
    }
    if (t == Tag::Closure)   return "«lambda»";
    if (t == Tag::PrimOp)    return "«primop»";
    if (t == Tag::PrimOpApp) return "«partially applied primop»";
    if (t == Tag::Thunk)     return "«unforced thunk»";
    if (t == Tag::App)       return "«unforced app»";
    if (t == Tag::App3)      return "«unforced app3»";
    if (t == Tag::Blackhole) return "«potential infinite recursion»";
    if (t == Tag::Slot)      return "«slot»";
    return "«value»";
}

/// #685 — opcode-side mirror of TW's `forceStringNoCtx`
/// (libexpr/eval.cc:2826).  Throws TW's exact error text when the
/// string value carries any context.  Used for dynamic attr names
/// (`{ ${ctxedStr} = v; }` / `.${ctxedStr}` / `?${ctxedStr}`) which
/// TW gates via forceStringNoCtx in evalDynamicAttrs.  The
/// primops.cc version of this helper takes an EvalState; this one
/// reaches the store via the global `getNixEvalState()` so it can
/// be called from inside opcode dispatch where no `state` is in
/// scope.  Empty-context strings short-circuit cheaply.
void requireNoStringContextRuntime(const Value & v,
                                          std::string_view siteHint)
{
    if (!v.isString() || !v.asString()) return;
    auto * raw = lookupStringContextEntries(v.asString());
    if (!raw || raw->empty()) return;
    std::string display = raw->front();
    if (auto * ns = getNixEvalState())
        display = ffi::displayContextElem(*ns, raw->front());
    // V3_DBG_NOCTX_SITE: see matching site in primops.cc.  Cold path.
    static const bool s_dbgNoCtxSite =
        std::getenv("V3_DBG_NOCTX_SITE") != nullptr;
    if (__builtin_expect(s_dbgNoCtxSite, 0))
        std::fprintf(stderr, "v3 NOCTX-SITE: requireNoStringContextRuntime hint=%.*s\n",
                     (int)siteHint.size(), siteHint.data());
    throw std::runtime_error(
        std::string("the string '") + v.asString()
        + "' is not allowed to refer to a store path (such as '"
        + display + "')");
}

/// Coerce a Value to its string representation for OP_STR_CONCAT.
/// Bring-up subset: int / float / bool / string / path / null.  Lists,
/// attrsets, and lambdas trigger an error here for now (the AST → IR pass
/// is responsible for inserting `toString` primop calls where needed).
///
/// In interpolation context (`forceString = true`) we route Path values
/// through tree-walker's `copyPathToStore` (DryRun under
/// settings.readOnlyMode = true) so `${./foo}` produces the proper
/// `/nix/store/<32-hash>-name` representation, not the absolute file
/// path.  Required by tests like `eval-okay-context` that count on the
/// store-path prefix length.
/// #680 — coerce a value to a string for the `+` operator and `${...}`
/// interpolation.  Mirrors TW's `coerceToString(coerceMore=false)`
/// (libexpr/eval.cc:2874): String + Path → string (paths optionally
/// copied to store when forceString=true); EVERYTHING else rejects
/// with `cannot coerce <type> to a string: <value>` matching TW's
/// libexpr/eval.cc:2911 phrasing.
///
/// Pre-fix v3 silently accepted Int/Float/Bool/Null: `"x" + 1`
/// returned `"x1"`, `null + 1` returned `"1"`, `"${1}"` returned `"1"`.
/// All of these are TW errors.  The relaxed behavior could hide bugs
/// in nixpkgs / user code where a value of the wrong type leaks into
/// string-context.
///
/// Attrset coercion (`__toString` / `outPath`) is handled BEFORE this
/// helper by the OP_STR_CONCAT attr-unwind loop (vm.cc:8051+), so by
/// the time we get here the value is a primitive.
std::string coerceToString(const Value & vIn, bool forceString)
{
    // C-4b / «slot» residual (CODEBASE_REVIEW_2026-06-11): Tag::Slot is a
    // v3-only TRANSPARENT pointer into a tenured cell — the actual value is
    // `*slot` (tree-walker has no Slot, so every value-expecting leaf must
    // deref it; the VM chases Slots at ~15 other sites).  coerceToString did
    // NOT: a Slot operand fell straight through to the "cannot coerce a value
    // to a string: «slot»" error.  That is exactly the ghc98 .drvPath residual
    // (a Slot reached `${...}` / `+` coercion un-deref'd).  Chase the Slot
    // chain (bounded by kMaxIndirectionChase, mirroring the other deref loops)
    // to the underlying value before coercing.  If the resolved value is still
    // non-coercible, the error below now reflects that REAL type, not «slot».
    Value vDeref = vIn;
    size_t slotGuard = 0;
    while (vDeref.tag() == Tag::Slot && vDeref.asSlot()
           && ++slotGuard < kMaxIndirectionChase)
        vDeref = *vDeref.asSlot();
    const Value & v = vDeref;
    // Use if/else rather than switch to avoid -Wswitch-enum on every
    // tag we don't care to spell out individually.
    auto typeName = [](const Value & v) -> std::pair<const char *, const char *> {
        Tag t = v.tag();
        if (t == Tag::Int)   return {"an", "integer"};
        if (t == Tag::Float) return {"a",  "float"};
        if (t == Tag::Bool)  return {"a",  "Boolean"};
        if (t == Tag::Null)  return {"",   "null"};
        if (t == Tag::List)  return {"a",  "list"};
        if (t == Tag::Attrs) return {"a",  "set"};
        if (t == Tag::Closure || t == Tag::PrimOp || t == Tag::PrimOpApp)
            return {"a", "function"};
        if (t == Tag::Thunk || t == Tag::App || t == Tag::App3)
            return {"a", "thunk"};
        return {"a", "value"};
    };
    auto throwCoerceError = [&](const Value & v) -> std::string {
        // #691 — use the shared `valueRepr` for TW-equivalent rendering.
        // Pre-fix this lambda used a stub that emitted `{ ... }` / `[ ... ]`
        // placeholders, leaving a PREFIX-class gap vs TW's actual values.
        auto [article, name] = typeName(v);
        std::string msg = "cannot coerce ";
        if (*article) { msg += article; msg += ' '; }
        msg += name;
        msg += " to a string: ";
        msg += valueRepr(v);
        throw std::runtime_error(msg);
    };

    switch (v.tag()) {
    case Tag::String: return std::string(v.asString());
    case Tag::Path: {
        std::string p(v.asPath() ? v.asPath() : "");
        if (forceString) {
            if (auto * ns = getNixEvalState()) {
                // Let copyPathToStore exceptions propagate — tree-walker
                // raises on missing paths during interpolation, and v3
                // should match.  Note for the caller: this string carries
                // an Opaque context entry for `storePath`; the caller is
                // responsible for recording it (see OP_STR_CONCAT below).
                return ffi::coercePathToStore(*ns, p);
            }
        }
        return p;
    }
    case Tag::Int:
    case Tag::Float:
    case Tag::Bool:
    case Tag::Null:
    case Tag::List:
    case Tag::Attrs:
    case Tag::Closure:
    case Tag::PrimOp:
    case Tag::PrimOpApp:
    case Tag::Thunk:
    case Tag::App:
    case Tag::App3:
    case Tag::Blackhole:
    case Tag::External:
    case Tag::Slot:
    case Tag::Uninitialized:
    default:
        return throwCoerceError(v);
    }
}

// P0.1b / #768 (2026-07-02): mergeBindings' env-gate knobs, promoted
// from function-local `static const` to file-scope `static const`.  As
// function-locals each read paid a magic-static guard load — and the
// chain paths read them ~11× per call, in the #1 Bindings producer.  At
// file scope they are initialized once at dynamic-init from the SAME
// getenv logic (byte-identical values ⇒ BI preserved) with no per-read
// guard.  Used only by mergeBindings.
//   NIX_V3_NO_MAPATTRS_MERGE_LAZY  — disable the MapAttrs no-realize merge.
//   NIX_V3_NO_CHAIN_BINDINGS / NIX_V3_CHAIN_BINDINGS(=0) — chain on/off.
//   NIX_V3_CHAIN_MIN_NA=1    — parent must be non-empty to chain-construct.
//   NIX_V3_CHAIN_MAX_NB=8192 — overlay cap (bounds chain materialization).
static const bool s_mapAttrsMergeLazy = []{
    return std::getenv("NIX_V3_NO_MAPATTRS_MERGE_LAZY") == nullptr;
}();
static const bool s_chain = []{
    const char * e = std::getenv("NIX_V3_CHAIN_BINDINGS");
    if (e) return e[0] != '0';   // explicit force on/off (=0 → off)
    return true;                 // default ON
}();
static const uint32_t s_minNa = []{
    const char * e = std::getenv("NIX_V3_CHAIN_MIN_NA");
    return e ? (uint32_t) std::strtoul(e, nullptr, 10) : 1u;
}();
static const uint32_t s_maxNb = []{
    const char * e = std::getenv("NIX_V3_CHAIN_MAX_NB");
    return e ? (uint32_t) std::strtoul(e, nullptr, 10) : 8192u;
}();

Bindings * mergeBindings(const Bindings * a, const Bindings * b,
                         MergeBindingsSite siteId)
{
    // #821 per-site call counter — bumped at function entry so the
    // empty-operand short-circuit (below) contributes to the call
    // count even though it doesn't allocate; the BYTES counter is
    // updated only after `Alloc::allocBindings` so its sum matches
    // `bytesBindings` for the merge-attributed slice.
    // P0.1b (2026-07-02): wrapped in V3_STATS_BLOCK so the #821 raw
    // counters strip under -Dv3_release=true (audit §1.3 — they bypassed
    // even V3_STATS before).  No change under the default instrumented
    // build (V3_STATS_BLOCK == `if (true)`); the counters still feed the
    // NIX_VM_STATS mergeBindings-by-site table (run.cc:516-580).
    V3_STATS_BLOCK {
        const uint8_t s = static_cast<uint8_t>(siteId);
        if (s < AllocStats::kMergeBindingsSiteSlots)
            ++allocStats().mergeBindingsCallsBySite[s];
    }

    // #821 — input-size histograms (na, nb).  Sample both `//`
    // opcodes: older HNE profiles were UPDATE_TAIL-dominant, while the
    // current register VM profile routes the hot path through
    // OP_ATTRS_UPDATE.  Bucket via a small switch (5 cmps avg) —
    // negligible cost compared to the merge itself.
    // P0.1b: V3_STATS_BLOCK guard strips the whole histogram (incl. the
    // bucket_of lambda) under -Dv3_release=true; no-op under the default
    // instrumented build.  NB (dangling-else guard): V3_STATS_BLOCK
    // expands to `if (true/false)`, so this is `if (…) if (siteId …) {…}`
    // — do NOT add an `else` to the inner `if` below; it would bind to
    // the V3_STATS_BLOCK `if`.  Wrap in explicit braces first if an else
    // is ever needed.
    V3_STATS_BLOCK if (siteId == MergeBindingsSite::AttrsUpdate
        || siteId == MergeBindingsSite::AttrsUpdateTail) {
        // #821 follow-on: split the 0..1 bucket into nb=0 (short-
        // circuit) vs nb=1 (single-key patch).  The two have very
        // different optimisation implications: nb=0 is already free
        // (just return parent), while nb=1 is the canonical "single-
        // attr override" pattern where a smart patched representation
        // could amortise parent copy.
        auto bucket_of = [](uint32_t n) -> uint8_t {
            if (n == 0)   return 0;   // nb=0 — short-circuit
            if (n == 1)   return 1;   // nb=1 — single-key patch
            if (n <= 4)   return 2;
            if (n <= 8)   return 3;
            if (n <= 16)  return 4;
            if (n <= 32)  return 5;
            if (n <= 64)  return 6;
            if (n <= 128) return 7;
            if (n <= 256) return 8;
            return 9;
        };
        ++allocStats().mergeBindingsNaHist[bucket_of(a ? a->size : 0)];
        ++allocStats().mergeBindingsNbHist[bucket_of(b ? b->size : 0)];
    }

    // `a // {}` / `{} // b`: return the non-empty operand before
    // materialising MapAttrs inputs.  The previous ordering flattened a lazy
    // mapped attrset even when the other operand was empty, paying a full
    // Bindings copy plus one App3 cell per mapped entry for a merge whose result
    // is just the original operand.
    if (b && !b->isChain() && b->size == 0) return const_cast<Bindings *>(a);
    if (a && !a->isChain() && a->size == 0) return const_cast<Bindings *>(b);

    // P0.1b / #768: s_mapAttrsMergeLazy / s_chain / s_minNa / s_maxNb
    // are now file-scope statics (defined just above this function) so
    // the ~11 reads below no longer each pay a magic-static guard load.

    if (s_mapAttrsMergeLazy && s_chain
        && a && b && a->isMapAttrs() && !b->isMapAttrs() && !b->isChain()
        && b->size > 0 && b->size <= s_maxNb
        && a->chainDepth() < Bindings::Cursor::kMaxLayers) {
        Bindings * c = Alloc::allocChainBindings(a, b->size);
        for (uint32_t j = 0; j < b->size; ++j) {
            c->entries[j] = b->entries[j];
            c->entries[j].pos &= Bindings::kPosMask;
        }
        bindingsPostConstructBarrier(c);
        if (__builtin_expect(g_sharedWbDetect, 0)) ++chainChildCount()[a];
        return c;
    }

    auto tryMergeMapAttrsNoRealize = [&]() -> Bindings * {
        if (!a || !b)
            return nullptr;
        if (!s_mapAttrsMergeLazy)
            return nullptr;
        if (!a->isMapAttrs() && !b->isMapAttrs())
            return nullptr;
        if (a->isChain() || b->isChain())
            return nullptr;

        const Bindings * mapShape = a->isMapAttrs() ? a : b;
        if (a->isMapAttrs() && b->isMapAttrs()
            && (a->parent != b->parent
                || a->mapAttrsAux()->w != b->mapAttrsAux()->w))
            return nullptr;

        if (!a->isMapAttrs() && b->isMapAttrs()
            && s_chain && b->size <= s_maxNb && a->size >= s_minNa
            && uint64_t(a->size) > uint64_t(b->size) * 2)
            return nullptr;
        if (a->isMapAttrs() && !b->isMapAttrs()
            && s_chain && b->size <= s_maxNb && a->size >= s_minNa
            && uint64_t(a->size) > uint64_t(b->size) * 2)
            return nullptr;

        const uint32_t na = a->size, nb = b->size;
        uint32_t kExact = 0;
        {
            uint32_t i = 0, j = 0;
            while (i < na && j < nb) {
                const SymbolId an = a->entries[i].name;
                const SymbolId bn = b->entries[j].name;
                if (an < bn)        { ++kExact; ++i; }
                else if (an > bn)   { ++kExact; ++j; }
                else                { ++kExact; ++i; ++j; }
            }
            kExact += (na - i) + (nb - j);
        }
        if (kExact == 0)
            return Alloc::allocBindings(0);

        Bindings * out = Alloc::allocMapAttrsBindings(kExact);  // P1a: aux tail
        out->parent = mapShape->parent;
        *out->mapAttrsAux() = *mapShape->mapAttrsAux();

        V3_STATS_BLOCK {
            const uint8_t s = static_cast<uint8_t>(siteId);
            if (s < AllocStats::kMergeBindingsSiteSlots)
                allocStats().mergeBindingsBytesBySite[s]
                    += uint64_t(kExact) * sizeof(Bindings::Entry);
        }

        uint32_t i = 0, j = 0, k = 0;
        auto copyA = [&]() {
            out->entries[k] = a->entries[i];
            if (!a->isMapAttrs())
                out->entries[k].pos &= Bindings::kPosMask;
            ++k; ++i;
        };
        auto copyB = [&]() {
            out->entries[k] = b->entries[j];
            if (!b->isMapAttrs())
                out->entries[k].pos &= Bindings::kPosMask;
            ++k; ++j;
        };
        while (i < na && j < nb) {
            if (a->entries[i].name < b->entries[j].name) {
                copyA();
            } else if (a->entries[i].name > b->entries[j].name) {
                copyB();
            } else {
                copyB();
                ++i;
            }
        }
        while (i < na) copyA();
        while (j < nb) copyB();
        bindingsPostConstructBarrier(out);
        return out;
    };

    if (Bindings * merged = tryMergeMapAttrsNoRealize())
        return merged;

    if (a && a->isMapAttrs())
        a = a->materialize();
    if (b && b->isMapAttrs())
        b = b->materialize();

    // Lever A Step 4 (MEMORY_REPRESENTATION §6) — CHAIN COMPOSITION.
    // Before materialising chain inputs, try to EXTEND an existing
    // chain by prepending `b` as a new highest-precedence overlay.
    // This is the firefox win: deep override stacks (`a // b // c // …`
    // from overrideAttrs / wrapFirefox / buildMozillaMach) then cost
    // O(depth) tiny overlays + ONE shared base, instead of re-copying
    // the whole base on every `//` (the prior unconditional
    // `a->materialize()` made a depth-D stack pay O(D) full copies).
    // Conditions: chains enabled; `a` is a chain with room under the
    // Cursor layer cap; `b` is a small Sorted overlay.  Depth is
    // bounded by Cursor::kMaxLayers so chain-aware lookup / cursor
    // iteration stay O(cap).  Beyond the cap we fall through to
    // materialise-and-merge (the Cursor also has a materialise safety
    // net, but capping here keeps every consumer cheap).
    if (s_chain && a->isChain() && !b->isChain()
        && b->size > 0 && b->size <= s_maxNb
        && a->chainDepth() < Bindings::Cursor::kMaxLayers) {
        Bindings * c = Alloc::allocChainBindings(a, b->size);
        for (uint32_t j = 0; j < b->size; ++j)
            c->entries[j] = b->entries[j];  // overlay sorted
        bindingsPostConstructBarrier(c);    // Phase D batch barrier
        if (__builtin_expect(g_sharedWbDetect, 0)) ++chainChildCount()[a];  // WS-A detector
        return c;
    }

    // Chained RHS rescue: `(large-or-chain a) // (small visible chain b)`.
    // The sorted-merge fallback below would materialise BOTH inputs before
    // merging.  If the RHS chain's visible surface is still a tiny overlay,
    // copy just those visible entries into a fresh leaf above `a`.  This
    // preserves `//` precedence, keeps writeback-safe private RHS slots, and
    // avoids copying `a` solely because `b` happened to already be a Chain.
    if (s_chain && b->isChain()
        && (a->isChain() || a->size >= s_minNa)
        && a->chainDepth() < Bindings::Cursor::kMaxLayers) {
        const uint32_t nbVisible = b->countDistinct();
        if (nbVisible == 0) return const_cast<Bindings *>(a);
        if (nbVisible <= s_maxNb) {
            Bindings * c = Alloc::allocChainBindings(a, nbVisible);
            uint32_t j = 0;
            b->forEach([&](const Bindings::Entry & e) {
                c->entries[j++] = e;  // visible RHS entries sorted
            });
            bindingsPostConstructBarrier(c);  // Phase D batch barrier
            if (__builtin_expect(g_sharedWbDetect, 0)) ++chainChildCount()[a];  // WS-A detector
            return c;
        }
    }

    // `a // {}` — return `a` UNCHANGED, preserving its chain form (do
    // not materialise just to drop an empty overlay).  Common in
    // nixpkgs via `a // lib.optionalAttrs cond {…}` when cond is false.
    // Guard `!b->isChain()` because a chain's `size` is its overlay
    // count, not its logical size (a chain is never empty by
    // construction, but the guard keeps the invariant explicit).
    if (!b->isChain() && b->size == 0) return const_cast<Bindings *>(a);

    // Not composing and at least one input is still a Chain.  Stream the
    // visible entries from both inputs with Cursor and build the final Sorted
    // merge directly, instead of first materialising flat copies of the
    // inputs and then copying again into `out`.
    auto mergeByCursor = [&]() -> Bindings * {
        const uint32_t naVisible = a->countDistinct();
        const uint32_t nbVisible = b->countDistinct();
        if (naVisible == 0 && nbVisible > 0) return const_cast<Bindings *>(b);
        if (nbVisible == 0 && naVisible > 0) return const_cast<Bindings *>(a);

        uint32_t kExact = 0;
        {
            Bindings::Cursor ca(a), cb(b);
            const Bindings::Entry * ea = ca.next();
            const Bindings::Entry * eb = cb.next();
            while (ea && eb) {
                if (ea->name < eb->name)      { ++kExact; ea = ca.next(); }
                else if (ea->name > eb->name) { ++kExact; eb = cb.next(); }
                else                          { ++kExact; ea = ca.next(); eb = cb.next(); }
            }
            while (ea) { ++kExact; ea = ca.next(); }
            while (eb) { ++kExact; eb = cb.next(); }
        }

        Bindings * out = Alloc::allocBindings(kExact);
        V3_STATS_BLOCK {
            const uint8_t s = static_cast<uint8_t>(siteId);
            if (s < AllocStats::kMergeBindingsSiteSlots)
                allocStats().mergeBindingsBytesBySite[s]
                    += uint64_t(kExact) * sizeof(Bindings::Entry);
        }

        Bindings::Cursor ca(a), cb(b);
        const Bindings::Entry * ea = ca.next();
        const Bindings::Entry * eb = cb.next();
        uint32_t k = 0;
        auto copyA = [&]() {
            out->entries[k++] = *ea;
            ea = ca.next();
        };
        auto copyB = [&]() {
            out->entries[k++] = *eb;
            eb = cb.next();
        };
        while (ea && eb) {
            if (ea->name < eb->name) {
                copyA();
            } else if (ea->name > eb->name) {
                copyB();
            } else {
                copyB();       // duplicate; b wins (incl. its position)
                ea = ca.next();
            }
        }
        while (ea) copyA();
        while (eb) copyB();
        bindingsPostConstructBarrier(out);  // Phase D batch barrier
        return out;
    };

    if (a->isChain() || b->isChain())
        return mergeByCursor();

    // Sorted-merge two attrsets (b wins on duplicate keys).  Per-attr
    // positions in attrPosTable are keyed by (Bindings*, SymbolId), so
    // when an entry is copied to the freshly-allocated `out`, we
    // forward its source position too.  Without this,
    // `builtins.unsafeGetAttrPos` on a merged attrset returns null
    // for every name (REVIEW critic §8 #4).
    //
    // #747 (2026-05-21) two-pass: pass 1 counts the exact number of
    // distinct keys; pass 2 allocates the precise size and fills.
    // The prior single-pass version called Alloc::allocBindings(na +
    // nb) up front and wrote `out->size = k` at the end, leaving the
    // arena pinned at the worst-case size even when duplicates were
    // collapsed.  #746 attribution measured ~387 MB of that slack on
    // hello.drvPath (~half of all v3-arena Bindings bytes).  The
    // bump-pointer arena cannot reclaim the unused tail, so the slack
    // becomes permanent until process exit.
    //
    // Pass 1 cost: one extra mirror-merge sweep reading only `name`
    // fields (4 B per Entry).  For na+nb in the thousands the array
    // stays L1-warm across both passes.  The arithmetic is identical
    // to the original branch logic so `kExact` and the final `k` from
    // pass 2 always agree by construction.
    const uint32_t na = a->size, nb = b->size;

    // #748 (2026-05-22) zero-operand short-circuit.  `a // {}` and
    // `{} // b` are common in nixpkgs (`pkgs // optionalAttrs cond
    // {...}` where cond is false; lib.optionalAttrs returns {}).
    // Both inputs are already WHNF and `mergeBindings` callers treat
    // the result as immutable, so returning the non-empty operand
    // directly is safe — no entries to merge, no allocation, no
    // per-attr position rebuilding.
    //
    // Why this is safe with #752 inline PosIdx: positions are stored
    // INSIDE Bindings::Entry rather than in a side-table keyed by
    // (Bindings*, SymbolId), so a shared Bindings pointer carries its
    // own positions inherently.  Pre-#752 this short-circuit was
    // unsafe because the side-table would have given the wrong
    // Bindings identity for position lookup.
    //
    // Empty-result `{} // {}` falls through to the (now trivial)
    // two-pass which produces a size-0 Bindings — small enough that
    // a special case here isn't worth its mental overhead.
    if (na == 0 && nb > 0) return const_cast<Bindings *>(b);
    if (nb == 0 && na > 0) return const_cast<Bindings *>(a);

    // MEMORY_ATTACK_PLAN re-test (2026-06-06): the prior 5 Chain Phase C
    // falsifications (ledger below) were all on the PRE-register-VM tree.  The
    // register-VM rework this session materially changed the call / formals /
    // arg-passing path — exactly the subsystem where the unidentified
    // `f origArgs -> {}` collapse lived (the formals destructure at vm.cc:~5350
    // now materialises Chains; the whole calling convention changed via R_CALL).
    // So "5×-falsified" is STALE; per the falsification rule this material change
    // to the failing subsystem is the new insight that justifies a re-test (NOT
    // a blind rebuild).  Gated NIX_V3_CHAIN_BINDINGS=1 (default OFF); conditions
    // mirror attempt #4/#5 (large parent, tiny overlay; a/b already materialised
    // above so neither is a Chain).  Build Chain{parent=a, overlay=b} instead of
    // copying a's na entries.
    // Fresh chain construction (a, b both Sorted here): `big // small`
    // builds Chain{parent=a, overlay=b} instead of copying a's na
    // entries.  Uses the hoisted s_chain / s_minNa / s_maxNb knobs.
    if (s_chain && nb <= s_maxNb && na >= s_minNa) {
        Bindings * c = Alloc::allocChainBindings(a, nb);
        for (uint32_t j = 0; j < nb; ++j)
            c->entries[j] = b->entries[j];  // overlay sorted
        bindingsPostConstructBarrier(c);    // Phase D batch barrier
        if (__builtin_expect(g_sharedWbDetect, 0)) ++chainChildCount()[a];  // WS-A detector
        return c;
    }

    // #826 / A1a Phase C attempt #4 + #5 (2026-05-30, EXIT_GC_SPIRAL):
    // REVERTED — same failure mode as prior 3 attempts.
    //
    // Enabled NIX_V3_CHAIN_BINDINGS=1 with chain-construct path:
    // when nb ≤ 4 && na ≥ 16 && !a->isChain() && !b->isChain(),
    // build Chain{parent=a, overlay=b}.  All 5 nixpkgs paths failed
    // (hello.name, hello.pname, hello.drvPath, hello.outPath,
    // firefox.name) with the same error as v2/v3:
    // "attribute 'buildPythonApplication' missing".
    //
    // Per [[measure-twice-cut-once]] "Three failed pivots on same
    // premise = falsification" — this is the 4th pivot.  The
    // structural failure mode reproduces exactly across 4 attempts
    // with different chain-construct shapes, confirming the
    // architectural prereq (190-site entries[] audit + Nix-level
    // repro) is genuinely required.
    //
    // See EXIT_PHASE_C_4_FALSIFIED_2026-05-30.md for the 4-pivot
    // ledger + the architectural-blocker writeup.
    //
    // allocChainBindings helper stays in alloc.hh as the (now-
    // documented) staging point for a future multi-session attempt
    // that completes the prereqs first.

    // #826 / A1a Phase C — FALSIFIED across three attempts this
    // session (per measure-twice-cut-once §3.8 "three failed pivots
    // = falsification").  Chain construction reserved for a future
    // multi-session push that includes the full 208-site entries[]
    // audit.  Falsification ledger:
    //
    // - v1 (6f8095cd5): missed Phase D barrier in materialize();
    //   reverted.
    // - v2 (2cf14fdce): fixed materialize() barrier + consumer-site
    //   fallbacks + materialise mergeBindings inputs upfront.  Lang
    //   + core PASS; nixpkgs hello.name FAILED with
    //   `attribute 'buildPythonApplication' missing` on a 2-entry
    //   Bindings.
    // - v3 (this session, reverted in this commit): added
    //   `serializeAttrs` + `valuesEqual` chain-materialise to fix
    //   suspected Phase 5 cache corruption.  Brute audit clean;
    //   nixpkgs hello.name STILL FAILED with the same
    //   2-entry-Bindings error EVEN WITH the disk cache disabled
    //   (`NIX_V3_NO_DISK_CACHE=1`).  Diagnostic with V3_DBG_CHAIN_
    //   SELECT=1 confirmed: chain materialise IS firing correctly
    //   (chain size=1-3 → materialised size=41-494), so the 2-
    //   entry failure Bindings is a *Sorted* of overlay-only-shape,
    //   not a Chain.  Hypothesis: the chain spike is causing some
    //   Nix-level `f origArgs` to silently return `{}`, then
    //   `{} // {override, overrideDerivation}` short-circuits to
    //   the overlay (size=2).  Tracing requires reduced repro
    //   isolating the `f origArgs` failure point.
    //
    // Phase C revival prerequisites (carry-forward to a multi-
    // session task):
    //   1. Build a Nix-level minimal repro that triggers the
    //      `{} // overlay` collapse under chain spike.
    //   2. Identify which value-flow within nixpkgs `lib.make-
    //      Overridable` / `callPackageWith` / `python3.pkgs`
    //      machinery silently returns `{}` under chain interaction.
    //   3. Audit the 208 `entries[]` sites across the v3 tree.
    //   4. Either convert all iteration sites to `forEach` /
    //      `materialize()` OR keep chain construction gated on a
    //      whitelist of confirmed-safe call patterns.

    // Pass 1: count distinct keys.  Mirror of the branch logic
    // below; only reads `name` fields, no allocations, no copies,
    // no position lookups.
    uint32_t kExact = 0;
    {
        uint32_t i = 0, j = 0;
        while (i < na && j < nb) {
            const SymbolId an = a->entries[i].name;
            const SymbolId bn = b->entries[j].name;
            if (an < bn)        { ++kExact; ++i; }
            else if (an > bn)   { ++kExact; ++j; }
            else                { ++kExact; ++i; ++j; } // duplicate collapsed
        }
        kExact += (na - i) + (nb - j);
    }

    // Pass 2: allocate the exact size and fill.  `Alloc::allocBindings`
    // already sets `out->size = kExact`, so no second size-write is
    // needed (or wanted — it would be a redundant store to a Phase D
    // shared cell line, and on the empty-sentinel path a write to
    // shared read-only state).
    Bindings * out = Alloc::allocBindings(kExact);
    // #821 per-site bytes attribution.  Bytes attributed = kExact
    // entries (slack-free since #747's two-pass) × sizeof Entry
    // (24 B post-#752 inline-pos).  Calls counter is bumped at
    // function entry above; bytes counter accumulates only on the
    // allocating path.
    V3_STATS_BLOCK {
        const uint8_t s = static_cast<uint8_t>(siteId);
        if (s < AllocStats::kMergeBindingsSiteSlots)
            allocStats().mergeBindingsBytesBySite[s]
                += uint64_t(kExact) * sizeof(Bindings::Entry);
    }
    uint32_t i = 0, j = 0, k = 0;
    // #752: Entry copies include the inline `pos` field, so the
    // per-attr position is forwarded by the raw Entry copy itself.
    // The old explicit recordAttrPos call (which used the side-table)
    // is no longer needed and would be a no-op anyway.
    auto copyA = [&]() {
        out->entries[k] = a->entries[i];
        ++k; ++i;
    };
    auto copyB = [&]() {
        out->entries[k] = b->entries[j];
        ++k; ++j;
    };
    while (i < na && j < nb) {
        if (a->entries[i].name < b->entries[j].name) {
            copyA();
        } else if (a->entries[i].name > b->entries[j].name) {
            copyB();
        } else {
            copyB();   // duplicate; b wins (incl. its position)
            ++i;
        }
    }
    while (i < na) copyA();
    while (j < nb) copyB();
    // Invariant: k == kExact by construction (the two passes share
    // identical branch arithmetic).  No need to rewrite out->size.
    bindingsPostConstructBarrier(out);  // Phase D batch barrier
    return out;
}

} // namespace nix::v3
