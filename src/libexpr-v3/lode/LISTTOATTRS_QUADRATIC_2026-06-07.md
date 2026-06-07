# v3 builds large attrsets O(n²) — `listToAttrs` 127× at 200k (2026-06-07)

**Status:** CONFIRMED FINDING (scaling-verified), root cause **narrowed but not
line-pinned** — handed to the team to pin (it lives in the active
`dispatchLoop` / `forceValue` re-entry WIP). First real measurement off the
darwin-4 timing host + the [[MEASUREMENT_GATE_2026-06-07]] gate.

## 1. The finding

Building a large attrset incrementally is **O(n²)** in v3. Forced via `seq`
(no iteration, no getAttr — pure build):

| n | v3 (s) | TW (s) | v3/TW |
|---|---|---|---|
| 50,000 | 1.05 | 0.05 | 21× |
| 100,000 | 3.9 | 0.08 | 49× |
| 200,000 | **15.3** | **0.12** | **127×** |

v3 quadruples per doubling of n (3.7×, 3.9×) = **O(n²)**; TW is ~linear
(O(n log n)). The gap grows without bound. Memory 1.66×.

## 2. The kill-list — five hypotheses, each killed by measurement

This is [[feedback_measure_twice_cut_once]] in action: every guess about the
cause was **falsified by a measurement**, not by argument. Recorded so the team
doesn't re-walk these.

| hypothesis | killed by | verdict |
|---|---|---|
| `getAttr` is O(n) (linear scan) | 1M lookups on a 1000-set = 0.64 s; `lookupLocal` is binary search; `globalInternSymbol` is `unordered_map` O(1) | ❌ getAttr is O(log n) |
| `listToAttrs` sort/dedup/result-build | **same-name input (dedups to 1 entry) is STILL O(n²)** (15.6 s @200k); `primListToAttrs` code is genuinely O(n log n) | ❌ not the sort/result |
| allocation / arena `refill` O(blocks) | building n attrsets / n strings / n ints via `foldl'` all O(n) (~0.18 s @200k); `bindingsSetValue` is O(1) | ❌ allocation is O(n) |
| the new non-moving major GC mark | `gc_count=1`, `gc_total_ms=0` | ❌ GC not involved |
| `foldl'`+`genList` fusion masking | let-bound (fusion-blocked) lists forced via `foldl'` still O(n) | ❌ not a fusion artifact |

## 3. What it actually is (narrowed)

- **`sample` profile (6 s @ n=400k): 99.8 % in `dispatchLoop`** — *not* in
  `primListToAttrs`'s C++ (no `stable_sort` / `lookup` / `forceValue` callee in
  the hot path), *not* in any primop, *not* in GC.
- **Bytecode instruction count is O(n)**: `insns` = 1.30 M → 2.60 M (exactly
  doubles per 2×n). But 15.3 s / 2.6 M ≈ **5.8 µs per instruction** — ~1000×
  too slow for an interpreter. ⇒ a *few* opcode handlers each do **O(n) work**.
- Forcing the *same* genList elements via `foldl'` / `let`-bound lists is
  **O(n)**. So it is **specific to `listToAttrs`'s per-element `forceValue`
  re-entry** (primops.cc:1650, `forceValue(*state.vm, src->elems[i])`).
- **Same-name is also O(n²)** ⇒ the cost is tied to the **collect vector
  holding n entries live during the loop**, not to the result's size/key-count.

**Leading hypothesis:** each `forceValue` re-entry pays a cost that is **O(live
roots / stack)**, and listToAttrs grows that to O(n) as it accumulates entries
→ O(n²). Prime suspect: the **arena-noroot + non-moving-major-GC** machinery
(recent commits `8fd7c8d3b` arena-noroot default-on, `2de6d2b97` major-GC) —
e.g. a per-force safepoint / root-registration / write-barrier that scans or
touches the live set. (Unconfirmed — see §4.)

