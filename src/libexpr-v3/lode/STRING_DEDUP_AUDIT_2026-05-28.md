# String dedup audit — measurement spike proposal

**Date:** 2026-05-28
**Author:** session synthesis (codebase audit)
**Status:** MEASUREMENT SPIKE PROPOSAL — not committed; pre-committed thresholds inside
**Triggering question:** "Do we have any form of string-dedupe? Lots of keys/strings are identical during evaluation?"

Companion docs:
- [`MEMORY_REDUCTION_AVENUES_2026-05-26.md`](MEMORY_REDUCTION_AVENUES_2026-05-26.md) §Category 1 — measurement-first framing this proposal extends
- [`T1_3_PAIRS_LISTS_ATTR_2026-05-27.md`](T1_3_PAIRS_LISTS_ATTR_2026-05-27.md) — template for the per-site attribution pattern
- [`HNE_BUCKET_DECOMP_2026-05-27.md`](HNE_BUCKET_DECOMP_2026-05-27.md) — current memory breakdown (note: strings not separately bucketed)
- [`MEASURE_TWICE_CUT_ONCE_2026-05-23.md`](MEASURE_TWICE_CUT_ONCE_2026-05-23.md) §3 — pre-commit thresholds required

---

## 1. TL;DR

**v3 dedupes attribute keys (`globalSymbolTable`) and source positions (`posSnapshotIndex`), but does NOT dedupe runtime string VALUES.** Every `+`, every `toString`, every store-path interpolation calls `Alloc::allocChars` fresh. Source LITERALS sit in a `std::deque<std::string>` pool that's append-only — same literal repeated 100× in source = 100 pool entries.

**The hypothesis is real but unquantified.** Per-Tag live-fraction data (`LIVE_FRACTION_SPIKE_2026-05-27.md`) tracks Closures/Thunks/Bindings/Lists/Pairs but does NOT separately bucket strings. We don't know how many bytes string values consume on HNE today.

**Proposed spike (~0.5-1 day):** add `NIX_V3_STRINGS_ATTR=1` per-alloc-site attribution in `allocChars`, mirroring the T1.3 pattern. Pre-committed thresholds inside §5.

---

## 2. Inventory of what's currently deduped

### 2.1 Attribute keys — FULLY DEDUPED ✓

`ir.cc:99-95` `globalInternSymbol(std::string_view s)`:
- Process-wide `GlobalSymTab` singleton
- `std::unordered_map<std::string, SymbolId, StringHash, StringEq>` with heterogeneous-lookup hash (avoids per-probe `std::string` allocation)
- Slot 0 reserved for `kInvalidSymbol` sentinel (empty string)
- Every `Module::internSymbol` calls through; per-module `symbols` vector mirrors for diagnostics only

Effect: `pkgs.lib.attrsets.mapAttrs` produces SymbolIds 17, 42, 88, 153 once and reuses across every reference. ~10²-10⁴ distinct symbols per eval; bounded.

### 2.2 PosIdx → {file, line, col} — FULLY DEDUPED ✓

`alloc.hh:1841-1875` `posSnapshotIndex()` — dedupes by {file, line, column} tuple. Stable handle. Required for Schema 14 cross-process determinism.

### 2.3 Compile-time string literals — HALF-MEASURE ⚠

`lower.cc:780-786` `lowerString(ExprString * e)`:
```cpp
static std::deque<std::string> stringPool;
stringPool.emplace_back(e->v.string_view());
return addBinding(ir::LitString{stringPool.back()});
```

Same pattern for `pathPool` (`lower.cc:790+`) and `interpStr` (`lower.cc:716-722`).

**The pool is append-only.** No dedup. Every `ExprString` site pushes a new entry. So `"foo"` appearing 100× in source = 100 pool entries.

