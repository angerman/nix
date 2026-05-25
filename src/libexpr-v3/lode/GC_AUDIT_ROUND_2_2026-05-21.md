# Round 2 GC Audit — Comprehensive Findings

**Date**: 2026-05-21 (same day as `RCA_VALUEPAIR_EVALUATED_2026-05-21.md`).
**Scope**: Exhaustive follow-up to Round 1 (`RCA_VALUEPAIR_EVALUATED_2026-05-21.md`)
covering 5 audit axes — struct/field walks, mutation sites, exception/fiber/
signal/atexit paths, Boehm interop, and tooling infrastructure. Five parallel
agents + independent verification.
**Method**: Each agent was given Round 1's findings as exclusions and tasked
to dig deeper in a specific axis. Findings were cross-verified against the
source before recording.

## 0. Headline

Round 1's primary cause (`ValuePair::evaluated` missed walk) and three of the
other eight findings have **landed as fixes** in `gc.cc` (verified at lines
289-293, 416, 564, 568, 704). Four Round 1 issues remain open: `Thunk::shapeCell`
(gated), `forceWriteTarget` pointer-in-nursery, `forceDeep`/`printNixValueRich`
C-stack hazard, and the Auditor-vs-Scavenger mirror gap.

Round 2 surfaced **twelve new findings** ranging from real-but-rare SIGSEGV
paths (N1 Blackhole tail-walk) through latent issues (N3 CU transitive walk,
N5 thread-local destructor leaks, N6 FFI handles) down to micro-corrections
(N8-N11).

The **single highest-leverage tooling investment** is wiring the existing
`V3_DBG_NURSERY_BRUTE=1` infrastructure into CI — it would mechanically catch
every Round 1 missed-root class AND most Round 2 issues at the cost of 2 days
of test plumbing.

## 1. Status of Round 1's eight findings

| # | Round 1 finding | Status (Round 2 verification) |
|---|---|---|
| 1 | `ValuePair::evaluated` not walked | **FIXED** — `gc.cc:289-293` (fwdPair gate), `gc.cc:416` (walkPair), `gc.cc:704` (Auditor::visitPair) |
| 2 | `importCache().results` not walked | **FIXED** — `walkImportCacheRoots` (primops.cc:6990), called from `gc.cc:564` |
| 3 | `bridgeThunkCache` not walked | **LATENT** — Bridge thunks are tenured-only (`allocBridgeThunk` uses `threadArena`) and currently carry no nursery payload (`t->cell` not set for Bridge state); would re-surface if Bridge cell-update protocol is reintroduced |
| 4 | `g_cachedCallFlake.closureValue` | **FIXED** — `walkCallFlakeRoot` (v3_call_flake.cc:160), called from `gc.cc:568` |
| 5 | `Thunk::shapeCell` not walked | **STILL OPEN** — gated by `NIX_V3_CELL_EVERYWHERE=1` (off by default); four write sites at `vm.cc:6409`, `vm.cc:6523`, `vm.cc:7680`, plus initialisation at `alloc.hh:460`. If the gate is ever flipped on, this becomes an immediate SIGSEGV path |
| 6 | `CallFrame::forceWriteTarget` pointer can be nursery | **STILL OPEN** — `vm.cc:8930` (deepForceList) stores `&list->elems[i]` where `list` may be nursery. Scavenger walks `*forceWriteTarget` at `gc.cc:493` but does NOT update the pointer when the underlying object moves. Silent memoization loss; not a direct SIGSEGV in current code paths |
| 7 | `forceDeep`/`printNixValueRich` C-stack | **STILL OPEN** — `print.cc:33-49` and `:530-542` recurse with `ListVec*` held in C-locals across `forceValue` calls. When invoked AFTER the outer dispatch has returned (`vm.frames.empty()`), the inner `forceValue` enters dispatchLoop at `exitDepth==0` → scavenge enabled → C-locals dangle |
| 8 | Auditor doesn't mirror scavenger's roots | **STILL OPEN** — `postScavengeAudit` (gc.cc:706-727) walks only valueStack/withStack/frames; misses bridge tables, primopReplacementMap, vBuiltins, importCache, callFlake, AttrSelectIC, forceWriteTarget |

## 2. New findings from Round 2

### 2.1 Active / High-severity

**N1 (HIGH, narrow trigger). Blackhole-state tail[] + suspended.capturedWiths not walked.**