## 4. How to pin it (the deferred step — for the team)

Stopped here deliberately: pinning the exact handler means
reading/instrumenting `dispatchLoop` + the `forceValue` re-entry path, which is
active engine WIP. Concrete plan, cheapest-first:

1. **Symbolize the profile offsets.** `sample` already names the hot frame as
   `dispatchLoop + 21180 / 21012 / …`. Map those to source:
   `atos -o build/src/libexpr-v3/libnixexprv3.dylib -l <load-addr> 0x…<offset>`
   (or rebuild with `-g` + Instruments / `samply`). That gives the **exact
   opcode `case`** burning the time — the single most decisive step.
2. **Read that handler for an O(live) operation.** Look for a loop / scan /
   registration that scales with the live set or VM stack: a root walk, a
   safepoint, a nursery/arena card-scan, a `dirtyContainers` walk, or a
   re-scan of the operand stack on each `forceValue` re-entry.
3. **Confirm by toggling the suspected subsystem.** Re-run the §6 repro with
   the relevant env flipped (`NIX_V3_NO_PHASE_D=1`, any arena-noroot / GC
   opt-out). If listToAttrs drops to O(n), the culprit subsystem is confirmed
   (same-host-bisect, [[feedback_same_host_bisect]]).
4. **Cross-check the live-set hypothesis.** If a variant that holds fewer
   entries live during the force is O(n) while the accumulating one is O(n²),
   that nails "per-force cost ∝ live set."
5. **Fix shape:** make the per-`forceValue` re-entry cost O(1) (don't re-scan
   roots/stack per force; batch or cache the root set across the primop's
   loop). Validate against the §6 scaling + `--core` + nixpkgs byte-equality.

**Pre-committed SHIP gate:** listToAttrs 200k drops from 15.3 s to **≤ 0.5 s**
(restores ~O(n)), `--core` green, nixpkgs byte-identical.

## 5. Impact

Latent on typical workloads (most nixpkgs attrsets are small, so n is tiny).
But a **hard cliff for large-collection construction** — big package sets, large
`listToAttrs` / `builtins.fromJSON` / generated attrsets. If any real eval
builds a 10k+ attrset incrementally, this is a multi-second tax. Worth pinning
because the fix is likely small (an O(n)→O(1) per-force change) and the same
re-entry path is hit by every primop that forces many values
(`map`, `filter`, `concatMap`, …) while holding results live.

## 6. Reproduce (on the darwin-4 timing host)

```bash
ssh root@aarch64-darwin-4.lan
cd /var/root/iohk-nix
export NIX_CONFIG="experimental-features = nix-command flakes"
NIX=/var/root/iohk-nix/build/src/nix/nix
# O(n^2): quadruples per doubling
for n in 50000 100000 200000; do
  e="let s = builtins.listToAttrs (builtins.genList (i: { name = toString i; value = i; }) $n); in builtins.seq s 0"
  echo -n "n=$n v3="; { /usr/bin/time -l env NIX_V3_DIRECT_EVAL=1 "$NIX" eval --expr "$e" >/dev/null; } 2>&1 | grep -oE '[0-9.]+ user'
  echo -n "      TW="; { /usr/bin/time -l "$NIX" eval --expr "$e" >/dev/null; } 2>&1 | grep -oE '[0-9.]+ user'
done
# profile (find the hot dispatchLoop offset):
e="let s = builtins.listToAttrs (builtins.genList (i: { name = toString i; value = i; }) 400000); in builtins.seq s 0"
NIX_V3_DIRECT_EVAL=1 "$NIX" eval --expr "$e" >/dev/null 2>&1 & PID=$!
sleep 5; sample $PID 8 -file /tmp/lta.txt; kill $PID
sed -n '/Sort by top of stack/,/Binary Images/p' /tmp/lta.txt
```

---

Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.
SPDX-License-Identifier: Apache-2.0
