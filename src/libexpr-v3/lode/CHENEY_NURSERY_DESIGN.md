# Cheney nursery design (#434)

Decided 2026-05-10.  Motivation in
`project_v3_runtime_roadmap` user memory and
`V3_DIRECT_RCA_PROGRESS_2026-05-09.md` Phase 2 findings: v3-direct
nixpkgs.hello.name allocates ~50M thunks at ~88 B each, in arena
blocks of 16 MB grain that don't release back to Boehm during eval,
producing 50+ GB residency and OOM at ~150s.  Most thunks are
short-lived (the diagnostic showed `alloc=5.7M force=7` patterns).
A young-generation copying GC reclaims them.

## Goals

1. **Reclaim short-lived thunks during eval.**  Cut working set
   from ~50 GB to a steady-state proportional to live data
   (~hundreds of MB for nixpkgs eval).
2. **Don't slow down the hot path.**  Allocation must stay
   bump-pointer; scavenge cost must amortize over many allocations.
3. **Don't break correctness.**  Every reachable thunk/closure/
   bindings/list must survive scavenge with all its references
   updated to the post-copy address.
4. **Don't break Boehm's role.**  Tenured allocations stay arena-
   backed (Boehm-rooted via `GC_add_roots`).  The nursery is a
   pre-tenured staging area.

## Non-goals (explicitly)

- Not aiming for GHC-level allocation throughput.  The interpreter
  has its own overheads; nursery alone won't make v3 faster than
  TW per-thunk.
- Not changing the `Thunk` / `Closure` / `Bindings` / `ListVec`
  layouts.  Tagged pointers (the next runtime piece) are a
  separate refactor.
- Not making allocation thread-safe.  v3 is single-threaded; the
  nursery is per-thread (matching existing `threadArena()`).

## Architecture

### Layout

A nursery is a fixed-size contiguous block.  Allocations bump a
pointer until exhausted; then we scavenge.

```
[ ──── nursery (e.g. 32 MB) ──── ][ free ────────── ]
^                                  ^
nurseryBase                        nurseryNext
                                   nurseryEnd = nurseryBase + size
```

Allocation:

```
void * nurseryAlloc(size_t bytes) {
    bytes = align16(bytes);
    if (nurseryNext + bytes > nurseryEnd) {
        scavenge();          // copy live to tenured; reset nursery
        if (nurseryNext + bytes > nurseryEnd)
            // oversized: fall through to tenured allocation
            return tenuredAlloc(bytes);
    }
    void * p = nurseryNext;
    nurseryNext += bytes;
    return p;
}
```

The nursery is in plain malloc'd memory (NOT arena, NOT Boehm-
rooted).  Boehm sees it via the conservative C-stack scan; that's
sufficient because *during* eval our roots (`vm.frames`,
`vm.valueStack`, `vm.withStack`, `partialBindingsRegistry`,
`thunkCreationMap`) all have C-stack reachability — they're held
by `VMState &` references.

After scavenge, the nursery is reused.  No `GC_add_roots` /
`GC_remove_roots` churn per scavenge.

### Sizing

Initial: 32 MB.  Tunable via `NIX_V3_NURSERY_SIZE` env var (MB).

Trigger scavenge when nursery would overflow.  Optionally: also
trigger every N opcodes for latency smoothing (deferred).

### Forwarding

Each scavengeable object needs a forward-pointer slot during
scavenge — somewhere we can write "I've been copied; here's my
new address."

Options considered:

a. **Repurpose first word.**  After copying, write the new address
   into the first word of the old object.  Tag the low bit as
   "forwarded."  Read-side: when scavenge encounters an object,
   check if its first word looks like a tagged forward pointer.
   Standard Cheney implementation but requires the first word to
   be unambiguous (not a normal value).

b. **Side-table.**  `unordered_map<oldPtr, newPtr>` populated
   during scavenge; consulted on each reference encounter.
   Discarded after scavenge.  More memory but simpler — no
   layout coupling.

