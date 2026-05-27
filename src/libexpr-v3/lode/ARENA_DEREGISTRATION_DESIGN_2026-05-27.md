# Arena deregistration spike — 2-3 d session handoff design

**Status**: Ready-to-execute design for a focused future session.
**Estimated effort**: 2-3 days end-to-end (audit + impl + measure).
**Prerequisite**: this document + Stage 5 MVP infrastructure (commit
`173481af3`) + the Stage 3 walker (commit `e7639f837`).
**Origin**: identified as concrete Stage 6 prerequisite per
[`BOEHM_TUNING_FALSIFIED_2026-05-27.md`](BOEHM_TUNING_FALSIFIED_2026-05-27.md)'s
periodic-GC probe follow-up.

## 1. The problem this solves

`Arena::refill()` at `alloc.hh:706` currently registers EVERY 16 MB
block with Boehm as a root region via `GC_add_roots(blk, blk +
kBlockSize)`.  On a 587 MB arena workload (hello.drvPath under
v3-direct), Boehm's per-collection scan walks **587 MB of arena
plus its own ~400 MB heap** every time it considers collecting.

Per the 2026-05-27 periodic-GC probe:
* gc_count = 1 over the entire eval lifetime (Boehm's auto-heuristic
  chose growth over collection)
* Each forced collect costs ~40 ms (1500 forced collects = 64 s)
* The 402 MB Boehm heap stays at watermark; unmap mechanism IS
  functional but doesn't fire because Boehm's threshold is never
  met under the high per-collect cost

If the arena were NOT a Boehm root region, Boehm collections would
scan only its own ~5 MB live heap (~ < 1 ms per collect).  Auto-
collection would fire freely, releasing pages, keeping the heap
watermark close to live size.

## 2. Why the arena was registered in the first place

Per the `alloc.hh:34-46` comment block and the WC-13 historical note:

> "register arena blocks as Boehm GC roots so any raw `nix::Value *`
>  (or other GC-managed pointer) stored inside a Bridge thunk's
>  bridgeSrc field keeps the underlying object alive."

The motivating storage was `Thunk::bridgeSrc` — bridge thunks (state
`ThunkState::Bridge`) hold a `void *` cast of `nix::Value *` from
TW's heap.  Without the arena being a Boehm root, Boehm doesn't see
those nix::Value* references and may reclaim them mid-evaluation.

Trade-off: SAFE but EXPENSIVE.  This design provided correctness via
the most-expensive mechanism available (conservative full-arena scan).

## 3. The fix: targeted side-table

Maintain a **bridge-source registry** — a thread-local
`std::vector<nix::Value *>` holding every nix::Value* stored in any
v3 cell.  Register the registry's storage with Boehm via
`GC_add_roots(&v[0], &v[v.size()])` — small surface area, exact
coverage.

The arena itself is no longer a Boehm root, so the per-collect scan
drops from 587 MB to ~ KB.

## 4. Audit — what Boehm-managed pointers live in arena cells

The arena holds: `Closure`, `Thunk`, `Bindings`, `ListVec`,
`ValuePair`, `Env`, standalone `Value` cells, and `const char *`
character buffers.  The audit's question is: which fields HOLD a
Boehm-managed pointer (i.e., a pointer Boehm must see to know the
target is reachable)?

| Field                       | Type            | Boehm-managed?      | Action                |
|----------------------------|-----------------|---------------------|-----------------------|
| `Closure::desc`             | LambdaDescriptor* | NO — lives in CompilationUnit (libc malloc) | none |
| `Closure::cu`               | CompilationUnit* | NO — lives in ImportCache::cus deque (libc) | none |
| `Closure::capturedWiths`    | ListVec*        | NO — arena            | none |
| `Closure::upvalues[]`       | Value           | (recursive)           | per-tag below |
| `Thunk::bridgeSrc`          | void* (nix::Value*) | **YES**           | **register in side-table** |
| `Thunk::cell`               | Value*          | NO — arena cell       | none |
| `Thunk::cellContainer`      | Bindings*       | NO — arena            | none |
| `Thunk::shapeCell`          | Value*          | NO — arena            | none |
| `Thunk::tail[]`             | Value           | (recursive)           | per-tag below |
| `Thunk::suspended.{desc,cu,capturedWiths}` | as above | NO | none |
| `Bindings::entries[].value` | Value           | (recursive)           | per-tag below |
| `Bindings::entries[].name`  | uint32_t SymbolId | NO — symbol table index | none |
| `ListVec::elems[]`          | Value           | (recursive)           | per-tag below |
| `ValuePair::{left,right,evaluated}` | Value   | (recursive)           | per-tag below |
| `Env::values[]`             | Value           | (recursive)           | per-tag below |
| Standalone `Value` cells    | Value           | (recursive)           | per-tag below |

