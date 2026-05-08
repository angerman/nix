# Publish/Recovery Use-Site Audit (2026-05-08)

Scope: every read/write site of the legacy `partialBindingsRegistry` /
`publishToNearestBlackThunkFrame` mechanism in `src/libexpr-v3/vm.cc`,
mapped to the STG-7/8 replacement (Tag::Slot + thunk->cell). The goal is
to identify cases where STG mode + KEEP_HOOKS hangs because a publish-era
backstop is missing.

Bisect signature: `NIX_V3_STG=1 + NIX_V3_STG_KEEP_HOOKS=1` hangs on
`(import <nixpkgs> {}).lib`; either flag alone works.

---

## 1. Use-site table

| Kind  | File:Line                              | Scenario                                                                          | Pre-STG behavior                                                              | STG-mode behavior                                                       | Replacement quality |
|-------|----------------------------------------|-----------------------------------------------------------------------------------|-------------------------------------------------------------------------------|-------------------------------------------------------------------------|---------------------|
| WRITE | `vm.cc:889` (in `publish…`)            | `OP_ATTRS_REC_INIT` runs while a Black thunk frame is active                      | Register `thunk -> Bindings*` so foreign forces / readers can recover         | NO-OP (`s_stgMode` early-return at line 856)                            | Replaced by `cell`  |
| WRITE | `vm.cc:928-930` (state-flip in publish)| Same path as above, only fires if NOT `s_disabled`                                | Eagerly flip outer Black thunks to Evaluated with the published Value         | NO-OP                                                                   | Not replicated      |
| READ  | `vm.cc:3010-3013` (OP_RETURN cleanup)  | OP_RETURN of a thunk frame                                                        | Erase registry entry (housekeeping)                                           | Still runs (registry is just empty); STG-8 cell-write at `vm.cc:3003`   | Replaced by cell    |
| READ  | `vm.cc:4456-4460` (OP_REC_BINDING_SLOT_REF, pre-force) | Pop a thunk-shaped src; if it's Black, recover its in-progress Bindings* before `forceValue` | Return a synthetic `Tag::Attrs` from the registry — proceed to binary-search | `s_stgMode_recref` at 4450: skip; falls to `forceValue(vm, attrs)` which throws BlackholeError | Mostly replaced by Tag::Slot — but **only if attrs was already a Slot or already Evaluated**. If attrs is still a Tag::Thunk that is Black on a foreign VM, this throws. |
| READ  | `vm.cc:4478-4486` (OP_REC_BINDING_SLOT_REF, post-throw) | `forceValue` threw BlackholeError; last-ditch registry consult | Return registry entry | Skipped under `s_stgMode_recref` | Same: relies on Tag::Slot path having been threaded |
| READ  | `vm.cc:5226,5236-5237` (`clearBlackMarksOnException`) | exception-recovery scrubs registry | Erase entries for all popped Black frames | Still runs (no-op since registry empty under STG) | OK |
| READ  | `vm.cc:6196-6210` (forceValue chase, Black branch) | A chase landed on a Black thunk; just before throwing, return its registered partial Bindings | Return synthetic `Tag::Attrs` → caller proceeds with the partial Bindings | `s_stgMode_recover` at 6190: skip; throws BlackholeError | **Not replicated when the source is a bare `Tag::Thunk` (no cell)** |

Cell-update / slot-set sites:

| Site | File:Line | Effect |
|------|-----------|--------|
| Cell ATTACH | `vm.cc:5174-5180` (OP_ATTRS_REC_SET) | If entry is fresh Suspended thunk, set `thunk->cell = &entries[i].value` |
| Cell COMMIT | `vm.cc:3003-3006` (OP_RETURN, CFF_THUNK_RETURN) | `*cell = retVal; cell = nullptr` |
| Slot CREATE | `vm.cc:4386-4390` (OP_REC_SLOT_PUBLISH) | Heap Value*, copy rec attrs in, push Tag::Slot |
| Slot CREATE | `vm.cc:4547-4549` (OP_REC_BINDING_SLOT_REF) | After resolving attrs, push `Tag::Slot(&entries[i].value)` |
| Slot DEREF on read | `vm.cc:439-457`, `5986-5996` (withLookup, forceValue) | Single-deref through cell |

