# Representation-rewrite attack plan — 5-agent investigation + adversarial synthesis (2026-07-05)

Five independent code-grounded investigators fanned out over the "representation
rewrite" (the multi-quarter path to close v3's ~1.8-2.4× CPU / ~1.6-2.3× RSS
single-eval gap to the tree-walker). This doc records their findings, the
CONTRADICTIONS the adversarial review caught between them, the reconciled truth,
and a concrete phased plan.

## Agents (all read-only, file:line-grounded)
- A1 v3 byte-audit (per-cell sizeof + reducible fields + live-count recipe)
- A2 tree-walker study (why TW is lighter/faster, grounded in src/libexpr/)
- A3 thunk/laziness representation (the 623-659MB arena sink)
- A4 Bindings/attrset representation (the 347-362MB sink + drv-hash invariant)
- A5 alloc/GC model + the CPU-vs-RSS cross-check

## CONTRADICTIONS CAUGHT + RECONCILED (the point of the fan-out)

**C1 — "M5 at RSS parity" (A4) vs "M5 2.29× RSS" (A1/A5 + the authoritative
baseline). REJECTED A4's parity.** A4 cited MEMORY_REPRESENTATION_2026-06-07
("cardano-node.drvPath 924 vs TW 915MB"). That is a STALE/narrow claim: it
predates both the 2026-06-27 RSS decomp (M5 arena 1560MB / peak 3121MB, POST-
Chain, POST-Lever-B) and the 2026-07-04 darwin-4 baseline (M5 warm 2245MB vs TW
982 = 2.29×, git-noted @ e156a9874). The Chain (−268MB firefox) + NaN-box wins
are REAL but already BAKED INTO the 2.29× residual — M5 is not at parity.
Trust the recent authoritative number.

