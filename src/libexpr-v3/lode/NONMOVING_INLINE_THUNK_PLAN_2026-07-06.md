# Non-moving tenured GC + inline `{env,code}` thunks — phased plan + first spike

**Date:** 2026-07-06
**Status:** DESIGN + REVIEW ONLY (no production code; no builds). Execution handoff.
**Lever:** the N1 finding of `BEAT_TW_LEVERS_AND_ARCHITECTURE_2026-07-06.md` — the
CPU (not RSS) case for a non-moving tenured region. C1 verdict = **GO**: barrier
removal ALONE measured **16.4 % (firefox) / 19.0 % (git)** warm CPU on darwin-4
(median-5, byte-id 5/5). This plan is how to bank that (and the additional
separate-thunk-alloc + scavenge-copy components) increment-by-increment.

**Read first:** `IMMIX_NOFL_DESIGN_2026-06-02.md` (the committed non-moving
family = Nofl-style precise mark-region), `GC_DECISION_2026-05-29.md` (PIVOT-IMMIX
+ what survives), `GC_PAUSE_2026-05-29.md` + `IMMIX_FALSIFIED_2026-05-29.md` (the
6+1 RSS falsifications — this is a DIFFERENT justification, same discipline),
`barrier.hh` (the `-DNIX_V3_BARRIER_NOOP` apparatus), CLAUDE.md Rule 0 + Critical
Constraint 0.

---

## 0. The one-paragraph thesis

The barrier tax the C1 A/B measured is not the whole prize — it is the *lower
bound*. The barriers, the separate 24 B Thunk cell, and the Cheney scavenge-copy
are three symptoms of **one** root cause: the tenured heap is served by a *moving*
collector (the nursery Cheney copy + the gen-major promote), so a Value living in
a tenured `Bindings` cannot itself hold a mutable, forwardable `{env,code}`
payload — v3 must indirect through a separately-allocated Thunk cell AND track
every old→young write so the scavenger can fix up pointers. Make the tenured
region **non-moving** (objects never move → pointers are stable), and all three
symptoms are addressable: (a) a suspended thunk's `{desc,env,withs}` can live
*inline* in the very cell the Value points at and be mutated in place to the
forced result, eliminating the separate-cell alloc for the tenured case; (b) the
Phase-D barriers + remembered set + scavenge-copy for the tenured generation drop
(non-moving ⇒ no inter-gen pointer fixups). The substrate to do this is **already
in tree** and correctness-clean — it was only ever KILLED on RSS grounds.

---

## 1. What already exists (do NOT rebuild it)

The 6-week Stage-6 GC effort left a large, correctness-validated, **gated-off**
substrate. The RSS payoff was falsified (`IMMIX_FALSIFIED_2026-05-29.md`), the
track paused (`GC_PAUSE_2026-05-29.md`), but the machinery stands:

