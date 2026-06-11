# v3 VM — Comprehensive Codebase Review (2026-06-11)

**Scope**: full review of `src/libexpr-v3/` by six parallel deep-read audits: (1) VM core
(`vm.cc` 13.6 KLoC + value/closure/bytecode/barrier headers), (2) a dedicated firefox
getLib cross-write mechanism hunt, (3) memory subsystem (`alloc.hh`, `gc.cc`,
`mark_sweep.cc`, nursery, barriers, precise roots), (4) primop layer (`primops.cc`
9.7 KLoC / 216 registrations + `bytecode_primops.cc`), (5) compiler pipeline (`emit.cc`,
IR, all 15 `opt_*.cc` passes, `cli/lower_v3.hh`), (6) FFI / runtime integration / caches
(`ffi.cc`, `run.cc`, `disk_cache.cc`, `aot_cache.cc`, `serialize.cc`,
`value_serialize.cc`, CLI plumbing).

**Method + epistemic status**: every finding below is from *code reading* at HEAD
(branch `angerman/2.35-eval-profiling-v2`), each tagged with a confidence level
(`verified-in-code` = the mechanism was traced end-to-end in source; `likely`;
`needs-measurement`). Per Rule 0, nothing here is "the answer" until its falsifier
runs; per the measurement gate, **every** perf claim must go through
`bench/v3-vs-tw-gate.sh` before being acted on. Line numbers were read at HEAD and
will drift.

---

## 0. State corrections (the briefing context several of us carry is stale)

These change how to read everything else:

1. **Value is already 8 B.** Lever B landed (`value.hh:260`
   `static_assert(sizeof(Value)==8)`). ValuePair is **32 B**, `Bindings::Entry` 16 B,
   ListVec 8 B/elem. "Value 16→8 / ValuePair 64→32" are DONE, not planned.
   (Doc comments in `value.hh:255-296` still narrate the old layout — fix them.)
2. **Major mark-sweep GC is DEFAULT-ON** (`alloc.hh:958` — opt-out
   `NIX_V3_NO_MAJOR_GC=1`), and the arena is **de-registered from Boehm** by default
   (`alloc.hh:933`). Blocks are mmap'd; whole-dead blocks/huge cells are munmapped per
   GC. "Boehm conservative GC is the allocator, MS is gated off" is wrong at HEAD.
   **Consequence**: every pointer-keyed side cache that survives a GC is now a live
   aliasing/UAF hazard class, not a theoretical one (see C-2, M-1).
3. Still default-OFF: nursery (`NIX_V3_NURSERY`), Immix line-reuse allocation
   (`V3_DBG_IMMIX_ALLOC`), free-list reuse, evacuation.

---

## 1. P0 — The open correctness residuals: concrete mechanisms found

### C-1. Stale `CFF_FORCE_WB_PTR_KEEP` writeback fires with a later, unrelated value
**The prime suspect for BOTH the firefox getLib="21" residual AND the suspected
"4th non-SELECT path" (optionalString PAP→String).**
Severity: critical-correctness. Confidence: verified-in-code (mechanism);
likely (that it is the firefox root cause).

Mechanism (all sites traced):

- The 3 SELECT writeback arm sites (`vm.cc:8669-8676`, `8848-8855`, `9132-9139`) set
  `f.forceWriteTarget = &entry.value; f.flags |= CFF_FORCE_WB_PTR_KEEP|CFF_FORCE_RETRY`.
  The e87272115 guard (`!isUnderappliedClosurePap(slot)`) checks the slot's **current**
  tag only. An entry that is currently `App(Thunk(...), arg)` — leaf not yet a Closure —
  **passes the guard** (`isUnderappliedClosurePap`, `vm.cc:199-212`, requires the spine
  leaf to already be `Tag::Closure`).
- `op_force_slow` chases, forces the leaf, gets a PAP, and breaks
  (`vm.cc:7173`), then `push(v); applyForceWriteback(vm)` (`vm.cc:7281-7286`).
- `applyForceWriteback`'s KEEP branch (`vm.cc:2394-2398`) sees `Tag::App` and
  **returns false WITHOUT disarming**: `if (t==Thunk||App||App3||Slot) return false;`.
  A PAP is permanently App-tagged, so no later retry can ever disarm it.
- The flag + raw pointer into a shared `Bindings::entries[i].value` stay armed on the
  frame. **The next force in the same frame** that resolves through `op_force_slow`
  fires the stale KEEP and writes that unrelated WHNF — e.g. the `String "21"` from
  forcing `llvmVersion` — into the old getLib entry.

This reproduces the localization exactly: entry *named* getLib (name-validated reads
all succeed), *value* = llvmVersion's String; the cross-write is upstream of any merge;
mergeBindings instrumentation never sees it. It also explains the firefox
`optionalString` PAP→String symptom with the same single mechanism.

Secondary protocol break, same function: KEEP is checked **before** CFF_FORCE_WB/PTR
(`vm.cc:2385` vs `2409/2419`) — a frame carrying a stale KEEP plus a freshly armed
slot-WB services the stale one and leaves the new one pending with a wrong stack shape.