### Per-Tag audit of Value payloads

| Tag                         | Payload          | Boehm-managed?      | Action                |
|----------------------------|------------------|---------------------|-----------------------|
| Int / Float / Bool / Null   | scalar           | NO                  | none |
| String / Path               | `const char *`   | **NEEDS AUDIT**     | see §4.1 below |
| Attrs / List / Closure / Thunk / App / PrimOpApp / Slot | v3-arena ptr | NO — arena | none |
| PrimOp                      | `const PrimOp *` | NO — static registration | none |
| Blackhole / Uninitialized   | sentinel         | NO                  | none |
| **External**                | `void *`         | **POTENTIALLY YES** | see §4.2 below |

### 4.1 String / Path audit — PROBE DATA AVAILABLE 2026-05-27

Sources of `const char *` stored in v3 cells:

1. `globalSymbolTable` (`std::vector<std::string>` in symbol table) —
   uses `std::allocator` → libc malloc.  Safe.
2. `Alloc::allocChars(n)` (arena-allocated char buffer) — arena.  Safe.
3. cppnix's `nix::Symbol` table interning — needs check (could be
   Boehm-managed depending on cppnix init order).
4. TW-bridged string values via `treeWalkerToV3()` — chars from TW's
   string ownership.  Could be Boehm-managed if TW uses Boehm for
   strings (libexpr's symbol table does).

**Probe data (2026-05-27)** via `NIX_V3_LIVE_TRACE=1`:

* hello.drvPath: 70 K String + 881 Path Values reached
* HNE: 1.44 M String + 11 K Path Values reached

Sample addresses span TWO distinct address ranges (illustrated on
HNE):
```
  Range A:   0xb98c2f980  /  0xb9c7747e0       (arena-like)
  Range B:   0x850400d00  /  0x850400d10       (smaller-pointer-range)
```

Range A's spacing + alignment matches arena addresses (e.g.,
`0xb9c774620, 0xb9c7746e0` are 0xc0 = 192 B apart — consistent
with per-Value `allocValue()` 192 B objects in arena).

Range B's addresses are smaller pointers and probably libc / libexpr
symbol-table-managed.  Their content (`'aarch64-darwin'`,
`'/nix/store'`, `'flake:nixpkgs'`) suggests system-info-style
interned strings.

**Audit task for the future session** (now data-anchored):

1. Run the probe (above command) on the target workload.
2. Classify the SAMPLED addresses:
   * Compare against `threadArena().blockRanges()` to identify
     arena-managed addresses (safe — moves with arena).
   * Identify libc-malloc'd ranges (typically `<= 0x6000_0000_0000`
     on macOS aarch64).
3. For each range NOT in arena AND that points to Boehm-managed
   memory: register or convert.

Expected: most v3-native string construction is arena-allocated via
`allocChars` (search for `allocChars` callers).  TW-bridged strings
are the risk.  Range-A addresses are the safe arena majority;
Range-B requires per-source identification.

### 4.2 External tag audit — DONE (2026-05-27 probe)

`Value::payload.raw` of Tag::External is an opaque `void *`.  Origin:

* TW's `nix::Value::External` (via `treeWalkerToV3`) — Boehm-managed
* v3 never CREATES External (`value.cc` checked: no `mkExternal` defined)

**Cheap probe DONE 2026-05-27** via the extended live-trace audit
(`NIX_V3_LIVE_TRACE=1` reaches every Value during transitive walk
and tallies Tag::External / String / Path counts).

**Measurement** on the two anchor workloads:
```
                            hello.drvPath   HNE
  Tag::External (reached):       0           0      ← clean!
  Tag::String   (reached):  70,035    1,439,011
  Tag::Path     (reached):      881       10,971
```

**Conclusion**: Tag::External is NOT a concern for arena dereg.
On both anchor workloads, NO v3 cells hold External-typed Values.
External flows through v3 only as a transient bridge intermediate;
never materialized into arena-resident state.

**Future session's action**: no External-specific work required.

