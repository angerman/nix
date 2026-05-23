#!/usr/bin/env bash
# Workload heterogeneity audit — runs each candidate workload under v3-direct
# with all instrumentation gates on, captures key metrics, emits a markdown
# cross-workload comparison table.
#
# Purpose: identify whether the wall-clock structure varies across workloads.
# If ALL workloads look like hello.drvPath (drv primops dominate, libstore
# tail is small), then in-process caching levers are exhausted and the next
# investment should shift elsewhere (Phase 4 IFD primops or eval-level
# memoization).  If SOME workloads have different bottlenecks, those are
# the next levers.
#
# Usage:
#   bench/workload-heterogeneity.sh > workload_audit.md
#
# Required: NIX_PATH or default nixpkgs channel resolves to a usable
# nixpkgs.  Each workload eval is bounded by NIX_V3_MAX_WALL_TIME=30s.
#
# Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
#   Input Output Group.
# SPDX-License-Identifier: Apache-2.0

set -u

NIX_BIN="${NIX_BIN:-./build/src/nix/nix}"
TIMEOUT_S="${TIMEOUT_S:-60}"
WALL_TIME_S="${WALL_TIME_S:-30s}"
MAX_HEAP="${MAX_HEAP:-2G}"

# Workloads ordered by expected eval cost (light → heavy).  Each is
# evaluated as `(import <nixpkgs> {}).<EXPR>`.
WORKLOADS=(
    "hello.name"
    "hello.drvPath"
    "hello.outPath"
    "bash.drvPath"
    "coreutils.drvPath"
    "gcc.drvPath"
    "python3.drvPath"
    "firefox.name"
    "firefox.drvPath"
)