**Fix**: in the KEEP branch, treat `isUnderappliedClosurePap(top)` as WHNF — write it
(the PAP *is* the entry's true value) and disarm; or disarm without writing. Exclude
`Tag::Blackhole` from the KEEP write (see C-4). Belt: assert at each arm site that no
KEEP is already pending.
**Falsifier**: debug counter "KEEP declined on App-like top" → run firefox.drvPath
v3-direct → expect nonzero hits at/near the corruption; fix; require byte-identical
drvPath via the gate + python3/15-pkg corpus + a synthetic repro
(rec attrset with a 2-arity partial application entry + interleaved forces).

### C-2. Runner-up mechanisms for firefox (ranked; falsify in this order)

1. **Disk-cache symbol-remap REC_SET permutation desync** (`serialize.cc:449-592`).
   On CU load, SymbolIds are remapped per-process; the REC_INIT trailer must stay
   sorted, so the loader re-sorts and patches subsequent `OP_ATTRS_REC_SET` operands
   through a hand-rolled linear bytecode walk + `pending` stack
   (`serialize.cc:469-580`). Any decode desync ⇒ the next attrset's REC_SETs write
   values into permuted-wrong slots — a *birth-time* name/value cross under a correct
   name, zero memory-safety violation. No runtime sortedness check exists in
   `OP_ATTRS_REC_INIT` (`vm.cc:7999-8010`) to catch it.
   **Zero-code falsifier**: `rm -rf` the v3 disk cache + `NIX_V3_NO_DISK_CACHE=1`,
   re-run firefox. (No record this A/B was ever run for this residual.)
   Cheap hardening regardless: gated sortedness+dup oracle in REC_INIT.
2. **Pointer-keyed caches stale across the (default-ON) major GC** — see M-1
   (`materialize()` memo) and C-3 (`recSlotCache` IC has no name validation). Both
   are address-aliasing paths that produce wrong-but-self-consistent values under
   correct names after block free + mmap reuse.
   **Falsifier**: re-run the exact failing firefox command with `NIX_V3_NO_MAJOR_GC=1`
   on HEAD (re-verify the earlier rule-out actually ran on this binary/config), and
   add the 2-line gated asserts below.
3. Stale `forceWriteTarget`/`Thunk::cell` into reused memory (defended by
   interior-owner marking in `mark_sweep.cc:277-339`; needs a gap to fire) — only if
   1-2 and C-1 falsify.

### C-3. `recSlotCache` IC hits have NO name validation (unlike SELECT's IC)
Severity: correctness (latent aliasing trap, live under default-ON GC block reuse).
Confidence: verified-in-code.
`OP_REC_BINDING_SLOT_REF` `vm.cc:9603-9608`: `if (ic.bindings == b) found =
&b->entries[ic.slot].value;` — no `entries[ic.slot].name == sym` check; same in
`OP_GET_UPVALUE_REC_BINDING` `vm.cc:9849-9851` and the `_SLOT` variant. Pointer
identity is only sound if addresses are never reused — exactly what default-on
mark-sweep block free+reuse breaks. A stale hit silently returns a Slot to a
wrong-named entry. **Fix**: validate the name on IC hit (fall through to binary search
on mismatch); 2-line gated assert first to measure whether stale hits occur on firefox.

### C-4. Blackhole sentinel gets permanently memoized into shared storage
**Candidate for the ghc98 «slot» residual.** Severity: correctness.
Confidence: verified-in-code (writes happen); likely (user-visible).
The blackhole-as-value protocol (#466) leaks the transient marker into permanent memo
state at three places: (a) `forceValue` memoSlot write `vm.cc:13128-13129` (only
excludes `Tag::Slot`; the path-compression block 4 lines below explicitly excludes
Blackhole — inconsistent); (b) OP_RETURN self-cycle defer `vm.cc:6703` →
`thunkSetEvaluated`/`cellWrite` at `6817-6840` make the thunk permanently
Evaluated→Blackhole and write Blackhole into the shared Bindings cell; (c)
`applyForceWriteback` KEEP accepts Blackhole (`vm.cc:2394-2406`). A "value not yet
known" becomes a permanent value; later reads fail where TW succeeds.
**Fix**: exclude Blackhole at all three; keep self-cycle thunks Suspended-or-throwing.

### C-5. The decisive instrumentation plan (one firefox run, env-gated `V3_DBG_GETLIB_WRITE=1`)

Step 0 (zero new code): the existing `V3_DBG_REC_SET_NAME=getLib` probe
(`vm.cc:11102-11218`) already logs every REC_SET into a getLib-named slot with value
tag + frame + disasm. If it shows `tag=String` at REC_SET time → C-2.1 confirmed.

Then, predicate `tag==String && strlen<=3 && entryName==SymbolId(getLib)`, fire at:
1. `barrier.hh:186 bindingsSetValue` (+ a `const char* site` tag arg from
   `vm.cc:11219` and `vm.cc:8215`),
2. `barrier.hh:206 bindingsSetEntry` (distinguishes first-write vs propagation),
3. `barrier.hh:415 cellWrite` — **the discriminator**: resolve the owner via
   `findContainingCellStart`; if owner is a Bindings and the named entry is getLib,
   log the writing thunk's `desc->name/codeOffset/pos` (`vm.cc:6840` caller — expect
   "llvmVersion" if C-1/C-2.3) and whether `cellContainer` matches the resolved owner
   (mismatch/null = proof of stale-pointer write),
4. `vm.cc:8198` OP_APPLY_OVERRIDES in-place write (bypasses the barrier helpers),
5. REC_INIT sortedness oracle (kills/keeps C-2.1 independently),
6. recSlotCache name-validation assert (kills/keeps C-3).
Plus the two zero-code A/Bs: cold-cache `NIX_V3_NO_DISK_CACHE=1`, and
`NIX_V3_NO_MAJOR_GC=1` on HEAD.

---

## 2. P0 — Error masking: wrong answers instead of errors (systemic)

This single pattern cost weeks on the drvPath divergence and is **still live at HEAD**.

### C-6. derivationStrict: ANY native-path exception → silent `/v3-fake-store/` drvPath
Severity: critical-correctness. Confidence: verified-in-code. (Found independently by
two agents.) `primops.cc:4319-4383`: the native path is wrapped in
`catch (const std::exception&)` that logs only under `V3_DRV_DEBUG` and falls through;
the TW bridge it used to fall through *to* is deleted (`:4409-4419`), so control lands
in the fake-path synthesizer (`:4468-4527`) **even when a real store is wired**
(`state.nixEvalState != nullptr`). It also swallows *user* errors (`throw` forced
inside the native body) — eval continues with a wrong drvPath instead of failing.
**Fix (highest-leverage single change in the repo)**: when a store is wired, rethrow;
reserve fake-store synthesis for standalone `v3-eval` (no store), ideally behind
`NIX_V3_ALLOW_FAKE_STORE=1`. Belt: assert in OP_CALL_PRIMOP's derivationStrict return
that the drvPath doesn't start with `/v3-fake-store/` when a store is wired.
**Test**: `derivation { name="x"; system="s"; builder = throw "boom"; }` must throw in
both evaluators.

### C-7. The rest of the masking inventory (same class, fix as a sweep)
- `builtins.path` fake-store fallback keyed on a **thread_local being unset**
  (`primops.cc:7709-7746`, `tlNixEvalState` set only in `run.cc:163`) — any future
  thread/fiber entry silently fabricates store paths. Throw "no store wired" instead.
- `storeRefsContextFor` `ffi.cc:183-208`: outer `catch (...)` returns an **empty
  string context** — readFile of a store path under any store hiccup yields a
  context-less string → missing `inputSrcs` → wrong drvPath. Narrow to the inner
  unknown-path catch.
- `__derivCoerce` path→store copy failure degrades to the raw `/source/...` path with
  no context (`primops.cc:808-824` + `:973-981`) — and this coercion runs on `builder`,
  `args`, every env attr via the default-on bytecode derivation wrapper. Rethrow when
  `copyPathsToStore` was requested.
- Malformed string-context tokens silently dropped at 5 absorption sites
  (`primops.cc:279, 4041, 4948-4957, 7322-7324, 8275-8277`) — a context token that
  fails to parse is by definition a v3 bug; skipping deletes a dependency edge from
  the drv. Assert/throw on the derivation path.
- `primReadFile` `primops.cc:3463-3473`: `catch (...)` after `realisePath` retries
  plain `ifstream` and reports "does not exist" — masks IFD build failures, can read
  stale content. Narrow (contrast `primReadDir :3544` which rethrows correctly).

---

## 3. P1 — Hang class: under-applied PAPs vs the force-handshake (one helper fixes all)

The V3_IS_OP fix (f5875a89c) closed one instance; the same blind spot persists at
every other rewind-handshake site. Pattern: `if (isAppLike()||Thunk||Slot) { ip--;
flags|=CFF_FORCE_RETRY; goto op_force_slow; }` — a PAP survives the chase unchanged
(`vm.cc:7173`), re-entry re-tests the same tag → **infinite loop where TW raises a
type error**.

- **C-8** `deepForceList` element scan (`vm.cc:10744-10760`) has no PAP exclusion
  (the arg scan at `:10660` was fixed; elements were not — and `:10660` itself only
  tests `Tag::App`, missing App3 PAPs). Affected: foldl' (incl. `lib.pipe`'s function
  list!), concatLists, listToAttrs, catAttrs, partition…
  Repro: `builtins.foldl' (acc: f: f acc) 0 [ (add 1) (add 2) ]` with 2-arity add.
  Also: the PTR writeback branch (`vm.cc:2409-2417`) has **no tag guard at all** —
  writes PAPs and Blackholes through.
