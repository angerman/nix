# Why v3 still uses more memory than the tree-walker — root analysis (2026-06-14)

The team has falsified every *single* RSS lever against a −80 MB bar (materialize
−33 MB, capture-sharing −12 MB, broader-A ≤72 MB cap-trade, depth>0-GC +38.8 % CPU).
That pattern — every lever individually "too small" — is itself the diagnosis: **v3's
memory excess is a distributed per-object constant tax plus unreclaimed churn, not a
single removable structure.** This note decomposes it from fresh measurements and
names the levers (one of which the H-probe mis-measured).

All numbers: laptop `build/src/nix/nix` @ HEAD `d9bd96a05`, `NIX_VM_STATS`, byte-identical.

## 1. The measured firefox decomposition (the load-bearing fact)

firefox.drvPath: **arena 503 MB, RSS 506 MB** (TW RSS 333 MB). The major GC mark:

```
LIVE 262.2 MB   DEAD 224.4 MB (resident — blocksFreed=0)   reclaim 46.1%
live cells: thunks 1,015,251 · pairs 1,000,553 · bindings 144,734 · chars 1,036,394
            · "cells" 656,509 · lists 49,251 · closures 48,010
cumulative alloc (per-tag): bindings 264.5 · thunks 111.9 · pairs 45.8 · chars 26.7 MB
thunks allocated 2,046,969 · forced 641,393  → 68.7 % NEVER FORCED
```

Two facts jump out, and they are *different problems*:

- **44 % of peak RSS (224 MB) is DEAD** — stranded because the collector freed 0 blocks.
- The **262 MB live** set is itself fatter than TW's equivalent graph, per-object.

## 2. The three layers (the team has been conflating them)

### Layer A — per-object representation tax (the LIVE set is ~1.4× TW)
Apples-to-apples, for the same firefox eval graph:

| object | v3 | TW equivalent | v3 excess |
|---|---|---|---|
| **thunk** | 40 B header + 8 B/upvalue (≈55 B avg) | 16 B Value `{Env*, Expr*}`, **Env shared per scope** | ~30–40 MB |
| **App / pair** | `ValuePair` 32 B (`left,right,evaluated,third`) | ~16 B `{left,right}` | ~16 MB |
| **bindings entry** | 16 B inline `{name,pos,value}` | 16 B `Attr` **+ 16 B separate Value** | v3 **leaner** (−) |
| **materialize copies** | 72 MB ceiling / 33 MB live (v3-only) | TW layers, never copies | +33 MB |

The thunk is the core of it. **TW's thunk is a 16 B `{Env*, Expr*}` that shares one
`Env` across every sibling thunk in a scope; v3 closure-converts — each thunk is a fat
heap object carrying its own `desc` + `cu` + `cell` + `state` + flat-copied upvalues.**
This is the same architectural bet (flat capture, register VM) that makes v3 *win* on
compute (fib RSS 0.27× TW) and *lose* on the millions of lazy thunks a derivation graph
produces. The bindings array is actually leaner in v3 (inline 8 B value vs TW's
pointer-to-separate-Value); v3 loses entirely in the thunk and pair objects.

### Layer B — churn volume (peak ≈ cumulative alloc, because *neither* engine reclaims)
TW firefox RSS 333 MB < its 384 MB Boehm initial heap → **TW never collects.** v3's GC
fires once near end-of-eval and frees 0 blocks. So for BOTH engines, peak RSS ≈ total
bytes ever allocated. v3's high-water is higher because it allocates more garbage bytes:
- bigger objects (Layer A) → bigger garbage, and
- v3 lacks TW's `maybeThunk` zero-alloc paths (TW allocates **no** thunk for `ExprVar`/
  literals — returns the existing env slot / `&v` in the AST) and TW updates thunks
  **in place** (the thunk Value becomes its result; no separate result object). v3
  allocated 2.05 M thunks and forced only 641 K — the 1.4 M never-forced thunks plus
  forced-thunk headers are pure churn TW largely never creates.

### Layer C — the 224 MB stranded dead (the biggest single firefox number)
v3 *could* beat TW here, because **TW architecturally cannot reclaim at all** (conservative
Boehm, never collects below 384 MB). v3's reclaim is blocked three ways, all measured:
(1) the major GC fires only at `exitDepth==0` (once, near end); (2) it is non-moving, so
44 %-dead-but-not-fully-dead 16 MB blocks can't be returned (blocksFreed=0); (3) the mark
is cache-bound (~225 ns/cell, §1.8) and non-generational, so firing it mid-eval costs
+38.8 % CPU per fire (workstream E, falsified at its stage-1 CPU bar).

## 3. Why the lever search kept "failing": the bar is mis-shaped, not the levers

