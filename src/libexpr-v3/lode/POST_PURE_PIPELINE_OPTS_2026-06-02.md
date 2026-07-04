# Optimizations unlocked by the pure v3 pipeline

**Date:** 2026-06-02
**Status:** OPPORTUNITY ANALYSIS — what became possible specifically because the eval path is now fully native (`.nix → v3 AST → v3 IR → bytecode → VM`, no `nix::Expr`, no TW in eval). NOT a commitment; a ranked menu with one concrete task spelled out (§3).
**Author:** session synthesis (grounded in the V3_VM_STATE_2026-06-02 architecture map)

Companion docs:
- [`V3_VM_STATE_2026-06-02.md`](V3_VM_STATE_2026-06-02.md) — the architecture this builds on
- [`NATIVE_PARSER_FEASIBILITY_2026-06-01.md`](NATIVE_PARSER_FEASIBILITY_2026-06-01.md) — the PosIdx-not-cache-stable finding that §1 turns into an unlock
- [`NATIVE_VM_CODE_REVIEW_2026-06-02.md`](NATIVE_VM_CODE_REVIEW_2026-06-02.md) — findings 9/10 (position loss) that §4 fixes
- [`ROADMAP_TO_VISION_2026-05-15.md`](ROADMAP_TO_VISION_2026-05-15.md) — Stage 10 (salsa) + the R8a AOT candidate that §1 + §2 feed
- [`TW_VALUE_ERADICATION_GOAL_2026-06-02.md`](TW_VALUE_ERADICATION_GOAL_2026-06-02.md) — the parallel priority (different axis)

---

## 0. The framing (read first — avoids overclaiming)

**Most VM/IR optimizations were never blocked by the parser.** Beta-reduction, stream-fusion, strictness analysis, inline caches, the register-VM / superinstruction rework, JIT — all operate on v3 IR or bytecode, which v3 has **always** owned. The TW parser produced `nix::Expr`; v3 lowered it; everything downstream was already v3's. So "what can we do now" is a *smaller, sharper* set than the parser milestone might suggest.

The optimizations specifically unlocked by going pure are the ones tied to:
- **(a) owning the position representation** → determinism (the big one)
- **(b) owning the whole frontend** → being able to skip or reshape the intermediate, and to do work at parse/lex time

This doc lists exactly those, ranked, and is explicit (§6) about what is **NOT** unlocked, so the menu isn't oversold.

---

## 1. Content-addressed bytecode cache + AOT distribution — THE standout

This is the one genuine hard "couldn't before."

### 1.1 Why it was blocked

TW's parser assigns global `PosIdx` values in **import-load order** (`pos-table.hh` `addOrigin` accumulates a byte-offset per origin as files load). Two builds that traverse imports in a different order produce **different `PosIdx` for the same source line** → different position-derived data in the serialized CU → **the bytecode disk cache could never be content-addressed**. It was effectively same-process / same-import-order only; cross-machine sharing was impossible because the cache key could not be a pure function of the source.

### 1.2 Why it's now possible

The v3 parser emits **file-local `uint32_t` byte offsets** (`ast/expr.hh:62`) — deterministic from the source bytes alone, independent of import order. Bytecode becomes (modulo the audit in §3) a pure function of `hash(source bytes) + v3 schema version`. That unlocks:

- **Content-addressed bytecode cache** keyed on source hash — **shareable across machines**.
- **AOT distribution** (the parked R8a candidate): ship a `nixpkgs-bytecode-cache` as a binary-cache artifact; CI primes it; every consumer gets warm-eval on a cold checkout.
- **Reproducible bytecode** as a verifiable property (CI can assert `compile(src)` is byte-stable).

### 1.3 Why it compounds

Warm-eval today is a per-machine luxury (the SQLite cache at `disk_cache.cc`). Content-addressing converts it into a *distributable artifact* — the difference between "this machine has eval'd nixpkgs before" and "any machine can fetch pre-compiled nixpkgs bytecode." It's also a **different axis** from the two current priorities (F1 fetcher-eradication for memory; strictness-parity for correctness), so it doesn't compete for the same work.

**See §3 for the concrete task.** This is the recommended first pickup of the menu.

---

## 2. Collapse `.nix → IR` directly — skip the AST

### 2.1 Status

The parser actions currently build a v3 AST (`ast/expr.hh`), then `lowerV3Ast` (`cli/lower_v3.hh`) walks it to IR. **Before, this was impossible** — TW produced `nix::Expr`, so v3 *had* to consume an AST. Now we own both the parser actions and the lowering.

### 2.2 The move

