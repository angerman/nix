/goal — v3 CI-THROUGHPUT PHASE 1: drive lode/CI_THROUGHPUT_PRD_2026-07-13.md (@ a1125544a) to its Week-5 decision points.

LOAD FIRST: src/libexpr-v3/CLAUDE.md, then the PRD. Task IDs below (C1..C5, V1..V3, W1..W4, F0, M1) are defined THERE with entry points (file:line), acceptance gates, and kill criteria — do not re-derive them.

BINDING REFRAME: single-eval TW parity is CLOSED (PRD §1.1). Goal = CI throughput (warm repeated evals, IFD wall, N-parallel density). Do NOT re-fund anything in PRD §3.

GOAL MET = D1–D5 stamped DONE. DONE means: committed with a Rule-0 body ("what hypothesis does this kill?"), full `nix develop -c bash src/libexpr-v3/test/all-v3-tests.sh --brute` ALL GREEN, nixpkgs golden byte-identity where applicable, measurements git-noted on the measured commit (+ darwin4-rows.tsv row if perf), memory updated. A clean pre-committed-threshold falsification also counts as DONE.

D1 — CORRECTNESS (WS-1 C1–C5): fix hashFile/readFileType/findFile non-realising + pathExists swallowing failed IFD builds; CallFrame static_assert. Each: positive+negative+regression tests kept under test/; TW-vs-v3 parity tests with built AND unbuilt drv args; C4 failing-first. Then H4 lint: every IfdProbeKind op actually realises. Golden byte-id must stay green (these paths don't touch it — divergence ⇒ STOP + RCA).

D2 — IFD VISIBILITY (WS-2 V1–V3): V1 CI-config + USAGE.md for profile-import-from-derivation (zero code). V2 default-on end-of-eval stderr IFD summary (count, built/substituted/cached, blocked s, % wall) — NO new env gate, silent at 0 IFDs. V3 bench/ifd-decomp.sh: wall = compute + IFD-blocked + store-RPC for firefox/M5/HNE on darwin-4, git-noted. → DECISION INPUT #1: IFD-blocked = X% of CI-shaped wall.

D3 — LINUX RECLAIM FALSIFIER (WS-6 M1, measurement ONLY, no M2): transfer-test the macOS "reclaim dead" chain (alloc.hh:2758 R1). Ask the user ONCE for a quiet Linux host; if none, mark BLOCKED-ON-HOST and continue. Thresholds per PRD: M5 peak-RSS ≥150MB → M2 fundable; <50MB → verdict transfers, falsification commit. → DECISION INPUT #2.

D4 — PERSISTENT WORKER + AOT (WS-3 W1–W4): W1 worker mode — correctness core = explicit between-evals reset list (taint mask, limits, tlActiveVMStack; import + applied caches STAY). Gate: triple byte-identity (eval#1 == eval#2 == fresh-process), then KPI-4 on darwin-4: eval#2 ≤ 0.5× fresh warm (firefox+M5). Kill: unboundable state-bleed after 1wk → document as falsification. W2: ImportCache LRU bound by measurement + 100-eval RSS-plateau gate. W3: `make aot-cache-ci` + ≥95% hit-rate on 2nd run. W4: disk-cache eviction — implement or document wipe-per-image, kill the other.

D5 — FIBER AUDIT (WS-4 F0, WRITTEN REPORT ONLY, no F1 build): lode/FIBER_IFD_AUDIT_<date>.md with code evidence + cheap prototypes: (1) parked-fiber vs Cheney-scavenge invariant (force-scavenge-before-yield vs conservative-pin; stress under 1MB nursery + audit), (2) blackhole collision rate — par_trace on the REAL CI jobset shape, (3) store/EvalState threading at the 6 realise sites. → DECISION INPUT #3: fund F1 or delete fiber.cc (H1).

HARD STOP after D1–D5: present DECISION INPUTS #1/#2/#3 + KPI deltas. WS-4 F1+, WS-5, WS-6 M2, WS-7, WS-8 are HUMAN-FUND-GATED — do not start.

OPERATIONAL (will bite): darwin-4 ssh flaky → `-o IdentityAgent=none -i ~/.ssh/id_rsa`, rsync source, pass COMMIT=<laptop HEAD> to profiling. Layout changes (Value/Thunk/Closure/CallFrame/AllocStats) → rebuild v3-smoke BEFORE brute. <nixpkgs> tests: source test/nixpkgs-pin.sh (M5/HNE own locks). nixpkgs probes: always NIX_V3_MAX_WALL_TIME / MAX_HEAP / DIRECT_EVAL=1. No new env gate without inline retirement criterion. Unstamped benchmark numbers are worthless.