c. **Per-object header bit.**  Reserve a bit in each
   scavengeable struct's header field that means "forwarded;
   payload now points to new location."  Minimal overhead but
   requires layout cooperation.

**Choice: (b) for the initial implementation.**  Side-table is the
cleanest non-invasive option and the per-scavenge overhead is
acceptable for the first version.  We can switch to (a)/(c) later
if profiling shows the side-table is the bottleneck.

### Scavengeable types

For each type we need:
- Allocator routes through nursery.
- Sizeof (for copy).
- A walk function that visits each `Value` / pointer it contains.

| Type       | Where allocated         | Size                        | References to walk                        |
|------------|-------------------------|-----------------------------|-------------------------------------------|
| `Thunk`    | `allocThunkSuspended`   | `sizeof(Thunk) + nUp*16`    | `tail[]` (Value), `cell` (Value*),        |
|            |                         |                             | union: suspended.{capturedWiths, cu},     |
|            |                         |                             | evaluated (Value), bridgeSrc (TW Value)   |
| `Closure`  | `allocClosure`          | `sizeof(Closure) + nUp*16`  | `upvalues[]` (Value), `capturedWiths`     |
| `Bindings` | `allocBindings`         | `sizeof(Bindings) + n*Entry`| `entries[i].value` (Value)                |
| `ListVec`  | `allocList`             | `sizeof(ListVec) + n*16`    | `elems[]` (Value)                         |

**Cells (`Value*`) for cell-update protocol:** allocated via
`Alloc::allocValue()`.  These are 16-byte heap cells holding a
single `Value`.  Their refs to nursery objects need updating.
Currently allocated in arena (tenured); under nursery design they
move to tenured directly (cells are referenced by long-lived Tag::Slots
captured in upvalues, so nursery-allocating them is wrong).

**TW values, primops, `LambdaDescriptor`, `CompilationUnit`,**
**static data:** never nursery-allocated.

### Walking

`walkValue(Value & v)`:
- `Tag::Closure`: visit `closure->upvalues[i]`, `closure->capturedWiths`.
- `Tag::Thunk`: depending on state, visit different fields.
- `Tag::Attrs`: visit `bindings->entries[i].value`.
- `Tag::List`: visit `list->elems[i]`.
- `Tag::Slot`: deref `slot` once; if the dereferenced Value
  references a nursery object, the SLOT POINTER (not the value
  it points to) gets forwarded — the cell stays where it was
  (cells are tenured), but its content might point at a nursery
  object that's been forwarded.  So forward `*slot`.
- `Tag::App`: visit `app->lhs`, `app->rhs`.
- Other tags: no references.

### Roots

The collection set: every Value/pointer outside the nursery that
might reach a nursery object.

| Root location          | What to walk                                         |
|------------------------|------------------------------------------------------|
| `vm.valueStack[]`      | Each Value                                           |
| `vm.withStack[]`       | Each Value                                           |
| `vm.frames[].closure`  | Closure pointer (already tenured, but its upvals?)   |
| `vm.frames[].thunk`    | Thunk pointer                                        |
| `partialBindingsRegistry` | each Thunk* key (already in stack), each Bindings* |
| `thunkCreationMap`     | each Thunk* key (diagnostic; only if enabled)        |
| Per-thread arena's tenured live set | cells holding Values pointing at nursery |

Closures and thunks themselves can live in nursery.  When a frame
is pushed, `frame.closure`/`frame.thunk` points at a nursery
object.  We need to forward the *frame's slot*.

For tenured objects pointing at nursery objects, we don't have a
remembered set yet — for the first cut, we walk ALL tenured
allocations during scavenge.  This is O(tenured size), which
defeats the nursery's perf goal for large heaps.

**Mitigation: write barrier.**  When a tenured object writes a
reference to a nursery object, record the tenured object in a
"remembered set."  On scavenge, only walk the remembered set
instead of all tenured.

**For initial cut: skip the remembered set.**  Walk all roots from
VMState; assume tenured objects hold only refs to other tenured
objects (= true if cell-update writes only Evaluated values, which
are concrete WHNF after force).  We track this assumption and
break if violated.

