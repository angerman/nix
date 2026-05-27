# Memory & GC Strategy in Nix v3 — A Critical Review

> **ARCHIVED-IN-PLACE 2026-05-27**: This doc is historical (pre-strategic-doc-set or one-off RCA). Preserved here because it is still referenced from one or more KEEP docs. See [`LODE_CLEANUP_REVIEW_2026-05-27.md`](LODE_CLEANUP_REVIEW_2026-05-27.md) for the archival rationale.

---


> **Status update (2026-05-07):** Phase 0 (CRIT-2/3/4 arena
> correctness) closed via #432.  Phase 1+ (runtime GC redesign)
> recommendations in this doc still useful as research, not yet
> scoped.

*2026-05-04. Synthesises a code audit of `libexpr-v3/` against modern GC literature
(GHC RTS, OCaml, V8 Orinoco, Immix/LXR/Whippet, MMTk).*

## 1. What v3 actually does today

The v3 evaluator at `src/libexpr-v3/` uses a deliberately minimal scheme that the
source comments describe as bring-up scaffolding, not the intended end state.

**Allocator** (`include/v3/alloc.hh:180–262`): a per-thread bump-pointer arena.
16 MB blocks; 16-byte alignment; allocations >4 MB fall back to `std::calloc`.
Every block is registered with Boehm via `GC_add_roots(blk, blk + kBlockSize)`
(alloc.hh:259). All Values, Thunks, Closures, Envs, Bindings, and ListVecs flow
through `Alloc::alloc*` (alloc.hh:274–374).

**Reclamation: none.** The arena is allocate-and-leak. Objects are never freed
during evaluation; blocks are freed only when the thread exits. The header
comment is candid: *"the arena/refcount story is the architectural Phase A item
— not yet implemented"* (alloc.hh:3–8).

**Object layout**: tagged 16-byte `Value` (5-bit tag in low byte, 8-byte payload
union; value.hh:63–169). FAM tails on Closure/Thunk/Env/Bindings/ListVec keep
small heap objects in a single allocation. Lazy thunks transition Suspended →
Blackhole → Evaluated, and the `evaluated` field caches the final Value
(closure.hh:98–140; vm.cc:1616–1619). A Phase-4 `Tag::Slot` indirects through a
`Value*` into a Bindings entry to support GHC-style update-in-place semantics
for recursive attribute sets (value.hh, vm.cc:384–410).

**Boehm interaction**: v3 still lives inside a process that links Boehm GC. The
arena's blocks are registered as conservative root regions, which means **Boehm
scans the entire arena and treats every byte ever bumped as a live root**.
Boehm's job here is solely to keep tree-walker `nix::Value` objects alive that
v3 references through bridge thunks; it does *not* reclaim v3 objects.

**Known holes** (from `REVIEW_2026-05-03.md` §3): three blocks of `Value`-bearing
storage live entirely outside the arena and outside Boehm's view:

- `vm.valueStack` and `vm.withStack` use plain `std::allocator`
  (vm.hh:72,75) — **CRIT-2**.
- `ValuePair` allocations call `std::malloc` directly (vm.cc:1140, 3603;
  primops.cc:602, 675) — **CRIT-3**.
- Bridge primop string buffers via `std::malloc` — **CRIT-4**.
- `static std::vector<Value>` bridge tables (`v3BridgeAttrs`, etc.,
  primops.cc:2226+) grow unboundedly across a process lifetime.

## 2. What this scheme actually costs

It's tempting to read "bump allocator + Boehm" as a sensible interim choice.
Critically reviewing the consequences:

**(a) RSS grows monotonically with evaluation work.** A
`nixpkgs.hello.outPath` allocates several hundred MB to >1 GB of intermediate
thunks; a `nixos-rebuild` evaluation more. None of it is recoverable. This is
currently a non-issue for one-shot CLI invocations, but it makes v3 unusable as
a long-running evaluator daemon — the precise role v3 was meant to fill in
build farms and language servers.

**(b) The Boehm interop is largely ornamental.** Registering every arena block
as a root tells Boehm "everything in here is live." The conservative scan does
no real reclamation of v3 storage; it only keeps tree-walker Values pinned
through bridge references. The cost is non-trivial: Boehm's mark phase walks
every byte of registered roots on each collection, and v3's arena is a
substantial fraction of process memory. We are paying mark-phase overhead for
storage that is simultaneously declared 100% live.