- **C-9** OP_HEAD `:10946`, OP_TAIL, OP_LENGTH, OP_ELEM_AT, OP_LIST_CONCAT `:7770`,
  OP_ATTRS_UPDATE `:9226`, OP_STR_CONCAT `:10013`, OP_R_PRIMOP2 `:10826` — same loop;
  e.g. `builtins.head ((a: b: a) 1)` hangs; TW errors.
- **C-10** App3 PAPs invisible to OP_CALL/OP_TAIL_CALL PAP detection
  (`vm.cc:4745, 4766-4774, 4891-4898, 5872-5878` test `Tag::App` only) → iter-force
  ping-pong.

**Fix**: one shared predicate
`needsForce(v) = (Thunk|App|App3|Slot) && !isUnderappliedClosurePap(v)` used at every
handshake; PAP operands then fall through to the existing type-error paths. Negative
tests per opcode (PAP operand → TW-matching error, currently hangs).

**Strategic root fix (LOW-6)**: the lowering KNOWS closure arity
(`Function.extraParams`) but never materializes it — emit an arity byte in
OP_MAKE_CLOSURE and cache remaining-arity in App/App3 cells ⇒ O(1) "is this a PAP"
everywhere, retiring this entire bug family (PAP spin, SELECT poisoning, this hang
class) at its root, and deleting spine walks from 4+ hot paths.

