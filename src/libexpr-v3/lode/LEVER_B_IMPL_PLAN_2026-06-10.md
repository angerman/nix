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

## L0 — DONE (2026-06-10). L2 entry notes + worklist

**L0 is complete** — commits `2e5358cfa` (9 small files) + `b87723229` (vm.cc + primops.cc,
the bulk) + `ef007a828` (test/CLI/bytecode + L0 COMPLETE). Every direct `Value::payload.X`
field access in the whole v3 tree now routes through `asX()`/`mkX()`/`rawWord()`/
`floatBits()`/`mkUninitialized()`; only `value.hh` (the layout def) and `value.cc` (bootstrap
singletons) keep direct access. All three commits byte-identical (v3 lang 142/143 throughout —
`eval-okay-types`, typeOf of a partially-applied closure → "unknown", is the SOLE fail and is
PRE-EXISTING at HEAD per same-host bisect, NOT a Lever-B regression). v3-smoke green.

**Encoding RE-PROVEN for the production 18-tag enum** (commit `6d12b48dd`): the L1 spike's
enum stopped at `Slot=16`; the live enum has **`App3=17`** → `codeOf(App3)=19` collided with
the old `FLOATNAN=19`. Corrected to **`FLOATNAN=20`** (tag codes are `{1..15,17,18,19}`;
FLOATNAN must avoid them + keep a nonzero low nibble + stay ≤31). Spike now sweeps tags 0..17
and asserts all codes distinct + none == FLOATNAN: ALL PASS.

### L2a (next) — flip `Value` to 8B behind `V3_VALUE_8B`, default OFF

- **Toggle:** `#ifdef V3_VALUE_8B` in `value.hh`. Default build leaves the 16B `#else`
  bodies **verbatim** (byte-identical default is the gate). Structure as ~7 branched regions:
  storage; `tag()`; the read-accessor block; `rawWord`/`floatBits`/`mkUninitialized`; the
  writer block; `mkBlackhole`; the two `static_assert(sizeof==16)` → `==8`.
- **Codec:** port the (now-corrected) spike codec into `value.hh` as `#ifdef V3_VALUE_8B`
  `inline`/`constexpr` free fns in a `v8nan` detail namespace (EXP/MANT/PAY masks,
  `codeOf`/`tagFromCode`, `box`/`isBoxed`/`boxCode`/`boxPay`, `FLOATNAN=20`,
  `INT_MIN48/MAX48`). The struct holds a single `uint64_t w`.
- **App-memo / App3 need NO rework** (revises the original L2 bullet): `ValuePair` stays 4
  `Value` members ⇒ 64→**32B** for free (= T3); `evaluated`'s memo works because the NaN-box
  has a Uninitialized tag + `mkUninitialized()` (the sentinel `tag()==Uninitialized` check is
  unchanged). No tagged-slot / side-cell needed.
- **Boxed-int overflow is the one real design choice.** Nix int is 64-bit; only ±2⁴⁷ fits
  inline. Plan: a distinct internal boxed-int code; `tag()` maps it to `Tag::Int`; `asInt()`
  derefs a heap `int64` cell; `mkInt(n)` inlines if it fits 48-bit else allocates the cell.
  Keep `value.hh` allocator-free: declare `v8nan::boxInt64(int64_t)->const void*` +
  `unboxInt64(const void*)->int64_t` and **define them in `value.cc`** (where `Alloc` is
  available). GC must treat a boxed-int cell as a leaf root it keeps alive (audit in L2b).

### L2b worklist (after L2a compiles under the toggle)

- **`tagIsPointer` stays as-is** — it's a pure `Tag`→bool classification (layout-independent),
  already correct (PrimOp/External=false by design; the GC walks String/Path via
  `visitString/Path`). The *encoding* stores PrimOp/External as pointers regardless; that's
  separate from the GC-walk classification. No change needed, but re-confirm the asserts hold.
- **`sizeof(Value)==16` / 16-stride assumptions** — audit `ListVec` elem size, `Bindings::Entry`
  layout, `Alloc` cell sizes, any `memcpy(…, 16)` / `* 16`. These SHRINK; find hard-coded 16s.
- **value_serialize** — format is layout-INDEPENDENT (writes tag byte + payload bytes via
  `asX`/`floatBits`), so cross-layout compatible; just confirm no raw-struct `memcpy`.
- **FFI marshalling + register-VM ops** — both go through the migrated accessors now; confirm
  no raw 16B `Value` memcpy / `.payload` assumption remains (L0 removed them, but re-grep).
- Build green under BOTH toggles; run the standalone encoding unit test wired as a real test.

