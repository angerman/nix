# v3 evaluator — bring-up Makefile.
# Standalone build (no meson/cmake yet) — depends only on a C++23 compiler.
# Run targets via `nix develop -c make <target>` from this directory.

CXX        ?= c++
CXXFLAGS   ?= -std=c++23 -O2 -g -Wall -Wextra
CXXFLAGS_DEBUG  := -std=c++23 -O0 -g -Wall -Wextra -fsanitize=address,undefined

INCLUDE    := -Iinclude

# All v3 source files except tests.
LIB_SRCS   := value.cc ir.cc emit.cc vm.cc primops.cc
TEST_SRCS  := test/smoke.cc

BUILD_DIR  ?= /tmp
SMOKE_BIN  := $(BUILD_DIR)/v3-smoke
SMOKE_DBG  := $(BUILD_DIR)/v3-smoke-dbg

# ANSI color codes for self-documentation
GREEN  := \033[1;32m
BLUE   := \033[1;34m
YELLOW := \033[1;33m
RESET  := \033[0m

.PHONY: help all smoke smoke-dbg clean check check-debug list

help: ## Show this help message
	@printf "$(BLUE)v3 evaluator — make targets$(RESET)\n\n"
	@awk 'BEGIN {FS = ":.*?## "} /^[a-zA-Z_-]+:.*?## / { \
		printf "  $(GREEN)%-15s$(RESET) %s\n", $$1, $$2 \
	}' $(MAKEFILE_LIST)
	@printf "\n$(YELLOW)Examples:$(RESET)\n"
	@printf "  nix develop -c make smoke      # build + run smoke tests\n"
	@printf "  nix develop -c make check-debug # build + run with ASan/UBSan\n"
	@printf "  nix develop -c make clean      # remove build artifacts\n"

all: smoke ## Build the smoke-test binary

smoke: $(SMOKE_BIN) ## Build the optimized smoke-test binary
	@printf "$(GREEN)build OK:$(RESET) $(SMOKE_BIN)\n"

$(SMOKE_BIN): $(LIB_SRCS) $(TEST_SRCS) $(wildcard include/v3/*.hh)
	@printf "$(BLUE)building (release)$(RESET) $(SMOKE_BIN)\n"
	$(CXX) $(CXXFLAGS) $(INCLUDE) $(LIB_SRCS) $(TEST_SRCS) -o $@

smoke-dbg: $(SMOKE_DBG) ## Build the debug smoke-test binary (ASan + UBSan)
	@printf "$(GREEN)build OK (debug):$(RESET) $(SMOKE_DBG)\n"

$(SMOKE_DBG): $(LIB_SRCS) $(TEST_SRCS) $(wildcard include/v3/*.hh)
	@printf "$(BLUE)building (debug + sanitizers)$(RESET) $(SMOKE_DBG)\n"
	$(CXX) $(CXXFLAGS_DEBUG) $(INCLUDE) $(LIB_SRCS) $(TEST_SRCS) -o $@

check: smoke ## Build and run the smoke tests
	@printf "$(BLUE)running smoke tests$(RESET)\n"
	@$(SMOKE_BIN)

check-debug: smoke-dbg ## Build and run the smoke tests with sanitizers
	@printf "$(BLUE)running smoke tests (sanitized)$(RESET)\n"
	@$(SMOKE_DBG)

list: ## List source files
	@printf "$(BLUE)library sources$(RESET)\n"
	@for f in $(LIB_SRCS); do printf "  %s\n" $$f; done
	@printf "$(BLUE)test sources$(RESET)\n"
	@for f in $(TEST_SRCS); do printf "  %s\n" $$f; done

clean: ## Remove build artifacts
	@rm -f $(SMOKE_BIN) $(SMOKE_DBG)
	@printf "$(GREEN)cleaned$(RESET)\n"