The cells (`Value*`) are tenured.  They hold a Value that may
reference nursery objects (e.g. when set to a Suspended thunk
pre-cell-update).  Walking cells: maintain a list of all
allocated cells; walk each on scavenge.  Cheap because cells are
relatively few.

### Scavenge algorithm

```
scavenge():
    grayQueue = []
    forwardMap = {}  // side-table

    // Stage 1: roots.
    for each root r in VMState:
        if r is a nursery pointer:
            r' = copy r to tenured
            forwardMap[r] = r'
            r ← r'
            grayQueue.push(r')

    // Stage 2: graph walk.
    while grayQueue not empty:
        obj = grayQueue.pop()
        for each ref-field f of obj:
            if f is a nursery pointer:
                if forwardMap.has(f):
                    obj.f ← forwardMap[f]
                else:
                    f' = copy f to tenured
                    forwardMap[f] = f'
                    obj.f ← f'
                    grayQueue.push(f')

    // Stage 3: reset nursery.
    nurseryNext = nurseryBase
    forwardMap.clear()
```

### What's in the nursery vs tenured

**Nursery (default):**
- Thunks (the dominant short-lived type).
- Closures (most are call-frame scope).
- ListVecs (typical attrset/list literals).
- Bindings (each non-rec attrset is fresh).

**Tenured (always):**
- Cells (Value* allocations) — referenced by Tag::Slot which may
  outlive the function frame.
- Static data (Lambdas, CompilationUnits, primops, IR).
- Anything explicitly opt'd via `tenuredAlloc()`.

### Promotion policy

For the first cut, ALL surviving objects go to tenured immediately.
No second-chance survivor space.  Simpler but means anything that
survives one scavenge is stuck in tenured forever.  Acceptable —
the nursery's job is reclaiming SHORT-LIVED thunks (the 99% case);
long-lived survivors are rare enough that tenuring them eagerly
is fine.

## Implementation phases

### Phase A: nursery allocator (no scavenge yet)

- Add `Allocator::nurseryAlloc(size_t)`.
- Route Thunk/Closure/Bindings/ListVec allocations through it.
- Add `NIX_V3_NURSERY=1` env-var gate; default OFF until Phase B.
- Add `NIX_V3_NURSERY_SIZE=N` (MB) tunable.
- When nursery overflows, FALL BACK to tenured (no scavenge).
  Validates routing without correctness risk.

### Phase B: stub scavenge (no actual copy)

- Implement scavenge entry point that just RESETS the nursery and
  fails the eval (intentional; verifies trigger semantics).
- Hook trigger at allocation.
- Run a trivial test → expect crash on nursery exhaustion.  Confirms
  Phase A's routing.
- (Don't merge this phase; it's only for testing the wiring.)

### Phase C: forwardMap-based scavenge

- Implement `walkValue`, `walkThunk`, `walkClosure`, `walkBindings`,
  `walkListVec`.
- Implement root walking.
- Run language tests (142/142) under `NIX_V3_NURSERY=1`.
- Run cutover-parity (142/142).
- Validate v3-fhook nixpkgs.hello.name still produces "hello-2.12.x".

### Phase D: cell tracking

- Maintain a per-thread set of allocated cells.
- Walk cells at scavenge.
- Validate same test set.

### Phase E: enable by default

- Flip `NIX_V3_NURSERY` to default-on (with `NIX_V3_NO_NURSERY=1`
  escape).
- Re-run v3-direct nixpkgs.hello.name with Phase 2 cycle bypass
  (per-name re-lowering); expect *completion* (slow OK).
- Document baseline timing/memory in `lode/`.

## Risks

1. **Missed root.**  An object reachable through a path we don't
   walk gets reclaimed → use-after-free.  Mitigation: rigorous
   walk-function audit; full test suite with `NIX_V3_NURSERY=1`.
2. **Pointer aliasing across scavenge.**  Local C++ variables
   holding nursery pointers across allocation calls become stale.
   Mitigation: scavenge ONLY at well-defined points (between
   opcodes); audit dispatch loop for held pointers.  Most opcode
   handlers re-fetch from `vm.frames.back()` each iteration so
   are safe.