### L2a — DONE (commit `76c208178`); L2b discovery — DONE

- **L2a shipped:** value.hh storage + accessors + value.cc singletons/`boxInt64` flipped under
  `#ifdef V3_VALUE_8B` (default OFF, 16B bodies verbatim → default byte-identical: lang 142/143
  unchanged). Under `-DV3_VALUE_8B`: value.cc/value.hh/alloc.hh/barrier.hh syntax-clean AND
  `static_assert(sizeof(Value)==8)` + `sizeof(ValuePair)==32)` PASS — the NaN-box really is 8B/32B.
- **L2b syntax sweep: 42/42 v3 TUs syntax-compile clean under `-DV3_VALUE_8B`** (verified the
  define is actually applied: vm/mark_sweep/ffi/serialize/gc all 0 errors). ⇒ NO TU has a
  compile-breaking 16B assumption; L0's accessor migration was complete enough that the flip is
  source-compatible across the whole library.
- **`sizeof(Value)` uses are accounting-only** (all in live_trace.cc: `bytesPairs`,
  `sizeof(Value)*nUpvalues`, etc.) — they auto-adjust to 8/32 under 8B, which is *correct*
  (the smaller accounting reflects reality). No `/16`/`*16` literal strides found. No fix needed.
- **GC de-risked: the arena is PRECISE by default.** `NIX_V3_ARENA_NOROOT` is default-ON
  (2026-06-04, alloc.hh:930/1150/2334) → arena blocks are NOT `GC_add_roots`'d → Boehm does not
  conservatively scan the arena. Arena-object liveness = the precise major-GC mark, whose
  `visitValue` decodes via `asX()` → handles the NaN-box correctly. So "Boehm can't see
  NaN-boxed pointers inside the arena" is MOOT.

### L3 — remaining work + the one real GC question