**(c) The CRIT-2/3/4 holes are correctness liabilities, not just cleanups.**
`valueStack`/`withStack` content is reachable today only because the
`std::vector` data pointer happens to live somewhere a conservative scan finds
— typically through a Boehm-tracked allocator chain or a C-stack frame holding
a reference. Under heap pressure or any reorganisation that drops a transient
pointer, a Value held only by `valueStack` could be reclaimed. This is the kind
of bug that's quiescent for months and surfaces as a sporadic segfault under
load. It's not a v3 problem so much as evidence that the current "we plug into
Boehm" story is brittle.

**(d) Indirection chains never collapse.** v3 implements thunk update via
in-place mutation of the Thunk's `evaluated` field (vm.cc:1616–1619), and
`Tag::Slot` adds a second level of pointer-following. Memoisation in
`forceValue` (vm.cc:384) flattens slots eagerly, but Tag::App / Tag::PrimOpApp
chains and bridge thunks accumulate. GHC and OCaml compress these during GC
copy; v3 has no GC, so chains live forever. That's spatially cheap (a Value is
16 bytes) but cache-hostile on long evaluations.

**(e) Per-thread allocation, per-fiber semantics.** The arena is
`thread_local` (alloc.hh:264–268). Cooperative fibers on the same thread share
an arena, so a long-running fiber pollutes the arena for its peers. This is
fine today; it constrains any future scheme that wants per-fiber lifetimes.

**(f) Stated direction is incomplete.** `OPTIMIZATION_PLAN.md` and
`REVIEW_2026-05-03.md` are explicit that the GC story is the largest
unaddressed architectural item. `BENCH-REAL-WORLD-2026-05-04.md` concludes
that the dispatch loop is no longer the bottleneck — meaning further v3 wins
increasingly *depend* on solving the memory side.

## 3. Evaluating the alternatives

The literature ranking (precise GenImmix > hybrid Cheney+Boehm > MMTk >
everything else) is largely correct, but changes once you weight it by v3's
specific constraints. The generic survey under-weights three things:

1. **v3 has no precise stack maps.** Bytecode handlers are written in C++, and
   during a primop call live Values sit in C++ locals. Any *moving* collector
   needs to find and update those. (Quantified in §7 below: ~25 of 85 primops
   hold heap references across allocation sites, but the hazard is bounded and
   mechanically fixable.)
2. **Boehm is not just legacy — it owns the rest of the process.** The store
   layer, parser, settings, and tree-walker all use Boehm. v3 cannot
   unilaterally adopt a new heap without negotiating the boundary.
3. **v3 already concentrates mutator state.** Unlike GHC native code, v3's
   live values mostly sit in five known data structures: `valueStack`,
   `withStack`, `frames`, `capturedWiths`, and a small set of bridge tables.
   This is a *huge* engineering advantage that a survey based on
   GHC/V8/Ruby experience underweights — precise root enumeration is mostly a
   matter of fixing the CRIT-2/3/4 holes and walking known vectors.

With that re-weighting:

| Strategy | Verdict for v3 |
|---|---|
| Pure Boehm (status quo) | Loses on every dimension as the v3 heap grows. Not a long-term answer. |
| Pure GenImmix | Right destination, wrong starting move. Needs precise stack maps for primop bodies; ~6–12 person-months realistically. |
| **Cheney nursery + Boehm tenured** | More attractive than the literature suggests because of v3's concentrated mutator state. Needs precise roots into the nursery (Boehm's conservative scan cannot update pointers, hence cannot promote). |
| Reference counting / regions | Disqualified. Nix's allocation/death ratio is in the regime where RC overhead dominates; thunk capture defeats lifetime inference. |
| MMTk | The right tool if the goal is a *paper*. For shipping, the binding cost (months) and Rust toolchain dependency aren't justified by the differential vs a hand-rolled Cheney nursery on a workload with this much structural symmetry. |
| **Whippet (Andy Wingo)** | Worth a serious second look. Explicitly designed as a Boehm replacement; embeddable as a C library; supports both conservative and precise modes. If it can run in precise mode for v3 while still scanning the rest of the process conservatively, it is the closest off-the-shelf match for the hybrid story. |

## 4. Recommendation: a phased plan

### Phase 0 — Stop the bleeding (1–2 weeks)

Fix CRIT-2/3/4 unconditionally, regardless of which collector wins long-term.

- Route every `Value`-bearing container through `traceable_allocator<Value>`
  or `Alloc::*`.
- Replace `std::malloc` for `ValuePair`/string buffers with arena allocation.
- Bound the bridge tables (weak-keyed or `~EvalState` cleanup, REVIEW MED-14).

These are correctness fixes the codebase already has tickets for; they are
also a precondition for any precise GC because they put every live Value in a
known scannable location.

### Phase 1 — Concentrate roots, prepare for moving GC (3–4 weeks)

