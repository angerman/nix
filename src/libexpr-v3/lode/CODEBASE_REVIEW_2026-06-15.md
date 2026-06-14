# v3 codebase review — 2026-06-15 (post nursery-flip attempt)

Second in-depth review (the first was `CODEBASE_REVIEW_2026-06-11.md`). Trigger:
the user asked to flip the generational nursery + Shape-A major collection
(`NIX_V3_NURSERY` + `NIX_V3_GEN_MAJOR`) default-on, then do another review. The
flip attempt was **byte-correct but BLOCKED by a real latent UAF**; this review
is anchored on the RCA + resolution of that blocker, then widens to a systematic
barrier-coverage audit, the Shape-A safepoint, and a structural recommendation.

Bottom line: **the flip blocker is found, fixed, and validated**; the correctness
gates now pass under the flip-equivalent config. The default-on flip itself
remains a cost/benefit call (nursery is a CPU lever; RSS is workload-dependent —
see §5).

---

## 1. The flip blocker — RCA (instrument, don't reason)

The default-on flip produced byte-identical drvPaths on hello + firefox, but
`git.drvPath` raised **7 SCAVENGE AUDIT missed roots**:
`nursery Thunk reachable via ListVec.elems[]`. A missed root under a moving
collector = use-after-free → the flip cannot ship.

Two RULE-0 corrections of fast hypotheses on the way to the real cause:

1. **"It's the Bindings-side raw writebacks again" — FALSE.** The earlier PhD-6
   classes were Bindings/cellWrite writebacks; this was assumed to be more of the
   same on the list side. It was not.

2. **"It's the deep-force list writebacks (print.cc forceDeep / primops.cc
   forceDeepRec)" — FALSE for this hit.** The AUDIT reported nursery **Thunks**,
   but the deep-force functions write *forced WHNF* (forceValue resolves thunks);
   a forced value is never a Thunk. So the culprit was **lazy list construction**
   storing nursery *thunks* into a *tenured* list — not a writeback of forced
   values. Two no-op edits to the deep-force functions were reverted.

**The instrument that pinned it:** `gc.cc visitList` was extended to mirror
`visitBindings` — under `V3_DBG_NURSERY_AUDIT` it now names the offending list's
allocation site (via the existing `listOriginTable` / `NIX_V3_LISTS_ATTR`) and
the element's last-writer. The offending lists are **tenured (arena = stable
pointer)**, so the alloc-site lookup hits. It named **`primops.cc:2870` =
`primZipAttrsWith`** (`builtins.zipAttrsWith`, used heavily by nixpkgs `lib`):

```
ListVec * vl = Alloc::allocList(vs.size());
for (...) vl->elems[i] = vs[i];   // vs[i] = lazy entry thunks; NO BARRIER
lv.mkList(vl);
```

**Mechanism:** `allocList` → `nurseryOrArena` returns an **arena (tenured)** list
exactly when the nursery is full at alloc time. The list is then filled with
nursery thunks. Without `listPostConstructBarrier`, the tenured list is neither
young nor in the remembered set → the scavenger never visits it → its nursery
thunks are stranded on scavenge. hello/firefox happened not to hit zipAttrsWith
under nursery pressure at the wrong moment; git did. **The blocker was a
scavenge-cadence-dependent, non-deterministic latent UAF** — exactly the kind
that "byte-correct on 2 workloads" hides.

Deterministic reproduction (the flip's exact config, via env on the reverted
binary): `NIX_V3_NURSERY=1 NIX_V3_NURSERY_SCAVENGE=1 NIX_V3_GEN_MAJOR=1
NIX_V3_NO_MAJOR_GC=1 V3_DBG_NURSERY_AUDIT=1` → git = 7 hits, reproducible. The
missing `NIX_V3_NURSERY_SCAVENGE=1` (the per-iteration dispatch-loop scavenge,
which the flip default-enabled separately from the gen-major safepoint) was why
opt-in `NIX_V3_NURSERY=1 NIX_V3_GEN_MAJOR=1` alone showed 0 — a config-fidelity
lesson for reproducing flips via env.

---

## 2. The fix — class-3 completion (list side)

