# #741 IFD content-addressed eval-result cache — engineering design

Operationalises `IFD_DEEP_DIVE_2026-05-21.md` §11 (S4 + UNISON Item 3
narrow) into engineering tasks with falsifiable per-phase exit criteria.

Authored 2026-05-23 in the post-#792 audit slot.  #792 closed the
"mechanical redundancy in derivation primops" lever with sub-threshold
measurements.  #741 is the only multi-week architectural lever left
for the derivation-construction wall on hello.drvPath / cardano-node.

Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
Input Output Group.  SPDX-License-Identifier: Apache-2.0.

---

## 1. The premise

cppnix has no eval-result memory.  Every `nix eval` re-runs every pure
subexpression from scratch.  haskell.nix ships a manual workaround
(`materialized = ./materialized;`) that content-addresses a generated
`default.nix` keyed on plan-sha256.  #741 lifts that workaround into
v3's evaluator: a content-addressed cache of v3 WHNF Values, keyed on
content-hashes of the primop inputs.

The cache covers TWO classes of primop:

| Class | Primops | Wall savings target |
|---|---|---|
| **A. Derivation construction** | `derivationStrict` / `__derivationStrictRaw` / `__derivationFromPreprocessed` | hello.drvPath ~500 ms / call set |
| **B. IFD-triggering** | `import` / `readFile` / `pathExists` on a derivation-output path | cardano-node ~5s/project floor (matches haskell.nix materialization) |

Per-#788 data, class A dominates hello.drvPath.  Per the haskell.nix
materialization evidence in IFD_DEEP_DIVE §6.4, class B dominates
cardano-node.  S4-as-originally-scoped covers only class B; this design
extends to both classes since the cache machinery is identical.

## 2. Scope decision

**Phase 1 spike validates class A only** (no IFD).  Class A is testable
on hello.drvPath which already runs reliably under v3-direct.  Class B
requires haskell.nix infrastructure that's session-overhead-heavy for a
single-session spike.

Class B is folded into Phase 3 once class A's correctness and
measurement story are nailed down.

## 3. The Value-subset to serialize

Reading `primDerivationStrictNative` / `buildAndWriteDrvNative` in
primops.cc, the result attrset shape is bounded:

```
Tag::Attrs
├── "drvPath": Tag::String { str, ctx: { Built(drvPath, "") | DrvDeep(drvPath) | Opaque(storePath) }* }
├── "outputs": Tag::List of Tag::String (output names; out, dev, …)
├── "<outputName>": Tag::String { str = storePath, ctx: { Built(drvPath, outputName) } }  (one per output)
├── "type": Tag::String "derivation"
└── "all": Tag::List of Tag::Attrs (each one a recursive derivation-result attrset)
```

No Thunks (the result is fully WHNF post-primop), no Closures, no
Functions, no Path values, no nested attrset other than the recursive
`all` entries (which themselves are derivation-result attrsets).

This subset has bounded variants.  The serializer is a switch on Tag
across exactly: String (with context), List, Attrs, with terminal
recursion on Attrs entries.

Out of scope for Phase 1:
- Closures, Thunks, Functions, PrimOps
- Int, Float, Bool, Null (not present in derivation results;
  trivial to add later)
- Path values (not present in derivation results)

## 4. Key derivation

The cache key is `SHA256( canonical-bytes(input-attrset) )`.

`canonical-bytes` must be DETERMINISTIC across processes for the same
logical input.  The input attrset can contain (per primops.cc reading):

- Strings (with context) — canonical: utf-8 bytes + sorted context entries
- Lists — canonical: 0x4C + 4-byte LE length + each element's canonical bytes
- Attrs — canonical: 0x41 + 4-byte LE count + sorted-by-name entries
  (name as length-prefixed utf-8 + canonical value)
- Ints — canonical: 0x49 + 8-byte LE
- Floats — canonical: 0x46 + 8-byte LE (bit-exact)
- Bools — canonical: 0x42 + 1 byte
- Null — canonical: 0x4E (1 byte)
- Path values — canonical: 0x50 + length-prefixed bytes

