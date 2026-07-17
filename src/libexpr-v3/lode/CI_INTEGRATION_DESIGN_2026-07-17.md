# v3 → zw3rk CI integration design (Hydra on linux-0 + darwin builders)

**Date:** 2026-07-17
**Scope:** concrete plan to put the shipped v3 evaluator into the real CI, grounded in `~/Projects/zw3rk/infra` (Hydra on `x86_64-linux-0`, darwin builder fleet). Not aspirational: every hook lands on a config knob that already exists in that repo.
**Copyright:** (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group. SPDX-License-Identifier: Apache-2.0.

---

## 0. TL;DR

The CI evaluator is **`nix-eval-jobs`**, spawned by `hydra-eval-jobset` on `linux-0`. It **forks `evaluator_workers = 20` worker processes, each capped at `evaluator_max_memory_size = 1024` MB** (`modules/web-service-hydra.nix:214-215`). Each worker independently re-imports the same nixpkgs + iohk-nix + haskell.nix and holds its own private copy of the CU bytecode. IFD is on (`allow_import_from_derivation = true`) and IFD builds (haskell.nix `plan-to-nix`) are dispatched over SSH from linux-0's `nix-daemon` to the aarch64-darwin builders.

That is a **one-to-one map onto the two CI pains and the shipped v3 work**:

| CI pain (user) | Hydra knob it shows up as | Shipped v3 lever |
|---|---|---|
| "memory too high to run evals in parallel" | `evaluator_workers=20` × `evaluator_max_memory_size=1024` (workers OOM-restart on haskell.nix) | **WS-5** in-place AOT mmap borrow → ~105 MB of CU shared once across all 20 workers instead of private per worker |
| "IFDs are invisible and block sequentially" | `allow_import_from_derivation=true` + SSH `buildMachines` + 3-hour eval watchdog | **WS-2** default-on IFD visibility + **WS-1** realise-context correctness |
| warm re-eval of the same jobset every push | `hydra-evaluator` re-runs the same expr on every jobset trigger | **WS-3** persistent worker + applied-import cache + AOT |

**The single real engineering item** was whether v3-direct engages inside `nix-eval-jobs` (a libnixexpr *embedder* that is not the `nix` CLI). **RESOLVED 2026-07-17 (§4 Phase 0): it does not, and cannot as-is** — the `forceValue` hook was removed (#735) and the v3 root entry is called from only the `nix eval` CLI + the `v3-eval` binary, nothing an embedder links. So integration = plumbing **plus one bounded nix-eval-jobs patch** whose template already exists in-tree (`runV3DirectEval`, `src/nix/eval.cc:50-180`). Everything else is flake-input + systemd-env plumbing with a `git revert`-able blast radius. **The version-coordination cost** is 2.34 → 2.35: the infra is pinned to a 2.34 nix; the v3 branch is `angerman/2.35-eval-profiling-v2`. Recommended vehicle: backport v3 onto the 2.34 base first (Option A), keep `nix-eval-jobs@2.34.1` and the Hydra ABI unchanged.

---

## 1. Topology (read from the repo, not assumed)

```
                          GitHub webhook
                                │
                    hydra-github-bridge (linux-0)
                                │  triggers jobset eval
                                ▼
   ┌──────────────────────────  linux-0 (x86_64)  ──────────────────────────┐
   │  hydra-evaluator.service                                                │
   │     └─ hydra-eval-jobset  (per jobset; killed at 3 h by watchdog)       │
   │           └─ nix-eval-jobs  v2.34.1                                     │
   │                 ├─ worker 1  (own EvalState, ≤1 GB)  ─┐                 │
   │                 ├─ worker 2  ...                       │  ← THE fleet   │
   │                 └─ worker 20 (own EvalState, ≤1 GB)  ─┘    WS-5 targets │
   │                                                                         │
   │  nix-daemon  ── IFD build (plan-to-nix) ──SSH──►  darwin builders       │
   │  Rust hydra-queue-runner ──gRPC──►  darwin builders (regular builds)    │
   └─────────────────────────────────────────────────────────────────────────┘
                                                    aarch64-darwin-{6..f}
                                                    (build only; no jobset eval)
```

Key facts, with sources:
- **Evaluator = `nix-eval-jobs`.** Confirmed by the eval watchdog comment "Also kills their child nix-eval-jobs workers" (`flake.nix:452,457`) and the `nix-eval-jobs` v2.34.1 lock entry (`flake.lock:1001`).
- **20 workers, 1 GB cap each** (`modules/web-service-hydra.nix:214-215`). These become `nix-eval-jobs --workers 20 --max-memory-size 1024`.
- **IFD on**, plan-to-nix builds routed to darwin via `buildMachines` SSH (`modules/web-service-hydra.nix:420-448`). The comment is explicit: "the Nix daemon used by the evaluator still needs SSH-based builders … haskell.nix plan-to-nix requires aarch64-darwin."
- **The nix under everything is already a patched fork.** `nix.url = github:input-output-hk/nix/angerman/2.34-ifd-profiling` (source, `flake.nix:25`), `hydra.inputs.nix.follows = "nix"` (`:28`), and `nix-pkg.url = …/angerman/2.34-ifd-profiling` (package flake, `:73`) → `nix.package` on linux-0 (`:409`) and every darwin host. **v3 is the next commit on this exact lineage** — the integration seam already exists.
- **Darwin builders do not evaluate jobsets.** They realise drvs (IFD + regular). So v3-on-darwin is a *developer* `nix build` concern, not the CI-eval bottleneck.

---

## 2. Where each shipped v3 lever lands

### 2.1 WS-5 (AOT mmap sharing) → the `evaluator_max_memory_size` wall

Today each of the 20 workers re-imports nixpkgs/haskell.nix and privately allocates the CU bytecode + descriptors (the largest read-only chunk). Measured HNE/haskell.nix AOT footprint is **126 MB** of CU; 20 private copies = ~2.5 GB of duplicated read-only data, and each worker's private total is what trips the 1 GB cap.

With WS-5: the parent maps **one** AOT file; every forked worker borrows code + descriptors **in place** from that shared, file-backed, read-only mapping. Measured (Linux, WS-5): 100 % of CUs borrowed, **`Shared_Clean = 105 MB` of the 126 MB AOT across concurrent processes**. Net effect on the knob:
- Per-worker *private* RSS drops by roughly the shared-CU amount → fewer workers hit the 1 GB cap on haskell.nix.
- We can **raise `evaluator_workers`** and/or **lower `evaluator_max_memory_size`** to pack more parallel evals per box — which is precisely "run multiple evals in parallel" that the box couldn't before.

### 2.2 WS-3 (persistent worker / fork-server) → warm re-eval + the fork model

`nix-eval-jobs` already forks its workers from a parent. If the parent **warms the AOT map + the applied-import cache before forking**, the warm state is COW/file-shared into every worker for free (this is the mechanism behind the 105 MB Shared_Clean). Two integration shapes:
- **Zero-patch:** just set `NIX_V3_AOT_CACHE_FILE` in the evaluator environment; the AOT pages are clean/file-backed and shared across the existing nix-eval-jobs fork automatically.
- **Fork-server (v3 `--fork-worker`, D3):** a longer-lived warm parent that also carries the applied-import cache across *jobset* evals (not just within one). Higher value for the "same jobset every push" case, but needs nix-eval-jobs to keep a parent alive between evals — a patch, deferred to Phase 3.

### 2.3 WS-2 (IFD visibility) + WS-1 (correctness) → the IFD pain

`allow_import_from_derivation=true` means the evaluator *is* the IFD-heavy path. WS-1's six realise-context fixes (hashFile/readFileType/findFile/pathExists/scopedImport, and pathExists no longer swallowing a failed IFD build) are therefore **mandatory** for a correct evaluator here — not optional polish.

WS-2's default-on end-of-eval IFD summary + per-realise line goes to each worker's stderr → Hydra's eval log. Today an IFD that stalls is invisible until the 3-hour watchdog kills the whole jobset (`flake.nix:446`). With WS-2 the log names each IFD being realised and its wall-time, so a stuck plan-to-nix is attributable in minutes, and the watchdog threshold can become an informed per-IFD budget instead of a blunt 3 h.

---

## 3. Exact changes

### 3.1 The nix build (flake inputs)

**Option A — backport onto 2.34 (recommended shadow vehicle, lowest blast radius). BUILT + VERIFIED 2026-07-17 → GO.**
Branch `angerman/2.34-v3` (committed `36bcbda12`, not pushed) = the v3 subsystem rebased onto the `angerman/2.34-ifd-profiling` (2.34.6) base. Gates on aarch64-darwin: `v3-eval` + `nix` + all 187 targets build OK; `all-v3-tests.sh --brute` **41/41 GREEN**; byte-id `hello.drvPath` v3-direct == TW == golden. **The feared 2.34↔2.35 Value/EvalState/forceValue signature problem did not materialize** — v3 touches TW only at FFI leaves (store/paths/realisePath/SourcePath/derivation/flake), which are identical across 2.34.6↔2.35.0, so `libnixexprv3` compiled against 2.34 headers with only `-Wunused`. The one real delta was cosmetic (`using namespace nix;` at file scope on 2.34 vs `namespace nix { … }` on 2.35 in `eval.cc`). Full conflict map + reproduce steps: `lode/BACKPORT_2_34_2026-07-17.md` (on the `angerman/2.34-v3` branch). **Two operational flags:** (1) the CI artifact MUST be built optimized — a `-O0` debug build ~2×'d eval wall-time and timed out the `iterative-force` brute suite; `debugoptimized`/`-O2` → 41/41 (the nixpkgs-packaged `nix` builds optimized, so this bites only local dev builds). (2) **x86_64-linux — the real Hydra target — is not yet validated on this branch** (backport was built on darwin); it inherits the pre-existing v3 Linux bring-up debt (the rebase introduced none, since the hook deltas are API-clean), so greening Linux CI ≈ the already-scoped v3 Linux bring-up, not the 2.34 rebase.

To wire into the infra:

```nix
# ops/flake.nix
nix.url     = "github:input-output-hk/nix/angerman/2.34-v3";   # was 2.34-ifd-profiling  (:25)
nix-pkg.url = "github:input-output-hk/nix/angerman/2.34-v3";   # was 2.34-ifd-profiling  (:73)
```
`hydra.inputs.nix.follows = "nix"` and `nix-eval-jobs@2.34.1` stay unchanged — same ABI, so Hydra + nix-eval-jobs rebuild against the v3-carrying libnixexpr with no version bump.

**Option B — bump the stack to 2.35 (eventual home).** Move `nix`/`nix-pkg` to `angerman/2.35-eval-profiling-v2`, bump `nix-eval-jobs` to a 2.35-compatible tag, and confirm the patched Hydra builds against 2.35. Larger coordination; do it only after Option A has proven the win in shadow.

Rollback for either: revert the two input lines, `nix flake lock`, `colmena apply --on linux-0`. It is a pin flip.

### 3.2 Evaluator environment (linux-0)

```nix
# modules/web-service-hydra.nix — engage v3 + point at the shared AOT for the eval fleet
systemd.services.hydra-evaluator.environment = {
  NIX_V3_DIRECT_EVAL   = "1";                        # engage the bytecode VM (see §4 Phase 0)
  NIX_V3_AOT_CACHE_FILE = "/var/cache/hydra/v3-aot/current.aot";
  NIX_V3_MAX_HEAP      = "2G";                        # typed OOM instead of SIGKILL; RLIMIT_AS-safe post-B1
};
```
Note the WS-5/B1 fix means `NIX_V3_MAX_HEAP` no longer collides with upstream's 8 GiB `MAP_NORESERVE` arenas on Linux — it baselines `RLIMIT_AS` on current VmSize, so this is safe to set on the evaluator.

### 3.3 AOT cache build (keyed to the jobset's nixpkgs pin)

```nix
# A oneshot that (re)builds the AOT whenever the pin changes, writing an
# atomically-swapped current.aot.  Uses bench/build-aot-cache-ci.sh (WS-3).
systemd.services.v3-aot-cache = {
  description = "Build the v3 AOT cache for the active jobset nixpkgs/haskell.nix pin";
  serviceConfig = { Type = "oneshot"; User = "hydra"; };
  # ExecStart: build-aot-cache-ci.sh → /var/cache/hydra/v3-aot/<narHash>.aot,
  # then ln -sfn to current.aot (atomic swap; workers mmap the target).
};
```
Keying: the AOT is only sound for the CU set it was built from. Build it against the same nixpkgs/haskell.nix rev the jobset uses (derive from the jobset flake.lock, mirror of `test/nixpkgs-pin.sh`). A stale AOT is *not* a correctness risk (v3 falls back to owning any CU not in the map), only a sharing-effectiveness one — but rebuild on pin change to keep the 105 MB shared.

### 3.4 Retune the fleet (Phase 2, after measuring)

Once per-worker private RSS is measured under the shared AOT:
```
evaluator_workers = 30            # was 20 — more parallel evals per box
evaluator_max_memory_size = 768   # was 1024 — tighter cap now that CU is shared
```
These are measured settings, not guesses — set them from the Phase-2 smaps numbers.

---

## 4. Phased rollout (each phase has a gate + a revert)

**Phase 0 — engagement proof: RESOLVED 2026-07-17 (code-level, definitive) → a bounded patch is required.**

The question was: does v3-direct engage when a *non-CLI* libnixexpr embedder (nix-eval-jobs) forces values? **Answer: no, and it cannot as-is.** Proven by exhaustive enumeration, not inference:

- The `forceValue`-level v3 hook was **removed** (`libexpr/eval.cc:1208-1216`, #735, 2026-05-21). libexpr's eval/`forceValue` is pure tree-walker with no v3 dispatch.
- The v3 root entry points `nix::v3::runRootExprFromString` / `runRootExprModule` are called from **exactly four files**: `src/nix/eval.cc` (the `nix eval` CLI's `CmdEval::run`), `src/libexpr-v3/cli/v3-eval.cc` (the standalone binary), and two v3-internal TUs (`run.cc`, `bytecode_primops.cc`/`aot_cache.cc` — recursion, import re-entry, AOT warm). **Nothing in libexpr / libstore / libmain** — nothing an external embedder links.
- Therefore nix-eval-jobs, which links libnixexpr and runs its own worker forcing loop (`state.eval(topExpr)` → `getAttr(path)` → `forceValue(.drvPath)`), **always runs the tree-walker.** There is no code path, static initializer, or build flag that would trip v3 from it.

**Consequence: CI integration = plumbing + ONE bounded nix-eval-jobs patch** (not "flip two inputs"). The patch has a precise, self-contained template already in-tree — `runV3DirectEval` (`src/nix/eval.cc:50-180`):
1. acquire the raw `.nix` source (`cmd.expr` / file contents) + base dir + `$HOME` + optional `SourcePath`;
2. `auto rr = v3::runRootExprFromString(state, src, base, home, file?)` → `{cu, value}`;
3. build a `v3::VMState`, push one synthetic `CallFrame` on `rr.cu` (so re-entrant forcing has a frame);
4. `r = v3::forceValue(vm, rr.value)` to WHNF;
5. descend the job's attrPath: split on `.`, `r.asAttrs()->lookup(v3::ir::globalInternSymbol(seg))`, `forceValue` each segment.
The patch reproduces that inside nix-eval-jobs' per-attr worker, then reads `.drvPath`/`.outputs`/`.meta`/`.name` off the v3 `Value` (the marshalling surface — nix-eval-jobs currently reads these off TW Values), and warms the CU + AOT **in the parent before the worker fork** so the fork inherits them COW/file-shared.

**Feasibility for the zw3rk jobsets (VERIFIED 2026-07-17, correcting an earlier assumption): the target jobsets are ALL flake-shaped — and v3 handles that natively.** The `runV3DirectEval` *CLI* entry rejects `InstallableFlake` (the `.#attr` shape, "Phase 2", `eval.cc:80-86`) — but that is a CLI-front-end limitation, **not** an evaluator one. v3 evaluates flakes natively: `builtins.getFlake` is the sole v3-native implementation (`primops.cc:10945` #758) → `callFlakeV3` (`v3_call_flake.cc:329`) runs `call-flake.nix` on the VM via `callClosure ×3`. Smoke-tested end-to-end on a local flake with `NIX_V3_DIRECT_EVAL=1 NIX_V3_REQUIRE=1` (which *throws* on any TW fallback):
```
(builtins.getFlake "git+file://…").foo      → "hello-from-flake"   [v3 engaged, no fallback]
(builtins.getFlake "git+file://…").bar.baz  → "nested-value"       [v3 engaged, attrpath descent]
"git+file://…#foo"  (flake installable)     → NIX_V3_REQUIRE throws  [CLI shape, not evaluator]
```
**Consequence for the patch:** the nix-eval-jobs patch does NOT need a new flake front-end in v3. For each job it **synthesizes a getFlake expression** — `(builtins.getFlake "<locked-flakeref>").<attrpath>` — and runs it through the same `runRootExprFromString` template below. v3's native getFlake does the lock/fetch (FFI leaf) + native `call-flake.nix` + native outputs eval; the patch then descends the attrPath and reads `.drvPath`/`.outputs`/`.meta`/`.name` off the v3 `Value`. The **locked** flakeref must match what nix-eval-jobs/Hydra resolved (rev-pinned `git+file://…?rev=…`) so drvPaths are byte-identical — the A4 flake-lock keying already caches flake-metadata attrs cross-process by byte-id, a bonus for the 20-worker fleet. (The trivial smoke flake proves the *mechanism*; a real haskell.nix jobset is heavy + IFD-laden — correctness is still confirmed by the Phase-1 shadow drvPath byte-compare.)

**Built-in engagement gate:** `NIX_V3_REQUIRE=1` (`eval.cc:326-334`) already hard-fails if v3 does not engage — bake it into the shadow test so a silent TW fallback cannot masquerade as a v3 run.

- **Remaining Phase-0 empirical step (follow-on to this proof):** write the nix-eval-jobs patch above, build it against the v3 nix, and confirm with `NIX_V3_REQUIRE=1` that a jobset attr forces through v3. Also decides Option A vs B (does v3 build cleanly on the 2.34 base? — the backport track answers this).
**Gate:** patched nix-eval-jobs forces a jobset attr through v3 under `NIX_V3_REQUIRE=1` (no fallback); `--brute` 41/41 on the built nix (both OSes).

**Phase 1 — shadow eval (no production impact).**
On linux-0 (or linux-1), run nix-eval-jobs with v3 over one real jobset (e.g. a haskell.nix project) in parallel with the production TW evaluator. **Byte-compare the emitted drvPaths** against the production evaluator's for the same rev.
**Gate:** drvPath set identical (this is the nixpkgs-golden discipline applied to a real jobset). Any divergence is a WS-1-class correctness bug → fix before proceeding, do not adjust the jobset.

**Phase 2 — density on linux-0 (the memory win).**
Point `hydra-evaluator` at the v3 nix + shared AOT (§3.2, §3.3). Measure per-worker `Private_Dirty` + fleet `Shared_Clean` via `/proc/*/smaps_rollup` during a real 20-worker eval.
**Gate:** fleet CU `Shared_Clean` ≥ 60 % of the AOT (WS-5 met this at 83 % in the lab); per-worker private RSS down enough to raise `evaluator_workers` without tripping the cap. Then retune §3.4 and re-measure.

**Phase 3 — IFD visibility to the Hydra log + watchdog.**
Confirm the WS-2 IFD lines reach the Hydra eval log; convert the blunt 3-hour `hydra-eval-watchdog` into a per-IFD budget informed by the WS-2 timings.
**Gate:** a deliberately-stalled IFD is named in the log within minutes.

**Phase 4 — developer darwin nix (optional, dev-experience).**
Flip `nix-pkg` on the darwin hosts to the v3 package so local `nix build` benefits from the applied-import cache. Not on the CI-eval critical path; do last.

---

## 5. What this does NOT solve (honesty)

- **IFD still blocks on the darwin build.** WS-2 makes it *visible* and WS-1 makes it *correct*; it does not make plan-to-nix stop being a synchronous build the evaluator waits on. The *latency* levers are orthogonal to the evaluator: (a) **store/substituter warming** so the IFD output is already built (converts cold→warm — biggest lever, pure infra, zero VM change), (b) the never-built **IFD phase-3 pre-eval narHash cache** (WS-7), (c) **fiber-overlap + multi-root** eval (WS-4, mechanism de-risked, needs a multi-root work source — nix-eval-jobs' 20-way fan-out *is* such a source, so this is more attractive here than in the CLI).
- **Single-eval CPU is still ~1.8–2.5× TW.** The density win is about fitting more parallel evals per box, not making one eval faster. If a *single* jobset eval's wall-time is the complaint, this integration does not address it (that gap is structural — see the campaign ledger).
- **The in-memory per-worker representation floor persists** between the shared AOT pages: the live thunk/closure/Bindings graph each worker builds is still 1.6–2.0× TW's. AOT sharing removes the *duplicated read-only* cost, not the *per-worker live* cost. Lowering that is the representation-rewrite track, not this integration.

---

## 6. Risk register

| risk | likelihood | mitigation |
|---|---|---|
| v3 does not engage in nix-eval-jobs without a patch | medium | Phase 0 blocks on exactly this; patch is small + localized to the forceValue seam |
| 2.34-vs-2.35 ABI mismatch (nix-eval-jobs/hydra) | medium | Option A backports v3 onto 2.34; Option B only after shadow proves the win |
| stale AOT under-shares after a pin bump | low | not a correctness issue (fallback to owned); rebuild-on-pin-change timer keeps sharing high |
| drvPath divergence on a real jobset | low | Phase-1 shadow byte-compare is the gate; treat any diff as a WS-1 bug, never touch the jobset |
| watchdog kills a legitimately-long v3 eval | low | WS-2 timings make the threshold informed; keep the 3 h backstop until Phase-3 data justifies changing it |
| `NIX_V3_MAX_HEAP` interaction on Linux | resolved | WS-5 B1 fix baselines RLIMIT_AS on current VmSize |

---

## 7. First concrete step

Phase 0, on a throwaway checkout: build `nix-eval-jobs` against the v3 nix and prove v3 opcodes execute when it forces a jobset. That single result decides whether CI integration is "flip two flake inputs + set three env vars" (plumbing) or "plumbing + one nix-eval-jobs forceValue patch" (a bounded, well-scoped build). Nothing else in this plan is blocked on new evaluator research.
