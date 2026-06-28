# Strict-eval / unboxed-thunks — measure-first verdict: BOUNDED (NO-GO)

**Date:** 2026-06-28  **Branch:** angerman/2.35-eval-profiling-v2
**Method:** 3 parallel subsystem maps (force path / C1 strict-lowering coverage / alloc
sites) + the fresh RSS decomposition (RSS_DECOMP_FRESH_2026-06-27). No new code — bound the
ceiling before building, as B0.3 did for BiBOP. Reuses the existing C1 strict ceiling
(C1_P01/P02_2026-06-23) rather than re-measuring it.

## Half 1 — strict-eval: ALREADY MEASURED + KILLED (C1_P01/P02)

- Static call-arg + force-in-place strictness is **shipped default-on** (opt_func_strictness
  + opt_strict_call_unthunk + opt_strictness redundant-Force elim). Syntactically-strict
  positions (if-cond, BinOp operands, ==/// // ++, Assert, ConcatStrings, AttrSelect root)
  are never thunked.
- The FULL remaining call-arg ceiling = **3.5% of thunks** (firefox ~100K) ≈ **1% CPU /
  0.4% RSS** — at/below the darwin-4 noise floor.
- **65.7% of thunks are NEVER forced** = real lazy DATA (attrset values, list elems,
  let-bindings). Strictness CANNOT touch these: eager eval of an unused attr → divergence /
  over-evaluation. Structurally untouchable.
- Dynamic speculative strictness (#143) was killed: un-deopt-safe (can't un-force an
  error/non-termination); the sound form needs an optimistic-eval subsystem (major) for a
  ~1% gross win. NO-GO stands.

## Half 2 — unboxed thunks: BOUNDED by mechanism + the dead-arena wall

Force-path map (vm.cc OP_FORCE/OP_RETURN, closure.hh): forcing caches the result INSIDE the
24B cell (`t->evaluated` union) and the cell stays LIVE (reachable via the `Tag::Thunk`
Value). There is already a TW-style write-back (`Thunk.cell` → `*cell` on OP_RETURN, for
heap-stable Bindings entries) + path compression of Evaluated chains.

So the candidate "overwrite the Value in place with the result, let the cell die" does NOT
help:
- RSS: it would only move the 24B cell from "live" to "dead-unreclaimed" — and the arena
  never returns pages (the entire GC/BiBOP campaign's wall). The page stays mapped → 0 RSS.
- CPU: re-force chase is already path-compressed → 0.
- Header size: Thunk is at its **24B floor** (FP-2 already removed cu/capturedWiths/shapeCell;
  56→40→24B). No more to shave.

## The decisive number — the thunk RSS is DEAD CELLS, not the representation

M5: 17.2M thunks allocated (659 MB) but only **2.21M live** at the major GC.
- live thunk bytes ≈ 2.21M × ~32B ≈ **~71 MB** — a small slice of the 592 MB peak-live arena.
- the other **~588 MB of "thunks"** is DEAD cells (forced-and-discarded + never-forced-
  unreachable) sitting in the arena because it never reclaims.

v3's peak-LIVE arena (592 MB) is already LEANER than TW's ENTIRE RSS (982 MB). The live
thunk representation is NOT the problem. The thunk-related bloat is the unreclaimable dead
arena (GC/BiBOP — killed) + real laziness — not the per-thunk bytes.

## One genuinely-unfinished sub-lever (modest, blocked)

Env-sharing is default-on but still allocates the inline FAM upvalues (poisoned, not
dropped). Completing it — `allocThunk(0)` + a shared/interned Env pointer — would drop the
~14B avg FAM. But live thunks are ~2.21M → ~**31 MB** live saved, and the bring-up itself
costs +1 Env/thunk unless **Env interning** lands first (the hard part, the standing
blocker). Below the bar on its own.

## Verdict — NO-GO, re-confirms the campaign

Neither half clears the noise floor. The thunk lever is the dead-arena + real-laziness
problem in disguise — the same wall every reclamation lever hit. The thunk lever is NOT a
fresh path to beating TW; it RE-CONFIRMS that the gap is structural (dead arena unreclaimable
+ bytecode-VM non-arena tax + FFI), not the live cell representation. The only directions
left with real headroom remain the deferred long-horizon ones: native codegen (JIT, CPU) and
a from-the-ground-up leaner+reclaimable heap (RSS) — multi-week programs, not a thunk tweak.

Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.
SPDX-License-Identifier: Apache-2.0