- *Site*: `gc.cc:407` (scavenger `walkThunk`) and `gc.cc:680-682` (auditor
  `visitThunk`): both have `case ThunkState::Blackhole: break;` with a comment
  "Currently-being-forced; payload is irrelevant until the body completes."
- *Falsifier*: `clearBlackMarksOnException` (`vm.cc:9400`) reverts
  `Blackhole → Suspended` on exception unwind. After the revert, OP_FORCE
  re-reads `t->tail[i]` and `t->suspended.capturedWiths` to rebuild the fakeClo.
  If a scavenge fired while the state was Blackhole, those fields hold stale
  nursery pointers — direct SIGSEGV path.
- *Trigger conditions*: exception during a thunk body (`assert`, `throw`,
  `primAddErrorContext`) + outer dispatch at `exitDepth==0` + scavenge fires
  during the body. All three are individually common in real workloads
  exercising `tryEval` etc.
- *Fix*: Treat Blackhole identically to Suspended in both walkers (walk tail[]
  and suspended.capturedWiths).

### 2.2 Latent / Architectural

**N2 (MED, gated NIX_V3_FIBER_BRIDGE). Fiber-yielded fiberVm and fiber-stack C-locals invisible to both scavenger and Boehm.**

- *Sites*: `bridge_yield.cc:40-96`, `fiber.cc:130-184`.
- *Two intertwined problems*:
  1. **v3 scavenger**: Re-entry from the driver (TW callback → v3 bridge primop
     → fresh VMState) dispatches at `exitDepth==0` on the FRESH VMState.
     Scavenger walks only its argument VMState; the fiber's frozen `fiberVm`
     is invisible.
  2. **Boehm**: The fiber stack is mmap'd with no `GC_add_roots` registration.
     If Boehm collects while a fiber is yielded, fiber-stack C-locals
     (including TW `nix::Value*` arguments) are unscanned.
- *Stale doc*: `fiber.hh:19-24` claims "the only fiber-stack values that
  matter for GC are tree-walker `nix::Value *` arguments to yields" — this
  ignores the v3 nursery scavenger entirely.
- *Existing context*: `lode/REVIEW_2026-05-06b.md:64` already flags the Boehm
  side as a documented GC defect.
- *Status*: Latent because `NIX_V3_FIBER_BRIDGE` defaults off. Before flipping
  on, must add `GC_add_roots(stack, stack+stackSize)` to `fiberCreate`/
  `fiberDestroy` AND extend the scavenger to walk all live fiber `VMState`s.

**N3 (MED, default-on path). CU transitive walk is incomplete.**

- *Site*: `gc.cc:528-559` walks `attrSelectCache` only for CUs reached from:
  - `frames[].cu`
  - `valueStack` entries with Tag::Closure or Tag::Thunk (Suspended/Blackhole)
  - `withStack` entries with the same tags
- *Gap*: Does NOT walk CUs reached transitively through:
  - Closures inside vBuiltins entries (each could carry its own bytecode CU)
  - Closures inside `primopReplacementMap`
  - Closures captured in upvalue chains (a Closure whose `upvalues[i]` is itself
    a Closure with a different CU)
  - Closures inside `importCache.results`
- *Symptom*: A bytecode-installed primop whose body uses `OP_ATTRS_SELECT_IC`
  against an attrset with nursery-payloaded entries → the IC entry's cached
  Bindings is not walked → stale pointer on next IC hit.
- *Fix*: Use the (currently dead) `Scavenger::walkedCUs` field at `gc.cc:114`.
  Populate from `walkClosure` (insert `c->cu`) and `walkThunk` (insert
  `t->suspended.cu`); drain the set walking IC entries.

**N4 (Dead code, signals incomplete intent). Unused `Scavenger::walkedCUs` field.**

- *Site*: `gc.cc:114` — declared with comment "Populated from every walkClosure /
  walkThunk so any CU transitively reachable from a root is covered." But
  nothing populates or reads it.
- *Status*: Implies an intended-but-unfinished transitive CU walk. Either wire
  up (closing N3) or remove.

### 2.3 Latent / Pre-multi-thread

**N5: `thread_local` Nursery/Arena/ClosurePool have no destructors.**

- *Sites*: `nursery.hh:296` (Nursery), `alloc.hh:364` (Arena),
  `alloc.hh:710` (ClosurePool).