**Automated re-verification** (recommended):
```bash
bench/arena-dereg-audit.sh                          # all anchor workloads
bench/arena-dereg-audit.sh --workload hello         # single workload
bench/arena-dereg-audit.sh --workloads hello,firefox,hne,ackermann
bench/arena-dereg-audit.sh --threshold-external 100 # tolerate small leaks
```

The harness probes each workload under `NIX_V3_LIVE_TRACE=1`,
extracts the Tag::External / String / Path counts from the audit
section, and reports PASS / FAIL against the External-clean
criterion (default threshold: 0).  Exit 0 = clean; 1 = failure; 2
= harness error.

**Manual probe** (if needed for debugging):
```bash
NIX_V3_LIVE_TRACE=1 ./build/src/nix/nix \
  --extra-experimental-features nix-command \
  eval --impure --expr '...your workload...' 2>&1 \
  | grep -A4 "Arena-dereg audit"
```
If `Tag::External` is 0, the audit re-confirms; otherwise diagnose
which cells store the new External payloads (the audit section
includes sample addresses for follow-up).

## 5. Implementation outline (~2 days code)

```cpp
// In include/v3/bridge_root_registry.hh (new file):
namespace nix::v3 {
struct BridgeRootRegistry {
    // Thread-local; appended to at allocBridgeThunk; Boehm sees the
    // backing storage via GC_add_roots registered on first growth.
    void push(void * src) noexcept;  // src is nix::Value*
    // No pop — bridge thunks are tenured forever today.  When Phase 6
    // mark-sweep arrives, eviction tracks Thunk reachability.
    void * const * data() const noexcept;
    size_t size() const noexcept;
};
BridgeRootRegistry & bridgeRoots() noexcept;
}
```

```cpp
// bridge_root_registry.cc:
namespace nix::v3 {
namespace {
struct Impl {
    std::vector<void *> srcs;
    void * curRootStart = nullptr;
    void * curRootEnd = nullptr;
};
thread_local Impl impl;

// Re-register the vector's storage with Boehm.  Called when the
// vector grows past previously-registered range.
void reregister()
{
#if NIX_USE_BOEHMGC
    if (impl.curRootStart) GC_remove_roots(impl.curRootStart, impl.curRootEnd);
    if (!impl.srcs.empty()) {
        impl.curRootStart = &impl.srcs[0];
        impl.curRootEnd = &impl.srcs[0] + impl.srcs.size();
        GC_add_roots(impl.curRootStart, impl.curRootEnd);
    }
#endif
}
}

void BridgeRootRegistry::push(void * src) noexcept
{
    void * oldData = impl.srcs.empty() ? nullptr : &impl.srcs[0];
    size_t oldCap = impl.srcs.capacity();
    impl.srcs.push_back(src);
    if (&impl.srcs[0] != oldData || impl.srcs.capacity() != oldCap) {
        // vector moved or grew capacity; re-register.
        reregister();
    }
}
// ...
}
```

```cpp
// alloc.hh::allocBridgeThunk — add registry registration:
static Thunk * allocBridgeThunk(void * src) noexcept
{
    auto * t = static_cast<Thunk *>(threadArena().alloc(sizeof(Thunk)));
    // ... existing init ...
    t->bridgeSrc = src;
    bridgeRoots().push(src);  // <-- NEW
    return t;
}
```

```cpp
// alloc.hh::Arena::refill — make arena registration conditional:
void refill() noexcept
{
    char * blk = static_cast<char *>(std::calloc(1, kBlockSize));
    blocks.push_back(blk);
    cur = blk;
    end = blk + kBlockSize;
    totalBytes += kBlockSize;
#if NIX_USE_BOEHMGC
    // gate: NIX_V3_ARENA_NOROOT=1 — opt-OUT of arena registration.
    // Default: ON (status quo).  Set NOROOT to deregister arena.
    static const bool s_noRoot =
        std::getenv("NIX_V3_ARENA_NOROOT") != nullptr;
    if (!s_noRoot) {
        GC_add_roots(blk, blk + kBlockSize);
    }
#endif
}
```

