# Nursery Phase D — Write-barrier design (deep dive)

**Date**: 2026-05-18.
**Purpose**: design Phase D of the Cheney nursery (write barrier / cell tracking) carefully enough to start implementation when the team chooses to. NO CODE in this document. Supplements `CHENEY_NURSERY_DESIGN.md` (the canonical reference); this doc goes deeper on the specific design decisions Phase D requires.

**Why this matters now**: the team's stated cumulative target — `hello.drvPath` 30× → ≤3× over TW — assumes Stage 3 (nursery default-on) closes the ~5-10× GC-scan factor of the 200× force-rate gap (per `EXTEND_DERIVATION_INVESTIGATION_2026-05-18.md`). Stage 3 cannot land without Phase D. Phase D has been deferred since 2026-05-10 because the first-cut design ("Bindings dirty bit + write barrier") turned out to be unsafe alone (commit `78835a047`).

## 0. Recap of what Phase D needs to do

In Cheney generational GC:
- **Minor GC** (scavenge): collect only the nursery (the young region). Trace from roots; copy live nursery objects to the tenured region; reset nursery.
- **Inter-generational pointer problem**: if a tenured object holds a pointer into the nursery, that pointer is a root for the nursery scavenge. Without finding it, the pointed-to nursery object gets reclaimed (use-after-free) or the pointer is left pointing into stale memory.

Phase D's job: track all tenured-to-nursery pointers so scavenge can find them without walking the entire tenured heap.

The existing Phase C implementation works (in correctness) by walking ALL reachable tenured objects on each scavenge. This is O(tenured size) per scavenge — kills the perf benefit of generational GC for large heaps. Phase D's job is to bring this to O(remembered set size) — proportional to the number of inter-gen pointers, which should be small.

## 1. The specific hazard that already broke (commit `78835a047`)

The first-cut Phase D design was: per-Bindings dirty bit + write barrier on `entries[i].value` writes. That works for writes that go through a Bindings struct.

But OP_RETURN's cell-write path writes through a bare `Value*`:

```cpp
if (Value * cell = fr.thunk->cell) {
    *cell = retVal;        // cell may point INTO bindings->entries[i].value
    fr.thunk->cell = nullptr;
}
```

The cell pointer is a raw `Value*`. We don't know which Bindings (if any) it lives in. If `retVal` is a nursery pointer and the containing Bindings is tenured, the Bindings's dirty bit doesn't get set. Next scavenge skips that Bindings as "clean," nursery memory is reset, the cell is left pointing into garbage → use-after-free at next read.

This is the **load-bearing constraint**: Phase D must handle bare `Value*` cell writes, not just Bindings-internal writes.

## 2. The three viable shapes (from existing doc) — critical review

The existing design doc proposes three shapes. Let me review each more carefully.

### (a) `Thunk::cellContainer` — back-pointer per cell-bearing Thunk

Idea: each Thunk that has a cell pointer also stores a pointer to the containing Bindings (or null for standalone `allocValue()` cells). At cell-write, set `cellContainer->dirty = 1`.

**Memory cost**: 8 bytes per Thunk. v3 allocates ~50M Thunks during `hello.drvPath` per the existing Cheney doc; 8 × 50M = 400 MB additional. **Unacceptable** at face value.

Wait — that's the gross allocation count. The LIVE Thunk count is much smaller (nursery-reclaimed). At any point, live Thunks are maybe 10K-100K. 8 × 100K = 800 KB. Fine.

But: the field exists per-Thunk regardless of liveness, allocated at creation. The 400 MB number is the cumulative allocation cost, which the nursery is meant to reclaim. So the actual peak overhead is 800 KB-ish. OK, acceptable.

**Coverage**: handles OP_RETURN cell-writes when the cell points INTO a Bindings. Doesn't handle:
- Cells from standalone `allocValue()` (cellContainer is null; no Bindings to dirty-mark; scavenge would need to walk these separately).
- Inter-gen writes from non-Thunk sources (Tag::App memoization, closure upvalues, etc.).