The root cause is a **class**, not a site: every list-construction primop that
fills a (possibly-tenured) `ListVec` with (possibly-nursery) elements needs
`listPostConstructBarrier`. Some siblings already had it (primMap / primTail /
primAttrNames); ~12 did not. Barriers added at every nursery-capable site:

| Primop | element source | why nursery-capable |
|---|---|---|
| `primZipAttrsWith` | lazy entry values | the git blocker |
| `primAttrValues` | Bindings entry values | lazy thunks |
| `primConcatLists` | sublist elements | lazy thunks |
| `primFilter` | kept src elements | lazy thunks |
| `primConcatMap` | mapped-out sublist elems | lazy thunks |
| `primPartition` | src elements | lazy thunks |
| `primCatAttrs` | attr values | lazy thunks |
| `primGenericClosure` | work-list values | forced/thunk |
| `primSplit` | caps **sublists** | nursery ListVec elems |
| `primGroupBy` | src elements | lazy thunks |
| `fromTOML` / `fromJSON` | recursively-built values | nursery lists |
| `forceDeepRec` (writeback) | forced children | not root-covered (§3) |

**Verified-safe sites deliberately skipped** (the barrier would always no-op —
documented to prevent a future "why is this one missing?"):

- **String/Null-only lists** (splitVersion, getContext outputs, splitString,
  match groups, split caps): `isNurseryPayload` is false for String/Null tags,
  and `mkStringValueOwned` uses arena `allocChars`.
- **Attrset-element lists** (nixPath `{path,prefix}`, derivation `.all`):
  `Bindings` are **always** tenured (`allocBindings` → `threadArena`), so an
  attrset Value is never a nursery payload.
- **genList**: elements are `Tag::App` `ValuePair`s allocated tenured
  (`allocPair` → `threadArena`); their nursery captures are already covered
  per-element by `pairPostConstructBarrier`.
- **`allocList(0)`**: empty, no fill.

A **cross-file sweep** confirms no unbarriered nursery-capable list construction
remains outside primops.cc (vm.cc ops `OP_LIST_INIT`/`OP_LIST_CONCAT`/`OP_TAIL`,
value_serialize, lower — all already barriered).

### Validation (all green)

- `git.drvPath` AUDIT **7 → 0**; byte-identical.
- hello / git / firefox `drvPath`: **0 hits**, all byte-identical (flip-equiv).
- brute battery (`run-brute-audit.sh`, 1 MB nursery, whole-arena scan): **15/15**.
- new deterministic regression `test/repro-phd6-list-primop-barriers.nix` (tiny
  nursery hammers every swept primop): **"AUDIT: clean" + "ok"**.
- `--core` lang suite under flip-equiv: **21/21, ALL GREEN**.

Commits: `6643ef8a4` (construction sweep + instrument), `67856e930`
(forceDeepRec writeback).

---

## 3. Barrier-coverage audit (the invariant)

Two independent audits (construction + writeback) establish the invariant:
**every store of a possibly-nursery value into a possibly-tenured container must
either (a) go through a barrier, (b) be root-covered at scavenge time, or (c) be
provably non-pointer / always-tenured.**

- **Construction (alloc-then-fill):** Bindings (always tenured + per-entry
  `bindingsSetValue`/`bindingsSetEntry`, or `bindingsPostConstructBarrier`),
  ValuePair (always tenured + `pairPostConstructBarrier`/`pairSetEvaluated`),
  Closure (`closurePostConstructBarrier` after upvalue fill), Thunk
  (`thunkPostConstructBarrier`), ListVec (now complete, §2). **No gaps.**

- **Writeback (mutate existing container):**
  - `cellWrite(cell, v, nullptr)` standalone sites (valueEqual aSlot/bSlot,
    applyForceWriteback, slotStorage, memoSlot, OP_REC_SLOT_PUBLISH, thunk-cell
    init): all route through `standaloneCellRoots`. Intact from the earlier
    PhD-6 fix.
  - **print.cc `forceDeep`: root-covered.** Its worklist `tlDeepForceRoots` IS
    walked by the nursery scavenge (gc.cc:992) → its raw `elems[i] =` writebacks
    are safe. (Confirmed, not assumed.)
  - **primops.cc `forceDeepRec`: was a GAP, now FIXED** (§2). Walks via a plain
    C++ local — not a scavenge root — so its writeback needed the barrier.
  - **vm.cc:11459 `deepForceList` `forceWriteTarget`: documented-latent, but
    empirically clean.** The interior pointer `&list->elems[i]` can go stale if
    the list moves mid-force, but: (1) the value stack is a root, so re-entry
    re-reads the forwarded list and re-derives; (2) the brute battery's
    `fold-genlist-100k/5k/1k` stress this exact path with hundreds of scavenges
    under a 1 MB nursery and produce no hit/crash. Verdict: memoization-loss-
    only, not UAF. Left as-is (the documented "null the target" fix
    infinite-loops). **Re-audit if this path is ever changed.**