- Add an explicit `RootSet` interface that enumerates every Value reference
  held by v3 outside the arena: the three vectors, frame fields, bridge
  tables, and the small set of primop-body locals.
- Build a `Rooted<Value>` / `RootedVector<Value>` RAII pair (SpiderMonkey/V8
  pattern) that registers itself with the root set on construction and
  unregisters on destruction. ~1 day to design and unit-test.
- Mechanically replace the 13 known invisible-root sites in primops.cc
  (`primFilter`, `primConcatMap`, `primPartition`, `primZipAttrsWith`,
  `primListToAttrs`, `primCatAttrs`, `primGroupBy`, `primGenericClosure`,
  `primSplitString`, `primSplit`, `primReadDir`, `primFromJSON` object branch,
  `primAttrValues`) — `std::vector<Value>` → `RootedVector<Value>`. ~1 day.
- Refactor the ~10 interior-pointer sites (`primMap`, `primGenList`,
  `primMapAttrs`, `primRemoveAttrs`, `primIntersectAttrs`, `primNixPath`,
  `primAttrNames`, `primFindFile`, `primAppendContext`, `primToPath`) to
  re-fetch `Value::payload.bindings` / `payload.list` after each allocation,
  or hold the parent `Value` as a `Rooted<Value>`. ~3–5 days.
- Stress-test under a probe-collector that moves every object on every
  allocation. This is the single most valuable testing tool in the entire
  plan and should be the first thing built. ~1 week.

### Phase 2 — Cheney-style nursery (4–6 weeks)

Replace the front of the arena with a copying nursery (a single 4–8 MB region,
sized empirically).

- Allocation remains a bump.
- On nursery exhaustion, copy survivors into the existing arena (now playing
  the role of tenured space, still backed by Boehm root regions).
- **Nursery-bypass threshold**: primop-allocated `Bindings` / `ListVec`
  larger than a small size threshold (say 256 bytes) skip the nursery and
  go straight to tenured. This is the V8/JVM "large object space" pattern.
  *Important*: this cuts the primop hazard set roughly in half, because
  most interior-pointer hazards in §7 point into containers that
  bypass the nursery and therefore never move. The Phase 1 `Rooted<Value>`
  refactor is still needed for thunks and small Values, but the bar is lower.
- **Write barrier**: the only mutating site is thunk update at `OP_RETURN`
  (vm.cc:1616–1619); record any tenured-thunk → nursery-Value pointer in a
  remembered set.
- **Indirection short-circuiting**: while copying a `Tag::Thunk` whose state
  is `Evaluated`, copy the cached value directly instead. This single
  optimisation collapses the chains that today grow without bound.

This intermediate state — nursery + Boehm tenured — captures the bulk of the
win. Allocation throughput stays at single-instruction bump; intermediate
thunks are reclaimed without ever appearing in Boehm's mark phase; tenured
objects continue to interoperate with the rest of the process via the existing
arena-as-root-region machinery.

### Phase 3 — Replace Boehm tenured with Immix or Whippet's nofl (later, optional)

Only if Phase 2 measurements show Boehm's conservative mark of the tenured
region dominating GC time. At nixpkgs scale this is plausible but not certain;
**measure first**. The interface from Phase 1 (precise roots) makes this a
drop-in once committed.

## 5. Risks and what could disqualify the plan

**Risk: the C++-locals-during-primop hazard is bigger than expected.**
*Quantified*. An audit of primops.cc (§7) found ~25 of 85 primops hold heap
references across allocation sites. The hazard is real but bounded: 13 sites
follow the canonical `std::vector<Value>` invisible-root pattern (mechanical
fix via `RootedVector<Value>`), and ~10 hold interior pointers that need
re-fetching after each allocation. Total effort: 1–2 weeks of mechanical
edits behind a 1-week tooling investment, well within Phase 1's revised
budget. The fallback (Sticky-Immix, no movement) remains available if the
probe-collector finds something the audit missed, but is no longer the
expected outcome.

**Risk: Boehm mark-phase cost on the tenured arena doesn't come down enough.**
The premise is that an evaluation's working set is small relative to
allocations. In nixpkgs that is *probably* true (most thunks die), but until
measured we don't know whether Phase 2 alone delivers a perceptible RSS / time
win. A 1-day prototype that simulates Cheney's "only survivors are kept" by
post-eval drop is worth doing before committing.

**Risk: bridge-thunk lifetime gets harder, not easier.** Tree-walker Values
referenced by v3 must remain alive; today the arena-as-root-region story
handles this by accident (every arena byte is a root). After Phase 2, only
*copied* survivors remain — the bridge thunks' tree-walker payloads must be
explicitly registered with Boehm or themselves treated as roots.

