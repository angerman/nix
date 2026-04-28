#!/usr/bin/env bash
set -u
NIX=/Users/angerman/Projects/iohk/nix/build/src/nix/nix-instantiate
NPK=/Users/angerman/Projects/zw3rk/nixpkgs

declare -a WORKLOADS=(
    "fib35:let f=n: if n<2 then n else f(n - 1) + f(n - 2); in f 35"
    "hello-name:(import $NPK { system = \"aarch64-darwin\"; }).hello.name"
    "git-name:(import $NPK { system = \"aarch64-darwin\"; }).git.name"
    "drv3:let pkgs=import $NPK { system = \"aarch64-darwin\"; }; in [pkgs.hello.drvPath pkgs.git.drvPath pkgs.vim.drvPath]"
    "attr-pkgs:builtins.length (builtins.attrNames (import $NPK { system = \"aarch64-darwin\"; }))"
    "attr-hask:builtins.length (builtins.attrNames (import $NPK { system = \"aarch64-darwin\"; }).haskellPackages)"
)

run_once() {
    local mode="$1" expr="$2"
    local env_setup=""
    case "$mode" in
        v3)       env_setup="NIX_USE_V3=1" ;;
        v3-fhook) env_setup="NIX_USE_V3=1 NIX_USE_V3_FORCE=1" ;;
        tw)       env_setup="" ;;
    esac
    /usr/bin/time -p env $env_setup "$NIX" --eval --strict --expr "$expr" >/tmp/bench.out 2>/tmp/bench.t
    local rc=$?
    local real=$(grep '^real' /tmp/bench.t | awk '{print $2}')
    if [[ $rc -ne 0 ]]; then echo "FAIL($rc):$real"; else echo "ok:$real"; fi
}

best_of_3() {
    local best="ok:99999"
    for i in 1 2 3; do
        local r=$(run_once "$1" "$2")
        local t=${r#*:}
        local s=${r%:*}
        # only consider 'ok' for "best"; but if all fail, keep last fail
        if [[ "$s" == "ok" ]]; then
            local bt=${best#*:}
            local bs=${best%:*}
            if [[ "$bs" != "ok" ]] || awk -v t=$t -v b=$bt 'BEGIN{exit !(t<b)}'; then
                best=$r
            fi
        elif [[ "${best%:*}" != "ok" ]]; then
            best=$r
        fi
    done
    echo "$best"
}

printf "%-12s %-15s %-15s %-15s\n" "workload" "tw" "v3" "v3+fhook"
printf "%-12s %-15s %-15s %-15s\n" "--------" "----" "----" "--------"
for entry in "${WORKLOADS[@]}"; do
    name="${entry%%:*}"
    expr="${entry#*:}"
    tw=$(best_of_3 tw "$expr")
    v3=$(best_of_3 v3 "$expr")
    v3f=$(best_of_3 v3-fhook "$expr")
    printf "%-12s %-15s %-15s %-15s\n" "$name" "$tw" "$v3" "$v3f"
done