**C2 — "inline the thunk like TW" (A2, its #1 RSS lever) vs "inline-thunk
FORBIDDEN" (A3). A3 WINS.** A2 (studying TW) correctly found TW's thunk is
`{Env*,Expr*}` inline in the 16B Value — 0 separate allocation — vs v3's separate
24B+ heap Thunk (the 659MB M5 sink). But A3 (grounded in v3's constraints)
proved porting it is BLOCKED: a `Tag::App`/ValuePair-encoded thunk inside a
tenured `Bindings::Entry` is the PhD-6 missed-root class under the MOVING GC; the
`cell`-writeback contract (STG-8/#498) + `Tag::Slot` pointer-stability depend on
the separate cell; and a ValuePair can't hold N upvalues + the desc pointer
(Tag::App/App3 already cover only the 1-2-arg deferred-application subset).
**TW's inline-thunk does not port to v3's moving-GC bytecode architecture.**

**C3 — thunk LIVE-vs-DEAD split: A3 "~71MB live / ~588MB dead" vs A1 "~592MB
live at peak GC" on M5. BOTH mis-attributed; the authoritative number is in
between and is a MEASUREMENT GAP.** A3 conflated "63% never-forced" with "dead"
(a never-forced thunk in the result graph is LIVE, just unforced). A1's "592MB
live" is ~90% of allocated, implausible against the never-forced data. The
authoritative memory (reference_fresh_numbers_2026-06-27) says the M5 ARENA
(1560MB) is ~590MB LIVE / ~970MB DEAD *total across all cell types*. So A3 over-
attributed all dead-arena to thunks; A1's "592MB live thunks" ≈ the total live
arena, not thunks alone. THE EXACT PER-TYPE LIVE-vs-DEAD SPLIT IS UNMEASURED —
and it decides whether effort goes to representation (live) or GC (dead). This
is Phase 0.

## RECONCILED TRUTH (what all five actually establish)

1. **The leaner-cells "rewrite" is MOSTLY ALREADY SHIPPED.** NaN-box 8B Value
   (Lever B, c690b3f19: Entry 24→16, ValuePair 64→32, ListVec 16→8, ~−30%
   arena), Chain/overlay Bindings (Lever A, c6cba9e12: firefox −268MB/−29%),
   thunk 24B floor (three shrinks 56→40→32→24), pair-tax — all DONE. The 2.29×
   residual is the state AFTER these.

2. **Remaining cell-level wins are REAL but SMALL** (A1, byte-id-safe, not yet
   shipped): Bindings::aux removal (−16B/Bindings incl. rounding = ~50MB M5) +
   Closure header diet (drop cu/capturedWiths/upvalEnv per the proven FP-2
   Thunk pattern, 40→16B = −16..−32B/closure = ~37-74MB M5). Total ~90-130MB ≈
   **~3-4% of the 3121MB peak.** NOT parity.

3. **The DOMINANT RSS sink is ~970MB DEAD arena — a GC problem, not
   representation** (A3+A5 converge; authoritative memory confirms). But
   reclaiming it is PRIOR-KILLED both ways: non-moving sweep can't munmap
   (blocks are mixed live/dead — blocksFreed=0 every sweep), moving compaction
   raises peak (Cheney 2×). And it can't run MID-EVAL at all (moving GC can't
   forward C-stack locals → the exitDepth==0 gate). The nursery underperforms
   (7-16% hit vs 60-80% design; A5).

4. **The moving GC is the LINCHPIN blocking BOTH #2's ceiling AND the inline-
   thunk (C2) AND mid-eval reclamation (#3).** A non-moving, mid-eval-capable,
   page-releasing collector would unblock all three. THIS is the real coupled
   "representation rewrite": it's a GC rewrite that unblocks representation.

5. **Representation is an RSS lever, NOT a CPU lever** (A5, proven: Value 16→8B
   gave −18-24% RSS but +1-4% wall). CPU is DISPATCH+semantic = JIT (the
   register-VM substrate exists). FFI marshalling is already gone (F4).

6. **CU-bytecode "elsewhere" (~207MB M5, 488,998 LambdaDescriptors) has no TW
   analog** (A2: TW walks a shared parsed Expr AST). WS-B (descriptor diet)
   ~40-62MB, ~38% diagnostic. Real, separate lever.

## HONEST VERDICT
- A "representation rewrite" as narrowly framed (leaner cells) is ~90-130MB /
  ~3-4% RSS (Phase 1) — most of it already shipped. It does NOT reach parity and
  does NOT help CPU.
- REAL RSS parity requires reclaiming the ~970MB dead arena, which requires
  replacing the moving GC with a non-moving, mid-eval, page-releasing collector
  — the SAME change that would unblock TW-style inline representation. This is a
  multi-quarter GC+representation program, and the prior campaign's KILLs
  (non-moving-can't-munmap, moving-2×-peak) mean it needs a genuinely new design
  with a pre-committed falsifier BEFORE building.
- CPU parity is a JIT program (orthogonal).

## PHASED PLAN (measure-first, pre-committed gates, Rule 0)

### P0 RESULT (measured 2026-07-06, darwin-4 @ b636f62c4, bench/rss-live-dead-split.sh)
Per-type LIVE-vs-DEAD at peak, from the periodic L(t) trace (per-type live_*_mb
columns; DEAD = resident arena − live):
- **M5: arena 1456MB resident, LIVE 522MB (L=0.36), DEAD ~934MB (64%).**
  Live per-type: thunks 210MB, bindings 227MB, closures 21MB, pairs 48MB,
  lists 16MB.  (Allocated: thunks 623, bindings 362, closures 125, pairs 125.)
  So dead ≈ thunks 413 + bindings 135 + pairs 77 + … — spread across types.
  Major-mark-sweep live COUNTS: thunks 2.29M, bindings 731K, closures 163K.
- **firefox: arena 208MB, LIVE 86MB (41%), DEAD ~122MB (59%).**
- CU-bytecode (M5): 80MB, 100% cold/evictable at end (referenced=0).

**C3 RESOLVED**: neither A1 (~592MB live thunks) nor A3 (~71MB) was right —
it's ~210MB LIVE / ~413MB DEAD thunks on M5.  The arena is ~64% DEAD.

### P0b — total-tenured per-type census (measured 2026-07-06, darwin-4, NIX_V3_MIDEVAL_GC near-peak sweep @ arena ~1510MB)
Added a DEAD-cell type histogram to the sweep (mark_sweep.cc sweepOneBlock,
mirrors the live cellTypeHist) so total-tenured(type) = live + dead is known.
Near-peak sweep (liveBytes 672.6MB + deadBytes 837.4MB):
- **Bindings total-tenured = 3.84M** (live 2.32M + dead 1.52M) →
  **P1a aux-shrink ceiling = 8B × 3.84M ≈ 30.7MB.**  OVERTURNS the naive
  projection (~9MB): tenured Bindings are millions of SMALL attrsets (few
  entries), NOT the ~19-entry average of the LIVE subset — the 24B header is a
  large fraction of a small binding.  Ceiling sits AT the SHIP gate → P1a MUST
  be BUILT + measured, cannot be projection-killed.  (Live-count discrepancy
  vs P0's 730K is marking-conservatism; total-tenured is bit-census, so it is
  robust to how live/dead are split.)
- **P2 dead arena sizing** (dead MB per type at near-peak): Thunk 362.5 (43%),
  Bindings 135.5 (16%), Closure 103.6 (12%), Pair 74.6 (9%), List 44.7 (5%),
  Chars+Value ~16 — total ~837MB dead.  Thunk dominates the dead sink →
  P2's munmap-to-OS falsifier is sized against ~837MB, Thunk-heavy.

### P0-GATE DECISION: **GC-LEVER** (dead-heavy, 64%).
The dominant reducible sink is the ~934MB DEAD arena (≈30% of the 3121MB M5
peak RSS), and it is GC-bound (mid-eval reclamation + munmap-to-OS), NOT cell
size.  P1 (leaner cells) addresses only the ~522MB LIVE portion, of which just
~90-130MB is reducible (aux + closure diet) = ~3-4% of peak — worth banking
(cheap, low-risk) but NOT the needle-mover.  The real lever is P2 (GC).  NB:
NIX_V3_MIDEVAL_GC already exists (non-moving mid-eval mark-sweep, reclaims dead
into free-list bins) — so P2's spike targets the MUNMAP-TO-OS gap (the arena
never returns pages), not the collector itself.

### Phase 0 — MEASURE the live-vs-dead split (resolves C3; ~1 day; darwin-4)
Run A1's recipe on M5+firefox: `NIX_VM_STATS=1 NIX_V3_LIVE_TRACE=1
NIX_V3_LIVE_TRACE_PERIODIC=100` + a forced peak major-mark-sweep
(`NIX_V3_MAJOR_GC_THRESHOLD_MB=<just-under-peak>` → the `closures=N thunks=M
bindings=K...` line). Produce the exact per-type LIVE bytes at peak and the
LIVE-vs-DEAD split. GATE the whole program: if arena is mostly LIVE (>70%) →
representation matters (Phase 1 + the inline-thunk-via-GC-rewrite); if mostly
DEAD (>60%, as memory suggests) → the lever is GC reclamation (Phase 2), and
Phase 1 cell-shrinking is a ~3% sideshow. git-note it.

### Phase 1 — Bank the safe representation wins (~90-130MB, low-risk, shippable)
Only the byte-id-safe, GC-simple ones A1 ranked:
1. **Bindings::aux removal** (rank 1, ~50MB): header 24→16B, MapAttrs's aux →
   flag-gated tail slot / side-table. No GC complexity (Bindings tenured), no
   serialize change (headers runtime-only), drv-hash-agnostic (A4:
   lexicographicAttrEntries re-sorts by name string). Failing-first test +
   byte-id ladder + brute. Post-build darwin-4 RSS gate: SHIP ≥30MB.
2. **Closure header diet** (rank 2, ~37-74MB): apply the PROVEN FP-2 Thunk
   pattern — drop `cu` (derive from desc->cu), move `capturedWiths`+`upvalEnv`
   to flag-gated FAM tail. Touches closure alloc + every GC walk of Closure
   (scavenger/marker/evac/auditor) → adversarial review + full --brute (missed-
   root class). Post-build RSS gate: SHIP ≥30MB.
Each: measure-first ceiling on darwin-4, gated, byte-id, brute-green. These are
the concrete "representation rewrite" deliverables that are actually left.

### P1a RESULT — **SHIPPED** (cbe4fe55f, 2026-07-06)
Bindings header 24B→16B: dropped `Value aux` (dead for 99%+; Sorted/Chain never
use it); for kind==MapAttrs ONLY the aux moved to a tail slot after entries[size]
(allocMapAttrsBindings + mapAttrsAux(); mirrors the Thunk withs-slot idiom).
- **Realized saving = 16B/binding, not 8B**: the arena rounds to 16B granules
  (alloc.hh:1473); old 24+16n is 8-past-a-boundary → wasted a granule (→32+16n),
  new 16+16n is aligned → dropping aux removes the field AND the rounding waste.
  MapAttrs (rare) net-zero (old 24+16n == new header16+16n+8 tail).
- **darwin-4 same-host A/B (M5, default config, N=5):** arena bump high-water
  1456→1248MB (**−208MB deterministic**); **peak RSS 2160→2105MB = −55MB**
  (median-of-5, range 45-68MB) ≥ the 30MB SHIP gate; byte-identical
  (cardano-node.name ==); CPU −0.9% (faster, within ≤2%). Full --brute 34/34;
  adversarial layout review clean. (Arena −208 vs RSS −55: peak RSS is a mid-eval
  transient dominated by non-arena Boehm/SQLite/flake, so the arena win is only
  partially captured at the peak moment; the 55MB is robust.)
VERDICT: **SHIP** — all three gate criteria met. Unconditional layout change
(no runtime env gate); MapAttrs tail gated on the kind flag.

### Phase 2 — The GC-rewrite research spike (the ONLY path to real RSS parity)
DO NOT build; SPIKE + falsify first (the prior GC KILLs demand it). The design
question: a collector that (a) runs MID-EVAL (safepoints or precise stack maps,
not exitDepth==0), (b) RELEASES pages to the OS (munmap), (c) does NOT raise
peak (non-moving, or moving-with-a-peak-bound). Candidate: precise-stack-map
safepoints + segregated-by-lifetime blocks (so whole-block-free becomes
possible) OR a compacting collector with a peak cap. PRE-COMMITTED FALSIFIER
(before any build): on M5, a forced mid-eval collection at peak must reclaim
≥300MB of the ~970MB dead AND lower OS RSS (munmap fires) — measured on a
prototype. If it can't clear that (as every prior variant failed), KILL and
declare the ~2× RSS floor final. This spike ALSO unblocks the inline-thunk (C2):
a non-moving GC removes the PhD-6 constraint, making TW-style inline thunks
viable — so Phase 2's payoff is BOTH dead-reclamation AND the 659MB inline-thunk
lever. That coupling is why it's the real program.

### Phase 3 — CPU (orthogonal; JIT on the register-VM substrate)
Separate from representation. The register VM (shipped, fib stack-free) is the
IR substrate for a copy-patch/optimizing JIT that removes dispatch + fuses
semantic ops. Its own plan (NON_JIT_LEVER_MAP + REGISTER_VM_MEASUREMENT verdict:
JIT ceiling ~1.5-2.2×, needs J3 GC-safepoints — which Phase 2 would also
provide). Note the coupling: Phase 2's safepoints are a shared prerequisite.

## THE STRATEGIC CALL (for the human)
Phase 1 is cheap and worth doing (~3-4% RSS, low-risk). But Phase 2 (the GC
rewrite) is the only path to real RSS parity, it is multi-quarter, and the prior
campaign KILLED every reclamation variant — so it must be gated behind the Phase
0 measurement + the Phase 2 falsifier PROTOTYPE before committing. If the Phase
2 falsifier can't reclaim ≥300MB-to-OS at peak (the prior wall), then ~1.6-2.0×
RSS is v3's defended structural floor and the honest move is to lean into the
repeated-eval moat (the caches) rather than chase single-eval parity.

Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
Input Output Group. SPDX-License-Identifier: Apache-2.0.
