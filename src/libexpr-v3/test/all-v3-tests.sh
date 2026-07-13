#!/usr/bin/env bash
# v3 master test runner — invokes every regression suite in one place.
#
# Modes:
#   ./all-v3-tests.sh              # default: "core" suite (~5 min)
#   ./all-v3-tests.sh --core       # explicit core
#   ./all-v3-tests.sh --full       # core + every run-*.sh repro (~15 min)
#   ./all-v3-tests.sh --quick      # smoke only (~30 sec)
#   ./all-v3-tests.sh --brute      # core suite under aggressive scavenge +
#                                  # V3_DBG_NURSERY_BRUTE / _AUDIT; fails if
#                                  # any post-scavenge brute or audit hit
#                                  # signature appears in stderr (~10 min).
#                                  # Closes ACTION_PLAN Phase 1.7 exit
#                                  # criterion (audit's "highest-leverage
#                                  # tooling investment").  See GC_AUDIT_
#                                  # ROUND_2_2026-05-21.md §3.4 R1.
#
# Modes pick which tests run; per-test verbosity is set by V3_TEST_VERBOSE=1
# (passed through to scripts that honour it).
#
# Exit codes:
#   0   all suites passed
#   1   any suite failed
#   2   harness / preflight error
#
# Output: one line per suite with PASS / FAIL / SKIP markers; a final
# summary table.  Per-suite stdout/stderr captured under
# /tmp/v3-test-logs-<PID>/ for failure diagnostics.
#
# Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
# Input Output Group.  SPDX-License-Identifier: Apache-2.0

set -u

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
TEST_DIR="$ROOT/src/libexpr-v3/test"
PROPERTY_DIR="$TEST_DIR/property"
NIX="${NIX:-$ROOT/build/src/nix/nix}"
V3_EVAL="${V3_EVAL:-$ROOT/build/src/libexpr-v3/v3-eval}"

mode="core"
case "${1:-}" in
  --core)  mode="core"  ;;
  --full)  mode="full"  ;;
  --quick) mode="quick" ;;
  --brute) mode="brute" ;;
  "")      mode="core"  ;;
  -h|--help)
    sed -n '2,25p' "$0"
    exit 0
    ;;
  *)
    echo "all-v3-tests: unknown argument '$1' (try --help)" >&2
    exit 2
    ;;
esac

# In `--brute` mode the core suite runs with the post-scavenge brute
# scanner and reachable-graph auditor both on, plus aggressive 1 MB
# nursery (forces frequent scavenge so any missed root is exercised).
# A hit in either diagnostic flips the suite to FAIL via the
# post-process step below — even if the underlying eval succeeds.
#
# Exporting here (instead of per-command) lets every SUITES[] entry
# inherit the gates without rewriting the table.  The gates are pure
# diagnostics; they don't change semantic correctness, only surface
# missed-root bugs the gateless run would silently swallow.
# #820 (2026-05-26): suppress the libutil stack-size warnings on darwin.
# `setrlimit(RLIMIT_STACK, 64 MB)` returns EINVAL whenever the inherited
# hard limit is below 64 MB — universally true on darwin without root.
# The warning is informative for end-users but pure noise for the test
# harness, where every `nix` invocation prints it onto stderr and (when
# captured via `2>&1`) contaminates expected-vs-actual comparisons.
# `_NIX_TEST_NO_ENVIRONMENT_WARNINGS=1` is the libutil-supported escape
# hatch; this commit extends its scope to cover the "Failed to increase
# stack size" lvlError print as well.  See current-process.cc.
export _NIX_TEST_NO_ENVIRONMENT_WARNINGS=1

if [[ "$mode" == "brute" ]]; then
  export NIX_V3_NURSERY=1
  export NIX_V3_NURSERY_SCAVENGE=1
  export NIX_V3_NURSERY_SIZE=1                   # 1 MB nursery → frequent scavenge
  export V3_DBG_NURSERY_AUDIT=1
  export V3_DBG_NURSERY_BRUTE=1
  echo "all-v3-tests: --brute — gates exported:"
  echo "  NIX_V3_NURSERY=1 NIX_V3_NURSERY_SCAVENGE=1 NIX_V3_NURSERY_SIZE=1"
  echo "  V3_DBG_NURSERY_AUDIT=1 V3_DBG_NURSERY_BRUTE=1"
  echo
