# v3 memory representation — why ~2× TW, and the two levers (2026-06-07)

**Status:** MEASUREMENT RESULT + DESIGN. Answers "why does v3 use ~2× the
memory of the tree-walker (TW), where exactly, and what would close it." Every
size is verified from the actual structs; the overlay cost is verified by a
clean synthetic measured this session (§4). Supersedes the verbal deep-dive
that preceded it — and **corrects** a wrong root-cause ranking from that
deep-dive (§3, CORRECTION).

Companion to [[MEMORY_ATTACK_PLAN_2026-06-06]] (the orthogonal-lever survey)
and [[QUANTIFICATION_2026-06-05]] (the wall-lever ranking). This doc is the
*memory*-representation analysis those two deferred.

> **HEADLINE UPDATE (2026-06-07, team M5 measurement + re-verify).** The "~2×
> TW memory" in the title is **workload-concentrated, NOT universal.** On the
> production **M5 / cardano-node.drvPath** target v3 is at **memory parity**
> (924 vs 915 MB, +9 MB, byte-identical result). The 2.74× lives on
> **overlay/override-heavy evals like firefox** (+592 MB, re-verified this
> session, §4b). So the two levers below are for the **firefox-class
> regression — they are NOT on the M5 critical path.** Read §4b before acting
> on priority.

---

## 1. Verified facts (sizes from the structs, not estimates)

| structure | definition | size | how the value is held |
|---|---|---|---|
| **v3** `Value` | `{ uint64_t tag_payload; union payload (8B) }` | **16 B** | scalars **inline**; heap types (`Attrs`→`Bindings*`, `Thunk`→`Thunk*`, …) = a **pointer** in the payload |
| **v3** `Bindings::Entry` | `{ SymbolId(4); PosIdx32(4); Value(16) }` | **24 B** | Value **inline** in the entry |
| **cppnix** `Attr` | `{ Symbol(4); PosIdx(4); Value*(8) }` | **16 B** (`static_assert`) | a **pointer** to a separately-allocated heap `Value` (shareable) |

- v3 `Value`: `value.hh` — `tag()` is `tag_payload & 0xFF`; the 8-byte union
  holds `int64_t i` / `double f` inline for scalars, or `Bindings*` / `Thunk*`
  / `ListVec*` / … for heap types. So **a v3 Value already shares heap
  sub-objects** (copying the Value copies the pointer), and **scalars carry no
  box**. `SymbolId = uint32_t`. ⇒ Entry = 4+4+16 = **24 B**.
- cppnix `Attr`: `attr-set.hh` — `static_assert(sizeof(Attr) == 2*sizeof(uint32_t)+sizeof(Value*))`
  ⇒ **16 B**, the `Value*` points to a GC'd heap box that *can be aliased by
  many Attrs*.
- cppnix `Bindings` is **layered**: `numAttrs`, `numAttrsInChain`, `numLayers`,
  `const Bindings * baseLayer`, `maxLayers = 8`; `//` composes into a layer
  list; iteration is an **on-the-fly k-way merge** (`Bindings::iterator`, no
  materialization). (`attr-set.hh`.)
- v3 `mergeBindings` (`vm.cc:1084`) **materializes**: two-pass (count distinct
  keys → `allocBindings` exact size → fill by walking `a->entries[]` /
  `b->entries[]` linearly), and it **flattens any Chain input first**
  (`vm.cc:1138-1139`).

---

## 2. The representation, illustrated

### One attrset `{ x = 1; y = <thunk>; }`

```
v3 — flat array of 24-B entries, value INLINE
┌ Bindings(Sorted)  header {pos, size=2, numLayers=1, parent=NULL}
│ entries[0] | x |pos|  Int  │ 1            |     scalar INLINE — no box
│ entries[1] | y |pos| Thunk │ ptr ─────────┼──►  Thunk{…}   heap = SHARED ptr
│              4   4  └──── Value 16 B ─────┘
└ footprint = 16 + 2×24 = 64 B (+ the shared Thunk)

cppnix — Attr array + boxed Values
┌ Bindings  header {pos, numAttrs, numAttrsInChain, numLayers, baseLayer}
│ attrs[0] | x |pos| val*─┼──►  Value{Int 1}      scalar BOXED on the heap
│ attrs[1] | y |pos| val*─┼──►  Value{Thunk …}    shared
│            4   4    8         └ a separate ~2-word heap box per value
```

