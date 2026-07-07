# Persistent cross-process result store — Phase-0 (measure-first) + design (2026-07-07)

Part C of the LEVER-1 "productionize the moat" plan.  Supersedes the forward-looking
sections of `RESULT_STORE_DESIGN_2026-07-04.md` with **measured** Rule-0 gate numbers
(this doc's §1) and re-scopes the build around what the current binary already ships.
Grounded at HEAD `b500141b7`.  Measured on the laptop (host-independent counters only;
NO CPU/RSS perf claims — those are darwin-4-only).

Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.
SPDX-License-Identifier: Apache-2.0.

---

## 0. TL;DR — the go/no-go

**The cross-process PURE payoff is NOT big enough to justify building the store standalone
today, because the addressable surface is blocked by two orthogonal walls that a store does
not remove:**

1. **The serialize wall (leaf-only).** The shipped WHNF serializer (`value_serialize`)
   round-trips ONLY a fully-forced value graph (Int/Float/Bool/Null/String+ctx/Path/List/
   Attrs-of-WHNF).  It **throws on the first unforced Thunk or any Closure** (measured §1.3).
   So a *leaf projection* (`.drvPath`/`.name` = a WHNF string) is serializable TODAY, but a
   *package-set attrset* or even a raw `derivation {…}` result is NOT (unforced `drvPath`/
   `outPath` thunks).  The store can persist leaf-projection results now; anything coarser
   needs the lazy-graph serializer (the hard core, still unbuilt).

2. **The taint wall (the A5 soundness wall — dominates in practice).** Under `--impure`,
   EVERY `<nixpkgs>`-touching eval is `tainted` (measured §1.2): the `<nixpkgs>` search-path
   lookup itself bumps `TAINT_READFILE` (`primFindFile`, primops.cc:3152), a **hard-reject**
   axis — so hello.name / hello.drvPath / any pkg leaf is rejected before serialize is even
   attempted.  Under `--pure-eval` (where the taint model is sound), `<nixpkgs>` search-paths
   AND absolute-path imports are **both forbidden** (measured §1.2) → the only route to pinned
   nixpkgs is a flake, which is `TAINT_GETFLAKE` → demotable-iff-keyed (A3/A4).  So the top-
   level whole-eval cache reaches a real pkg workload ONLY through the flake+pure-eval+A3/A4
   path — which is exactly what A1 already shipped-and-DEFERRED.

The honest read: **the store is the right SHIP VEHICLE, but it is downstream of soundness
work already scoped (A1 SHIP=DEFER-to-A3, IFD phase-3 build-boundary key), not a standalone
win.**  The smallest first slice that ships value (§7 v0-slice) is a persistent tier for the
**leaf-projection results that are already serializable AND already have a sound key** — i.e.
promote the existing in-memory applied-import cache + the drvHash `EvalResults` cache to a
first-class, generalized, LRU-evicting, signable cross-process/cross-machine store, WITHOUT
attempting the lazy-graph serializer or the top-level whole-eval cache.  That slice is mostly
plumbing on top of substrate that already exists (§2) and is the only part with a clean
soundness story today.  The bigger payoff (whole-eval / package-set caching) is GATED on
A3/A4 (pure payoff) and IFD phase-3 (the real M5/HNE payoff) and should NOT be built first.

---

## 1. RULE-0 GATE — the measured addressable share (host-independent counters, HEAD b500141b7)

Binary: `build/src/libexpr-v3/v3-eval` (built 2026-07-07) for `<nixpkgs>` workloads;
`build/src/nix/nix` with `NIX_USE_V3=1` for the getFlake workloads (v3-eval does NOT wire
flake settings — a methodology note: the top-level cache shadow classifier is reached ONLY
via the `nix` CLI's `runRootExprFromString`, NOT via `v3-eval --expr`, whose user result
bypasses it; all top-level numbers below are the `nix` CLI).  Local nixpkgs
`/Users/angerman/Projects/zw3rk/nixpkgs` (24.05, hello-2.12.1).  Flags per the plan:
`--no-eval-cache --impure --option allow-import-from-derivation true`,
`NIX_V3_NO_NATIVE_CALL_FLAKE=1` for flake workloads.

### 1.1 Env gates verified present in the binary (before relying on them)

- `NIX_V3_APPLIED_CACHE` = `probe`|`count`|`shadow`|`1`|`off` (vm.cc:4149; default ON).
  `count` = eligibility counters only (non-invasive); `probe` = bounded-force key
  (EXPLODES on nixpkgs — >2 min on hello.name, per the vm.cc:3901 warning, do NOT use for
  scale); default `1` = the real non-forcing key (`keyAttempts`/`keyUnhashable`/`inserts`).
- `NIX_V3_IFD_PROV_CACHE` = `shadow`|`active`|`off` (primops.cc:6964).  Stats:
  `inserts / wouldHits / mismatchHits / activeServes / poisonSkips`.
- `NIX_V3_TOPLEVEL_CACHE` = `shadow`|`active` (default OFF; run.cc:1823).  Stats:
  `evals / serializable / unserializable / tainted / lookups / hits / mismatch / inserts`.
  This is the cleanest whole-eval addressable-share probe (it classifies the outermost
  result directly).  `V3_DBG_TOPLEVEL=1` prints the per-eval tag + unserializable reason.
- `NIX_V3_MEM_BUCKETS=1` + `NIX_V3_LIVE_TRACE=1` + `NIX_VM_STATS=1` → retained-graph MB.

### 1.2 Whole-eval top-level classification (the cross-process whole-result share)

The A1/A5 taint gate fires BEFORE serialize, so on real pkg workloads the outermost result
is classified `tainted` and we never observe its serializability.  Decomposed:

| eval (nix CLI, `--impure`)                    | tag | serializable | unserializable | tainted | insertable |
|-----------------------------------------------|-----|:---:|:---:|:---:|:---:|
| `(import <nixpkgs> {}).hello.name`            | String | 0 | 0 | **1** | NO |
| `(import <nixpkgs> {}).hello.drvPath`         | String | 0 | 0 | **1** | NO |
| `builtins.typeOf (import <nixpkgs> {})`       | String | 0 | 0 | **1** | NO |

**Root cause (measured):** the `<nixpkgs>` search-path lookup bumps `TAINT_READFILE`
(`primFindFile`, primops.cc:3152) — a hard-reject axis (NOT perturbable, NOT manifest-
recoverable).  So `--impure` + `<nixpkgs>` ⇒ hard reject for the WHOLE-eval cache,
independent of currentTime and independent of serializability.

`--pure-eval` (the only mode where the taint model is sound) FORBIDS the route entirely
(both measured):
- `cannot look up '<nixpkgs>' in pure evaluation mode`
- `access to absolute path '…/nixpkgs' is forbidden in pure evaluation mode`

⇒ Under pure-eval the ONLY route to pinned nixpkgs is a **flake** → `TAINT_GETFLAKE` →
demotable-iff-keyed (A3/A4).  This is the A1 SHIP=DEFER-to-A3 wall, re-confirmed by direct
measurement.  **The whole-eval cross-process cache reaches a real pkg workload only after
A3/A4 land AND under pure-eval + flake phrasing.**

Control (pure WHNF top-level results, NO nixpkgs → no taint) — all INSERT cleanly, proving
the machinery works when the taint+serialize walls are absent:
`1+2`, `"hello world"`, `[1 2 3]`, `{a=1;b="x";c=[1 2];}`, `builtins.tryEval (throw "x")`
→ **serializable=1, inserts=1**.  `builtins.currentTime` / `getEnv` (--impure) →
**tainted=1** (correctly rejected).  `(x: x)` → **unserializable=1** (no WHNF).

### 1.3 Serializable-result fraction — the serialize boundary (NO nixpkgs → taint isolated)

| top-level result                                  | serializable | reason if not |
|---------------------------------------------------|:---:|---|
| `{a=1;b="s";c=[1 2 3];d={e=4;};}` (all WHNF)       | **1** | — |
| `{a={b={c={d=[1 2 [3 4]];};};};}` (deep WHNF)      | **1** | — |
| `{a=1;b=(let x=2; in x+3);}` (one unforced thunk)  | 0 | `Thunk not Evaluated; cannot canonical-hash` |
| `let f=x:x+1; in {a=1;b=f;}` (a function attr)     | 0 | `Thunk not Evaluated` (b thunk) |
| `{a=1;f=(x:x);}` (a closure attr)                  | 0 | `unsupported tag in serialise: 9` (Closure) |
| `[1 (x:x) 3]`                                      | 0 | `unsupported tag in serialise: 9` (Closure) |
| `derivation {name="t";system="x";builder="b";}`    | 0 | `Thunk not Evaluated` (lazy drvPath/outPath) |

**Boundary, exactly:** a value graph serializes TODAY **iff it is fully deep-forced WHNF**
(scalars/strings+ctx/paths/lists/attrs, recursively).  The first unforced `Thunk` or any
`Closure`/function throws.  ⇒ **leaf projections (`.drvPath`, `.name`, `.outPath`) are
serializable strings TODAY**; **package-set attrsets and raw `derivation {}` results are NOT**
(they carry unforced thunks).  This is the exact demarcation between "ships now" (leaf) and
"needs the lazy-graph serializer" (coarse).

### 1.4 Applied-import cache — the real (non-forcing) key stats (default-on path)

| eval (nix CLI, `--impure`)         | keyAttempts | keyUnhashable | inserts | hashable % |
|------------------------------------|:---:|:---:|:---:|:---:|
| `(import <nixpkgs> {}).hello.name`    | 349  | 347  | 2 | 0.6% |
| `(import <nixpkgs> {}).hello.drvPath` | 1031 | 1029 | 2 | 0.2% |

The SHIPPED applied cache is *very* narrow: it keys only `fromImportCU && hasFormals &&
arity≤1 && capturedWiths==nullptr && nUpvalues==0` applications with a serializable arg
(vm.cc:7056-7060).  ~99.7% of `(import f) args` applications have computed/closure args or
captured state → `keyUnhashable`.  Net: **~2 serializable applied-import entries per eval.**
Cross-process value of persisting THOSE 2 entries is real but tiny per eval — the win is
across N≥3 repeated evals of the same (f, args), which is the moat thesis (in-process it's
default-on; cross-process needs the store).

### 1.5 Eligibility scale (count mode — non-invasive) and the getFlake workloads

| workload                         | eligible import-CU apps | noFormals | formals-eligible | notes |
|----------------------------------|:---:|:---:|:---:|---|
| hello.name (`<nixpkgs>`, v3-eval) | 12,021  | 10,844  | ~1,177  | complete |
| firefox.drvPath (v3-eval)        | 16,555  | 15,135  | ~1,420  | pkg unavailable on darwin (firefox-128 not on aarch64-darwin) — eval aborts at the leaf but the count dump is authoritative for eligibility |
| HNE.drvPath (getFlake, nix CLI)  | 333,129 | 309,689 | ~23,440 | **partial** — hit the 10-min wall mid-IFD-build under heavy laptop contention (this is an IFD-build-bound workload, not an eval-bound one) |
| M5.name (getFlake, nix CLI)      | _(pending; IFD build under contention)_ | | | see §1.7 |

The formals-eligible count is the *upper bound* on applied-import cacheable applications; the
serializable subset of THOSE is a further ~0.2-0.6% (§1.4 keyUnhashable), because the eligible
applications overwhelmingly take computed/closure args (callPackage-class).

### 1.6 Retained-graph footprint (the store's per-entry cost) — hello.drvPath, v3-eval

`NIX_VM_STATS + NIX_V3_MEM_BUCKETS + NIX_V3_LIVE_TRACE`:
- **arena total allocated 128 MB** (churn); **arena_pinned (live at end) 16.8 MB**; peak_rss 30 MB.
- thunks allocated 1,372,137 / forced 443,117 → **force ratio 0.323** (only 32% of thunks ever
  force; 68% are dead weight — matches C2_JIT_RCA's 65.7% unforced).
- closures 112,398, attrsets 303,447 (43.7% are size-2), lists 140,717.
- The RESULT of hello.drvPath is a single ~44-byte string.

**Interpretation for the store:** a *leaf-projection* store entry is ~50 bytes (the string +
context).  A hypothetical *whole-eval* store entry (the lazy graph) is ~17 MB LIVE at peak —
and 68% of it (the unforced thunks) is dead weight that a lazy-graph store would fault in only
to never force.  This is the RSS argument for the lazy (`Tag::External`-faulting) design over
eager materialization, and it is why leaf-first (§7) is the cheap, RSS-safe slice.

### 1.7 IFD-fragment share + poison (the M5/HNE portion) — cited, not re-derived

The IFD-prov shadow numbers on M5/HNE are darwin-4 authoritative (IFD_PROVENANCE_CACHE_SPEC
Phase-2 verdict, git-noted): on the real IFD workloads **shadow inserts fire, wouldHits==inserts
(100% cross-process recall), poisonSkips==0, mismatchHits==0** → the v2 IFD key is SOUND and
FIRES.  Re-deriving them on the laptop is not viable under CPU contention (HNE hit a 10-min wall
mid-build; the IFD *build* dominates, not the eval).  The load-bearing Phase-2 finding stands:
the v2 IFD key is **POST-EVAL** (built from transitive content-ids that exist only AFTER the
fragment's reads fire) → an active HIT cannot skip the fragment body, only replace its result →
`T_hit/T_eval` 0.41 (HNE) / 0.61 (M5, FAIL) → **KILL-active-to-shadow**.  So the M5/HNE
IFD-fragment portion is NOT cross-process-addressable by any post-eval-keyed store; it is
GATED on the **phase-3 build-boundary narHash key** (fold the realised IFD output narHash,
known right after `realisePath`, into a PRE-eval key that CAN skip the body).

### 1.8 Per-workload addressable-share summary (the Rule-0 table)

| workload | (a) cache-addressable share | (b) pure vs IFD split | (c) serializable-result fraction | (d) retained-graph MB |
|---|---|---|---|---|
| **hello.drvPath / .name** (pure-import proxy) | Whole-eval: **0% under --impure** (TAINT_READFILE hard-reject via `<nixpkgs>`); gated on A3/A4+pure-eval+flake. Applied-import: **~2 entries/eval** (0.2-0.6% of ~350-1030 key attempts). | 100% pure (no IFD). | Leaf `.drvPath`/`.name` = **100% serializable** (WHNF string). Whole pkg attrset = **0%** (unforced thunks). | live ~17 MB; result string ~50 B. |
| **HNE.drvPath** | Whole-eval: 0% under --impure (getFlake → TAINT_GETFLAKE; needs A4-keyed + pure-eval). IFD-fragment: sound+fires in shadow but **NOT body-skipping** (post-eval key) → 0% net until phase-3. | **IFD-heavy** (haskell.nix materialization; the wall time is the *build*, not eval). | leaf `.drvPath` serializable; the intermediate IFD import results are attrsets/thunks → need lazy-graph or phase-3 build-boundary key. | not re-measured under contention; darwin-4 arena ~453 MB (reference_x86_64). |
| **M5.name** | Whole-eval: getFlake → GETFLAKE, A4-keyed cross-proc byte-id works (memory: A4 3a750eb46) but the leaf `.name` still hits `.drvPath` REJECTS on nixpkgs readFile in the transitive graph (A5-fix). IFD: same post-eval-key wall as HNE. | IFD-heavy. | leaf `.name` serializable string; transitive IFD graph not. | darwin-4 arena ~1543 MB (reference_x86_64). |

---

## 2. SUBSTRATE INVENTORY — what already exists (the build is mostly plumbing on this)

The persistent cross-process store is **~80% already built** as disjoint pieces; the design is
generalization + LRU/eviction + signing, NOT green-field:

1. **`value_serialize` (WHNF serialize/deserialize + `canonicalHash`)** — validated 0-mismatch
   (#741 shadow), deterministic (sorted attrs, sorted string-context), endian-explicit LE.
   THROWS on Thunk/Closure (§1.3 the leaf-only boundary).  This is the entry format for a
   leaf/WHNF store entry TODAY.
2. **`disk_cache::{lookup,insert}EvalResult`** — a SQLite `EvalResults` table (schema
   `kEvalResultSchemaVersion=1`), WAL + `synchronous=OFF` batched (`begin/commitEvalResultBatch`),
   INSERT-OR-IGNORE race-safe, best-effort (never source of truth).  **This IS a cross-process
   persistent key→blob store already** — the top-level cache (A1) and drvHashCache use it.
3. **`aot_cache` (mmap flat-file, L3 read-only snapshot)** — `V3AOTC01`, sorted 56-byte entry
   table, `MAP_PRIVATE|PROT_READ`, MADV-split (WILLNEED entries / NORMAL blobs).  **Already has
   `TBL_EVAL_RESULT` (table_id=2)** alongside `TBL_CU` — the cross-machine distributed snapshot
   tier for eval results already exists as a format + reader; `bench/build-aot-cache.py` emits it.
4. **The sound key discipline (A1/A3/A4)** — `topLevelCacheKey` (run.cc:2143) is ALREADY
   `SHA256(v3-toplevel-v7 ‖ codegenGateFingerprint ‖ manifestContentHash ‖ keyBody)` where
   keyBody = `schema ‖ system ‖ resolvedNixPathContentIds ‖ basePath ‖ source ‖ flakeLockStr`.
   This is precisely the store's binding-key discipline the plan asks for — already implemented,
   already versioned (v1→v7 = the soundness-evolution ledger), already fail-closed.
5. **The taint mask (A5-fix)** — `topLevelTaintMask()` per-axis (GETENV/CURRENTTIME/READFILE/
   FETCH/STORE/GETFLAKE), every ambient read routed through a bumping primop (the enumerable
   completeness obligation, discharged).  This is the store's "no persistent entry when impure"
   gate, already sound.
6. **The IFD provenance accumulator (Phase-1 shadow, shipped)** — `provFramePush` + per-input
   contentId fold + poison — the sound foundation for the phase-3 build-boundary key.
7. **`codegenGateFingerprint()` / `serialize::opcodeTableFingerprint()`** — a codegen change
   invalidates the store for free (already folded into every key).

**What is genuinely missing (the build):**
- **A. Generalized cross-process serve for leaf/WHNF applied-import + drvHash results** with
  proper LRU/eviction, a two-layer `input_key→result_hash→blob` content-address (verify-don't-
  trust), and best-effort semantics.  (Substrate 1+2+4+5 exist; this is plumbing + a schema.)
- **B. The lazy-graph serializer (V3RG)** — serialize a NON-WHNF graph (unforced thunks by
  `(cuContentKey, funcIdx, upvalues, withs, defEnv)`, cycles, three thunk-tail variants,
  Env-sharing) + relink.  **The hard, novel core; no in-tree prior art.**  Gated behind its own
  falsifier (§7 v2).
- **C. Cross-machine sync + ed25519 signing + narinfo-style distribution + snapshot LRU/compaction.**

---

## 3. DESIGN — leaf-first, then lazy-graph (phased around the falsifier)

### 3.1 What the user's `.drvPath`/`.name` workloads need (leaf, TODAY)

A leaf projection deep-forces to a WHNF string (with store-path context).  `value_serialize`
round-trips it 0-mismatch.  So the leaf-projection tier needs ONLY:
- the sound binding key of §2.4 (already built), extended to the applied-import grain
  `(codegenFingerprint ‖ system ‖ storePathOrFlakeLockPinOf(f) ‖ argsHash)` where `argsHash =
  canonicalHash(deep-forced args)` — but with the HARD RULE that the ENTIRE frame is untainted
  (`topLevelTaintMask()==0`, or only perturbable-and-manifest-blessed under pure-eval).  This is
  the applied-cache generalization of the already-shipped top-level gate.
- the `EvalResults` SQLite tier (already built) + the AOT L3 snapshot (already built) as the
  storage; add a two-layer `input_key→result_hash→blob` + a `taint`/`size`/`last_used` column
  for LRU (§3.3).

**This is the v0-slice (§7): no lazy graph, no new serializer, sound key already exists.**  It
persists exactly the results that are serializable AND untainted TODAY — leaf projections and
WHNF applied-import results.

### 3.2 Thunk/Closure serialization (V3RG — the hard core, DEFERRED behind its falsifier)

To cache a *package-set* result (a mostly-unforced attrset) we must serialize the lazy graph.
Encoding (`V3RG`, offset-addressed node table, relocatable — no absolute pointers, only
node-indices + CU content-keys):
- WHNF leaves reuse the V3VR bodies verbatim.
- `'T' Thunk`: `cuContentKey(32) ‖ funcIdx(u32) ‖ flags(u8) ‖ nUpvalues ‖ nodeIdx*n ‖
  [withsNodeIdx] ‖ [defEnvNodeIdx]` — the thunk names its CODE by the content-hash of its
  owning CU (= the `disk_cache::CacheKey` that keys the bytecode cache) + a funcIdx; upvalues/
  withs/defEnv are recursive child nodes.  Must round-trip all THREE physical thunk tails
  (inline-FAM / ENV_SHARED / ENV_CAPTURE, closure.hh:263-293).
- `'C' Closure`, `'E' Env`, `'V' evaluated-indirection`, cycles via u32 back-refs (letrec
  knots), sharing via `IdentityMap<void*,u32>` (DAG stays a DAG).
- **Relink (not "load")**: 2-pass — alloc all shells uninitialized, resolve cuContentKey via
  `disk_cache.lookup ?: aot_cache.lookup ?: recompile-from-source`, then wire child pointers
  firing Phase-D write barriers (UAF-critical into a live moving heap).
- **Lazy materialization (v2 RSS win)**: a relinked entry pointing at an unforced subgraph
  gets a `Tag::External` faulting node `(mmapSegment*, nodeOffset)`; `forceValue` faults in
  just that node.  RSS ≈ demanded working set, not graph size (the 68% never-forced thunks
  from §1.6 stay as evictable page-cache).  This is the GC-critical build (faulting nodes are
  roots-that-allocate — the PhD-6 UAF class; its gate is the full `--brute` under stress).

**Scope it as its own phase (v2) with its own pre-committed falsifier (§7).**  It is only worth
building AFTER the leaf slice proves the pipeline AND after A3/A4/phase-3 remove the taint wall
that currently rejects the whole-eval graph anyway.

### 3.3 Key discipline + soundness (reuse the shipped machinery verbatim)

Binding key (already implemented as `topLevelCacheKey`, generalize the body to the applied grain):
```
input_key = SHA256( "v3-result-store-v1" ‖ codegenGateFingerprint ‖ manifestContentHash
                  ‖ currentSystem ‖ storePathOrFlakeLockPinOf(f) ‖ argsHash )
```
HARD RULES (all already enforced by the A5 taint mask + the WHNF-serialize throw):
1. **No persistent entry when any input is a mutable working-tree path.**  `storePathOrFlakeLockPinOf`
   returns empty (→ empty key → no-op) for anything not store-pinned or flake-lock-pinned.  The
   taint mask makes this observable: any `TAINT_READFILE`/`TAINT_FETCH`/`TAINT_STORE` bit ⇒ reject.
2. **`__currentTime`** → `TAINT_CURRENTTIME` (perturbable; recoverable only via the offline
   ≥3-clock manifest under pure-eval, else reject).
3. **`getEnv`** → `TAINT_GETENV` (perturbable under pure-eval where getEnv==""; reject under --impure).
4. **getFlake** → `TAINT_GETFLAKE`, demote-IFF-keyed (A4: the exact flake.lock text in the key).
5. **IFD** → the imported file's reads bump their axes; pure store-path IFD is content-committed
   (cacheable once built); a mutable transitive read poisons (fail-closed).
**Result-content-hash VERIFY layer (verify-don't-trust)**: store `input_key→result_hash` and
`result_hash→blob` separately; on a hit, relink + recompute the structural hash, assert ==
`result_hash` (shadow always, production per-segment-first-hit).  A mismatch is a Rule-0
falsifier → miss + counter.  This is the #741 shadow discipline made permanent.

### 3.4 IFD interaction (make the composition explicit)

- **Pure imports** (store-path / flake-locked, no mutable transitive read) → cacheable NOW
  (leaf) or via V3RG (graph).
- **IFD fragments** → the shipped v1 IFD import cache keys on `(path ‖ narHash(path))`
  (has the N1 under-capture bug the Phase-1 shadow accumulator fixes).  A cross-process store
  entry for an IFD result is sound ONLY with the **phase-3 build-boundary narHash key** — fold
  the realised IFD output narHash (known right after `realisePath`, BEFORE downstream parse/
  eval) into a PRE-eval key that CAN skip the body.  The post-eval v2 key (§1.7) is sound but
  NOT body-skipping → measured KILL-to-shadow.  **The store composes with IFD only through the
  phase-3 pre-eval key; until then IFD fragments are shadow-only (soundness), not served.**

### 3.5 Cross-machine (sync + signing) — reuse the binary-cache precedent

- **Distribution = narinfo-style**: `input_key → result_hash + segment URL + length` fetched
  over HTTP(S); blobs substituted by `result_hash` (content-addressed, fetch-once-share).
  Rides the existing substituter infra.  Read-only tier = the `aot_cache` `TBL_EVAL_RESULT`
  snapshot (already exists); `bench/build-aot-cache.py` extended to seal a fleet snapshot.
- **Signing = ed25519 over `(input_key, result_hash)`** (narinfo `Sig:` precedent, libstore
  signing); consumer trusts a snapshot only if signed by a configured key.  **The store
  distributes NO executable code** — a relinked thunk names a CU by content-key that the
  consumer recompiles from LOCAL source; the remote cache can only *reference* code, not
  inject it.  Verify-don't-trust re-hash (§3.3) is the second line.
- **Endianness**: all encoders explicit LE (already).  **Store-path context**: canonical store
  paths, remap like NAR contexts.  **currentSystem in the key** → cross-platform never collides.
- **Eviction**: page-granular LRU on the SQLite index (`last_used`, `size` columns); segment-
  granular unlink-while-open for the mmap tier (POSIX inode refcount = epoch-free cross-process).

---

## 4. PRE-COMMITTED FALSIFIER (Rule 0, measured EARLY, before any real store build)

**Hypothesis:** a persistent leaf/WHNF store hit (open + SQLite lookup + deserialize + relink)
costs **≤ 20% of re-evaluation** on a firefox-class result graph.

**Prototype (v0, ~2 days, gated `NIX_V3_RESULT_STORE_SPIKE`)** — NOT the store, only the
measurement.  Because firefox is unavailable on this darwin nixpkgs (§1.5), use a
firefox-class SYNTHETIC: a deep, fully-forced attrset of ~10⁵ WHNF nodes (the §1.3 machinery
proves it serializes) OR a `<nixpkgs>` package-set deep-forced to WHNF via `builtins.deepSeq`
on an x86_64-linux remote where firefox builds.  Serialize once to disk; in a fresh process,
mmap + deserialize + eager-materialize into arena cells (Phase-D barriers); measure on
**darwin-4** (`T_eval` cold vs `T_hit_eager` vs `blob_bytes` vs `materialised_arena_bytes`).

| `T_hit / T_eval` | verdict |
|---|---|
| ≤ 0.20 | **GO** — build v0-slice (leaf) then v2 (lazy). |
| 0.20–0.50 | **CONDITIONAL** — lazy `Tag::External` materialization mandatory; ship only if §6 RSS also wins. |
| > 0.50 | **KILL** — deserialize+relink ≈ re-eval; write the memo (mirror STAGE_9_KILLED). |

RSS falsifier (same spike): if `materialised_arena_bytes > 1.3 × blob_bytes`, eager is
RSS-hostile at fleet scale → v2 lazy is not optional.  (§1.6 already shows 68% dead-weight
thunks → strong prior that eager is RSS-hostile for graph entries, RSS-neutral for leaf.)

---

## 5. PHASING (each phase a pre-committed kill/ship gate; Rule 0)

- **v0-falsifier** (~2 d, darwin-4): §4.  `> 0.50` → KILL.
- **v0-slice — leaf/WHNF cross-process store** (~1 wk): generalize the applied-import + drvHash
  `EvalResults` tier into a first-class store with the §3.3 two-layer key + LRU/eviction +
  verify-don't-trust; serialize ONLY untainted WHNF results (no lazy graph).  **Ship gate:**
  cross-process warm hit on a leaf-projection workload gives the §4-measured CPU win AND
  `mismatchHits==0` under full `--brute` + byte-identity.  **Kill gate:** any mismatch.  This
  slice is the ONLY part with a clean soundness story today and does not depend on A3/phase-3.
- **v1 — whole-eval tier (GATED on A3/A4)**: only after A3/A4 let a flake-pinned nixpkgs eval
  under pure-eval with getFlake demoted-to-keyed.  Reuses the shipped `topLevelCacheKey` + the
  offline clock-stability manifest.  Blocked until then (§1.2 wall).
- **v2 — lazy-graph (V3RG) + `Tag::External` faulting + mmap segments** (~3 wk, GC-critical):
  §3.2.  **Ship gate:** warm anonymous RSS ≤ cold baseline + full `--brute` 22/22 under moving-
  GC stress.  **Kill gate:** RSS win < 15% vs eager, OR any missed-root UAF the brute can't pass.
- **v3 — IFD phase-3 build-boundary key** (separate track): fold the realised IFD output narHash
  into a PRE-eval key that skips the body.  This is the ONLY thing that reaches M5/HNE net
  (§1.7).  Gated on the A1 monadic-capture accumulator (Phase-1 shadow, shipped).
- **v4 — cross-machine sync + signing** (~2 wk): §3.5.  Security gate: a poisoned blob (wrong
  result_hash) is rejected 100%.

---

## 6. RISKS (highest first)

1. **The taint wall is the real gate, not the store.** Even a perfect store caches ~0% of a
   real `--impure` pkg eval (§1.2).  The pure payoff is DOWNSTREAM of A3/A4 (whole-eval) and the
   IFD portion is DOWNSTREAM of phase-3.  Building the store first, without those, ships a store
   that only serves the leaf-projection + WHNF-applied-import trickle (§1.4 ~2 entries/eval).
2. **v2 GC integration** (`Tag::External` faulting nodes = roots-that-allocate; PhD-6 UAF).  The
   single most likely thing to kill v2.  Gate = full `--brute` under stress; can't pass → v2 dies.
3. **Lazy-graph serialization is genuinely novel** (no in-tree prior art; cycles + 3 thunk-tails
   + Env-sharing round-trip).  Mitigation: the CU deserializer's 2-pass node-table discipline is
   battle-tested; heavy property tests (force-idempotence, sharing-equivalence).
4. **Serialize-boundary narrowness** (§1.3): raw `derivation {}` results and package-set attrsets
   are unserializable today — the leaf slice only serves fully-forced leaves.  The coarse win
   needs v2.
5. **Blob-hash re-verify cost** may itself exceed 20%.  Mitigate: re-verify per-segment once,
   shadow always / production periodic.

---

## 7. THE SMALLEST FIRST SLICE THAT SHIPS VALUE

**v0-slice (§5): the leaf/WHNF cross-process result store.**  Generalize the two persistent
tiers that ALREADY exist (`EvalResults` SQLite + `aot_cache` `TBL_EVAL_RESULT`) into one
first-class store keyed by the ALREADY-sound §3.3 discipline, serializing ONLY untainted WHNF
results via the ALREADY-validated `value_serialize`.  It:
- ships the moat's cross-process half for the trickle that is sound + serializable TODAY
  (leaf projections + the ~2 serializable applied-import entries/eval),
- is mostly plumbing (LRU column + two-layer content-address + best-effort semantics) on
  existing substrate,
- has a clean soundness story with NO dependence on A3/phase-3/lazy-graph,
- and validates the store pipeline end-to-end (key → serialize → SQLite/mmap → deserialize →
  verify) BEFORE the expensive lazy-graph + cross-machine builds.

**Do NOT build first:** the lazy-graph serializer (v2) or the whole-eval tier (v1) — both are
gated (v1 on A3/A4 removing the taint wall; the M5/HNE payoff on IFD phase-3).  Build the
falsifier (§4) first; if it clears ≤0.50, build the v0-slice; sequence A3/A4 + IFD phase-3 on
their own tracks; only then does the store's big payoff become reachable.

---

## 8. GO / NO-GO (honest)

**NO-GO on building the persistent store as a STANDALONE cross-process pure-payoff win right
now.**  The measured addressable share under real conditions is ~0% for the whole-eval cache
(taint wall, §1.2) and ~2 entries/eval for the applied cache (§1.4); the M5/HNE IFD portion is
0% net until phase-3 (§1.7).  The store does not remove those walls — A3/A4 (pure whole-eval)
and IFD phase-3 (the real workloads) do.

**GO on the v0-slice + falsifier as the SHIP VEHICLE**, sequenced AFTER (or alongside) A3/A4:
the substrate is 80% built (§2), the sound key + taint + serializer already exist, and the
leaf/WHNF slice is the cheap, RSS-safe, soundness-clean first step that de-risks the pipeline.
The correct order is: **falsifier (§4) → v0-slice (leaf) → A3/A4 (unblocks whole-eval v1) →
IFD phase-3 (unblocks M5/HNE) → v2 lazy-graph → v4 cross-machine.**  Phase-3 IFD soundness
should come first for the workloads the user actually cares about (M5/HNE are IFD-heavy); the
pure cross-process payoff is real but small and A3/A4-gated, not a standalone justification.