**Verdict**: targets the specific known hazard cleanly. Needs supplementation for other write sites.

### (b) Global "any cell-write happened" flag

Idea: any cell-write sets a global flag. On scavenge, if flag is set, walk all Bindings (Phase C v1 behavior); else use dirty bits.

**Pros**: trivial to implement. No memory cost.

**Cons**: loses Phase D benefit during active eval. Cell-writes happen constantly (OP_RETURN fires on every thunk completion). The flag will essentially always be set between scavenges.

**Verdict**: this is "Phase C with a small optimization that doesn't help." Not a real Phase D. Reject.

### (c) Card-marking write barrier

Idea: divide tenured arena into 4 KB cards (per existing doc) or 512 B (per standard HotSpot tuning). Each card has a dirty bit. Cell-writes mark the card. Scavenge scans dirty cards for pointers.

**Memory cost**: 1 byte per 512 B = ~0.2% overhead. For 100 MB tenured = 200 KB card table. Acceptable.

**Coverage**: handles ALL inter-gen writes generically (cell-writes, Tag::App memoization, closure upvalues, attrset slot writes) — as long as the write site is instrumented with a barrier call.

**Pros**: generic, scales to all write sites without per-site reasoning.
**Cons**: more implementation work; needs scanning logic to walk card contents (knowing where pointers live inside arbitrary tenured objects).

**Verdict**: more work upfront but more robust. Standard textbook choice.

## 3. The hidden third hazard (NOT covered in existing doc)

The existing doc identifies the OP_RETURN cell-write as the known hazard. But there are other inter-gen write sites that need consideration:

### 3.1 Tag::App memoization (added 2026-05-18, commit `d3e41c13d`)

```cpp
// At first force of an App:
ValuePair* app = ...;  // may be tenured (after promotion)
app->evaluated = result;  // result may be in nursery
```

If the App is tenured (it was allocated nursery then promoted, OR allocated tenured on overflow) and the result is in nursery, this is an inter-gen write. The existing recommendation (a) does NOT cover this.

**Detection**: audit Tag::App layout. Per the existing doc: "ValuePair stays tenured (allocPair() unchanged)." So ValuePair is ALWAYS tenured. This means EVERY first-force write to `app->evaluated` is a tenured-write — and if the result is nursery, it's an inter-gen write.

This is a **structural** issue, not an edge case. After today's Tag::App memoization landing, Phase D MUST handle this write site.

### 3.2 Closure upvalue assignment

Closures hold upvalues. If a closure is tenured (promoted) and a captured upvalue is later mutated to point into nursery, inter-gen write.

But: are closure upvalues mutable post-creation? In v3 lower.cc, upvalues are set at closure-construction time and don't change. So this is a non-issue UNLESS closure-pool recycling (fakeClo) reuses a closure with fresh upvalues — which IS a mutation. Closure-pool retirement (already action-plan item) eliminates this.

### 3.3 Bindings extension via `//`

`OP_ATTRS_UPDATE` produces a new Bindings (the result of `a // b`). The new Bindings is freshly allocated (typically nursery). Its slots are written once at creation, then immutable.

But: the new Bindings's slots may point to:
- Nursery values from `a`'s slots
- Nursery values from `b`'s slots
- Tenured values from either

When new-Bindings is itself promoted later, the slots become tenured-pointing-anywhere. The slot writes happened at creation time (in nursery); the tenured-write happens at promotion via the scavenge copy. The promotion copy itself doesn't need a barrier — it's the scavenge — but the resulting tenured Bindings's dirty state must reflect "has nursery pointers" if scavenge couldn't promote them all in one pass (multi-survivor-pool scenario; not in v1 since all survivors go directly tenured).

For single-pool promotion (current design): once promoted, all slots either point tenured or were copied in the same scavenge. No outstanding nursery pointers. OK.

For future multi-pool: Phase D needs to track this.

### 3.4 List literal construction

`OP_LIST_BUILD` builds ListVec inline. Similar story: nursery write at creation; immutable after.

### 3.5 Closure-pool reuse (already flagged)

