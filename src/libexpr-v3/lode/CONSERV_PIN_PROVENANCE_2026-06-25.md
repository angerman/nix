# Conservative C-stack pin PROVENANCE — firefox.drvPath (S1.2)

**Date:** 2026-06-25  **Branch:** angerman/2.35-eval-profiling-v2
**Instrument:** `mark_sweep.cc` — per-CellType counter in `markConservative`
(`v3 conserv-provenance:` line) + `backtrace` at the mid-eval safepoint
(`NIX_V3_PIN_FRAMES`, `[pin-frames]` lines). Both gated on `NIX_VM_STATS` /
`NIX_V3_PIN_FRAMES`; production pays zero. Counts are deterministic +
host-independent (cell counts), so measured on the laptop.

## Why

The safepoint-foundation RSS path needs the conservative C-stack scan REMOVED so
mid-eval-compacted blocks can actually free (S2.3 proved Bartlett can't realize
RSS while scattered C-stack pins keep blocks mapped). To remove the scan we must
migrate the C++-local Values that the scan currently pins to precise handles
(`GcRoot`/`GcRootRange`/`GcRootVec`). TWICE the obvious targets were measured
unchanged — primop `args[]` (S1.2 start) and the 6 list-fold primops
(filter/partition/concatMap/groupBy/foldl′/__foldlMap). Guessing from a static
census kept missing. This instrument localizes firefox's ACTUAL pins.

## Result — peak mid-eval sweep (`conservativeOnly=357805`, the "12.5%" headline)

```
v3 conserv-provenance: None=0 Value=0 Closure=12 Thunk=40
                       Bindings=38562 List=127 Pair=41125 Env=13 Chars=84977
```

| Type | Direct C-stack ROOT pins | Share of pins |
|---|---:|---:|
| **Chars** (string/path buffers) | 84,977 | **51.5%** |
| **Pair** (App / App3 / PrimOpApp) | 41,125 | **24.9%** |
| **Bindings** (attrsets) | 38,562 | **23.4%** |
| List | 127 | 0.08% |
| Thunk / Closure / Env / Value | 65 | 0.04% |

## Two hard kills (Rule 0)

1. **"conservativeOnly == the unmovable C-stack pin set" — FALSIFIED.** The per-type
   ROOT pins sum to **164,856** out of (preciseMarked 2,512,865 + conservativeOnly
   357,805) = 2,870,670 live cells = **5.7%**, NOT 12.5%. The instrument counts only
   the cells `markConservative.tryMark` marks NEW (after the precise mark), i.e. the
   cells DIRECTLY referenced by a C-stack word. The remaining ~192,949 of the
   "12.5%" are the remembered-set walk + the transitive typed-closure of the pins —
   those are PRECISE / rewritable, not C-stack-pinned. The true unmovable-root
   fraction is ~5.7%.

2. **"the list-fold primops are firefox's pin source" — RE-CONFIRMED FALSE (3rd time).**
   `List = 127` pins total (0.08%). filter/partition/concatMap/groupBy/foldl′ build
   List cells; firefox barely pins any. Correct + complete migrations, but not
   firefox's RSS lever.

## Localized targets (type → code path)

