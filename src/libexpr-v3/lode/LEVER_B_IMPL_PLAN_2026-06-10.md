<!--
Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.
SPDX-License-Identifier: Apache-2.0
-->
# Lever B — `Value` 16→8B (pointer tagging): GO decision + staged implementation plan (2026-06-10)

**Status: GO (projection clears the pre-committed gate).** Design home:
`MEMORY_REPRESENTATION_2026-06-07.md` §7. Reality-check + composition that justify it:
`LIST_ITERATION_PERF_2026-06-08.md` ("Real-workload reality check → Lever B framing").
This doc is the *execution* plan. Lever B is **representation-wide / multi-week** — it is
staged behind a compile toggle with a per-stage gate; do **not** attempt it as one edit.

## Why (framed/gated on Bindings, not ValuePair)

The list-iteration arc shipped T1/T2/T4/Stage-2 (foldl −23%/−7.5%, filter −48%/−259MB) and
*killed* the TLS lever (a profiling artifact) — foldl is near the interpreter ceiling. The
one remaining big lever is **memory**, and the composition says it's a **`Value`-width**
problem, not a niche ValuePair one:

| eval | thunks | bindings | pairs | lists |
|---|--:|--:|--:|--:|
| attrset-of-attrsets ×100k | 47% | **33%** | 14% | 3.5% |
| REAL hello.drvPath (audit 2026-05-21) | 31% | **52%** | minor | — |

**Bindings + Thunks dominate; ValuePair is a minority.** A `Value` is 16B; every big bucket
is *Value-bearing*:
- `Bindings::Entry { SymbolId(4) + PosIdx32(4) + Value(16) } = 24B` → **16B (−33%)**
- `ValuePair` 64→**32** (−50%) — this *is* T3, as a by-product
- `ListVec.elems[]` 16→**8** per elem (−50%)
- `Thunk` / closure captured upvalues: the `Value` field(s) halve

Applying these to the measured composition ⇒ **≈ −28% arena** — clears the gate.

## Pre-committed SHIP gate (do not loosen post-hoc)

- **≥ 20 % peak-RSS reduction** on a Bindings-heavy eval (and firefox.drvPath once #455
  unblocks it — see below), measured on darwin-4.