If retained in the design, pool reuse is a mutation site. Best to retire the pool, removing this write site entirely.

### 3.6 Bindings::sortByName / canonicalization

If Bindings entries are sorted in place after construction, that's a mutation. Need to check if v3 does this.

**Summary**: there are at least 2-3 confirmed inter-gen write sites beyond OP_RETURN cell-writes:
- Tag::App memoization (CRITICAL: ValuePair is always tenured per existing doc)
- Closure upvalue mutation (only if pool is retained)
- Future multi-pool promotion

The first one ALONE means recommendation (a) is insufficient as currently specified.

## 4. Sharpened recommendation: hybrid (a) + targeted extensions, with future migration to (c)

### Phase D v1 (minimum viable):

**Mechanism**: per-Bindings dirty bit + Thunk::cellContainer (recommendation (a) from existing doc) + **additional dirty-bit-bearing fields on ValuePair (for Tag::App memoization)**.

Concretely:

1. Bindings carries a `dirty: bool` field (already in existing design).
2. Thunk gets `cellContainer: Bindings*` (the existing (a) recommendation). At OP_RETURN cell-write, set `cellContainer->dirty = 1` if cell-write touches an inter-gen value (i.e. result is in nursery and cell is in a tenured Bindings).
3. **NEW**: ValuePair carries a `dirty: bool` field. At Tag::App memoization write, set `app->dirty = 1` if the evaluated result is in nursery.
4. **NEW**: scavenge walks the ValuePair dirty set in addition to Bindings.

**Coverage**: all currently-known inter-gen write sites.

**Cost**: 1 bit per Bindings (in existing dirty field), 1 bit per ValuePair (8 bytes if word-aligned, or co-locate with existing tag), 8 bytes per Thunk (for cellContainer). Total: ~ten MB at peak live set on nixpkgs eval. Acceptable.

**Scope**: each new write site identified must have a barrier added. Mechanism is per-site-tailored.

### Phase D v2 (future): migrate to card-marking (c)

When v3 adds more inter-gen write sites (anticipated in Stage 4 strictness analysis introducing uniform thunkification; Stage 5 hidden classes potentially mutating shapes; etc.), the per-field dirty-bit approach becomes hard to maintain. At that point, migrate to card-marking (c) which generalizes.

Migration path: keep dirty bits during transition; add card table; reconcile both during scavenge; eventually retire dirty bits.

### Why not start with (c)?

Two reasons:

1. **Card table scanning needs precise pointer layout knowledge.** v3's tagged Values are 16-byte aligned, but tenured arena may contain mixed v3 types (Thunk, Closure, Bindings, ValuePair, ListVec, plus cells) with different internal layouts. Scanning a card requires either:
   - Per-card metadata "what type is this" — extra overhead
   - Per-object layout descriptors — significant infrastructure
   - Conservative scan (treat any pointer-shaped word as candidate; verify it's actually a pointer to v3) — false positives but works

   v3 doesn't have layout descriptors today. (c) needs that infrastructure.

2. **The per-site dirty-bit approach forces us to AUDIT every write site.** This is itself valuable — it surfaces hidden inter-gen writes. Once we've enumerated all sites, the (c) migration becomes a "for each known site, do barrier call" change rather than "for every write in the codebase, do barrier call" (which is what (c) would require if we didn't already know the sites).

So: ship (a)+ first; audit completeness; migrate to (c) only when (a)+ becomes unwieldy.

## 5. The C-stack invisibility problem (still load-bearing)

Per existing doc and `feedback_v3_nursery_cstack_safety.md`: scavenge ONLY at `exitDepth == 0`. Inner dispatch loops hold v3 nursery pointers in C-stack locals that no root walker sees.

This is **independent of Phase D**. The barrier alone doesn't fix it; the barrier only handles HEAP-TO-NURSERY pointers, not C-STACK-TO-NURSERY pointers.

**Status**: Phase C handles this by the `exitDepth == 0` gate (no inner-loop scavenge). Phase D doesn't change this. Both must coexist.

