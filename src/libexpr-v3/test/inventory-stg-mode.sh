#!/usr/bin/env bash
# #547 — Phase 1 inventory: catalog STG-mode failures across a
# representative workload spectrum.

set -u

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
NIX="${NIX:-$ROOT/build/src/nix/nix}"
TIMEOUT="${TIMEOUT:-10}"

if [[ ! -x "$NIX" ]]; then
  echo "nix CLI not found at $NIX" >&2
  exit 1
fi

# Workloads: name | expr (one per line, tab-separated).
read -r -d '' WORKLOADS <<'EOF'
rec-simple	rec { x = 1; y = x + 1; }
rec-self-dot	rec { x = 1; y = self.x; self = { x = 99; }; }
let-prev-update	let prev = { a = 1; }; in prev // { b = 2; }
let-rec-mutual	let a = b + 1; b = 2; in a + b
formals-default	({ a ? 1, b ? a + 1 }: a + b) {}
formals-mutual	({ a ? b, b ? 7 }: a) {}
lib-id	(import <nixpkgs/lib>).id 42
lib-fix-simple	let lib = import <nixpkgs/lib>; in lib.fix (self: { x = 1; y = self.x + 1; })
lib-extends-1	let lib = import <nixpkgs/lib>; in (lib.fix (lib.extends (final: prev: { bar = "X"; }) (self: { foo = "Y"; }))).bar
lib-extends-with	let lib = import <nixpkgs/lib>; in (lib.fix (lib.extends (final: prev: with final; { bar = foo + "/over"; }) (self: { foo = "base"; }))).bar
lib-makeextensible	let lib = import <nixpkgs/lib>; obj = lib.makeExtensible (self: { a = 1; }); in obj.a
nixpkgs-typeof	builtins.typeOf (import <nixpkgs> {})
nixpkgs-lib-id	(import <nixpkgs> {}).lib.id 42
nixpkgs-hello-name	(import <nixpkgs> {}).hello.name
EOF

run_one() {
  local mode="$1"
  local expr="$2"
  local env_prefix=""
  case "$mode" in
    tw)            env_prefix="" ;;
    v3-direct)     env_prefix="NIX_V3_DIRECT_EVAL=1" ;;
    v3-direct-stg) env_prefix="NIX_V3_DIRECT_EVAL=1 NIX_V3_STG=1" ;;
    v3-fhook)      env_prefix="NIX_USE_V3=1" ;;
    v3-fhook-stg)  env_prefix="NIX_USE_V3=1 NIX_V3_STG=1" ;;
  esac
  local out
  out=$(eval "$env_prefix" timeout "$TIMEOUT" "$NIX" --extra-experimental-features '"nix-command flakes"' eval --impure --expr "'$expr'" 2>&1)
  local rc=$?
  if [[ $rc -eq 124 ]]; then
    echo "TIMEOUT"; return 0
  fi
  if [[ $rc -ne 0 ]]; then
    if echo "$out" | grep -q "cycle while resolving"; then
      echo "CYCLE"
    elif echo "$out" | grep -q "OP_WITH_LOOKUP: name"; then
      echo "WITHMISS"
    elif echo "$out" | grep -q "BlackholeError\|infinite recursion"; then
      echo "BLACKHOLE"
    elif echo "$out" | grep -q "error:"; then
      local m=$(echo "$out" | grep "error:" | head -1 | sed 's/error: //' | head -c 50)
      echo "ERR:$m"
    else
      echo "CRASH:rc=$rc"
    fi
    return 0
  fi
  local v=$(echo "$out" | grep -v "^warning:" | tail -1 | head -c 50)
  echo "OK:$v"
}

modes=(tw v3-direct v3-direct-stg v3-fhook v3-fhook-stg)

# Print progress to stdout so we see results live.
printf "%-22s" "workload"
for m in "${modes[@]}"; do printf " | %-30s" "$m"; done
echo
printf "%.s-" {1..200}; echo

while IFS=$'\t' read -r name expr; do
  [[ -z "$name" ]] && continue
  printf "%-22s" "$name"
  for m in "${modes[@]}"; do
    res=$(run_one "$m" "$expr")
    printf " | %-30s" "${res:0:30}"
  done
  echo
done <<< "$WORKLOADS"
