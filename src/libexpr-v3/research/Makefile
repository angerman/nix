# v3 research / spikes — self-documenting Makefile.
# Run inside the dev shell:  nix develop -c make -C src/libexpr-v3/research <target>
#
# Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.
# SPDX-License-Identifier: Apache-2.0

CXX      ?= clang++
CXXFLAGS ?= -std=c++20 -O2 -Wall

.PHONY: help jit-spike jit-encoder-test clean

help: ## Show this help
	@grep -E '^[a-zA-Z_-]+:.*## ' $(MAKEFILE_LIST) \
	  | awk 'BEGIN{FS=":.*## "}{printf "  \033[36m%-12s\033[0m %s\n", $$1, $$2}'

jit-spike: jit_feasibility_spike ## JIT-0: build + run the executable-codegen feasibility probe (macOS aarch64 MAP_JIT/W^X)
	@echo "── running JIT feasibility spike ──"
	@./jit_feasibility_spike

jit_feasibility_spike: jit_feasibility_spike.cc ## Build the JIT feasibility spike
	$(CXX) $(CXXFLAGS) -o $@ $<

jit-encoder-test: jit_encoder_test ## J1: build + RUN the aarch64 encoder/JitArena validation (emits + executes generated code)
	@echo "── running JIT encoder test (J1) ──"
	@./jit_encoder_test

jit_encoder_test: jit_encoder_test.cc ../include/v3/jit.hh ## Build the J1 encoder test
	$(CXX) $(CXXFLAGS) -I../include -o $@ $<

clean: ## Remove built spike binaries
	rm -f jit_feasibility_spike jit_encoder_test
