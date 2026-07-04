# Persistent Result Store Design — 2026-07-04

Concrete design for a persistent, mmap-backed, content-addressed LRU
**result** store for the v3 bytecode VM: memoize `(import f) args →
forced result` across processes and machines. This is the *ship
vehicle* for the in-process eval-cache spike (`#741`,
`value_serialize.{hh,cc}`) — the same key discipline, extended to
survive process exit and to distribute like a binary cache.

This is a design proposal, not a commitment. Per Rule 0 the first
section is a pre-committed falsifier; the whole thing dies cheap at §1
if the hit path can't beat re-eval.

Synthesises: the existing `aot_cache` mmap flat-file reader
(`aot_cache.cc`), the `#741` WHNF Value serialiser + `canonicalHash`
(`value_serialize.cc`), the CU disk cache + sparse-remap deserialiser
(`disk_cache.cc` + `serialize.cc`), Unison content-addressing
(`UNISON_IDEAS_2026-05-07.md` Item 3), the Nix binary-cache /
narinfo distribution model, and the Stage 9 kill memo
(`STAGE_9_KILLED_2026-05-22.md`) — which killed *code* dedup at
1.17× but explicitly **left the result cache open** (§7.5.3 Trigger C:
"a closer fit to 'Nix binary cache for eval results'").

## 0. What "result store" means (and what it is not)

Stage 9 (`LINKING_DESIGN_2026-05-17.md`) content-addressed **code**
(thunk-body bytecode). It was killed: byte-dedup was 1.03–1.07×
(`STAGE_9_KILLED §4`). This store content-addresses **results** — the
forced `Value` graph a `(import f) args` application denotes. Different
lever entirely: the deterministic ceiling is that firefox eval'd twice
via separate imports costs 73.0M insns vs 36.9M shared — a result-cache
hit makes eval #2 ≈ free. The `#741` in-memory `EvalResults` cache
already proved the *validation* half (shadow-mode, 0 mismatches on
hello/gcc/python3, `value_serialize.hh:169-171`). This doc is the
persistent + cross-machine tier of exactly that.

Three deployment shapes the store must serve (all one mechanism):
1. **Separate `nix` invocations, one machine** — process A populates,
   process B hits. (SQLite `EvalResults` already does this for the
   drv primop; we generalise to arbitrary imports.)
2. **Long-lived daemon** — an in-process L1 fronts the persistent tier;
   the daemon is the single writer (§3.5).
3. **A fleet** — results sync like a binary cache; a CI builder
   populates, developers substitute (§5).

---

## 1. THE PRE-COMMITTED FALSIFIER (Rule 0, first)

**Hypothesis under test.** A persistent result-store hit (open +
lookup + deserialize + relink) costs **≤ 20% of re-evaluation** on a
firefox-class result. If it costs more, the store loses to re-eval and
the design is dead — kill before building any of §2–§7.

**Why 20%.** The ceiling is 36.9M insns saved on the second firefox
eval. Deserialize+relink must be cheap relative to that. The existing
CU deserialiser costs ~1.8 ms/file (`serialize.cc`, Stage-9 memo §6);
a firefox result graph is far larger than one CU, so the naïve fear is
that materialising the whole graph costs as much as computing it. 20%
is the line below which the win is worth the architecture; between
20–50% it's marginal (ship only if RSS also wins); above 50% it dies.

**The minimal prototype (v0, ~2 days, gated `NIX_V3_RESULT_STORE_SPIKE`).**
Do NOT build the store. Build only the measurement:
1. Extend `value_serialize` to serialise a **lazy** WHNF graph — i.e.
   walk one level and, at each unforced `Tag::Thunk`/`Tag::App`, emit a
   *placeholder* (the thunk's `(desc→cu contentKey, funcIdx, upvalue
   blob)` per §2) instead of throwing `SerializeError`. Reuse
   `chaseToWHNF` (`value_serialize.cc:150`) for the forced spine.
2. Pick the firefox top-level attrset result. Serialise it once to a
   flat blob on disk.
3. In a fresh process, `mmap` the blob and **eager-materialise** the
   entire graph back into arena cells (rebuild `Bindings`/`ListVec`/
   `Thunk` via `Alloc::*` with Phase-D barriers, resolve CU
   contentKeys through the existing `disk_cache`/`aot_cache` on §2's
   relink path).
4. Measure, on darwin-4 (quiet host — the laptop's 5-10% noise floor
   exceeds this bar, per subsystem CLAUDE.md), three numbers with
   `bench/profile-at-scale.sh` methodology:
   - `T_eval` = cold eval of firefox result (the 36.9M-insn baseline).
   - `T_hit_eager` = mmap + deserialize + eager-materialise.
   - `blob_bytes` and `materialised_arena_bytes`.

**Pre-committed decision table (measured at the spike commit, git-noted):**

| `T_hit_eager / T_eval` | Verdict |
|---|---|
| ≤ 0.20 | **GO** — build v1 (eager) then v2 (lazy). Ceiling is real. |
| 0.20 – 0.50 | **CONDITIONAL** — v2 lazy-materialisation is mandatory (eager loses); ship only if §6 RSS also wins. Re-gate at v2. |
| > 0.50 | **KILL** — deserialize+relink is as expensive as re-eval; the store cannot beat re-eval. Delete the spike, write the memo (mirror `STAGE_9_KILLED`). |

**Second, RSS falsifier (same spike, cheap add).** Materialise the
firefox graph eagerly and measure resident growth vs the mmap'd blob
size. If eager-materialise RSS ≈ graph size, that is the v1 tax and
the justification for v2's page-faulting design (§2.4/§6). Pre-commit:
if `materialised_arena_bytes > 1.3 × blob_bytes`, v1 eager is
RSS-hostile at fleet scale and v2 is not optional.

**Why the spike is valid without building the store.** It measures the
*dominating* cost (deserialize + relink + materialise) directly, on
the real firefox graph, before any SQLite/mmap-segment/eviction/sync
machinery exists — exactly the Stage-9 discipline (cheap proxy that
gives a one-directional bound sufficient to kill).

---

## 2. SERIALIZING LAZY RESULT GRAPHS — the hard core

A cached pkgs graph is **mostly unforced thunks** (65.7% of thunks
never force, per the memory index / `C2_JIT_RCA`). The `#741`
serialiser (`value_serialize.hh:37-40`) *throws* on `Tag::Thunk` /
`Closure` / `App` — it only handles WHNF derivation results. To cache a
package-set result we must serialise the lazy graph, unforced thunks
and all, or we'd force the whole tree (defeating the point and changing
semantics). This is the part with no prior art in the tree.

