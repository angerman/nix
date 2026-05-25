# Module Linking Design — 2026-05-17

Concrete linking-story design for Nix .nix files in the v3 bytecode VM. Synthesises OCaml interface/implementation split, GHC ABI-hash salting, Unison content-addressed cells, and JVM classloader semantics into a Nix-fit design.

This document is the design proposal. Migration phases are codified in `ROADMAP_TO_VISION_2026-05-15.md` Stage 9.

## 1. Background — what "linking" means for Nix

Nix has no formal linker because each .nix file is a **single expression**, not a sequence of declarations like Haskell, OCaml, or Java. There are no "exports"; the export is the value the expression denotes. Cross-file references happen at one syntactic site: `import path` (and its sugars `<angle>`, `builtins.import`, `scopedImport`, `getFlake`).

At runtime, `import` is a primop that:
1. resolves a path,
2. parses + lowers + evaluates the file,
3. returns the resulting Value.

v3's current `disk_cache.cc` caches step 1–2 on a per-file SHA-256. Step 3 is re-done every process invocation.

There are three "linking" decisions Nix conflates that other languages separate:

- **Code lookup** — what bytecode runs? Currently keyed on whole-file SHA.
- **Symbol resolution** — which `builtins.foo`? Currently re-resolved at deserialize time via `findPrimOp(name)`.
- **Scope binding** — which `lib`, which `pkgs`? Purely dynamic; resolved by closure capture at eval, never statically.

Only the first two are amenable to a static-ish linker. Scope must remain dynamic because Nix's `with`, overlays, and `_module.args` are explicitly extensional.

**Definition.** Linking for Nix = (i) content-addressed code lookup at thunk-body granularity, (ii) symbolic primop resolution at module load, (iii) lazy module-result thunks dispatched on import. No more, no less.

## 2. Proposed design

### 2.1 Unit of content-addressing: the thunk-body

Three observations decide the granularity:

- A .nix file is one expression; "top-level let-bindings" don't exist syntactically.
- Every `MkThunk`/`MkClosure` IR node is a self-contained unit with explicit free-var capture (`ir.cc:287-456` `computeFreeVars`).
- nixpkgs duplicates the same lambda shapes across thousands of files — `mkDerivation`-shape callbacks, `mapAttrs` lambdas, `lib.fix` curries (LESSONS_LEARNED §4.1).

→ **Each `ir::Function` / `ir::Lambda` / `ir::MkThunk` is the unit.** The whole-file CU becomes a manifest listing the BLAKE3 hashes of its constituent thunk-bodies plus a top-level entry-point reference.

