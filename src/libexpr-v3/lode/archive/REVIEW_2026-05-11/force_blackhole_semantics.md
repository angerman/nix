# Force / WHNF / Blackhole semantic audit — TW vs v3

Scope: forcing, thunks, blackholes, exception propagation. Code-reading
only (no tests run). Citations use absolute paths and line numbers as of
branch `angerman/2.35-eval-profiling-v2` on 2026-05-11.

## Summary

- **TW caches failure-state inside `Value::Failed`; v3 has no Failed
  state and re-runs a thrown thunk body on every subsequent force.**
  Observable on `tryEval`-of-thrown-thunk patterns when the body has
  visible side effects (`builtins.trace`, allocation counters). The
  divergence is acknowledged in `vm.cc:8421-8429` and is exactly the
  scenario the upstream test
  `tests/functional/lang/eval-okay-tryeval-failed-thunk-reeval.nix`
  exercises.

- **v3 has TWO blackhole representations** — a `ThunkState::Blackhole`
  marker on a `Thunk*` (analogous to TW's `eBlackHole` sentinel) AND a
  separate `Tag::Blackhole` Value singleton (`Value::vBlackhole`) used
  as a propagating "value not yet known" marker. The latter has no TW
  counterpart and is returned by `forceValue`/`OP_FORCE` for
  cross-VMState (foreign-frame) Black thunks. Consumers can therefore
  observe a `Tag::Blackhole` flowing through subsequent ops, which is
  fundamentally outside TW's model.

- **`OP_WITH_LOOKUP` is more forgiving than TW** — TW lets the very
  first `InfiniteRecursionError` from `forceAttrs(*env->values[0])`
  propagate (`eval.cc:948`); v3 catches `BlackholeError` per scope,
  records `anyBlackholed`, and continues to the outer scope
  (`vm.cc:759-772`, `vm.cc:799-808`). The outer scope may resolve the
  name where TW would have died.

## Confirmed divergences

### D1. No `Failed` state — thrown thunks re-run on subsequent force

**v3 site**: `src/libexpr-v3/vm.cc:8430-8453` (catch in forceValue's
Suspended->Blackhole driver) and `vm.cc:7023-7069`
(`clearBlackMarksOnException`). Catch path resets each Black thunk on
the unwound frames back to `Suspended`; the exception itself is NOT
stored, the thunk is re-runnable from scratch.

**TW site**: `src/libexpr/include/nix/expr/eval-inline.hh:137-158` —
TW's `forceValue` catches all exceptions and calls
`handleEvalExceptionForThunk` (`src/libexpr/eval.cc:2668-2685`), which
calls `v.mkFailed(e, recovery)`. Subsequent forces hit
`v.isFailed()` (`eval-inline.hh:186-188`) and rethrow via
`failed().rethrow()` (`src/libexpr/include/nix/expr/value.hh:451-464`).

**Behavioural difference**: TW evaluates the body exactly once and
caches the exception; v3 re-evaluates on every observation.

**Minimal Nix expression (and the upstream test)**:
```nix
let
  foo = builtins.trace "throwing" throw "nope";
in builtins.seq (builtins.tryEval foo).success
    builtins.seq (builtins.tryEval foo).success "done"
```
The expected `.err` is exactly one `trace: throwing` line
(`tests/functional/lang/eval-okay-tryeval-failed-thunk-reeval.err:1`);
v3 will emit two. The comment at `vm.cc:8421-8429` explicitly
acknowledges this gap.

### D2. Black-thunk infinite-recursion exception type differs

**v3 site**: `src/libexpr-v3/vm.cc:4488` (`OP_FORCE`) and
`src/libexpr-v3/vm.cc:8317` (`forceValue`):
```c++
throw BlackholeError("v3 OP_FORCE: infinite recursion (blackhole)");
```
`BlackholeError` is defined in
`src/libexpr-v3/include/v3/errors.hh:57-61` as
`class BlackholeError : public std::runtime_error`. **Not** derived
from `nix::EvalError` or `nix::InfiniteRecursionError`.

**TW site**: `src/libexpr/eval.cc:2655-2662` —
`ExprBlackHole::eval` calls
`ExprBlackHole::throwInfiniteRecursionError(state, v)` which throws
`nix::InfiniteRecursionError` (a `nix::EvalError`).

**Behavioural difference**:

1. Any TW-side `catch (EvalError &)` or `catch (nix::Error &)` would
   catch TW's exception but NOT v3's `BlackholeError` (since the
   latter derives only from `std::runtime_error`/`std::exception`).
   Callers that catch `std::exception` see both.
2. The bridge at `src/libexpr-v3/primops.cc:4003-4015` translates a
   v3 `Tag::Blackhole` value (NOT the exception) into TW's
   `mkBlackhole()` so a TW-side `forceValue` on the bridged result
   throws `nix::InfiniteRecursionError` via `ExprBlackHole::eval`.
   But the exception thrown FROM v3 directly (not via a
   Tag::Blackhole bridged value) keeps its v3 type as it crosses the
   boundary.

**Minimal expression that exposes the type**: any cycle that throws
from v3 without going through the Tag::Blackhole-value path, e.g.
`let x = x; in x` with `NIX_USE_V3=1 NIX_USE_V3_FORCE=1`. A consumer
filtering by `nix::InfiniteRecursionError` would miss it; only
`std::exception` (or `BaseError`-by-name string match) would catch.

### D3. `Tag::Blackhole` value propagation (no TW counterpart)

**v3 site**: `src/libexpr-v3/vm.cc:4443-4446` (`OP_FORCE`) and
`vm.cc:8062-8103` (`forceValue`). When a Black thunk is detected and
it is NOT on the current vm's frame stack (a cross-VMState / leaked
mark), v3 pushes `Value::vBlackhole` (Tag::Blackhole value) instead
of throwing. Default-on; opt out via `NIX_V3_NO_BLACKHOLE_AS_VALUE=1`
(line 8064).