**Risk: serialised bytecode caches encode object layouts that change.** Phase
1's stricter root invariants and any new tags require a `serialize.cc` schema
bump (cf. CRIT-1, the existing OP_REC_BINDING_SLOT_REF remap bug). This is a
known ongoing cost; budget for it.

## 6. The bottom line

v3's current GC is *not really a GC* — it is a bump arena that informs Boehm
"treat me as live." That is fine as bring-up but is the largest outstanding
architectural debt in the v3 design and is now the most likely ceiling on
perceived performance, given that the dispatch loop is no longer the
bottleneck.

The right destination is the GHC/OCaml/V8 shape — precise generational
copying nursery with indirection short-circuiting, optionally Immix in the old
generation. The right *first move* is not to build that destination but to
(a) close the GC blind spots that already exist, (b) concentrate roots so a
moving GC becomes possible, and (c) ship a Cheney-style nursery against
Boehm-managed tenured space. That captures the dominant win at modest
engineering cost and leaves the eventual Immix/Whippet upgrade as a
measurable, isolated follow-up rather than a leap.

The strongest single insight from the literature bears repeating: **for a
lazy bytecode VM, the question is not whether to be generational and copying
— it is whether the old generation is Cheney, Immix, or temporarily-Boehm.**
v3 should treat "temporarily-Boehm" as the answer for the next twelve months
and the others as targets to measure into.

## 7. Appendix: primops.cc moving-GC hazard audit

A line-by-line audit of `primops.cc` (5998 lines, 85 registered primops)
to quantify the cost of switching to a moving collector.

**Headline numbers**:

- 85 distinct primop functions registered in `registerBuiltinPrimOps()`
  (primops.cc:5767–5996).
- ~45 primops are *safe*: they read args off the value stack, compute, write
  one result. No Value held across an allocation. (Type checks, basic arith,
  `head`/`elemAt`, `getAttr`/`hasAttr`, `stringLength`, `seq`, `floor`/`ceil`,
  `parseInt`, `pathExists`, `readFile`, `hashString`, etc.)
- ~25 primops are *hazardous* under a moving GC. Of these, ~20 are HIGH
  severity (would dangle interior pointers or hold invisible-root vectors)
  and ~5 are MED (tight windows or singleton returns).
- 7 bridge call sites (`v3ToTreeWalker` / `treeWalkerToV3` / cross-VM
  forwarders): all copy Values rather than hold them across the bridge —
  no v3-side hazard.

**Pattern A — invisible-root loop accumulators (13 sites).** The canonical
`std::vector<Value>` populated across allocation calls. Mechanical fix:
replace with a GC-aware `RootedVector<Value>`.

| Primop | File:line | Vector(s) | Allocations in loop |
|---|---|---|---|
| `primFilter` | primops.cc:630–655 | `kept` | `callClosure`, `forceValue` |
| `primConcatMap` | primops.cc:808–836 | `all` | `callClosure`, `forceValue` |
| `primPartition` | primops.cc:838–883 | `right_`, `wrong_` | `callClosure`, `forceValue` |
| `primZipAttrsWith` | primops.cc:1605–1677 | `entries`, `vs` | `allocList`, `allocPair` |
| `primListToAttrs` | primops.cc:901–952 | `entries`, `dedup` | `forceValue`, VM work |
| `primCatAttrs` | primops.cc:1076–1096 | `kept` | `forceValue` |
| `primGroupBy` | primops.cc:2149–2183 | `groups`, `entries` | `callClosure`, `allocList` |
| `primGenericClosure` | primops.cc:1805–1878 | `result`, `work` | `callClosure` |
| `primSplitString` | primops.cc:1771–1801 | `parts` | `mkStringValueOwned` |
| `primSplit` | primops.cc:1914–1956 | `parts`, capture lists | `mkStringValueOwned`, `allocList` |
| `primReadDir` | primops.cc:2082–2112 | `entries` | `mkStringValueOwned` |
| `primFromJSON` (object branch) | primops.cc:5053–5111 | `entries` | `jsonToValue` (recursive) |
| `primAttrValues` | primops.cc:328–349 | `pairs` | `allocList` (post-loop) |

**Pattern B — interior pointers held across allocation (~10 sites).** A C++
local of the form `auto * src = value.payload.bindings` (or `.list`)
dereferenced after a subsequent allocation. Fix: re-fetch after each
allocation, or hold the parent `Value` as a `Rooted<Value>`.