| Asset | Where | State |
|---|---|---|
| Precise root walk `walkAllV3Roots` + `RootVisitor` (per-type `visit*` + `visitValue` hub + `visitEnv`) | `include/v3/precise_root.hh:85-222`, `precise_root.cc` (367 L) | SHIPPING (used by mark, live_trace, audit) |
| Precise `BitmapMarker` + `MarkVisitor` + non-moving `runMajorMarkSweep` | `mark_sweep.cc:2229` (2794 L total) | gated `g_majorGcEnabled` (hard-false) |
| Conservative C-stack scan `walkCStackConservative` | `mark_sweep.cc:849` | SHIPPING at the safepoint |
| Cell-start bitmap + nibble-packed `CellType` side-table (stamped by typed allocators) | `alloc.hh:1256-1467` (`cellStarts`, `cellTypes`) | gated `cellMetaEnabled()` |
| **Immix line-mark bitmap + free-span line-region allocator + recycle policy (Steps 11′-13′)** | `alloc.hh:1354-1467` (`lineMarks`, `freeSpans`, `FreeSpan`), `alloc()` line-region fast path `alloc.hh:1541-1588` | gated `g_immixAllocEnabled` (built, correctness-clean, <30 % hit) |
| `freeWholeBlock` → munmap (mmap'd blocks only) | `alloc.hh:2371` | gated `blocksAreMmapped()` |
| Optional evacuation from sparse blocks (Nofl "opportunistic evac", R2.4) | `mark_sweep.cc:1220` (`runMajorMarkSweep` relocates) | gated, validated ~100 % reach (703cff5ed) |
| Free-list reuse (`freeListBins_`) | `alloc.hh` freeListTryPop | gated `g_freeListReuseEnabled` / mid-eval |
| **The always-on gen-major safepoint** (empties nursery via `forceScavenge`, then marks) + IC/materialize-memo invalidation | `vm.cc:4685-4780`, `g_genMajorEnabled=true` (`vm.cc:381`) | **SHIPPING default-on** |
| C1 barrier-noop A/B apparatus | `barrier.hh:162-186` `#ifdef NIX_V3_BARRIER_NOOP` | SHIPPING as measurement valve |

**The gap** the RSS effort hit: it never returned pages to the OS at *peak*
(dead cells uniformly interleaved → `blocksFreed=0`; P2 falsifier KILL). That is
irrelevant here. **This lever is CPU**: eliminate the per-write barrier work, the
separate-thunk alloc, and the scavenge-copy. Those pay off whether or not a single
page is munmap'd.

### 1.1 The current allocation + force reality (the thing we are changing)

- `nurseryOrArena(bytes,type)` (`alloc.hh:2877`) routes **Thunk / Closure / List /
  Pair / Value** nursery-first, tenured on overflow. **Bindings and Env go
  straight to `threadArena()`** (always tenured — `alloc.hh:3177/3033`).
- So the canonical case the inline-thunk idea targets — a lazy attr `{ x = e; }`
  — is: a **tenured** `Bindings` entry holds a `Value{Tag::Thunk}` pointing at a
  **separately-allocated** `Thunk` cell (usually nursery-born, 24 B header + FAM
  upvalues). Every write of that thunk-Value into the tenured Bindings goes
  through `bindingsSetValue` → the Phase-D barrier (`barrier.hh:247`).
- Force: `OP_FORCE` on a Suspended thunk sets `t->state = Blackhole`, pushes a
  `CFF_THUNK_RETURN` frame; on completion `OP_RETURN` publishes the WHNF into the
  thunk's `evaluated` union member and `cellWrite(f.forceWriteTarget, forced,
  nullptr)` writes it back into the origin cell in place (`vm.cc:3451/3471`).
  Cycle detection = re-forcing a `Blackhole` thunk → `BlackholeError`.
- The thunk header is **24 B** (`closure.hh:238` static_assert): `{state,
  hasWithsSlot-flags, nUpvalues, forces}` (8) + `cell` (8) + `union{desc |
  evaluated}` (8), with a FAM tail carrying upvalues (or a shared `Env*` under
  env-sharing) + an optional withs slot. `desc->cu` gives the CU (`thunkCU`).

---

## 2. The representation change (Deliverable §1)

### 2.1 The constraint that forces the design

`Value` is a **NaN-boxed 8-byte word** (`value.hh:120-261`, `static_assert
sizeof(Value)==8`). It CANNOT hold `{env,code}` inline — it holds one 48-bit
pointer or a 48-bit immediate. So "inline thunk" does **not** mean "widen the
Value." It means: **the cell the thunk-Value points at is a right-sized in-region
cell whose header IS the suspended `{desc(→code), env(upvalues), withs}` and which
is mutated in place, on the same address, to the forced value on force** — no
*second* cell, no forwarding, because the region is non-moving so that address is
stable forever.

This is already almost exactly today's `Thunk` layout. The change is not the
*shape* of the Thunk cell; it is (a) **where** it is allocated (a non-moving
tenured region instead of the moving nursery, for the tenured-container case) and
(b) **removing the separate-cell allocation** for the dominant pattern where a
tenured Bindings entry is the sole reference — i.e. fusing "allocate a Thunk cell"
with "the Bindings entry that will hold it," so no distinct nursery cell is born
that must then be scavenge-copied and barrier-tracked.

There are **two** legitimate readings of "inline"; the plan does the cheaper,
lower-risk one first and treats the aggressive one as an optional later phase:

- **Reading A (the one this plan commits to first): non-moving Thunk cell, same
  24 B shape, allocated directly in the non-moving tenured region, forced in
  place.** The Value in the Bindings still points at a Thunk cell — but that cell
  never moves, so (1) no barrier is needed for the Bindings→Thunk edge, (2) no
  scavenge-copy of the thunk, (3) the `cell` writeback + `evaluated` in-place
  mutation are already how force works — they just stop racing with a mover. This
  banks the **entire C1 barrier tax + the tenured scavenge-copy** and is a pure
  *allocator/collector* change: **Value/Thunk/Closure layout is UNCHANGED.** This
  is the safe, measurable core.

- **Reading B (optional, Phase F, high-risk): eliminate the pointer indirection
  for the tenured-Bindings-sole-owner case** — store the suspended `{desc,env}`
  *in the Bindings entry's value slot region itself* (an out-of-line "thunk tail"
  co-allocated with the Bindings, or a per-entry inline thunk area), so there is
  no distinct Thunk cell to allocate at all. This is what removes the `allocThunk`
  call (C1b ≈ 2 %). It touches Bindings layout, `bindingsSetValue`, the
  cell-writeback, and every walker — high blast radius. **Do NOT attempt B before
  A ships and is measured.** Most of the win is in A (barrier 16 %; scavenge
  1.2–7 %); B adds only the ~2 % alloc slice at large risk.

### 2.2 What breaks / what to check (per §1's "what breaks" prompt)

- **App-memoization ValuePair** (`ValuePair::evaluated`, `value.hh:297`): pairs
  are always tenured (`allocPair`→`threadArena`). Under a non-moving tenured
  region they simply never move — `pairSetEvaluated`'s barrier (`barrier.hh:290`)
  becomes unnecessary for the pair itself. No layout change; memo semantics
  identical (same address, in-place `evaluated` write).
- **FAM upvalue arrays** (`Thunk::tail[]`, `Closure::upvalues[]`, `ListVec::elems[]`):
  variable-size cells. Immix mark-region handles variable size natively (objects
  span granules/lines; `IMMIX_NOFL_DESIGN §4`). The `CellType` side-table + the
  size accessors (`thunkScanSize`, `closureAllocatedSize`, `ListVec`) already give
  the mover/marker the exact byte size — reuse them verbatim. No FAM change.
- **drv-hash-critical relocation assumptions**: NON-MOVING ⇒ addresses are stable
  ⇒ *fewer* relocation hazards than the current moving nursery, not more. The one
  live relocation hazard (the optional evacuation, Phase E) is deferred and
  gated; the core (A) does zero relocation. `Closure::desc`/`desc->cu` point into
  `ImportCache::cus` (libc deque) and must NOT be walked as arena pointers —
  already handled (`tagIsPointer` excludes them; `visitString`/`visitPath`
  default no-op; the mark visitor's skip-list per `STAGE_6_IMPL_GUIDE Day 4`).
- **`Tag::Slot`** (`Value*` into a tenured cell, `value.hh:66`): slots point at
  let-rec cells and are followed by `visitSlot`. Non-moving keeps them valid
  permanently — today they are the single trickiest thing for the mover. Strict
  improvement.
- **String/Path `const char*`** (`allocChars`, `CellType::Chars`): immutable arena
  text. Non-moving ⇒ no forwarding, no string-context re-keying. Strict
  improvement vs the moving design's §3.1 headache.

### 2.3 Force / blackhole / cycle detection without a separate cell

Unchanged for Reading A (the cell still exists, just never moves): `state`
transitions Suspended→Blackhole→Evaluated on the *same* cell; `BlackholeError` on
re-force; `evaluated` written in place; `cellWrite` publishes to the origin. For
Reading B (later), the "thunk" is the Bindings-entry region: blackhole becomes a
`Tag::Blackhole` write into the entry's Value slot + a side flag for the in-flight
`{desc,env}`; cycle detection reads that tag. Specified only when B is scheduled.

---

## 3. The GC reconciliation (Deliverable §2)

### 3.1 Target: Nofl-style precise non-moving mark-region for the TENURED region

Per `IMMIX_NOFL_DESIGN_2026-06-02.md` §1 this is the *committed* non-moving family.
The tenured allocator becomes: bump-allocate into holes (free line-spans) of
non-moving blocks; mark precisely via `walkAllV3Roots`+`RootVisitor` setting
line-marks; reclaim by recomputing free-spans (no copy). Steps 11′-13′ already
implement the line-mark bitmap + free-span allocator + recycle policy
(`alloc.hh:1354-1588`) — they were built and are correctness-clean; they were only
short on the RSS *hit-rate/peak* gate, which is not this lever's gate.

### 3.2 Nursery: stays, but the promotion target goes non-moving

- **Keep the nursery as a moving bump-front-end** initially (Reading A minimal):
  the young generation still Cheney-copies survivors, but the *tenured* promotion
  destination is the non-moving mark-region. This is the least-disruptive first
  cut and lets us measure the tenured-side win in isolation. Note: with a moving
  nursery you STILL need the old→young barrier for *young* roots — so the **full**
  C1 barrier win requires the next step.
- **Then subsume the nursery** (Nofl sticky-mark-bit generational, `IMMIX_NOFL §3.3
  / §5.5`): the metadata byte's mark state is "sticky" across minor cycles → young
  = the unmarked; one non-moving mechanism replaces the copying nursery entirely.
  This is what lets ALL Phase-D barriers drop (no moving generation anywhere → no
  inter-gen pointer fixups → no remembered set). This is the phase that banks the
  *full* measured 16 % barrier tax. v3's young mortality is weak (0.43–0.58,
  `L_TIME_SERIES`), so losing the copying nursery's compaction is cheap.

### 3.3 What is deleted vs kept (per PIVOT-IMMIX §6 + this lever)

**Deleted (once the non-moving generation subsumes the nursery, Phase D):**
`barrier.hh`'s 11 Phase-D barrier bodies (bindingsSetValue/Entry, pairSet/PostConstruct,
thunkSet/PostConstruct, closure/env/list/bindings PostConstruct, cellWrite's
inter-gen arm), `dirtyContainers()` + `standaloneCellRoots()` remembered set,
`isNurseryPayload`, the Cheney scavenge-copy in `gc.cc` (`fwdThunk`/`fwdClosure`/
`visitValue`/survivor pool), `Thunk::cellContainer` derivation (already gone).
**Kept:** `walkAllV3Roots` + `RootVisitor`, `BitmapMarker`, conservative C-stack
scan, `bridge_root_registry`/`walkV3BridgeRoots`, `walkImportCacheRoots`, IC +
materialize-memo invalidation, whole-block-free, the safepoint model
(`exitDepth==0` + nested-VMState defer).

---

## 4. THE PHASING (Deliverable §3) — dependency-ordered, each byte-id-gated, each with a pre-committed CPU gate

De-risk order: prove **non-moving tenured is correct** → prove it **wins CPU on the
tenured side** → **drop the barriers** (the measured 16 %) → optionally subsume
nursery / inline. Each phase is an independently-committable increment behind a
build flag, validated `--brute 22/22` + drvPath byte-id (hello/git/firefox/python3),
measured warm-CPU on darwin-4 median-5.

| Phase | Increment | Correctness gate | **Pre-committed CPU gate (KILL if unmet)** |
|---|---|---|---|
| **S (SPIKE)** | Non-moving tenured region behind a flag: flip the gen-major mark from "mark only" to "mark + line-region reclaim into the SAME non-moving blocks" using the existing Steps 11′-13′ allocator, **but do NOT drop any barrier and keep the nursery moving.** Prove the tenured region is correct + get a first CPU signal. | `--brute 22/22`; hello/git/firefox/python3 drvPath byte-id ON==OFF==golden | **Signal only** — expect ≤ ±3 % CPU (this phase changes allocator, not barriers). KILL only if byte-id fails or CPU regresses > 8 %. This phase's job is CORRECTNESS + a foothold. |
| **A** | Route the **tenured** thunk/closure/list/pair allocation (the `nurseryOrArena` overflow path + all always-tenured Bindings/Env) into the non-moving region, and make gen-major reclaim it in place (no evac). Nursery still moving. Tenured-side scavenge-copy disappears. | same | Tenured scavenge-copy component: M5 `tryMark`/`fwdThunk`/`visitValue` self-time should drop measurably. Gate: **≥ 3 % warm CPU on M5 OR firefox → proceed; < 1.5 % → investigate before B** (do not KILL the whole plan — A alone is not where the 16 % lives). |
| **B** | Nofl sticky-mark-bit generational: subsume the copying nursery into the non-moving region (young = unmarked). Removes the *moving* generation entirely. | same + a soak (repeated HNE/M5 eval, no leak) | This is the enabling step for C; on its own expect neutral CPU (allocation path changes). Gate: neutral-to-positive; KILL if > 5 % regression that A-scavenge-removal doesn't offset. |
| **C** | **Drop the Phase-D barriers.** With no moving generation, `phaseDActive()`→false is SOUND (this is exactly the `-DNIX_V3_BARRIER_NOOP` config, now made correct because there is no scavenge to consume the remembered set). Delete the barrier bodies + remembered set. | same + soak | **THE gate: ≥ 15 % warm CPU on firefox AND ≥ 12 % on git (the C1-measured numbers, 16.4 % / 19.0 %, are the target) → SHIP; < 8 % → KILL and revert to gated.** M5 extrapolated ≥ firefox (more thunk-heavy). |
| **D** | Delete the Cheney scavenger + survivor pool + nursery moving machinery (now dead). Retire `gc.cc` copying paths, `dirtyContainers`, `isNurseryPayload`. | `--brute 22/22`; byte-id; soak | Cleanup; CPU-neutral vs C. KILL only on regression. |
| **E (opt)** | Opportunistic evacuation from sparse blocks (the ONE moving feature of Nofl) — only if RSS becomes a goal again. **Not on the CPU critical path.** | full brute + evac stress | RSS gate per `IMMIX_NOFL §8` (out of scope for this lever). |
| **F (opt)** | Reading-B true inline thunk (eliminate the separate Thunk cell for the tenured-Bindings-sole-owner case), banking C1b ≈ 2 %. High blast radius. | full brute + byte-id + soak | **≥ 2 % warm CPU beyond C → keep; else document-close.** Only after C ships. |

**Why this order de-risks:** S proves "non-moving tenured is correct" before any
representation touch. A proves the tenured *win* exists (scavenge-copy). B is the
mechanical enabler with no user-visible semantics change. C is where the measured
16 % is banked — and it is *sound* only after B (per the barrier.hh:172 caveat:
noop is byte-id ONLY when no scavenge fires; B removes the scavenge). D/E/F are
cleanup and optional upside.

---

## 5. THE FIRST SPIKE — fully specified (Deliverable §4), executable directly

**Goal:** prove the non-moving tenured region is **correct** (byte-id + brute) and
get a first CPU reading, with the SMALLEST possible change and ZERO representation
or barrier changes. It reuses the existing, gated Steps 11′-13′ machinery.

### 5.1 The hypothesis this spike kills

> "Making the tenured region reclaim in place (non-moving line-region), with the
> existing precise mark + gen-major safepoint, is byte-identical to the current
> moving-tenured behaviour and does not regress warm CPU > 8 %."

If byte-id fails → the non-moving-tenured path has a missed root the moving path
masked; STOP and RCA before any further phase. If it passes → the foundation for
Phases A-C is proven cheaply.

### 5.2 Exact files / functions to touch

1. **New build flag** `NIX_V3_NONMOVING_TENURED` (compile-time, like
   `NIX_V3_BARRIER_NOOP`). Add a `constexpr bool nonmovingTenured()` in
   `include/v3/alloc.hh` next to the `detail::g_*` gates (~`alloc.hh:1242`), with an
   inline retirement criterion in the comment (Rule 5: no bare getenv; this is a
   `-D` compile flag, not a runtime getenv — matches the barrier-noop precedent).
2. **`alloc.hh` — turn the gated Immix path on under the flag.** The line-region
   allocator at `alloc.hh:1548` currently keys on `majorGcEnabled() &&
   g_immixAllocEnabled` (both hard-off). Add: when `nonmovingTenured()`, (a) make
   `blocksAreMmapped()` return true (so `freeWholeBlock` works and blocks are
   reclaimable) and `cellMetaEnabled()` return true (cell-start + CellType +
   lineMarks maintained), and (b) let `alloc()` take the line-region fast path.
   This is flipping existing gates, not writing an allocator.
3. **`vm.cc` — extend the gen-major safepoint (`vm.cc:4718`).** Today
   `s_genMajor` empties the nursery then runs `runMajorMarkSweep`. Under the flag,
   after mark, call `arena.rebuildFreeSpansFromLineMarks()` (exists,
   `alloc.hh:1864`) so the *next* allocations bump into reclaimed line-spans of the
   SAME blocks (in-place reclaim, no move). Do NOT enable evacuation
   (`runMajorMarkSweep`'s sparse-block relocation stays off).
4. **Keep the nursery moving and ALL barriers ON** (this spike changes only the
   tenured allocator's reclaim behaviour). No `barrier.hh` edit, no `value.hh` /
   `closure.hh` edit.

### 5.3 The +/−/R tests (Rule: every change gets positive + negative + regression)

- **(+) positive:** `test/repro-nonmoving-tenured-pos.nix` — a `let`-heavy /
  rec-attrset expr that forces many tenured thunks across a gen-major safepoint
  (size the expr so `bytesAllocated()` crosses `NIX_V3_MAJOR_GC_THRESHOLD_MB`
  default 256 MB at least twice). Assert result byte-id to TW.
- **(−) negative / stress:** run under `V3_DBG_GC_STRESS=1000` (force scavenge +
  mark every 1000 ops) AND a tiny `NIX_V3_MAJOR_GC_THRESHOLD_MB=16` so the
  non-moving reclaim fires constantly — this is the missed-root surfacer. Expect
  NO crash, byte-id. (Mirrors the `--brute` moving-GC stress rationale.)
- **(R) regression:** the full `--brute` battery is the regression guard.

### 5.4 Byte-id gate + brute expectation

```bash
# Correctness (runs anywhere — laptop fine):
nix develop -c bash test/all-v3-tests.sh --brute          # expect 22/22 ALL GREEN
# drvPath byte-id, flag ON vs OFF vs golden (source test/nixpkgs-pin.sh first):
for p in hello git firefox python3; do
  a=$(NIX_V3_DIRECT_EVAL=1 v3-eval --impure --expr "(import <nixpkgs> {}).$p.drvPath")   # OFF
  b=$(... same, built with -DNIX_V3_NONMOVING_TENURED ...)                                # ON
  [ "$a" = "$b" ] || echo "DIVERGE $p"
