# V3 Data-Structure Audit — 2026-05-21

Multi-agent review of the runtime data structures in `src/libexpr-v3/`,
prompted by histogram analysis showing the Bindings allocation cost is
dominated by 1,644 large attrsets (~600 MB / ~365 KB each) rather than
the small-attrset tail.

Five agents (Explore subagents) read the following in parallel:

| Agent | Targets |
|---|---|
| Value/ValuePair | `value.hh`, `value.cc`, `vm.cc` (tag readers/writers), `print.cc`, `alloc.hh` (allocPair) |
| Bindings | `alloc.hh` (Bindings def + allocBindings), `closure.hh` (Intrinsic enum), `vm.cc` (OP_UPDATE, mergeBindings), `primops.cc`, `lower.cc` |
| Closure/Thunk/Env/ListVec | `closure.hh`, `alloc.hh`, `vm.cc` (Closure field readers), `bytecode.hh`, `gc.cc` / `nursery.cc` (scavenge walks) |
| LambdaDescriptor | `closure.hh`, `vm.cc` (OP_CALL / OP_FORCE / OP_MAKE_CLOSURE), `emit.cc`, `lower.cc`, `bytecode.hh` (CompilationUnit) |
| VM/bytecode/IR | `vm.hh`, `vm.cc` (dispatch entry), `bytecode.hh`, `ir.hh`, `bridge_yield.hh`, `fiber.hh` |

Synthesis below distinguishes **verified wins** from **overstated agent
claims** from **measurement-needed items**. Every finding cites code
locations. Three claims agents made were verified wrong via
`sizeof()` test programs or direct re-reading; these are listed
separately so they don't propagate.

---

## A. Verified bugs & near-zero-risk wins (land today)

### A1. Instrumentation counters NOT gated as the comment claims

**Severity: bug.** `closure.hh:268-273` says "gated behind
V3_DBG_ALLOC_DUMP for zero cost otherwise." The actual code:

- `vm.cc:3097` — `++cu->lambdas[funcIdx].allocCount;` — **unconditional**.
  The `if (__builtin_expect(g_dbgAllocDump, 0))` at line 3098 wraps
  only the periodic dump body (`cuRegistry().insert(cu)` etc.), not
  the increment.
- `vm.cc:5785` — `++desc->forceCount;` — **unconditional** on every
  OP_FORCE-on-Suspended-thunk.
- `vm.cc:5786` — `++allocStats().thunksForced;` — **unconditional**.

Cost: two 8-byte writes into the `LambdaDescriptor` cache-line on every
OP_FORCE / OP_MAKE_THUNK. The descriptor's hot fields (`codeOffset`,
`nUpvalues`, `nLocals`, etc.) live in the same cache-line as the
instrumentation counters (single 152 B struct), so the writes ping the
line that the dispatch loop is reading. Under hello.drvPath force
volumes this contributes measurable cache pressure.

**Fix (~5 LoC):** wrap each `++` site in
`if (__builtin_expect(g_dbgAllocDump, 0))`.

**Rule 0 falsifier:** `bench-v3-vs-tw.sh hello.drvPath` before/after.
If force-rate doesn't shift, the cache-line hypothesis was wrong and
the fix only buys memory hygiene.

### A2. Tag enum renumbering for `isForced()` fast path

`value.hh:111-115` — `isForced()` does three compares:

```cpp
return t != Tag::Thunk && t != Tag::App && t != Tag::Slot;
```

Renumber Tag so `Thunk=0, App=1, Slot=2` and forced tags = 3..N; the
check collapses to `tag >= 3` (one compare + one branch).

`isForced()` runs on every OP_FORCE, OP_GET_LOCAL_FORCE, OP_GET_UPVALUE,
OP_GET_UPVALUE_FORCE. Mechanical via the enum cast — every reader goes
through `Tag` so renumbering propagates automatically.

**Expected: 1–2 % dispatch win, zero memory change. 3 lines.**

**Rule 0 falsifier:** opcode microbench on a tight thunk-force loop.

### A3. `mergeBindings` over-allocates on duplicate collapse

`vm.cc:920+` allocates `na + nb` slots, then runs the 2-pointer merge.
If duplicates collapse, `out->size = k` is set with `k < na + nb` —
the arena bump pointer has already moved past the unused tail and
cannot reclaim it.

**Fix options:**

1. Pre-pass walk to compute true merge size, then `allocBindings(k)`.
   Extra read pass, but the arrays are small and already cache-warm
   from the merge.