fi

# Preflight — verify binaries exist.  If not, hint at what to build.
if [[ ! -x "$NIX" ]]; then
  echo "all-v3-tests: NIX not executable at $NIX" >&2
  echo "  Build with: nix develop -c ninja -C build" >&2
  exit 2
fi
if [[ ! -x "$V3_EVAL" ]]; then
  echo "all-v3-tests: v3-eval not executable at $V3_EVAL" >&2
  echo "  Build with: nix develop -c ninja -C build" >&2
  exit 2
fi

# Portable temp dir: macOS mktemp's `-t` is a PREFIX (not template), so
# use `--tmpdir` equivalent via explicit TMPDIR / fallback.  Both BSD
# and GNU mktemp accept `-d <template-with-XXXXX>`.
logdir="${TMPDIR:-/tmp}/v3-test-logs.$$"
mkdir -p "$logdir" || {
  echo "all-v3-tests: cannot create logdir $logdir" >&2
  exit 2
}
echo "all-v3-tests: mode=$mode logdir=$logdir"
echo

# -- Test definitions ------------------------------------------------------
#
# Each entry: name|description|invocation
# The runner captures stdout/stderr to $logdir/<name>.log and reports
# PASS / FAIL based on exit status.

declare -a SUITES=()

# Quick (~30 sec) — smoke + parity + iterative-force
# meson tests: invoke the test binaries directly so we don't depend on
# `meson` being on $PATH (it lives inside nix develop's shell, not the
# user's interactive env).  Each test() block in meson.build points at
# one of these binaries.
#
# v3-smoke's disk-cache subtest writes to $NIX_V3_CACHE_DIR (or the user's
# XDG cache when unset).  Scope it to the logdir so the test is hermetic
# and isolated from prior runs that may have populated a shared cache.
SUITES+=( "smoke|v3-smoke|NIX_V3_CACHE_DIR=$logdir/cache $ROOT/build/src/libexpr-v3/v3-smoke" )
SUITES+=( "drv-preflight|libnixstore drvPath validation|$ROOT/build/src/libexpr-v3/v3-drv-preflight" )
SUITES+=( "evalscope|EvalScope handle invalidation|$ROOT/build/src/libexpr-v3/v3-evalscope-handles" )
SUITES+=( "gc-root-handles|Stage 5 GcRoot RAII + registry + walker|$ROOT/build/src/libexpr-v3/v3-gc-root-handles" )
SUITES+=( "iterative-force|deep let/curry/app-spine|NIX=$NIX $TEST_DIR/iterative-force-depth.sh" )
SUITES+=( "derivation-parity|drvPath byte-equal vs TW|$TEST_DIR/derivation-parity.sh" )
SUITES+=( "flake-sourceinfo-parity|callFlake sourceInfo byte-equal vs TW (non-git + git)|NIX=$NIX $TEST_DIR/run-flake-sourceinfo-parity.sh" )
SUITES+=( "fetcher-parity|native fetchGit/fetchTree byte-equal vs TW|NIX=$NIX $TEST_DIR/run-fetcher-parity.sh" )
SUITES+=( "readdir-import-coerce-parity|readDir/import attrset (recursive outPath/__toString) coercion byte-equal vs TW|NIX=$NIX $TEST_DIR/run-readdir-import-coerce-parity.sh" )
SUITES+=( "chain-bindings-parity|Lever A ChainBindings pure-refactor (chain-off == chain-on)|NIX=$NIX $TEST_DIR/run-chain-bindings-parity.sh" )
SUITES+=( "ws1-realise-parity|WS-1 C1-C6 hashFile/readFileType/findFile/pathExists/scopedImport realise arg context (build unbuilt IFD output) byte-equal vs TW; C4 failed-build must not become false|NIX=$NIX $TEST_DIR/run-ws1-realise-parity-tests.sh" )
SUITES+=( "lint-ifd-realise-coverage|WS-1 H4: every read-class IFD-probe primop realises its argument (source lint guarding the C1/C2 regression)|$TEST_DIR/lint-ifd-realise-coverage.sh" )