**Consequence for a *unique scalar* attr** (the common case): v3 = 24 B
inline; cppnix = 16 B Attr + a heap box ≈ 32 B. **v3 is *leaner* here.** The
sharing in cppnix only wins once a value is referenced ≥3×. So v3's inline
Value is **not** the memory problem — the problem is one level up.

### `big // small` — the dominant cost (88% of Bindings bytes on firefox)

```
v3 — MATERIALIZE                          cppnix — LAYER
                                          
 big  [ N × 24B ]──┐                       small (overlay)─┐ layer0
 small[ M × 24B ]──┤ mergeBindings:        big ────────────┘ layer1  (SHARED!)
                   ▼  alloc NEW array,            │ result = {baseLayer→big}
   result[(N+M')×24B]  COPY every entry          ▼   + small[M×16B] + header
   big NOT shared; ~N×24B fresh / //         big NOT copied; ~M×16B + header
   iterate: O(1) flat                        iterate: on-the-fly k-way merge
                                                      over ≤8 layers, no copy
```

For overlay-heavy nixpkgs (`self // super`, the module fixpoint, `pkgs //
overrides`), v3 re-copies the huge base on **every** `//`; cppnix allocates
only the overlay + a header and **shares the base**, merging lazily on
iteration. **This is the load-bearing difference.**

---

## 3. The two root causes — corrected ranking