---

## 4. P1 — Laziness divergences (v3 forces what TW never forces)

- **C-11** Stale `CFF_FORCE_RETRY` never cleared on synchronous force resolution
  (`vm.cc:7281-7286` clears WB bits but not RETRY; only OP_RETURN's pop path
  `:6956` clears it; OP_TAIL_CALL preserves stale flags `:5919-5930`). The frame's
  next legitimate lazy return then gets spuriously forced. **This is the exact
  signature of the open libsForQt5 deferral** ("v3 forces `inherit (pkgs) lib` while
  pkgs is BLACK; TW never enters that thunk"). Fix: clear RETRY in
  `applyForceWriteback` success + at the synchronous exit + on tail-call retarget.
  Validate with a NIX_TRACE_EVAL force-order diff on the libsForQt5 repro.
- **C-12** Strictness analysis marks `with x` as forcing `x`
  (`opt_func_strictness.cc:100-103`) — the runtime is lazy (#686 fix) but the
  *analysis* isn't, so `f = x: with x; 1` gets `strictArgs[0]=true` and call sites
  de-thunk the arg. `f (throw "boom")` → TW `1`, v3 throws. The #745 cross-fn
  fixpoint then spreads the wrong fact. Fix: With case must not insert `x.attrs`
  (only WithLookup forces).
- **C-13** `elem` registered fully strict (`primops.cc:9486/9663` default
  `lazyArgs=0`) but TW never forces the needle when the list short-circuits
  (`libexpr/primops.cc:4057-4067`): `builtins.elem (throw "x") []` → TW `false`,
  v3 throws — and the wrong mask also feeds the strictness analyzer. Only 19/216
  registrations set lazyArgs explicitly; sweep the rest against TW
  conditional-forcing. Fix elem now; add the audit lint.
- **C-14** `foldl'` (+`__foldlMap`, +C `groupBy`) carry `deepForceList` bits that
  pre-force elements TW passes unforced (`primops.cc:9462, 9471, 9526`; TW
  `prim_foldlStrict` `libexpr/primops.cc:4133-4152` doesn't force elems) — the same
  justified-by-comment reasoning the T4 filter fix already declared wrong.
  `builtins.foldl' (a: x: a) 0 [1 (throw "boom")]` → TW `0`, v3 throws. Also
  `primFoldlMap` eagerly forces `f x` (`:1375-1379`), so default-on stream fusion
  changes semantics for element-ignoring ops.
- **C-15** `resolveCalleeLambda`'s structural fallback matches same-block LetRecs **by
  entry name only** (`opt_strict_call_unthunk.cc:508-543`) and can return a different
  function's strictness; require uniqueness or return nullptr.
- **C-16** `valueEqual` force-evaluates ALL container elements up front
  (`vm.cc:640-657, 700-717`) — TW short-circuits per element:
  `[1 (throw "x")] == [2 3]` → TW `false`, v3 throws; also O(n) forced work on early
  mismatch (perf: `lib.unique`/`elem`). And PAPs fall to raw-pointer equality in the
  `default:` arm (`:737-746`) instead of the function arm.

---

## 5. P1 — ChainBindings guard gaps + semantic parity (primops)

- **C-17** `zipAttrsWith` (C, **the production path**) iterates `entries[]` raw with
  no `isChain()`/materialize guard (`primops.cc:2706-2713`) → silently drops parent
  entries of any `//`-composed input. The comment claiming the bytecode form is the
  default is stale/inverted (`bytecode_primops.cc:671` makes bytecode opt-in).
  **C-18** `toXML` same gap (`primops.cc:7351-7380`) + missing TW `<derivation>`
  special-case. **Class fix**: grep ALL remaining raw `entries[]` reads for
  chain-safety (the Lever-A audit pattern); add a debug-build "iterating a chain
  raw" assert in the accessor.
- **C-19** Purity holes (store-path-affecting): `getEnv` reads the real environment
  under pure/restricted eval (`primops.cc:1466-1475`; TW returns `""`),
  `currentTime` always wall-clock, `storePath` lacks pure-eval rejection. The
  `PRIMOP_IMPURE/RESTRICTED/INTERNAL` flags exist (`primop.hh:309-333`) but are set
  nowhere and enforced nowhere. Implement enforcement at OP_CALL_PRIMOP.
- **C-20** `currentSystem` is a compile-time constant (`primops.cc:3369-3383`),
  ignoring `--system`/`settings.thisSystem` → wholesale drvPath divergence under any
  system override. Read via the FFI leaf like `primNixVersion`.
- **C-21** `sort` uses unstable `std::sort` (`primops.cc:8381-8393`); TW guarantees
  stability (peeksort) and is resilient to non-strict-weak comparators where
  `std::sort` is UB. Equal-key reorders can reach drv args → hash divergence.
  Use stable_sort minimum; port peeksort ideally.
- **C-22** `builtins` attrset pollution: internal/v3-only primops exposed
  (`derivCoerce`, `foldlMap`, `splitString`, `parseInt`, …) via `getBuiltinsValue`
  (`vm.cc:11382-11470`); bare-vs-`__` alias picks are hash-order-dependent when the
  two registrations carry different flags (foldl' does!). Feature-detection
  (`builtins ? x`, attrNames) diverges from TW.
- **C-23** smaller parity items: `lessThan` rejects paths (TW compares);
  `genericClosure` keys stringified (float collisions at 1e-6, int/float mixed
  throws where TW compares numerically, bools accepted where TW rejects);
  `substring` rejects coercible 3rd args; `toString`/`toJSON` of a PAP hits a
  generic "tag=%u" error instead of TW's "cannot coerce a function".

---

## 6. P1 — Latent memory-layer correctness (live combos under default-ON GC)

- **M-1** `Bindings::materialize()` memo `s_matMemo` (`value.cc:100-147`) — a
  thread_local `unordered_map<const Bindings*, const Bindings*>` **never cleared,
  never walked as roots** — predates default-ON major GC. (a) flat copy reachable
  only via the memo gets swept → dangling return; (b) freed+reused chain address
  aliases a key → wrong Bindings for an unrelated chain. The pre-GC invalidation
  pass at `vm.cc:3076` clears the SELECT/rec ICs for exactly this hazard and missed
  this cache. **Fix now (cheap, always sound): clear it at the GC safepoint.**
  Also a live candidate feeding the firefox class (C-2.2).
- **M-2** String-context side-table keyed by arena char* (`alloc.hh:3856-3896`),
  swept only for whole-block frees — granule reuse (the moment Immix alloc flips on)
  silently transplants the previous string's context (#682 family). Sweep it against
  the mark bitmap per GC — prerequisite for M-7.
- **M-3** Nursery + major GC combo corrupts: the marker ignores nursery-resident
  cells (`tryMark` → `inActive` false → edges never walked) so arena cells reachable
  only through nursery cells get swept. Nursery is opt-in today; hard-disable major
  GC under `NIX_V3_NURSERY=1` (one line) until integrated.
- **M-4** Nursery C-stack rule (scavenge only at exitDepth==0) is enforced only by
  caller convention at the two vm.cc sites, not inside the scavenge entry points —
  assert it structurally. Also the free-list-reuse "no C-stack scan" blocking comment
  (`alloc.hh:1164-1177`) is stale: `walkCStackConservative` exists and runs.
- **M-5** FFI GC blind spots (dormant but load-bearing for #485/embedder):
  EvalScope `HandleSlot::payload` is never walked (`ffi.cc:806-823`); `fiber.cc`
  never does the GC root registration its own header documents as required.
- **M-6** `value_serialize` has no cycle/depth guard (`value_serialize.cc:280-330`)
  → C-stack overflow (uncatchable) instead of SerializeError on cyclic/deep values
  reaching the IFD/drvHash caches. Add depth cap + visited set.
- **OP_RETURN's STG-8 cell write** doesn't verify the cell still holds the returning
  thunk (`vm.cc:6831-6843`) — cheap two-compare guard recommended (interacts with C-1).
- **VM-13** `Thunk::shapeCell` raw write bypasses the barrier (`vm.cc:6848-6851`,
  gated `NIX_V3_CELL_EVERYWHERE`, no retirement criterion — repo rule 4 violation).

---

## 7. Performance — CPU (ranked by expected value ÷ effort)

The metric is instructions-retired/user-CPU; all claims below the line need the gate.

1. **P-1 Per-opcode GC safepoint TLS** (found independently by two agents — the known
   `_tlv_get_addr` #1 profile entry, exact structure): `vm.cc:3007-3102` executes per
   dispatched opcode: `threadArena()` TLS deref + `static thread_local
   s_majorGcThresholdBytes` (second TLS) + 2 magic-static guards + `bytesAllocated()`
   — and the `exitDepth==0` test sits *after* the TLS work, so nested dispatch loops
   (every callClosure2 per-element re-entry) pay it all for a check that can never
   fire there. **Fix**: hoist to dispatchLoop-entry locals exactly like the existing
   nursery pattern (`vm.cc:2870-2875`), or make `refill()` set a cheap global poll
   flag so the loop tests one non-TLS bool.
2. **P-2 Two magic-static guards per dispatch iteration**: `s_countOpcodes`/
   `s_countOpCycles` declared `static const` INSIDE the while body
   (`vm.cc:3190-3201`) — the exact anti-pattern the file's own comments at
   `:379-386`/`2793-2799` document fixing elsewhere. Promote to namespace scope.
3. **P-3 Allocation fast path**: `nurseryOrArena` does a nursery-TLS resolve + branch
   even with nursery permanently off, then `threadArena()` TLS again
   (`alloc.hh:2422-2426`); `static const bool s_reuseOn = getenv(...)` **inside
   `alloc()`** (`alloc.hh:1240`) = guard-byte load per allocation (the #768 fix
   missed this one); cellStarts/cellTypes metadata writes go through
   vector-of-vectors `.back()` double indirection (`alloc.hh:1276-1288`).
   Cache Arena*/Nursery* per dispatch loop; keep current-block bitmap base pointers
   as raw Arena members.
4. **P-4 Force spine**: `op_force_slow`/`forceValue` heap-allocate a
   `std::vector<Value> rights; reserve(8)` per non-memoized App force
   (`vm.cc:7208, 12319`) — use an inline `Value[8]` + heap fallback (observed depth
   ≤4); spine walks never consult inner pairs' `evaluated` memos → shared partial
   chains re-applied repeatedly.
5. **P-5 Saturated calls**: `callClosure2` exists only for foldl'
   (`vm.cc:13164-13223`); the **sort comparator** still curries one throwaway PAP
   pair per comparison (`primops.cc:8383-8384`, on top of sort's open super-linear
   XFAIL) and `filterSource` likewise (`:7885-7886`). Route through callClosure2;
   add generic callClosureN mirroring OP_CALL's saturated-entry block.
6. **P-6 Pre-switch gate chain**: ~8 dependent conditional branches before every
   `switch (op)` (`vm.cc:3291`); after P-1/P-2, fold remaining default-off gates
   into one `g_slowPathMask` test. Computed-goto is a bigger lever — measure the
   mask first (measure-twice).
7. **P-7 Mark phase O(edges×blocks)**: `tryMark`→`inActive`→`regionOf` linear-scans
   all blocks per visited pointer (`alloc.hh:2148-2159`; comment says "~36 blocks",
   M5 has 424; mark = 11.6 s) and `markLinesForCell` ignores the existing
   `blockIndexContaining` sorted index (`alloc.hh:1732-1769`). Drop the precheck;
   route through the index.
8. **P-8 Per-GC dead work**: `rebuildFreeSpansFromLineMarks` runs unconditionally
   (consumed only under Immix alloc, default-off); line-marking during mark likewise.
   Gate on `g_immixAllocEnabled` — or flip Immix on (M-7) and make them load-bearing.
9. **P-9 emit-level fusion headroom** (needs-measurement): ~48% of iteration opcodes
   are GET_LOCAL/FORCE/SET_LOCAL stack motion; FORCE+GET_LOCAL and paired-operand
   fusions, plus a saturated-call superinstruction at emitted call sites (enabled by
   the LOW-6 arity byte). SET_LOCAL_KEEP's +1.4 % shipped win is the template.
10. **P-10 valueEqual short-circuit** (C-16) is also a perf item: O(n) forces on
    early-mismatch comparisons.
11. **P-11 beta-reduce coverage**: `bodyIsSimple` rejects any body containing
    If/With/And/Or sub-blocks (`opt_beta_reduce.cc:319-333`) — the pass fires mainly
    on toy shapes; clone support for If sub-blocks exists in
    opt_strict_call_unthunk's machinery. Also multi-use-via-alias inlining clones N
    body copies (code bloat, `:438-452`).
12. **P-12 FFI hot-path costs**: disk-cache hit pays a SQLite UPDATE per hit
    maintaining an LRU index nothing consumes (`disk_cache.cc:409-415` — no DELETE
    exists in the file); context elems re-parsed/printed as strings per crossing;
    `applyClosure` allocates a fresh ~1 MB VMState per call (test-only today).

## 8. Performance — Memory (ranked)

1. **M-7 Finish Immix line reuse (the big lever, now unblocked)**: default config
   reclaims only fully-dead 16 MB blocks + huge cells (`alloc.hh:1125-1291`:
   span reuse gated `V3_DBG_IMMIX_ALLOC`, free-list gated, default = pure bump).
   Measured 46-50 % fully-dead *lines* (265 MB hello / 796 MB HNE recoverable) stay
   resident. The GC_PAUSE doc paused this at the projection stage, but its blocking
   pin (page release) has since landed and the stale "no C-stack scan" rationale is
   fixed (M-4) — legitimate recalibration per the threshold-recalibration rule.
   **Prereqs: M-1 + M-2 first** (pointer-keyed caches must be GC-coherent before
   granule reuse). Pre-commit a ≥150 MB firefox bar.
2. **M-8 Thunk header 56 B → 40 B**: `forces` (debug-only), `shapeCell` (default-off
   feature), `cellContainer` (nursery-only, derivable via `findContainingCellStart`)
   ≈ 20 dead bytes/Thunk in production (`closure.hh:90-195`). Thunks ≈ 320 MB on
   HNE-class → order 50-100 MB. Re-measure with NIX_V3_THUNKS_ATTR at 8 B-Value HEAD.
3. **M-9 GC metadata 7.1 % of arena**: per 16 MB block, cellTypes alone is 1 MB (a
   full byte per 16 B granule, 9 enum values → 4 bits suffice) + cellStarts 128 KB +
   lineMarks 16 KB (`alloc.hh:2315-2323`) ≈ **43 MB on firefox's ~600 MB arena**.
4. **M-10 ImportCache/CU retention**: `results` unbounded by default
   (`NIX_V3_IMPORT_CACHE_MAX_ENTRIES` default 0); the CU deque is **never** evicted
   even with the gate on; each CU carries its own symbolTable/stringConstants
   duplicating the global table across hundreds of CUs (`primops.cc:5867-5944`,
   `bytecode.hh:556-562`; ~700 MB "elsewhere" on HNE). The PHASE_4B LRU falsification
   predates munmap landing — re-measure (legitimate recalibration). Intern CU string
   tables against the global table.
5. **M-11 ValuePair `third`** dead for App/PrimOpApp (25 % of pair bytes; ~15 MB
   HNE-scale): split 24 B App/PrimOpApp vs 32 B App3 size classes. Fold into the next
   repr change (consistent with the T3/T6 deferral).
6. **M-12 disk_cache growth**: no eviction at all (FFI-6 above) — startup size-capped
   purge or stop maintaining `last_used`.

---

## 9. Measurement-integrity traps (these corrupt the team's own A/Bs)

- **T-1 CU disk-cache key omits ~20 codegen-affecting env gates**
  (`primops.cc:6484-6508` keys on path+content+schema+opcode-fingerprint only; the
  emitted bytecode depends on `NIX_V3_NO_DEFER/NO_FUSE_SETGET/NO_CALL_N/NO_OPT/
  OPT_PHASE_LIMIT/...` read at emit time across emit.cc/opt_*.cc). **One bisect run
  with a gate set poisons every later warm run** — and vice versa. A `NIX_V3_NO_*`
  A/B with a warm cache measures the wrong arm. Fix: hash the canonical gate list
  into the key (lint-enforced), or auto-disable the disk cache when any such gate is
  present. This may retroactively explain "irreproducible" bisect results.
- **T-2 Silent TW fallback inside `nix eval`** (the one v3-enabled command):
  `--write-to`, `--arg/--argstr`, and **all flake installables**
  (`nixpkgs#hello`) silently run TW under `NIX_V3_DIRECT_EVAL=1`
  (`src/nix/eval.cc:50-90, 311-319`). Add a one-line stderr warning + opt-in
  `NIX_V3_REQUIRE=1` hard-fail — bakes the gate's ENGAGED check into the binary.
  Full CLI coverage table: only `nix eval --expr/--file` (plain) and standalone
  `v3-eval` engage v3; nix build/run/develop/repl/flake-*/legacy = TW.
- **T-3 `v3-eval --expr` default skips `optimise`** (`cli/v3-eval.cc:30-36`) —
  production-comparable numbers require `--optimize`.
- **T-4 Unconditional per-GC stderr line** `"v3 mark-split: ..."`
  (`mark_sweep.cc:1875-1885`, outside the NIX_VM_STATS gate) pollutes byte-identity
  harnesses that capture stderr.
- **T-5 IFD EvalResults cached by store path only** (`primops.cc:6316-6321`) and
  exported into AOT snapshots — input-addressed outputs can change content under the
  same path (GC + non-deterministic rebuild) and across machines. Key on narHash;
  exclude from AOT distribution.
- **T-6 canonicalHash is insertion-order-sensitive on string context**
  (`value_serialize.cc:641-643` claims sorted; contexts live in an insertion-ordered
  vector) → spurious cross-process cache misses. Sort at serialize.
- **T-7 `NIX_V3_NO_DISK_CACHE` doesn't cover the IFD EvalResults table**
  (only `NIX_V3_NO_IFD_IMPORT_CACHE_DISK` does) and the stats counters violate
  `lookups == hits + misses`.
- **T-8 name-keyed `alwaysWHNFPrimOps` whitelist** (`opt_strictness.cc:104-204`)
  vs swappable bytecode primop implementations: a bytecode override that returns a
  non-WHNF tail breaks the elided-Force invariant silently. Move the fact into
  `PrimOp` (per-registration `whnfResult`), cleared by `installBytecodePrimop`.

---

## 10. Audited-clean (so the team doesn't re-audit)

- emit.cc eager-vs-lazy (#686/WC-38 class): With/Assert/If/list-elems/string-interp/
  inherit-from all match TW laziness; no new instances found.
- #668 defer leakage: all five branch ops flush; every sub-block passes through
  `emitBlock`'s `flushAllDeferred`.
- opt_const_fold/opt_primop_fold: all ~23 folds refuse TW-throwing inputs (overflow,
  div-by-zero, INT64_MIN/-1, OOB, NaN compares); folds fire only on context-free
  literals. One stale comment (NaN `<` "TW throws" — it doesn't; `opt_const_fold.cc:152`).
- CSE (per-block, whitelisted-pure), DCE (pure+unreferenced only), betaReduce sharing
  (VarId alias substitution — an initial work-duplication claim was checked and
  falsified), strictness fixpoint framework (monotone; the defects are its *inputs*:
  C-12/C-13/C-15).
- `tryEval` semantics (catches AssertionError/ThrownError only; v3 internal errors
  correctly escape — the risk is upstream masking, C-6).
- Inherit lowering itself (`cli/lower_v3.hh:785-1184`): per-entry isolated thunks,
  symbol interned per entry, no shared mutable state — X's *construction* is the
  corruption site, not inherit emission. The IR `AttrSetSetInheritFrom` deferred path
  has no producer (dead, but its tracking assumption lives on in serialize.cc — C-2.1).
- Write-barrier coverage at ~87 sites is consistent; barriers cost one predicted
  branch when gated off. The recurring defect class is *unwalked pointer-keyed side
  tables*, not missed barriers.
- Re-entrancy: bytecode-primop install recursion, nested runRootExpr (STG-10),
  eval-batch transactions — all guarded.

---

## 11. Recommended execution order

**Sprint 1 — kill the residuals + stop the masking (correctness, ~days):**
1. C-6 loud-failure inversion for derivationStrict (+ C-7 sweep). Unmasks everything
   else; any remaining divergence becomes an error at its source.
2. C-1 KEEP-disarm fix + C-5 probe run on firefox (step 0 is zero-code). Run the
   C-2 zero-code A/Bs (cold cache; NO_MAJOR_GC re-verify) the same day.
3. M-1 s_matMemo clear at GC safepoint + C-3 IC name validation (both tiny; both are
   live aliasing hazards regardless of firefox).
4. C-4 Blackhole memoization exclusions → re-test ghc98.
5. C-8/9/10 via the shared `needsForce()` helper + per-opcode negative tests.

**Sprint 2 — laziness + parity (correctness, ~week):**
6. C-11 stale RETRY clear → re-test libsForQt5. 7. C-12 With strictness, C-13 elem,
C-14 foldl' deepForce bits, C-15 fallback uniqueness. 8. C-17/18 chain-guard class
sweep. 9. C-19/20 purity + currentSystem (store-path-affecting). 10. C-21 stable sort.

**Sprint 3 — measurement integrity (protects all future work):**
11. T-1 gate-aware cache keys. 12. T-2 fallback warning + NIX_V3_REQUIRE.
13. T-4 stderr gate. 14. T-5/T-6 cache key soundness.

**Sprint 4 — CPU (gate-measured, pre-committed thresholds each):**
15. P-1+P-2+P-3 (TLS/static hoists — the profiled #1; expect mid-single-digit %).
16. P-4 spine vector + inner-memo. 17. P-5 callClosure2 for sort/filterSource.
18. P-7 mark-phase indexing. 19. LOW-6 arity byte (perf AND retires the PAP bug
family — schedule early if Sprint 1 confirms more PAP fallout). 20. P-6 mask, then
re-evaluate computed-goto.

**Sprint 5 — memory (each behind a pre-committed bar):**
21. M-7 Immix line reuse (after M-1/M-2), ≥150 MB firefox bar. 22. M-8 Thunk 56→40 B.
23. M-9 cellTypes nibble-packing. 24. M-10 ImportCache LRU re-measure + CU symbol
interning. 25. M-11/M-12 fold-ins.

Rule-0 note: each Sprint-1 item names the hypothesis it kills (C-1: "firefox residual
is a stale-KEEP cross-write"; C-2.1: "it's a cache-remap permutation"; …). If C-1's
counter shows zero hits on firefox, the fix still ships (the mechanism is real) but
the residual investigation moves to C-2's ranked list — do not stack a new RCA letter.

---

*Compiled 2026-06-11 by 6-track parallel code review. Line numbers at HEAD of
`angerman/2.35-eval-profiling-v2` (vm.cc mtime 2026-06-11 17:41).*
*Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.
SPDX-License-Identifier: Apache-2.0.*