(Plus the same gating for `alloc()`'s huge-block path.)

## 6. Measurement methodology

### 6.1 Pre-committed thresholds (per [[measure-twice-cut-once]])

* **SHIP**: ≥ 100 MB peak_rss reduction on HNE (cold cache) + no
  crashes + `--quick` 6/6 PASS + `--core` 15/15 PASS + HNE eval
  byte-identical to TW.
* **TUNE**: 50-100 MB reduction; ship + add explicit cap on bridge
  registry growth (to bound the trade-off).
* **FALSIFY**: any crash OR < 50 MB reduction OR any test regression
  → revert + measurement-commit documenting what went wrong.

### 6.2 Measurement plan

Run 4 configurations, capture peak_rss + gc_count + gc_total_ms:

```
Configuration                                 expected delta vs baseline
default (arena registered)                    BASELINE
NIX_V3_ARENA_NOROOT=1                         peak_rss ↓ 50-200 MB,
                                              gc_count ↑ 5-100×,
                                              gc_total_ms ↓ or = (cheaper per-collect)
NIX_V3_ARENA_NOROOT=1 + force_unmap_on_gcollect=1  peak_rss ↓ further
NIX_V3_ARENA_NOROOT=1 + NIX_V3_BOEHM_FORCE_UNMAP=1  end-of-eval unmap check
```

Run each 3× to characterize noise + cache warmth.  Compare both
hello.drvPath AND HNE.

### 6.3 Validation

* `all-v3-tests --quick` 6/6 PASS under NIX_V3_ARENA_NOROOT=1
* `all-v3-tests --core` 15/15 PASS under NIX_V3_ARENA_NOROOT=1
* hello.drvPath byte-identical to TW under NIX_V3_ARENA_NOROOT=1
* HNE byte-identical to TW under NIX_V3_ARENA_NOROOT=1
* `firefox.name` under NIX_V3_ARENA_NOROOT=1 (heavier nixpkgs path)

If any test fails: that's evidence of a missed Boehm-managed pointer
in arena cells.  Investigate which Value-tag was the carrier (look
at crash signature + diff with the External / String audit).

## 7. Pitfalls to avoid (per session's falsifications)

1. **Don't skip the External-tag audit.** If v3 cells routinely hold
   Tag::External pointing to TW objects, deregistering arena +
   not registering Externals → silent corruption.
2. **Don't assume strings are safe without checking.** TW-bridged
   strings can be Boehm-managed even when v3-native strings are
   arena-allocated.
3. **Don't ship without `--core` PASS.** The first session attempt
   may pass `--quick` but break on more complex flows.  Run --core +
   nixpkgs hello byte-equality before declaring SHIP.
4. **Don't bypass the registry growth re-registration.** If the
   vector reallocates (`push_back` past capacity), the OLD address
   range is no longer the registry — Boehm scans stale memory + may
   miss new entries.  The `reregister()` call MUST fire on every
   capacity change.
5. **Don't forget the huge-block path** in `alloc.hh::alloc(bytes)`
   (the `bytes > kHugeCutoff` branch at line 604-628).  That path
   ALSO calls GC_add_roots; needs the same gate.

## 8. Retirement criterion

When the gate is default-ON (i.e., arena is permanently deregistered
and bridge-root registry is the production mechanism), the env-var
can be removed.  Trigger: `--core` + `firefox.name` + cardano-node
M5 all PASS under the gate, AND measurement shows ≥ 100 MB peak_rss
reduction across two workloads.

## 9. Expected outcome

Per the cost model:
* Today: gc_count=1, gc_total_ms=0, peak_rss = N MB
* After dereg: gc_count = 10-100 (auto-fired by Boehm's normal
  heuristic, now affordable), gc_total_ms ≤ 100 ms, peak_rss = N − Δ
  where Δ ≈ 200 MB on HNE (Boehm watermark stays close to live)

The wall regression should be near-zero because the per-collect cost
has dropped from 40 ms (587 MB scan) to ~1 ms (5 MB scan).  100
collects × 1 ms = 100 ms total — well within noise on a 30-60 s eval.

## 10. Cross-references

* `BOEHM_TUNING_FALSIFIED_2026-05-27.md` — origin of this design
* `alloc.hh:578-715` — Arena class to modify
* `alloc.hh:862-877` — allocBridgeThunk to extend
* `primops.cc:4067-4116` — TW-bridge hooks for External audit
* `value.cc` — Tag::External never constructed by v3 (verified)
* `[[falsification-rule]]` — every commit kills a hypothesis
* `[[measure-twice-cut-once]]` — pre-commit thresholds above

Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
  Input Output Group.
SPDX-License-Identifier: Apache-2.0