| Primop | File:line | Hazard |
|---|---|---|
| `primMap` | primops.cc:591–628 | `src = lst.payload.list` across `allocPair` per iter |
| `primGenList` | primops.cc:674–701 | same shape as primMap |
| `primMapAttrs` | primops.cc:1011–1039 | `src = bindings` across `allocPair × 2` per iter |
| `primRemoveAttrs` | primops.cc:954–984 | `src = bindings` across `allocBindings` |
| `primIntersectAttrs` | primops.cc:986–1009 | two `bindings` interior pointers across `allocBindings` |
| `primAttrNames` | primops.cc:304–326 | `bindings->size`, then `mkStringValueOwned` × n |
| `primNixPath` | primops.cc:1522–1556 | `lv` interior pointer across per-iter `allocBindings` |
| `primFindFile` | primops.cc:1561–1600 | `el.payload.bindings` across `forceValue` in loop |
| `primAppendContext` | primops.cc:1408–1463 | `ctxB->entries[]` across `forceValue` |
| `primToPath` | primops.cc:1246–1275 | `s.payload.str` across `callClosure` (short window) |

**Pattern C — bridge call sites (7 sites, all SAFE).** Listed for
completeness; none hold v3 Values across the cross-VM call:

`primV3CallBridge1` (primops.cc:2302–2432), `primV3ForceAttr`
(primops.cc:2448–2530), `primStorePath` (primops.cc:5617–5647),
`primToFile` (primops.cc:5651–5684), and the fetch primops at
primops.cc:5723–5760 all use a copy-out pattern: tree-walker Values are
synthesised, converted via `treeWalkerToV3*`, and the result is a stack-local
`Value` that does not need to survive a subsequent allocation.

**Effort estimate (revised)**:

- Build `Rooted<Value>` + `RootedVector<Value>` + probe-collector: ~1 week.
- Apply Pattern A fixes (13 sites, ~30 lines each): ~1 day.
- Apply Pattern B fixes (10 sites, ~20 lines each): ~3–5 days.
- Stress-test under probe-collector across nixpkgs eval: ~1 week.
- **Total: 3–4 weeks of focused work.** This is the Phase 1 budget.

**One observation that materially shifts the risk profile**: every hazard
above is *latent*. v3 has no GC today, so none of these sites are bugs at
present — they only become bugs when a moving GC is wired up. This means the
audit, the `Rooted*` API, and all Pattern A/B fixes can be landed
independently, in any order, with zero behavioural change in the current
no-GC build. The probe-collector verifies each fix in isolation. The risk
profile is "incremental refactor with mechanical verification," not
"big-bang redesign of the primop API."

## References

GC literature consulted for this review:

- Marlow, Harris, James, Peyton Jones. "Parallel Generational-Copying GC with
  a Block-Structured Heap." ISMM 2008.
  <https://simonmar.github.io/bib/papers/parallel-gc.pdf>
- Marlow, Peyton Jones. "Exploring the Barrier to Entry: Incremental
  Generational GC for Haskell."
  <https://simonmar.github.io/bib/papers/ExploringBarrierToEntry.pdf>
- Blackburn, McKinley. "Immix: A Mark-Region Garbage Collector with Space
  Efficiency, Fast Collection, and Mutator Performance." PLDI 2008.
  <https://www.steveblackburn.org/pubs/papers/immix-pldi-2008.pdf>
- Zhao, Blackburn. "Low-Latency, High-Throughput Garbage Collection (LXR)."
  PLDI 2022. <https://www.steveblackburn.org/pubs/papers/lxr-pldi-2022.pdf>
- Well-Typed. "A Concurrent Garbage Collector for GHC."
  <https://well-typed.com/blog/aux/files/nonmoving-gc/design.pdf>
- Wingo, Andy. Whippet GC. <https://github.com/wingo/whippet>;
  <https://wingolog.org/archives/2025/05/09/a-whippet-waypoint>
- MMTk. <https://www.mmtk.io/>;
  Ruby 3.4 integration:
  <https://railsatscale.com/2025-01-08-new-for-ruby-3-4-modular-garbage-collectors-and-mmtk/>
- V8 Orinoco posts. <https://v8.dev/blog/trash-talk>;
  <https://v8.dev/blog/orinoco-parallel-scavenger>
- osa1. "Three runtime optimizations done by GHC's GC."
  <https://osa1.net/posts/2018-03-16-gc-optimizations.html>
- Real World OCaml — GC chapter.
  <https://dev.realworldocaml.org/garbage-collector.html>
- Tofte, Talpin. "Region-Based Memory Management." Information & Computation 1997.
  <https://web.cs.ucla.edu/~palsberg/tba/papers/tofte-talpin-iandc97.pdf>
- Boehm-Demers-Weiser GC. <https://www.hboehm.info/gc/>
