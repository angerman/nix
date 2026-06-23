# The architecture program to beat the tree-walker — 2026-06-23

Successor to the BEAT_TW campaign (lode/BEAT_TW_PLAN_2026-06-23.md), which proved
by measurement that the gap is STRUCTURAL: every single cheap lever was a KILL
(thunk-avoidance #135, ImportCache eviction #134, arena page-release #136) or
modest/foundational (#139), and the production gap is CPU 1.83–2.49× / RSS
1.64–2.26× (warm, darwin-4, #132).  This doc specifies the fundamental
architectural changes that close it, with planned steps + pre-committed gates.

## Thesis — cash in the compilation bet

A bytecode VM's bet is: pay an analysis cost ONCE at compile time, reap a runtime
saving on EVERY eval.  v3 PAYS the cost (parse+lower = 32–40% of cold CPU) but has
not fully REAPED the two savings that justify it — (1) strictness → allocate fewer
thunks, (2) native code → no dispatch — and its runtime representation is heavier
than TW's, not lighter.  TW is lean because it (a) barely allocates (on-stack
demand-driven descent + maybeThunk skips 1.65M thunks on firefox), (b) uses tight
shared values (16 B niche-tagged Value, 16 B `{Env*,Expr*}` thunk, whole-Env
capture, copy-free `//`), and (c) runs ONE runtime (no arena + Boehm + FFI bridge).

The through-line of every fix below: v3 has STARTED all the right moves
(`opt_strictness`/`opt_func_strictness`/`opt_strict_call_unthunk`, `ChainBindings`,
`upvalEnv` env-sharing) but each is blocked exactly where nixpkgs is hardest —
**dynamic dispatch** (hides callees → defeats static strictness) and **deep
override chains** (defeats flat/copied attrsets).  Beating TW = make v3 allocate
LESS than TW (strictness + sharing) and run the residue NATIVE (JIT).

Measured cost map (firefox warm unless noted):
- thunk churn: 65.7% of 2.88M thunks never forced → ALLOC ~20% CPU + arena RSS.
- attrset merge: `countDistinct` 16–17% CPU; Bindings 142 MB (largest arena bucket).
- dispatch: 36.86M opcodes, ~52% trivial stack ops; DISPATCH 7–23% CPU.
- cells: thunk 24 B vs TW 16 B; pair 32 B; arena never munmaps.
- second heap: Boehm 402 MB (M5, 14%) = transient TW values at the FFI boundary.

---

## Change 1 — Strictness that survives dynamic dispatch  (biggest shared lever)

**Cause:** 65.7% of thunks allocated-but-never-forced; each costs allocation
(ALLOC ~20%) + 24 B arena (never reclaimed) + a later force-dispatch.

**Exists:** per-function `strictArgs` inference + call-site `MkThunk` skip — but
only when the callee is STATICALLY KNOWN.  nixpkgs is higher-order (callPackage,
mapAttrs, overlays) → the analysis can't fire where the thunks are.

**The change:**
1. Lowerer: strict-by-default emission for known-strict positions (force-in-place,
   no `MkThunk`, for if-conditions / BinOp operands / select roots) — match TW's
   on-stack descent; reserve thunks for genuinely-stored lazy values.
2. Reach dynamic callees via **speculative strictness + deopt** (tracing-JIT style):
   at a hot call site, speculate the observed callee's strictArgs, eval the arg
   eagerly, guard + bail to lazy if a different callee appears.  (Alt/auxiliary:
   0-CFA-lite to narrow targets for the existing static pass.)

**Helps:** removes a slice of the 2.88M thunks → fewer OP_MAKE_THUNK (7.8% ops) +
fewer OP_FORCE + fewer arena bytes.  Because the arena never frees, NOT allocating
beats reclaiming — lowers peak RSS with zero GC cost.  The only lever that moves
CPU and RSS together.

**Risk:** byte-identity — eager eval changes WHEN errors/IFD-builds fire; analysis
must be provably-always-demanded or guarded by deopt.  (The "L3-hard" item.)

**SHIP gate:** byte-id + --brute 22/22; warm CPU and/or RSS improvement above the
darwin-4 noise floor on firefox AND M5; deopt guards proven correct under --brute.

---

## Change 2 — Persistent HAMT attrsets  (nixpkgs CPU+RSS multiplier)

**Cause:** Bindings = largest arena bucket (142 MB firefox); `countDistinct`
16–17% CPU.  nixpkgs is override/overlay/`rec`-heavy.

**Exists:** `ChainBindings` avoids COPY on `//` (stores parent + overlay delta)
but trades it for a CHAIN WALK — `countDistinct` walks the parent chain; deep
nixpkgs chains make it long.  Gated `NIX_V3_CHAIN_BINDINGS`, never default (missed
its ≥200 MB bar).

**The change:** replace flat-array + chain with a **persistent HAMT** — `//` is
O(changed keys) with structural sharing of unchanged subtrees; lookup O(log n);
merge no longer walks a linear chain.

**Helps:** CPU — collapses the `countDistinct` chain-walk (16–17%).  RSS — deep
override layers SHARE their common base instead of copying/chaining (the biggest
attrset-RSS win on nixpkgs, where one base is overlaid dozens of times).

**Risk:** Nix attrsets must iterate in SORTED key order for byte-id (attrNames,
drv hashing).  HAMT with deterministic sorted iteration is intricate; attr-select
becomes a trie walk (tune vs binary search).  Subsystem rewrite (PERF_STRATEGY
Stage 11).

**SHIP gate:** byte-id incl. sorted-iteration (full nixpkgs soak, diverge=0) +
--brute 22/22; `countDistinct` CPU down + Bindings RSS down on darwin-4; attr-select
hot path not regressed.

---

## Change 3 — Environment interning + 16 B uniform cells

**Cause:** v3 thunk 24 B vs TW 16 B; closures copy free vars inline.  Millions of
cells → real arena RSS; TW's whole-Env-by-one-pointer capture is cheaper.

**Exists:** `upvalEnv` env-sharing shipped default-on — but bring-up still allocs
one Env per closure/thunk AND keeps the inline upvalue FAM, so it is currently MORE
memory, not less.  The real win (interning + drop FAM) is unfinished.

**The change:** intern environments (share one Env per lexical scope across the
closures/thunks created in it) and drop the inline FAM so a thunk is `{desc,
sharedEnv}` = 16 B.  (Bundle the cheap cell-shrinks here: LambdaDescriptor
diagnostic repack — move name/contextualName/3 counters to a gated side-table,
#139 — and pair-tax via 8 B-granular allocator if it pays.)

**Helps:** closes 24 B→16 B thunk gap + eliminates upvalue copying → arena RSS
down (thunks 117 MB firefox) + copy/alloc CPU down.  Converges on TW's lean
shared-frame model.

**Risk:** fast "same-scope?" intern key + GC for multiply-referenced shared Envs
(env-sharing already proved the GC machinery).  Medium.

**SHIP gate:** byte-id + --brute 22/22; thunk/closure arena bytes down on darwin-4;
no force-path CPU regression.

---

## Change 4 — Method JIT with GC safepoints  (CPU dispatch lever)

**Cause:** 36.86M opcodes, ~52% trivial stack ops, each fetch-decode-dispatch;
DISPATCH 7–23% CPU.

**Exists:** J0 platform / J1 encoder / J2 byte-id codegen / J3 safepoint CONTRACT
proven standalone.  Missing: J3 real-scavenger integration + value-stack ABI
trampoline + hot-callCount compile trigger + deopt/bail.

**The change:** compile hot LambdaDescriptors to native code — removes dispatch +
lets a register allocator elide trivial stack traffic.  Crux: native code spills
live v3 pointers at allocation safepoints so the moving GC finds/relocates them.

**Helps:** eliminates DISPATCH (7–23%) + push/pop traffic for hot bodies.

**Ceiling (honest, #137):** JIT ALONE → ~1.5–2.2× (narrows, does NOT beat TW); it
doesn't remove allocations or real work.  Necessary, not sufficient — only wins
paired with Change 1 (fewer allocs to wrap) + Change 3 (cheaper cells).  Therefore
sequenced AFTER 1+3 so it compiles the reduced instruction stream.

**Risk:** safepoint correctness (a missed spill = silent UAF under moving GC);
must cover allocating bodies (pure-arith JIT = gaming).  Multi-week.

**SHIP gate:** byte-id + --brute 22/22 under 1 MB-nursery moving-GC stress; warm
CPU improvement on darwin-4 measured on the POST-Change-1 stream.

---

## Change 5 — Shrink the FFI/Boehm bridge  (second-heap RSS)

**Cause:** Boehm 402 MB (M5, 14%) = transient TW values materialized at the FFI
boundary, dead but un-returned (Boehm munmap falsified on macOS).  v3 keeps a
SECOND value graph in a SECOND heap.

**Exists:** FFI audit — ~109 primop wrappers + 104 TW-cross sites.  Each crossing =
a Boehm alloc + a marshal.

**The change:** push the highest-volume primops fully v3-native (no TW Value
materialised) and pass v3-native reps (e.g. store paths as v3 strings) across the
remaining boundary.

**Helps:** RSS — fewer transient TW values → Boehm never grows to 402 MB.  CPU —
removes per-call v3↔TW marshalling across GC ownership.

**Risk:** the legitimate FFI leaves are real system boundaries (store, drv hashing,
path canonicalisation) — large careful surface, must stay byte-id with TW store
semantics.

**SHIP gate:** byte-id + --brute 22/22; Boehm live+reserved down on darwin-4 M5.

---

## Change 6 — Compacting (Immix) GC that returns pages

**Cause:** arena never munmaps; #136 measured no whole-dead/sparse blocks at
current density → reclaim never lowers peak RSS.

**Exists:** Stage-6 Immix designed + PAUSED (GC_PAUSE_2026-05-29, below SHIP gate).

**The change:** Immix mark-region with EVACUATION — compact the live set into dense
blocks, munmap the emptied ones.

**Helps:** returns freed pages to the OS — the reclaim enabler for cells that must
be allocated then die mid-eval (the ones Change 1 can't avoid).

**Ceiling (honest, #136 + RSS decomp):** even perfect reclaim → firefox ~1.34× TW;
M5 LIVE arena alone (~1023 MB) ≈ TW's whole RSS.  Bounded — only matters AFTER
1–3 shrink the live set.  Therefore LAST, and re-gated on a fresh density measure
(does evacuation opportunity exist once the live set is smaller?).

**Risk:** evacuation correctness under the moving GC; the highest-risk GC change.

**SHIP gate:** re-measured post-1/3 density shows ≥ a pre-committed sparse-block
fraction; --brute 22/22 under stress; peak RSS down on darwin-4.

---

## Sequencing + dependency graph

```
        Phase 0  de-risk keystone (speculative strictness ceiling)  ── GATE ──┐
                                                                              │ go
   ┌──────────────────────────── FOUNDATION (allocate less) ──────────────────┤
   │  Change 1 strictness        Change 3 env-intern + 16B cells               │
   └───────────────┬────────────────────────┬─────────────────────────────────┘
                   │                         │
        Change 2 HAMT (nixpkgs multiplier; parallel-capable)
                   │                         │
        Change 4 JIT (after 1+3: reduced stream)
                   │
        Change 5 FFI/Boehm   ·   Change 6 Immix (after 1+3 shrink live set; re-gated)
                   │
        Integration: full nixpkgs soak + combined warm head-to-head vs TW
```

**Principle — de-risk before committing.** The whole program's payoff hinges on
Change 1's reach against dynamic dispatch (highest leverage, highest uncertainty).
Phase 0 prototypes speculative strictness on the M5/firefox hot call sites and
MEASURES the realizable thunk elimination BEFORE the JIT/HAMT rewrites.  If it
can't reach the dynamic sites, re-scope (lean on HAMT + JIT, accept match-not-beat)
— that is the Rule-0 falsifier for the program.

**Endpoint caveat.** Closing the WARM gap (1.8–2.5× CPU, 1.6–2.3× RSS) is the
target; whether the endpoint is "beats TW" or "matches TW" depends most on Change
1.  Every step keeps the discipline: RCA/measure-first → gated impl with retirement
criterion → byte-id + full --brute → darwin-4 measure → git-note + lode note.

*Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output
Group. SPDX-License-Identifier: Apache-2.0*