- *Today*: Single-threaded; just a memory + virtual-address leak on thread exit.
- *Future risk*: Multi-thread plans would face Boehm continuing to scan
  freed/recycled memory as if it held v3 objects. `GC_remove_roots` is never
  called; on a recycled buffer this could cause false retention or scanning of
  unmapped memory.
- *Fix when needed*: Add destructors that `GC_remove_roots(base, base+size);
  free(base);` before the thread exits.

### 2.4 Latent / FFI-future

**N6: FFI `HandleSlot::payload` not walked.**

- *Site*: `ffi.cc:56-77`. `ScopeNode::slots[i].payload` is `void*` cast to
  `Value*` by `applyClosure` (ffi.cc:271).
- *Today*: Only test code uses `allocClosureHandle`.
- *Future*: Production FFI consumers would hit this. Need
  `walkEvalScopeRoots(visit)` in ffi.cc called from `gc.cc:run()`.

### 2.5 Latent / Debug-only

**N7: Auditor union-misuse for Native state.**

- *Site*: `gc.cc:635-640` — `visitThunk` groups Suspended and Native, reads
  `t->suspended.capturedWiths` for both. Native's union variant is
  `{ const PrimOp * fn }`, so the read is out-of-bounds for the variant.
- *Doubly latent*: Native state is currently never assigned anywhere in v3
  (only checked at `vm.cc:1914`). Fires only under V3_DBG_NURSERY_AUDIT=1
  AND if Native state is ever reactivated.
- *Fix*: Split the Suspended and Native cases in the auditor (mirroring the
  scavenger at gc.cc:373-398).

### 2.6 Micro-findings

**N8: `clearBlackMarksOnException` leaves CFF_FORCE_WB* flags and `forceWriteTarget` stale.**

- *Site*: `vm.cc:9426-9430` — only clears `CFF_FORCE_RETRY`.
- *Effect*: For re-used VMStates (activeV3VM re-entry pattern), a stale
  `CFF_FORCE_WB / CFF_FORCE_WB_PTR / CFF_FORCE_WB_PTR_KEEP` flag could trigger
  a spurious `applyForceWriteback` on the next opcode landing on that frame.
- *Fix*: Clear all CFF_FORCE_* flags + `forceWriteTarget = nullptr` in
  `clearBlackMarksOnException`.

**N9: `recycleFakeClo` doesn't zero `capturedWiths`.**

- *Site*: `alloc.hh:807` — zeros upvalues but leaves `capturedWiths` stale.
- *Today*: Safe because every pool-consumer at `vm.cc:5958, 9554, 9712, 9833`
  immediately overwrites `capturedWiths`. Defensive coding.
- *Fix*: Zero `capturedWiths` alongside upvalues at recycle time.

**N10: `vm.frames` uses `std::vector<CallFrame>` without traceable_allocator.**

- *Site*: `vm.hh:103`.
- *Today*: Functionally OK because all CallFrame pointer fields (closure,
  thunk, forceWriteTarget) point at memory rooted via other paths (arena
  GC_add_roots). The scavenger walks frames directly.
- *Risk*: If a future scavenger refactor skips the explicit frames-walk in
  some scenario, frames-resident nursery pointers become UAF.
- *Mitigation*: Either switch to traceable_allocator (one-line change), OR
  codify "scavenger MUST walk frames" as a hard invariant in a comment + an
  assertion.

**N11: Brute scanner misses huge allocations.**

- *Site*: `alloc.hh:281-294` — allocations > 4 MB go via `std::calloc` outside
  `Arena::blocks`. `Arena::blockRanges()` (alloc.hh:312) only iterates `blocks`.
- *Effect*: `V3_DBG_NURSERY_BRUTE=1` misses stale pointers stored in any
  tenured allocation > 4 MB (e.g., Bindings with > ~170K entries — rare but
  possible at full-nixpkgs scale).
- *Fix*: Track huge allocations in a separate vector and include in
  `blockRanges()`.

**N12: `installedPrimops()` vector holds redundant Values not walked.**

- *Site*: `bytecode_primops.cc:62`.
- *Status*: Each entry's `rr.value` carries a closure also stored in
  `primopReplacementMap` (which IS walked). The holder's redundant copy is
  never re-read today. Latent — fragility, not active bug.

### 2.7 Boehm interop notes (mostly clean)