**Future option** (NOT Phase D): a "shadow stack" of `Rooted<Value>` wrappers (similar to V8's HandleScope, SpiderMonkey's Rooted<T>) that lets inner dispatch loops register their C-locals as roots. This would let scavenge fire from inner loops. Significant refactor; defer to a hypothetical post-Stage-3 hardening pass.

## 6. The FFI boundary problem

When v3 calls TW (through `ffi.cc` / `bridge_yield.cc` / vm.cc bridge plumbing), v3 Values cross into TW. If a nursery pointer crosses, TW may write it into a Boehm-managed cell. Our barrier doesn't fire because TW doesn't know about the barrier.

Three options:

(i) **Promote at FFI exit**: every nursery Value being passed to TW is promoted to tenured immediately before the bridge call. Correctness-clean; some perf cost (an extra copy per FFI value).

(ii) **Conservative invariant**: treat the entire Boehm heap as potentially-dirty whenever an FFI call has happened since the last scavenge. Scavenge walks all of Boehm (which is what Phase C does today). Loses Phase D benefit if FFI is frequent.

(iii) **Disallow nursery values crossing FFI**: FFI calls only accept tenured Values. Lower.cc / FFI wrappers enforce promotion at entry. Strict but clean.

For v3: the V3-NATIVE constraint (LESSONS §1.1) limits FFI to system boundaries (store ops, derivations, file I/O). These are RARE compared to eval hot path. So (i) — promote-at-exit — is cheap in practice.

**Recommendation**: promote-at-FFI-exit. Add an explicit `ensureTenured(Value&)` helper invoked at each FFI exit point. The promotion is essentially "if this Value is a nursery pointer, scavenge-copy it to tenured and update the local Value."

Cost per FFI call: O(depth of the Value graph being promoted). Most FFI values are simple (string, number, single store path). Acceptable.

## 7. The closure-pool / fakeClo coordination

The fakeClo pool reuses closures across allocations. If a pooled closure:
- Holds nursery pointers
- Gets recycled into a new context where it's treated as fresh

The new context has stale nursery references.

**Current state**: pool sits on top of Boehm allocation. Pool entries are tenured (Boehm-allocated). Pool entries CAN hold nursery pointers (if a closure was created during eval with nursery captures).

**Implications for Phase D**: pool entries are tenured-with-possibly-nursery-pointers. They need to be in the remembered set / barrier path.

**Cleanest solution**: retire the pool. The action plan already lists this. Once retired, pool-related inter-gen writes vanish.

**Phase D dependency**: retire pool BEFORE Phase D ships, OR add pool-entries to the remembered set walked at scavenge. Recommendation: retire pool first.

## 8. Validation strategy

Phase D's correctness risk is high: any missed barrier site = lost reference = use-after-free or wrong result. Detection requires aggressive stress testing.

Required tests (in order of importance):

1. **`V3_DBG_GC_STRESS=1` mode**: forces scavenge after every N allocations (N = 10, 100, 1000 sweeps). Catches missed barriers by maximizing pressure. Should be run continuously in CI.

2. **AddressSanitizer / Valgrind**: run lang tests + cutover-parity under ASan/Valgrind. Use-after-free shows up.

3. **Property test**: TW-oracle comparison. Eval result must equal TW result under random `V3_DBG_GC_STRESS` schedules. (We have property tests already; extend to scavenge schedules.)

4. **Targeted repros**: for each inter-gen write site identified in the audit, write a `.nix` repro that exercises the path. Run under stress mode. Each repro stays as a regression test.

5. **drvPath / outPath parity**: extend `derivation-parity.sh` to run under scavenge stress.

6. **Adversarial allocation pattern**: pathological inputs that maximize the inter-gen write rate (e.g., a `mkDerivation`-like shape where every binding is updated post-creation). Catches barriers that work for normal workloads but fail under pathological mixes.

7. **Memory ballooning regression**: confirm Phase D actually reclaims memory. Bench shows tenured-arena ceiling on hello.drvPath under nursery default-on; should be MUCH less than the current 1 GB Boehm watermark.

## 9. Effort estimate