3. **Tenured-to-nursery references.**  Without a remembered set,
   we walk all roots which doesn't include tenured-side cell
   contents.  If a tenured cell holds a Value pointing into
   nursery, that Value won't be forwarded → use-after-free.
   Mitigation: track cells (Phase D) so we walk them.
4. **Performance regression.**  Allocation hot path adds a check;
   scavenge cost amortized.  Mitigation: measure with full suite;
   `NIX_V3_NO_NURSERY=1` escape if regression is real.

## Validation plan

1. After Phase A: full regression suite (337 tests) green with
   nursery routing on, no scavenge.  Validates allocator change
   doesn't break anything.
2. After Phase C: same suite green with scavenge on.  Synthetic
   stress test (`fib 20`, deep let-recs) under valgrind for use-
   after-free.
3. After Phase D: v3-direct nixpkgs.hello.name with Phase 2
   bypass *completes*.  Memory under 5 GB.
4. After Phase E: bench suite (TW vs v3-fhook vs v3-direct)
   measured.

## File scope

- New: `src/libexpr-v3/include/v3/nursery.hh` — allocator interface.
- New: `src/libexpr-v3/nursery.cc` — implementation.
- Modified: `src/libexpr-v3/include/v3/alloc.hh` — route alloc to
  nursery when enabled.
- Modified: `src/libexpr-v3/vm.cc` — scavenge trigger, root walk,
  cell tracking.
- Modified: `src/libexpr-v3/meson.build` — add nursery.cc.

Disk-cache schema: NOT bumped.  The on-disk format is bytecode-
only, no runtime allocator metadata.

## Estimate

Phases A–E: ~3-5 focused sessions of work.  Each phase is
independently committable and validates against the full test
suite.  We can pause between any two phases.

## Status (last updated 2026-05-10)

- **Phase A: LANDED** (commit `f8b489ada`).  Bump-pointer routing
  for Closure / Thunk / Bindings / ListVec under `NIX_V3_NURSERY=1`,
  fall-back-to-tenured on overflow, no scavenge.  Allocator,
  config gates, and `run-nursery-tests.sh` are in place.
  **Reality check**: Phase A alone does NOT yield memory benefit.
  When the 32 MB nursery fills, all subsequent allocations spill
  to the tenured arena — so memory characteristics match the
  no-nursery baseline.  Phase A's role is to validate the
  routing infrastructure and provide the `NIX_V3_NURSERY` toggle
  that Phase C will re-use.

- **Phase B: SKIPPABLE** (the design doc proposed a stub scavenge
  for testing the trigger mechanism; Phase A's tests already
  exercised the routing, so we can skip directly to C).