---

## 2. Scenarios where STG path does NOT replicate publish-era behavior

### Gap A — Cross-VMState mid-eval rec-bindings read (`vm.cc:6196-6210`, `4456-4460`)

**Setup**: Outer v3 VMState is mid-`OP_ATTRS_REC_INIT`/`REC_SET` for a rec
attrset whose body has not yet returned. A KEEP_HOOKS re-entry runs TW (e.g.
`callFunction → eval hook → fresh body → forceValue`) and it forces a
`Tag::Thunk` that the OUTER frame is currently constructing (or selecting
through it).

**Pre-STG behavior**: `forceValue` at vm.cc:6196 consulted the registry,
got the outer's in-progress `Bindings*`, returned it as `Tag::Attrs`, and
the inner consumer found the partially-filled entries. This is exactly
TW's lazy-attr access on a partial env.

**STG behavior**: registry empty → throw `BlackholeError`. With
`NIX_V3_NO_BLACKHOLE_AS_VALUE` unset, `vm.cc:6244-6268` returns
`Tag::Blackhole` for foreign-VM Black — caller fails on the sentinel. With
the legacy fallback at `vm.cc:6298` it raises BlackholeError that propagates
through the lambda-skip retry loop and re-creates fresh VMStates. Either
way the chain doesn't terminate when KEEP_HOOKS is on (eval hook keeps
spinning new VMStates that all see the same outer Black).

**Why STG-8's cell update is insufficient**: `cell` is **only attached at
OP_ATTRS_REC_SET to fresh Suspended thunks** that hold per-attribute
sub-thunks; and **the cell is committed at OP_RETURN of THAT sub-thunk's
body, not at OP_RETURN of the outer rec frame**. So while the outer
rec-frame's body is still mid-evaluation:
- The outer thunk itself has no cell. (Its OWN cell could only have been
  set if it were stored at a parent's rec entry — usually not the case for
  the rec attrset literal that holds `final` in `lib.fix`.)
- Inner sub-thunks have cells but are still Suspended; their cell stays
  unwritten until each individual body returns.
- A foreign VM forcing the OUTER thunk gets the leaked Black mark, falls
  past the chase loop, hits 6013 (`Black` branch), and now lacks the
  partial-Bindings escape hatch.

**Most likely tripped by**: nixpkgs `lib/fixed-points.nix` `extends` and
`lib.fix`. The all-packages fix-point is the canonical case (issue #455
root cause memo). Cardano-node `(prev // overlay final prev)` is an
instance, but the report names `(import <nixpkgs> {}).lib` so the
plain-nixpkgs `lib.fix` of `composeExtensions self prev` is the immediate
trigger.

### Gap B — Stale partial published in publish-era was MASKING an eager-walk path (`vm.cc:928-930`)

**Setup**: Pre-STG, when an ATTRS publish fired, EARLY_PUBLISH eagerly
flipped every outer Black thunk on the frame stack to Evaluated with the
just-built `v` (default outermost-only, opt-in publish-all). That means
even unrelated outer Black thunks would receive a partially-correct value
they could later be re-forced from.