The Boehm interop deep dive found **no new SIGSEGV-class bugs** beyond the
fiber-stack issue (N2) and the dead-code findings (N4, comment fixes). Key
positives confirmed:

- **Arena / Nursery init order is correct**: calloc (zero-fill) → GC_add_roots
  → publish pointer.
- **Scavenger does not allocate Boehm memory**: `threadArena().alloc` uses
  libc calloc; scratch buffers use `std::allocator`. Boehm cannot fire
  mid-scavenge.
- **`GC_set_all_interior_pointers(0)` + cppnix's `GC_register_displacement`**:
  handles TW-side tagged Value pointers stored in v3 arena bytes correctly.
- **`GC_set_no_dls(1)`** (set by cppnix's `initGCReal`): BSS is NOT scanned.
  Static caches like `g_vBuiltins` and `g_cachedCallFlake.closureValue` are
  unscanned, BUT their inner v3-payload pointers reach into the nursery/arena
  (both Boehm-rooted) so transitively safe. **Fragile invariant**: any future
  default-allocator container holding `nix::Value*` reachable only via BSS
  would be lost — codify this rule (E.3).
- **Bridge thunks are tenured-only**: bridgeSrc reachability runs through
  the arena scan, robust against scavenge.

Misc:
- `alloc.hh:265` comment claims MAX_ROOTS=1024; actual Boehm 8.2.8 default is
  2048. Comment correction only.
- `alloc.hh:271` says "1 MB blocks" but constant is 16 MB. Stale.
- `heap_trace.cc:94` spawns `std::thread` without `GC_register_my_thread`.
  Defensive only; no current correctness impact.

## 3. Tooling findings

### 3.1 Existing infrastructure

| Tool | Status |
|---|---|
| `V3_DBG_NURSERY_BRUTE=1` | **EXISTS**, exhaustively scans tenured arena for stale nursery pointers. Would catch every Round 1 missed-root mechanically. **NOT wired into any test.** |
| `V3_DBG_NURSERY_AUDIT=1` | **EXISTS**, reachable-graph audit. Has the same blind spots as the scavenger plus extra (Round 1 #8). |
| `V3_DBG_NURSERY_NO_RESET=1` | **EXISTS**, diagnostic for isolating "missed root" from "downstream logic bug". |
| ASan / UBSan via `-Db_sanitize=address` | Compile-supported through meson, wired into the CI matrix for `componentTests`/`vmTests` ONLY — NOT for v3's test suite. Would NOT have caught Round 1 bugs anyway because the nursery memset doesn't free pages. |

### 3.2 Referenced but absent

These appear in `CLAUDE.md`, `LESSONS_LEARNED_2026-05-15.md §4.9`, and
`NURSERY_PHASE_D_DESIGN_2026-05-18.md §8` as required for Stage 3, but have
**zero source-code references**:

- `V3_DBG_GC_STRESS` — force scavenge every N opcodes
- `V3_DBG_RECYCLE_STRESS`
- `V3_DBG_ALLOC_SEED`
- Differential fuzzer
- Property tests for VM invariants
- Crash artifacts on abort

### 3.3 Why ASan didn't catch Round 1 bugs

ASan's UAF detection requires actual `free()`. The nursery `memset`s on reset
but never frees pages. A stale-pointer deref reads zeroed memory and silently
corrupts (or SIGSEGVs at `desc->nLocals` later via NULL deref). ASan is
oblivious. Page-protect-on-reset (`mprotect(PROT_NONE)`) would convert this
to a clean SIGSEGV at the moment of deref.

### 3.4 Prioritized recommendations

| # | Tool | Effort | Catches | Cost |
|---|------|--------|---------|------|
| R1 | Wire `V3_DBG_NURSERY_BRUTE=1` into lang+property+derivation-parity tests | 2 days | All 8 Round 1 + most Round 2 mechanically | Low (small targeted suite) |
| R2 | `mprotect(PROT_NONE)`-on-reset mode for the nursery (gated debug build) | 3 days | Round 1 #1-7 with stack traces at deref site | Dev-time only |
| R3 | Implement `V3_DBG_GC_STRESS=N` (force scavenge every N opcodes) | 3 days | Timing-sensitive bugs (N1, future regressions) | High (slow); nightly only |
| R4 | Differential under stress: same expr with stress=on vs off, assert byte-equal | 3 days | Any future regression of any class | Nightly only |
| R5 | Field-walker registry (codegen or X-macros) | 5-7 days | Round 1 #1-class bugs at compile time | Zero runtime |
| R6 | Mirror Auditor's walks to Scavenger's via shared visitor | 1 day | Round 1 #8 — closes audit asymmetry | Zero |
| R7 | Fix Blackhole `walkThunk`/`visitThunk` to walk tail[] + suspended.capturedWiths | <1 day | N1 | Zero |
| R8 | Fix `clearBlackMarksOnException` to clear CFF_FORCE_WB* + forceWriteTarget | <1 day | N8 | Zero |
| R9 | Wire `Scavenger::walkedCUs` for transitive CU IC walk | 1 day | N3 + removes N4 | Low |
| R10 | Track huge-allocation ranges; include in `blockRanges()` | <1 day | Makes BRUTE complete (N11) | Zero |

**Recommended landing order**:
**R6 → R7 → R8 → R10 → R9 → R1 → R2 → R5 → R3 → R4.**

R6-R10 are each <1 day and close existing gaps. R1 (BRUTE in CI) is the
highest-leverage testing investment because the infrastructure is in place.

## 4. Critical review — falsification attempts

Each Round 2 finding was independently verified before reporting:

- **N1** verified by reading `vm.cc:9400` (the explicit Blackhole → Suspended
  revert) and confirming OP_FORCE re-reads `tail[i]` at `vm.cc:6027 / 11089`.
- **N2** verified by reading `bridge_yield.cc:40-96` + `fiber.cc:130-184`:
  fiber stack is mmap'd, not `GC_add_roots`'d.
- **N3** verified by reading `gc.cc:528-559`: walks only direct frame/stack
  CUs, doesn't recurse into closures' upvalues' CUs.
- **N4** verified by grep: `walkedCUs` is declared at `gc.cc:114`, has no
  callers.
- **N5** verified by reading `nursery.hh`, `alloc.hh`: no destructors
  declared.
- **N7** verified by reading the Thunk union at `closure.hh:158-183`:
  Suspended's `capturedWiths` is at union offset 8, Native's `fn` is at
  offset 0, so the auditor's blind union-read is out-of-bounds for Native.
  Additionally verified Native is dead code (only checked at `vm.cc:1914`,
  never assigned).
- **N8** verified by reading `clearBlackMarksOnException` at `vm.cc:9426-9430`.
- **N9** verified by reading `recycleFakeClo` at `alloc.hh:807` + grep on
  all pool consumers.
- **N10** verified at `vm.hh:103`.
- **N11** verified by reading `Arena::blockRanges()` and the huge-allocation
  path at `alloc.hh:281-294`.

The `bridgeThunkCache` (Round 1 #3) was carefully re-investigated: agent
finding clarifies it's currently LATENT because Bridge thunks are tenured AND
the `t->cell` field is currently never set on Bridge thunks (`vm.cc:5806`
references a defunct `prepHookUpvaluesAndWiths`). Re-surfaces if Bridge
cell-update is reintroduced.

## 5. Bottom line

Round 1's primary cause is fixed. Of the remaining four open Round 1
findings, only N1 (Round 2's Blackhole tail-walk gap) is a SIGSEGV path with
realistic triggers on default settings; the others are gated or non-fatal.

The single most impactful next step is **R1 (wire BRUTE into CI)** — 2 days
of plumbing that mechanizes the catch-all diagnostic the user has been running
manually. After R1, the small R6-R10 fixes close architectural gaps in
under a week of focused work.

## 6. Cross-references

- `RCA_VALUEPAIR_EVALUATED_2026-05-21.md` — Round 1 RCA with the eight
  original findings + fix recommendations.
- `NURSERY_PHASE_D_DESIGN_2026-05-18.md` — predicted the ValuePair::evaluated
  hazard in §3.1 ahead of Round 1.
- `CHENEY_NURSERY_DESIGN.md` — Phase A/C design context.
- `feedback_v3_nursery_cstack_safety.md` — codifies the `exitDepth == 0` gate.
- `LESSONS_LEARNED_2026-05-15.md §4.9` — the 10-item debug story including
  the missing stress modes referenced in §3.2.
- `REVIEW_2026-05-06b.md:64` — earlier identification of the fiber-stack /
  Boehm gap (now also Round 2 N2).

## Copyright

Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
Input Output Group. SPDX-License-Identifier: Apache-2.0
