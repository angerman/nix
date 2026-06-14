# Scoping: broader lookup-without-materialize — and the pivot (2026-06-14)

Goal: scope extending the chain-SELECT L1 lever (lookup-without-materialize) to
the OTHER materialize consumers for the bigger firefox RSS win — or pick another
direction. **Outcome: broader-A is NOT the firefox RSS lever; pivot to the eval
heap (workstream H / E).** All numbers from `V3_DBG_MAT_SITES` (per-caller
materialize-volume attribution, added this session in `Bindings::materialize`)
on darwin laptop, L1 off, high GC threshold (so `s_matMemo` isn't cleared →
clean per-chain attribution).

## The measurement that decides it

**Total materialize flat-copy volume — the entire removable ceiling:**
- firefox.drvPath: **71.9 MB** over 14061 copies, 10 distinct callers
- hello.drvPath:   **39.5 MB** over 3478 copies

firefox attribution (MB / copies):
| consumer | MB | copies | removability |
|---|---|---|---|
| `mergeBindings`+1032 (deep-chain cap-flatten) | 36.1 | 833 | flattens chains > kMaxLayers=8 to bound lookup — removing it RAISES the cap → O(depth) lookups (a different CPU/RSS trade, not lookup-without-materialize) |
| `dispatchLoop`+13640 (OP_ATTRS_SELECT) | 23.5 | 7689 | **already addressed by L1** (−33.5 MB measured incl. secondary) |
| `primMapAttrs` | 6.4 | 44 | read-only-ish; chain-aware map would save the input copy (small) |
| `mergeBindings`+1060 | 1.3 | 504 | cap-flatten (as above) |
| `primDerivationFromPreprocessed` | 1.2 | 1874 | the FFI drv leaf |
| `primRemoveAttrs` | 1.1 | 2839 | builds a new attrset anyway |
| rest | ~2 | — | negligible |

## Why broader-A cannot reach the −80 MB bar

The §3 bar (firefox peak RSS −≥80 MB) rests on the "merge+materialize ≈ 264 MB
firefox" figure. **That figure conflated two different things:** the chain
*overlay* Bindings (the `a // b` chains themselves — which are Lever A's RSS WIN
over flat merge copies, and are NOT removable; removing them = reverting Lever A)
and the materialize *flat copies* (the only thing lookup-without-materialize can
remove). The flat-copy volume is **72 MB**, not 264 MB. So:

- The total removable ceiling is **72 MB** (firefox), and that's optimistic
  (some copies are transient, not live at peak; L1 measured −33.5 MB live).
- L1 (SELECT) already took the biggest cleanly-removable slice (~33 MB).
- The remaining ~38 MB is dominated by `mergeBindings` cap-flattening (37 MB),
  which is a CPU/RSS *trade* (raise kMaxLayers → deeper chains → slower lookup),
  not a free win — plus `mapAttrs` (6 MB) and small consumers.

**A lever whose entire ceiling is 72 MB cannot clear an 80 MB bar.** Broader-A
is retired as the firefox RSS lever (Rule 0: kills "the firefox materialize
volume is ~230 MB and removable").

## Where firefox RSS actually is → the pivot

firefox arena ≈ 503 MB (laptop) / 687 MB (darwin-4); materialize is ~14 % of it.
The bulk is the **live eval heap** (§0.1: 262 MB live + 224 MB scattered dead;
the GC mark-split counted ~1.0 M thunks + ~1.0 M pairs + 0.66 M cells + 1.0 M
chars among 3.94 M marked cells). The two real RSS levers:

1. **Workstream H — shared capture frames (the representation tax).** v3
   flat-captures upvalues per thunk (~57 B avg); TW shares an Env chain across
   sibling thunks. The M5 decomposition put this at 928 MB of M5's arena;
   firefox's ~1 M thunks + ~1 M pairs make it a large share here too. **Highest-
   ROI RSS lever.** Measure-twice first step (cheap, ~150 LoC): a gated
   captured-upvalue-duplication histogram across sibling thunks (NIX_V3_*_ATTR
   style) to size the recoverable fraction before committing to the write-barrier
   /shared-frame work.
2. **Workstream E — GC reclaim of the 224 MB scattered dead.** Blocked twice
   over: the mark is cache-bound (§1.8, not lookup-bound) and depth>0 GC is
   multi-week (E stage-1+). Lower near-term ROI than H.

**Recommendation: pivot to H, starting with the upvalue-duplication probe.**
broader-A stays retired (≤72 MB ceiling, mostly a cap trade). L1 remains the
shipped, gated, validated chain-SELECT machinery (its standing value is the C-1
falsification, not RSS).

## H sizing result (2026-06-14, V3_DBG_UPVAL_DUP, commit f9dd326b5)

The upvalue-duplication probe ran — and **H is NOT the firefox RSS lever either**:

| workload | thunks-w/up | upvalue-tail bytes | recoverable (identical-tuple dedup) |
|---|---|---|---|
| hello.drvPath | 445,673 | 7.1 MB | 2.7 MB (38.6%) |
| firefox.drvPath | 1,880,010 | 30.0 MB | **12.1 MB** (40.2%) |

Recoverable UPPER bound = the *total* upvalue-tail volume = **30 MB firefox**
(realistically 12 MB via identical-tuple dedup; broader subset-Env sharing can't
exceed 30 MB). Far below the −80 MB bar. Reason: the thunk RSS is dominated by the
per-thunk **HEADER** (40 B × 1.88 M ≈ **75 MB firefox** / ~680 MB M5; already
minimised to 40 B per the M-8 review — `state`+`nUpvalues`+`forces`+`cell`+ the
24 B suspended union), which capture-frame sharing does NOT touch. Kills "the
thunk representation tax is mostly recoverable upvalues."

M5 (H's original motivation — 17 M thunks) **could not be sized under the probe**:
the cardano eval now exceeds 80 s wall (cold eval-cache) and the per-thunk map
over 17 M entries compounds it; the one completed run reached only getFlake's
flake-input loading (1431 thunks) — INVALID. A valid M5 sizing needs a
sampling/streaming probe variant — a follow-up only if cardano RSS is prioritised.

**Net verdict.** No cheap RSS lever remains for firefox: broader-A ≤72 MB (mostly
a cap trade), H ≤30 MB (header-dominated tax it can't touch). firefox RSS is the
live eval heap — the 224 MB scattered dead (workstream E / GC reclaim, mark
cache-bound §1.8 + multi-week) plus the 40 B/thunk header (a deeper representation
change). Honest options: E (multi-week) or a thunk-header shrink investigation,
NOT capture sharing.

## Instruments left in place (gated, default-off)
- `V3_DBG_MAT_SITES=1` — per-caller materialize-volume attribution
  (`__builtin_return_address`+`dladdr`, atexit). Reusable on any workload.
- `V3_DBG_UPVAL_DUP=1` — per-thunk capture-tuple dedup sizing (the H probe).
  Both retire when their machinery is reworked.

*Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output
Group. SPDX-License-Identifier: Apache-2.0.*
