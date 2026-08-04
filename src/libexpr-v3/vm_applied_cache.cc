/// @file
/// v3 VM applied-import RESULT cache — policy / key-building / probe / shadow
/// cluster, extracted from vm.cc (step 3 of the vm.cc split).  PURE MOVE: these
/// definitions are byte-for-byte the ones that used to live in vm.cc; only the
/// linkage of the five cross-TU entry points changed (internal → external,
/// declared in v3/vm_internal.hh) so vm.cc's OP_CALL / OP_TAIL_CALL / callClosure
/// apply paths + OP_RETURN shadow-compare/insert can reach them.
///
/// This is the LEVER-1 applied-import result cache's DECISION half — it decides
/// whether an application is cacheable (appliedCacheOn / appliedCacheShadowMode),
/// builds the desc+canonical-args memo key (appliedCacheTryKey / appliedKey-
/// Precheck), runs the pre-build NIX_V3_APPLIED_CACHE=probe/count instrumentation
/// (appliedCacheProbeObserve / appliedProbeBoundedKey / AppliedCacheProbeStats),
/// and validates in shadow mode (appliedShadowCompare / appliedShadowCompareOne).
///
/// The cache STORAGE half — the key→Value table, LRU cap, hit/miss counters, and
/// import-result provenance set (appliedCacheLookup / appliedCacheInsert /
/// appliedCacheLookupPeek / appliedCacheNote* / appliedCacheStatsDump /
/// appliedCacheRecordImportResult / appliedCacheIsImportResultDesc) — already
/// lives in its own TU (primops.cc, declared in v3/primop.hh) and is UNCHANGED;
/// the moved decision code here calls into it across the TU boundary.
///
/// File-local (anonymous-namespace / `static`) and NOT part of the cross-TU
/// surface: AppliedCacheProbeStats + appliedCacheProbeStats, kAppliedKeyBudget,
/// appliedProbeBoundedKey, appliedKeyPrecheck, appliedShadowCompareOne.
///
/// Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.
/// SPDX-License-Identifier: Apache-2.0

#include "v3/vm_internal.hh"      // 5 cross-TU entry-point decls; brings v3/vm.hh
#include "v3/alloc.hh"            // full Bindings/ListVec layout (forEach/countDistinct)
#include "v3/primop.hh"           // VMState, forceValue, appliedCache{Lookup,Note}* storage layer
#include "v3/value_serialize.hh"  // value_serialize::canonicalHash (the memo-key digest)
#include "v3/ir.hh"               // ir::globalSymbolTable (probe key attr names)
#include "v3/gc_root.hh"          // GcRoot — root the probe arg across re-entrant forceValue

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_set>
#include <vector>