- **Phase C: LANDED.**  Side-table forwarding scavenge under
  `NIX_V3_NURSERY_SCAVENGE=1` (independent gate from
  `NIX_V3_NURSERY` so we can route allocations without reclaiming
  until the implementation is validated).  Files: `gc.hh` /
  `gc.cc`; trigger wired into `dispatchLoop` top-of-loop with
  `Nursery::shouldScavenge()` (75 % fill threshold) and
  `Nursery::maybeScavenge()`.  Frame locals (`cu`, `closure`,
  `stackBase`, `ip`) are re-read after a scavenge runs, since
  forwarded `frame.closure` / `.thunk` may have been rewritten.
  Scope deltas vs the original design:
   - Bindings stay tenured (cell pointers + Tag::Slot targets
     would otherwise be invalidated by a move).
   - ValuePair stays tenured (allocPair() unchanged).
   - No cell registry yet (Phase D).  Instead, every reachable
     tenured object is also walked (gated by a `walked` set) so
     tenured-to-nursery references are still found.  This is
     O(reachable tenured) per scavenge — accepted for v1 since
     the priority was correctness; Phase D's cell registry will
     bound this to O(remembered cells).
   - **`exitDepth == 0` gate**: scavenge only fires from the
     outermost `dispatchLoop`.  Inner `dispatchLoop` invocations
     (re-entered via `forceValue`, `runOnExistingVm`,
     `runFunction`, or any TW→v3 bridge) hold v3 nursery
     pointers in C-stack locals (`Value arg = pop(vm), fun =
     pop(vm); ... fun = forceValue(vm, fun); ... use arg`) that
     are NOT in any walked root set.  A scavenge during the
     inner loop would orphan those C-locals.  The gate keeps
     scavenge confined to the outer loop's iteration boundary
     where opcode handlers have already run to completion.
     Cost: re-entry chains let the nursery fill to its overflow
     ceiling; the next outer iteration reclaims.  Acceptable
     because re-entry depth is bounded by the call chain.
     A future shadow-stack of "Rooted<Value>" wrappers would let
     us scavenge from inner loops too.
  Validation (2026-05-10):
   - `run-nursery-tests.sh` 16/16 (8 Phase A + 8 Phase C added,
     including p7/p8 for the exitDepth gate).
   - `run-direct-eval-tests.sh` 26/26 under
     `NIX_V3_NURSERY_SCAVENGE=1 NIX_V3_NURSERY_SIZE=2`.
   - `run-cutover-parity-tests.sh` 142/142 under same env.
   - `run-let-rec-publish-split-tests.sh` 11/11 under same env.
   - `V3_DBG_NURSERY=1` confirms scavenge fires on the outermost
     loop with full reclamation (forwarded=0 walked=0 on a
     drained foldl' workload, used-pre ≈ threshold).
   - Latent Phase A bug discovered + fixed: `tryAlloc` gated
     `initLazy()` behind `!enabled`, so the nursery silently
     fell back to tenured even with `NIX_V3_NURSERY=1` set.
     Phase A's tests passed because they only checked result
     equality between on/off, which is trivially true when
     "on" is a no-op.

- **Phase D: not yet started.**  Cell registry + walk.  Designed
  + investigated 2026-05-10; first-cut "Bindings dirty bit + write
  barrier on entries[].value writes" turns out to be *unsafe* on
  its own because of the OP_RETURN cell-write path:

  ```
  if (Value * cell = fr.thunk->cell) {
      *cell = retVal;        // cell may point INTO bindings->entries[i].value
      fr.thunk->cell = nullptr;
  }
  ```

  We don't know the containing `Bindings *` from a raw `Value *`.
  If `retVal` carries a nursery payload and we don't mark the
  containing Bindings dirty, the next scavenge skips it (dirty=0
  from a prior clean walk) and the nursery memory gets reset
  while the entry still points into it → use-after-free at the
  next read.

  Three viable Phase-D shapes:

  a. **Track Bindings* in Thunk.**  Add `Thunk::cellContainer` (8
     bytes) populated wherever `t->cell = &bindings->entries[i].value`
     is set.  At cell-write, set `cellContainer->dirty = 1`.
     Standalone `allocValue()` cells leave it null.

  b. **Conservative "any cell-write happened" global flag.**
     When set, scavenge ignores Bindings dirty bits and walks all
     Bindings (Phase C v1 behavior).  Reset after scavenge.  Loses
     Phase-D benefit when there are cell-writes between scavenges
     (the common case during active eval) but trivial to wire.

  c. **Card-marking write barrier.**  Tenured arena divided into
     4 KB cards, each with a dirty bit.  Cell-writes mark the
     card; scavenge scans dirty cards for word-aligned candidate
     pointers (precise scanning since allocations are 16-aligned
     and Value layout is known).  Standard HotSpot/Hotspot-style
     approach.

  Recommendation: (a) for the first cut.  Pairs cleanly with the
  existing per-Bindings dirty bit; cellContainer can be re-used
  later for any per-Bindings remembered-set work.

- **Phase D Bindings-dirty-bit alone is INSUFFICIENT** without
  one of the cell-write-barrier mechanisms above.  Documented
  here so a future contributor doesn't repeat the half-step.

- **Phase E: not yet started.**  Default-on flip.

Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
Input Output Group.
SPDX-License-Identifier: Apache-2.0