### 2.1 The encoding — extend V3VR to a lazy graph format (V3RG)

New magic `V3RG` (V3 Result Graph), schema 1, independent of the WHNF
`V3VR` schema and of `serialize::kSchemaVersion` (18). Reuse the
LE encoders (`value_serialize.cc:59-81`) and `Reader`
(`value_serialize.cc:85`) verbatim.

The graph is serialised as an **offset-addressed node table**, not a
recursive stream, so cycles and sharing round-trip (§2.3). Each node:

```
  Node ::= tag:u8  payload
  -- WHNF leaves (reuse V3VR bodies verbatim):
  'I' Int | 'F' Float | 'B' Bool | 'N' Null
  'S' String(+sorted context) | 'P' Path
  -- structural (children are u32 node-indices, NOT inline):
  'L' List   : u32 n, nodeIdx*n
  'A' Attrs  : u32 n, (symName:strlen+bytes, nodeIdx)*n   -- sorted by name
  -- lazy (the new part):
  'T' Thunk  : cuContentKey:32, funcIdx:u32, flags:u8,
               u32 nUpvalues, nodeIdx*nUpvalues,          -- captured upvalues (recursive)
               [withsNodeIdx  iff flags&WITHS],           -- capturedWiths as a List node
               [defEnvNodeIdx iff flags&ENV_CAPTURE]      -- captured defEnv (§2.2)
  'C' Closure: cuContentKey:32, descIdx:u32, flags:u8,
               u32 nUpvalues, nodeIdx*nUpvalues,
               [withsNodeIdx iff flags&WITHS]
  'E' Env    : parentNodeIdx (or 0xFFFFFFFF=none), isWith:u8,
               u16 nValues, nodeIdx*nValues
  'V' Evaluated-thunk : nodeIdx  -- a forced thunk stores only its result
  'X' External : (see §4 taint — normally makes the frame uncacheable)
```

**`cuContentKey` is the linchpin.** A serialised thunk names its code by
the **content hash of its owning CU** — precisely the `disk_cache`
`CacheKey` (`disk_cache.hh:37`, the SHA-256 that keys the bytecode
cache) — plus a `funcIdx`/`descIdx` into that CU's `lambdas` vector.
This is Stage-9's "cell contentKey" idea (`LINKING_DESIGN §2.5`) but at
**whole-CU** granularity — which is *exactly* what Stage 9's kill memo
said survives: coarse-grained (whole-CU) is fine, fine-grained
(thunk-body) doesn't dedup. We never store bytecode in the result
store; we store a *reference* to a CU the disk/AOT cache already holds
(or can recompile from source).

Upvalues, `capturedWiths`, and `defEnv` are serialised **recursively as
child nodes** (they are themselves `Value`s / `Env`s / `ListVec`s).
`flags` mirrors `closure.hh`'s `THUNK_WITHS_SLOT` / `THUNK_ENV_SHARED`
/ `THUNK_ENV_CAPTURE` (`closure.hh:263-275`) so the exact tail layout
is reconstructed on relink.

### 2.2 The three thunk-tail variants (must all round-trip)

`closure.hh` has three physical thunk shapes; the serialiser must
distinguish them because relink rebuilds the tail:
- **inline-FAM** (default): upvalues at `tail[0..nUpvalues)`; withs at
  `tail[nUpvalues]` iff `THUNK_WITHS_SLOT`.
