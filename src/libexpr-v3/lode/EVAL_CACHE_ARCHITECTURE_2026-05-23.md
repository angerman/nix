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

Phase 4 was always the bigger lever. The leaf-primop result is small even with the mmap fix (~3-10 % wall). Phase 4's per-hit savings are seconds, not microseconds, so any cache implementation wins there.

**Architecture handoff:** the mmap'd flat file IS the right Phase 4 substrate too. Same format, larger Value payloads, same OS-page-cache sharing properties. Phase 4 implementation is then "wire up the new IFD primop call sites to the existing mmap'd cache" — minimal architectural new code.

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
5. **(Independent) Continue Phase 4 (Class B IFD primops) planning** — the multi-second-savings lever stays valid regardless of leaf-scope outcome. The mmap'd cache substrate from §7.1 is the right Phase 4 substrate.

---

## 11. Codified position

> **The Phase 5 falsifier is the SQLite cost, not the cache concept.** The substrate is validated; the L2 implementation needs replacement. Mmap'd flat file (§4.3) addresses the falsifier directly, leverages 40 years of OS-page-cache engineering, composes with AOT distribution, and is the cleanest architecture for read-heavy cross-process structurally-stable cache traffic.
>
> **Implementation effort: 3-5 days for spike, measurable wall-positive expected.** Decision rules pre-committed (§7.2). If spike confirms, AOT distribution opens (1-2 weeks + Nix-team coord). If spike fails, the leaf-primop scope is structurally too cheap for any caching layer and Phase 4 becomes the primary lever (which it might be anyway).
>
> **Service / daemon architectures are scope-conditional:** wrong at leaf primop, right at Phase 4 if the team decides to ship cross-network or cross-eval-coordination features. Daemon-does-eval is a Stage 13-class commitment unrelated to this cache decision.

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