# Brute mode (~2 min) — purpose-built BRUTE / AUDIT harness.  Runs
# a curated battery of allocating workloads, captures stderr
# separately, and fails on any `v3 SCAVENGE BRUTE: N>0 tenured words`
# or `v3 SCAVENGE AUDIT: nursery ... reachable via` line.  The other
# suites in core (lang / property / derivation-parity) pipe their
# eval output through `tail -1`, so a per-suite stderr-grep is
# unreliable — this dedicated harness is what closes the Phase 1.7
# exit criterion.
if [[ "$mode" == "brute" ]]; then
  SUITES+=( "brute-audit|BRUTE / AUDIT diagnostic regression|$TEST_DIR/run-brute-audit.sh" )
fi

# Core (~5 min) — quick + lang + property + key repros
# Brute mode runs the same core suite under the scavenge gates above,
# then post-processes logs (see "brute hit scan" block at end).
if [[ "$mode" == "core" || "$mode" == "full" || "$mode" == "brute" ]]; then
  SUITES+=( "lang|143 functional/lang tests through v3|$TEST_DIR/run-lang-tests.sh" )
  SUITES+=( "property|58 primop categories × 10 cases|$PROPERTY_DIR/run-property-tests.sh" )
  SUITES+=( "let-rec-publish|#546 OP_ATTRS_REC_INIT split regression|NIX=$NIX $TEST_DIR/run-let-rec-publish-split-tests.sh" )
  SUITES+=( "583-tag-app-cache|mapAttrs-style App cache regression|$TEST_DIR/run-583-tag-app-cache-tests.sh" )
  SUITES+=( "815-cross-workload-cache|#815 cross-workload disk-cache (path collision + formals round-trip)|NIX=$NIX $TEST_DIR/run-815-cross-workload-cache-tests.sh" )
  SUITES+=( "r1-trigger-verify|R1 trigger fully closed (zero CU-cached-vs-fresh DIFFs)|NIX=$NIX $TEST_DIR/run-r1-trigger-verify.sh" )
  # broader-thunkify: NOT included in core — the test still uses the
  # retired NIX_USE_V3=1 cutover hook (deleted in e8d7c3885) and pins
  # the pre-#497 failure mode behind NIX_V3_NO_COMPLEX_FROM_THUNK=1, a
  # negative-test setup that's stale.  Re-add when the test is
  # updated to use NIX_V3_DIRECT_EVAL=1 + a current gate.
  #SUITES+=( "broader-thunkify|#496-498 broader-thunkify upvalue bug|$TEST_DIR/run-broader-thunkify-tests.sh" )
  SUITES+=( "apply-overrides-1.7|§1.7 OP_APPLY_OVERRIDES chain guard + __overrides byte-identity|NIX=$NIX $TEST_DIR/run-1.7-apply-overrides-tests.sh" )
  SUITES+=( "branch-bool-typecheck|P1.1 §2.1 branch-opcode non-Boolean condition errors (TW-parity) + valid-bool BI|NIX=$NIX $TEST_DIR/run-branch-bool-typecheck-tests.sh" )
  SUITES+=( "primsort-barrier|P1.2 §2.2 primSort Phase-D missed-root barrier (tenured sorted list holding nursery cells)|$TEST_DIR/run-primsort-barrier-tests.sh" )
  SUITES+=( "withlookup-dangling|Q1.1 §1.1 withLookup write-by-index (>64-with realloc UAF; deep with-scope correctness + audit)|$TEST_DIR/run-withlookup-dangling-tests.sh" )
  SUITES+=( "valueless-barrier|Q1.2 §1.2 valueLess writeback cellWrite barrier (list-of-list compare; correctness + audit)|$TEST_DIR/run-valueless-barrier-tests.sh" )
  SUITES+=( "replacestrings-barrier|Q1.3 §1.3 replaceStrings force-check-store order (valid+error correctness + audit)|$TEST_DIR/run-replacestrings-barrier-tests.sh" )
  SUITES+=( "primimport-eval-error|P1.3 §2.3 disk-hit catch scoped to deserialize (erroring warm import runs once, error propagates)|NIX=$NIX $TEST_DIR/run-primimport-eval-error-tests.sh" )
  SUITES+=( "value-equal|P3.4 §3.5 valueEqual scalar fast path + general equality-engine parity vs TW|NIX=$NIX $TEST_DIR/run-value-equal-tests.sh" )
  SUITES+=( "formals-error|P3.2 §3.1 formals-validation error parity (unexpected/missing/non-set) + valid-formals vs TW|NIX=$NIX $TEST_DIR/run-formals-error-tests.sh" )
  SUITES+=( "attrs-init-cache|P3.3 §3.4 warm-disk-cache OP_ATTRS_INIT round-trip (non-rec attrset import; guards the reverted pre-sort lever)|$TEST_DIR/run-attrs-init-cache-roundtrip-tests.sh" )
  SUITES+=( "applied-cache|LEVER-1 applied-import result cache (CU-key collision + distinct-args + insns-collapse + throw-not-cached)|NIX=$NIX $TEST_DIR/run-applied-cache-tests.sh" )
  SUITES+=( "toplevel-cache|top-level result cache soundness (pure round-trip + getEnv/currentTime taint not-stale)|NIX=$NIX $TEST_DIR/run-toplevel-cache-tests.sh" )
  SUITES+=( "ifd-provenance-cache|IFD import cache soundness fix (N1 stale-HIT bug-fix: transitive readFile store-path narHash folded into a v2 key; N3/N5 poison fail-closed; N4 folded-set completeness; SHADOW compare-not-serve)|NIX=$NIX $TEST_DIR/run-ifd-provenance-cache-tests.sh" )
  SUITES+=( "dedup-survey|B1 instrument accuracy: cold+warm import CUs observed + FINAL report (guards disk-load observe + atexit total)|$TEST_DIR/run-dedup-survey-tests.sh" )
  SUITES+=( "nonmoving-tenured|Phase-S non-moving tenured line-region reclaim (byte-id + GC-stress; flag-OFF regression guard, ON gate via V3_NMT)|V3_NMT=${V3_NMT:-} $TEST_DIR/run-nonmoving-tenured-tests.sh" )
  SUITES+=( "lint-no-inline-getenv|cached env-var lint|$TEST_DIR/lint-no-inline-getenv.sh" )
  SUITES+=( "lint-no-direct-tw-include|FFI consolidation: no new direct TW #includes|$TEST_DIR/lint-no-direct-tw-include.sh" )
  SUITES+=( "lint-cache-coherence|#814/#815 schema-bump operating rules|$TEST_DIR/lint-cache-coherence.sh" )
  SUITES+=( "cache-gate-coverage|Rule 3: every codegen env gate is in kGates[] fingerprint (§1.4)|$TEST_DIR/run-cache-gate-coverage-tests.sh" )
  SUITES+=( "lint-serialize-symbolid-coverage|CR2 serialize.cc collect/remap symmetry (AR24)|$TEST_DIR/lint-serialize-symbolid-coverage.sh" )