- **ENV_SHARED** (`thunkUpvalEnv`, `closure.hh:285`): `tail[0]` is a
  shared `Env*`; upvalues live in that Env; withs relocate to
  `tail[1]`. Serialise the `Env` as an `'E'` node; on relink, intern it
  (§2.5) so multiple thunks that shared one Env still share it.
- **ENV_CAPTURE** (`thunkCapturedDefEnv`, `closure.hh:293`): hybrid —
  residual flat upvalues in the FAM **plus** a captured parent
  `defEnv` at `tail[nUpvalues]`; withs at `tail[nUpvalues+1]`. Serialise
  both the flat upvalues and the defEnv node.

Closures mirror this via `Closure::upvalEnv` / `capturedDefEnv`
(`closure.hh:79-92`) and `closureUpvalue()` (`closure.hh:102`). The
serialiser reads through `closureUpvalue()` / `thunkUpvalEnv()` /
`thunkCapturedDefEnv()` so it is oblivious to which physical layout the
producer used, but records `flags` so relink rebuilds a *valid* layout
(it may canonicalise to inline-FAM on read — the layout is an
allocation detail, not semantic identity).

### 2.3 Cycles (letrec knots) — offset-based back-references

`let x = ...x...; in` produces `Tag::Slot` cycles and self-referential
`Env`s / recursive `Bindings`. The node-table encoding handles this the
way every serious serialiser does (and the way `serialize.cc` handles
intra-CU refs): **a node references its children by `u32` node-index,
not inline**, so a back-edge is just a smaller (or already-emitted)
index. Serialise = two-pass: (1) DFS the graph assigning each reachable
`Value`/`Env`/`Bindings` a node-index in an `IdentityMap<void*, u32>`
(pointer identity → index; this also captures **sharing**, so a DAG
stays a DAG and doesn't blow up); (2) emit nodes in index order,
children as indices. Deserialise = allocate all node cells first
(uninitialised `Tag::Uninitialized`), then wire pointers in a second
pass — so a cycle's back-edge resolves to an already-allocated cell.
`Tag::Slot` (`value.hh:56-66`) serialises as `'V'`/nodeIdx to the
pointed-at node; on relink it becomes a real `Value*` into the
reconstructed cell.

This is the CU deserialiser's own discipline (`serialize.cc` allocates
the CU then remaps in-bytecode indices in a second pass) applied to the
value graph.

### 2.4 Deserialization = RELINK (not "load")

A hit does not "load a Value"; it **relinks** a graph against the
running process's CU universe:

```
relink(blob):
  1. mmap blob (§3), read node table header.
  2. First pass: for each node, Alloc the shell cell (Bindings/ListVec/
     Thunk/Closure/Env/String) UNINITIALISED, register in nodeCells[].
     - For 'T'/'C': resolve cuContentKey → CompilationUnit:
         disk_cache.lookup(cuContentKey)          -- L2 SQLite
         ?: aot_cache.lookup(cuContentKey, TBL_CU) -- L3 mmap (aot_cache.cc)
         ?: recompile source (miss → parse+lower+emit, then re-key)
       Then desc = &cu->lambdas[funcIdx]; set thunk->suspended.desc.
       (thunkCU/desc->cu backpointer re-established at first runtime
        OP_MAKE_THUNK, per closure.hh:619-635 — TRANSIENT, not stored.)
     - Symbol/Pos identity is NOT our problem: the CU deserialiser
       already sparse-remaps SymbolIds (schema 9) and PosIdx (schema
       14) into the running process's global tables (serialize.cc
       remapSymbolsInBytecode / remapPositionsInBytecode). Attr names in
       'A' nodes are stored as STRINGS (like V3VR), interned to the
       running SymbolTable on read — same trick as the CU cache.
  3. Second pass: wire child pointers (Bindings entries, list elems,
     upvalues, Env slots, Slot targets) from nodeCells[]. Fire Phase-D
     write barriers on every tenured→cell store (closurePostConstruct /
     cellWrite) — this is a UAF-critical relink into a live moving heap.
  4. Return nodeCells[root].
```

