# Tier-B design plan — Incremental / query-based (salsa-style) evaluation for v3

**Status:** DESIGN PLAN ONLY. No source change, no push, no heavy eval. Deliverable is
this document. All numeric thresholds are pre-committed Rule-0 falsifiers, not results.

**Author framing:** senior compiler / build-systems architect. This is the principled
version of the deployed "cache moat": a memoized eval-DAG with *precise* invalidation so
that re-evaluating a Hydra jobset after a small input change is near-instant.

**Grounding read (files actually inspected):**
`src/libexpr-v3/vm_applied_cache.cc` (live applied-import cache),
`include/v3/disk_cache.hh` + `disk_cache.cc` (CU cache + EvalResults table),
`primops.cc` §7001–8076 (IFD import cache soundness),
`emit.cc` §1978–2043 (`OP_IFD_PROBE` markers),
`include/v3/value_serialize.hh` (`canonicalHash`), `include/v3/serialize.hh` (CU schema),
`include/v3/ifd_trace.hh` (per-realise instrument), `include/v3/ffi.hh` §462–496
(top-level-cache NIX_PATH soundness), `aot_cache.cc` (mmap L3), `run.cc` §233–575
(root-eval scope). Prior art: `lode/PERF_STRATEGY_2026-05-17.md` (Stage 10 salsa),
`lode/LINKING_DESIGN_2026-05-17.md` (content-addressed cells).

---

## 1. Problem statement + the Hydra re-eval workload

### 1.1 The workload
The deployed value of v3 is CI: Hydra on `linux-0` runs `nix-eval-jobs` with ~20 worker
processes (@1 GB each), evaluating a jobset flake on **every commit** (see project
memory `reference_zw3rk_infra.md`). Each worker evaluates one or more top-level
attributes (jobs) of the jobset. IFD builds are dispatched to darwin over SSH.

Two structural facts make this the single best-case workload for incremental eval:

1. **Almost all of the eval-DAG is unchanged between commits.** A jobset flake has
   pinned inputs (`nixpkgs`, `haskell.nix`) whose `narHash` is byte-identical between
   commit *cₙ* and *cₙ₊₁*; only the project's own source (e.g. `cardano-node` HEAD)
   changes. The nixpkgs + haskell.nix substrate — the overwhelming majority of forced
   thunks — is **re-derived from scratch on every commit**.
2. **The same substrate is re-derived across the 20 workers within one commit too.**
   Every worker imports the same `<nixpkgs>`/haskell.nix base independently.

Today v3 mitigates (1)/(2) only crudely: the **CU disk cache** (`disk_cache.hh:37–70`)
saves *parse+lower+emit* per source file, and the **applied-import result cache**
(`vm_applied_cache.cc`, default-on) saves a narrow class of pure closure applications
*within a single process*. Neither is a precise, cross-commit, cross-process memo of the
eval result DAG.