fi

# Full (~15 min) — every run-*.sh that exists.  Each script is responsible
# for its own pass/fail semantics; if it exits 0 we mark PASS.
if [[ "$mode" == "full" ]]; then
  # Already-covered repros (above).  Skip in this loop to avoid double-run.
  declare -A already_added
  already_added[run-lang-tests.sh]=1
  already_added[run-let-rec-publish-split-tests.sh]=1
  already_added[run-583-tag-app-cache-tests.sh]=1
  already_added[run-broader-thunkify-tests.sh]=1
  already_added[run-1.7-apply-overrides-tests.sh]=1  # in core
  already_added[run-branch-bool-typecheck-tests.sh]=1  # in core
  already_added[run-primsort-barrier-tests.sh]=1  # in core
  already_added[run-primimport-eval-error-tests.sh]=1  # in core
  already_added[run-value-equal-tests.sh]=1  # in core
  already_added[run-formals-error-tests.sh]=1  # in core
  already_added[run-attrs-init-cache-roundtrip-tests.sh]=1  # in core
  already_added[run-applied-cache-tests.sh]=1  # in core
  already_added[run-toplevel-cache-tests.sh]=1  # in core
  already_added[run-ifd-provenance-cache-tests.sh]=1  # in core
  already_added[run-dedup-survey-tests.sh]=1  # in core
  already_added[run-nonmoving-tenured-tests.sh]=1  # in core

  for script in "$TEST_DIR"/run-*.sh; do
    base="$(basename "$script")"
    [[ -n "${already_added[$base]:-}" ]] && continue
    # Some scripts are interactive / benchmarks — skip.
    case "$base" in
      run-v3-tests.sh)            ;;  # included below
      bench-*)        continue ;;
      *)              ;;
    esac
    name="${base%-tests.sh}"
    name="${name#run-}"
    SUITES+=( "$name|repro script $base|$script" )
  done
  SUITES+=( "v3-eval-tests|hand-rolled run-v3-tests.sh|BUILD=$ROOT/build $TEST_DIR/run-v3-tests.sh" )