**Open correctness question for Phase 2**: do we have to FORCE inputs
deeply to compute the key, defeating laziness?  Yes — content
addressing fundamentally requires the content; lazy thunks have
indeterminate content until forced.  The primop already forces the
inputs (that's why it has the wall cost), so this overhead is
naturally subsumed.

**Open correctness question for Phase 2**: derivations nested as input
attrs (passing `helloDerivation` as a build input).  The natural
cache key is the inner derivation's drvPath (already deterministic)
not its full attrset content (which would require recursive
canonicalisation).  This degrades only the "input contains a thunk
that produces the derivation" case which is rare in practice.

## 5. Cache schema

Extend `disk_cache.cc` from per-CU blob storage to a SQLite database
with TWO tables:

```sql
-- Existing in-memory equivalent.  CU blob keyed by source-content hash.
CREATE TABLE cu_blob (
  key  BLOB PRIMARY KEY,  -- 32-byte SHA256
  blob BLOB NOT NULL
);

-- NEW for #741.  Eval-result Value keyed by primop + input-content hash.
CREATE TABLE eval_result (
  primop_name TEXT NOT NULL,
  input_hash  BLOB NOT NULL,
  result_blob BLOB NOT NULL,
  inserted_at INTEGER NOT NULL,
  PRIMARY KEY (primop_name, input_hash)
);
```

`primop_name` is `derivationStrict`, `__derivationStrictRaw`, etc.
Indexing by primop_name first matches lookup pattern.

Migration: existing `v3-bc-v1/<hex64>` file-per-CU cache directory
stays as fallback.  New code writes BOTH (preserve disk-cache wins
already validated under #770/#771).  SQLite cache is opt-in via
`NIX_V3_EVAL_RESULT_CACHE=1` until validated.

## 6. Phases + falsifiers

Each phase has a kill criterion.  If kill fires, the cache stays
opt-in and the next phase doesn't enable.

### Phase 1 — Value-subset serialiser round-trip (THIS session)

Scope:
- `value_serialize.cc`: `serializeDerivResult(Value &, std::string & out)`
  + `deserializeDerivResult(const std::string &, EvalState &) → Value`
- Round-trip test: build a derivation, serialise, deserialise,
  re-printNixValue, byte-identical to pre-serialise printNixValue.
- Measure round-trip overhead: serialise + deserialise wall time
  per derivation-result.

Falsifier:
- Round-trip byte-identical for 100% of hello.drvPath's 380 results.
- Round-trip overhead per result < 100 µs (target: cache lookup is
  faster than the primop body it replaces, which is ~4 ms/call).

If round-trip cannot be made byte-identical OR if overhead exceeds
half the primop body cost, **kill #741 here**.  Document why.
The cache is unviable.

### Phase 2 — Input-attrset canonical hasher

Scope:
- `value_canonical_hash.cc`: `canonicalHashInputAttrset(Value &, uint8_t out[32])`
- Tests across the input shapes that hit `derivationStrict` /
  `__derivationFromPreprocessed` on hello.drvPath.
- Determinism test: run the hash across two independent processes,
  expect byte-identical results.

Falsifier:
- Two processes hashing the same logical input must produce the same
  bytes for ≥ 99.9% of inputs.
- If determinism breaks (probably via pointer-identity creeping in,
  context-list order, dict insertion order), trace the source.  If
  fixable, fix it.  If not, **kill #741 here**.

### Phase 3 — Cache integration + measurement

Scope:
- Hook `disk_cache.cc` with SQLite schema extension.
- Wire `primDerivationStrict` to check the cache before calling the
  native fast path; on hit, deserialise and return; on miss, build +
  insert.
- Same for `__derivationFromPreprocessed`.

Cold-run measurement (`hyperfine` n=15):
- Wall delta vs current main (target: cold-run < +10%
  due to canonicalHash + serialise + SQLite insert overhead)
- v3_arena delta (target: ≤ +5% due to extra allocations)
- peak_rss delta (target: ≤ +5%)

Warm-run measurement (cache pre-populated):
- Wall delta vs current main (target: ≥ -30% wall, with primop
  wall reduced ≥ 80%)

Falsifier:
- Warm-run wall savings < 30%: **kill #741**.  The architecture
  is correct but the lever is too small for hello.drvPath; reassess
  if class B (IFD-triggering) workloads make up the difference.
- Cold-run wall penalty > 25%: **gate cache opt-in**; we don't
  ship a regression to make warm runs cheaper unless very explicit.

### Phase 4 — Class B (IFD-triggering) extension

Scope:
- Hook OP_IFD_PROBE → cache lookup BEFORE the realisePath bridge.
- Workload: haskell.nix hello-world or cardano-node M5.

Falsifier:
- IFD-cached run hits the haskell.nix `materialization` baseline
  (≈ 5 s saved per project on warm runs).  If not, **investigate
  before promoting default-on**.

### Phase 5 — Cross-process locking + production hardening

Scope:
- SQLite WAL mode + busy-wait
- Eviction policy (oldest-inserted-at; LRU not worth the metadata)
- Telemetry: hits, misses, insert failures, bytes-saved
- Default-on gating

## 7. Risks + sharp edges

1. **Determinism**: anything in the result Value that varies between
   processes (memory addresses, file timestamps, RNG-influenced
   ordering) breaks the cache.  Phase 2 has the determinism test.

2. **Schema evolution**: cache entries from old v3 binaries against
   new ones.  Embed a version byte; bump on any breaking serialiser
   change.  Bumping invalidates the whole result-cache (acceptable
   trade-off for correctness).

3. **Context propagation**: derivation result strings carry NixStringContext
   that downstream code reads.  Serialiser MUST preserve every
   context entry exactly.  Pre-mistake: see #682 (toFile context
   bug) for what happens when a primop drops context.  Phase 1 test
   should include a derivation with string-context for downstream
   reads.

4. **Cache poisoning via bug**: a bug that mis-serialises results
   then the WRONG bytes get cached and replay produces wrong
   downstream eval.  Mitigation: ship behind opt-in gate until 100%
   of one workload has been verified byte-identical across cold +
   warm runs.

5. **Memory pressure during deserialise**: the Bindings layout
   changed in #752 (PosIdx inlined).  Deserialiser must produce
   identical bytes-on-heap layout, or use the constructive
   `setStringContext` / `mkAttrs` APIs and not raw cell writes.

## 8. Falsifiable hypothesis (Rule 0)

This document's existence kills the hypothesis: "We don't have a
concrete plan for #741 in a form a contributor can pick up tomorrow."

Phase 1 spike kills the hypothesis: "We can't deterministically
serialise + deserialise a derivation-result Value byte-identically."

If Phase 1 SUCCEEDS, the multi-week feature has empirical green-light
for Phases 2-5.

If Phase 1 FAILS, the architecture is dead and we close #741 with
documented reasons — saving 1-2 weeks vs discovering the failure
mid-Phase-3.

## 9. Cross-references

- `IFD_DEEP_DIVE_2026-05-21.md` §11 — §11.1 the materialization
  workaround, §11.2 the v3 replacement table.
- `UNISON_IDEAS_2026-05-07.md` §3 — the general Hash-keyed evaluation
  cache (S4 = narrow to IFD; #741 = narrow to derivation results
  initially, then S4).
- `lode/SESSION_ARC_2026-05-23.md` "What's NOT been tried" row #741.
- `memory/project_792_drv_primop_audit_2026-05-23.md` — #792 conclusion
  pointing to #741 as the only architectural lever.
- `memory/project_788_per_primop_2026-05-23.md` — per-primop wall-clock
  data justifying the ≈ 500 ms savings estimate.
- `lode/IFD_DEEP_DIVE_2026-05-21.md` §8 "Recommended roadmap" —
  the 5-step ladder S5 → measurement → S2 → S4 → S7.
- #770/#771 (per-CU disk cache) — the substrate this extends.
- #736 OP_IFD_PROBE — the probe emission used in class B.

## 10. Next-session pickup

Implement Phase 1 spike (this session does the design + decides
whether spike fits in the remaining time).  If spike fits, deliver
ONE commit that lands the round-trip serialiser + test + measurement.
If not, queue Phase 1 for next session as the first commit.
