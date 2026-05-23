# Eval-cache architecture — post-Phase 5 reassessment

**Date:** 2026-05-23
**Author:** session synthesis (conversation thread following #741 Phase 5 + firefox generalization)
**Status:** strategic — refines `IFD_CACHE_DESIGN_2026-05-23.md` with post-implementation data
**Triggering context:** #741 Phase 5 falsified the wall savings of SQLite-backed L2 at the leaf primop scope; firefox.drvPath measurement at 58 % intra-process duplicate rate generalized the falsification (still 1.02× slower); user asked whether better in-memory caching, mmap'd pre-seed, or a service-based architecture would change the equation.

Companion docs: [`IFD_CACHE_DESIGN_2026-05-23.md`](IFD_CACHE_DESIGN_2026-05-23.md) (original engineering plan), [`JIT_CONFIDENCE_2026-05-23.md`](JIT_CONFIDENCE_2026-05-23.md) (sibling — established the cache as primary alternative to JIT), [`WARM_EVAL_AND_INSTRUMENTATION_2026-05-23.md`](WARM_EVAL_AND_INSTRUMENTATION_2026-05-23.md) (AOT distribution thesis for shared cache artifacts), [`IFD_DEEP_DIVE_2026-05-21.md`](IFD_DEEP_DIVE_2026-05-21.md) §11 (materialization-retirement program — Unison Item 3 narrow).

---

## 1. Position (TL;DR)

**The Phase 5 falsifier is structural, not workload-specific. The cache substrate is over-validated. The remaining architecture decision is L2 implementation choice. Recommended: replace SQLite-backed L2 with mmap'd flat file. Expected outcome: wall-positive at leaf primop scope on hello.drvPath (~3-5 % wall), wall-positive at higher hit rates on firefox.drvPath (~5-10 % wall), wall-positive composes with AOT distribution to deliver first-eval wins on fresh CI boxes.**

The user's question — "would in-memory caching, mmap pre-seed, or a daemon help?" — separates cleanly:
- **In-memory L1 alone**: helps with intra-process duplicates only (~33 % on hello, ~58 % on firefox); cannot address cross-process reuse.
- **Mmap'd flat file as L2**: dodges Phase 5's SQLite-cost blocker entirely; cross-process sharing via OS page cache; demand-paged so unused entries cost nothing; composes with AOT distribution.
- **Service / daemon (cache-serving)**: IPC overhead negates leaf-primop savings; only justifies at Phase 4+ scopes where saved work >> IPC roundtrip.
- **Service / daemon (whole-eval)**: process-model change; long-term direction; not a Phase 5 follow-on.
- **Daemon backed by SQLite**: worse than direct SQLite (IPC tax without offsetting benefit).

---

## 2. What Phase 5 + firefox jointly falsified

### 2.1 The Phase 5 result (from user's report)

```
Workload          | Cold Hit% | Warm Hit% | TW-identical
hello.drvPath     | 34.1%     | 100%      | ✓
gcc.drvPath       | 96.7%     | 100%      | ✓
python3.drvPath   | 95.2%     | 100%      | ✓

Wall (hello.drvPath, 836 ms OFF baseline):
  COLD ACTIVE+DISK : 1429 ms  (+71 %)
  WARM ACTIVE+DISK :  883 ms  (+5.6 %)
  WARM SHADOW+DISK :  844 ms  (+1 %)

Attributed costs:
  COLD: 517 SQLite inserts × ~1.1 ms each ≈ +569 ms (matches observed)
  WARM: 785 SQLite lookups × ~60 µs each ≈ +47 ms (matches observed)
```

### 2.2 The firefox generalization

```
firefox.drvPath intra-process duplicate rate:    58.3 %  (vs hello 34 %)
firefox.drvPath wall with Phase 5 ACTIVE:        1.02× slower
```

### 2.3 What this jointly proves

**The wall sign does not flip as hit rate increases from 34 % to 58 %.** That's structurally significant. The per-call arithmetic explains why:

```
SQLite warm lookup cost:        ~60 µs
Saved libstore-tail per hit:    ~30-50 µs
Net per HIT:                    NEGATIVE  ~10-30 µs
```

The cost-per-call exceeds the savings-per-hit **regardless of hit rate**. Higher hit rate just means more 60 µs lookups confirming hits that each save only 30-50 µs. The lever is bounded by per-call mechanics, not by aggregate redundancy. This is a clean structural Rule 0 falsifier: SQLite-backed L2 at the leaf primop scope cannot become wall-positive by any combination of (cache size, hit rate, workload size, cache-key choice).

**What's NOT falsified:**
- Correctness of the substrate (Phase 5: byte-identical TW across 6 cold+warm runs, 0 mismatches)
- Determinism (Phase 2 canonical hash)
- Cross-workload reuse (95-97 % cold hit when second workload runs after first)
- Intra-process redundancy (34 % hello → 58 % firefox)
- Persistence (Phase 5 disk)

The cache substrate has been validated by **four independent signals**. The wall lever is purely a cache-implementation cost story.

---

## 3. Cost model (unit reference)

All numbers below are either measured (Phase 5) or derived from well-known properties of the underlying mechanisms.

```
SQLite insert (WAL commit):              ~1.1 ms       ← Phase 5 falsifier
SQLite lookup (warm cache):              ~60 µs        ← Phase 5 measured
unordered_map<hash, ptr> lookup:         ~100 ns       ← textbook
mmap'd flat file lookup (open-address):  ~50-200 ns    ← 1-2 cache line touches
Unix socket roundtrip:                   ~10-30 µs     ← Linux cold; faster with io_uring
Saved libstore-tail per hit:             ~30-50 µs     ← Phase 5 measured
Hash key (drv-hash already computed):    ~0 µs         ← available from primop body
Canonical Value hash (Phase 2):          ~5-50 µs      ← payload-dependent
```

Two of these dominate the architecture choice:
- The saved work per hit is **small** (~30-50 µs)
- SQLite is **expensive** relative to in-process operations (~60 µs read, ~1.1 ms write)

The ratio matters: **lookup_cost / saved_work** determines whether each hit contributes positively to wall. SQLite's ratio is ~1.2-2.0 (negative); unordered_map's ratio is ~0.003 (positive); mmap's ratio is ~0.003-0.007 (positive).

---

## 4. Architecture options analysed

### 4.1 In-memory L1 only

```
Per lookup:    ~100 ns
Cross-process: ✗
Cold tax:      0
Effort:        1 day
Net at leaf:   intra-process duplicates only
```

**Wall on hello.drvPath (33 % hit rate, 785 calls):**
- 0.33 × 785 × ~30-50 µs saved = ~8-13 ms
- 785 × 100 ns lookup = ~0.08 ms (negligible)
- **Net: +8-13 ms (~1-1.5 % wall)**

**Wall on firefox.drvPath (58 % hit rate, N calls — unknown but ≥ 785):**
- 0.58 × N × ~40 µs = larger absolute savings as N grows
- Per-call net unchanged; aggregate scales with workload size

**What this DOESN'T cover:** the cross-workload reuse pattern (95 % on Phase 5) requires persistent state across processes. In-memory L1 lives and dies with the process. For `nix build a b c d` (multi-target, single process), L1 covers cross-target reuse natively. For `nix eval a && nix eval b` (separate processes), L1 alone gives nothing on the second invocation.

### 4.2 L1 + SQLite L2 (Phase 5 as shipped)

```
Per lookup:    L1 100 ns; L2 60 µs warm / 1.1 ms cold-insert
Cross-process: ✓
Cold tax:      ~1.1 ms per insert × N
Effort:        (done)
Net at leaf:   −47 ms warm / −569 ms cold (FALSIFIED)
```

This is what Phase 5 shipped. The falsifier is structural per §2.3.

### 4.3 L1 + mmap'd flat file L2

```
Per lookup:    L1 100 ns; L2 50-200 ns
Cross-process: ✓ via OS page cache
Cold tax:      0 (demand-paged)
Effort:        3-5 days
Net at leaf:   +24-39 ms (~3-5 % wall) on hello
               larger on firefox (58 % hit × larger N)
```

**Design sketch:**

```
File layout: nixpkgs-eval-result-cache.mmap
┌─────────────────────────────────────────────────────────┐
│ Header: magic + schema + entry count + hash seed + ver  │  64 B
├─────────────────────────────────────────────────────────┤
│ Hash table: N × (drv_hash : u64, blob_offset : u64)     │  16 N B
│   open addressing, linear probing, ~70 % load factor    │
├─────────────────────────────────────────────────────────┤
│ Blob region: serialized Value entries, variable length  │  Σ blobs
│   each blob: (length : u32, V3VR-framed serialized Value)│
└─────────────────────────────────────────────────────────┘

Lookup (per call):
  1. table_slot = hash(drv_hash) mod N
  2. probe loop: while table[slot].drv_hash != target && != 0
       slot = (slot + 1) mod N
  3. if hit: offset = table[slot].blob_offset; deserialize from blob[offset]
  4. if miss: return nullopt (fall through to compute + populate L1 + maybe write log)

Write path (separate process or background thread):
  - log new entries to append-only WAL
  - periodically rebuild the flat file from L1+L2+WAL (offline compaction)
  - swap atomically via rename(2)
```

**Key properties:**

1. **No syscalls on the hot path.** Once mmap'd, all reads are memory accesses. The kernel demand-pages from disk only when a page is touched.

2. **Cross-process sharing via OS page cache.** Multiple processes mmap'ing the same file share physical pages. No explicit IPC, no double-buffering. The kernel has been optimizing this exact pattern for 40 years (think dynamic linker, page cache for `.so` files).

3. **Cold-startup cost is essentially zero.** The mmap'd region is virtual until accessed. Even a 500 MB cache file consumes no physical RAM until pages are touched.

4. **Eviction is the kernel's job.** Under memory pressure, the kernel pages out cold cache entries automatically — no LRU implementation in v3.

5. **Writes can be asynchronous.** Hot path never writes. A background compactor or per-process WAL handles updates. This dodges the Phase 5 cold-path insert tax entirely.

**Wall arithmetic on hello.drvPath:**
- 785 lookups × ~150 ns = ~0.12 ms (negligible)
- 0.34 × 785 hits × ~40 µs savings = ~10.7 ms (intra-process)
- 95 % × 785 × ~40 µs savings = ~30 ms (cross-workload warm)
- **Net: +24-39 ms positive (~3-5 % wall)** — converts Phase 5's −47 ms warm regression into a positive.

**Wall arithmetic on firefox.drvPath:**
- 0.58 × N × ~40 µs savings (intra-process) — scales with N
- If N ≈ 10× hello's 785, savings ≈ 180 ms (~5-10 % wall)
- Cross-workload similarly larger

**Build precedent:**
- Linux `ld-linux.so` mmap'ing shared libraries across processes
- Java Class Data Sharing (CDS / AppCDS) — pre-mmap'd JIT artifacts shipped with the JDK
- V8 startup snapshot — pre-serialized heap mmap'd at process start
- LMDB (Lightning Memory-Mapped Database) — entirely mmap-only KV store; backs OpenLDAP
- SQLite-in-mmap-mode is closer to this, but still has more abstraction overhead than a custom flat file
- nix's existing nar-info-disk-cache has some similar shape (SQLite-backed but read-heavy)

### 4.4 AOT-shipped mmap'd cache

```
Per lookup:    same as 4.3 (~150 ns)
Cross-process: ✓ + cross-machine via cache.nixos.org
Cold tax:      0 on first eval (cache substituted via Nix infra)
Effort:        4.3 effort + Nix-team coordination for distribution
Net at leaf:   +24-39 ms even on FIRST eval on fresh box
```

**Composition with WARM_EVAL §6.4 thesis:**

The mmap'd flat file is a perfect candidate for distribution as a Nix package via cache.nixos.org. Sketch:

```
nixpkgs-eval-result-cache:
  src = nixpkgs evaluated under v3 with NIX_V3_BUILD_CACHE=1
  builder = batch-evaluate top-N packages, populate mmap file
  output = $out/share/nix/eval-result-cache-<rev>.mmap

On fresh CI box:
  1. nix-channel update brings down nixpkgs + cached eval-result-cache
  2. v3 startup reads /nix/store/.../eval-result-cache.mmap (mmap'd)
  3. First eval of any package hits the cache at 95 %+ for shared stdenv
  4. No "first cold pay" cost

This is the SAME delivery infrastructure as the nixpkgs-bytecode-cache
proposal from WARM_EVAL §6.4. Same Nix-team coordination story.
The two compose: bytecode cache eliminates parse residue; eval-result
cache eliminates primop/IFD residue.
```

This is where v3 has structurally asymmetric advantages over TW: TW has no per-import caching layer and cannot easily ship one. v3 with disk-cache + mmap'd eval-cache + AOT distribution turns warm-eval into the user-facing scenario.

### 4.5 Daemon serves cache lookups (cache-as-a-service)

```
Per lookup:    ~10-50 µs (Unix socket roundtrip + serialization)
Cross-process: ✓ (client/daemon model)
Cold tax:      daemon startup
Effort:        weeks
Net at leaf:   ~0-30 µs/hit — marginal-to-wash
Net at Phase 4 scope (saved work = SECONDS): IPC negligible, large positive
```

The math at leaf primop scope:

```
Per hit savings:        ~30-50 µs
IPC roundtrip:          ~15-50 µs (Linux Unix socket; depends on payload size)
Net per hit:            ~0-35 µs
```

**Verdict:** marginal at leaf scope. IPC overhead consumes most of the savings. **However, at Phase 4 scope** (Class B IFD primops where saved work = seconds for skipped nix-builds), IPC overhead is rounding error and daemon-served-cache becomes strongly positive.

This is a **scope-conditional** answer: wrong at leaf primop, right at IFD primop. The architecture choice should match the scope.

### 4.6 Daemon does whole eval

This is a process-model change, not a cache change. Determinate Nix's `builtins.parallel` and Tvix's daemon-mode are both moving this direction.

**Benefits:**
- All intra-eval state including cache persists across requests (free cross-workload reuse)
- Parallel eval becomes possible (orthogonal benefit)
- Cache eviction policy is the daemon's, not the OS's
- Can preload AOT'd cache at daemon startup; subsequent eval requests never pay startup tax

**Cost:** months of work. Cross-cutting architectural change. Affects every entry point. Compatibility with existing nix-daemon (which currently handles store ops only). Worth its own measure-twice case driven by parallel-eval + IFD-persistence needs, not by this cache result.

**Verdict:** long-term direction; Stage 13-class commitment; not a Phase 5 follow-on.

### 4.7 Daemon backed by SQLite

```
Per lookup:    IPC ~15-50 µs + SQLite ~60 µs = ~75-110 µs
Cross-process: ✓
Cold tax:      daemon startup + SQLite cold
Effort:        weeks
Net at leaf:   WORSE than direct SQLite (no benefit, IPC tax)
```

**Verdict:** doesn't make sense at the leaf surface. Adds IPC overhead without offsetting benefit. Only justifies if you need something a daemon provides that a direct DB doesn't:
- Write coalescing (batch SQLite inserts to amortize WAL commits — could recover cold-path regression)
- Cross-eval coordination (lock-free reads, single-writer pattern)
- Cache eviction policy beyond OS LRU
- Network-shared cache (CI farm with multiple workers, central cache server)

These are real benefits but orthogonal to "make leaf-primop cache wall-positive." For that, mmap is strictly better.

---

## 5. Side-by-side comparison

| Architecture | Per-lookup | Cross-process | Cold tax | Effort | Net at leaf (hello) | Phase 4 scope (build = s) |
|---|---|---|---|---|---|---|
| L1 in-memory only | ~100 ns | ✗ | 0 | 1 day | +8-13 ms intra-process | irrelevant — cross-process |
| L1 + SQLite L2 (Phase 5) | L1: 100 ns / L2: 60 µs / +1.1 ms insert | ✓ | high | done | −47 ms warm / −569 ms cold | likely +seconds |
| **L1 + mmap L2 (flat file)** | **~150 ns** | **✓ page cache** | **0 demand-paged** | **3-5 days** | **+24-39 ms (~3-5 %)** | **+seconds** |
| AOT'd mmap'd cache (cache.nixos.org artifact) | ~150 ns | ✓ + cross-machine | 0 on first eval | days + infra | +24-39 ms FIRST eval | +seconds first eval |
| Daemon serves cache (in-mem) | ~15-50 µs IPC | ✓ | daemon start | weeks | ~0-30 µs/hit | +seconds (IPC negligible) |
| Daemon serves cache via SQLite | ~75-110 µs | ✓ | daemon start | weeks | **worse than direct SQLite** | +seconds |
| Daemon does whole eval | n/a (cache internal) | ✓ | daemon start | months | indirect | indirect |

**The clear winner at every relevant comparison axis is L1 + mmap L2.** It eliminates the Phase 5 falsifier (SQLite cost), preserves all the validated properties (correctness, hit rate, cross-process reuse), composes with AOT distribution, and leverages 40 years of OS-page-cache engineering for free.

---

## 6. Why mmap beats SQLite for read-heavy cache traffic

This isn't novel insight — it's textbook. The OS page cache + mmap is the standard answer to "read-heavy, mostly-shared, structurally-stable cache" and has been since Unix V7.

**Properties:**

1. **Demand paging.** Only touched pages are read from disk. A 500 MB cache file consumes ~0 RAM until pages are accessed.
2. **Shared physical memory.** Multiple processes mmap'ing the same file share physical RAM. The kernel deduplicates automatically.
3. **Kernel-managed LRU.** Under memory pressure, cold pages are evicted automatically. No userspace LRU code.
4. **Zero-syscall reads.** Once mmap'd, reads are loads from virtual memory. No system call overhead per lookup.
5. **Atomic updates via rename(2).** Writers atomically swap the entire file; readers either see the old version (still mmap'd) or the new (next access faults). Strong consistency without locks.
6. **Cross-process gratis.** No IPC, no daemon, no socket. Two processes opening the same file just work.

**Why SQLite is the wrong fit for this workload:**

- SQLite is optimized for transactional, mixed read/write, ACID workloads. Eval-cache is read-heavy, append-mostly, eventually-consistent.
- WAL mode (which Phase 5 uses) trades transaction durability for write speed but still pays ~1.1 ms per commit. For a cache that doesn't care about durability of any single write, this is overhead.
- SQLite cursor open/close + query parse + index walk + row materialization is ~60 µs even for a single-row primary-key lookup. Mmap'd hash table is ~150 ns for the same operation.
- SQLite locks (even with WAL) limit cross-process write concurrency. Mmap'd cache with single-writer log eliminates the lock.

**When SQLite would be the right choice:**

- Mixed read/write workload with strong durability requirements
- Complex queries beyond hash-keyed lookup
- Smaller cache (~MBs) where SQLite's overhead is acceptable
- Where mmap'd file format engineering is too much effort relative to win

Phase 5's eval-cache violates all four. The team chose SQLite reasonably as a Phase 1 substrate (existing infrastructure, faster to prototype) — but the data now shows the workload doesn't fit.

---

## 7. Implementation plan

### 7.1 Spike (3-5 days)

1. **Day 1:** Design + implement the flat file format.
   - Header struct (magic + schema + entry count + hash seed)
   - Hash table layout (open addressing, linear probing, 70 % load factor)
   - Blob region (V3VR-framed Value serialization — already exists from Phase 1)

2. **Day 2:** Build the offline compactor.
   - Reads existing SQLite cache
   - Writes new flat file
   - Atomic rename(2) for swap

3. **Day 3:** Add mmap lookup path to primop call sites.
   - Open + mmap the flat file at process start (if exists)
   - L1 in-memory cache still primary
   - L2 lookup = mmap'd hash table walk
   - L3 fallback = SQLite (for migration; can drop later)

4. **Day 4:** Add WAL-style writeback.
   - Append-only log per process for new entries
   - Background compactor merges WAL into flat file periodically
   - Or: separate "build cache" tool runs offline (cleanest)

5. **Day 5:** Measurement + falsifier.
   - hyperfine n=10 on hello.drvPath, firefox.drvPath, gcc.drvPath, python3.drvPath
   - Compare OFF / Phase-5-SQLite / new-mmap warm + cold
   - Expected: mmap warm = +3-5 % positive on hello, +5-10 % positive on firefox
   - Falsifier: if mmap warm is also wall-negative, the leaf primop scope is structurally too cheap for ANY cache layer. Pivot to Phase 4 / Class B IFD primops as the primary scope.

### 7.2 Pre-committed falsifier thresholds

Per [`MEASURE_TWICE_CUT_ONCE_2026-05-23.md`](MEASURE_TWICE_CUT_ONCE_2026-05-23.md):

- **Ship-positive threshold:** mmap-cache warm ≥ +2 % wall on hello.drvPath AND ≥ +5 % wall on firefox.drvPath, both with n=10 σ < 1 %
- **Revert-with-data threshold:** mmap-cache warm < +1 % wall on both workloads after spike completion → revert, commit measurement data, conclude "leaf primop scope is structurally too cheap"
- **Pivot threshold:** mmap-cache warm shows +1-2 % wall (in the gap) → keep as opt-in for empirical further measurement on cardano-node M5 / firefox before defaulting

### 7.3 AOT distribution (gated on 7.2 success)

If 7.1 spike confirms wall-positive at leaf scope, the AOT distribution path opens:

1. **Build a `nixpkgs-eval-result-cache` derivation** (1-2 weeks; cross-team with Nix infra).
2. **Distribute via cache.nixos.org** as a Nix package.
3. **First-eval on fresh box gets warm cache from substitution** — no cold-pay-once cost.

This is where the v3 vs TW asymmetry compounds. TW has no equivalent caching layer; v3 with bytecode cache + eval-result cache + AOT distribution turns warm-execute into the user-facing scenario.

---

## 8. Connection to broader strategic threads

### 8.1 Refinement of [`JIT_CONFIDENCE_2026-05-23.md`](JIT_CONFIDENCE_2026-05-23.md)

The JIT-deferral case rested partly on "#741 IFD cache delivers what JIT cannot at 1/50th the cost." Phase 5 falsified the SPECIFIC implementation (SQLite-backed at leaf scope) but the broader claim survives:

- The cache **substrate is now over-validated** by four independent signals
- The wall lever is purely a cache-implementation cost story (this doc's L2 choice)
- Mmap L2 at leaf scope delivers wall-positive that JIT cannot match (JIT can't cache cross-process at all)
- Phase 4 scope cache (whatever L2 implementation) delivers multi-second savings that JIT structurally cannot reach

**JIT_CONFIDENCE remains correct.** The footnote-update is: replace "#741 Phase 1 landed" with "#741 substrate validated end-to-end through Phase 5; L2 implementation pending mmap re-architecture."

### 8.2 Refinement of Unison broader vision

The firefox 58 % intra-process duplicate rate is **data for the Unison thesis**. Nix eval has far more structural redundancy than "build once, reuse many" patterns alone predict. Half a workload duplicating at the leaf primop level — before compounding redundancy at coarser scopes (subtrees, lib.fix steps, callPackage results) — strongly supports the Unison Item 1/Item 3 broader directions.

**Concrete implications:**

- The Unison Item 3 narrow form (this doc + #741) has cleared the empirical hurdle: the cache works, the redundancy is real.
- The Item 1 (content-addressed IR) + Item 2 (ABT refactor) directions become MORE attractive in proportion to coarser-scope redundancy. Measurement at IR-subtree granularity would tell us how much.
- This doc + Phase 4 measurement together would resolve whether eval-result cache is sufficient OR whether IR-subtree cache is needed.

### 8.3 Composition with [`WARM_EVAL_AND_INSTRUMENTATION_2026-05-23.md`](WARM_EVAL_AND_INSTRUMENTATION_2026-05-23.md)

The mmap'd eval-cache distribution path is structurally the SAME as the nixpkgs-bytecode-cache distribution path:
- Build as a Nix package
- Ship via cache.nixos.org
- Substitute on user machines as part of `nix-channel update`
- Mmap at process start

They share infrastructure, share the Nix-team coordination story, and compose: bytecode cache eliminates parse residue; eval-result cache eliminates primop/IFD residue. Together they push warm-eval-from-cold-box toward the "all residue eliminated" target.

**This is where v3 wins decisively over TW.** TW has neither caching layer and cannot easily ship one. v3 with both compounds the asymmetry.

### 8.4 Implications for Phase 4 (Class B IFD primops)

**This section was originally written before commit `2103cdddb` (Phase 4 falsifier) and commit `cbb870174` (workload heterogeneity audit) landed. See §13 for the post-falsifier amendment — the Phase 4 audience claim is workload-conditional, not unconditional.**

Phase 4 *would be* the bigger lever where it fires. The leaf-primop result is small even with the mmap fix (~3-10 % wall). Phase 4's per-hit savings would be seconds, not microseconds, so any cache implementation wins there if there are candidates to cache.

**Architecture handoff (still valid):** the mmap'd flat file IS the right Phase 4 substrate too. Same format, larger Value payloads, same OS-page-cache sharing properties. Phase 4 implementation is then "wire up the new IFD primop call sites to the existing mmap'd cache" — minimal architectural new code.

**Conditional applicability:** see §13.

---

## 9. Honest limits

- §4.3 wall arithmetic on firefox uses estimated N (10× hello). Actual measurement during 7.1 spike will confirm or correct.
- The `~30-50 µs saved libstore-tail` is from Phase 5 measurement on hello.drvPath. May be larger on firefox if its derivations have more inputs requiring hashDerivationModulo recursion.
- AOT distribution (§4.4 / §7.3) requires Nix-team coordination not yet committed. Same blocker as bytecode-cache distribution.
- The mmap'd flat file format is a NEW serialization surface. Adds a versioning/schema-evolution concern. WAL-style append-only writes mitigate but don't eliminate.
- Cross-machine cache (cache.nixos.org distribution) introduces a "cache compromise" attack surface. Same threat model as binary cache. Solutions: SHA-256 narHash gating, signatures, trusted-substituter pinning. Real but tractable.
- The `~150 ns mmap lookup` assumes warm page cache. First touch of a cold mmap'd page pays a disk read (~10 µs SSD, ~5 ms HDD). At 95 %+ hit rates on cross-workload reuse, the first-page-fault tax is amortized across many hits. But the very first eval on a freshly substituted cache will pay disk-read latency for each new cache line.
- Daemon analysis (§4.5-§4.7) IPC numbers are Linux Unix socket; performance under macOS Mach IPC may differ.

---

## 10. Recommended next moves

In priority order:

1. **Run §7.1 mmap spike (3-5 days).** Measure on hello + firefox + cardano-node M5. Apply pre-committed thresholds from §7.2.
2. **Re-run Phase 5 with L1-in-memory-only (no SQLite)** on firefox as an isolated control. 1-day spike. Expected: wall-positive driven by 58 % intra-process hit rate × in-memory cost. Confirms the SQLite-cost-isolation hypothesis even before mmap implementation.
3. **Measure cardano-node M5 with existing Phase 5 substrate.** Higher hashDerivationModulo recursion + deeper graph means larger per-hit savings. Confirms whether the per-call ceiling extends to deeper-graph workloads or whether per-call savings scale with graph depth.
4. **(Gated on 1 success) Spec the AOT distribution path** (§7.3) — design doc + Nix-team coordination, 1-2 weeks v3 work + cross-team.
5. **(Audience-gated, see §13) Phase 4 (Class B IFD primops) is INAPPLICABLE on the standard nixpkgs workload set** per commit `2103cdddb` measurement: 0 with-context IFD probes across 7 workloads. The Phase 4 lever requires committing to haskell.nix-shaped workloads as a benchmark target — substantial setup work that's a workload-strategy decision, not a v3-architecture decision. The mmap'd cache substrate from §7.1 IS the right Phase 4 substrate when/if the audience materialises; no architectural work needed today.
6. **(New, identified by `cbb870174`) `.name`-class workload optimization** is a structurally separate lever — parser/lowerer-dominated, 0 derivation primop calls, top primops are __findFile / import / removeAttrs. Eval-cache architecture has nothing to say here. If `.name` perf matters (flake exploration, IDE hover, attr enumeration), the levers are parser / lowerer / module-system traversal — a separate audit.

---

## 11. Codified position

> **The Phase 5 falsifier is the SQLite cost, not the cache concept.** The substrate is validated; the L2 implementation needs replacement. Mmap'd flat file (§4.3) addresses the falsifier directly, leverages 40 years of OS-page-cache engineering, composes with AOT distribution, and is the cleanest architecture for read-heavy cross-process structurally-stable cache traffic.
>
> **Implementation effort: 3-5 days for spike, measurable wall-positive expected.** Decision rules pre-committed (§7.2). If spike confirms, AOT distribution opens (1-2 weeks + Nix-team coord). If spike fails, the leaf-primop scope is structurally too cheap for any caching layer and Phase 4 becomes the primary lever (which it might be anyway).
>
> **Service / daemon architectures are scope-conditional:** wrong at leaf primop, right at Phase 4 if the team decides to ship cross-network or cross-eval-coordination features. Daemon-does-eval is a Stage 13-class commitment unrelated to this cache decision.

---

## 13. Post-Phase-4-falsifier amendment (added 2026-05-23, post-commit)

Four commits landed within hours of this doc's first commit (`65c56a732`) that **substantially reframe §8.4 and §10**. Together they map both the audience question AND the wall-savings question for Phase 4. Documented here so future readers don't apply §8.4's original framing without the amendment.

**Quick summary of the four commits:**
- `cbb870174` — Workload heterogeneity audit. drvPath class homogeneous; .name class structurally separate.
- `2103cdddb` — Phase 4 *audience* falsifier: 0 with-context IFD candidates across 7 nixpkgs workloads (literal-path imports are already cached at two layers above).
- `88402090b` — Phase 4 disk-backed import-result cache IMPLEMENTED. Validated on synthetic IFD workload. Architecture correct.
- `b248b0f8d` — Phase 4b: forceDeep at import-exit before serialisation. Enables disk-cache of lazy `rec { ... }` imported attrsets. **HYP-3 wall mostly falsified**: COLD +78 %, WARM +2.2 % (within noise).

The team executed the full "audience + architecture + wall" cycle for Phase 4 in the same session — and the wall result is the same recurring pattern: architecture correct, lever bounded by what's skippable vs total eval. The §10 next-moves item 5 was outdated within hours of this doc's first commit.

### 13.1 Phase 4 audience falsifier (commit `2103cdddb`)

The team added `AllocStats::ifdProbeWithCtx[16]` — a counter that increments in `primImport`/`primReadFile`/`primPathExists` **only** when the path argument carries non-empty `NixStringContext` (the discriminator between literal-path imports, which are already TW-eval-cached + v3-disk-cached via #770, and derivation-output-path imports, which can trigger real builds via `realisePath`).

**Cross-workload measurement:**

| Workload | Total IFD probes | With-context (Phase 4 audience) |
|---|---|---|
| hello.name | 573 | **0** |
| hello.drvPath | 983 | **0** |
| hello.outPath | 983 | **0** |
| gcc.drvPath | 1009 | **0** |
| python3.drvPath | 1006 | **0** |
| firefox.name | 618 | **0** |
| firefox.drvPath | 3179 | **0** |

**Every single `import` / `readFile` / `pathExists` call across these seven workloads has empty string context.** They are all literal-path imports — already cached at two layers above (TW eval-cache + v3 disk cache). **Phase 4 has NO AUDIENCE on the standard nixpkgs workload set.**

### 13.2 Workload heterogeneity audit (commit `cbb870174`, `WORKLOAD_HETEROGENEITY_AUDIT_2026-05-23.md`)

Nine workloads measured under `NIX_VM_STATS + NIX_VM_PRIMOP_TIME`. Two-part outcome:

| Class | Insns | Memory | Drv calls | Top primops |
|---|---|---|---|---|
| `.name` | 2.3M | 195 MB | **0** | __findFile / import / removeAttrs |
| `.drvPath` (hello/bash/coreutils/gcc/python3) | 12-15M | 666 MB | 12-17K | __derivation* / __derivCoerce |
| firefox.drvPath | 60M | 1432 MB | 77K | same as drvPath class (= hello at 4-5× scale) |

**The drvPath workload class is homogeneous; the .name class is structurally separate.**

Confirms firefox.drvPath Phase 5 wall at 4-5× scale: 2575 ms OFF vs 2623 ms WARM ACTIVE+DISK = **1.02× slower** — i.e. the libstore-tail-too-small finding is invariant across the drvPath class, not specific to hello.

### 13.3 What this jointly implies for this doc

**Four concrete amendments:**

**(a) §8.4's "Phase 4 remains the larger lever" framing is workload-conditional. The architecture is BUILT and validated; the wall lever still depends on workload mix.**

The "haskell.nix-materialization-equivalent multi-second savings" framing in §8.4 (and in `IFD_CACHE_DESIGN_2026-05-23.md` and `IFD_DEEP_DIVE_2026-05-21.md` §11) was correct in PRINCIPLE. **Per `88402090b` + `b248b0f8d`, the Phase 4 architecture is now actually built**:

- Disk-backed import-result cache hooked at `primImport`'s resolved-path check (line 7530-area)
- Gate: `NIX_V3_IFD_IMPORT_CACHE_DISK=1`
- Key: `SHA256("ifd-import\0" + path)`
- Storage: existing Phase 5 EvalResults table
- Phase 4b extends with `forceDeep` at import-exit before serialisation, enabling disk-cache of lazy `rec { ... }` imported attrsets

**Validated on synthetic IFD workload** (constructed for this purpose since standard nixpkgs has zero with-context IFD candidates):
- HYP-1 correctness ✓ confirmed (byte-identical TW)
- HYP-2 architecture ✓ confirmed (IMPORT-DISK-HIT events on warm runs; cold inserts 737 KB blob for ifd-large's 1000-attr `rec { ... }` result)
- HYP-3 wall MOSTLY FALSIFIED (COLD +78 % one-time tax; WARM +2.2 % within noise)

The wall result follows the **same recurring pattern** as Phase 3e ACTIVE + Phase 5: architecture correct, lever bounded by what's skippable vs total eval. The IFD-result parse+eval is ~10 ms; total eval is dominated by nixpkgs-load (~700 ms). For Phase 4 wall savings to materialise, need workloads where IFD-result parse+eval **dominates** — haskell.nix's `callCabalProjectToNix`, flake outputs that import derivation result files, NixOS modules with IFD-generated configuration. Standard nixpkgs workloads (the audience falsifier) AND the synthetic IFD workload (the wall falsifier) both confirm: this lever fires only on workloads built around IFD.

**(b) §10 next moves — replace "(parallel-track) Phase 4 planning" with the correct status.**

Original §10 listed:
> 5. (Independent) Continue Phase 4 (Class B IFD primops) planning — the multi-second-savings lever stays valid regardless of leaf-scope outcome.

Corrected reading:

> 5a. **Phase 4 architecture is BUILT** (commits `88402090b` + `b248b0f8d`). Disk-backed import-result cache with forceDeep at import-exit. Validated on synthetic IFD workload; same wall pattern as Phase 5 (architecture correct, lever bounded). NO further architectural work needed today.
> 5b. **Phase 4 wall lever is workload-gated** on haskell.nix-class workloads where IFD-result parse+eval dominates total eval. Test target: `callCabalProjectToNix`, flake-input IFD imports, NixOS modules with IFD-generated config. This is a **workload-strategy decision** (commit to haskell.nix-shaped benchmark), not a v3-architecture decision.
> 5c. **`.name`-class workload optimization path** is structurally separate — parser/lowerer-dominated, 0 derivation primop calls, eval-cache architecture has nothing to say. Separate audit if `.name` perf matters (flake exploration, IDE hover, attr enumeration).

**(c) The drvPath workload class is homogeneous — the mmap-L2 spike (§7) result will generalise.**

If the mmap'd L2 spike confirms wall-positive on hello.drvPath, the workload audit predicts it generalises across hello/bash/coreutils/gcc/python3/firefox at the same per-call savings (~30-50 µs × call count). firefox at 77 K drv calls = ~2.3-3.8 s potential lever — but bounded by the SAME per-call ceiling.

**This actually strengthens the spike priority.** A 3-5 day investment with a known per-call ceiling that's structurally invariant across the workload class is exactly the kind of measure-twice spike the [[measure-twice-cut-once]] rule endorses. The downside is bounded; the upside scales with the workload size proportionally.

**(d) The same wall pattern across Phase 3e ACTIVE + Phase 5 + Phase 4b is itself a strategic signal.**

Three different cache placements (mid-body drv-hash, post-body SQLite-disk, import-exit forceDeep-then-disk) all show the same shape: **architecture correct + cache hits validated + wall savings either within noise or net-negative due to cache I/O cost.** This is now a documented pattern, not a single data point.

The pattern says: **the per-skip savings target lives in the same order-of-magnitude as the cache overhead.** Both are µs-scale on hello-class workloads, both grow at similar rates across the workload class. The mmap'd L2 architecture (§4.3) is the design that breaks the symmetry by dropping cache lookup cost ~400× (60 µs → 150 ns) while keeping savings constant. That's why it's expected to flip the sign positive where SQLite couldn't.

**If the mmap'd L2 spike also fails to flip the sign**, the pattern hardens further: the entire eval-result-cache concept at primop-call boundaries is structurally too cheap on drvPath-class workloads, and the lever must move up to coarser granularities (IR subtrees, lib.fix steps, module results) — which is exactly the Unison Item 1 / Item 2 direction. The mmap spike is therefore not just a cache-implementation question but **a falsifier for the eval-result-cache-at-primop-boundary thesis itself.**

### 13.4 Updated #741 falsifier ledger (cumulative)

**15 falsifiers total across the #741 arc:** 7 positive confirmations + 8 falsifications. Updated from the Phase 4 ledger to include Phase 4 architecture validation + Phase 4b wall falsifier:

- Phase 1 round-trip ✓ PASSED (56× margin)
- Phase 2 determinism ✓ PASSED
- Phase 3a SHADOW correctness ✓ PASSED (11 % hit)
- Phase 3a RCA-A forceDeep ✗ FALSIFIED
- Phase 3b chase ✓ defensible
- Phase 3c RCA-B forceDeepReadOnly ✗ FALSIFIED
- Phase 3e SHADOW ✓ PASSED (33 % hit exact match)
- Phase 3e ACTIVE wall ✗ FALSIFIED (1.00× ± 0.02)
- Phase 5 cross-process correctness ✓ PASSED (100 % warm)
- Phase 5 wall ✗ FALSIFIED (+71 % cold / +5.6 % warm)
- Phase 5b batching ✗ FALSIFIED (variance 24×)
- Workload heterogeneity audit ✓ drvPath class HOMOGENEOUS
- Phase 4 audience ✗ FALSIFIED (0 across 7 nixpkgs workloads)
- **Phase 4 architecture (`88402090b` + `b248b0f8d`)** ✓ PASSED (IMPORT-DISK-HIT validated on synthetic IFD workload, byte-identical TW)
- **Phase 4b wall** ✗ FALSIFIED (COLD +78 %, WARM +2.2 %; ~10 ms saved below noise floor of ~700 ms total eval)

**#741 is now CLOSED at "correct, no wall benefit at primop-call-boundary scope on any tested workload."** The strategic question is no longer "does the cache work" (it does — substrate is over-validated with Phase 4 architecture built and validated) but "is there a workload OR a cache implementation where the per-call ceiling allows a wall lever." Per §13.3, the candidates are:

1. **mmap'd L2 at leaf primop scope** on the existing drvPath class — bounded by ~5 % wall ceiling (the §7 spike answers this in 3-5 days). Falsifier for the "cache-implementation cost is the binding constraint" hypothesis.
2. **Phase 4 wall on haskell.nix-class workloads** — architecture is built; just needs commitment to the workload as a benchmark target. Falsifier for "IFD-result parse+eval becomes dominant on real IFD-heavy workloads."
3. **`.name`-class optimization** — separate code-path audit, not eval-cache work.
4. **Stage 4 v4 / let-floating work (#776)** — orthogonal to caching entirely.
5. **Coarser-granularity caching** (Unison Item 1/2 territory) — only justifies if both (1) and (2) above also fail to deliver wall savings. The §7 spike is therefore a dual falsifier: it tests SQLite vs mmap AND (indirectly) tests the eval-result-cache-at-primop-boundary thesis itself.

The strategic conclusion of this doc holds: **the SQLite-cost falsifier is structural; mmap'd L2 is the right next experiment; the substrate work was the right investment regardless of the specific scope.** What the Phase 4 + Phase 4b commits changed is the *certainty* that Phase 4 also follows the same wall pattern (it does, on the synthetic IFD workload), which strengthens the §13.3(d) reading: the cache-at-primop-boundary thesis itself may be too cheap a scope.

### 13.5 ROADMAP integration follow-on (landed in same commit as this amendment)

The ROADMAP_TO_VISION integration commit (`65c56a732`) added a "Stage 10 partial-subset substrate landed 2026-05-23" subsection that referenced Phase 4 as the larger lever. **That subsection has been amended** in the same commit as this §13 (item 4 and item 5 of the subsection both updated, plus the "Next moves" list refreshed). The amendment is purely additive (no claim retraction, just conditional scoping + acknowledging the architecture-is-built status).

---

## 12. Cross-references

- [`IFD_CACHE_DESIGN_2026-05-23.md`](IFD_CACHE_DESIGN_2026-05-23.md) — original 5-phase plan; Phases 1-5 landed, this doc supersedes the L2-implementation decision in §4 (was "extend SQLite schema")
- [`JIT_CONFIDENCE_2026-05-23.md`](JIT_CONFIDENCE_2026-05-23.md) — sibling; remains valid; substrate-validation strengthens the JIT-deferral case
- [`WARM_EVAL_AND_INSTRUMENTATION_2026-05-23.md`](WARM_EVAL_AND_INSTRUMENTATION_2026-05-23.md) — §6.4 AOT distribution; this doc adds eval-result-cache as the sibling artifact to bytecode-cache
- [`IFD_DEEP_DIVE_2026-05-21.md`](IFD_DEEP_DIVE_2026-05-21.md) §11 — materialization-retirement program; Phase 4 (Class B IFD primops) is the cache lever per that program
- [`MEASURE_TWICE_CUT_ONCE_2026-05-23.md`](MEASURE_TWICE_CUT_ONCE_2026-05-23.md) — pre-committed thresholds in §7.2 follow this rule
- [`OPTIMIZATION_STRATEGIES_2026-05-23.md`](OPTIMIZATION_STRATEGIES_2026-05-23.md) §5.1 — IFD-boundary cache as Tier 2 composable; this doc refines to L1 + mmap L2 architecture
- [`UNISON_IDEAS_2026-05-07.md`](UNISON_IDEAS_2026-05-07.md) — Item 3 (hash-keyed eval cache); Phase 5 + this doc empirically validate the substrate; firefox 58 % rate data for Item 1 (content-addressed IR) thesis
- [`PERF_STRATEGY_2026-05-17.md`](PERF_STRATEGY_2026-05-17.md) Stage 10 (salsa) — sibling persistence-across-invocations direction; this doc proposes a simpler mmap-based subset rather than the full salsa machinery

Commit lineage:
- `23bb231d2` #741 Phase 1 spike — Value-subset serializer (the V3VR format reused here)
- `a3b491522` #741 Phase 2 — canonical Value hash
- `c329e4174` #741 Phase 3a SHADOW — in-memory cache
- `03162ccbe` #741 Phase 3b — Thunk/App/Slot chasing
- `7a06d36a6` #741 Phase 3c-RCA-B FALSIFIED — deep-force at primop entry unsafe
- `dc1bdd938` #741 Phase 3e SHADOW — drv-hash mid-body cache (33 % hit, 0 mismatches)
- `dac070989` #741 Phase 3e ACTIVE — wall-neutral
- `bff1f670f` #741 Phase 5 — full architecture mapped; HYP-3 wall savings FALSIFIED
- (this doc) — post-Phase-5 architecture reassessment + firefox generalization

---

*Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group. SPDX-License-Identifier: Apache-2.0.*