fi

# -- Execute ---------------------------------------------------------------

declare -i total=0 pass=0 fail=0
declare -a failed_names=()

for entry in "${SUITES[@]}"; do
  name="${entry%%|*}"
  rest="${entry#*|}"
  desc="${rest%%|*}"
  cmd="${rest#*|}"
  total=$((total + 1))
  log="$logdir/$name.log"
  echo "  [$total] running $name ($desc)..."
  if bash -c "$cmd" >"$log" 2>&1; then
    suite_status="PASS"
  else
    suite_status="FAIL"
  fi
  # Brute mode: post-process the captured log for any BRUTE / AUDIT
  # hit signature and demote PASS to FAIL.  The diagnostic patterns:
  #   - `v3 SCAVENGE BRUTE: N tenured words` with N > 0  (missed root)
  #   - `v3 SCAVENGE AUDIT: nursery .* reachable via`    (post-scav root)
  # Both are emitted by gc.cc's postScavengeBruteScan / postScavengeAudit
  # when V3_DBG_NURSERY_BRUTE / V3_DBG_NURSERY_AUDIT are set.  The
  # gateless run never emits them, so this scan is a no-op outside
  # --brute mode.
  brute_hit=""
  if [[ "$mode" == "brute" ]]; then
    # `[1-9][0-9]*` matches any non-zero word count.  The "0 tenured
    # words" line is emitted at every scavenge in BRUTE mode and is
    # the expected steady-state output.
    # Match only LIVE hits (in reachable objects); DEAD hits
    # (arena-bloat, harmless) end with `inside DEAD` and are filtered
    # out by the `point into nursery` suffix.
    if grep -E 'v3 SCAVENGE BRUTE: [1-9][0-9]* tenured words point into nursery' "$log" >/dev/null \
       || grep -E 'v3 SCAVENGE AUDIT: nursery .* reachable via' "$log" >/dev/null; then
      brute_hit="yes"
      suite_status="FAIL"
    fi
  fi
  if [[ "$suite_status" == "PASS" ]]; then
    pass=$((pass + 1))
    echo "       PASS"
  else
    fail=$((fail + 1))
    failed_names+=( "$name" )
    if [[ -n "$brute_hit" ]]; then
      echo "       FAIL  (BRUTE / AUDIT hit; log: $log)"
      # Show the first 3 hit lines inline so the failure mode is
      # obvious without opening the log.
      grep -E '(v3 SCAVENGE BRUTE: [1-9][0-9]* tenured words|v3 SCAVENGE AUDIT: nursery .* reachable via)' "$log" \
        | head -3 | sed 's/^/         /'
    else
      echo "       FAIL  (log: $log)"
    fi
    if [[ "${V3_TEST_VERBOSE:-0}" == "1" ]]; then
      tail -20 "$log" | sed 's/^/         /'
    fi
  fi
done

# -- Summary ---------------------------------------------------------------

echo
echo "==================== summary ===================="
echo "  mode:   $mode"
echo "  total:  $total"
echo "  pass:   $pass"
echo "  fail:   $fail"
echo "  logdir: $logdir"
if (( fail > 0 )); then
  echo "  failed: ${failed_names[*]}"
  echo
  echo "To re-run a single failed suite verbosely:"
  echo "  V3_TEST_VERBOSE=1 $0 ${mode/--}"
  exit 1
fi
echo "ALL GREEN"
exit 0