**TW site**: no equivalent. TW always throws InfiniteRecursionError
on `eBlackHole` re-entry (`eval.cc:2660-2662`).

**Behavioural difference**: under v3, a downstream op may observe a
non-thunk Tag::Blackhole and propagate it. `coerceToString` at
`vm.cc:610-664` will then throw a `cannot coerce type to string`
runtime_error (case `Tag::Blackhole` at line 641 falls into the
default unhandled branch). Arithmetic ops will produce a type-error.
Both flavours of error are "an error happens" — but the surfaced
error message is different from TW's "infinite recursion" message,
and unwinds at a later op rather than at the originating force.

**Bridge protocol**: `primops.cc:4003-4015` converts Tag::Blackhole
to TW's `mkBlackhole()`, so when the value reaches TW its protocol
takes over. Forward direction works; the rounding does not preserve
identity (a sub-expression in v3 may have already consumed and
swallowed the Tag::Blackhole into a different error before the bridge
is reached).

**Minimal expression**: `NIX_USE_V3=1` running anything that surfaces
a cross-VMState cycle through bridge primops; cf. the comment at
`vm.cc:8035-8050` describing the protocol.

### D4. `forceValueDeep` keys cycle-detection differently

**TW site**: `src/libexpr/eval.cc:2730-2774` —
`std::set<const Value *> seen` keyed by Value-slot pointer; insert is
done BEFORE `forceValue`. Self-references via Value* identity are
short-circuited without forcing.

**v3 site**: `src/libexpr-v3/primops.cc:1457-1480` —
`std::unordered_set<const void *> seen` keyed by **container
pointer** (`Bindings*` for attrs, `ListVec*` for lists); insert is
done AFTER `forceValue`.

**Behavioural difference**: subtle but real. The insert-after-force
ordering means a Value whose force-loop chases back into itself (e.g.
SECD-style Tag::Slot cycle) will reach `forceValue`'s own cycle limit
(`vm.cc:7747`) and throw before v3 has a chance to short-circuit via
the seen-set. TW's pre-check would silently return.

Additionally: TW's per-slot keying means two distinct list slots
pointing to value-shared payloads each get visited (and re-force
shared sub-values, though the inner per-slot keying still dedupes).
v3's container-pointer keying dedupes earlier — IF two list slots
share the same Bindings, v3 visits the entries only once. In
practice the inner Bindings dedup converges on the same result, but
the ORDER of forces and where an exception originates differs.

**Minimal Nix expression**: `let v = [v]; in builtins.deepSeq v "ok"`
exercises both paths and both terminate cleanly (cycle through
ListVec is broken). A more illuminating case:
```nix
let inner = { x = throw "boom"; };
in builtins.deepSeq [ inner inner ] "ok"
```
TW will catch the first throw inside `recurse(*v2)` and rethrow with
trace `while evaluating list element at index 0`. v3 sees
`seen.insert(inner.bindings)` succeed on index 0, force the entries,
throw — same end result, but error trace order differs and the
second slot is not visited.

