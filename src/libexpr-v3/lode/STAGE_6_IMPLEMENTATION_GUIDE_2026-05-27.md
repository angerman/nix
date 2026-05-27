# Stage 6 production precise GC — implementation guide

**Date**: 2026-05-27
**Companion to**: `STAGE_6_PRECISE_GC_DESIGN_2026-05-27.md`
  (design + Option A recommendation + per-workload SHIP gates)
**Purpose**: turn the 2-3 week design into a day-by-day execution
  playbook with pseudocode, file:line modification map, regression-
  test checklist, and measurement-anchor checkpoints.

## Why this guide exists

The 34-commit session arc through 2026-05-27 evening has:

* Validated Stage 6 SHIP-GREEN via `LIVE_FRACTION_SPIKE` —
  239 MB freeable hello.drvPath, 797 MB HNE.
* Falsified Phase E v0.2 default-on as the simpler alternative
  (Day-2 mortality + Path A trigger tuning both FALSE).
* Closed the External-tag audit via `bench/arena-dereg-audit.sh`.
* Anchored every architectural-risk acknowledgement.

The remaining work is the actual Stage 6 implementation —
multi-week, multi-session.  This guide gives the implementer a
concrete day-by-day path with prerequisites verified.

## Pre-implementation reading order

Order matters for context:

1. `STAGE_6_PRECISE_GC_DESIGN_2026-05-27.md` — design choices
   (Option A vs B vs C; Cheney semispace recommended)
2. `LIVE_FRACTION_SPIKE_2026-05-27.md` — Stage 6 ROI per workload
3. `PHASE_E_V02_PATH_A_FALSIFIED_2026-05-27.md` — why the
   cheaper Phase E tuning alternative doesn't work; informs the
   safe-point model design
4. `ARENA_DEREGISTRATION_DESIGN_2026-05-27.md` — sibling design
   (arena-Boehm relationship); informs External-tag concerns
5. THIS DOC — execution playbook

Plus the canonical strategic refs:
* `CLAUDE.md` (auto-loaded; §6.3 + Critical Constraint 0)
* `lode/CHENEY_NURSERY_DESIGN.md` — the pattern Stage 6 extends
* `lode/SESSION_ARC_2026-05-27.md` — full 34-commit context

## Day-by-day execution playbook

### Week 1 — Foundation (Days 1-5)

#### Day 1 — Dual-region Arena MVP (~6 h)

**Goal**: make `threadArena()` support 2 regions (T1 = active,
T2 = backup), with T2 lazily allocated.  No GC behavior change
yet; default behavior identical.

**Files to modify**:

* `include/v3/alloc.hh:578` (Arena class)
  * Refactor `blocks` + `hugeBlocks` storage into `Region`
    inner struct
  * Add `Region activeRegion`, `Region backupRegion`
  * Add `swapRegions() noexcept` (no-op for now; just swaps
    member identities)
  * `alloc()` continues to use `activeRegion` exclusively
  * `blockRanges()` returns active's ranges (legacy behavior)

**Code sketch** (pseudo, ~40 LoC):

```cpp
class Arena {
public:
    struct Region {
        char *  cur = nullptr;
        char *  end = nullptr;
        std::vector<char *> blocks;
        std::vector<HugeBlock> hugeBlocks;
        size_t  totalBytes = 0;
    };

    void * alloc(size_t bytes) noexcept {
        // bytes aligned + huge cutoff handled
        if (active().cur + bytes > active().end) refill(active());
        // ... unchanged ...
    }

    void swapRegions() noexcept {
        std::swap(active_, backup_);
    }

private:
    Region & active() noexcept { return *active_; }
    Region * active_ = &region0_;
    Region * backup_ = &region1_;
    Region   region0_;
    Region   region1_;
    // ...
};
```

**Validation**:
* `all-v3-tests --quick` 6/6 PASS (no behavioral change)
* `all-v3-tests --core` 15/15 PASS
* HNE + hello byte-identical to TW
* No RSS increase (T2 lazy; uninitialized)

