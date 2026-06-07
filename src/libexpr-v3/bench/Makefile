# ════════════════════════════════════════════════════════════════════════════
#  v3 measurement Makefile — self-asserting v3-vs-TW benchmarks
#
#  The whole point: NEVER emit a v3-vs-TW number without the gate verifying
#  that v3 actually engaged, the result is byte-identical, and the baseline is
#  pinned. See lode/MEASUREMENT_GATE_2026-06-07.md for why (the ~7 flips).
#
#  This Makefile is standalone (it shells out to the built nix) so it never
#  touches the engine build — safe to run alongside in-flight engine work.
# ════════════════════════════════════════════════════════════════════════════

GATE := ./v3-vs-tw-gate.sh
WORKLOAD ?=

# Quoting rule (learned the hard way): workloads go through make → sh → ssh →
# the gate, so NO embedded `"` and NO `$${...}` (shell-expands to empty). Use
# numeric `toString` names and `builtins.getAttr` instead of `."x"` / `.$${k}`.
# `foldl'` (single quote) is fine inside the recipe's "$(VAR)" double-quoting.
# Sizes target the ≥1–2 s duration floor (sub-100 ms is startup-jitter noise).

# Compute / dispatch (register-VM regime). ~1.6 s; v3 ≈ 1.28× TW on the optimized build.
FIB := let f = n: if n < 2 then n else f (n - 1) + f (n - 2); in f 33

# getAttr-heavy on a SMALL set (2M lookups) — isolates lookup, AVOIDS the
# listToAttrs O(n²) build cliff. ~1.3 s.
LOOKUP := let s = builtins.listToAttrs (builtins.genList (i: { name = toString i; value = i; }) 1000); in builtins.foldl' (a: _: a + (builtins.getAttr (toString 500) s)) 0 (builtins.genList (x: x) 2000000)

# Primop-driven iteration (the fold-add family) — many small attrset builds. ~1 s.
FOLD := builtins.foldl' (a: o: a + builtins.length (builtins.attrNames o)) 0 (builtins.genList (i: builtins.listToAttrs (builtins.genList (j: { name = toString j; value = i + j; }) 500)) 2000)

# The O(n²) listToAttrs build cliff (lode/LISTTOATTRS_QUADRATIC_2026-06-07.md).
# Regression guard: ~15 s on v3 today, should be ~O(n) once pinned+fixed.
LTA := let s = builtins.listToAttrs (builtins.genList (i: { name = toString i; value = i; }) 200000); in builtins.seq s 0

.PHONY: help selftest scaling bc-vs-cpp measure measure-fib measure-lookup measure-fold measure-listtoattrs measure-firefox

help: ## Show this help
	@printf '\n\033[1mv3 measurement targets\033[0m\n\n'
	@grep -E '^[a-zA-Z_-]+:.*?## .*$$' $(MAKEFILE_LIST) \
	  | awk 'BEGIN{FS=":.*?## "}{printf "  \033[36m%-18s\033[0m %s\n", $$1, $$2}'
	@printf '\n  \033[1mexamples\033[0m\n'
	@printf '    make selftest                       # validate the engagement detector\n'
	@printf "    make measure WORKLOAD='1 + 1'       # any expr, gated\n"
	@printf '    make measure-firefox                # demonstrates the DIVERGENT catch\n'
	@printf '\n  \033[1mknobs\033[0m  RUNS=3  V3_WALL=120s  V3_HEAP=6G  NIX_BIN=…  ALLOW_DIVERGENT=1\n\n'

selftest: ## Validate the gate's own engagement detector (run this first)
	@$(GATE) --selftest

scaling: ## Complexity-class regression guard (O(n) vs O(n²) via doubling-ratio; XFAILs known blowups)
	@./scaling-check.sh

bc-vs-cpp: ## A/B each bytecode primop vs its C++ impl (NIX_V3_NO_BC_<NAME>); finds bytecode regressions
	@./bc-vs-cpp.sh

measure: ## Gate one workload:  make measure WORKLOAD='<nix-expr>'
	@test -n "$(WORKLOAD)" || { echo "set WORKLOAD='<nix-expr>'"; exit 2; }
	@$(GATE) "$(WORKLOAD)"

measure-fib: ## Compute/dispatch (fib 33) — register-VM regime, ~1.28× TW
	@$(GATE) "$(FIB)"

measure-lookup: ## getAttr lookups on a small set (isolated, no build cliff)
	@$(GATE) "$(LOOKUP)"

measure-fold: ## Primop iteration / small-attrset construction (fold-add family)
	@$(GATE) "$(FOLD)"

measure-listtoattrs: ## The O(n²) listToAttrs build cliff — regression guard (~15s)
	@$(GATE) "$(LTA)"

measure-firefox: ## firefox.drvPath — derivation-bound; shows the fake-store DIVERGENT gate
	@$(GATE) '(import <nixpkgs> {}).firefox.drvPath'
