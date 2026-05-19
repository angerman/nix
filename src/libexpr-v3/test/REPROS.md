# v3 repro fixture manifest

Per-fixture index: file → lesson it pins → original issue/commit.

This file is the source of truth for `test/run-repros.sh` (the automated
runner) and the human reference when investigating new bugs ("did we
ever see this shape before?").

Convention: every `repro-<topic>.nix` in `test/` MUST appear here with
at least the lesson + commit columns filled in.  New repros must be
added when a bug is fixed — see `LESSONS_LEARNED_2026-05-15.md` §4.8
on the bisect → fixture → permanent-guard workflow.

| Fixture | Lesson it pins | Original commit | Run modes |
|---|---|---|---|
| `repro-455.nix` | `pkgs ? lib` returns true (env-shape mismatch in `prev // overlay final prev`) | (RCA #455) | TW + v3-direct must agree |
| `repro-455-aliases.nix` | RCA #455 with aliases shape | (RCA #455) | TW + v3-direct must agree |
| `repro-495-broader-thunkify-bug.nix` | broader-thunkify upvalue bug (#495 / #496 / #497 / #498) | `b3a7c5e6e` family | v3-direct only |
| `repro-583-mapattrs-app-cache.nix` | mapAttrs Tag::App caching shape | (issue #583) | v3-direct only |
| `repro-583-tag-app-cache-negative-1.nix` | Tag::App cache: bad behavior under specific shape | issue #583 | v3-direct, negative |
| `repro-583-tag-app-cache-negative-2.nix` | Same family | issue #583 | v3-direct, negative |
| `repro-583-tag-app-cache-positive-1.nix` | Tag::App cache: correct behavior post-fix | issue #583 | v3-direct, positive |
| `repro-583-tag-app-cache-positive-2.nix` | Same family | issue #583 | v3-direct, positive |
| `repro-583-tag-app-cache-positive-3.nix` | Same family | issue #583 | v3-direct, positive |
| `repro-583-tag-app-cache-regression-1.nix` | Regression guard for fix | issue #583 | v3-direct, guard |
| `repro-a12b-op-call-iter-force.nix` | A12b OP_CALL iter-force fix | (A12b series) | v3-direct only |
| `repro-hello-name.nix` | Action plan Phase 1 exit criterion | (Phase 1 closure 2026-05-17) | v3-direct only |
| `repro-genericClosure-bytecode-filter.nix` | genericClosure callClosure WHNF-on-return | `b0a0ff2e1` (2026-05-17) | TW + v3-direct must match |
| `repro-removeAttrs-lazy-elements.nix` | removeAttrs element-WHNF force | within `7adc7e61f` | TW + v3-direct must match |
| `repro-isTrueValue-slot.nix` | Tag::Slot reaching OP_NOT via CFF_FORCE_WB_PTR_KEEP | within `7adc7e61f` | TW + v3-direct must match |
| `repro-app-memo-regression.nix` | Tag::App `evaluated`-field memo (pos + neg via gate) | `d3e41c13d` (2026-05-18) | both with + without `NIX_V3_NO_APP_MEMO=1` |
| `repro-hello-name-real.nix` | Real-nixpkgs hello.name smoke (Option 4 hybrid guard) | (session 2026-05-17/18) | v3-direct vs TW oracle |
| `repro-beta-reduce.nix` | IR Phase A semantic guard (8 patterns) | (session 2026-05-18) | TW + v3 ON + v3 OFF (NIX_V3_NO_BETA_REDUCE=1) all match |
| `repro-beta-reduce-perf.nix` | IR Phase A alloc-reduction guard | (session 2026-05-18) | v3 ON: 10 closures; v3 OFF: 19 closures (47% reduction) |
| `repro-primop-fold.nix` | IR Phase B semantic guard (11 patterns) | (session 2026-05-18) | TW + v3 ON + v3 OFF (NIX_V3_NO_PRIMOP_FOLD=1) all match; on static `length [1..10]`: insns 56→23, lists 1→0 |
| `repro-stream-fusion.nix` | IR Phase C semantic + perf guard (foldl'+map → __foldlMap) | (session 2026-05-18) | TW + v3 ON + v3 OFF (NIX_V3_NO_STREAM_FUSION=1) all match; N=100K perf budget 5s catches PrimOpCall-shape regression (60× slowdown) |
| `repro-lambda-lift.nix` | IR Phase D semantic + alloc guard (closure-free intern) | (session 2026-05-18) | TW + v3 ON + v3 OFF (NIX_V3_NO_LAMBDA_LIFT=1) all match; N=100 alloc guard: ON saves ~99 closure allocs vs OFF (224 vs 323) |
| `repro-path-with-context-coerce.nix` | Tag::Path coerceToString → /nix/store + `__structuredAttrs` env emission | `78fa43631` (2026-05-19) | TW + v3-direct must match |
| `repro-665-tostring-vs-derivcoerce.nix` | `toString` non-copying vs new `__derivCoerce` copying; bash bootstrap cascade fix | `7b2535fc9` (2026-05-19, #665) | TW + v3-direct must match |
| `repro-666-derivation-equality.nix` | `primops.cc` `valueEqual` derivation outPath special-case (was diverging from `vm.cc` `valueEqual`) — `builtins.elem` + `lib.unique` on derivations | `45da225c6` (2026-05-19, #666) | TW + v3-direct must match |
| `repro-667-assert-force.nix` | `OP_ASSERT` CFF_FORCE_RETRY iterative-force protocol (was the only bool-consuming opcode missing it) — unblocks gtk3 / firefox-unwrapped / firefox | `120def5bc` (2026-05-19, #667) | TW + v3-direct must match |
| `repro-668-defer-across-branch.nix` | #542 emit-time deferring leaked OP_SET_LOCAL ops INSIDE then-block via flushAllDeferred-on-block-entry; else-path saw mismatched stack depth → later OP_GET_LOCAL on Uninit slot → STR_CONCAT cascade → SIGTRAP on go.drvPath | (2026-05-19, #668) | TW + v3-direct must match |

## Run all repros

The umbrella driver `test/all-v3-tests.sh` invokes the dedicated
`run-*-tests.sh` shell drivers for repro families that have them
(e.g. `run-583-tag-app-cache-tests.sh` for the #583 family,
`run-broader-thunkify-tests.sh` for #495-498, etc.).

For the standalone `.nix` fixtures listed above, the simple recipe is:

```bash
for f in src/libexpr-v3/test/repro-*.nix; do
  tw=$(build/src/nix/nix eval --impure -f "$f" 2>/dev/null)
  v3=$(NIX_V3_DIRECT_EVAL=1 NIX_V3_SKIP_INSTALLABLE_PREEVAL=1 \
       build/src/nix/nix eval --impure -f "$f" 2>/dev/null)
  printf "%-60s TW=%-30s v3=%s\n" "$(basename "$f")" "$tw" "$v3"
  if [[ "$tw" != "$v3" ]]; then echo "DIVERGE"; fi
done
```

(Some fixtures are TW-only or v3-only by design — see the table.  The
driver above doesn't handle those; the per-family shell scripts do.)

## Coverage gaps (TODO)

The 2026-05-17/18 session identified the following missing repros;
they're listed here as homework rather than committed empty fixtures.
Once added, they go in the table above.

- (none currently; the 5 from the session are now committed)

If you fix a bug and don't add a repro: see Rule 0 in
`src/libexpr-v3/CLAUDE.md` (every commit must answer "what hypothesis
does this kill?") and `LESSONS_LEARNED_2026-05-15.md` §4.8 (bisect-to-
fixture workflow).

---

Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
Input Output Group. SPDX-License-Identifier: Apache-2.0