### D5. `OP_WITH_LOOKUP` swallows per-scope cycles; TW propagates the
first

**v3 site**: `src/libexpr-v3/vm.cc:715-1001` (`withLookup`). The loop
catches `BlackholeError` from a per-scope `forceValue`, sets
`anyBlackholed = true`, and continues to the outer scope
(`vm.cc:759-772`, `vm.cc:799-808`). Only if NO scope resolved the
name does it throw `BlackholeError` (`vm.cc:997-1000`).

**TW site**: `src/libexpr/eval.cc:946-962`
(`EvalState::lookupVar` for `var.fromWith`). On each step it calls
`forceAttrs(*env->values[0], fromWith->pos, ...)` — any
`InfiniteRecursionError` propagates immediately; no per-scope catch.

**Behavioural difference**: v3 can resolve a name from an outer scope
where TW would throw infinite-recursion. v3 is strictly more
permissive in cycle scenarios involving partially-constructed `with`
sources. Also, when v3 does throw, it throws `BlackholeError`
(std::runtime_error) carrying message
`"v3 OP_WITH_LOOKUP: cycle while resolving 'NAME'"` rather than TW's
"infinite recursion encountered".

**Minimal Nix expression** (mirror of the cardano-node case the
permissive path was added for):
```nix
let xs = rec { y = with xs; "x" + "y"; z = throw "no"; };
in xs.y
```
Worth checking in a small repro — depending on the rec construction
order, TW raises before the outer scope ever sees `y`, while v3 may
let the outer with see `y`.

### D6. Cross-VM Blackhole + partial-Bindings "STG WHNF recovery"
returns approximate WHNF

**v3 site**: `src/libexpr-v3/vm.cc:8132-8283` (`forceValue`) and
`vm.cc:4471-4485` (`OP_FORCE`). When a Black thunk is on `myFrames`
AND a partial Bindings was registered via
`publishToNearestBlackThunkFrame` /
`publishToAllThunkFrames` (`vm.cc:1316-1442`,
`vm.cc:1527-1660`), v3 returns the **largest chain layer's
Bindings** as a Tag::Attrs (see `pickLargestLayer`), AND taints the
current THUNK_RETURN frame with `CFF_TAINTED` so its OP_RETURN does
NOT memoize the result (`vm.cc:4047-4052` and `vm.hh:38-55`).

**TW site**: no equivalent. TW always throws on Black-thunk
re-entry.

**Behavioural difference**: this is a deliberate architectural
divergence enabling lib.fix-style cycles to make progress. The result
is "approximate" — a downstream consumer that reads the returned
Bindings sees whatever was registered up to that point, not the
final fix-point. Two readers at different points in the same eval
can observe different Bindings (different chain.back()). TW always
sees the same Failed/Black sentinel.

**Sharing/re-entry consequence**: if reader A sees `vBlackhole` and
reader B sees `Tag::Attrs (recovered)` because publishing happened
between A and B, they get different values for the same logical
thunk. Under TW, both readers would either both succeed (after the
body completes) or both fail (with the cached Failed).

Gated by `NIX_V3_NO_STG_WHNF=1` and `NIX_V3_NO_TAINT=1` for
bisecting. Default behaviour is to return approximate WHNF.

### D7. Slot doesn't update in place on first force (v3); TW always
writes WHNF into the slot

**v3 site**: `src/libexpr-v3/vm.cc:1869-1914` (`OP_GET_LOCAL_FORCE`)
and `vm.cc:4297-4357` (`op_force_slow`). The slot at
`stackBase + operand` is read, the thunk is chased through its
`Evaluated` field, but the slot itself is NOT mutated to point at the
WHNF. The thunk pointer is preserved; subsequent reads still go
`Tag::Thunk -> chase Evaluated -> result` (one extra indirection per
read).

The slot IS updated in `forceValue`'s memoSlot path
(`vm.cc:7797-7798`, `vm.cc:8478-8479`) but only when the call entered
via a Tag::Slot indirection.

**TW site**: `src/libexpr/include/nix/expr/eval-inline.hh:138-151` —
`expr->eval(*this, *env, v)` writes WHNF directly into the same
`Value &v`. Future reads see WHNF with no indirection.

**Behavioural difference**: pure performance, not correctness — but
worth flagging because it means v3 always pays the thunk-chase cost
on shared bindings. Counter-bug: if `Thunk::tail[]` upvalues are
zeroed at OP_RETURN (`vm.cc:4023-4030`) to help GC reclaim, but the
slot still holds Tag::Thunk pointing at the (now-stripped) thunk, the
chain `Tag::Thunk -> Evaluated -> WHNF` is the only path.