done
# Expect: ON == OFF == the run-brute-audit.sh golden (hello-2.12.2 / git-2.51.2 / firefox-148.0 per CLAUDE.md).
```

### 5.5 The measurement (perf gate — darwin-4 ONLY, median-of-5)

```bash
# On aarch64-darwin-4.lan, warm cache, build BOTH binaries (OFF baseline + ON),
# median-of-5 wall via the committed methodology:
COMMIT=<laptop HEAD> bench/profile-at-scale.sh          # firefox + M5 + HNE, warm
```
Compare against **two** references: (1) the OFF baseline (regression budget: warm
CPU must not regress > 8 %); (2) the `-DNIX_V3_BARRIER_NOOP` ceiling from C1
(firefox 1.68 s / git 0.68 s) — the spike is NOT expected to reach that ceiling
(it keeps barriers on), it is expected to be ≈ the OFF baseline. **git-note the
result to the tested commit** (`git notes append`) + a row in
`bench/baselines/darwin4-rows.tsv`. Deterministic counters (opcode histogram,
scavenge count, `is.spanHits`/`bumpFresh` Immix hit-rate) are host-independent —
capture them on the laptop too.

**Spike verdict logic:** byte-id PASS + brute 22/22 + CPU within ±8 % → foundation
proven, proceed to Phase A. Byte-id FAIL → RCA the missed root (build a minimal
repro per LESSONS §4.8) before anything else — this is the highest-value negative.

---

## 6. ADVERSARIAL UAF / CORRECTNESS REVIEW (Deliverable §5) — ranked, mitigated, phase-validated

Non-moving *removes* the forwarding-class hazards but introduces reuse-class ones.
Ranked by likelihood × blast-radius:

1. **[HIGH] Reclaim hands out a cell that is actually still LIVE (mark-completeness
   gap) or C-stack-live inside a primop body.** This is NOT hypothetical: it is a
   **known-open, currently-SEGV'ing bug** — `NIX_V3_MIDEVAL_REUSE` (`alloc.hh:975`)
   "currently SEGVs: popping a swept cell that is still live (a residual mark-
   completeness gap) or wrong-sized," which is why the correct sweep+bin path
   (`NIX_V3_MIDEVAL_GC`) is SPLIT from the reuse/pop (`NIX_V3_MIDEVAL_REUSE`,
   default-off). Same class as the HNE SIGBUS under `freeListReuse`
   (`alloc.hh:1512-1527`): a primop C-local holds a raw pointer to a cell the
   allocator (which does NOT consult the C-stack) re-hands.
   *Mitigation:* (a) **the Spike deliberately does NOT enable reuse/pop** — it
   uses the line-region *span* reclaim path (`g_immixAllocEnabled`), which
   rebuilds spans ONLY at the safepoint from the freshly-recomputed line-marks,
   never mid-primop, and never reclaims a span whose block `anyMarkInRange` pins
   (incl. conservative C-stack marks; `mark_sweep.cc:1334`). (b) Before promoting
   any reclaim to default, land the reuse-safety diagnostic already scaffolded
   (`alloc.hh:980`: sentinel-stamp-on-bin + verify-on-pop) to catch the live-but-
   reclaimed cell. The open MIDEVAL_REUSE SEGV is the **failing-first test** for
   this hazard — it must be root-caused (the mark-completeness gap) and closed
   before Phase A ships in-place reclaim. *Validated in:* Spike §5.3 (−) stress
   (which will reproduce it if present) + the sentinel diagnostic + HNE/M5 soak in
   Phase A/B.
2. **[HIGH] Barrier drop (Phase C) with a residual moving generation.** If ANY
   moving path survives when barriers are removed, an old→young edge is missed →
   forwarding miss → the `STALE THUNK ... nursery=YES` abort (the M5 negative in
   the C1 caveat, `barrier.hh:176`). *Mitigation:* C is gated on B having removed
   the moving nursery ENTIRELY; add an assertion/`--brute` invariant that
   `scavengeCount==0` for the whole eval before C's barrier removal is allowed to
   ship. *Validated in:* Phase C soak; the existing `NIX_V3_BARRIER_NOOP` M5-abort
   is the pre-built failing-first test (it MUST stop aborting after B).
3. **[MED] In-place thunk mutation racing cycle detection / blackhole.** Force
   sets `state=Blackhole` then `Evaluated` on the same cell; a non-moving region
   keeps the address stable, so a concurrent walker (a mark at a nested safepoint)
   could see a Blackhole cell. *Mitigation:* single-threaded VM + safepoints only
   at `exitDepth==0` means no mark runs *during* a force (the force holds the
   dispatch loop). `BitmapMarker` must treat `Tag::Blackhole` / in-flight thunks
   as live (they are reachable from the frame). *Validated in:* Spike (+) forces
   thunks across a safepoint; Phase A.
4. **[MED] IC / materialize-memo raw-Bindings pointers dangling after reclaim.**
   `attrSelectCache`/`recSlotCache`/`Bindings::materialize` hold raw `Bindings*`
   not walked as roots (`vm.cc:4749-4774`). A reclaimed-in-place Bindings whose
   only reference is an IC = UAF. *Mitigation:* the safepoint ALREADY invalidates
   these before mark (`vm.cc:4765`); the spike keeps that path. *Validated in:*
   Spike brute (the R2.4d code-review #1/#6/#7/#8 class).
5. **[MED] Interior `Tag::Slot` pointers into a reclaimed cell.** A `Slot` points
   into a let-rec Bindings; if the target line is reclaimed while a Slot still
   references it → UAF. *Mitigation:* `visitSlot` + the interior-owner discovery
   (`mark_sweep.cc` `visitor.setArena` + `findContainingCellStart`) marks the
   owning cell live; non-moving keeps the Slot valid. Strictly safer than the
   moving case. *Validated in:* Spike brute (chain-parity + brute-audit cover
   let-rec/with).
6. **[LOW] Fragmentation: a block holding one live inline thunk can't be
   whole-freed.** This is the RSS pin (P2 KILL), NOT a correctness bug, and NOT
   this lever's concern. *Mitigation:* accept it (CPU lever); Phase E evac only if
   RSS re-enters scope. *Validated in:* n/a (documented non-goal).
7. **[LOW] `desc`/`cu`/String/Path pointers into libc (ImportCache deque, symbol
   table) mistaken for arena pointers.** *Mitigation:* `tagIsPointer` excludes
   them; the mark skip-list per `STAGE_6_IMPL_GUIDE Day 4`; conservative scan
   filters by arena bounds (`tryMark` rejects non-arena, `mark_sweep.cc:111`).
   Already handled. *Validated in:* Spike brute.

---

## 7. HONEST CEILING + PER-PHASE KILL GATES (Deliverable §6)

### 7.1 The three additive components (C1 decomposition, `BEAT_TW §C1`)

- **C1a barrier tax: MEASURED 16.4 % firefox / 19.0 % git** (darwin-4 median-5,
  byte-id). This is the bulk and it is the *floor* of the prize. Banked at **Phase
  C**.
- **C1b separate-thunk-alloc: ≈ 2 %** (`allocThunkSuspended` self-time). Banked
  only by **Phase F** (Reading B); optional.
- **C1c scavenge-copy: ≈ 1.2–7 % on M5** (`tryMark`/`fwdThunk`/`visitValue`).
  Partially banked at **Phase A** (tenured side) + fully at **Phase D** (nursery
  removed).

**Ceiling:** ~**18–24 % warm CPU on firefox** (C1 estimate: barrier + alloc +
scavenge), extrapolated ≥ that on M5 (more thunk-heavy → higher inter-gen write
density). This NARROWS the single-eval gap; it does **not** cross 1× TW (Reason B:
the 8B-NaN codec per-op tax remains; only a JIT touches that). Frame it honestly:
this is *the largest unclaimed single-eval CPU lever*, worth banking, but the
strategic win remains the repeated-eval moat (`BEAT_TW` TL;DR).

### 7.2 Pre-committed KILL gates (per phase; Rule 0)

- **Spike:** byte-id FAIL → STOP + RCA. CPU regress > 8 % → STOP (allocator
  pessimizes). Else proceed.
- **Phase A:** tenured scavenge-copy self-time must drop; ≥ 3 % CPU on M5 or
  firefox → good; < 1.5 % → investigate (not a plan-KILL — A isn't the 16 %).
- **Phase C (the decision gate):** **≥ 15 % warm CPU on firefox AND ≥ 12 % on git
  → SHIP; 8–15 % → marginal, re-review; < 8 % → KILL, revert to gated, write
  `NONMOVING_INLINE_THUNK_FALSIFIED_YYYY-MM-DD.md`.** The C1 A/B already measured
  16.4 %/19.0 % so this is expected to PASS — but the C1 numbers were a
  barrier-noop *approximation* (no-scavenge config); Phase C is the real,
  sound, byte-id number and it is the one that decides SHIP.
- **Phase F:** ≥ 2 % beyond C → keep; else document-close.

### 7.3 GC-spiral discipline (why this is not falsification #8)

Six+one GC variants were KILLED (`GC_PAUSE §2.1`, `IMMIX_FALSIFIED`) — **all on
RSS**. This lever's justification is **CPU** and is DISTINCT: the RSS KILL
(reclaim-to-OS = 0 MB at peak, P2) does not touch the CPU case (barriers + alloc +
scavenge cost regardless of whether pages return to the OS). C1 is a *measured*
GO (16.4 %/19.0 %), not a projection. But the same discipline binds: **each phase
has a pre-committed gate; the SHIP decision is Phase C's byte-id number, not the
approximation; a miss below 8 % writes a falsification doc and reverts to gated —
it does not "keep both behind a flag."** The substrate stays gated (like Steps
11′-13′ today) until Phase C clears its gate.

---

## 8. Cross-references

- `BEAT_TW_LEVERS_AND_ARCHITECTURE_2026-07-06.md` §N1 + §C1 (the GO verdict, the
  16.4/19.0 % measurement, the 3-component decomposition)
- `IMMIX_NOFL_DESIGN_2026-06-02.md` (the committed non-moving family: Nofl precise
  mark-region, metadata-byte-per-granule, sticky-bit generational, evacuation)
- `GC_DECISION_2026-05-29.md` (PIVOT-IMMIX; what survives §6) +
  `GC_PAUSE_2026-05-29.md` + `IMMIX_FALSIFIED_2026-05-29.md` (RSS KILLs — different
  justification)
- `STAGE_6_PRECISE_GC_DESIGN_2026-05-27.md` + `STAGE_6_IMPLEMENTATION_GUIDE_2026-05-27.md`
  (Option A/B/C, MoveGCVisitor, Day-4 skip-list, safepoint model, per-workload gates)
- Code: `barrier.hh` (Phase-D + noop apparatus), `value.hh` / `closure.hh`
  (8B Value, 24B Thunk), `nursery.hh` + `gc.cc` (Cheney young gen), `alloc.hh`
  (Arena, Immix Steps 11′-13′, `nurseryOrArena` routing, `freeWholeBlock`),
  `precise_root.hh` / `precise_root.cc` (`walkAllV3Roots`/`RootVisitor`),
  `mark_sweep.cc` (`runMajorMarkSweep`, `BitmapMarker`, `walkCStackConservative`),
  `vm.cc:4685-4780` (gen-major safepoint), `vm.cc` force/OP_RETURN writeback
- CLAUDE.md Rule 0 + Critical Constraint 0 (nursery + barriers default-on) + the
  pre-merge `--brute` gate + darwin-4 perf discipline

*Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group. SPDX-License-Identifier: Apache-2.0.*

---

## PHASE-S VERDICT (2026-07-06, darwin-4) — GATE FIRED → LEVER KILLED

Phase S built + committed (e6f8ee33c), byte-id PASS (ON==OFF), --brute 36/36. But
the pre-committed Phase-S CPU gate ("KILL if regresses >8%") **FIRED**: warm CPU
median-5 flag-ON vs OFF = **firefox +5.4%, M5 +10.4%** (M5 > 8%).

**DECISIVE ARITHMETIC — this KILLs the whole lever, not just Phase S:** the C1 A/B
(16.4%/19.0%) measured the barrier-CHECK cost WITH the moving GC present. Dropping
the barriers (Phase C) REQUIRES the non-moving GC, whose reclaim cost Phase S
measures as +5.4%/+10.4%. Net = barrier-win − reclaim-cost ≈ 11% ff / 5-9% M5.
Even if Phase A recovered the ENTIRE moving-scavenge cost (~1.2-7% per prior
profiles), the net stays ~7-13% — BELOW the Phase-C SHIP gate (≥15% ff / ≥12% git).
Phase A cannot lift it above 15%. The lever's net is robustly below its own SHIP
gate regardless of the remaining phases → **KILL** (Rule 0: honor the gate; KILL is
a deliverable).

This REFINES C1 GO: the 16% barrier tax is real, but NOT an achievable net win —
the enabling mechanism (non-moving tenured GC) costs 5-10% that offsets it. Phases
A-F NOT built. Phase-S code stays gated/default-off/byte-id-neutral as the KILL
reference. Single-eval CPU parity remains out of reach; the moat is the applied-
import cache #1 (repeated-eval), consistent with the whole program's conclusion.