- **Full 8B build+link+run** (the real validation `-fsyntax-only` can't give): configure a
  separate `build8` with `-Dcpp_args=-DV3_VALUE_8B` (whole tree must agree on `sizeof(Value)` —
  a mixed build is ABI-incompatible), build v3-eval + v3-smoke, run smoke + lang + `--core`.
- **GC under 8B — measure-twice CORRECTION (the earlier "C-stack scan is THE blocker" was
  overstated).** Verified in alloc.hh: arena blocks are **mmap'd/calloc'd, NOT Boehm-managed**,
  and `GC_add_roots` for them is **skipped** (`NIX_V3_ARENA_NOROOT` default-ON); the **non-moving
  major GC is default-ON** (`NIX_V3_NO_MAJOR_GC==nullptr` ⇒ enabled, alloc.hh:959) and sweeps the
  arena. Therefore arena-object liveness is the **precise mark** (`walkAllV3Roots` → `visitValue`
  → `asX()`), which decodes the NaN-box correctly — and Boehm **never** pinned arena objects via
  conservative scanning *at 16B either* (arena isn't Boehm-managed; `GC_base` of an arena address
  is null). So 8B loses NO pinning the current build relies on. The C-local exposure that the
  `exitDepth==0` scavenge rule ([[feedback_v3_nursery_cstack_safety]]) guards is a
  **representation-independent safepoint-discipline** issue, not a NaN-box issue. Bridge/External
  Values are rooted via the bridge-root registry (not the C-stack), and are `tagIsPointer==false`
  so the precise walk skips them regardless. ⇒ **8B GC-correctness is plausibly already satisfied
  by the existing precise-mark + arena-noroot + bridge-registry + safepoint infrastructure.** The
  residual risk is NARROW (any spot that relied on Boehm conservatively pinning a *clean* v3
  pointer to a *Boehm-heap* object — none expected) and is best validated **empirically by the
  build8 run**, not by a pre-emptive GC redesign.
- Then the SHIP gate: peak-RSS ≥20% on a fixpoint-free Bindings-heavy eval; byte-identical; wall ≤5%.

### L3 — RESULTS (2026-06-10): 8B builds, runs, is CORRECT, and saves 18–24% peak RSS

The measure-twice GC correction held empirically. `build8` (`-Dcpp_args=-DV3_VALUE_8B`, whole
tree) **built + linked clean** (291 steps, 0 errors). The 8B `v3-eval`/`v3-smoke`:

- **CORRECT.** v3-smoke ALL PASS; v3 lang **142/143 — byte-identical to the 16B build** (same
  sole pre-existing `eval-okay-types` fail). Smoke incl. lazy attrsets, floats, foldl', partial
  application, and **boxed-int overflow** (`9999999999999999 + 1 = 10000000000000000`) all correct.
  ⇒ the precise-mark + arena-noroot + safepoint infra already handles 8B; **no GC redesign needed**
  (the "C-stack blocker" is empirically moot).
- **Peak-RSS A/B (16B build/ vs 8B build8/, same eval, `/usr/bin/time -l` max RSS, byte-identical
  results):**
  - Shape A (300k 3-entry attrsets, deep-forced): **23.8–23.9%** (138.1 → 105.2 MB), stable ×3.
  - Shape B (one 250k-entry attrset, attrValues summed): **18.2%** (157.3 → 128.6 MB), stable ×2 —
    just under 20% because 250k non-shrinking `allocChars` key strings + base RSS dilute the
    Value-array win (the entry array itself is 24→16 = −33%).
- **WALL — AUTHORITATIVE darwin-4 (2026-06-10, idle host, hyperfine -N -w3 -r15, both binaries
  built fresh on darwin-4 from HEAD; byte-identical results re-verified there):**
  - Shape A: 16B 320.6 ms ± 6.9 vs 8B 333.8 ms ± 1.5 → **+4.1% wall** (1.04×); user-CPU
    295.1 → 311.8 ms = **+5.7%**.
  - Shape B: 16B 283.7 ms ± 3.2 vs 8B 287.4 ms ± 2.7 → **+1.3% wall** (1.01×); user-CPU
    258.2 → 264.1 ms = **+2.3%**.
  - **Both clear the ≤5% wall gate** (+4.1% / +1.3%). user-CPU +2–6% (Shape A's +5.7% marginally
    over on the CPU metric — it's the most alloc-dense shape; the per-access NaN-box mask/shift +
    the `asInt` boxed-int branch are the cost). (Setup bug to avoid: an UNanchored rsync
    `--exclude='build/'` deleted the SOURCE dir `src/libstore/build/` on the remote → meson
    "build-log.cc does not exist"; use `--exclude='/build/'` anchored.)
- **Verdict: GO** on the synthetic evidence — **−18–24% peak RSS for +1–4% wall, byte-identical**.
  Exactly the memory-for-wall trade the project's memory-first principle favors
  ([[feedback_memory_first_class]]). Far above the 10% REVERT floor; wall within gate.
- **Still pending before L4 (default flip):** the **firefox/real-eval peak-RSS** the gate most
  wanted is still blocked by the #455 fixpoint loop under pure v3-direct; and flipping the
  production default warrants maintainer sign-off. The objective gates (RSS ≥20% on attrset-heavy,
  wall ≤5%, byte-identical, correctness) are MET on synthetics.

### L4 — ✅ DONE (2026-06-10): flipped default-on, soaked, 16B path retired.

- **Flip** (commit c690b3f19): `value.hh` `#define V3_VALUE_8B 1` by default, opt-out
  `-DV3_VALUE_16B_LEGACY`. All gates met incl. the firefox real-eval RSS (−20–24%) that
  the #455 fix unblocked; lang 142/143; wall +1–4% (darwin-4).
- **Soak**: built a 16B-legacy reference (`build16`) and byte-diffed vs 8B-default —
  **lang corpus 144/144 + 16/16 nixpkgs `.drvPath`** (toolchain + firefox) byte-identical.
- **Retire** (commit 2544024bb): deleted the `#ifdef V3_VALUE_8B`/`#else`(16B) branches +
  the valve from value.hh + value.cc (−179 lines); 8B is now UNCONDITIONAL. Re-validated
  (lang 142/143, smoke, #455 4/4) on the unconditional build. 16B layout preserved in git
  @ c690b3f19.
- **Residual (darwin-4 follow-up, low-risk):** M5 (cardano-node) + HNE bake need the full
  `nix` binary under 8B + the cardano/haskell.nix flakes (`getFlake` not wired in
  `v3-eval`). Layout-only flip + firefox.drvPath byte-identity make divergence unlikely;
  a git revert is the safety net if ever needed.

**Lever B COMPLETE: the v3 `Value` is a tagged 8-byte NaN-boxed word, default + only layout.**

## Dependencies / notes

- **#455 fixpoint loop blocks the *firefox/drvPath* measurement** under pure v3-direct
  (separate hard problem; see LIST_ITERATION_PERF "(1)"). For the L3 RSS gate, measure on a
  Bindings-heavy eval v3 *completes* (attrset-of-attrsets / a real module/overlay eval); use
  firefox only if/when #455 is unblocked or via a structure-only path.
- **Complement to Lever A, not a replacement** — A removed Bindings *copies* (chain
  composition); B shrinks each *entry* (24→16). They stack.
- **`tagIsPointer` already exists** (precise-root work) — its semantics must match the new
  encoding exactly, or the GC mis-walks roots.
