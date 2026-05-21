# Cardano-node M5 — bisection findings 2026-05-21

## Status: M5 COMPLETES today with `NIX_V3_NO_NATIVE_CALL_FLAKE=1`

After #753 plugged the NIX_V3_MAX_HEAP safety hole, a structured
bisection (test/bisect-cardano-node-m5.sh) on cardano-node's
`packages.aarch64-darwin.cardano-node.name` target pinpointed the
remaining M5 blocker to **v3-native callFlake** (`88199c4a0`,
default-on as of `511074ff6`).

```
| target                                | v3-native callFlake | TW callFlake | TW (reference) |
|---------------------------------------|---------------------|--------------|----------------|
| .outputs.lib or 1                     | 9.3s OK             | --           | 14.5s OK       |
| .outputs.devShells.aarch64-darwin     | <5s OK              | --           | <5s OK         |
| .outputs.hydraJobs                    | <5s OK              | --           | <5s OK         |
| .outputs.legacyPackages.aarch64-darwin| killed (4G watchdog)| <5s OK       | <5s OK         |
| .outputs.packages.aarch64-darwin      | killed (4G watchdog)| <5s OK       | 8.3s OK        |
| .packages.aarch64-darwin.bech32.name  | killed              | --           | 6.8s OK        |
| .packages.aarch64-darwin.cardano-node.name (M5) | killed    | **10.2s OK** | 7.97s OK       |
```

(All v3 runs: `NIX_V3_DIRECT_EVAL=1
NIX_V3_SKIP_INSTALLABLE_PREEVAL=1 NIX_V3_MAX_HEAP=4G
NIX_V3_MAX_WALL_TIME=120s` on a local
`path:/Users/angerman/Projects/iohk/cardano-node` checkout.)

## Headline

```
v3 + TW-callFlake M5:  wall 10.2s, peak_rss 919 MB
TW M5:                 wall  7.97s, peak_rss 898 MB
gap:                   1.28× wall, 1.02× memory
output: byte-identical "cardano-node-exe-cardano-node-10.6.1"
```

## Where it falls over with v3-native callFlake

`.outputs.packages.aarch64-darwin` and `.outputs.legacyPackages.
aarch64-darwin` blow past the 4 GB watchdog cap in ~5 seconds —
even on `builtins.typeOf` (which should not force the inner
attrset).  Other outputs (`devShells`, `hydraJobs`) work fine.

Both blowup targets go through the cardano-node flake's
haskell.nix machinery (`pkgs.haskellPackages.mkProject`-style
attribute generation).  Other outputs do not.  The native
callFlake path constructs the outputs attrset in a way that
forces something the TW callFlake bridge defers.

## What we changed this session

1. **#753 safety fix** (`5af3dd1b1`) — added SIGALRM RSS watchdog
   + `setrlimit(RLIMIT_AS)` + in-dispatch RSS poll.  Without this,
   the M5 attempt earlier in the day grew to 100+ GB.  With it,
   every blowup is caught cleanly inside 5 seconds and ~1 GB.
2. **bisect-cardano-node-m5.sh** — reproducer script for the
   ladder above.  Safe to re-run because of (1).

## What's NEXT

The v3-native callFlake pathology is a separate task.  Hypotheses
to investigate:

1. **Over-eager attrset construction in callFlakeV3**.  Per the
   `lode/CALLPACKAGE_BUG_2026-05-09.md` family of bugs, v3 has a
   history of constructing intermediate `prev`-bindings eagerly
   when TW defers.  The native callFlake's outputs assembly may
   trip the same shape.
2. **emitTreeAttrs port (#701 deferred)** — the still-deferred
   Phase 4b that would replace the last per-node bridge in
   callFlakeV3.  Without it, callFlakeV3 mixes v3-native attrset
   building with TW-Value marshalling, which is where the
   pathology likely lives.
3. **Strictness analysis on call-flake.nix outputs** — the
   compiled call-flake.nix may be lowering `outputs = self:
   ...` in a way that forces the user's outputs function on
   attribute access instead of on use.

Workaround for users today: set
`NIX_V3_NO_NATIVE_CALL_FLAKE=1`.  v3 + TW-callFlake M5 is at
parity with TW.

## Cross-references

  * `5af3dd1b1` (#753) — RSS watchdog safety fix.
  * `88199c4a0` (#700 fix) — v3-native callFlake current
    implementation (the broken default).
  * `511074ff6` — flip v3-native callFlake to default-on.
  * `lode/CARDANO_NODE_FEASIBILITY_2026-05-18.md` — original M5
    plan, predicted "1-12 hours, 4× memory" assuming the per-op
    gap from EXTEND_DERIVATION_INVESTIGATION.  Actual is much
    better (1.28× wall, 1.02× memory) — once the right callFlake
    path is picked.
  * `#701` (pending) — Phase 4b emitTreeAttrs port that may
    incidentally fix the over-forcing.