---

## 4. Shape-A generational major (FP-4) — correctness

The opt-in `NIX_V3_GEN_MAJOR=1` safepoint (vm.cc ~3478) is sound:
`(s_majorGcEnabled || s_genMajor) && exitDepth == 0`, with `s_genMajor =
g_genMajorEnabled && nursery != nullptr`. At the safepoint it calls
`nursery->forceScavenge(vm)` **before** `runMajorMarkSweep` — emptying the
nursery (all young survivors promoted to tenured, single-region) so the major
mark sees no nursery-resident cells. This is exactly what satisfies the **M-3
invariant** (the major marker skips nursery cells → running it with live nursery
cells would be UAF). The nested-VMState defer and the pre-GC IC invalidation
(attrSelectCache + recSlotCache hold raw `Bindings*` invisible to the precise
walk) are both retained. **No correctness concern.**

Measured earlier (opt-in): M5 −624MB vs major-default at +3% CPU (the Layer-C
tenured-dead reclaim); firefox neutral (its dead is already major-reclaimed).

---

## 5. The structural lesson + recommendation

This is the **second** time a barrier was forgotten at a fill site (first the
Bindings construction question during PhD-6; now the list-construction class).
The `allocList(n); fill; listPostConstructBarrier(l); mkList(l)` idiom is
**error-prone by construction** — the barrier is a separate, forgettable step,
and omission is a *silent, non-deterministic* UAF that "byte-correct on N
workloads" hides.

**Recommendations (ranked):**

1. **Make the regression permanent, not incidental.** The list-origin AUDIT
   instrument added this session + the synthetic regression are the detector.
   Add `git.drvPath` (the workload that exposed zipAttrsWith) and
   `repro-phd6-list-primop-barriers.nix` to the brute battery's standing set so
   this class cannot regress silently. (Cheap, high value.)

2. **Consider a fill-with-barrier helper / builder** so the barrier can't be
   forgotten, e.g. `Alloc::fillList(n, [&](Value* e){ ... })` that calls
   `listPostConstructBarrier` internally, or a debug assertion (under V3_DBG)
   that flags an `allocList`'d list which escapes to `mkList` without a barrier.
   This converts a silent-UAF class into a compile/CI-time guarantee. (Larger;
   propose as a follow-up sprint, not a flip blocker.)

3. **Flip readiness.** The correctness gates now pass. The remaining flip steps
   are mechanical-but-heavy: re-apply the 7 gate edits, **rebuild ALL binaries**
   (the Thunk-ABI / stale-binary trap), run a broader nixpkgs byte-equality
   sweep, then decide the default. The flip's *value* is a cost/benefit call,
   not pure correctness: the **nursery is a CPU lever** (firefox/HNE/M5 −19/−53/
   −42% CPU) with **workload-dependent RSS** (firefox +59 MB worse, M5 −241 MB
   better), and **gen-major Shape A is the RSS lever** (M5 −624 MB @ +3% CPU).
   Recommend flipping `NIX_V3_GEN_MAJOR`+nursery together (the combo the user
   asked for) only after the broader sweep, and presenting the per-workload
   tradeoff for the default decision.

---

## 6. Open items (non-blocking)

- vm.cc:11459 deepForceList `forceWriteTarget` — documented-latent, empirically
  clean; re-audit if changed (§3).
- The pair-tax foundational sprint (kAlign 16→8 or ValuePair 32→16) remains the
  next big RSS lever after the flip (memory `project_forward_plan_fp_2026-06-14`).
- The brute battery should grow git + the new synthetic into its standing set
  (§5.1).

*Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output
Group. SPDX-License-Identifier: Apache-2.0.*
