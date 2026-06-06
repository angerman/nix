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
            peak vs TW       attacks            risk
 today        2.74×            —                  —
 + Lever A    ~1.6–1.9×    cause #1 (88% //)   24-site audit, multi-session
 + Lever B    ~1.3–1.5×    cause #2 + broad    pervasive Value refactor + 64b-int box
 (cppnix)      1.00×       both, by design
```

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

Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.
SPDX-License-Identifier: Apache-2.0