| Step | Effort | Risk |
|---|---|---|
| Write-site audit | 2-3 days | Low — codebase grep-able |
| Implement Thunk::cellContainer | 1-2 days | Low |
| Implement Bindings dirty bit + barrier in cell-write path | 2-3 days | Medium (subtle conditions) |
| Implement ValuePair dirty bit + barrier in Tag::App memo path | 1-2 days | Medium |
| Implement scavenge updates (dirty-set walk) | 2-3 days | Low (Phase C scaffolding exists) |
| FFI promotion at exit points | 1-2 days | Low |
| Closure-pool retirement coordination | 1 day | Low (already action-plan item) |
| Stress test infrastructure (`V3_DBG_GC_STRESS`) | 2-3 days | Low |
| Run lang/cutover/property/parity tests under stress | 1 week running + bug-fix loop | High (correctness validation; race conditions hide) |
| Bench validation: drvPath memory + wall-time under nursery on | 1-2 days | Medium |
| **Total** | **3-4 weeks focused work** | **Cumulative: medium-high** |

Existing doc estimated Phase D as part of "~3-5 sessions across Phases A-E." Realistic isolated estimate: 3-4 weeks of focused work, dominated by the audit and the stress-test validation loop.

## 10. Open questions

These are decisions the team needs to make before implementation:

**Q1: Per-Bindings dirty bit OR card table for tenured Bindings?**
Per existing doc: per-Bindings dirty bit (cheaper memory; simpler). Confirmed for Phase D v1.

**Q2: Promote-at-FFI-exit or conservative-after-FFI?**
Recommendation: promote-at-FFI-exit. The cost is small given FFI is rare in v3-direct.

**Q3: Retire closure pool before or during Phase D?**
Recommendation: BEFORE. Eliminates one inter-gen-write site cleanly. Both are action-plan items.

**Q4: What scavenge frequency policy?**
Existing doc: 75% nursery fill threshold. Confirmed. Add `V3_DBG_GC_STRESS=N` for forced scavenge every N allocations.

**Q5: How to handle standalone `allocValue()` cells (cellContainer == null)?**
The existing recommendation (a) leaves these uncovered. Options:
- Maintain a separate cell registry (per-thread list of allocated cells)
- Promote standalone cells eagerly so they're always tenured-pointing-tenured
- Forbid standalone cells with nursery values

Recommendation: option 2 (eager promotion). Standalone cells are rare and small; promoting at allocation cost is fine.

**Q6: Does FFI ever cross BACK from TW to v3 with new Values?**
Yes — TW may return values from store ops, file reads, etc. These are Boehm-allocated. When v3 receives them, no nursery is involved (Boehm values stay tenured). No barrier needed at FFI entry.

But: if TW's return value is captured in a v3 nursery Bindings (e.g. an attrset literal), that's a nursery→tenured pointer, which is NOT an inter-gen issue (nursery scavenge finds tenured pointers naturally). No barrier needed.

**Q7: Should Bindings dirty bits be per-entry or per-Bindings?**
Per-Bindings (single bit). Per-entry adds memory; the precision win is small because scavenge already walks each entry once when the Bindings is dirty.

**Q8: What about the with-stack (`vm.withStack[]`)?**
With-stack entries are Values; if they're nursery-pointing, scavenge sees them as roots (already in existing Phase C root walk). No barrier needed.

**Q9: How does Phase D coordinate with Boehm's own GC?**
Boehm runs on its own schedule. If Boehm GC fires DURING our scavenge, we have a race. Mitigation: wrap our scavenge in `GC_disable()` / `GC_enable()`.

**Q10: What if a tenured object holds a Value pointing into the OLD nursery (pre-reset)?**
After scavenge, nursery is reset. The tenured pointer should have been updated to the post-promotion address (in tenured). If not, we have a missed barrier or missed walk. Test: stress mode with forced scavenge should catch this.

## 11. Self-critique — what might be wrong with this analysis

Following the discipline of `PERF_STRATEGY` and `PARALLEL_EVAL_CAPABILITIES`: let me question my own claims.