Have the parser actions emit `ir::Module` **directly**, eliminating the AST node allocation + the tree-walk pass entirely (the parser-feasibility doc's "Path B"). Win: removes a whole allocation pass + a walk per parse — a memory + cold-eval-wall win (parse is a real fraction of cold-eval on big flakes).

### 2.3 The trade-off + sequencing

Collapsing parse→IR removes the AST as a debugging / `--parse` / validation surface. **Sequence this AFTER the strictness-parity audit** (the #677-#693 analogue closing the 7 silent-permissiveness code-review findings): you want the adversarial findings closed against the current, more-inspectable two-stage shape before deleting the intermediate. Keep an opt-in `--emit-ast` for debugging.

---

## 3. CONCRETE TASK — content-addressed bytecode cache (the §1 unlock, spelled out)

The native parser *removes the blocker*; realizing the unlock is a bounded, well-scoped task. Pre-committed shape:

### 3.1 Step A — serializer position-determinism audit (~1-2 days)

Confirm the serialized `CompilationUnit` is a pure function of source bytes + schema. Audit `serialize.cc` / `value_serialize.cc` for ANY residual order- or address-dependence:
- positions: confirm only file-local `uint32_t` offsets are serialized (no global PosIdx, no `PosTable::Origin` ordering).
- symbol IDs: the global symbol table is interned in encounter order — confirm CU serialization is symbol-ID-stable or remaps to a CU-local table (Schema 14 did the PosIdx remap; confirm symbols too).
- any pointer/address baked into the blob.
- **Falsifier:** `compile(src)` serialized twice in two fresh processes with different prior import history → byte-identical blob. If not, name the order-dependent field.

### 3.2 Step B — key the cache on source hash (~1 day)

Today `disk_cache.cc` keys rows on `(key, schema)`. Confirm/convert `key` to be `hash(source bytes)` (content address), not anything path- or order-derived. Verify cache HIT across two checkouts of the same source at different store paths.

### 3.3 Step C — cross-machine validation (~1 day)

Prime the cache on host A; copy the SQLite (or a flat export) to host B; confirm host B gets cache HITs (byte-identical drvPath, no recompile). This is the property that was IMPOSSIBLE before.

### 3.4 Step D (optional, larger) — AOT artifact (R8a)

If A-C land, the flat-file export becomes a distributable `nixpkgs-bytecode-cache`. This is the R8a candidate; its measurement gate + thresholds live in the AOT docs. Defer until A-C prove content-addressing works.

**Pre-committed acceptance for A-C:** two fresh processes / two checkouts / two hosts all produce byte-identical serialized CU and cache HIT. Effort ~3-4 days for A-C. **This is the recommended first pickup** — bounded, high-leverage, different axis from the active work.

---

## 4. Lazy / on-demand positions — fixes a finding AND saves memory

Elegant two-for-one, newly clean only because we own the position model.

### 4.1 The old false dichotomy

- TW's way: store a `PosIdx` on **every** AST node (memory cost; PosIdx resolution "very expensive" per `pos-table.hh`).
- Current v3: the IR carries **no** position field on App/AttrSelect/If/Lambda (`ir.hh`) → error traces and `«lambda @ pos»` are lost (code-review findings 9/10).

### 4.2 The third option (now available)

Store **one file-local byte offset** per node (4 bytes, cheap) and compute `{file, line, column}` **lazily** from a per-file line-map **only when** `unsafeGetAttrPos` or an error trace actually asks. TW couldn't do this cleanly (PosIdx → Pos re-reads source); we can (byte offset → line/col via a cached line-map is O(log lines)).

### 4.3 Why it's worth it

- **Closes code-review findings 9 + 10** (error-trace positions, `«lambda @ pos»`, dynamic-key `unsafeGetAttrPos`) — without TW's per-node memory cost.
- Keeps the node small (4-byte offset, not a resolved triple).
- Memory-first-class aligned: pay for positions only on the rare path that reads them.

---

## 5. Parse/lex-time wins (marginal but now-trivial)

We own the lexer, so:

- **Parse-time symbol + string-literal interning** straight into the CU constant pool; **dedup identical string literals at parse time.** Runtime string-dedup was a measured no-lever (≤25 MB, Step-18 falsification), but *parse-time* literal dedup is nearly free and shrinks the const pool — a different (cheaper) point in the design space than the runtime intern table that was rejected.
- **Single-use-var classification at lex/parse time** → register-allocation hints threaded to `emit.cc` — a marginal enabler for the local-stack-motion rework (GET/SET_LOCAL+GET_UPVALUE = 48.9% of dispatch per #778).

## 5.1 v3-tailored AST nodes (speculative)

The AST is no longer constrained to nix::Expr's 27 kinds. We *could* add v3-specific nodes recognizing hot patterns (mapAttrs-shaped calls, lib.fix overlays) at parse time and lower them to specialized IR directly, instead of recognizing them post-lowering in an optimizer pass. Marginal and speculative — the optimizer already catches most of these — but newly available. Low priority.

---

## 6. What the pure pipeline does NOT unlock (do not oversell)

| Candidate | Status | Why the parser changed nothing |
|---|---|---|
| **JIT** | not unlocked | dispatch is ~5% of wall; primops / eval-bodies dominate. Parser is irrelevant. |
| **GC / RSS reduction** | orthogonal | the memory lever is bridge eradication (kill the `nix::Value` feeder); the parser never touched the runtime value path |
| **Register VM / superinstructions** | always possible | operates on bytecode; parser is at most a marginal hint-provider (§5) |
| **Whole-module IR opts** (CSE/DCE/global const-prop) | already present | the optimizer already runs on the whole `Module`; the parser didn't add module-scope visibility |
| **Interpreter ceiling** (~1.5-2× native) | structural | unchanged by anything frontend |

---

## 7. Ranked menu + recommendation

| # | Optimization | Newly unlocked by | Value | Effort | Sequence |
|---|---|---|---|---|---|
| 1 | **Content-addressed bytecode cache + AOT** (§1/§3) | deterministic file-local positions | HIGH (distributable warm-eval) | ~3-4 d (A-C) | **first pickup** — different axis from active work |
| 2 | **Skip-the-AST direct lowering** (§2) | owning parser actions + lowering | MED (memory + cold-eval pass) | ~1 wk | AFTER strictness-parity audit |
| 3 | **Lazy on-demand positions** (§4) | owning the position model | MED (closes findings 9/10 + node memory) | ~2-3 d | anytime; pairs with the code-review fixes |
| 4 | **Parse-time interning / literal dedup** (§5) | owning the lexer | LOW | ~1-2 d | opportunistic |
| 5 | **v3-tailored AST nodes** (§5.1) | owning the AST shape | LOW (speculative) | varies | defer |

**Recommendation:** #1 is the standout — genuinely impossible before, synergistic with the parked R8a/AOT candidate, bounded (§3 A-C ~3-4 days), and on a *different axis* from the two current priorities (F1 fetcher eradication for memory; strictness-parity for correctness). #3 is the cheap two-for-one to fold into the code-review fix batch. #2 is real but gated on the strictness audit.

The honest meta-point: the pure pipeline's biggest payoff is **determinism**, not raw speed. The speed levers (register VM, primop fusion, strictness) were always available; what the parser bought is a bytecode layer that is a *pure function of source* — and that turns the disk cache from a per-machine convenience into a shareable, AOT-distributable artifact. That is the optimization that was architecturally impossible before and is the one to reach for first.

---

## 8. Honest limits

- **§1/§3 hinges on the serializer audit (Step A).** If a residual order-dependent field is found (symbol IDs, an embedded address), content-addressing needs that fixed first — the native parser removes the *position* blocker, but Step A must confirm no *other* blocker survives. Don't claim the cache is content-addressable until the two-process byte-identical falsifier passes.
- **§2 (skip-AST) trades away a validation surface.** Sequencing after the strictness audit is load-bearing, not optional.
- **§4 (lazy positions) needs a per-file line-map** built once per CU — small, but it's new state; confirm it's cheaper than the per-node-triple it replaces (it is: 1 line-map per file vs 1 triple per node).
- **None of these is the memory lever.** M5 < 4 GB still routes through bridge eradication (§6). This menu is determinism + cold-eval + correctness-adjacent wins, not the RSS target.
- **Effort estimates are for the bounded pieces (A-C, #3, #4);** the AOT artifact (§3 Step D / R8a) is a larger, separately-gated arc.

---

## 9. Cross-references

- [[v3-vm-state-2026-06-02]] — the architecture map this builds on
- [[native-parser-feasibility-2026-06-01]] — the PosIdx-not-cache-stable finding (§1's blocker); its determinism rationale is exactly this unlock
- [[native-vm-code-review-2026-06-02]] — findings 9/10 that §4 closes
- [[tw-value-eradication-goal-2026-06-02]] — the parallel priority (memory axis; §6 notes the parser didn't touch it)
- `ROADMAP_TO_VISION_2026-05-15.md` — Stage 10 (salsa, likely subsumed) + R8a AOT candidate (§3 Step D)
- [[memory-first-class]] — §4 lazy-positions framing
- [[measure-twice-cut-once]] — §3 pre-committed acceptance (two-process / two-host falsifier)
- [[falsification-rule]] — Step A is a falsifier (byte-identical-across-processes)

### Code anchors
- `ast/expr.hh:62` — file-local uint32 positions (the §1 enabler)
- `disk_cache.cc` `(key, schema)` PK — the §3 Step B target
- `serialize.cc` / `value_serialize.cc` — the §3 Step A audit surface
- `cli/lower_v3.hh` — the §2 skip-AST site (parser actions → IR)
- `ir.hh` — App/AttrSelect/If/Lambda have no pos field (§4)
- `pos-table.hh` (TW) — `addOrigin` import-order PosIdx (the historical blocker)

---

*Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group. SPDX-License-Identifier: Apache-2.0.*