namespace nix::v3 {

// ---------------------------------------------------------------------------
// LEVER-1 applied-import cache — PROBE instrumentation (NIX_V3_APPLIED_CACHE=
// probe; lode/NEXT_LEVERS_2026-07-04.md Part B step 1).  Rule-0 falsifier run
// BEFORE building the cache: counts would-cache OP_CALL applications (callee =
// import-CU closure with formals, plain single-arg path) and how many DISTINCT
// (CU, argsHash) keys they collapse to.  wouldHit = the in-process hit ceiling.
// Pure observation — no insert, no reuse, no forcing (canonicalHash chases only
// already-Evaluated indirections and throws on Suspended → counted unhashable).
// RETIREMENT CRITERION: this probe is replaced by the real cache's stats at
// spike step 2+, or deleted with the probe-verdict handback if the spike KILLs.
namespace {
struct AppliedCacheProbeStats {
    uint64_t eligibleCalls  = 0;  // import-CU callee, plain single-arg path
    uint64_t noFormals      = 0;  // of those: callee WITHOUT formals (diagnostic)
    uint64_t bySite[3]      = {0, 0, 0};  // 0=OP_CALL 1=OP_TAIL_CALL 2=callClosure
    uint64_t hashedCalls    = 0;  // args canonically hashable (deep-forced)
    uint64_t unhashableArgs = 0;  // hash/force threw (fn args, throw, etc.)
    uint64_t wouldHit       = 0;  // key seen before = the cache's hit ceiling
    std::unordered_set<std::string> keys;   // distinct (CU*, argsHash)
};
AppliedCacheProbeStats & appliedCacheProbeStats()
{
    static auto * s = [] {
        auto * p = new AppliedCacheProbeStats();
        // Belt-and-braces: atexit dump (works in v3-eval) AND the run.cc
        // end-of-root-eval dump (works in the `nix` binary, where atexit
        // output is lost).  Both print the same cumulative counters.
        std::atexit([] {
            const auto & st = appliedCacheProbeStats();
            if (st.eligibleCalls == 0) return;
            std::fprintf(stderr,
                "v3 APPLIED-CACHE PROBE: eligible=%llu (call=%llu tail=%llu cc=%llu "
                "noFormals=%llu) hashed=%llu unhashable=%llu distinctKeys=%zu wouldHit=%llu\n",
                (unsigned long long)st.eligibleCalls,
                (unsigned long long)st.bySite[0],
                (unsigned long long)st.bySite[1],
                (unsigned long long)st.bySite[2],
                (unsigned long long)st.noFormals,
                (unsigned long long)st.hashedCalls,
                (unsigned long long)st.unhashableArgs,
                st.keys.size(),
                (unsigned long long)st.wouldHit);
        });
        return p;
    }();
    return *s;
}
/// BOUNDED force+serialize for the memo key (probe form).  The first probe
/// iteration used forceDeep + canonicalHash — it EXPLODED on nixpkgs (>>120 s):
/// nixpkgs-internal import-CU applications (booter.nix, stage fns) take
/// pkgs-sized lazy args, and deep-forcing them evaluates enormous graphs.  So
/// the key computation MUST be budget-capped: walk the args, forcing as we go,
/// appending a process-stable byte encoding; BAIL (uncacheable) on budget
/// exhaustion or a non-data tag (closure/PAP/primop — e.g. overlays).  Small
/// top-level config attrsets (`import <nixpkgs> { config... }`) fit easily in
/// the budget; pkgs-sized args bail after kAppliedKeyBudget nodes of work —
/// bounded perturbation.  Encoding is per-process stable (not canonical): the
/// probe only measures within-process hit rates.  GC discipline: every Value
/// held across the re-entrant forceValue is GcRoot'd (Rule 1).
constexpr int kAppliedKeyBudget = 512;
bool appliedProbeBoundedKey(VMState & vm, const Value & v0, std::string & out, int & budget)
{
    if (--budget < 0) return false;
    Value local = v0;
    GcRoot r(local);
    local = forceValue(vm, local);   // may scavenge; local is rooted+rewritten
    switch (local.tag()) {
    case Tag::Int: {
        int64_t i = local.asInt();
        out.push_back('i'); out.append(reinterpret_cast<const char *>(&i), 8);
        return true;
    }
    case Tag::Float: {
        double d = local.asFloat();
        out.push_back('f'); out.append(reinterpret_cast<const char *>(&d), 8);
        return true;
    }
    case Tag::Bool:  out.push_back(local.asInt() ? 'T' : 'F'); return true;  // vTrue/vFalse: asInt()=0|1
    case Tag::Null:  out.push_back('n'); return true;
    case Tag::String: {
        // NOTE: string CONTEXT is ignored here (probe-only; per-process
        // discrimination not canonical).  The real cache must include it.
        const char * s = local.asString();
        uint32_t n = s ? (uint32_t)std::strlen(s) : 0;
        out.push_back('s'); out.append(reinterpret_cast<const char *>(&n), 4);
        if (s) out.append(s, n);
        return true;
    }
    case Tag::Path: {
        const char * s = local.asString();
        uint32_t n = s ? (uint32_t)std::strlen(s) : 0;
        out.push_back('p'); out.append(reinterpret_cast<const char *>(&n), 4);
        if (s) out.append(s, n);
        return true;
    }
    case Tag::List: {
        ListVec * l = local.asList();
        uint32_t n = l ? l->size : 0;
        out.push_back('['); out.append(reinterpret_cast<const char *>(&n), 4);
        for (uint32_t i = 0; i < n; ++i) {
            // Re-read through the rooted local each iteration: the recursive
            // call can scavenge and relocate the list.
            if (!appliedProbeBoundedKey(vm, local.asList()->elems[i], out, budget))
                return false;
        }
        return true;
    }
    case Tag::Attrs: {
        Bindings * b = local.asAttrs();
        uint32_t n = b ? b->size : 0;
        out.push_back('{'); out.append(reinterpret_cast<const char *>(&n), 4);
        const auto & symTab = ir::globalSymbolTable();
        for (uint32_t i = 0; i < n; ++i) {
            Bindings * bb = local.asAttrs();   // re-read (relocation-safe)
            const uint32_t sym = bb->entries[i].name;
            if (sym >= symTab.size()) return false;   // defensive: unknown symbol
            const std::string & nm = symTab[sym];
            uint32_t sn = (uint32_t)nm.size();
            out.append(reinterpret_cast<const char *>(&sn), 4);
            out += nm;
            if (!appliedProbeBoundedKey(vm, local.asAttrs()->entries[i].value, out, budget))
                return false;
        }
        return true;
    }
    case Tag::Uninitialized:
    case Tag::Closure:
    case Tag::Thunk:
    case Tag::PrimOp:
    case Tag::PrimOpApp:
    case Tag::App:
    case Tag::App3:
    case Tag::Slot:
    case Tag::External:
    case Tag::Blackhole:
    default:
        return false;   // closure / PAP / primop / external / … ⇒ uncacheable
    }
}

/// LEVER-1 applied-import cache — the REAL memo key (NIX_V3_APPLIED_CACHE=1).
/// NON-FORCING: value_serialize::canonicalHash chases only already-Evaluated
/// indirections and throws on any Suspended thunk / Closure — which is exactly
/// the structural filter that rejects the callPackage-class computed-args
/// flood (~7.4K/eval, probe-measured) in nanoseconds while accepting WHNF
/// const args (e.g. the vEmptyAttrs `{}` of `import <nixpkgs> {}`).
/// KEY = callee LambdaDescriptor pointer + canonical args digest.  The DESC
/// (not the CU!) is the identity: the first acceptance run keyed on the CU
/// and produced a WRONG drvPath — `fromImportCU` marks EVERY closure defined
/// in an imported file, and DIFFERENT closures sharing one CU with `{}` args
/// collided (126 lookups / 85 bogus hits on a hello eval).  With callers
/// restricted to nUpvalues==0 && capturedWiths==nullptr, the desc is the
/// COMPLETE behavioral identity (no captured state) ⇒ desc+args is sound.
/// (In-memory tier; the persistent tier uses content keys.)
/// Non-throwing structural pre-check: returns true iff canonicalHash(v)
/// would (very likely) SUCCEED.  Exact mirror of value_serialize's
/// chaseToWHNF + serializeOne acceptance — Int/Float/Bool/Null/String/Path
/// leaves, List/Attrs containers, Evaluated-indirection chasing (Thunk/App/
/// App3/Slot), same depth bound.  WHY (task #16c, gate git-note e156a9874):
/// ~370 of ~380 tryKey attempts per hello eval are UNHASHABLE, and each
/// paid a partial serialize (allocation + name-sort per attrs node) plus a
/// thrown SerializeError — the dominant share of the +60ms (+9.7%) eval#1
/// cache tax.  The pre-check bails on the first non-WHNF node with zero
/// allocation and zero exceptions.  Conservative-false only loses a
/// would-be hit; the try/catch backstop below stays (keyExceptionBail
/// counts how often the mirror is WRONG — expected 0, regression-tested).
static bool appliedKeyPrecheck(const Value & vIn, int depth) noexcept
{
    if (depth > 10000) return false;  // kMaxSerializeDepth mirror (cycles)
    const Value * cur = &vIn;
    for (int hop = 0; hop < 32; ++hop) {  // chaseToWHNF maxHops mirror
        Tag t = cur->tag();
        if (t == Tag::Thunk) {
            Thunk * th = cur->asThunk();
            if (!th || th->state != ThunkState::Evaluated) return false;
            cur = &th->evaluated;
            continue;
        }
        if (t == Tag::App || t == Tag::App3) {
            ValuePair * p = cur->asPair();
            if (!p || p->evaluated.tag() == Tag::Uninitialized) return false;
            cur = &p->evaluated;
            continue;
        }
        if (t == Tag::Slot) {
            if (!cur->asSlot()) return false;
            cur = cur->asSlot();
            continue;
        }
        // WHNF — accept exactly serializeOne's tag set.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wswitch-enum"
        switch (t) {
        case Tag::Int:
        case Tag::Float:
        case Tag::Bool:
        case Tag::Null:
        case Tag::String:
        case Tag::Path:
            return true;
        case Tag::List: {
            const ListVec * lv = cur->asList();
            if (!lv) return true;  // empty list serialises fine
            for (uint32_t i = 0; i < lv->size; ++i)
                if (!appliedKeyPrecheck(lv->elems[i], depth + 1)) return false;
            return true;
        }
        case Tag::Attrs: {
            const Bindings * b = cur->asAttrs();
            if (!b) return true;
            bool ok = true;
            // forEach (not forEachName) to MATCH serializeAttrs — it
            // realizes MapAttrs lazy entries exactly like serialize would.
            b->forEach([&](const Bindings::Entry & e) {
                if (ok && !appliedKeyPrecheck(e.value, depth + 1)) ok = false;
            });
            return ok;
        }
        default:
            return false;  // Closure / PrimOp / ... — serialize throws
        }
#pragma GCC diagnostic pop
    }
    return false;  // chase chain exceeded max hops
}

/// SHADOW lockstep compare (#16a): structural equality of the freshly
/// computed result vs the cached entry, comparing ONLY nodes that are
/// already WHNF on BOTH sides — Suspended/unevaluated subtrees are SKIPPED
/// (never forced; forcing would perturb the eval being validated).  Chases
/// Evaluated indirections like value_serialize::chaseToWHNF.  Returns false
/// ONLY on a definite structural mismatch of WHNF-vs-WHNF nodes.
static bool appliedShadowCompareOne(const Value & aIn, const Value & bIn,
                                    int depth, uint64_t & compared) noexcept
{
    if (depth > 512 || compared > 2'000'000) return true;  // bounded: treat as unknown
    // chase both sides to WHNF; bail (skip) if either side is not there yet
    auto chase = [](const Value & vIn) -> const Value * {
        const Value * cur = &vIn;
        for (int hop = 0; hop < 32; ++hop) {
            Tag t = cur->tag();
            if (t == Tag::Thunk) {
                Thunk * th = cur->asThunk();
                if (!th || th->state != ThunkState::Evaluated) return nullptr;
                cur = &th->evaluated;
                continue;
            }
            if (t == Tag::App || t == Tag::App3) {
                ValuePair * p = cur->asPair();
                if (!p || p->evaluated.tag() == Tag::Uninitialized) return nullptr;
                cur = &p->evaluated;
                continue;
            }
            if (t == Tag::Slot) {
                if (!cur->asSlot()) return nullptr;
                cur = cur->asSlot();
                continue;
            }
            return cur;
        }
        return nullptr;
    };
    const Value * a = chase(aIn);
    const Value * b = chase(bIn);
    if (!a || !b) return true;  // one side not WHNF — skip subtree
    ++compared;
    Tag ta = a->tag(), tb = b->tag();
    if (ta != tb) return false;
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wswitch-enum"
    switch (ta) {
    case Tag::Int:   return a->asInt() == b->asInt();
    case Tag::Float: return a->floatBits() == b->floatBits();
    case Tag::Bool:  return a->asInt() == b->asInt();
    case Tag::Null:  return true;
    case Tag::String: {
        const char * sa = a->asString(); const char * sb = b->asString();
        if (!sa || !sb) return sa == sb;
        return std::strcmp(sa, sb) == 0;
    }
    case Tag::Path: {
        const char * pa = a->asPath(); const char * pb = b->asPath();
        if (!pa || !pb) return pa == pb;
        return std::strcmp(pa, pb) == 0;
    }
    case Tag::List: {
        const ListVec * la = a->asList(); const ListVec * lb = b->asList();
        uint32_t na = la ? la->size : 0, nb = lb ? lb->size : 0;
        if (na != nb) return false;
        for (uint32_t i = 0; i < na; ++i)
            if (!appliedShadowCompareOne(la->elems[i], lb->elems[i],
                                         depth + 1, compared)) return false;
        return true;
    }
    case Tag::Attrs: {
        const Bindings * ba = a->asAttrs(); const Bindings * bb = b->asAttrs();
        uint32_t na = ba ? ba->countDistinct() : 0;
        uint32_t nb = bb ? bb->countDistinct() : 0;
        if (na != nb) return false;
        if (!ba || !bb) return true;
        // Same construction path ⇒ same ascending-SymbolId iteration order.
        bool ok = true;
        std::vector<Bindings::Entry> ea, eb;
        ea.reserve(na); eb.reserve(nb);
        ba->forEach([&](const Bindings::Entry & e) { ea.push_back(e); });
        bb->forEach([&](const Bindings::Entry & e) { eb.push_back(e); });
        if (ea.size() != eb.size()) return false;
        for (size_t i = 0; i < ea.size() && ok; ++i) {
            if (ea[i].name != eb[i].name) { ok = false; break; }
            if (!appliedShadowCompareOne(ea[i].value, eb[i].value,
                                         depth + 1, compared)) ok = false;
        }
        return ok;
    }
    default:
        // Closures / functions / external: identity not comparable
        // structurally without forcing — skip (count as compared).
        return true;
    }
#pragma GCC diagnostic pop
}
} // anonymous namespace — applied-cache file-local key/probe/shadow helpers

bool appliedCacheTryKey(const Closure * callee, const Value & arg, std::string & out)
{
    if (!appliedKeyPrecheck(arg, 0)) {
        static const bool s_dbgPre = std::getenv("V3_DBG_APPLIED") != nullptr;
        if (__builtin_expect(s_dbgPre, 0) && callee->desc)
            std::fprintf(stderr, "APPLIED tryKey PRECHECK-BAIL desc=%s argTag=%d\n",
                callee->desc->name.empty() ? "<anon>" : callee->desc->name.c_str(),
                (int)arg.tag());
        appliedCacheNoteTryKey(false);
        return false;   // unhashable ⇒ uncacheable (never force here)
    }
    uint8_t digest[32];
    try {
        value_serialize::canonicalHash(arg, digest);
    } catch (const std::exception & e) {
        // BACKSTOP (should be dead post-pre-check): counts mirror drift.
        appliedCacheNoteTryKeyException();
        static const bool s_dbg = std::getenv("V3_DBG_APPLIED") != nullptr;
        if (__builtin_expect(s_dbg, 0) && callee->desc)
            std::fprintf(stderr, "APPLIED tryKey UNHASHABLE desc=%s argTag=%d sz=%d why=%s\n",
                callee->desc->name.empty() ? "<anon>" : callee->desc->name.c_str(),
                (int)arg.tag(),
                arg.isAttrs() && arg.asAttrs() ? (int)arg.asAttrs()->size : -1,
                e.what());
        appliedCacheNoteTryKey(false);
        return false;   // unhashable ⇒ uncacheable (never force here)
    } catch (...) {
        appliedCacheNoteTryKeyException();
        appliedCacheNoteTryKey(false);
        return false;
    }
    appliedCacheNoteTryKey(true);
    static const bool s_dbg = std::getenv("V3_DBG_APPLIED") != nullptr;
    if (__builtin_expect(s_dbg, 0) && callee->desc)
        std::fprintf(stderr, "APPLIED tryKey OK desc=%s argTag=%d\n",
            callee->desc->name.empty() ? "<anon>" : callee->desc->name.c_str(),
            (int)arg.tag());
    char pbuf[2 * sizeof(void *) + 4];
    std::snprintf(pbuf, sizeof pbuf, "%p:", (const void *)callee->desc);
    out = pbuf;
    static const char hexd[] = "0123456789abcdef";
    for (int i = 0; i < 32; ++i) {
        out += hexd[digest[i] >> 4];
        out += hexd[digest[i] & 0xF];
    }
    return true;
}

/// Gate for the REAL cache ("1") or SHADOW validation ("shadow");
/// probe/count are the measurement modes.  In shadow mode the hooks arm and
/// insert exactly like "1" but a would-HIT never short-circuits: the
/// application evaluates normally and OP_RETURN lockstep-compares the fresh
/// result against the cached entry (appliedShadowCompare).  Retirement:
/// shadow is the #16a validation instrument — retire (fold into "1") once
/// the exit bar (0 mismatches on hello/firefox/HNE) has been recorded.
bool appliedCacheOn() noexcept
{
    // DEFAULT-ON (#1.2, 2026-07-05): the applied-import result cache is now
    // active unless explicitly disabled.  Gate the win into production after
    // the #1.0 overhead gate (single-eval Δcpu ≤0.3%, RSS flat across LRU caps)
    // + #1.1 impurity/taint lock (T10/T11) + a full nixpkgs byte-eq sweep.
    // Retirement of the OPT-OUT: drop it (hard-true) once the cache has soaked
    // in production; retirement of the whole gate is not planned (it stays as
    // the emergency kill).  POLARITY (careful — probe/count are measurement-
    // only and must NOT enable the real cache):
    //   unset          → ON   (production default)
    //   "0" / "off"    → OFF  (opt-out / A-B baseline / emergency kill)
    //   "probe"/"count"→ OFF  (measurement modes; the probe hooks run separately)
    //   "shadow"       → ON   (compare-not-reuse; appliedCacheShadowMode gates it)
    //   "1" / other    → ON   (back-compat with the pre-flip explicit enable)
    static const bool v = [] {
        const char * e = std::getenv("NIX_V3_APPLIED_CACHE");
        if (!e) return true;                              // default ON
        if (std::strcmp(e, "0") == 0 || std::strcmp(e, "off") == 0)
            return false;                                 // explicit opt-out
        if (std::strcmp(e, "probe") == 0 || std::strcmp(e, "count") == 0)
            return false;                                 // measurement-only
        return true;                                      // "1"/"shadow"/other → ON
    }();
    return v;
}

/// True iff NIX_V3_APPLIED_CACHE=shadow (compare-not-reuse).
bool appliedCacheShadowMode() noexcept
{
    static const bool v = [] {
        const char * e = std::getenv("NIX_V3_APPLIED_CACHE");
        return e && std::strcmp(e, "shadow") == 0;
    }();
    return v;
}

/// Entry point used by OP_RETURN in shadow mode.
void appliedShadowCompare(const std::string & key, const Value & fresh) noexcept
{
    Value cached;
    if (!appliedCacheLookupPeek(key, cached)) return;  // evicted — nothing to compare
    uint64_t compared = 0;
    bool ok = appliedShadowCompareOne(fresh, cached, 0, compared);
    appliedCacheNoteShadowCompare(ok, compared);
    if (!ok)
        std::fprintf(stderr,
            "v3 APPLIED-CACHE SHADOW MISMATCH key=%s (compared=%llu WHNF nodes)\n",
            key.c_str(), (unsigned long long)compared);
}

void appliedCacheProbeObserve(VMState & vm, const Closure * callee, const Value & arg,
                              int site, bool hasFormals) noexcept
{
    auto & st = appliedCacheProbeStats();
    st.eligibleCalls++;
    if (site >= 0 && site < 3) st.bySite[site]++;
    if (!hasFormals) { st.noFormals++; return; }  // formals-only cacheable (v1 rule)
    // NIX_V3_APPLIED_CACHE=count → eligibility counters ONLY, no key
    // computation.  The bounded-forcing key (probe mode) perturbs real evals
    // (every callPackage is an eligible import-CU application; forcing even a
    // bounded prefix of its args cascades — observed NixOS-module warnings in
    // a hello eval).  count-mode quantifies the flood non-invasively.
    static const bool s_countOnly = [] {
        const char * e = std::getenv("NIX_V3_APPLIED_CACHE");
        return e && std::strcmp(e, "count") == 0;
    }();
    if (s_countOnly) return;
    std::string k;
    k.reserve(160);
    char pbuf[2 * sizeof(void *) + 4];
    std::snprintf(pbuf, sizeof pbuf, "%p:", (const void *)closureCU(callee));
    k += pbuf;
    int budget = kAppliedKeyBudget;
    bool ok = false;
    try {
        ok = appliedProbeBoundedKey(vm, arg, k, budget);
    } catch (...) {
        ok = false;   // eval throw during bounded forcing ⇒ uncacheable
    }
    if (!ok) { st.unhashableArgs++; return; }
    st.hashedCalls++;
    if (!st.keys.insert(std::move(k)).second) st.wouldHit++;
}


// LEVER-1 applied-import cache PROBE — cumulative-counter dump, called from
// run.cc at end-of-root-eval (see the primop.hh declaration for why not
// atexit).  Monotonic — the LAST line printed in a process is authoritative.
// External-linkage (declared in v3/primop.hh); it reads the anonymous-namespace
// probe counters above (appliedCacheProbeStats / AppliedCacheProbeStats) —
// same-TU access, which is why it lives here with them.
void dumpAppliedCacheProbeStats() noexcept
{
    static const bool s_probe = [] {
        const char * e = std::getenv("NIX_V3_APPLIED_CACHE");
        return e && (std::strcmp(e, "probe") == 0 || std::strcmp(e, "count") == 0);
    }();
    if (!s_probe) return;
    const auto & st = appliedCacheProbeStats();
    if (st.eligibleCalls == 0) return;
    std::fprintf(stderr,
        "v3 APPLIED-CACHE PROBE: eligible=%llu (call=%llu tail=%llu cc=%llu "
        "noFormals=%llu) hashed=%llu unhashable=%llu distinctKeys=%zu wouldHit=%llu\n",
        (unsigned long long)st.eligibleCalls,
        (unsigned long long)st.bySite[0],
        (unsigned long long)st.bySite[1],
        (unsigned long long)st.bySite[2],
        (unsigned long long)st.noFormals,
        (unsigned long long)st.hashedCalls,
        (unsigned long long)st.unhashableArgs,
        st.keys.size(),
        (unsigned long long)st.wouldHit);
}
} // namespace nix::v3