**A CU-contentKey miss is recoverable** (recompile from source),
exactly like Stage 9's §2.5 step-5 refuse-and-recompile. If the source
is gone (store path GC'd), the whole result entry is invalid → miss.

### 2.5 LAZY MATERIALIZATION — the real RSS play (v2)

Eager relink (§2.4) makes **RSS ≈ graph size** — for a firefox package
set that is the co-primary problem (v3 is already 1.65× TW resident;
`reference_fresh_numbers_2026-06-27`). The unforced-thunk fraction
(65.7%) is *dead weight* we'd fault into anonymous memory only to never
force it. The win: **don't materialise the whole graph on a hit.**

Design: a new `Tag::External`-backed **faulting node** (`value.hh:55`
`External = 15` already exists; `mkExternal`, `value.hh` line ~242).
A relinked `Bindings`/`ListVec` entry that points at an *unforced*
subgraph gets a `Tag::External` whose payload is `(mmapSegment*,
nodeOffset)`. On `forceValue` of a `Tag::External`, the VM faults in
**just that node** (allocate the cell, wire its immediate children as
further `Tag::External` faulting nodes) and replaces the slot — a
generational-safe, on-demand relink. RSS then ≈ **demanded working
set**, not graph size: the 65.7% never-forced thunks are never
materialised; their bytes stay as evictable OS page-cache backing the
mmap (§6).

This is JVM lazy classloading + OS demand paging fused: the mmap'd blob
IS the backing store; `Tag::External` IS the fault handler. It requires
a GC-integration audit (a `Tag::External` faulting node is a root that,
when forced, allocates — same discipline as `OP_FORCE` on a
`Tag::Thunk`; the mmap segment must outlive every live `Tag::External`
that references it → refcount the segment, §3.4).

**Explicit v1 vs v2 split:**

| | v1 (eager) | v2 (lazy, page-faulting) |
|---|---|---|
| Relink | whole graph on hit | root + demanded nodes only |
| RSS | ≈ graph size (bad) | ≈ working set (the win) |
| Complexity | node-table + 2-pass wire | + `Tag::External` fault handler + segment refcounts + GC audit |
| GC risk | moderate (barriers on relink) | high (faulting nodes are roots that allocate) |
| Ships when | §1 GO + single-machine gate (§7) | §1 CONDITIONAL-or-better + v1 correct + RSS gate |

**v1 ships eager** — simplest thing that validates the CPU ceiling and
the whole key/relink/CU pipeline end-to-end. v2 is where the RSS win
lives and is the harder, GC-critical build. If §1 lands CONDITIONAL,
v1 is skipped and v2's lazy design is mandatory from the start.

---

## 3. STORE LAYOUT

### 3.1 The choice: SQLite index + mmap blob segments (hybrid)

| Option | Verdict |
|---|---|
| **Extend the `aot_cache` flat-file** (`aot_cache.cc`) | Read-only, immutable, offline-built by `bench/build-aot-cache.py`. Perfect for the *distributed snapshot* (§5, the fleet L3) but has **no write/eviction/mutation** path. Reuse its format for the shipped read-only tier; not for the mutable local tier. |
| **SQLite blobs** (like `EvalResults`, `disk_cache.cc`) | Mutable, transactional, LRU-ready (`last_used` column already exists), multi-process-safe (WAL). But blobs live *inside* SQLite pages → no `mmap` of the value graph → forces eager copy-out → **kills §2.5 lazy materialisation**. |
| **Hybrid: SQLite index + mmap'd blob segments** | **Chosen.** SQLite holds the *index* (inputKey → (segmentId, offset, len, resultHash, last_used, taint)); the blobs live in append-only `mmap`'d segment files the VM maps directly (so §2.5 can fault into them). Best of both: transactional mutable index, page-cache-evictable mmap'd graphs. |

### 3.2 Segment file format (borrowed from `aot_cache.cc` verbatim)

Reuse the `aot_cache` file discipline (`aot_cache.cc:28-32`,
`aot_cache.hh:12-24`): 8-byte magic `V3RS0001`, u32 format version,
LE-encoded, `mmap(PROT_READ, MAP_PRIVATE)`, MADV split
(`aot_cache.cc:171-197`: entry region `MADV_WILLNEED`, blob region
`MADV_NORMAL` — the exact tuning that fixed the HNE 6.83s regression).
Difference: segments are **append-only mutable** (a daemon/writer
appends new result blobs; the SQLite index points into them), and there
are **many** segments (generation files, §3.4), not one snapshot.

A blob within a segment is a self-contained V3RG node table (§2.1),
**relocatable** — every internal reference is a node-*index* not a
pointer (§2.3), and CU refs are content-keys not pointers. So a blob is
position-independent: it can be `mmap`'d at any address, copied between
segments during compaction, or shipped to another machine (§5) without
fixup. This is the one hard invariant: **no absolute pointers in a
blob, ever** — offsets and content-keys only.

### 3.3 SQLite index schema

```sql
create table ResultIndex (
    input_key   blob primary key,   -- SHA256(§4 binding key)
    result_hash blob not null,      -- canonicalHash-class hash of the graph (verify-don't-trust)
    segment_id  integer not null,   -- which V3RS segment file
    blob_offset integer not null,   -- offset within that segment (mmap-relative)
    blob_length integer not null,
    schema      integer not null,   -- V3RG schema + serialize::opcodeTableFingerprint()
    taint       integer not null,   -- 0 = cacheable; nonzero = never persisted (see §4)
    last_used   integer not null,   -- unixepoch(); LRU
    size        integer not null    -- byte cost for LRU accounting
);
create index idx_result_lru on ResultIndex(last_used);
-- Content-address the RESULT too (verify-don't-trust, §4):
create table ResultBlobs (
    result_hash blob primary key,   -- dedups identical graphs across input keys
    segment_id  integer, blob_offset integer, blob_length integer, refcount integer
);
```

Two-layer, per the binding rule: `input_key → result_hash` (the
binding) and `result_hash → blob` (the content-address). A hit
resolves `input_key → result_hash → blob`, relinks, and (in shadow
mode / periodically) **re-verifies** the relinked graph hashes back to
`result_hash`. `ResultBlobs.refcount` lets many input keys share one
identical graph and drives blob GC.

### 3.4 Eviction + concurrent readers (generation files + epoch reclaim)

The core tension: a reader has a live `Tag::External` faulting node
into segment S (§2.5); eviction wants to reclaim S. Naïve `munmap`
would UAF the reader. Solution, borrowed from LMDB/epoch-based
reclamation and Nix's own atomic-rename discipline:

- **Segments are append-only + immutable once sealed.** The writer
  appends to the *current* segment; when it hits a size cap it seals it
  and starts a new generation. Sealed segments are never mutated → any
  reader that mmap'd one sees a stable region.
- **LRU eviction is segment-granular, not entry-granular.** SQLite's
  `last_used` picks the coldest *segment* (min over its entries'
  `last_used`); eviction unlinks the segment file and deletes its index
  rows. Page-granularity within a segment is handled by the OS
  (unmapped cold pages are just reclaimed page-cache — §6).
- **Concurrent-reader safety via refcount + deferred unlink.** Each
  process holds an in-memory refcount on segments it has mmap'd
  (incremented when a `Tag::External` into that segment is live).
  Cross-process: eviction `unlink()`s the segment file but the inode
  stays alive for any process that has it open (POSIX unlink-while-open)
  — the reader's mmap keeps working until it exits; new lookups miss
  and re-populate. This is **epoch-free across processes** (the OS
  refcounts the inode) and refcount-based within a process (segment
  outlives its faulting nodes).
- **Compaction (v3+):** a background pass copies live blobs (refcount >
  0 in `ResultBlobs`) from sparse sealed segments into a fresh segment,
  rewrites the index rows, then unlinks the old segments (still safe
  for open readers via the inode trick). Blobs are relocatable (§3.2) so
  the copy is a `memcpy` + index update.

### 3.5 Multi-process write safety

- **Single-writer-per-segment via advisory lock** (`flock` on a
  `writer.lock` sidecar). A process that wants to insert takes the
  lock, appends to the current segment, commits the index row in a
  SQLite transaction (WAL, `synchronous=OFF` like `disk_cache.cc:126`
  batching), releases. Concurrent readers never block (they read sealed
  segments + the WAL-consistent index).
- **Daemon shape (deployment 2):** the daemon *is* the single writer;
  worker processes send it (inputKey, blob) over the existing daemon
  socket; the daemon owns the segment append + index. Removes the
  advisory-lock contention entirely. The in-process L1 cache
  (`value_serialize` in-memory map) fronts it.
- **Best-effort, never source of truth** (inherit `disk_cache.hh:59`):
  on any write failure (full disk, lock contention timeout, corrupt
  segment) the store silently no-ops and eval proceeds. A poisoned or
  truncated segment fails its blob-hash re-verify (§3.3) → miss →
  recompute. Corruption can never produce a wrong result, only a
  cache miss.

---

## 4. KEY DISCIPLINE + INVALIDATION + TAINT

**The binding key (verbatim from the soundness review):**

```
input_key = SHA256( schemaVersion
                  ‖ nixVersion
                  ‖ codegenGateFingerprint     -- = serialize::opcodeTableFingerprint()
                  ‖ currentSystem              -- e.g. aarch64-darwin
                  ‖ storePathOrFlakeLockPinOf(f)
                  ‖ argsHash )                 -- canonicalHash of the forced args
```

`codegenGateFingerprint` is **already implemented** as
`serialize::opcodeTableFingerprint()` (`serialize.hh:187-192`,
salted into every CU blob; a codegen change bumps it → old entries
rejected). We reuse it verbatim, so a compiler/opcode change
invalidates the result store for free — no new mechanism. `argsHash` is
`value_serialize::canonicalHash` (`value_serialize.hh:129`) over the
deep-forced args (the same hash the `#741` cache validated with 0
mismatches).

**PLUS a result content-hash verification layer (verify-don't-trust).**
Store `input_key → result_hash` and `result_hash → blob` separately
(§3.3). On a hit, relink the blob and (in shadow mode always, in
production periodically / on the first hit of a segment) recompute the
graph's structural hash and assert it equals `result_hash`. A mismatch
is a Rule-0 falsifier (hash collision or determinism bug) — bump a
counter, treat as miss, log. This is the `#741` shadow discipline
(`value_serialize.hh:141-171`, `mismatchHits` must stay 0) made
permanent and cross-process.

**HARD RULES (no persistent entry when):**
1. **Any input is a mutable working-tree path.** Only `flake.lock`
   pins or store-path pins are cacheable. A store path IS a transitive
   content commitment (its hash covers all inputs); a raw
   `/home/user/project/foo.nix` is not — its content can change under a
   fixed path. `storePathOrFlakeLockPinOf(f)` returns empty (→ empty
   key → no-op, mirroring `disk_cache.hh:37` empty-key convention) for
   any path that isn't store-pinned or lock-pinned.
2. **`__currentTime` was read** inside the cached application → the
   result is time-dependent → must not persist.
3. **`getEnv` / any impure read** was observed inside the cached frame
   → uncacheable (the result depends on ambient environment).
4. **IFD** that read a not-yet-built path — handle as `disk_cache`
   already does for CUs (block on build, then key on the built store
   path, which IS content-committed → cacheable once built,
   `LINKING_DESIGN §2.8`).

**Taint-bit plumbing.** Add a per-frame `uint8_t cacheTaint` to the
call frame (there's precedent — the retired `CFF_TAINTED` flag). The
primops that make a result unsound to persist
(`builtins.currentTime`, `getEnv`, `readFile`/`readDir` of non-store
paths, `currentSystem` is already in the key so it's fine, any impure
primop) **set the taint bit on the current frame** when invoked. Taint
**propagates up on return** (a caller whose callee was tainted becomes
tainted — `OP_RETURN` ORs the callee frame's taint into the caller's,
like exception propagation). When a top-level `(import f) args`
application returns, its accumulated taint is checked: taint ≠ 0 → do
**not** insert into the persistent store (in-memory L1 is still fine for
the process). This is conservative (over-taints — e.g. a `getEnv` in a
dead branch that was nonetheless evaluated) but sound: better a miss
than a stale hit. The set of tainting primops is small and enumerable;
a lint (mirroring `test/lint-no-inline-getenv.sh`) asserts every impure
primop sets the bit.

**Interaction with lazy graphs (§2.5):** a persisted graph may contain
unforced thunks that, *if forced later*, would read `__currentTime`.
Because taint propagates on the forced spine at *insert* time, and an
unforced thunk hasn't run, its potential taint is unknown. Conservative
rule: a thunk whose `desc` is statically known to (transitively) call a
tainting primop is itself taint-marked at compile time (a
`LambdaDescriptor::mayTaint` bit set by the lowerer's effect analysis);
any graph containing a live `mayTaint` unforced thunk is uncacheable.
This static over-approximation is the price of caching lazy graphs;
measure the hit-rate cost in v2 (§7 gate).

---

## 5. CROSS-MACHINE SHARING

### 5.1 Portability (already 90% solved by the CU cache)

The relink step (§2.4) delegates all machine-specific identity to the
existing CU deserialiser, which is **already cross-process/cross-machine
portable**:
- **Endianness:** all encoders are explicit LE bytewise
  (`value_serialize.cc:59-81`, `serialize.cc` same) — no host-endian
  assumption. Stays portable at zero cost (the existing comment,
  `value_serialize.cc:54-56`).
- **SymbolId / PosIdx:** the CU deserialiser sparse-**remaps** both into
  the loading process's global tables (`serialize.cc`
  `remapSymbolsInBytecode` schema 9, `remapPositionsInBytecode` schema
  14). Result-graph attr names are stored as strings (§2.1 `'A'`) and
  interned on read — identical trick. **No serialised pointer, SymbolId,
  or PosIdx is machine-specific.**
- **`currentSystem` is in the key** (§4) — an `aarch64-darwin` result
  and an `x86_64-linux` result have different `input_key`s and never
  collide. Cross-*platform* results are correctly distinct entries; a
  fleet of same-platform machines shares.
- **Store-path contexts** (string context `!out!drv` / `=drv` / `path`,
  `value_serialize.hh:24-31`) are already canonical store paths —
  portable as-is; they remap to the receiving machine's store the same
  way NAR contexts do (the paths are content-addressed).

64-bit is assumed throughout (v3 is 64-bit only — `value.hh` 48-bit
NaN-box payload assumes canonical 48-bit user pointers). Documented, not
a portability variable.

### 5.2 Distribution model — sync like a binary cache

The **read-only distributed tier reuses the `aot_cache` flat-file
format** (§3.1): a builder runs the fleet's canonical evals, seals the
result segments, and publishes a single immutable snapshot file (the
`aot_cache` builder `bench/build-aot-cache.py` already produces exactly
this shape for CUs — extend it to emit a `TBL_RESULT` table alongside
`TBL_CU`/`TBL_EVAL_RESULT`, `aot_cache.hh:60-63`). Consumers set
`NIX_V3_RESULT_STORE_SNAPSHOT=<path>` and get an L3 mmap'd read-only
tier below their mutable local SQLite tier, exactly mirroring the
existing L1/L2/L3/L4 cache hierarchy (`aot_cache.hh:29-35`).

Distribution itself is **narinfo-style**: an index file
(`input_key → result_hash + segment URL + length`) is fetched over
HTTP(S) like a binary-cache narinfo; blobs are substituted on demand by
`result_hash` (content-addressed, so a blob is fetched once and shared
across all input keys that map to it). This is deliberately the Nix
binary-cache protocol shape so it can ride existing substituter infra
(the fleet's binary cache already distributes build outputs; eval
results are just another content-addressed artifact).

### 5.3 Signing / trust (cache poisoning)

A cross-machine result cache is a **remote-code-adjacent trust
surface**: a poisoned result blob that relinks to a graph whose thunks,
when forced, produce attacker-chosen derivations = supply-chain
compromise. Mitigations, borrowed directly from the binary-cache
precedent:
- **ed25519 signatures** over `(input_key, result_hash)` pairs, exactly
  like `nix-store --sign` / narinfo `Sig:` lines. A consumer trusts a
  snapshot only if signed by a configured public key
  (`trusted-public-keys` analogue). Reuse libstore's signing.
- **Verify-don't-trust re-hash (§4) is the second line:** even a signed
  blob is relinked and re-hashed against `result_hash`; a blob that
  doesn't hash to its claimed `result_hash` is rejected. Signing
  attests the `input_key → result_hash` *binding*; the content-address
  attests the *blob*. An attacker must both forge a signature AND find a
  hash collision.
- **CU provenance:** a relinked thunk names a CU by content-key; if that
  CU isn't already in the local trusted cache, it's recompiled from
  local source (§2.4) — the remote cache can't inject bytecode, only
  *reference* code the consumer independently compiles. This is the
  strongest property: **the result store distributes no executable
  code**, only forced values + references to locally-recompilable CUs.

---

## 6. RSS ACCOUNTING

The co-primary question: "how much more memory does this eat at fleet
scale?" The answer hinges on mmap page-cache vs anonymous memory.

- **mmap'd blob pages are `MAP_PRIVATE, PROT_READ` file-backed** (like
  `aot_cache.cc:124`). They count as **page cache**, not anonymous RSS.
  Under memory pressure the OS **evicts clean file-backed pages for
  free** (no swap write — they're backed by the segment file). So a
  faulting-node graph (§2.5) that references 500 MB of segment but only
  demands 50 MB of nodes costs ~50 MB of *materialised* anonymous arena
  cells + whatever segment pages the OS keeps hot; the cold 450 MB is
  reclaimable page cache. **This is the entire RSS argument for v2 over
  v1.** v1 eager copies the whole graph into anonymous arena → RSS =
  graph size → hostile at fleet scale.
- **MADV strategy** (reuse `aot_cache.cc:171-197`): index/entry region
  `MADV_WILLNEED` (binary search touches log₂N scattered entries — cheap
  to prefault); blob region `MADV_NORMAL` (adaptive read-ahead handles
  "skip between offsets + sequential within a node table"). For v2, add
  `MADV_DONTNEED` on segments whose in-process refcount drops to 0 (all
  faulting nodes into them forced-and-replaced or dead) to actively
  return the page cache.

**Measurement protocol (pre-committed, darwin-4):**
1. Report page-cache vs anonymous split explicitly. macOS: `footprint`
   / `vmmap` phys_footprint separates file-backed from anonymous; the
   existing `NIX_VM_STATS` RSS number is total resident — split it.
2. Cold firefox eval (baseline RSS, no store). Warm firefox eval WITH
   the store (v1 then v2). Report: total RSS, anonymous RSS, mapped
   segment bytes, materialised arena bytes.
3. **v2 SHIP gate:** warm-with-v2 *anonymous* RSS ≤ cold baseline RSS
   (the store must not grow the anonymous working set — the whole point
   is that unforced subgraphs stay in evictable page cache). Mapped
   segment bytes may be large but are reclaimable → don't count against
   the gate, but MUST be reported so fleet operators can size disk.
4. **Fleet framing:** the honest answer to "how much more memory" is
   "~X MB anonymous per live eval + Y MB reclaimable page cache shared
   across all processes on the box (the segment files are shared via the
   OS page cache — N processes mapping the same segment pay the pages
   once)." The shared-page-cache property is the fleet win: 100 CI
   evaluators on one box hitting the same nixpkgs result set share one
   copy of the hot segment pages.

---

## 7. PHASING (each phase a pre-committed kill/ship gate)

Every phase is commit-mergeable and falsifies one hypothesis (Rule 0).

**v0 — Falsifier prototype** (~2 days, `NIX_V3_RESULT_STORE_SPIKE`).
§1 exactly. Serialise firefox graph lazily-encoded, eager-materialise
in a fresh process, measure `T_hit_eager / T_eval` + RSS.
**Gate:** §1 decision table. > 0.50 → KILL (write the memo, stop).
≤ 0.50 → proceed; ≤ 0.20 → v1-then-v2, 0.20–0.50 → skip v1, v2 lazy is
mandatory.

**v1 — Eager, single-machine, SQLite-indexed** (~1.5 wk,
`NIX_V3_RESULT_STORE=1` default-off). §2.1–2.4 encoding + relink; §3.3
SQLite index + one growable segment; §4 key + taint bit + hard rules.
Eager materialise (RSS = graph size, accepted for v1). Shadow-mode
re-verify always on.
**Ship gate:** on hello/git/firefox/M5, cross-process warm hit gives
≥ the §1-measured CPU win AND `mismatchHits == 0` across all four
under full `--brute` + byte-identity. **Kill gate:** any mismatch, or
hit-rate < 30% (the `#741` Phase-3a bar, `value_serialize.hh:169`)
after taint over-approximation.

**v2 — Lazy materialisation + mmap segments** (~3 wk,
GC-critical). §2.5 `Tag::External` faulting nodes; §3.2 mmap'd
append-only segments; §3.4 segment-granular LRU + refcount/inode
eviction; §6 MADV_DONTNEED. This is the RSS win and the hard GC build.
**Ship gate:** §6 SHIP gate (warm anonymous RSS ≤ cold baseline) AND
full `--brute` 22/22 under moving-GC stress (faulting nodes are
roots-that-allocate — this MUST pass the aggressive `V3_DBG_NURSERY_*`
brute). **Kill gate:** anonymous RSS win < 15% vs v1 (mirrors the
`aot_cache` retirement bar `aot_cache.hh:40-44`), OR any missed-root
UAF the brute can't be made to pass → v2 dies, v1 (CPU-only, no RSS
win) is the shipped ceiling.

**v3 — Cross-machine sync + signing** (~2 wk). §5: extend
`bench/build-aot-cache.py` to emit `TBL_RESULT` snapshots; L3 read-only
mmap tier (`NIX_V3_RESULT_STORE_SNAPSHOT`); ed25519 sign/verify;
narinfo-style HTTP index + content-addressed blob substitution.
**Ship gate:** a builder-populated snapshot gives a cold consumer the
same CPU win as a local warm hit, signatures verify, and a
deliberately-poisoned blob (wrong `result_hash`) is rejected (100% —
this is a security gate, not a perf gate). **Kill gate:** signature/
re-hash overhead pushes the hit path back over the §1 20% line, OR the
snapshot is too platform-fragmented to share (measure fleet hit-rate
across real machine mix).

---

## 8. RISKS RANKED + OPEN QUESTIONS

**Risks (highest first):**

1. **v2 GC integration (faulting nodes are roots that allocate).** A
   `Tag::External` fault handler that allocates during `forceValue` in a
   moving nursery is the exact PhD-6 UAF class the subsystem CLAUDE.md
   Constraint 0 warns about. The mmap segment must outlive every live
   faulting node (§3.4 refcount) and the walker/scavenger must treat a
   `Tag::External` graph-node as an opaque leaf until forced. **This is
   the single most likely thing to kill v2.** Mitigation: v2's gate is
   the full `--brute` under stress; if it can't pass, v2 dies and v1
   (CPU-only) ships.

2. **Taint over-approximation tanks the hit rate.** The static
   `mayTaint` conservatism (§4) may mark too many nixpkgs thunks
   uncacheable (nixpkgs `getEnv`s / `builtins.currentTime` in `lib` are
   pervasive). If it does, most package-set results become uncacheable
   and the store is useless. **Measure hit-rate-after-taint in v1's kill
   gate (< 30% kills).** Open: can effect analysis be precise enough
   (per-attr rather than whole-graph taint)?

3. **Lazy graph serialisation is genuinely novel** (no in-tree prior
   art; Stage 9 only did WHNF). Cycle/sharing/three-tail-variant
   round-trip (§2.2/2.3) is intricate and UAF-adjacent. Mitigation: the
   node-table 2-pass discipline is battle-tested (the CU deserialiser
   uses it); heavy property tests (force-idempotence, sharing
   equivalence — debug story items 7).

4. **Blob-hash re-verify cost.** Re-hashing a relinked graph on every
   hit (§4 verify-don't-trust) may itself cost > 20%. Mitigation:
   re-verify per-*segment* once (first hit warms it), not per-entry;
   shadow-mode always, production periodically. Measure in v1.

5. **Store growth / eviction correctness.** Segment-granular LRU +
   unlink-while-open (§3.4) is subtle across the daemon + N workers.
   Mitigation: best-effort semantics (§3.5) — worst case is a miss,
   never a wrong result; the re-hash catches any relink of a
   partially-unlinked segment.

**Open questions:**

- **Q1.** Graph granularity: cache per `(import f) args` application, or
  finer (per-attr of a package set)? Finer = more hits + more index
  rows + more taint precision needed. **Decide from v0 data** (does the
  firefox root-attrset cache as one blob usefully, or must sub-attrs be
  independently keyed?).
- **Q2.** `result_hash` function: reuse `value_serialize::canonicalHash`
  (SHA-256 over V3VR)? But V3RG (lazy) ≠ V3VR (WHNF) — an unforced graph
  has no canonical WHNF hash. Need a *structural* hash over the V3RG
  node table that is stable under sharing/allocation-layout differences.
  **Design before v1.**
- **Q3.** Does `argsHash` (§4) need the args deep-forced (expensive,
  changes laziness) or can it hash the unforced arg graph structurally
  (like Q2)? Deep-forcing args to key them may itself cost more than the
  saved eval. **Measure in v0.**
- **Q4.** Interaction with the in-process spike being built in parallel:
  is the persistent store's L1 literally the spike's in-memory map, or a
  separate tier? **Coordinate — they should share the key + hash code.**
- **Q5.** Daemon protocol for deployment 2 — new socket op or piggyback
  on the existing daemon RPC? **Defer to v1-daemon variant.**

---

Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
Input Output Group. SPDX-License-Identifier: Apache-2.0.