# Run one workload with full v3 instrumentation; capture wall + stats.
# Args: $1 = expression
# Emits: tab-separated single line:
#   wall_ms  peak_rss_mb  insns  bytesBindings_mb  drvprim_count  drvprim_ms  top_primops  ifd_probes_total
measure_one() {
    local expr="$1"
    local tmp; tmp=$(mktemp)
    local twall; twall=$(mktemp)
    # /usr/bin/time -p emits to stderr; we want to capture both stderr (stats)
    # and the wall time.  Use two-step: stderr → tmp; wall time → twall.
    local start_ns end_ns
    start_ns=$(date +%s)
    {
        NIX_V3_DIRECT_EVAL=1 \
        NIX_VM_STATS=1 \
        NIX_VM_PRIMOP_TIME=1 \
        NIX_V3_MAX_WALL_TIME="$WALL_TIME_S" \
        NIX_V3_MAX_HEAP="$MAX_HEAP" \
        timeout "$TIMEOUT_S" "$NIX_BIN" eval --impure --expr "(import <nixpkgs> {}).${expr}" \
            > /dev/null 2> "$tmp"
    } 2>/dev/null
    local rc=$?
    end_ns=$(date +%s)
    local wall_s=$((end_ns - start_ns))
    local wall_ms=$((wall_s * 1000))

    if [ $rc -ne 0 ]; then
        echo "${expr}|FAIL|rc=${rc}|||||||"
        rm -f "$tmp" "$twall"
        return
    fi

    # Extract peak_rss (MB) — grab LAST occurrence (main eval, after sub-evals)
    local peak_rss
    peak_rss=$(grep -E '^v3-direct memory:.*peak_rss=' "$tmp" | tail -1 | \
        sed -E 's/.*peak_rss=([0-9.]+)MB.*/\1/' || echo "?")

    # Extract insn count — last occurrence
    local insns
    insns=$(grep -E '^v3-direct alloc:.*insns=' "$tmp" | tail -1 | \
        sed -E 's/.*insns=([0-9]+).*/\1/' || echo "?")

    # Extract bytesBindings MB — last occurrence
    local bytesBindings
    bytesBindings=$(grep -E '^v3-direct bytes' "$tmp" | tail -1 | \
        sed -E 's/.*bindings=([0-9.]+)MB.*/\1/' || echo "?")

    # Per-primop wall.  NIX_VM_PRIMOP_TIME emits a block like:
    #   v3 primop wall-clock (top 15 of N; total NNN ns ≈ MMM ms):
    #       <ns> ns  count=<n>  avg=<a> ns/call  <name>
    #       ...
    # followed by an unrelated line.  Extract just the data rows.
    local drvprim_ms drvprim_count top_primops total_primop_ms
    # Grab the LAST primop wall-clock block (main eval, not sub-evals).
    local primop_block
    primop_block=$(awk '
        /^v3 primop wall-clock/ {block=1; buf=""; next}
        block && / ns  count=/ {buf = buf $0 "\n"; next}
        block {block=0; last_buf=buf; buf=""}
        END {if (block) last_buf = buf; printf "%s", last_buf}
    ' "$tmp" || echo "")
    if [ -n "$primop_block" ]; then
        # Field layout: $1=ns, $2="ns", $3="count=<n>", $4="avg=<v>", $5="ns/call", $6=name
        top_primops=$(echo "$primop_block" | head -n 3 | \
            awk '{print $NF}' | paste -sd, -)
        drvprim_count=$(echo "$primop_block" | \
            grep -E "(__derivCoerce|__derivationStrictRaw|__derivationFromPreprocessed|derivationStrict)" | \
            awk '{gsub(/count=/, "", $3); s+=$3}END{print s+0}')
        drvprim_ms=$(echo "$primop_block" | \
            grep -E "(__derivCoerce|__derivationStrictRaw|__derivationFromPreprocessed|derivationStrict)" | \
            awk '{s+=$1}END{printf "%.0f", (s+0)/1e6}')
        total_primop_ms=$(grep -E "^v3 primop wall-clock" "$tmp" | tail -1 | \
            sed -E 's/.*≈ ([0-9.]+) ms.*/\1/' || echo "?")
    else
        top_primops="?"
        drvprim_count="?"
        drvprim_ms="?"
        total_primop_ms="?"
    fi

    # IFD probes total — last occurrence
    local ifd_probes
    ifd_probes=$(grep -E '^v3-direct ifd probes:.*total=' "$tmp" | tail -1 | \
        sed -E 's/.*total=([0-9]+).*/\1/' || echo "0")

    # All attrsetsAllocated count — last occurrence
    local attrsets
    attrsets=$(grep -E '^v3-direct alloc:.*attrsets=' "$tmp" | tail -1 | \
        sed -E 's/.*attrsets=([0-9]+).*/\1/' || echo "?")

    echo "${expr}|OK|${wall_s}s|${peak_rss}|${insns}|${attrsets}|${bytesBindings}|${drvprim_count}|${drvprim_ms}|${total_primop_ms}|${ifd_probes}|${top_primops}"

    rm -f "$tmp" "$twall"
}

emit_header() {
    cat <<'EOF'
# Workload heterogeneity audit — 2026-05-23

Cross-workload measurement of v3 wall structure.  Generated by
`bench/workload-heterogeneity.sh`.

Goal: identify whether the optimization levers exposed by
hello.drvPath generalise to other workloads, or whether any
workload has a DIFFERENT wall structure that would reveal new
levers.

## Methodology

Per workload: 1 eval run under v3-direct with `NIX_VM_STATS=1`
+ `NIX_VM_PRIMOP_TIME=1` enabled.  Stats parsed from stderr:

  * `wall_s` — total wall clock (whole-second resolution; coarse)
  * `peak_rss_MB` — peak resident memory
  * `insns` — total bytecode instructions executed
  * `attrsets` — attrsets allocated
  * `Bindings_MB` — bytes allocated to Bindings entries
  * `drv_calls` — derivation primop call count
    (sum of __derivCoerce + __derivationStrictRaw +
    __derivationFromPreprocessed + derivationStrict)
  * `drv_ms` — wall in derivation primops (inclusive force chains)
  * `ifd_probes` — total OP_IFD_PROBE invocations
  * `top_3_primops` — top 3 by inclusive wall

## Cross-workload table

EOF
}

emit_table_header() {
    cat <<'EOF'
| Workload | Status | Wall | Peak RSS | Insns | Attrsets | Bindings | Drv calls | Drv ms | All-primop ms | IFD probes | Top primops |
|----------|--------|------|----------|-------|----------|----------|-----------|--------|--------------|------------|-------------|
EOF
}

# Pipe `measure_one` output (pipe-separated) into a markdown table row.
to_table_row() {
    local line="$1"
    IFS='|' read -ra fields <<< "$line"
    echo "| \`${fields[0]}\` | ${fields[1]} | ${fields[2]} | ${fields[3]} MB | ${fields[4]} | ${fields[5]} | ${fields[6]} MB | ${fields[7]} | ${fields[8]} | ${fields[9]} | ${fields[10]} | ${fields[11]} |"
}

main() {
    emit_header
    emit_table_header
    for w in "${WORKLOADS[@]}"; do
        echo "  measuring ${w} ..." >&2
        local result
        result=$(measure_one "$w")
        to_table_row "$result"
    done
    cat <<'EOF'

## How to read this table

  * **Wall**: whole-second resolution from a single run.  Use hyperfine
    for fine-grained measurement (see `bench.py`).
  * **Drv calls vs Drv ms ratio**: signals per-call cost.  Higher
    means deeper inputDrvs chain or more complex derivations.
  * **IFD probes**: OP_IFD_PROBE counts.  These count when an
    IFD-class primop is CALLED; they do NOT mean an actual IFD
    fired (most calls don't trigger builds, just check `pathExists`
    on non-existent paths or `import` of non-derivation paths).
  * **Top primops**: which primops accumulate the most inclusive
    wall.  If derivation primops are NOT top-3, that workload is
    NOT bottlenecked on derivation construction.

## Findings

(Add manual analysis after running the script.)

EOF
}

main "$@"