What it DOES provide: stable string_view backing (deque doesn't invalidate). The IR `LitString` constant holds a string_view; the bytecode constant pool then holds the same. At RUNTIME, `OP_PUSH_CONST_STRING` (or equivalent) reads the const-pool buffer. So at the IR/bytecode level, references to the same literal CU-constant share — but the *backing bytes in the pool* are duplicated.

### 2.4 Runtime string VALUES — NOT DEDUPED ✗

`alloc.hh:1414` `static char * allocChars(size_t n) noexcept` — fresh allocation per call. Found at 20+ sites:

| File:line | Context | Rough frequency |
|---|---|---|
| `primops.cc:608` | primop string result | per primop call |
| `primops.cc:2292` | string concat / interp | per `+` op |
| `primops.cc:2721` | `abspath` result | per path op |
| `primops.cc:2908` | dirname/basename result | per path op |
| `primops.cc:5399` | path coerce | per path coerce |
| `primops.cc:9243` | `outPath` materialization | per derivation |
| `vm.cc:9740` | string concat in OP_STR_CONCAT | per `+` |
| `value_serialize.cc:344` | string deserialize | per disk-cache load |
| `value_serialize.cc:367` | path deserialize | per disk-cache load |
| `v3_call_flake.cc:225, 385, 433` | flake bridge string materialization | per flake input |

**Every operation allocates fresh.** No global intern table, no per-eval LRU, no nothing.

### 2.5 Path strings — NOT DEDUPED ✗

Same `allocChars` pattern as runtime strings. Store paths like `/nix/store/abc123-foo-1.0.0-bin` are allocated fresh on every materialization.

---

## 3. Why this likely matters (hypothesis, not yet measured)

Real nixpkgs evaluations produce massive duplication at runtime:

1. **drv hashes** — 32-char content-addressed hashes. Each derivation produces one; HNE has thousands of drvs; each hash is referenced multiple times through propagation chains.

2. **Store paths** — `/nix/store/abc123-foo-1.0.0` averages ~50-80 chars. Referenced by every dependent derivation, every `outPath` access, every interpolation that includes it. **A single store path can appear 100+ times in a large eval.**

3. **String concat dominance** — `"${pkg.outPath}/bin/${pkg.pname}"` allocates **3 fresh buffers** per evaluation:
   - One for the substring before `${pkg.outPath}`
   - Final concatenated result
   - Intermediate buffer for the second concat

4. **Build-output realization** — every `mkDerivation` produces a string Value for `outPath`, `drvPath`, `name`, `version`, etc. — each a fresh allocChars buffer.

5. **`builtins.toString` / `format-output`** — frequently called; never deduped.

6. **Position-attr strings** — `unsafeGetAttrPos` materializes `{file, line, column}` to attrs. The file string is sometimes long (`/nix/store/.../nixpkgs/pkgs/...`); deduplicated at PosIdx level but RE-materialized to a string per attr access.

7. **Disk-cache reload** — `value_serialize.cc:344` allocates fresh on every load from disk_cache. Cached strings are not shared with the original allocation site.

**Caveat per [[measure-twice-cut-once]]:** all of the above is plausibility, not measurement. The lever may be 10 MB or 500 MB — we don't know until we measure.

---

## 4. Measurement gap

Per [`LIVE_FRACTION_SPIKE_2026-05-27.md`](LIVE_FRACTION_SPIKE_2026-05-27.md) and [`HNE_BUCKET_DECOMP_2026-05-27.md`](HNE_BUCKET_DECOMP_2026-05-27.md), the per-Tag breakdown:

| Tag | Live (HNE) | Allocated (HNE) | Tracked separately? |
|---|---|---|---|
| Closures | 5.0 MB | 259 MB | ✓ |
| Thunks | 95.8 MB | 305 MB | ✓ |
| Bindings | 400 MB | 680 MB | ✓ (via #746) |
| Lists | 15.2 MB | 51 MB | ✓ |
| Pairs | 101 MB | 119 MB | ✓ |
| **Strings + Paths** | **?** | **?** | **✗ NO** |
| **Total** | 617 MB | 1.38 GB | — |

Strings and paths consume arena bytes via `allocChars`; those bytes are included in the total but NOT broken out. The 1.38 GB - 1.41 GB (sum of broken-out Tags) gap is ≤ ~30 MB which suggests strings on HNE may be modest in raw bytes, BUT this could be misleading because:

1. End-of-eval live-trace measures LIVE bytes; the high-churn nature of strings means most are DEAD at any moment — the cumulative allocated bytes (per-cycle GC pressure) is what matters for peak RSS.
2. Strings/paths don't participate in the precise-root walk yet (Day 4 fwdChars added them but as payload, not as a tracked Tag).
3. Live-trace doesn't reveal per-allocation-SITE breakdown — the 1.4 GB allocated for Bindings could include re-allocation of the same logical string content many times.

**Translation: we have no per-site data for strings today.**

---

## 5. Proposed spike — `NIX_V3_STRINGS_ATTR=1` per-site attribution

### 5.1 What lands

Mirror the T1.3 pattern from [`T1_3_PAIRS_LISTS_ATTR_2026-05-27.md`](T1_3_PAIRS_LISTS_ATTR_2026-05-27.md):

1. Instrument `Alloc::allocChars` with a per-caller-PC attribution map (or per `__builtin_FILE/__builtin_LINE` site, like the BINDINGS_ATTR approach in #746)
2. On end-of-run dump (gated by `NIX_V3_STRINGS_ATTR=1`), print top-N call sites by total bytes + alloc count + average size
3. Also dump duplication-rate estimate: for each site, count distinct content vs total allocations

### 5.2 Effort

~0.5-1 day. ~200 LoC over existing T1.3 infrastructure.

Code touch points:
- `alloc.hh` `allocChars` — add optional callsite parameter via `__builtin_LINE`/`__builtin_FILE` (same pattern as #746)
- `live_trace.cc` — new `dumpStringsAttr()` reporter
- Optionally: a side-table `unordered_map<string_view, count>` for duplication-rate estimation (sampled, not exhaustive — too expensive to track every string)

### 5.3 Pre-committed thresholds (per measure-twice §3)

**ALL thresholds set BEFORE running the spike. No post-hoc adjustment.**

| String-allocation total bytes on HNE | Verdict | Next step |
|---|---|---|
| **< 50 MB** | NO LEVER | Close the question; document the finding; move on |
| **50-100 MB** | MARGINAL | Limited intern table for hot sites only (drv hashes, store paths); ~1-2d implementation |
| **100-200 MB** | MODERATE | Full runtime string-intern table; ~1 week implementation |
| **> 200 MB** | SIGNIFICANT | Full intern table + revisit compile-time literal dedup; ~1-2 weeks |

**Additional discriminator (duplication-rate):**
- If <30% duplicate content: dedup has poor ROI; bytes are inherent diversity
- If 30-60%: per-content-hash intern table viable
- If >60%: strong lever; mass dedup would substantially reduce arena pressure

### 5.4 What we are NOT committing to in this doc

- Whether to implement the intern table (decided after spike, based on §5.3 thresholds)
- Intern-table data structure (HAMT vs hash vs trie — defer until lever quantified)
- LRU eviction policy (defer)
- Whether to dedup paths separately from strings (defer)

This doc commits to the MEASUREMENT only. Implementation is a separate decision per [[measure-twice-cut-once]].

---

## 6. Sequencing

This spike is **independent of the GC track** and can run in parallel with Phase 4-5 of [`GC_DESIGN_POST_CHENEY_2026-05-28.md`](GC_DESIGN_POST_CHENEY_2026-05-28.md). The spike adds counters; it doesn't change allocation behavior.

**Recommended timing:**
- Run after Phase 3.6 (current reuse-safety work) stabilizes — single-day insertion before Phase 4 SHIP-gate measurement
- Results inform whether string-intern is a Stage 7 candidate or a future deferred item
- If results land >200 MB on HNE, it's a competing lever with cache eviction; both are orthogonal to MS tenured

---

## 7. Honest limits

- The plausibility arguments in §3 are reasoning, not measurement. The lever COULD be small (10-30 MB) if most strings are unique (file paths often are).
- Per-site attribution requires patching every `allocChars` call site to pass `__builtin_LINE/FILE`. Some sites are wrappers (e.g., flake bridge) — attribution may bucket multiple logical origins under one wrapper site.
- Duplication-rate sampling is approximate; full content-hash tracking would dominate the measurement itself.
- HNE may not represent M5 or cardano-node; spike should run on both if results are positive.
- Intern table itself costs memory (hash + content); ROI math must include the table.
- String VIEWS into arena pages need careful lifetime management if dedup is implemented — current arena assumes payload outlives the caller's stack frame, which becomes a constraint.
- Compile-time literal dedup (§2.3 stringPool) is a separate concern; spike data may inform it but doesn't directly solve it.

---

## 8. Cross-references

- [`MEMORY_REDUCTION_AVENUES_2026-05-26.md`](MEMORY_REDUCTION_AVENUES_2026-05-26.md) §Category 1 — adds string-attribution to the measurement-first inventory
- [`T1_3_PAIRS_LISTS_ATTR_2026-05-27.md`](T1_3_PAIRS_LISTS_ATTR_2026-05-27.md) + [`T1_3_THUNKS_ATTR_2026-05-27.md`](T1_3_THUNKS_ATTR_2026-05-27.md) + [`T1_3_CLOSURES_ATTR_2026-05-27.md`](T1_3_CLOSURES_ATTR_2026-05-27.md) — template
- `NIX_V3_BINDINGS_ATTR` (commit `66b1061cd` #746) — original pattern this extends
- [`GC_DESIGN_POST_CHENEY_2026-05-28.md`](GC_DESIGN_POST_CHENEY_2026-05-28.md) — orthogonal lever; runs in parallel
- [`MEASURE_TWICE_CUT_ONCE_2026-05-23.md`](MEASURE_TWICE_CUT_ONCE_2026-05-23.md) §3 — pre-commit thresholds methodology
- `ir.cc:99-95` `globalInternSymbol` — what attribute-key dedup looks like (model for runtime-string version)
- `lower.cc:780-786` `lowerString` — compile-time literal pool (half-measure)
- `alloc.hh:1414` `allocChars` — runtime string alloc path

---

*Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group. SPDX-License-Identifier: Apache-2.0.*