**STG behavior**: NO state-flip ever happens. Outer thunks stay Black until
their own body returns. If the consumer reaches one of them via a Tag::Slot
that was ALSO captured into a closure freeVars vector, the consumer is OK
(slot deref reads the Bindings*). But if the consumer captured the OUTER
thunk by Tag::Thunk (not Tag::Slot — common when emit didn't take the
heap-stable path because there's no ir::RecVar mapping), it's stuck.

**Most likely tripped by**: any rec-attrset in nixpkgs whose lowering didn't
emit OP_REC_SLOT_PUBLISH. Inspection of `emit.cc:568-574` shows
OP_REC_SLOT_PUBLISH only emits when `m.recVarToSlotVar.find(e.recVar)`
succeeds — i.e., the lowerer must have mapped `recVar → slotVar`. For
`with rec { … }`, `let rec`, and a number of inherited-from rec attrsets
in lib, the slot-var bookkeeping may not be installed (review
`SLOT_TAGGING_AUDIT_2026-05-07.md`). Those rec attrsets fall back to a
plain Tag::Attrs on the operand stack with no Tag::Slot, so closures that
reference the rec by `Local`/`Upvalue` go through a plain Tag::Thunk and
will hit the Black path under STG.

### Gap C — OP_REC_BINDING_SLOT_REF source still a `Tag::Thunk` (`vm.cc:4435-4493`)

**Setup**: Code like `inherit (rec { x = …; y = self.x; }) y` lowers to a
RecBindingSlotRef whose `attrs` operand is the rec-attrset's variable. If
the lowerer chose Tag::Thunk (not Tag::Slot) for that var, the runtime
handler has to `forceValue` it. Pre-STG, when the rec is currently being
constructed by an outer frame, the registry hand-off provided the partial
Bindings*. Under STG, this is a `forceValue` on a Black foreign-VMState
thunk — same failure mode as Gap A, except the call site is not
`forceValue` from the chase loop but from the OP_REC_BINDING_SLOT_REF
opcode handler.

**Most likely tripped by**: `lib/lists.nix` and similar files whose rec
attrsets are referenced cross-module. Visible in `repro-495-broader-thunkify-bug.nix`
shape (`lib.systems.elaborate`) — the symptom from `#496` lives here.

### Gap D — `withLookup` over a v3 thunk that's Black on foreign VM (`vm.cc:460-487`)

**Setup**: `with self;` where `self` was pushed onto the with-stack as
Tag::Thunk (not Tag::Slot — depends on whether `OP_WITH_PUSH` saw a
heap-stable slot). If the body re-enters TW and TW tries to reuse the v3
with-stack, it can't — TW has its own with-handling. But under KEEP_HOOKS
the eval-hook spins up a fresh v3 VMState whose `withStack` is empty. A
v3 OP_WITH_LOOKUP from inside an inner-VM body will not find the outer's
withs unless they're transferred. Pre-STG didn't fix this either, but the
publish path masked it for the `self`-shape rec when `self` was published.

**STG-10 (`f216c3bcb`) attempted to address this** by not spawning fresh
VMStates from re-entries when `activeV3VM()` is set (`runOnExistingVm`).
However, when the eval-hook is FORCED ON (KEEP_HOOKS), the
`s_refuseInActiveV3` short-circuit at v3_hook.cc:1589 still runs, falling
back to TW eval. TW eval cannot see v3's withStack — the with-lookup never
gets to v3. So this gap is REAL but more subtle: it shows up only when the
TW path itself wants a name from `with self;` and self is mid-construction.

**Most likely tripped by**: nixpkgs all-packages.nix `with self;` block —
the same pattern as #455.

---

## 3. STG_KEEP_HOOKS specific finding

When `v3CallFunctionEntry` is called from TW with a TW-lambda whose body
needs an upvalue resolving through a rec-bindings on the outer v3 VM:

1. Hook wakes, sees `activeV3VM() != nullptr`, refuses (v3_hook.cc:1589),
   falls back to TW eval.
2. TW evaluates the body. TW's env contains pointers to TW Values that
   wrap v3 Bridge thunks (created by treeWalkerToV3 / v3ToTreeWalker).
3. When TW forces one of those wrapped Bridge thunks (because the body
   accesses an upvalue from the v3 rec), it goes through
   `forceBridgeThunk → ns->forceValue → eval-hook re-entry → runs TW or v3`
4. If the body reaches an OP_FORCE on a v3 Tag::Thunk that is the
   in-progress rec frame on the OUTER v3 VM, **the slot/cell mechanism
   does not save us**:
   - The outer rec's `cell` field (if any) is unwritten because OP_RETURN
     of that frame hasn't fired.
   - The outer rec's body is still on the OUTER VM's frame stack as
     Black. The CHILD VM has no idea about the partial Bindings, and
     STG's `s_stgMode_recover` skips the registry consult.
   - `vm.cc:6298` raises BlackholeError. Without `NO_BLACKHOLE_AS_VALUE=1`
     it returns Tag::Blackhole instead, which propagates and the consumer
     either errors or chases-and-retries.
5. Result: hang under retry-loop, or BlackholeError surfaced as cycle.

**Conclusion: the slot-tagging path does NOT correctly hand the outer
Bindings* to an inner body when:**
- the lowerer didn't emit OP_REC_SLOT_PUBLISH (so no Tag::Slot in the
  outer's locals to propagate as upvalue) — affected by issues
  documented in SLOT_TAGGING_AUDIT_2026-05-07.md, or
- the upvalue was captured as Tag::Thunk because the closure-build site
  saw the rec as Tag::Attrs (not Tag::Slot) — emit.cc closure-capture path
  doesn't auto-promote.

The legacy publish path circumvented both by being a pure side-table:
the registry was thread-local, scanned by `forceValue` regardless of how
the consumer captured the rec. Removing it without first making sure
EVERY rec-attrset emit goes through OP_REC_SLOT_PUBLISH and EVERY
closure that captures one captures the Tag::Slot version is the proximate
cause of the KEEP_HOOKS hang.

---

## 4. Recommendations (analysis-only; no code changes here)

1. Audit every emit-time path that produces a rec-attrset value and verify
   it emits OP_REC_SLOT_PUBLISH OR registers a `cell` on a parent thunk.
   Cross-reference SLOT_TAGGING_AUDIT_2026-05-07.md.
2. Audit every `forceValue` callsite that can reach a Black thunk on a
   FOREIGN VMState (top of vm.cc:6013 branch). Under STG mode, return
   `Tag::Blackhole` is the only escape — confirm consumers handle it.
3. Consider a hybrid: keep `partialBindingsRegistry` as a READ-ONLY
   foreign-VM hand-off (writes still happen at OP_ATTRS_REC_INIT) but keep
   the local-cycle / chase semantics STG. This was the implicit pre-STG
   intent, broken by STG-1 making publish a no-op.
4. The cell update at OP_RETURN (b00879e88) addresses **completed** outer
   rec frames; it does nothing for **mid-evaluation** outer rec frames
   accessed cross-VMState. Either: defer KEEP_HOOKS until OP_REC_SLOT_PUBLISH
   coverage is universal, or restore publish writes (not reads) gated on
   STG mode.

---

## 5. File:line index

- `vm.cc:807-811` — `partialBindingsRegistry`
- `vm.cc:843-932` — `publishToNearestBlackThunkFrame`
- `vm.cc:854-856` — STG-1 publish kill-switch
- `vm.cc:889` — registry write
- `vm.cc:928-930` — pre-STG eager state-flip
- `vm.cc:3003-3006` — STG-8 cell commit
- `vm.cc:3010-3013` — registry housekeeping at OP_RETURN
- `vm.cc:3639,3706,3777,4263` — publish call-sites (REC_INIT + 3 non-rec)
- `vm.cc:4393-4551` — OP_REC_BINDING_SLOT_REF (incl. STG-2 reader skips)
- `vm.cc:4450-4451` — STG-2 recref kill-switch
- `vm.cc:5148-5181` — OP_ATTRS_REC_SET (cell attach)
- `vm.cc:5224-5239` — `clearBlackMarksOnException`
- `vm.cc:5697+` — `forceValue` (incl. Slot deref + Black branch + STG-3)
- `vm.cc:6190-6212` — STG-3 recover kill-switch
- `vm.cc:6244-6268` — Blackhole-as-value foreign-VM escape
- `v3_hook.cc:1545-1573` — eval-hook STG kill-switch + KEEP_HOOKS override
- `v3_hook.cc:3069-3088` — call-hook STG kill-switch + KEEP_HOOKS override
- `closure.hh:114-129` — `Thunk::cell` field
- `emit.cc:524-580` — LetRec emit (OP_REC_SLOT_PUBLISH conditional)
- `emit.cc:502-506` — RecBindingSlotRef emit