## Suspect areas (could not confirm in pure code-reading)

### S1. OP_FORCE Bridge handler vs forceValue Bridge handler
duplication

`vm.cc:4490-4546` (OP_FORCE Bridge) and `vm.cc:8319-8372`
(forceValue Bridge) share the same `forceBridgeThunk` call sequence
and the Self-Bridge guard (Tag::Thunk-on-self) AND the cell-update
protocol. The duplication looks intentional but the two sites diverge
in small ways (the OP_FORCE handler has the `ScopedActiveV3VM`
declared INSIDE the case so its lifetime ends at `break`; the
forceValue copy lives inside the chase loop with the same scope).
Any condition where one path's cell-update fires but the other's
doesn't would be a re-entry/sharing bug. Worth a focused diff.

### S2. `nUpvalues` survives OP_RETURN's `tail[]` clear

`vm.cc:4023-4030` zeroes `t->tail[ui]` but explicitly preserves
`t->nUpvalues` ("Don't reset nUpvalues — the FAM size was set at
alloc time; reusing the slot would require the count. Leaving it
preserves alloc-time invariants (Bridge thunks etc.)."). A
subsequent re-evaluation of the same thunk (e.g. via CFF_TAINTED
re-suspend) reads upvalues that are now `Value{}` zero. If the body
relies on those upvalues, the re-eval will produce a wrong result
silently — no "uninitialised" check. The tainted-then-re-suspend
path at `vm.cc:4047-4052` should be examined for this hazard:
specifically `OP_GET_UPVALUE` (`vm.cc:1938-1944`) on a cleared
slot would push `Value{}` (Uninitialized) and almost any next op
would throw a type error, but the timing matters — a Black-then-
Tainted-then-re-suspended thunk that gets its upvalues re-populated
elsewhere would be a corruption path.

### S3. `clearBlackMarksOnException` unwinds valueStack + withStack
but does NOT clear `cell`

`vm.cc:7023-7069` resets Black thunks to Suspended and shrinks
valueStack/withStack/frames, but it does NOT walk Black thunks on
the unwound frames to null their `cell` field. If a thunk had its
`cell` populated by `OP_THUNK_SET_LOCAL_THROUGH_CELL`
(`vm.cc:6086-6142`) before the body threw, the cell still points at
an allocated Value. The next force of the same thunk will go
Suspended -> Blackhole -> body re-runs; on success its OP_RETURN
will do `*cell = retVal; t->cell = nullptr;` at `vm.cc:4068-4071`.
This LOOKS fine, but if the cell was meanwhile read through a
Tag::Slot by some other code path during the re-run, the reader
sees the pre-throw value (the Tag::Thunk that was initially stored
in the cell at `vm.cc:6119-6120`) — not a thrown error, not a
WHNF. Whether this is reachable in practice depends on whether any
Tag::Slot can be captured before the cell-owning thunk runs to
completion the first time; the comment at `vm.cc:6119-6131` says
slot-references are how `inherit (X // Y) ...` thunks expose
themselves to per-attr thunks. Hazard: an `inherit-from` throw in
mid-construction leaves observers seeing a Black thunk via the
slot. Worth tracing.

### S4. `forceDeepRec` insert-after-force ordering versus
TW pre-check (mentioned in D4)

The exception trace order in cycle-then-throw scenarios likely
differs (D4 above flagged the case). I could not construct a
concrete divergence beyond the example listed; a small targeted
test running `forceValueDeep` and v3's `primDeepSeq` on the same
expression and comparing trace ordering would settle this.

### S5. `prim_tryEval`'s `MaintainCount trylevel` not present in v3

TW's `prim_tryEval` (`src/libexpr/primops.cc:1316-1340`) increments
`state.trylevel`; v3's `primTryEval`
(`src/libexpr-v3/primops.cc:6837-6870`) does not. The only use of
`trylevel` in TW (`src/libexpr/eval.cc:846-849`) is to print an
extra hint line on uncaught error during debug. Not a semantic
divergence for correctness — flagged only because the absence is
visible in diff and worth confirming there are no other readers
of `trylevel` that the v3 path could trigger via re-entry.

### S6. `OP_FORCE` window between `t->state = Blackhole` and
`pushCapturedWiths`

`vm.cc:4735-4818` sets `t->state = Blackhole` at line 4736, sets
the caller's `CFF_FORCE_RETRY` at 4748, resizes the value stack
inside a try/catch (lines 4752-4764, with revert), pushes the new
frame at 4810, then calls `pushCapturedWiths(vm, thunkWiths)` at
4819. If `pushCapturedWiths` throws (it can; it's a vector copy),
we have `t->state = Blackhole`, a fresh thunk-return frame on
`vm.frames`, and an inconsistent withStack. The outer
dispatchAndClear/forceValue catch will call
`clearBlackMarksOnException` which unwinds the frame and resets
state to Suspended (vm.cc:7023-7069). Probably correct, but the
revert path at 4755-4763 inside the inner try/catch is different
from the outer cleanup — small chance the inner revert leaves
`t->state = Blackhole` if its conditional `t->state = priorState`
at 4760 doesn't fire (priorState is Suspended already so the
revert is correct). Worth eyeballing.

### S7. `BlackholeError` vs `nix::InfiniteRecursionError` at the
bridge boundary

The bridge code (e.g. `v3_hook.cc:4084-4094`) catches
`std::exception` and rethrows `BlackholeError`. If a TW caller
expects `nix::InfiniteRecursionError` to surface from a v3-bridged
force, the type mismatch could mean:
1. A `catch (InfiniteRecursionError &)` won't catch v3's flavour.
2. Error-reporting code that checks the typed exception's `pos`
   field (`InfiniteRecursionError::pos`) gets no position info
   from `BlackholeError` (which has no `pos` accessor).
A small repro running `nix eval --apply` with a Black-thunk cycle
under `NIX_USE_V3=1` and inspecting the resulting error type would
confirm.

## Matches (negative results — these look semantically aligned)

- **App-spine walking**: both `vm.cc:4320-4348` (OP_FORCE) and
  `vm.cc:7806-7812` (forceValue) walk the App spine iteratively
  before calling the leaf. TW does the same via the iterative
  forceValue loop at `eval-inline.hh:167-185`. Same shape.

- **Iterative thunk chase**: TW's `forceValue` loops on
  `v.isThunk() || v.isApp()` (`eval-inline.hh:104-185`); v3's
  `forceValue` does the equivalent (`vm.cc:7737-8480`). Both avoid
  C-stack recursion on long thunk chains.

- **App-with-thunk-fun gets forced before call**: TW's `callFunction`
  (`eval.cc:1810`) and v3's `OP_CALL` (`vm.cc:2699-2719`) both
  force the function value first.

- **`primTryEval` catches `AssertionError` only**: TW
  (`primops.cc:1326-1334`) and v3 (`primops.cc:6837-6870`) match —
  abort, infinite recursion, and type errors all propagate past
  tryEval in both.

- **`primDeepSeq` semantics**: both walk lists+attrs recursively,
  forcing every reachable thunk, with cycle protection
  (`eval.cc:2730-2774` vs `primops.cc:1457-1480`). Modulo D4's
  keying discrepancy, the net set of forces and the throw-on-error
  behaviour match.

- **Cell-update on OP_RETURN propagates WHNF to slot observers**:
  `vm.cc:4068-4071` mirrors TW's in-place slot mutation in
  `expr->eval` writing into `Value &v`. Different mechanism, same
  effect for Tag::Slot consumers.

- **`forceAttrs` / `forceList` shape checks**: v3 lower emits
  OP_FORCE before each attr/list-shape-dependent op
  (`lower.cc` annotations) so the WHNF check is done at point of
  use, mirroring TW's `forceAttrs`/`forceList` wrappers
  (`eval-inline.hh:194-221`).

- **WC-37 ghost-frame fix**: the catch path at `vm.cc:7039-7060`
  explicitly truncates `frames`, `valueStack`, `withStack` to the
  first-popped-frame's bases on exception, preventing the pre-fix
  bug where stale frames would be picked up by a later
  OP_RETURN. This matches TW's stack-unwinding-by-C++-exception
  behaviour structurally.

- **The Self-Bridge guard (#520) in both OP_FORCE
  (`vm.cc:4509-4529`) and forceValue (`vm.cc:8327-8336`)**: when a
  Bridge thunk's `forceBridgeThunk` cache hits the same Thunk*
  back (because `getOrAllocBridgeThunkCached` is keyed by
  `nix::Value *`), v3 leaves `state == Bridge` rather than going
  `Evaluated -> Tag::Thunk{self}` which would chase-loop. Equivalent
  to TW's identity preservation for round-tripped function values.