**Critique 1: Tag::App memoization as a "structural" issue.**
I asserted that ValuePair is "always tenured" per existing doc, making Tag::App memoization an inter-gen write site. Let me verify: the existing doc says "ValuePair stays tenured (allocPair() unchanged)." That's about the v1 nursery routing — ValuePair was not moved to nursery. Today (2026-05-18) Tag::App memoization was added; was it added with nursery allocation or tenured? Need to check. If today's Tag::App is nursery-allocated, the inter-gen concern disappears (or shifts to App-promoted-then-mutated).

**Possible correction**: today's ValuePair may have moved to nursery. Verify before committing to the per-ValuePair dirty bit design.

**Critique 2: 8 bytes per Thunk as "acceptable."**
I dismissed the per-Thunk overhead. But v3's Thunk struct is already memory-pressured (we care about cache lines). Adding 8 bytes pushes the struct from 56 to 64 bytes — possibly across a cache-line boundary depending on layout. Need to actually look at the struct.

**Possible correction**: cellContainer may need to be packed into existing padding rather than added as a new field.

**Critique 3: Effort estimate of 3-4 weeks.**
I estimated stress-test validation at "1 week running + bug-fix loop." Multi-threading bugs hide for years (LESSONS); single-threaded GC bugs are easier but still subtle. Realistic: 2-4 weeks of validation if anything surprises us. So total could be 4-6 weeks.

**Critique 4: "Card-marking is more work than (a)+."**
I claimed (c) needs object-layout-aware scanning that v3 doesn't have. But v3 DOES have per-tag layout info (we know what's in a Thunk vs Bindings vs ValuePair). The scanning could iterate v3-allocated objects (which we track) without needing card-level metadata. The "card" abstraction is over an arena of v3-typed objects, not arbitrary memory. Maybe (c) isn't as much more work as I claimed.

**Possible correction**: re-evaluate (c) as a first-cut option. May actually be cleaner than (a)+ because it scales generically.

**Critique 5: FFI is "rare."**
I asserted FFI is rare in v3-direct. Let me check: `hello.drvPath` evaluation calls into `primDerivationStrict` which... probably calls into store. Each call to `__derivationFromPreprocessed` (Option 4 hybrid FFI leaf) IS an FFI call. How often does that fire per derivation? Once per primDerivation invocation. For 50K derivations in nixos-rebuild, that's 50K FFI calls. Not rare. Promotion-at-exit cost matters.

**Possible correction**: measure FFI frequency before claiming promotion-at-exit is cheap.

**Critique 6: Audit completeness.**
I listed inter-gen write sites by reasoning. A more thorough audit would grep the codebase for every assignment to a `Value` field of a tenured-allocatable type. I haven't done that grep. The list I produced is "what I thought of," not "what's actually there."

**Action**: an actual grep-based audit is a Phase D prerequisite. Cannot ship Phase D without it.

**Critique 7: The "exitDepth == 0" gate as a Phase D dependency.**
I described it as independent of Phase D. But Phase D's barrier is most valuable when scavenge can fire frequently. If scavenge only fires at outer-loop boundaries (which may be rare during deep stdenv chains), Phase D's per-scavenge speedup is amortized over fewer scavenges. The combined consequence: Phase D ships, but doesn't help much because scavenge can't fire often enough.

**Possible correction**: Phase D's value depends on scavenge frequency. If exitDepth gate keeps scavenge rare, Phase D may not unlock the promised 5-10× GC factor. The Rooted<Value> shadow stack (future option I mentioned) may be a prerequisite, not a future optimization.

**Critique 8: Pool retirement as a Phase D dependency.**
I said "retire pool BEFORE Phase D." But the action plan has these as separate items. Coordinating them adds schedule complexity. If pool retirement slips, Phase D either waits or adds pool-entry handling.

**Possible correction**: design Phase D to handle pool entries (treat them as part of remembered set) so it can ship independently. Pool retirement remains action-plan work but doesn't block Phase D.

## 12. Revised recommendation given the self-critique

Five changes from the initial recommendation:

1. **Audit first**: actual grep-based audit of inter-gen write sites BEFORE picking (a)+ vs (c). The decision depends on how many distinct write sites exist.

2. **Re-evaluate (c)**: card-marking may be cleaner than I initially credited. v3 has per-tag layout info; card scanning over v3-typed objects is tractable.

3. **Verify today's Tag::App memo allocation site**: nursery or tenured? Affects whether ValuePair dirty bits are needed.

4. **Make Phase D resilient to closure-pool**: handle pool entries explicitly so pool retirement isn't a blocking dependency.

5. **Investigate the scavenge-frequency multiplier**: if exitDepth == 0 keeps scavenges sparse, calculate whether Phase D's per-scavenge speedup actually unlocks the 5-10× GC factor or whether the Rooted<Value> shadow stack is also needed.

## 13. Decision the team needs to make

Two paths forward from here:

**Path α (incremental)**: ship Phase D v1 as recommendation (a)+ from this doc — dirty-bit-bearing fields on Bindings + Thunk::cellContainer + ValuePair dirty bit. Targets known sites. Migrate to (c) when needed.
- Pros: smaller scope; cleaner audit per-site.
- Cons: future migration cost.

**Path β (card-marking from day one)**: ship Phase D v1 as recommendation (c) — card table over the tenured arena. Generic across all sites.
- Pros: no migration; scales to new write sites automatically.
- Cons: bigger upfront implementation; card-scanning infrastructure needed.

Either is defensible. Decision should be informed by:
- The actual audit (how many distinct write sites exist?)
- Whether the team plans to add many more inter-gen write sites in Stages 4-7 (yes for Stage 4 strictness analysis — uniform thunkification introduces more inter-gen writes)
- The available implementation time (Path β is +1-2 weeks)

**My weakly-held recommendation**: Path β (card-marking). The reasoning is that Stage 4 strictness will introduce more write sites; planning for (c) now avoids per-site audit work later. But this is "weakly held" because the audit results may reveal that the write-site count is small enough that (a)+ is adequate.

## 14. Cross-references

- Canonical existing reference: `CHENEY_NURSERY_DESIGN.md` — the design context.
- Existing memory: `feedback_v3_nursery_cstack_safety.md` — the exitDepth == 0 constraint.
- 200× decomposition: `EXTEND_DERIVATION_INVESTIGATION_2026-05-18.md` (~5-10× factor that Stage 3 / Phase D addresses).
- Force-rate decomposition memory: `project_force_rate_decomposition_2026-05-18.md`.
- Roadmap stage: `ROADMAP_TO_VISION_2026-05-15.md` Stage 3.
- Lessons on Boehm-as-current-allocator: `LESSONS_LEARNED_2026-05-15.md` §1.6.

## 15. Summary in one paragraph

Phase D needs a write barrier that catches inter-generational pointers (tenured → nursery). The existing design recommends per-Bindings dirty bits + Thunk::cellContainer (option (a)). This ultrathink identifies an under-recognized hazard: Tag::App memoization (today's `d3e41c13d`) writes into tenured ValuePairs, requiring a separate per-ValuePair dirty mechanism if we stick with (a)+. A grep-based audit of ALL inter-gen write sites is a prerequisite; without it, the per-site dirty-bit approach risks missing sites. Card-marking (option (c)) generalizes across all sites but needs object-layout-aware scanning that v3 doesn't currently have but COULD have given its tagged-Value design. The recommendation is to do the audit first, then choose; the weakly-held position is to lean toward (c) given Stage 4's anticipated additional write sites. Independent of the choice: FFI promotion-at-exit handles cross-VM-Boehm boundaries; closure-pool retirement removes one write site cleanly; the C-stack `exitDepth == 0` gate is a Phase C concern that limits Phase D's effective scavenge frequency, and may need a Rooted<Value> shadow stack to unlock the full GC-factor speedup. Effort: 3-4 weeks focused (audit + impl + validation), possibly 4-6 if surprises emerge in the stress-test loop.

Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group. SPDX-License-Identifier: Apache-2.0.