2. Arena "shrink last alloc" — if the merge result is still the most
   recent allocation in the active block, decrement `cur` by the
   unused tail. Requires the arena to expose a `shrinkLast(bytes)`
   API.

**Estimate from Bindings agent: 20–50 MB.** Drop-in fix; no
architectural risk.

### A4. Stale comment fix

`vm.hh:63` says "CallFrame is 40 bytes". Actual layout is 48 B (one
frame per 64-byte cache line with 16 B padding). Cosmetic, ~5 minutes.

---

## B. Likely wins pending measurement (Rule 0 spike first)

Each of these has a plausible multi-MB-to-multi-GB upside but rests on
an unmeasured assumption (typically "field X is null in ≥90 % of
instances"). **Do the measurement spike before the implementation
spike.**

### B1. `Closure::cu` and `Thunk::suspended.cu` as side-table

**Citations:** `closure.hh:65-67` documents "When null, the dispatch
loop uses the caller's CU — fine for intra-CU calls." `allocClosure`
initializes `cu = nullptr` (`alloc.hh:418`); only OP_MAKE_CLOSURE for
cross-CU bytecode sets it. Grep counts: only 2 dispatch-site reads of
`closure->cu` and 11 of `->capturedWiths` in `vm.cc`.

**If null fraction ≥ 80 %:** move `cu` to a sparse side-table keyed
by `Closure*`, save 8 B / closure + 8 B / suspended thunk.

**Measurement spike (~30 LoC):** add a `V3_DBG_CU_NULL` counter
incremented at OP_MAKE_CLOSURE / OP_MAKE_THUNK; run hello.drvPath +
a NixOS sample; report null vs non-null counts.

**Don't implement until the counter justifies it.** Closure/Thunk
agent estimated "2.4 GB" but did not measure.

### B2. `Closure::capturedWiths` & `Thunk::suspended.capturedWiths` similar

Same story — `closure.hh:69-72` says null when no enclosing `with`
at definition time. Same measurement spike (pair with B1 at the same
dispatch site). If null-dominated, encode as a flag bit on
`Closure::_pad` (or steal a bit from `nUpvalues`, which is 16-bit but
needs at most 12 in practice) and look up via sidecar when set.

### B3. `LambdaDescriptor::astLambda`

**Citation:** `closure.hh:384-401` — "nullptr if the lambda was
synthesised internally." Sole reader is the v3ToTreeWalker bridge
path (`primops.cc:4762`). Total descriptor count is small (~50 K), so
total bytes are minor (~400 KB), but eliminating the 8 B field
tightens the hot `LambdaCore` (see B5).

Same measurement: count null vs non-null at descriptor population
time in `lower.cc`.

### B4. Bindings structural sharing for OP_UPDATE — the biggest lever

**Citation:** the histogram's 1,644-allocation / ~600 MB 129+ bucket.
Bindings agent traced the dominant sources via `mergeBindings` callers:

| Site | Context | Volume |
|---|---|---|
| `vm.cc:7590` | `OP_UPDATE` (`prev // overlay`) | very high — every overlay step |
| `vm.cc:3987` | `OP_CALL / ExtendsBody` intrinsic | ~100-500 K |
| `vm.cc:4035` | `OP_CALL / ComposeBody` (2× merge per call) | ~200 K |
| `vm.cc:7662` | `OP_DEFS_IN_SCOPE` | ~50-200 K |
| `primops.cc:6026` | `buildAndWriteDrvNative` result attrset | ~10-50 MB |

The biggest single lever in the whole audit. **But blocked** by the
Tag::Slot pointer-stability rule (`alloc.hh:611-616`) — Bindings
entries are pointed at by long-lived `Tag::Slot` captures and
`Thunk::cell` write-back pointers that must stay pointer-stable.

**Realistic options:**

1. **Append-overlay shape** — `MergedBindings { Bindings * base;
   Bindings * overlay; }` with two-array lookup; only flatten when
   the merged result is observed by something that needs flat storage
   (e.g. iteration over all entries). Lookup cost: two binary
   searches instead of one. Saves the merge entirely when the result
   is forwarded to another merge or only ever queried for a few
   keys. **~2 weeks; ~100-300 MB plausible.**
2. **Full HAMT** — bigger payoff (300+ MB?) but pointer-instability
   problem makes this unsafe pre-Phase-D nursery (per
   `NURSERY_PHASE_D_DESIGN_2026-05-18.md`).

**Recommendation:** pursue (1) after the Phase D audit-first
decision lands. Without that decision, (2) is multi-week speculative
work with cycle-risk.

### B5. `LambdaDescriptor::formals` as inline FAM not `std::vector`

**Citation:** `closure.hh:219-224`. 24 B vector header per descriptor,
even when empty (~60-70 % of lambdas have no formals — simple `x:`
form). Replace with `uint8_t nFormals + Formal * formalsPtr` (12 B)
or move formals into a tail FAM after the descriptor body.

Saves ~14 B per simple-arg descriptor; ~50 LoC across
lower/emit/vm/primops. Memory is small (~500 KB at 50 K descriptors)
but tightens the hot `LambdaCore` cache-line — pairs naturally with
the LambdaCore / LambdaMeta split discussion below.

### B6. LambdaDescriptor hot/cold split (Design A from agent report)

Currently `sizeof(LambdaDescriptor) ≈ 152 B`, spanning 2–3 cache
lines. Hot fields read in dispatch (`codeOffset`, `nUpvalues`,
`nLocals`, `arity`, `selectorSym`, `intrinsicKind`, `identityLambda`,
`cachedSingletonClosure`) total ~40 B and would fit a single 64 B
line. Cold fields (`name`, `contextualName`, `forceCount`,
`allocCount`, `callCount`, `formals` vector, `astLambda`,
`posHandle`) span the second + third lines.

Proposed split:

```
struct LambdaCore {
  uint32_t codeOffset, prologueOffset;
  uint16_t nUpvalues, nLocals, nWithTargets;
  uint8_t  arity, hasFormals, ellipsis;
  uint32_t selectorSym;
  bool     identityLambda;
  int8_t   intrinsicKind, intrinsicVar0, intrinsicVar1, intrinsicVar2;
  Closure* cachedSingletonClosure;
  LambdaMeta * meta;  // cold sidecar
};

struct LambdaMeta {
  uint64_t forceCount, allocCount, callCount;
  std::string name, contextualName;
  uint32_t posHandle;
  std::vector<Formal> formals;
  void * astLambda;
};
```

**Code changes ~300 LoC** across `emit.cc`, `vm.cc`, `lower.cc`,
`primops.cc`. **Stage 9 impact** is real — the linking design
(`LINKING_DESIGN_2026-05-17.md`) assumes whole-LambdaDescriptor
fields in the CellHash; Design A requires schema revision so
`ModuleManifest.ownedCells` carries `CoreCell + optional MetaCell`.

**Defer until either:**
- A1 has landed and a cache-profile shows the LambdaDescriptor line
  is still hot, OR
- Stage 9 is starting and we want to redesign the cell schema once.

---

## C. Agent claims to discard (verified wrong via test programs)

These were proposed by agents but verified incorrect; **do not
attempt**:

### C1. "Drop ListVec `_pad`, save 4 B × 200 M lists = 800 MB"

```cpp
struct ListVec { uint32_t size; uint32_t _pad; Value elems[]; };
// vs
struct ListVec { uint32_t size;                Value elems[]; };
```

`sizeof()` test: both are 8 B. The compiler auto-pads to align the
`Value` FAM (which contains `uint64_t` fields, requires 8 B
alignment). **0 bytes saved.** The `_pad` field is documentation of
the alignment hole, not waste.

### C2. "Drop Bindings `_pad`, save 4 B × N"

Same — `Bindings::Entry` (24 B aligned) requires 8 B alignment;
removing the explicit `_pad` adds 4 B of implicit padding. **0 bytes
saved.** Same code change is welcome for cleanliness, but don't claim
memory wins.

### C3. "Elide `Thunk::shapeCell` allocation, save 4 GB"

The Closure/Thunk agent said the `allocValue()` call at
`alloc.hh:477` happens unconditionally. **Wrong** — it's already
gated by `if (__builtin_expect(s_cellEverywhere, 0))` (line 476).
The *Value backing storage* allocation is conditional. The *8-byte
struct field* in `Thunk` is unconditional, so removing the field via
`#ifdef NIX_V3_CELL_EVERYWHERE` saves 8 B × N thunks (~800 MB at
100 M thunks, not 4 GB). Still worth doing — `shapeCell` is opt-in
and most deployments never set the env var — but the magnitude is
1/5 of the agent's claim.

### C4. "NaN-box Value down to 8 bytes"

Correctly rejected by the Value agent. Tag is in the low byte of a
separate 64-bit word; pointers can't fit in 56 bits reliably on
aarch64 (TBI) or x86-64 (5-level paging). **Don't attempt.**

### C5. "Multi-GB savings from Closure::cu side-table"

Magnitude depends on closure-allocation count (not measured) AND
null fraction (not measured). The B1/B2 measurement spike is the
prerequisite — don't quote multi-GB numbers without a count.

### C6. Small-int unboxing in upper 56 bits

Mechanically possible (`value.hh:65-66` reserves the bits) but
requires rewriting every `mkInt` / `tag()` / `payload.i` site
(~25 + ~200 sites). Architectural, not a near-term win.

---

## D. Already optimal (don't touch)

- **Value 16 B** — tag + payload split is correct; PrimOpApp and App
  share `ValuePair` and the App memoization field justifies the 16 B
  cost (see `value.hh:170-182`); singletons already statics.
- **CallFrame 48 B** — fits 1 frame / cache line cleanly; the "40 B"
  comment in `vm.hh:63` is stale (see A4) but the layout itself is
  fine. `forceWriteTarget` (8 B) is cold but kept inline for locality
  with the CFF flags.
- **Bytecode 32-bit fixed-width** — variable-length encoding (WASM
  LEB128 style) would hurt dispatch i-cache more than it saves disk.
  Revisit only for Stage 9 disk-cache compression.
- **IR Expr variant** — `std::variant` (sum type, not virtual) is
  correct for build-time IR. Discriminant compression is a Stage 9
  disk-cache concern, not runtime.
- **Fiber / Mailbox** — `bridge_yield.hh` mailbox 48 B + fiber
  ucontext ~500 B; not on dispatch hot path (yields are ~1 K per
  large eval, not 100 M).
- **Empty-Bindings sentinel** — already deployed (`alloc.hh:590`).
- **dispatchLoop env-var caching + threadNursery() hoisting** —
  already done correctly (`vm.cc:2136-2141`).

---

## Recommended order

| # | Action | Effort | Risk | Falsifier |
|---|---|---|---|---|
| 1 | A1 — gate `allocCount`/`forceCount`/`thunksForced` increments | 1 h | low | bench-v3-vs-tw delta on hello.drvPath |
| 2 | A2 — renumber Tag for `isForced() = tag >= 3` | 30 min | low | OP_FORCE microbench |
| 3 | A4 — fix `vm.hh:63` "40 B" stale comment | 5 min | none | n/a |
| 4 | A3 — `mergeBindings` shrink-avoidance | 1 day | low | `bytesBindings` drop on hello.drvPath |
| 5 | Measurement spike — add `V3_DBG_CU_NULL` / `V3_DBG_WITHS_NULL` / `V3_DBG_AST_NULL` counters; run hello.drvPath + a NixOS sample | 1 day | none | data table for the next decision |
| 6 | Conditional on (5) — pick the highest-null field and implement side-table | 2-3 days | medium | bytes-saved on the same workload |
| 7 | C3 — `#ifdef NIX_V3_CELL_EVERYWHERE` around the `shapeCell` field | 0.5 day | low | thunk struct size drops 8 B |
| 8 | B5 — `formals` inline FAM instead of `std::vector` | 1 day | medium | sizeof(LambdaDescriptor) shrink |
| 9 | B6 — LambdaCore / LambdaMeta split | 1 week | high (Stage 9 coupling) | requires cache profile motivating it |
| 10 | B4 — Bindings append-overlay shape for OP_UPDATE | 2 weeks | high (Phase D coupling) | requires Phase D decision first |

**Net:** items 1-4 are low-risk and land today/this week. The
multi-GB story (B1/B4) is real but unverified — the right next
move is a measurement spike (item 5), not speculative
implementation.

---

## Discarded-claim audit summary

Three of the five agents made at least one claim that didn't
survive verification (C1/C2/C3/C5). This is **good** — the
parallel agent setup let the synthesis layer catch the errors.
The pattern: agents project memory savings by multiplying field
size × allocation count *without verifying* the field actually
occupies independent bytes (C1/C2) or that the allocation is
unconditional (C3). Future audits should require a `sizeof()`
sanity check on every quoted size, and a `grep` for the
conditional gate on every quoted allocation count.

---

Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
Input Output Group. SPDX-License-Identifier: Apache-2.0.