- **byte-identical** results on the cutover-parity corpus + `--core` + lang + property.
- **scaling guard green**; **wall ≤ 5 % regression** (scalars stay inline; one mask/shift
  per access, comparable to today's `tag_payload & 0xFF`).
- **REVERT if < 10 %** (the pervasive-refactor risk isn't justified). Projection says −28%,
  so the expectation is GO — but the *ship* decision is the measured number.

## The hard constraint (verified)

Nix `int` is **64-bit** (`NixInt = int64_t`); 8 bytes can't hold a full 64-bit int *and* a
tag. Choose: **61-bit inline immediates + boxed overflow** (range-check on construction, box
the rare large int into a heap cell) — keeps the register-VM's inline-scalar wall design;
this is the recommended path. (NaN-boxing is the alternative; heavier to audit on aarch64.)
Tag lives in the low 3 alignment bits of the 8B word; heap pointers are 16B-aligned so the
low bits are free.

## Staged plan (each stage independently committable; gate before advancing)

- **L0 — accessor abstraction (no layout change; byte-identical).** Route every direct
  `v.payload.X` read/write through methods (`v.asClosure()`, `v.asBindings()`, `v.setInt()`,
  …) so the layout flip is localized to `value.hh` + the accessors. Large but mechanical.
  `tag()` is already a method. Gate: `--core`/lang green + byte-identical (pure refactor).
  *This is the de-risking foundation; the rest is cheap once it lands.*
- **L1 — tagged 8B `Value` behind `V3_VALUE_8B` (default OFF).** Implement encode/decode +
  the accessors for the 8B layout incl. the 61-bit-int box/unbox + range check. Build green
  under BOTH toggles. Unit-test the encoding in isolation (every tag round-trips; int
  overflow boxes/unboxes; pointer tags). The crux — land + test it standalone first.
- **L2 — GC + serialize + FFI + register-VM under the toggle.** `tagIsPointer` / the precise
  root walk, `mark_sweep`/`live_trace` visitors, `value_serialize`, the FFI Value↔TW
  marshalling, and the register-VM ops (`OP_R_PRIMOP2` etc. read/write Values) all learn the
  8B encoding. Also rework the **App-memo + App3** in the now-32B `ValuePair`: `evaluated`
  (load-bearing H3 memo, vm.cc:12243) + `third` (App3 arg2) become tagged slots — design so
  the memo survives (a 32B pair still has room for left+right+memo via tagging, or a
  side-cell for the rare memoized-shared App).
- **L3 — validate + measure (the SHIP gate).** Under `V3_VALUE_8B`: byte-identical corpus +
  `--core`/lang/property/scaling green; peak-RSS A/B on the Bindings-heavy eval (+ firefox)
  → must clear ≥20%; wall ≤5%.
- **L4 — flip default-on + retire.** If the gate is met, default-on with an opt-out valve,
  soak on cutover-parity + M5/HNE, then retire the toggle + the 16B path. Closes T3 + T6.

## L1 encoding — RESOLVED design (2026-06-10): NaN-boxing

**Decision: NaN-box, not low-bit tagging.** The pointer kinds a `Value` holds have
*mixed* alignment — arena cells (Bindings/ListVec/Closure/Thunk/ValuePair/heap Values)
are 16-aligned, but **`PrimOp*` is 8-aligned** (points into an `unordered_map<string,PrimOp>`
node, primops.cc:113) and `char*` (allocChars, String/Path) has its own alignment — so a
uniform "tag in the low 3-4 bits" scheme would corrupt `PrimOp*`/`char*`. NaN-boxing puts
the tag in the **high** bits and the full pointer in the low 48, requiring **no** pointer-bit
alignment → handles all kinds uniformly.

**Layout of the 8-byte word `w`:**
- **Float (Tag::Float):** the raw IEEE-754 `double` bits, *except* the reserved boxed-NaN
  region. A Nix-produced NaN is canonicalised to one reserved quiet-NaN pattern that decodes
  back to Float (so float NaN never collides with a boxed value). `±inf` is a normal double
  (exp=0x7FF, **mantissa==0**) → distinct from boxed values (which set mantissa tag bits).
- **Boxed (everything else):** exp bits [52..62] = `0x7FF`; **tag = sign bit [63] ‖ bits
  [48..51]** = **5 bits → 32 tag values** (fits all 17 Tags with room); **payload = bits
  [0..47]** = a 48-bit pointer OR a 48-bit signed immediate. Mantissa [0..51] is always
  nonzero for boxed values (the tag bits guarantee NaN, not inf).
- **Int (Tag::Int):** 48-bit **inline** signed immediate (covers ±1.4e14 — the vast majority
  of Nix ints: counts, sizes, small numbers). Ints outside 48-bit **box** into a heap int
  cell (rare; range-check on `mkInt`). Keeps the register-VM's hot inline-int reads fast.
- **Pointers** (Attrs/List/Closure/Thunk/PrimOpApp/App/App3/Slot/String/Path/PrimOp/External):
  full 48-bit pointer in [0..47], each kind its **own tag value** → `tag()==App` / `App3` /
  `Attrs` stay a cheap mask+compare, **no cell-header deref** (preserves dispatch hotness;
  this is why we spend the 5 tag bits rather than collapse pointers into a "heap" tag + the
  arena CellType — App vs App3 share a ValuePair cell and are checked on the hot force path).
- **Constants** (Bool true/false, Null, Uninitialized, Blackhole): distinct tag values
  (payload unused / 0/1 for the bool).

**Cost:** `tag()` becomes "is-exp-0x7FF-and-mantissa-tagged? → extract sign‖[48..51] : Float",
a handful of ops — comparable to today's `tag_payload & 0xFF` (design §7). `asInt` =
sign-extend [0..47] (+ a boxed-int branch); `asPtr` = `w & 0xFFFF'FFFF'FFFF` (mask to 48-bit).
48-bit pointer assumes the aarch64/x86-64 user-canonical 48-bit address space (true on the
targets; assert in the encoder).

**Open micro-decision (measure in L1 spike):** whether masking to 48-bit is enough or we need
to also restore high bits for any non-canonical pointer (none expected on darwin-aarch64 /
linux-x86-64). The L1 isolated unit test round-trips every Tag + 48-bit int (incl. box
overflow) + a sample pointer per kind + float (incl. ±0, ±inf, NaN) to validate before any
VM wiring.

## Dependencies / notes

- **#455 fixpoint loop blocks the *firefox/drvPath* measurement** under pure v3-direct
  (separate hard problem; see LIST_ITERATION_PERF "(1)"). For the L3 RSS gate, measure on a
  Bindings-heavy eval v3 *completes* (attrset-of-attrsets / a real module/overlay eval); use
  firefox only if/when #455 is unblocked or via a structure-only path.
- **Complement to Lever A, not a replacement** — A removed Bindings *copies* (chain
  composition); B shrinks each *entry* (24→16). They stack.
- **`tagIsPointer` already exists** (precise-root work) — its semantics must match the new
  encoding exactly, or the GC mis-walks roots.