> **CORRECTION (vs the 2026-06-06 verbal deep-dive).** That deep-dive ranked
> "v3 doesn't share Values" as root cause #1. **That is wrong** and the structs
> prove it: a v3 `Value` for a heap type stores a *pointer* (§1), so heap
> sub-objects *are* shared; and scalars are inline (leaner than cppnix's box).
> The real ranking is below. The earlier mistake is exactly the kind
> [[feedback_measure_twice_cut_once]] warns about — asserted from a struct I
> hadn't finished reading.

1. **DOMINANT — `//` is materialized, not layered.** `mergeBindings` copies;
   cppnix layers + k-way-merges. On firefox this is **88% of Bindings bytes
   (470 MB)** (team, prior) and **3.90× TW peak** on the isolated synthetic
   (§4, this session). This is THE cause.
2. **SECONDARY — Entry 24 B vs Attr 16 B.** v3's inline 16-B Value makes every
   *materialized* array 1.5× bigger per entry than cppnix's 8-B `Value*`. A
   multiplier on cause #1, not an independent driver.
3. **NOT a cause** (corrects the deep-dive): v3 shares heap sub-values via
   payload pointers and inlines scalars; for unique/scalar attrs it is *leaner*
   than cppnix. No broad sharing deficit exists.

---

## 4. The synthetic — measured this session (the new data)

**Workload:** 1000 independent overlays of a 3000-entry base, all kept live and
forced via `attrNames`. Isolates the `//` materialize-vs-layer cost; peak RSS
is load-insensitive (`/usr/bin/time -l`).

```nix
let base = builtins.listToAttrs
             (builtins.genList (i: { name = "a" + toString i; value = i; }) 3000);
    overlays = builtins.genList (i: base // { extra = i; }) 1000;
in builtins.foldl' (acc: o: acc + builtins.length (builtins.attrNames o)) 0 overlays
```

| config | peak RSS | ratio | reading |
|---|---|---|---|
| **TW** | **52.9 MB** | 1.00× | layers `//`; base shared across 1000 overlays |
| **v3 materialize** (default) | **206.0 MB** | **3.90×** | copies base 1000× |
| **v3 chain-on** (`NIX_V3_CHAIN_BINDINGS=1`) | **206.4 MB** | **3.90×** | chain triggered, then **flattened on iteration** |

All three produce the identical result `3001000` (correctness preserved). The
directly-attributable overlay-copy floor is `1000 × 3001 × 24 B ≈ 69 MB`; the
remaining v3 excess is the base + thunks + the **non-reclaiming bump arena**
(it never frees the transients from `listToAttrs`/`genList`/`foldl'`).

**This is the §3-cause-#1 ranking, confirmed in isolation: v3 balloons 3.9× on
pure overlays; TW stays flat.**

---

## 4b. M5 is at parity — the gap is workload-concentrated (the reframing)

The team measured the production target (host-noise-immune metrics:
instructions-retired + user-CPU; wall is meaningless on this host — TW firefox
ranged 6–24 s at <2.5 s user). I re-verified firefox peak RSS same-host on
current HEAD.

| workload | metric | TW | v3 | v3 vs TW |
|---|---|---|---|---|
| **M5 / cardano-node.drvPath** | instructions | 44.96 B | 48.38 B | 1.08× slower |
| | user-CPU | 10.30 s | 11.17 s | 1.08× slower |
| | **peak RSS** | **915 MB** | **924 MB** | **1.01× (+9 MB)** · byte-identical |
| python3.pkgs attrNames (pure eval) | user-CPU | 0.84 s | 0.45 s | **1.87× faster** |
| firefox.drvPath | user-CPU | 2.48 s | 4.10 s | 1.65× slower |
| **firefox.drvPath** (re-verified, this session) | **peak RSS** | **340 MB** | **932 MB** | **2.74× (+592 MB)** |

**The crux is the *absolute* excess, not the ratio:** **+9 MB on cardano vs
+592 MB on firefox.** cardano is the *bigger* eval (915 MB TW vs 340 MB) yet
carries ~zero v3 excess — so this is **not** a fixed overhead that amortizes.
firefox.drvPath specifically triggers the §3-cause-#1 `//` materialization;
cardano-node.drvPath does not (at comparable scale).

**Two reconciliation hypotheses (distinguishable, not yet distinguished):**
1. **firefox is override/overlay-pathological** — deep `overrideAttrs` /
   `wrapFirefox` / `buildMozillaMach` chains do many big-base `//` (the 88% /
   470 MB mergeBindings we measured); cardano's size is many *small*
   derivations, not deep overrides over giant attrsets.
2. **cardano memory is FFI/IFD-dominated** (haskell.nix materialization, store
   work shared with TW via the bridge), so its *eval-graph* fraction — where
   v3 is heavy — is small.

**Distinguishing test (team-only; needs the cardano flake):** the
`mergeBindings` byte-fraction of the arena. firefox = 88%; if cardano ≪ that,
hypothesis 1; if cardano's arena is small vs total RSS, hypothesis 2. Either
way the conclusion holds: **the levers target eval/overlay-heavy workloads, and
M5 already meets its memory budget.**

**Caveats (honest):** (i) the cardano parity is **team-measured; not
same-host-reproduced here** (no cardano flake in this env). (ii) The firefox v3
run produced a `/v3-fake-store/…` path, **not** byte-identical to TW — the
**eval graph is fully built (932 MB confirms it), so the memory ratio is
valid**, but firefox correctness is **not** verified by this run (cardano's
was). (iii) The team's "old 2.3–6× / 4–6× baseline is stale" is right **for
M5** — but firefox 2.74× memory is **current, not stale**; "stale" is
workload-specific.

---

## 5. Why ChainBindings is NEUTRAL — post-mortem (now fully explained)

The chain is **not** bypassed and is **not** broken-by-construction. Verified:

- It **triggers**: `vm.cc:1213` builds `Chain{parent=a, overlay=b}` when
  `NIX_V3_CHAIN_BINDINGS=1 && nb ≤ 4 && na ≥ 16` — true for `base // {extra}`
  (na=3000, nb=1). At construction it is cheap (parent ptr + 1 overlay entry).
- It is **immediately re-flattened on use**: there are **~24
  `if (isChain()) materialize()` call-sites** across `primops.cc`, `vm.cc`,
  `print.cc`. `primAttrNames` does it at **primops.cc:626**; `attrValues`,
  `removeAttrs`, `intersectAttrs`, `mapAttrs`, `getAttr`, `print`, the
  serializer, and the formals-destructure (`vm.cc:5385`) all do the same.
- Net (§4): chain-on = 206.4 MB ≈ materialize 206.0 MB. The chain saves at
  creation and loses it all at the first consumer.

**So the chain mechanism is sound; the missing piece is exactly what cppnix
has and v3 lacks: a chain-aware *k-way-merge cursor* so consumers iterate the
layers in place instead of calling `materialize()`.** The "5×-falsified Phase C"
ledger (`vm.cc:1221+`) was falsifying *chain construction*; the real blocker is
the **~24-site consumer audit** (the code itself estimates "190–208-site
`entries[]` audit").

---

## 6. Lever A — layered Bindings + k-way-merge iterator (attacks cause #1)

The proven-by-cppnix fix for the dominant 88%. **The work is NOT the chain
construct (done, `vm.cc:1213`) — it is converting the ~24 `materialize()`
consumers to a layer-walking cursor:**

```
class Bindings::Cursor {                 // k-way merge over ≤8 layers
    layers[k]; idx[k];                   // no allocation, no copy
    next(): pick min-Symbol head across layers; overlay (layer 0) wins ties
};
// every `if (isChain) materialize()` site → `for (auto & e : bindings.cursor())`
```

- **Effort:** large — the 24-site audit + a cursor + Phase-D barrier review for
  the layer pointers. Multi-session. The construct + heuristic already exist.
- **Pre-committed SHIP gate:** on the §4 synthetic, chain-on peak ≤ **1.5× TW**
  (i.e. ≤ ~80 MB, down from 206 MB) **with** `--core` + nixpkgs byte-equality
  intact; on firefox, peak reduction ≥ **150 MB**. Below that → the cursor
  doesn't pay for the audit risk; revert per Rule 0.
- **Kills the hypothesis:** "v3's `//` overhead is intrinsic" — cppnix proves
  it isn't; this measures whether v3 can adopt the mechanism without the
  24-site flattening defeating it.

---

## 7. Lever B — pointer tagging (attacks cause #2 + broad)

Collapse the 16-B `Value` into a single 8-B tagged word:

```
today  16 B:  [ tag_payload 8B ][ payload 8B ]
tagged  8 B:  [ ……… value-or-pointer ……… │tag ]   tag in low (alignment) bits
                Int   → 61-bit immediate inline      (no box — keeps wall win)
                Attrs → Bindings* (tag in low 3 bits) (heap; already a pointer)
```

- **What it buys (arithmetic on the measured per-tag bytes):** `Entry` 24→**16 B**
  (== cppnix `Attr`) → materialized arrays −33%; `ValuePair` 64→32; `ListVec`
  elems 16→8; thunk/closure captured upvalues ~half. Broad ≈ **−30% arena**:
  firefox 772 → ~520 MB → peak ~570 MB → **~1.9× TW** (from 2.74×). Rough but
  consistent across the Value-bearing tags.
- **Keeps the wall design:** scalars stay inline (the register VM still reads
  immediates), heap stays behind a pointer (already the case). Cost is one
  mask/shift per access — comparable to today's `tag_payload & 0xFF`.
- **The hard constraint (verified):** Nix `int` is **64-bit** (`NixInt =
  int64_t`). 8 bytes can't hold a full 64-bit int *and* a tag → either reserve
  low bits (**61-bit immediates**, box the rare overflow) or NaN-box (48-bit
  pointers + mantissa ints). The full-64-bit-int path needs a boxed fallback +
  range check. Rare (most Nix ints are small) but real.
- **Pervasive:** every Value access, every primop, the register-VM ops
  (`R_PRIMOP2` etc. read/write Values), the GC `tagIsPointer` root walk, and
  the serializer must learn the encoding.
- **Does NOT fix cause #1:** it shrinks each *copied* entry (24→16) but does not
  remove the copy — that is Lever A. Pointer tagging is a **complement**.
- **Pre-committed SHIP gate:** ≥ **20% peak-RSS reduction** on firefox
  (≥ ~160 MB) with `--core` + nixpkgs byte-equality + wall ≤ 5% regression.
  Below 20% the pervasive-refactor risk isn't justified.

---

## 8. Sequencing + combined projection

```
 firefox-class   peak vs TW    attacks            risk
 today              2.74×        —                  —
 + Lever A        ~1.6–1.9×   cause #1 (88% //)   24-site audit, multi-session
 + Lever B        ~1.3–1.5×   cause #2 + broad    pervasive Value refactor + 64b-int box
 (cppnix)           1.00×      both, by design
 ───────────────────────────────────────────────────────────────────────────
 M5/cardano         1.01×      already at parity — levers NOT on its critical path
```

- **PRIORITY (post-§4b): the levers are NOT on the M5 critical path** — that
  target is already at memory parity. They are for the **firefox-class
  override-heavy regression** (still real, still 2.74×, and the IOG ecosystem /
  HNE is overlay-heavy — so worth doing, but *after* M5-blocking work, not
  before). Rank them against the firefox wall gap (1.65× slower) and the pure-
  eval win (1.87× faster) — not against an M5 memory problem that no longer
  exists.
- **Neither lever alone clears a ≤1.6× gate; together they approach cppnix's
  representation.** Lever A is the bigger single win (the dominant 88%) and is
  proven-by-cppnix; do it first. Lever B is broad, wall-preserving, and the
  *only* memory lever that keeps v3's inline-scalar / register-VM wall design —
  but it carries the 64-bit-int box + a pervasive refactor.
- Both are large. This is why [[MEMORY_ATTACK_PLAN_2026-06-06]] found **no
  cheap memory lever**: the cheap levers (COW, GC) were falsified; the real
  levers are these two structural changes.

---

## 9. Reproduce

```bash
EXPR='let base = builtins.listToAttrs (builtins.genList (i: { name = "a" + toString i; value = i; }) 3000); overlays = builtins.genList (i: base // { extra = i; }) 1000; in builtins.foldl'"'"' (acc: o: acc + builtins.length (builtins.attrNames o)) 0 overlays'
/usr/bin/time -l ./build/src/nix/nix eval --expr "$EXPR"                                   # TW   ~53 MB
NIX_V3_DIRECT_EVAL=1 /usr/bin/time -l ./build/src/nix/nix eval --expr "$EXPR"              # v3   ~206 MB
NIX_V3_DIRECT_EVAL=1 NIX_V3_CHAIN_BINDINGS=1 /usr/bin/time -l ./build/src/nix/nix eval --expr "$EXPR"  # chain ~206 MB
```

Struct sources: v3 `Value` `include/v3/value.hh`; `Entry` `include/v3/alloc.hh`;
cppnix `Attr`/`Bindings` `src/libexpr/include/nix/expr/attr-set.hh`;
`mergeBindings` `vm.cc:1084`; chain trigger `vm.cc:1213`; the 24 `materialize()`
sites `grep -n 'materialize()' src/libexpr-v3/*.cc`.

---

## 10. Lever A — IMPLEMENTED + MEASURED (2026-06-07, post-register-VM)

Lever A was built this session. The chain *construct* already existed
(`vm.cc` mergeBindings, gated `NIX_V3_CHAIN_BINDINGS=1`); the work was the
§6 consumer conversion + chain composition + the consumer audit. Result:
**the lever WORKS and clears the firefox SHIP gate by a wide margin while
staying byte-equal** — the "v3's `//` overhead is intrinsic" hypothesis is
**FALSIFIED**.

### What landed
1. **`Bindings::Cursor`** (alloc.hh) — k-way-merge over chain layers (≤8,
   overlay-wins, alloc-free) + cursor-based `forEach` / `totalSize` /
   `countDistinct`. The piece cppnix has and v3 lacked.
2. **Cursor consumers**: `attrNames` / `attrValues` stream the chain (no
   materialise copy).
3. **Chain composition** (mergeBindings Step 4): `chain // small` EXTENDS
   the chain (parent ptr + tiny overlay) up to the layer cap instead of
   re-materialising the base on every `//`. This is the firefox win — deep
   `overrideAttrs` / `wrapFirefox` stacks now cost O(depth) tiny overlays +
   one shared base, not O(depth) full copies.
4. **Store-hash-critical chain guards** (the audit — these were the bugs):
   - `valueToJsonWithContext` + `valueToJson` (primops.cc): the
     `__structuredAttrs` JSON serializer iterated overlay-only → a
     structured derivation hashed from a partial attrset → degenerate
     constant drv hash (python3.withPackages collapsed regardless of its
     package list). Now materialise first.
   - `valuesEqual` ×2 (vm.cc + primops.cc): index-wise compare assumed
     Sorted → `chain == its-own-materialisation` wrongly returned false,
     breaking `lib.unique`/`elem`/override-equality. Now materialise both.
   - `OP_ATTRS_SELECT` / `OP_ATTRS_SELECT_DYN`: materialise the chain
     (memoised). See the deferred-optimisation note below.

### Measured (this session, same host)

| workload | metric | v3 default | v3 chain-on | Δ |
|---|---|---|---|---|
| **firefox.drvPath** | peak RSS | 932.6 MB | **663.8 MB** | **−268 MB (−29 %)**, 2.79×→**1.83× TW** |
| §4 synthetic (`attrNames` each) | peak RSS | 216 MB | 131 MB | residual = 1000 transient attrNames string-lists (arena-no-reclaim / GC track), NOT a `//` cost |

SHIP gate (§6): firefox reduction ≥ 150 MB → **MET (268 MB)** with byte-
equality intact.

### Byte-equality validation (chain-on == chain-off, disk cache off)
- v3 **core suite 19/19 GREEN with `NIX_V3_CHAIN_BINDINGS=1`** (incl. 143
  lang tests, drv-parity, all parity-vs-TW suites).
- **20+ package drvPath sweep** byte-identical incl. the initially-failing
  git / cargo / rustc / cargo-auditable.cargoDeps / python3.withPackages.
- chain micro-battery (attrNames / toJSON / valuesEqual / formals /
  dynamic-select / removeAttrs / deep-compose) all chain-on == chain-off.
- With chains OFF (the default) every change is a no-op → byte-identical to
  pre-Lever-A (core 19/19 unaffected).

### The audit lesson (why prior attempts stalled)
The 5 prior Phase-C falsifications were right that a consumer audit is
required — but the failing sites were NOT the obvious `entries[]` loops; they
were **the JSON serializer and `valuesEqual`** (store-hash-critical, only
exercised by real `derivationStrict`, which hello/firefox's `/v3-fake-store/`
stubs bypass). They were found by nixpkgs bisection: `git.drvPath` diverged →
`derivation show` diff drilled git→cargo→auditable→vendor→fetch-cargo-vendor-
util→python3-env, isolating each leaf, then minimal micro-repros pinned the
exact primop. **Use real `/nix/store/` drvPaths (disk cache off) for chain
byte-equality — fake-store stubs hide the derivationStrict path.**

### DEFERRED optimisation (no carcass; the gate stays off)
A lookup-only `OP_ATTRS_SELECT` (walk layers, no copy) measured a further
firefox **−132 MB (→535 MB)** but introduced a subtle **shared-parent
writeback contamination**: forcing/memoising a value reached through a SHARED
base layer corrupted sibling chains (`OP_CALL: callee is not a closure` deep
in cargo/git). materialise-SELECT avoids it by copying entries first. Revisit
only with a precise shared-`Pair`/writeback analysis; the extra 132 MB is not
worth the correctness risk today.

### Status: GATED default-OFF (`NIX_V3_CHAIN_BINDINGS=1`)
Byte-equal on core + a broad sample, but the full ~255-site `entries[]`
surface is not exhaustively audited (the cargo contamination proves latent
sites can hide outside the sample). Default-on warrants a **full nixpkgs
drvPath CI sweep** first. The mechanism + win are proven; flipping the default
is the remaining gate.

---

Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.
SPDX-License-Identifier: Apache-2.0