Every Layer-A lever is 15–40 MB by construction — that is the *shape* of a distributed
constant tax. The **−80 MB single-lever bar caused correct wins to be rejected**:
materialize L1 (−33.6 MB, byte-identical, CPU-neutral) was gated-off for missing a
−40 MB floor; capture-sharing (−12 MB) retired. Stacked, L1 + the thunk-header shrink
below + the pair tax would clear 60–90 MB firefox / 300 MB+ M5 — but only if they are
**stacked, not gated individually against a single-lever threshold.**

## 4. The lever the H-probe mis-measured: thunk header 40 B → 24 B

The H-probe measured **upvalue-tuple dedup** (are two thunks' capture tuples
byte-identical? → 12 MB) and concluded the 40 B header is "untouchable." That measured
the wrong thing. The header is touchable — **two of its five 8-byte words are
removable**, verified in code:

- **`cu` (8 B) is redundant with `desc`.** The CU owns its `LambdaDescriptor`s
  (closure.hh:280), and the read sites already treat `cu` as derivable
  (`t->suspended.cu ? t->suspended.cu : cu`, vm.cc:8143/13657). Add a `const
  CompilationUnit*` back-pointer to `LambdaDescriptor` (shared — one per descriptor,
  not per thunk) and drop the per-thunk field.
- **`capturedWiths` (8 B) is null for any thunk with no enclosing `with`** (the common
  case; `pushCapturedWiths` early-returns on null, vm.cc:2485). Move it to a side table
  keyed by thunk*, consulted only when `desc.nWithTargets > 0`.

Result: header 40 B → 24 B. With the 16 B alloc granularity this is a clean **−16 B per
thunk** at every upvalue count (k=0/1: 48→32; k=2: 64→48). Sizing:

| workload | live thunks | live save | churn save (peak ≈ cumulative) |
|---|---|---|---|
| firefox | 1.0 M | ~16 MB | ~33 MB (2.05 M × 16 B) |
| **M5** | **4.9 M** | **~78 MB** | **~272 MB** (17 M × 16 B) |

**This is the M5 lever the team couldn't find** (M5 is thunk-dominated: 928 MB of its
arena is thunks). It is independent of, and stacks with, materialize L1 and capture-
sharing. It does NOT touch the upvalues the H-probe measured — it removes two
redundant/usually-null header words from *every* thunk.

## 5. The honest strategic fork

**v3's memory excess is the price of the closure-converted register-VM architecture
that wins CPU.** Closing it splits cleanly by workload:

- **M5 / thunk-heavy (the cardano class):** stackable representation shrinks WIN.
  Thunk header 40→24 B (−78 MB live / −272 MB churn) is the headline; it alone moves M5
  materially. Lower-risk, ~1 week, drv-hash-critical (byte-identity gate).
- **firefox / drv class:** Layer-A shrinks stack to ~60–90 MB (L1 −33 + header −16/−33 +
  pair tax), which gets firefox from 2.38× toward ~1.6–1.8× but **does not reach TW**,
  because 224 MB (44 %) of firefox's peak is *dead*, and no representation shrink touches
  dead. The only thing that beats TW on firefox RSS is **reclaiming that dead mid-eval —
  which is precisely what TW architecturally cannot do.** That is the generational /
  moving-nursery GC (workstream E's prerequisite): multi-week, but it is the one
  investment that converts v3's current memory *weakness* into a structural *advantage*
  TW can never match. Today's non-generational mark makes mid-eval GC too expensive; a
  nursery makes the common case (young garbage) cheap to reclaim without re-marking the
  live old set.

**Recommendation.** (1) Stop gating representation levers against a single −80 MB bar;
adopt a stacking bar (cumulative ≥X across a release) — and un-gate L1 (it is a measured,
byte-identical, CPU-neutral −33 MB already in the tree). (2) Ship the thunk-header shrink
(40→24 B) — biggest single win, fixes M5, sized and code-verified here. (3) Treat the
generational nursery as the *strategic* firefox lever, scoped honestly as multi-week, and
the only path to beating TW where 44 % of the peak is dead. Stop searching for a fourth
cheap firefox lever — the measurements say there isn't one.

## 6. One-line answer
v3 uses more memory because it closure-converts laziness into fat, individually-heap-
allocated thunks (40 B header + flat-copied captures) where TW uses 16 B `{Env*,Expr*}`
sharing one Env per scope — so v3's live set is ~1.4× and its churn is larger — and
because *neither* engine reclaims at these sizes, but v3 allocates more garbage. v3 can
beat TW only by (a) stacking the per-object shrinks the single-lever bar has been
rejecting, and (b) reclaiming mid-eval via a generational GC — the one move TW's
architecture forbids.

*Measurements 2026-06-14 (laptop, HEAD d9bd96a05, byte-identical). Object layouts:
closure.hh Thunk/ValuePair, alloc.hh Bindings::Entry. Cross-refs: SESSION_2026-06-13_14_INDEX,
BROADER_A_SCOPING_2026-06-14, E_DEPTH0_VERDICT_2026-06-14.*
*Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.
SPDX-License-Identifier: Apache-2.0.*
