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

# A big attrset built ONCE then read by name 5000× — stresses attrset
# lookup / forcing (the regime the team measured as v3-faster).
LOOKUP := let s = builtins.listToAttrs (builtins.genList (i: { name = "a" + toString i; value = i; }) 5000); in builtins.foldl' (a: k: a + s.$${k}) 0 (builtins.attrNames s)

# Many attrsets CONSTRUCTED via genList/listToAttrs + foldl' — primop-driven
# iteration, v3's documented weak spot (the fold-add-17× family).
FOLD := builtins.foldl' (a: o: a + builtins.length (builtins.attrNames o)) 0 (builtins.genList (i: builtins.listToAttrs (builtins.genList (j: { name = "a" + toString j; value = i + j; }) 200)) 300)

.PHONY: help selftest measure measure-lookup measure-fold measure-firefox

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

measure: ## Gate one workload:  make measure WORKLOAD='<nix-expr>'
	@test -n "$(WORKLOAD)" || { echo "set WORKLOAD='<nix-expr>'"; exit 2; }
	@$(GATE) "$(WORKLOAD)"

measure-lookup: ## Attrset lookup/forcing (v3-strong regime)
	@$(GATE) "$(LOOKUP)"

measure-fold: ## Primop-iteration / attrset construction (v3-weak, fold-add family)
	@$(GATE) "$(FOLD)"

measure-firefox: ## firefox.drvPath — derivation-bound; shows the fake-store DIVERGENT gate
	@$(GATE) '(import <nixpkgs> {}).firefox.drvPath'