Borrowed from:
- Unison ([Chiusano, *The big idea in Unison*](https://www.unison-lang.org/blog/the-big-idea-in-unison/)): smallest stable unit; identity = structural hash.
- Lean 4 (`Expr` hash-cons): bottom-up structural hash in production.
- Coq (`Hashcons.ml`): same pattern in a 30-year-old production system.
- GHC's `.hi` ABI hashes: cache invalidation by compiler-version salt.

### 2.2 On-disk format

Two SQLite tables in the existing `v3-bytecode-v1.sqlite`:

```sql
-- Replaces today's CompilationUnits table.
create table Modules (
    key       blob primary key,    -- 32-byte source SHA-256 + canonical path tag
    manifest  blob not null,        -- serialized ModuleManifest (§2.3)
    schema    integer not null,     -- kSchemaVersion + opcodeTableFingerprint
    last_used integer not null,
    size      integer not null
);

-- NEW: per-thunk-body content-addressed store. Shared across modules.
create table Cells (
    hash      blob primary key,     -- 32-byte BLAKE3 of canonical IR
    body      blob not null,        -- bytecode + const-pool slice
    deps      blob not null,        -- serialized vector<CellHash> (free-cell refs)
    schema    integer not null,
    refcount  integer not null,     -- for GC of orphaned cells
    last_used integer not null
);
create index idx_cells_lru on Cells(last_used);
```

This is **OCaml `.cmi` + `.cmo` split** (cmi = ModuleManifest; cmo = Cells) applied to Nix. Or equivalently **Java `.class` files + a shared `.jar`** where each cell is a "class" identified by hash.

### 2.3 ModuleManifest structure

```c++
struct ModuleManifest {
    uint32_t  schema;                       // kSchemaVersion + opcode fingerprint
    SourcePath canonicalPath;                // for diagnostics
    Sha256    sourceHash;                    // input identity

    // Module-local interning tables (small; not content-addressed).
    std::vector<std::string>  symbols;       // local SymbolId → name
    std::vector<PosEntry>      positions;     // PosIdx side-table
    std::vector<std::string>  stringConsts;   // pool, deduped by content

    // The CODE lives elsewhere:
    std::vector<CellHash>      ownedCells;    // BLAKE3 hashes (cells in store)
    CellHash                   entryCell;     // top-level expression

    // Symbolic refs out: resolved at module-load time.
    std::vector<std::string>   primOpRefs;    // names; findPrimOp on load
    std::vector<SourcePath>    importTargets;  // STATIC imports detected at lower
};
```

Each `Cell` body holds: bytecode slice, local const-pool indices remapped to module-relative indices, plus a `deps` vector of CellHashes referenced by `OP_MAKE_THUNK` / `OP_MAKE_CLOSURE` (which currently embed a `LambdaDescriptor` index — that index becomes a manifest-local cell index, the manifest dereferences it to the global cell store).

### 2.4 Link-time data structures (in-process)

```c++
struct LoadedModule {
    ModuleManifest *           manifest;
    std::vector<Cell *>         cells;          // resolved from cellStore
    std::vector<PrimOp *>       primOps;        // findPrimOp() once per module
    std::vector<Value *>        importedValues;  // lazy thunks; see §2.5
};

struct CellStore {
    folly::F14FastMap<CellHash, std::shared_ptr<Cell>> live;
    nix::Sync<DbState> persistent;               // SQLite
    Cell * resolve(CellHash);                    // memory → disk → recompile
};
```

The CellStore is **process-global, lock-free on the hot path** via a sharded F14 map (à la V8's `IsolateGroup` code cache). Concurrent evaluators in the same process share resolved cells.

### 2.5 Eval-time resolution flow for `import ./foo.nix`

```
primImport(path):
  1. canonicalPath = resolveSymlinks(path)
  2. sha = sha256(readFile(canonicalPath))
  3. moduleKey = (sha, canonicalPath)        # path-tagged so two files
                                              # with identical content but
                                              # different #line origins
                                              # don't collide for traces
  4. m = moduleCache.lookup(moduleKey)
     if miss:
       manifestBlob = sqlite.Modules.lookup(moduleKey)
       if hit:    m = deserializeManifest(manifestBlob)
       else:      m = parse → lower → emit;
                  for each ir::MkThunk/MkClosure in m:
                    cellHash = structuralHash(node, freeVarHashes)
                    if not cellStore.contains(cellHash):
                      cellStore.insert(cellHash, serializeCell(node))
                    m.ownedCells.push(cellHash)
                  sqlite.Modules.insert(moduleKey, serialize(m))
  5. resolved = LoadedModule{m, [cellStore.resolve(h) for h in m.ownedCells]}
  6. resolved.primOps = [findPrimOp(name) for name in m.primOpRefs]
  7. Return a Thunk whose body = entryCell, env = empty-with-builtins
     (Imports are evaluated in a fresh environment per Nix semantics.)
```

Step 5 is the actual "linker": it walks `ownedCells`, looks each up in the in-memory map; on miss, SQLite fetch; on miss, refuse (corrupt manifest) and recompile from source.

**Primop refs are resolved symbolically by name** (like ELF `R_X86_64_GLOB_DAT`), once per module load. Necessary because primop indices drift across compiler versions; primop names are stable. Mirrors what `serialize.cc` already does — keep that.

### 2.6 Builtins and primops

Two-tier resolution:

- **`builtins` as a Value**: a single immutable attrset built once per `Evaluator`. Closed-over at the entry-point cell's compilation; references resolve via normal attrset selection (`OP_ATTRS_SELECT` IC). This is what TW does and v3 inherits — keep it.
- **Direct primop calls** (`OP_CALL_PRIMOP_*` opcodes from `opt_primop_fuse.cc`): the bytecode holds an index into the module's `primOpRefs` table; the module loader fills `primOps[i] = findPrimOp(name)`. Identical to ELF GOT/PLT.

No "primop linking at compile time" — the registry can grow (plugins) and we don't want cache invalidation when an unrelated primop is added.

### 2.7 NIX_PATH / `<angle>` / overlays

These are **search-path resolution**, not linking. `findFile("<nixpkgs>")` runs at primop-call time and yields a concrete path; that path enters the normal `primImport` flow. The cache key is **the resolved path's content**, not the angle-bracket spelling — same file under two NIX_PATH entries shares cells.

`scopedImport` is `import` with a non-default starting environment — bytecode is identical to `import`, only the entry-thunk's env differs. Same cache key; different Value.

Overlays don't touch linking at all — they're attrset `//`-composition on already-evaluated values.

### 2.8 Import-from-derivation (IFD)

IFD breaks any static-link discipline because the imported path doesn't exist until the store has built it. Treat IFD exactly as today: the `primImport` primop blocks on `store.ensureValid(drvOutputPath)`, then re-enters the flow at §2.5 step 1. Cells from IFD modules are stored normally — the second import is a hot-cache hit modulo build determinism producing identical content.

**Novelty**: because cells are content-addressed, an IFD output identical to a non-IFD module's bytecode shares cells. Two different paths producing the same lambda body store one cell.

### 2.9 Lazy module-result thunks

`import` returns a `Thunk` whose body is the entry cell. First force → push frame → execute entry cell → memoize result on the Thunk. The LoadedModule is itself reference-counted; once all its result-thunks are blackholed-and-evaluated and all closures from it are unreachable, the LoadedModule can be dropped (cells stay in the global CellStore for the process lifetime; LRU-evicted from SQLite).

This is **JVM classloader-with-WeakReference semantics**: code stays loaded as long as something refers to it.

## 3. Why this design over alternatives

| Alternative | Why rejected |
|---|---|
| **Status quo: whole-file SHA cache** | Misses 90% of nixpkgs's intra-file duplication. UNISON_IDEAS §1 spells out the win. |
| **GHC `.hi`-style per-module ABI hash** | Modules in Nix don't have stable interfaces — the "interface" is the value, which is dynamic. No type signatures to abstract. So .hi-style ABI hashes would just be `hash(entryCell)`, gaining nothing over content-addressed cells. |
| **V8 ScriptCache: per-script bytecode snapshot** | Same flaw as status quo. V8 gets away with it because JS scripts are big and unique; nixpkgs .nix files are small and shape-shared. |
| **Unison-pure: every term is a cell, names are metadata** | Too radical. Nix has dynamic scope (`with`, overlays). Names cannot be metadata. Steal hash-as-identity for code; keep names for values. |
| **Lamdu hash-consed expressions** | Lamdu hashes whole programs as identity. For an evaluator that needs to talk to humans about line numbers, positions must be preserved separately — already done in v3's position side-table. |
| **BEAM module reload / code purging** | Erlang's hot-reload semantics need "old code" + "new code" double-mapping; Nix evaluates once per invocation, no reload needed. Skip the complexity. |

The proposed design is the **OCaml split** (interface/implementation) + **GHC ABI-hash salting** (cache invalidation) + **Unison fragment hashing** (granularity) + **JVM classloader** (lazy resolution). Each component is production-proven; the synthesis is the v3-specific contribution.

## 4. Migration path (5 phases, ~5 weeks)

Each phase is commit-mergeable on its own (Rule 0: each phase falsifies a hypothesis). Codified in `ROADMAP_TO_VISION_2026-05-15.md` Stage 9.

**Phase L0 (1 week, ~400 LoC)** — Add `structuralHash()` on IR nodes alongside `computeFreeVars()` in `ir.cc`. No on-disk change. **Falsifies**: "fragment hashing collides at acceptable rate." **Verify**: compute hashes for all nixpkgs .nix files; measure dedup ratio (expect 5-10× collapse on nixpkgs lib lambdas).

**Phase L1 (1 week, ~300 LoC)** — Switch from de-novo `VarId` counters to de Bruijn `(depth, index)` in IR (UNISON_IDEAS Item 2 — ABT identity). Touches every opt pass mechanically. **Falsifies**: "alpha-equivalence enables cell sharing." **Verify**: re-run L0 hash collision survey; dedup should jump again on the same hashed shapes. **Critical**: this is also the prerequisite for stable shape IDs in ROADMAP Stage 5 (hidden classes).

**Phase L2 (2 weeks, ~600 LoC)** — Schema change: bump `kSchemaVersion` to 9; add `Cells` table; change `Modules` row from blob to `ModuleManifest`. Write the cell-store side first; keep existing whole-blob deserialize path as fallback. **Falsifies**: "cells round-trip through SQLite faithfully under concurrent insertion."

**Phase L3 (1 week, ~300 LoC)** — Migrate `primImport` to the §2.5 flow. Two-stage: first new-format-only behind `NIX_V3_LINK=1`; once parity confirmed on nixpkgs eval, default-on. Old whole-blob entries silently invalidated by schema bump.

**Phase L4 (1 week)** — Hash-queryable CLI (UNISON_IDEAS Item 5): `nix v3-inspect cell <hash>`. Pure ergonomics; valuable for debugging cache mishits.

## 5. Risks and open questions

1. **Position metadata across cells**. A lambda body's `PosIdx` indexes into its module's PosTable; if cell C is shared between modules A and B, whose PosTable wins?
   **Resolution**: PosIdx becomes `(moduleId, idx)` — moduleId is the LoadedModule resolved at link time, NOT serialized into the cell. Cells reference positions via a separate `posKey` (cell-local index), which the LoadedModule resolves per its own PosTable.

2. **Source-position drift on cell sharing**. Two structurally-identical lambdas at different source positions show only one error trace.
   **Mitigation**: error traces include `(LoadedModule, callSite)` pair, not just cell-internal positions; callsite positions are always module-local.

3. **Cell invalidation on opcode-table changes**. Already handled — `opcodeTableFingerprint()` is salted into both Manifest and Cell schema. Bump invalidates everything.

4. **GC of dead cells in SQLite**. A cell whose all referring modules are LRU-evicted leaks.
   **Mitigation**: `Cells.refcount` decremented on Module eviction; scrub on startup. Cost: O(modules) at startup; tolerable.

5. **Plugin primops**: primops added after a module was cached won't appear in `primOpRefs` but might be referenced if dynamically constructed via `builtins.<plugin>`. Already solved — `findPrimOp(name)` resolves at load time, after plugins register.

6. **Open: hash function choice**. Proposal: BLAKE3 (faster than SHA-256, used by nix-store for NAR hashing in recent versions). Reuses existing hash infrastructure. SHA-256 would also work; cells just need 32+ bits of entropy and a stable encoding. **Decide before L0**.

7. **Open: hash IR pre- or post-optimization?** Pre-opt gives cross-optimizer-version stability; post-opt gives stronger dedup (optimized variants of the same source converge). **Tentative answer**: post-opt, because optimizer-version is already salted by `opcodeTableFingerprint`. **Measure during L0.**

8. **Open: integration with EvalCache**. UNISON_IDEAS §3 (hash-keyed result cache) is a separate cache keyed on `(cellHash, argHash)`. This linking proposal is the foundation; result-caching builds on top. **Track separately.**

9. **Open: shared CellStore between worktrees / users**. SQLite is per-user. A multi-user system could share Cells (content-addressed and immutable). Out of scope for this proposal but the schema admits it.

## Cited prior art

- Filliâtre & Conchon, *Type-safe modular hash-consing* (ML Workshop 2006).
- Chiusano, *The big idea in Unison* (unison-lang.org/blog/).
- de Moura et al., Lean 4 `Expr` representation (Lean 4 source).
- OCaml manual §11, .cmi/.cmo split.
- GHC user's guide ch. 5, recompilation checking via ABI hashes.
- V8 Code Caching design (v8.dev/blog/code-caching-for-devs).
- JEP 310, Application Class-Data Sharing.
- BLAKE3 specification (github.com/BLAKE3-team/BLAKE3-specs).