### 1.2 The target
A **query-based memo of eval results** keyed on *content* (descriptor-hash + arg-hash +
recorded dependency set), persistent across processes, with **precise invalidation**:
a 1-byte change to the project source dirties only the memo nodes whose dependency set
transitively includes the changed file; the entire nixpkgs+haskell.nix substrate stays
green and is served from the memo (ideally an mmap'd shared artifact) in ~O(deserialize)
instead of O(re-eval).

This is exactly salsa's red-green model (`PERF_STRATEGY §2`, Stage 10): tracked-function
invocations are the query nodes; input changes propagate a dirty frontier; unchanged
subtrees are reused. The make-or-break constraint — which killed the moat-store — is
**IFD soundness** (§4).

---

## 2. Design

### 2.1 Memo granularity — the query node

Candidates weighed against the falsified prior art (`PERF_STRATEGY §5–6`):

| Granularity | Verdict | Why |
|---|---|---|
| **Flake-output** (Tweag `eval-cache-v5`) | Too coarse | A 1-byte project change dirties the whole output; no substrate reuse. This is the layer nix-eval-jobs/TW already have and it doesn't help within a jobset. |
| **Thunk-body** | Too fine (KILLed direction) | Env-hash recursion through closed values + cycle handling (letrec/blackhole) + most thunks are cheaper to re-run than to look up. `PERF_STRATEGY §2/§6.3` and Stage-9 kill (dedup <2×). |
| **CU / whole-file** | Wrong axis | That is the *bytecode* cache (`serialize.hh`), not a result cache. Already shipped. |
| **Applied-closure invocation** `apply(desc, args) → result` | **CHOSEN** | It is *already* the live memo key (`vm_applied_cache.cc:187–216`) and already carries the soundness restriction that makes it a total function of its key. |

**The query node = one application of a capture-free closure to canonically-hashable
WHNF args.** In salsa terms: `apply(desc, args)` is a tracked function; its return is the
memoized value.

Why this is the right node, grounded in the live cache:
- `vm_applied_cache.cc:196–199` restricts the applied cache to callees with
  `nUpvalues == 0 && capturedWiths == nullptr`. With no captured state, **the descriptor
  IS the complete behavioral identity** — `desc + args` totally determines the result.
  This is the property salsa needs from a tracked function (pure in its inputs).
- The key digest already exists: `value_serialize::canonicalHash(arg)` =
  `SHA-256(serialize(arg))`, order-independent (attr names sorted by string, string
  context sorted) — `value_serialize.hh:77–98`. It is **non-forcing**: it chases only
  already-`Evaluated` indirections and throws on any `Suspended`/`Closure`/`PrimOp`
  (`vm_applied_cache.cc:187–192` + `appliedKeyPrecheck` :213–271). That structural filter
  is what rejects the `callPackage`-class computed-arg flood cheaply.

The design's *new* contribution over the live cache is three things the live cache does
NOT do: (a) a **cross-process** content key (below), (b) **recorded dependency sets** for
precise invalidation, and (c) **IFD-taint tracking** to decide which nodes are soundly
memoizable (§4).

### 2.2 Cross-process node identity — descriptor content hash

The live applied cache keys on the raw `LambdaDescriptor *` pointer
(`appliedCacheTryKey`, `vm_applied_cache.cc:407–409`: `"%p:" + hex(argDigest)`). A pointer
is process-local — useless across the 20 workers and across commits.

**Replace the pointer with a content hash of the descriptor**: BLAKE3/SHA-256 over the
descriptor's *bytecode slice + its constant-pool references + its free-var structure* —
i.e. the "cell hash" primitive from `LINKING_DESIGN §2.1`. Stage 9 (the full
content-addressed cell *store*) was KILLed for a dedup ratio < 2× (`ROADMAP` §Stage 9),
**but that kill was about de-duplication, not about the hashing primitive** — the
per-descriptor structural hash is independently needed here as the only stable
cross-process/cross-commit memo key. It must be salted with the `opcodeTableFingerprint`
already carried in the CU header (`serialize.hh` schema 3) so a compiler change can't
serve mis-executing memo entries.

`persistentKey = H(descContentHash ‖ canonicalHash(args))`.

**Do not regress the recorded soundness bug.** `vm_applied_cache.cc:194–199` documents
that the first implementation keyed on the *CU* not the descriptor and produced a **wrong
drvPath** — `fromImportCU` marks every closure in a file, so different closures sharing
one CU with `{}` args collided (126 lookups / **85 bogus hits** on one hello eval). The
persistent key MUST be descriptor-content-hash, never CU-hash. This is a load-bearing
lesson, cited in-source.

### 2.3 Dependency tracking

Each memo node records the dependency set whose change would change its result. Mechanism
= a thread-local **ActiveQuery stack** (salsa's model), pushed when a memoizable
application begins and popped at its `OP_RETURN`:

- **Source inputs.** Which `.nix` files were parsed/imported on the path to the result,
  recorded by their content SHA — the CU cache *already computes exactly this key*
  (`disk_cache::computeKeyForFile`, `disk_cache.hh:46`). In flake terms these roll up to
  flake-input `narHash`es. Recorded as a sorted vector of 32-byte CU keys.
- **Sub-node edges.** When evaluating node A consults the memo for sub-node B (hit or
  miss), record edge A→B. This is the red-green dependency graph; it lets a dirty B
  propagate to A without re-hashing A's whole input closure.
- **Store / IFD outputs — the crux.** When any `OP_IFD_PROBE` fires within a node's
  dynamic extent (see §2.4), the node is flagged IFD-touched and the store path + its
  `narHash` (via `ffi::storePathNarHash`, already used at `primops.cc:7029`) is captured
  into the node's dep record.

The recorder reuses infrastructure that already exists: the per-realise `RealiseScope`
nesting-depth counter (`ifd_trace.hh:79–101`) is the same thread-local discipline the
ActiveQuery stack needs, and the `OP_IFD_PROBE` opcode is already dispatched in the VM
loop (`emit.cc:2041–2042`).

### 2.4 IFD-taint propagation (feeds §4)

Taint sources are **static and already marked**: `emit.cc:1987–2030` emits `OP_IFD_PROBE`
before every IFD-class primop call — the complete set is `import`, `readFile`, `readDir`,
`pathExists`, `readFileType`, `findFile`, `fetchurl`, `fetchTarball`, `fetchTree`,
`fetchGit`, `fetchMercurial`, `filterSource` (both `__`-prefixed and bare), plus
`derivationStrict`'s `__impure` path (`primops.cc:4718/5436`).

Rule: a node is **IFD-touched** iff any `OP_IFD_PROBE` fires in its dynamic extent **OR
any sub-node it called is IFD-touched** (transitive). The recorder raises the current
frame's flag on a probe and ORs children's flags up the ActiveQuery stack on return.
Transitivity is essential: it lets the pure nixpkgs substrate nodes stay memoizable even
inside a top-level eval whose *root* is IFD-tainted (the cardano-node case: one dominant
plan-IFD job with ~96% pure graph beneath it — project memory
`project_v3_ifd_frontier_2026-08-08.md`).

### 2.5 Invalidation

- **Coarse gate:** flake-input `narHash`es. nix-eval-jobs already receives the locked
  flake with every input's hash. A node whose source-dep set ⊆ {inputs unchanged since
  the memo was written} is a *hit candidate*; any node touching a changed input is dirty.
- **Fine, precise:** the red-green walk. Changing the project source changes the CU keys
  of the project's files → marks exactly the nodes whose source-dep set contains them
  dirty → propagates up sub-node edges → the substrate nodes (dep set = only
  nixpkgs/haskell.nix CU keys) never dirty. This is precise input-change → dirty-frontier
  propagation, and it is the whole point vs the coarse flake-output cache.

### 2.6 Persistence across processes / commits

- **Storage:** a new `Memo` table in the existing bytecode SQLite DB, sibling to
  `EvalResults` (`disk_cache.hh:101–129`), with its own `kMemoSchemaVersion`.
  Row = `persistentKey → { blob = value_serialize(result), depRecord = {cuShas[],
  storeNarHashes[], subNodeKeys[]}, ifdTouched : bool, schema }`.
- **Result serialization already exists:** `value_serialize::serialize` round-trips the
  exact WHNF derivation-result subset (Attrs/List/String-with-ctx/Int/Bool/Null/Path/
  Float — `value_serialize.hh:1–44`). Non-serializable results (closures/functions) are
  simply not memoized — same rule the applied cache and the IFD import cache already use
  (`primops.cc:8066` catch-and-skip).
- **Composition with the CU cache (orthogonal layers):** CU cache = *compile* memo
  (parse→lower→emit), Memo table = *execute* memo. On a Memo hit we skip execution of
  that whole subtree and deserialize the result directly — we don't even need the CU
  loaded for it. On a miss, the CU cache still saves the compile. No conflict; the Memo is
  strictly the layer above.
- **Composition with the AOT mmap (the multi-tenant compounder):** ship a pre-built
  nixpkgs+haskell.nix Memo as a read-only mmap'd artifact, exactly the `aot_cache.cc` L3
  pattern (Shared_Clean pages across all 20 workers) and the
  `nixpkgs-eval-result-cache.mmap` idea already in `ROADMAP` lines 106–114. Then a fresh
  worker's first eval is a warm eval, and the substrate memo is shared physical memory
  across the whole worker pool — the asymmetry vs TW (which re-parses per process) that
  `ROADMAP` lines 74–114 identifies as invisible in single-process benches.
- **Batching:** persistence writes must batch or they dominate cold wall. This is the
  Phase-5b lesson recorded verbatim at `disk_cache.hh:131–140` (517 inserts × 1.1 ms ≈
  569 ms → the +71% cold slowdown). Reuse `beginEvalResultBatch/commitEvalResultBatch`
  bracketed at the `runRootExprModule` scope (`run.cc:247–258`).

---

## 3. Concrete source touchpoints

| Concern | File:line | Reuse / change |
|---|---|---|
| Node key (arg digest) | `vm_applied_cache.cc:370–416` `appliedCacheTryKey` | keep; add desc-content-hash variant for persistence |
| Non-forcing arg filter | `vm_applied_cache.cc:213–271` `appliedKeyPrecheck` | reuse verbatim as the "is this node memoizable" gate |
| Shadow compare | `vm_applied_cache.cc:279–367` `appliedShadowCompareOne` | reuse as the zero-false-hit oracle in Phase S1 |
| Gate polarity pattern | `vm_applied_cache.cc:425–460` `appliedCacheOn/ShadowMode` | mirror for `NIX_V3_MEMO=probe/shadow/1` |
| Result (de)serialize | `value_serialize.hh:60–98` | reuse `serialize`/`deserialize`/`canonicalHash` |
| Persistent table | `disk_cache.hh:101–159`, `disk_cache.cc:486–590` | add `Memo` table + `kMemoSchemaVersion` sibling to `EvalResults` |
| Batch bracket | `run.cc:247–258`, `disk_cache.hh:156–157` | wrap memo writes in the existing per-eval batch |
| Taint source markers | `emit.cc:1987–2043` `OP_IFD_PROBE` (12 primops) | consume the already-emitted opcode as taint signal |
| Realise arg + narHash | `primops.cc:7027–7041`, `ffi.hh:455` `storePathNarHash` | reuse to pin store deps in the dep record |
| ActiveQuery/depth stack | `ifd_trace.hh:79–101` `RealiseScope` | same thread-local discipline for the query stack |
| Desc content-hash primitive | `LINKING_DESIGN §2.1` (design only; not yet built) | build the per-descriptor structural hash, salted by `serialize.hh` opcode fingerprint |
| Cross-process mmap | `aot_cache.cc` (L3 reader) | ship the substrate Memo as a Shared_Clean artifact |
| Root-eval scope | `run.cc:233` `runRootExprModule` | node-tree root; recorder init/flush here |

---

## 4. THE SOUNDNESS ANALYSIS vs IFD (make-or-break)

**The bar.** This is what killed moat-store ("A5 top-level cache UNSOUND for IFD"; the
NO-GO was *soundness, not engineering* — project memory). A source-keyed memo returns a
result Value. IFD makes an eval result depend on **build outputs / store state**, so a
naive input-keyed memo can map a source-identical key to *different* results across store
states — e.g. serve a **wrong `drvPath`**. One false-hit on a drvPath is a corrupted CI
build. The soundness argument is the crux and must clear the same bar that killed the
moat-store.

### 4.1 The two IFD hazards
1. **IFD content dependence.** `import (runCommand …)`, `readFile drv.outPath`, etc. The
   result depends on the *content* at a built store path. The path is usually
   input-addressed (derived from the input closure hash), but the *content there* is only
   guaranteed identical if the build is **deterministic**. The shipped IFD import cache
   already confronts exactly this and is the template: `primops.cc:7021–7080` (review note
   T-5) keys on `path ‖ narHash` (content), and **skips the cache entirely if the narHash
   is unavailable**, precisely because "an input-addressed output is not content-addressed
   in its path, so the same path can hold different content after a non-deterministic
   rebuild; path-only keying then serves stale content."
2. **drvPath/outPath transitive dependence.** `derivationStrict` computes a `drvPath` from
   its input closure; if any input was itself produced via IFD, that drvPath transitively
   depends on store state. This is why taint must be transitive (§2.4).

A related, already-fixed unsoundness class to respect: keying on a *mutable channel
symlink* string. `ffi.hh:462–496` (R2) records that the top-level cache keyed on the raw
`getenv("NIX_PATH")` is unsound because `nixpkgs=…/channels/nixpkgs` is a stable string
whose *target* changes on `nix-channel --update` → stale hit. The fix
(`resolveNixPathContentIds`) resolves each entry to a store-path content id. **Lesson for
the Memo:** every input in a node's dep set must be pinned by *content*, never by a
mutable name. Flake `narHash`es satisfy this; channel symlinks do not.

### 4.2 When is a memo hit provably correct?
A Memo entry for node `(desc, args)` is sound to serve iff ALL hold:
- **(a) Total function of the key.** `desc + args` fully determines the result. Guaranteed
  by the `nUpvalues == 0 && capturedWiths == nullptr` restriction already enforced at
  `vm_applied_cache.cc:196–199` (no hidden captured state) and by the non-forcing key that
  only admits WHNF args (`appliedKeyPrecheck`).
- **(b) No unpinned store dependence.** EITHER the node is **PURE** (IFD-touched = false:
  no `OP_IFD_PROBE` fired in its transitive extent) — then the result is a pure function
  of `(desc, args, source-CU-SHAs)` and source-keying is sound; **OR** every store path
  the node read is pinned in the key by `narHash` (content-addressed extension), sound
  under the same build-determinism assumption the shipped IFD import cache already makes.

### 4.3 Detection / exclusion of IFD-dependent nodes
At node completion the recorder (§2.4) has a transitive IFD-touched flag:
- **IFD-touched = false → PURE →** memoize under the source+args key. Provably sound; no
  store dependence. This generalizes the memory's "top-level cache scope = pure-value
  evals only" rule from the *root* to *every sub-node* — which is the key unlock, because
  it lets the pure nixpkgs substrate be memoized *even inside an IFD-tainted top-level
  job*.
- **IFD-touched = true →** two options, phase-gated:
  - **(i) EXCLUDE** from the source-keyed memo. Always sound. This is the conservative
    default and the moat-store-safe subset.
  - **(ii) narHash-EXTENDED key**: memoize under `key ‖ sort(storeNarHashes read)`. Sound
    under build determinism — identical to the assumption the shipped IFD import cache
    already ships with (`primops.cc:7001–7006, 7021–7029`). Admitted ONLY after the shadow
    (Phase S1) proves zero false-hits on tainted nodes.

### 4.4 Residual soundness risks (named, not hand-waved)
- **Non-deterministic builds** break option (ii): same drv, different narHash on rebuild.
  Mitigation: option (ii) is gated on shadow proof; the safe fallback is (i)/EXCLUDE. The
  pure subset (S2) never touches this risk at all.
- **Impure primops other than IFD** (`currentTime`, `getEnv`, `builtins.currentSystem`,
  `--impure` reads). These are NOT in the IFD-probe set. Treat them as additional taint
  sources: either add probe markers for them or (simpler) require the memo to run only
  under a pure eval configuration (nix-eval-jobs' default). `PERF_STRATEGY §2` lists these
  as invalidation tags; here they are taint sources that force EXCLUDE.
- **The recorded CU-vs-desc bug** (§2.2): re-introducing CU-keyed identity would resurrect
  the 85-bogus-hit class. The key derivation is the single most safety-critical line.

---

## 5. Phased plan — shadow-mode measurement FIRST

Mirrors how the moat-store shadow and the live applied-cache shadow were done
(`appliedCacheShadowMode`, `vm_applied_cache.cc:453–474`; Phase-5 SHADOW,
`disk_cache.hh:118–120`): **measure would-be hit-rate + prove zero false-hits vs the TW
oracle BEFORE any active skip.**

- **Phase S0 — recorder + taint instrument, MEASURE-ONLY** (`NIX_V3_MEMO=probe`).
  Build the ActiveQuery stack + transitive IFD-taint on the existing `OP_IFD_PROBE` hook.
  No storage, no skip. Emit per candidate node: would-be persistent key, pure-vs-tainted,
  source-dep-set size, sub-node edge count, and — loading a prior process's persisted
  key-set — the **cross-process WOULD-HIT rate weighted by node eval cost**. This produces
  the warm-fraction number `PERF_STRATEGY §7` says must exist before any build. Also
  measure the **pure fraction** of eval-cost-weighted nodes (S2's ceiling) and the
  **memoizable fraction** (how many nodes pass `appliedKeyPrecheck` — the live probe found
  ~370/380 `tryKey` attempts UNHASHABLE per hello eval, so this is the top risk).
- **Phase S1 — SHADOW (compute + compare, never skip)** (`NIX_V3_MEMO=shadow`).
  Persist the Memo table. On a would-hit, evaluate normally and lockstep-compare fresh vs
  cached via `appliedShadowCompareOne` (`vm_applied_cache.cc:279–367`). Count mismatches,
  separately for PURE vs IFD-tainted nodes. Run across a real Hydra jobset at two
  consecutive commits (cₙ then cₙ₊₁). This is the zero-false-hit proof vs the TW oracle.
- **Phase S2 — ACTIVE, PURE-ONLY** (`NIX_V3_MEMO=1`, tainted EXCLUDED).
  Skip-on-hit for pure nodes only — the always-sound subset. Measure wall reduction on the
  cₙ→cₙ₊₁ re-eval and on the 20-worker within-commit fan-out. This subset alone should
  capture most of the substrate.
- **Phase S3 — ACTIVE, IFD narHash-EXTENDED (optional).**
  Admit IFD-touched nodes under the narHash-extended key. ONLY if S1 proved them sound AND
  S2 left material wall on the table.
- **Phase S4 — AOT substrate artifact.**
  Ship the pre-built nixpkgs+haskell.nix Memo as an mmap'd Shared_Clean artifact
  (`aot_cache.cc` pattern) shared across the worker pool. The multi-tenant compounder.

Each phase is a Rule-0 falsifier: it kills a hypothesis or confirms one.

---

## 6. Pre-committed KILL criteria (Rule 0)

- **S0 kill (the workload-mode gate):** cross-process would-hit on the *unchanged
  substrate*, weighted by node eval-cost, **< 40%**. (Stiffer than `PERF_STRATEGY §5`'s
  <20% Stage-10 kill, because Hydra is the best-case warm workload — if it isn't warm
  here, it's warm nowhere.) Also kill if the **memoizable fraction** (nodes passing
  `appliedKeyPrecheck`) is so small that the hit-cost-weighted ceiling is < 40% — the
  "~370/380 unhashable" risk realized.
- **S1 kill (the moat-store bar):** ANY shadow mismatch on a PURE node that is not a known
  external non-determinism → the taint/pure classification is unsound; stop. **Even one
  false-hit on a drvPath kills it.**
- **S2 kill (granularity):** active pure-only wall reduction **< 2×** on the cₙ→cₙ₊₁
  re-eval *despite* would-hit ≫ that — means the hits are on cheap nodes and lookup +
  deserialize costs more than re-eval (mirrors `PERF_STRATEGY §2` mid-impl kill). Retreat
  to coarser (flake-output) or abandon.
- **S3 kill:** any tainted-node false-hit in shadow, OR per-node `storePathNarHash` key
  cost dominates the saving.
- **Persistence kill:** if serialize + SQLite commit per node exceeds re-eval cost and
  batching (`disk_cache.hh:131–140`) doesn't fix it (the Phase-5b +71% cold lesson).

---

## 7. Secondary angle — v3-as-static-analyzer (briefer)

The IR (`ir.cc` `computeFreeVars`, the `opt_*.cc` passes) + the static `OP_IFD_PROBE`
markers enable analyses an eval-only tool (TW) cannot do without re-deriving structure:

- **Static impurity / IFD scan.** `emit.cc:1987–2030` marks IFD-class primops at compile
  time. A static reachability pass over a job's bytecode can report **"this job will / will
  not do IFD" ahead of eval** — the static complement to the dynamic taint recorder, and a
  direct input to the Hydra scheduler (route IFD jobs to store-capable workers; run
  provably-pure jobs anywhere / fully-cache them). This is unique leverage: TW has no
  bytecode to scan.
- **Dead-attr / dependency extraction.** `opt_occur.cc` + `opt_dce.cc` already compute
  reachability. A `nix v3-deps <attr>` could emit the *static* import graph + which flake
  inputs an attribute's eval can *possibly* touch, **without evaluating** — letting Hydra
  decide which jobs a commit can possibly affect (job-level pre-filtering before any eval,
  a coarse partner to §2.5's fine invalidation).
- **Better errors.** The position side-table + two-span diagnostic design
  (`ERROR_UX_DESIGN_2026-05-20.md`) is IR-grounded.

Rule-0 caveat: these are speculative directions, each needs its own falsifier; named here
as "what the IR uniquely enables," not committed. The static IFD-scan is the highest-value
one because it feeds the same Hydra scheduler the incremental memo serves.

---

## 8. Design skeleton (one-glance summary)

```
query node      = apply(descContentHash, canonicalHash(WHNF args))    [capture-free only]
dep record      = { sourceCuShas[], storeNarHashes[], subNodeKeys[], ifdTouched }
taint           = OP_IFD_PROBE in transitive extent  →  ifdTouched (propagated up stack)
sound to serve  = capture-free (a)  AND  ( PURE  OR  store deps pinned by narHash )  (b)
invalidation    = flake-input narHash gate (coarse) + red-green dep walk (precise)
persistence     = Memo table (sibling of EvalResults) + AOT mmap substrate (Shared_Clean)
compose         = CU cache (compile memo) ⟂ Memo (execute memo) ⟂ AOT (shared pages)
rollout         = probe → shadow(0 false-hits) → active-pure → active-IFD → AOT
```


Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.
SPDX-License-Identifier: Apache-2.0