Chars (51.5%) + Bindings (23.4%) → **derivation construction**; Pair (24.9%) → the
**App-spine** held in re-entrant frames. firefox.drvPath is derivation-heavy. The
FRAME instrument (below) shows the dominant on-stack source is
`primDerivationFromPreprocessed` (primops.cc:5357, the FFI leaf) +
`primDerivCoerce`; the static Explore audit (which ranked `primDerivationStrictNative`
5605-5984 #1) maps the re-entrant locals across the whole derivation family:

- **Tier 1 `primDerivationStrictNative`** (11 re-entrant-held locals): `drv`,
  `context` (hold string data), `attrCopy`/`attrV` (Value, across forces),
  `v3CachedOut` (Value), `structuredJson`, `declaredOutputs` (vec), the
  `outputHash*` strings. Loops over ALL derivation attrs forcing each → pins
  Chars+Bindings ~N× per call.
- **Tier 2 `v3CoerceToString`** (string/FFI bridge): `Value& v`, `const char* buf`
  (= `v.asString()` — INTERIOR pointer into a v3 Chars cell → a Chars pin),
  `out`(accum), `el`, `forced`. Every derivation-attr coercion goes through here.
- **Tier 3** listToAttrs / removeAttrs (Value-in-vec + result Bindings*),
  v3TryAttrsToString (3 Values across callClosure).
- **mergeBindings** (vm.cc OP_ATTRS_UPDATE): holds only `Bindings*` C-locals
  (`a`/`b`/`out`) across `materialize()` — pins Bindings *cells*, but via raw
  `Bindings*`, NOT a `Value`.

## Frame confirmation (`NIX_V3_PIN_FRAMES`, firefox, 8-dump cap)

`backtrace` at each mid-eval safepoint — the on-stack frames ARE the suspended
re-entrant primops whose C-locals are being pinned. Distinct primop symbols
(occurrences across the dumps):

```
55 forceValue        # the re-entrant callback the locals are held across
26 callClosure
16 primDerivationFromPreprocessed   # <-- DOMINANT derivation frame (FFI leaf)
 8 primFilter
 4 primDerivCoerce    # the string-coercion bridge (Chars source)
 2 primDerivationStrictNative
 2 primDerivationStrict
 1 primConcatMap
```

**This caught a primop the static census missed.** The Explore audit ranked
`primDerivationStrictNative` #1, but the frame instrument shows the actual
dominant on-stack derivation frame is **`primDerivationFromPreprocessed`**
(primops.cc:5357 — the `__derivationFromPreprocessed` bytecode-wrapper FFI leaf:
reads the env attrset of strings, builds the `Derivation`, coerces every string
field) with **`primDerivCoerce`** doing the coercion. So the confirmed firefox
migration target is the **derivation FFI-leaf path** (`primDerivationFromPreprocessed`
+ `primDerivCoerce`), not just `*StrictNative`. The 3rd guess-kill is also visible
here: `primFilter`(8)/`primConcatMap`(1) appear on-stack but contribute only 127
List pins — on-stack ≠ pinning-many-cells; the TYPE+FRAME instruments together
are what pin the target.

## Toolkit gap surfaced

`GcRoot`/`GcRootRange`/`GcRootVec` root `Value&` only. Re-entrant primops also hold
raw **`Bindings*` / `ListVec*` / `Closure*`** C-locals (mergeBindings `out`,
derivation result builders). Removing the conservative scan needs a cell-pointer
handle — a `GcRootCell<T>` that holds a `T*&` and calls `visitBindings/visitList/…`
so the moving GC rewrites it. The interior `const char* buf` case (v3CoerceToString)
is handled by Rule 2 (root the owning `Value`, re-extract `buf` after each callback)
— never root an interior pointer directly.

## Path

S1.2 next = (1) extend the toolkit with `GcRootCell<Bindings/ListVec/Closure>`;
(2) migrate the derivation FFI-leaf path **`primDerivationFromPreprocessed`
(primops.cc:5357)** + **`primDerivCoerce`** + `v3CoerceToString` (Rule 1 root held
Values, Rule 2 re-extract `buf`/`asString()` after forces, Rule 4 GcRootVec the
accumulators), then `primDerivationStrictNative`; (3) re-measure conserv-provenance
→ Chars+Bindings must drop; loop until the C-stack pin set → ~0; (4) then remove the
conservative scan (S2.1b clean compactor) + auditor-validate. The frame instrument
(`NIX_V3_PIN_FRAMES`) confirms the on-stack primop chain at each safepoint BEFORE
migrating, so we stop guessing.

## ★ DECISIVE REDIRECT (2026-06-26) — the pins are STALE, not live; the SCAN is removable

Rooting `toStringCoerceCtx`'s live Value locals (GcRoot v/el/tsFn/res/forced + Rule-2
re-read of `v.asList()`; byte-id vs TW ✓) left conserv-provenance UNCHANGED
(Chars 84977→84977, Bindings 38560, Pair 41125). That is the **4th** measure-first
redirect (args[] → list primops → toStringCoerceCtx all leave the count flat), and
the counts are STABLE to the cell across every mid-eval sweep — a pattern that
contradicts "transient live re-entrant locals."

**Decisive test (NIX_V3_NO_CONSERV_SCAN): DISABLE the conservative C-stack scan under
the mid-eval mark-sweep and check byte-identity.** If the pins were LIVE-but-precise-
missed, dropping the scan sweeps a live cell → UAF/divergence. If STALE/DEAD, byte-id
survives. **RESULT: byte-identical on hello + git + gcc + firefox, AND brute-audit
17/17 with MIDEVAL_GC=1 + NO_CONSERV_SCAN=1 under 1 MB-nursery stress (heavy synthetic
folds fold-genlist-100k / tail-1000 / deep-let-rec-fix with tight reuse + the nixpkgs
derivations) — ZERO corruption.**

→ The ~85k Chars + 41k Pair + 38.5k Bindings conservativeOnly pins are **STALE stack
slots pinning DEAD cells** (dead locals in long-lived frames — the dispatch loop, FFI
frames — never overwritten, within [sp, stackHi]). The PRECISE roots (value stack +
frames + globals + handles + remembered set) ALREADY cover the live set for the
non-moving mid-eval sweep. **The conservative scan is REDUNDANT for liveness — it only
over-pins dead cells, and those pins are exactly what blocked whole-block-free (the
S2.3 negative).**

**This OVERTURNS the S1.2 premise** that handle migration is the path to remove the
conservative scan: there is nothing live to migrate (4 redirects prove it). The
scan can be dropped directly for the non-moving mid-eval sweep. **It re-opens S2.1b on
a concrete basis:** a mid-eval MOVING compactor on PRECISE roots only (no conservative
pins) → everything movable → compact + free emptied blocks + munmap → the RSS win.
CAVEAT for the moving case: the non-moving no-scan test proves "no live cell was swept";
a moving compactor is stricter (a live-only-via-C-stack cell would DANGLE on relocation,
not just on reuse) — but if no such cell exists (4 derivations + 17 brute cases agree),
moving is also safe. SHIP-grade proof still requires the moving compactor built +
auditor-zero-missed under relocation. Strong GO, not yet a ship.

NEW critical path (supersedes the handle-migration plan above): **S2.1b = mid-eval
moving compactor on precise roots, conservative scan OFF** (validated by
NIX_V3_NO_CONSERV_SCAN as the oracle) → S2.2 whole-block-free + munmap → S2.3 re-measure
RSS (must beat default-v3). The GcRoot handle toolkit + the 6 list-primop + the
toStringCoerceCtx migrations stand as correct Rule-2 hardening for the moving path
(a cached cell-ptr across a callback dangles under relocation) but are NOT the RSS
lever — removing the scan is.

**Honest scope:** the derivation leaf path is FFI-heavy (nix::Derivation /
NixStringContext / store ops), hundreds of lines across `primDerivationFromPreprocessed`
+ `primDerivCoerce` + `primDerivationStrict{,Native}`. This is the genuine firefox
RSS lever but a substantial, careful migration — multi-day, not a `continue!` cycle —
and only the FIRST of several construction/bridge sites. The foundation remains
multi-week.

Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.
SPDX-License-Identifier: Apache-2.0