**Pitfalls**:
* Boehm `GC_add_roots` registers `active()` block ranges; T2's
  blocks register lazily on first `refill(backup_)` (which
  Day 1 doesn't trigger — backup_ stays empty)
* All existing `threadArena().blockRanges()` consumers see
  active's ranges only (preserved semantics)

#### Day 2 — RootVisitor mark+copy variant (~6 h)

**Goal**: extend `RootVisitor` interface with a `MoveGCVisitor`
subclass that mark + copies on visit.

**Files to modify**:

* `include/v3/precise_root.hh` — add `MoveGCVisitor` subclass
  (analogous to `LiveTracer` in `live_trace.cc`)
* `src/libexpr-v3/move_gc.cc` (NEW) — implement the major
  scavenge driver

**Code sketch**:

```cpp
class MoveGCVisitor : public RootVisitor {
public:
    void visitClosure(Closure * & slot) override {
        if (!slot || !inActive(slot)) return;
        Closure * forwarded = lookupForwarded(slot);
        if (!forwarded) {
            forwarded = copyToBackup(slot);  // size from header
            registerForwarded(slot, forwarded);
            enqueue(forwarded);  // for transitive walk
        }
        slot = forwarded;
    }
    // ... similar for Thunk / Bindings / List / Pair / Slot ...
private:
    std::unordered_map<void *, void *> forwardingTable_;
    std::vector<Gray> worklist_;
    // ...
};
```

**Validation**:
* Unit-test the forwarding-table logic
* Compare to gc.cc nursery scavenger's `fwdClosure` pattern;
  this is structurally the same

**Pitfalls**:
* Special-case fields per Day 4 below (CU, char *, External)
* Forwarding pointers must be DISTINGUISHABLE from real
  payload — use a sentinel tag OR install a `Tag::Forwarded`

#### Day 3 — Major-scavenge driver + threshold trigger (~4 h)

**Goal**: wire the major scavenge into the dispatch-loop trigger
infrastructure (analogous to nursery `maybeScavenge`).

**Files to modify**:

* `src/libexpr-v3/move_gc.cc` — add `majorScavenge(VMState & vm)`
  entry point
* `src/libexpr-v3/vm.cc` near line 2617 — add a `shouldMajorGC`
  check alongside `shouldScavenge`
* `include/v3/alloc.hh::Arena` — add `shouldMajorGC` heuristic
  (e.g., active region size > threshold)

**Code sketch**:

```cpp
// In Arena:
bool shouldMajorGC() const noexcept {
    if (!majorGCEnabled_) return false;
    // Default 256 MB threshold; env-overridable via
    // NIX_V3_MAJOR_GC_TRIGGER_MB
    return active_->totalBytes >= majorGCTriggerBytes_;
}

void majorScavenge(VMState & vm) noexcept {
    if (!majorGCEnabled_) return;
    // Same nested-VMState defer as nursery scavenger:
    // require activeVMStack is exactly { &vm }
    if (!atSafePoint(vm)) return;
    MoveGCVisitor mv(*this);
    walkAllV3Roots(vm, mv);
    mv.drain();  // transitive
    swapRegions();
    // free oldActive (== backup_ post-swap) blocks
    backup_->reset();
}
```

**Validation**:
* Triggered via `NIX_V3_MAJOR_GC=1` env-gate (opt-in)
* `all-v3-tests --quick` PASS with gate ON
* HNE byte-identical to TW with gate ON

**Pitfalls** (per Stage 6 design §6):
* 2× transient memory hazard — major scavenge briefly needs
  active + backup both populated
* Forwarding-pointer Tag must be unique
* CU pointers (Closure::cu) point INTO ImportCache::cus (libc
  deque) — must NOT rewrite
* String/Path chars — move to separate non-moving region OR
  forbid `Tag::String`/`Path` in moving cells

#### Day 4 — Special-case fields (~4 h)

**Goal**: handle fields that must NOT be rewritten:
* `Closure::cu` — points into ImportCache (libc), skip
* `Closure::desc` — points into CompilationUnit (libc), skip
* `Tag::String` / `Tag::Path` `payload.str/path` — for now,
  forbid these in the arena (force String→arena-chars OR
  bridge-source-side-table)
* `Tag::External` `payload.raw` — VERIFIED ABSENT per
  `bench/arena-dereg-audit.sh` cross-workload PASS; no action

**Files to audit**:

* `include/v3/closure.hh` — Closure / Thunk struct definitions
* `include/v3/alloc.hh` — Bindings::Entry layout
* `include/v3/value.hh` — Value::Payload union; tagIsPointer
  contract

**Validation**:
* Re-run `bench/arena-dereg-audit.sh` post-Day-4 to confirm
  External stays absent
* The audit harness with `--workloads hello,firefox,hne,ackermann`
  must PASS

#### Day 5 — Validation harness + initial sweep (~6 h)

**Goal**: build/extend a harness that validates major scavenge
correctness across workloads.

**Files**:

* `bench/stage-6-validate.sh` (NEW) — mirror of
  `bench/phase-e-stress-validate.sh` but with `NIX_V3_MAJOR_GC=1`
* Run on all 3 anchor workloads + ackermann

**Pre-committed SHIP gate per workload**:

* hello.drvPath: peak RSS ↓ ≥ 200 MB (validated by SPIKE)
* HNE: peak RSS ↓ ≥ 600 MB
* cardano-node M5: peak RSS ↓ ≥ 500 MB
* All workloads byte-identical to TW
* Wall regression ≤ 5%

### Week 2 — Tuning + edge cases (Days 6-10)

#### Day 6 — Trigger cadence tuning

* Measure scavenge count at thresholds 64/128/256/512 MB
* Find sweet spot for HNE (allocation rate is highest)

#### Day 7 — Long-tail workload sweep

* Run `bench/stage-6-validate.sh` on firefox + 64-pkg sweep
* Identify any workload that fails byte-identity OR exceeds wall
  budget

#### Day 8 — Stress mode

* `NIX_V3_MAJOR_GC_STRESS=N` analog to `NIX_V3_GC_STRESS`
* Force major scavenge every N opcodes (proxy for "scavenge much
  more often than the heuristic")
* Validate no missed roots under stress

#### Day 9 — Boehm interaction audit

* Confirm Boehm collections don't reclaim arena memory mid-GC
  (the bridge-source-side-table prereq from `ARENA_DEREGISTRATION_DESIGN`
  should be done first OR independently audited)

#### Day 10 — Day-2 mortality measurement

* Compare scavenge survival rates against the SPIKE prediction
* If significantly lower than predicted: investigate

### Week 3 — Ship + retirement (Days 11-15)

#### Day 11-12 — Final integration

* Default-on flip mechanics (mirror Phase D Step 11
  `c0911aee6`)
* Add `NIX_V3_NO_MAJOR_GC=1` opt-out
* Update lint scripts

#### Day 13 — Soak

* Run all-v3-tests --full + --brute for 2 hours minimum
* HNE eval repeated 10× without leak

#### Day 14 — Final perf validation

* Hyperfine n=10 on hello + firefox + HNE
* Capture wall + peak_rss + scavenge counts

#### Day 15 — Ship commit + memory update

* Commit per the SHIP gates
* Update CLAUDE.md §6.3 / lode/SESSION_ARC / memory entries

## Regression-test checklist

For each Day-N commit:

* [ ] `all-v3-tests --quick` 6/6 PASS
* [ ] `all-v3-tests --core` 15/15 PASS (every other commit at least)
* [ ] `bench/arena-dereg-audit.sh` PASSes
* [ ] HNE byte-identical to TW
* [ ] hello.drvPath byte-identical to TW
* [ ] firefox.drvPath (first time eval succeeds) byte-identical

If ANY fails: FALSIFY the commit per `[[falsification-rule]]`.

## Concrete file:line modification map

| File                                    | Day | What                                              |
|-----------------------------------------|-----|---------------------------------------------------|
| include/v3/alloc.hh:578 (Arena class)   | 1   | Refactor to dual-region; add Region inner struct  |
| include/v3/alloc.hh:578-715             | 1   | Keep allocator interface stable; internal refactor|
| include/v3/precise_root.hh              | 2   | Add MoveGCVisitor declaration                     |
| src/libexpr-v3/move_gc.cc (NEW)         | 2-3 | Implement MoveGCVisitor + majorScavenge driver    |
| src/libexpr-v3/vm.cc:2617               | 3   | Add shouldMajorGC check alongside shouldScavenge  |
| include/v3/closure.hh                   | 4   | Audit cu/desc fields for skip-list                |
| include/v3/value.hh::tagIsPointer       | 4   | Verify contract holds post-MoveGC                 |
| bench/stage-6-validate.sh (NEW)         | 5   | Validation harness                                 |
| src/libexpr-v3/CLAUDE.md §6.3           | 11  | Default-on flip marker                            |
| src/libexpr-v3/lode/SESSION_ARC         | 15  | Ship commit landing                                |

## What this guide intentionally leaves implementation-time

* Memory layout choices for forwarding-pointer encoding
* Survival-pool/age-promotion policy (independent of major GC vs
  nursery, can defer to nursery's Phase E)
* The Stage 6 trigger heuristic constants (256 MB? 512 MB?)
* Bookkeeping for which Boehm regions get re-registered post-swap

These are tunable / discoverable during implementation; not
pre-determined.

## Risks + mitigations (per session arc findings)

1. **2× transient memory hazard** (Stage 6 design §6)
   * Mitigation: trigger major GC at lower thresholds; if RSS is
     near the cap, fall back to "skip this cycle" rather than
     allocate T2 and crash
2. **External-tag concern** — verified absent on 4 workloads
   (per `bench/arena-dereg-audit.sh` PASS).  Re-verify on any new
   workload before relying on the absence
3. **Phase E v0.2 + Major GC interaction** — both walk via
   `walkAllV3Roots`.  Trigger them at different cadences but
   in COMPATIBLE safe-points (between bytecode opcodes ONLY).
4. **String/Path interaction** — see Day 4; resolve by either
   moving chars to a separate non-moving region OR forbidding
   Tag::String/Path in arena cells (force materialize)

## Cross-references

* `STAGE_6_PRECISE_GC_DESIGN_2026-05-27.md` — original design
* `LIVE_FRACTION_SPIKE_2026-05-27.md` — Stage 6 ROI
* `PHASE_E_V02_PATH_A_FALSIFIED_2026-05-27.md` — why simpler
  alternatives don't work
* `ARENA_DEREGISTRATION_DESIGN_2026-05-27.md` — sibling design
* `bench/arena-dereg-audit.sh` — External-tag audit
* `bench/phase-e-stress-validate.sh` — sibling harness template
* `CHENEY_NURSERY_DESIGN.md` — the pattern Stage 6 extends
* `[[memory-first-class]]` — SHIP gate framework
* `[[falsification-rule]]` — Rule 0
* `[[measure-twice-cut-once]]` — methodology

Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
  Input Output Group.
SPDX-License-Identifier: Apache-2.0
