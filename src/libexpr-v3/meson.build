# v3 evaluator — depends on nix-expr (for Expr AST + Symbol/SymbolTable +
# parser).  Provides a small library plus a v3-eval CLI that parses a Nix
# file and runs it through the v3 pipeline.

project(
  'nix-expr-v3',
  'cpp',
  version : files('.version'),
  default_options : [
    'cpp_std=c++23',
    'warning_level=1',
    'errorlogs=true',
  ],
  meson_version : '>= 1.1',
  license : 'Apache-2.0',
)

cxx = meson.get_compiler('cpp')

# A2 (2026-05-26) — V3_RELEASE build mode strips always-on
# instrumentation from the v3 evaluator's hot paths.  See
# `meson.options` for what is removed and `WARM_EVAL_AND_
# INSTRUMENTATION_2026-05-23.md` §6.1 for the rationale.
#
# Pre-committed ship-or-revert falsifier (per
# `lode/NEXT_STEPS_2026-05-25.md` §1.0 A2 entry):
#   - Ship if  >=2 % wall  reduction OR  >=20 MB peak-RSS reduction
#                (n=10 hyperfine, sigma < 1 %).
#   - Revert if <2 % wall AND <20 MB peak-RSS.
#
# Env-gated diagnostic counters (NIX_VM_OPCOUNTS, NIX_VM_OPCYCLES,
# NIX_V3_DBG_*) remain compileable; only payload-active when the
# env-var is set (cost when off is one cached-bool branch test).
v3_release_args = []
if get_option('v3_release')
  v3_release_args += [ '-DV3_RELEASE=1' ]
  message('v3: V3_RELEASE build — always-on instrumentation stripped')
endif
add_project_arguments(v3_release_args, language : 'cpp')

subdir('nix-meson-build-support/deps-lists')

deps_private_maybe_subproject = [
  dependency('nix-util'),
  dependency('nix-store'),
  dependency('nix-expr'),
  dependency('nix-main'),
  # #698: v3-native callFlake — links libflake for parseFlakeRef,
  # lockFlake, LockedFlake, flake::Settings.  No circular dep
  # (libflake depends on nix-expr / nix-store / nix-util; libexpr-v3
  # wraps libexpr; libflake does NOT depend on libexpr-v3).
  dependency('nix-flake'),
]
deps_public_maybe_subproject = []
subdir('nix-meson-build-support/subprojects')

# TOML parser used by builtins.fromTOML — tree-walker depends on it
# the same way.
toml11 = dependency('toml11', version : '>=3.7.0', method : 'cmake', modules : ['toml11::toml11'])
deps_other += toml11

# disk_cache.cc uses sqlite3 directly for BLOB column reads (the
# libnixstore SQLiteStmt::Use API exposes only TEXT/INT bind/fetch
# helpers; raw blobs need sqlite3_bind_blob / sqlite3_column_blob).
# Mirrors the dependency surface of src/libexpr/meson.build.
sqlite = dependency('sqlite3', 'sqlite', version : '>=3.6.19')
deps_other += sqlite

subdir('nix-meson-build-support/export-all-symbols')
subdir('nix-meson-build-support/windows-version')

subdir('nix-meson-build-support/common')

# #698: generate call-flake.nix.gen.hh from the canonical libflake
# source, so v3 can compile + run call-flake.nix natively without
# duplicating the source.  Mirrors libflake/meson.build:35-40.
subdir('nix-meson-build-support/generate-header')
generated_headers = []
foreach header : [
  '../libflake/call-flake.nix',
]
  generated_headers += gen_header.process(header)
endforeach

include_dirs = [ include_directories('include') ]

lib_sources = files(
  'value.cc',
  'ir.cc',
  'ir_dump.cc',
  'emit.cc',
  'vm.cc',
  'primops.cc',
  'bytecode_primops.cc',
  'limits.cc',
  'lower.cc',
  'opt_beta_reduce.cc',
  'opt_const_fold.cc',
  'opt_cse.cc',
  'opt_dce.cc',
  'opt_func_strictness.cc',
  'opt_inline.cc',
  'opt_strict_call_unthunk.cc',
  'opt_occur.cc',
  'opt_app_spine_fold.cc',
  'opt_genlist_unroll.cc',
  'opt_if_fold.cc',
  'opt_primop_fold.cc',
  'opt_primop_fuse.cc',
  'opt_strictness.cc',
  'opt_stream_fusion.cc',
  'serialize.cc',
  'disk_cache.cc',
  # R8a Phase 1 Day 10-12: AOT cache mmap reader.  Activated by
  # NIX_V3_AOT_CACHE_FILE=<path>; consulted as L3 before SQLite
  # disk_cache lookups.  See lode/AOT_PHASE1_DAY10-12_2026-05-26.md.
  'aot_cache.cc',
  # #827 / A3 / T1.1 per-call-site cache-hook instrumentation.
  # See `include/v3/cache_probe.hh` for the design.  Gated by
  # NIX_VM_CACHE_SITES=1; default-off cost is one cached-bool
  # branch test per cache-hook event.
  'cache_probe.cc',
  # #741 Phase 1 spike: round-trip serialiser for derivation-result
  # Values.  See lode/IFD_CACHE_DESIGN_2026-05-23.md.  Gated by
  # NIX_V3_TEST_DRV_RESULT_SERIALIZE=1; default-off cost is one
  # cached-bool load per primDerivationStrict call.
  'value_serialize.cc',
  'fiber.cc',
  'bridge_yield.cc',
  'gc.cc',
  # Phase D write-barriers (Stage 3 prereq): NURSERY_PHASE_D_DECISION_2026-05-21.md
  'barrier.cc',
  # Stage 3 of precise-root foundation: walkAllV3Roots(VMState &, RootVisitor &).
  # See lode/GC_PRECISE_ROOT_FOUNDATION_2026-05-27.md.  Foundation for
  # replacing Boehm conservative scan of the v3 arena.
  'precise_root.cc',
  # Stage 6 SPIKE: live-fraction tracer.  Cheap directional proxy for the
  # ditch-Boehm load-bearing claim BEFORE committing to Stages 5-6.
  # NIX_V3_LIVE_TRACE=1 reports the LIVE-vs-ALLOCATED ratio per type.
  'live_trace.cc',
  # Stage 5 of precise-root foundation: RAII GcRoot helper + thread-local
  # registry for C++-stack roots.  See lode/GC_PRECISE_ROOT_FOUNDATION_
  # 2026-05-27.md.  Forward-looking infrastructure for Stage 6 production.
  'gc_root.cc',
  # Arena deregistration side-table: per-thread bridge-source registry.
  # See lode/ARENA_DEREGISTRATION_DESIGN_2026-05-27.md.  Tracks
  # `nix::Value *` from Thunk::bridgeSrc so Boehm sees them as roots
  # independently of arena registration.
  'bridge_root_registry.cc',
  # Stage 6 production GC — flat mark-sweep over the v3 tenured arena.
  # Replaces the Cheney MajorScavenger (move_gc.cc) which was falsified
  # at commit eda44711a + 9bf527714.  See:
  #   lode/GC_DESIGN_POST_CHENEY_2026-05-28.md (the design)
  #   lode/STAGE_6_FALSIFIERS_DAY6_2026-05-28.md (F#1/F#2 data)
  #   lode/STAGE_6_CHENEY_FALSIFIED_2026-05-27.md (the falsification)
  # Phase 0: runMajorMarkSweep is a no-op stub; vm.cc dispatch-loop
  # wires the call path so subsequent phases (1-3) can drop in mark +
  # sweep + free-list-allocator without touching the trigger.
  'mark_sweep.cc',
  # #772 Stage 9 Phase L0 spike: bytecode-level dedup survey.
  'dedup_survey.cc',
  'disasm.cc',
  'ffi.cc',
  'print.cc',
  'run.cc',
  # #698: v3-native callFlake — see lode/V3_NATIVE_CALL_FLAKE_DESIGN_2026-05-20.md
  'v3_call_flake.cc',
  # PERF_TRACE_TOOL_DESIGN_2026-05-20.md: in-process Boehm-heap sampler.
  'heap_trace.cc',
)

this_library = library(
  'nixexprv3',
  lib_sources,
  generated_headers,  # #698: call-flake.nix.gen.hh
  dependencies : deps_public + deps_private + deps_other,
  include_directories : include_dirs,
  install : true,
  prelink : true,  # Pull static initializers into the final binary.
)

# Expose as a meson dependency so the main `nix` binary can link
# against us.  prelink + this override_dependency ensure libnixexprv3
# is pulled in and its static initializers fire (primop registry,
# content-cache, eval-trace, etc.).
meson.override_dependency(
  'nix-expr-v3',
  declare_dependency(
    include_directories : include_dirs,
    link_with : this_library,
    compile_args : [ '-std=c++23' ],
    dependencies : deps_public + deps_private + deps_other,
  ),
)

# Headers (publicly installed for downstream consumers).
install_headers(
  files(
    'include/v3/alloc.hh',
    'include/v3/bytecode.hh',
    'include/v3/closure.hh',
    'include/v3/install.hh',
    'include/v3/ir.hh',
    'include/v3/disk_cache.hh',
    'include/v3/gc.hh',
    'include/v3/limits.hh',
    'include/v3/lower.hh',
    'include/v3/primop.hh',
    'include/v3/print.hh',
    'include/v3/run.hh',
    'include/v3/serialize.hh',
    'include/v3/value.hh',
    'include/v3/vm.hh',
  ),
  subdir : 'v3',
)

# Tiny CLI: `v3-eval FILE.nix` — parses, lowers, runs.
#
# Bumped stack on darwin to 64 MiB to mirror `src/nix/meson.build`'s
# main-`nix` executable: v3-direct's residual C-recursion on deep
# stdenv.mkDerivation chains consumes ~14 KiB/frame × ~5000 logical
# forces, which overflows darwin's kernel-default 8 MiB pthread stack
# and SIGSEGVs silently.  Backstop pending the iterative-force
# refactor (see lode/A12B_BYTECODE_PRIMOPS_2026-05-17.md).
v3_eval_link_args = []
if host_machine.system() == 'darwin'
  v3_eval_link_args += [ '-Wl,-stack_size,0x4000000' ]  # 64 MiB
endif

v3_eval = executable(
  'v3-eval',
  'cli/v3-eval.cc',
  dependencies : deps_public + deps_private + deps_other + [
    declare_dependency(
      link_with : this_library,
      include_directories : include_dirs,
    ),
  ],
  link_args : v3_eval_link_args,
  install : true,
)

# 2026-05-18: IR-CHECK MVP — LLVM-FileCheck-compatible (subset)
# directive matcher.  In-tree, ~100 lines on top of `checkIrEx` in
# ir_dump.cc.  See lode/IR_CHECK_INFRASTRUCTURE_PLAN_2026-05-18.md.
v3_check = executable(
  'v3-check',
  'cli/v3-check.cc',
  dependencies : deps_public + deps_private + deps_other + [
    declare_dependency(
      link_with : this_library,
      include_directories : include_dirs,
    ),
  ],
  install : true,
)

# Smoke-test binary (hand-built IR; no parser).
v3_smoke = executable(
  'v3-smoke',
  'test/smoke.cc',
  dependencies : deps_public + deps_private + deps_other + [
    declare_dependency(
      link_with : this_library,
      include_directories : include_dirs,
    ),
  ],
  install : false,
)

test(
  'v3-smoke',
  v3_smoke,
  # v3-smoke's disk-cache subtest writes to NIX_V3_CACHE_DIR (or XDG
  # cache when unset).  Use a meson-isolated path so concurrent
  # test runs don't collide on a shared cache directory.
  env : {'NIX_V3_CACHE_DIR' : meson.global_build_root() / 'v3-smoke-cache'},
)

# BR-3.0 pre-flight: validate the libnixstore Derivation APIs we'll
# build BR-3 on top of produce a drvPath matching tree-walker's for a
# known-good input.
v3_drv_preflight = executable(
  'v3-drv-preflight',
  'test/drv-preflight.cc',
  dependencies : deps_public + deps_private + deps_other + [
    declare_dependency(
      link_with : this_library,
      include_directories : include_dirs,
    ),
  ],
  install : false,
)

test(
  'v3-drv-preflight',
  v3_drv_preflight,
)

# Stage 5 unit test — `GcRoot` RAII + thread-local registry +
# `walkCppStackRoots` walker integration.  See lode/
# GC_PRECISE_ROOT_FOUNDATION_2026-05-27.md.  Pure in-memory API
# check; no nixpkgs / eval-state / disk-cache dependency.
v3_gc_root_handles = executable(
  'v3-gc-root-handles',
  'test/gc-root-handles.cc',
  dependencies : deps_public + deps_private + deps_other + [
    declare_dependency(
      link_with : this_library,
      include_directories : include_dirs,
    ),
  ],
  install : false,
)

test(
  'v3-gc-root-handles',
  v3_gc_root_handles,
)

# #485: behavioural tests for EvalScope handle invalidation.
v3_evalscope_handles = executable(
  'v3-evalscope-handles',
  'test/evalscope-handles.cc',
  dependencies : deps_public + deps_private + deps_other + [
    declare_dependency(
      link_with : this_library,
      include_directories : include_dirs,
    ),
  ],
  install : false,
)

test(
  'v3-evalscope-handles',
  v3_evalscope_handles,
)

# Parser project Stage 1.1 (PARSER_PROJECT_PLAN_2026-06-01) — v3 AST
# show() byte-equality with TW.  Standalone (header-only AST; no
# libnixexprv3 link needed) — only the include dir is required.
# Asserts the v3 AST reproduces nix::Expr::show() for the operator
# core, locked to the parser-TI precedence-battery goldens.
v3_ast_show_test = executable(
  'v3-ast-show-test',
  'test/ast-show-test.cc',
  include_directories : include_dirs,
  install : false,
)

test(
  'v3-ast-show-test',
  v3_ast_show_test,
)

# Parser project Stage 1.2 (PARSER_PROJECT_PLAN_2026-06-01) — v3
# ParserState::addAttr attrset-merge + dup-detection vs TW.  Standalone
# (header-only ParserState + AST; needs the parser/ dir on the include
# path for parser-state.hh).
v3_ast_addattr_test = executable(
  'v3-ast-addattr-test',
  'test/ast-addattr-test.cc',
  include_directories : [include_dirs, include_directories('parser')],
  install : false,
)

test(
  'v3-ast-addattr-test',
  v3_ast_addattr_test,
)

# Parser project Stage 1.3 (PARSER_PROJECT_PLAN_2026-06-01) — bison/flex
# toolchain spike.  Minimal arithmetic grammar (parser/v3-spike.{y,l})
# emitting v3 AST, to de-risk the bison/flex -> v3 build integration
# BEFORE the full parser.y action rewrite (Stage 1.4).  Mirrors the
# libexpr custom_target wiring; unity=off because unity builds break
# the recursive flex/bison generated-header dependency (libexpr
# meson.build:238).
v3_spike_tab = custom_target(
  'v3-spike-tab',
  input : 'parser/v3-spike.y',
  output : ['v3-spike-tab.cc', 'v3-spike-tab.hh'],
  command : ['bison', '-o', '@OUTPUT0@', '@INPUT@', '-d'],
)
v3_spike_lex = custom_target(
  'v3-spike-lex',
  input : ['parser/v3-spike.l', v3_spike_tab],
  output : ['v3-spike-lex.cc', 'v3-spike-lex.hh'],
  command : ['flex', '--outfile', '@OUTPUT0@', '--header-file=@OUTPUT1@', '@INPUT0@'],
)
v3_parser_spike_test = executable(
  'v3-parser-spike-test',
  'test/parser-spike-test.cc',
  v3_spike_tab,
  v3_spike_lex,
  include_directories : [include_dirs, include_directories('parser')],
  override_options : ['unity=off'],
  install : false,
)
test(
  'v3-parser-spike-test',
  v3_parser_spike_test,
)

# #545: bench-harness self-tests.  Pure Python; loads workloads.toml,
# exercises filtering / stats / JSON / baseline-diff, and runs a tiny
# `nix eval` smoke test under TW + v3-direct.  Validates the
# V3_TIMING phase split for v3-direct landed in run.cc.
python3 = find_program('python3', required : false)
if python3.found()
  test(
    'v3-bench-harness-self-tests',
    python3,
    args : [files('bench/test_bench.py')],
    timeout : 60,
  )

  # #545: lint guarding against re-introduced inline std::getenv()
  # calls in vm.cc / primops.cc hot paths.  Cached env-var lookups
  # are the difference between v3-faster-than-TW and v3-1.5x-slower
  # on tight loops; we don't want a future contributor to silently
  # regress that.
  test(
    'v3-lint-no-inline-getenv',
    find_program('bash'),
    args : [files('test/lint-no-inline-getenv.sh')],
    timeout : 30,
  )

  # #546: regression suite for the OP_ATTRS_REC_INIT vs
  # OP_ATTRS_LET_REC_INIT split.  Verifies rec-attrs self-reference
  # (publish path) still works AND let-in-body / formals-default paths
  # (no-publish path) still work after the split.  See
  # CALLPACKAGE_BUG_2026-05-09.md.
  test(
    'v3-let-rec-publish-split',
    find_program('bash'),
    args : [files('test/run-let-rec-publish-split-tests.sh')],
    timeout : 60,
    env : {'NIX' : meson.global_build_root() / 'src' / 'nix' / 'nix'},
  )

  # Action plan Phase 1.3: regression-prevention for the A8 iterative-
  # force conversion.  Asserts deep let-chain (5000), deep curry-
  # apply (5000), and deep App-spine (3000) all complete without
  # C-stack overflow.  Also probes app-spine-5000 informationally
  # (Phase 1.2 candidate).
  test(
    'v3-iterative-force-depth',
    find_program('bash'),
    args : [files('test/iterative-force-depth.sh')],
    timeout : 180,
    env : {'NIX' : meson.global_build_root() / 'src' / 'nix' / 'nix'},
  )

  # 2026-05-18: derivation parity harness.  20 cases of drvPath
  # byte-for-byte comparison v3-direct vs TW.  Covers minimal,
  # multi-output, mixed-env, drv-cross-ref, buildInputs chains,
  # ignoreNulls, fixed-output, passthru, outpath-ref, deep chains
  # + direct builtins.derivationStrict variants.  Pinned to current
  # HEAD; regressions here surface broken Option 4 hybrid.
  test(
    'v3-derivation-parity',
    find_program('bash'),
    args : [files('test/derivation-parity.sh')],
    timeout : 120,
    env : {'NIX' : meson.global_build_root() / 'src' / 'nix' / 'nix'},
  )

  # 2026-05-18: lang test suite — 143 tests from functional/lang/
  # routed through v3-eval.  142 pass; 1 pre-existing fail tracked
  # in the test runner.  Catches AST-level + opcode-level
  # regressions.  Longer-running (~2 min).
  test(
    'v3-lang-tests',
    find_program('bash'),
    args : [files('test/run-lang-tests.sh')],
    timeout : 300,
  )

  # 2026-05-18: property tests — 58 primop categories × 10 cases of
  # TW vs v3 parity.  580 cases total.  Pure Python harness.
  test(
    'v3-property-tests',
    python3,
    args : [files('test/property/property_tests.py')],
    timeout : 300,
    env : {'V3_PROP_NIX' : meson.global_build_root() / 'src' / 'nix' / 'nix'},
  )

  # 2026-05-18: standalone repro-*.nix fixture runner — see REPROS.md
  # for the per-fixture lesson manifest.  Quick smoke (<10s).
  test(
    'v3-repros',
    find_program('bash'),
    args : [files('test/run-repros.sh')],
    timeout : 60,
    env : {'NIX' : meson.global_build_root() / 'src' / 'nix' / 'nix'},
  )

  # 2026-05-18: bug-specific repro suites.  Each has its own driver
  # script with its own pass/fail semantics; we treat exit-0 as PASS.
  # Run a curated subset by default — the master `all-v3-tests.sh
  # --full` covers everything else by hand.
  foreach script_info : [
    ['v3-583-tag-app-cache', 'test/run-583-tag-app-cache-tests.sh', 60],
    ['v3-456-chase-cycle',   'test/run-456-chase-cycle-tests.sh',   60],
    ['v3-558-emit-order',    'test/run-558-emit-order-tests.sh',    60],
    ['v3-direct-eval',       'test/run-direct-eval-tests.sh',       60],
    ['v3-lazy-bridge-arg',   'test/run-lazy-bridge-arg-tests.sh',   60],
    ['v3-cutover-parity',    'test/run-cutover-parity-tests.sh',    60],
    ['v3-stream-fusion',     'test/run-stream-fusion-tests.sh',     60],
    ['v3-lambda-lift',       'test/run-lambda-lift-tests.sh',       60],
    ['v3-resource-limits',   'test/run-resource-limits-tests.sh',   60],
    ['v3-eval-bytecode-primops-installed',
                             'test/run-v3-eval-bytecode-primops-installed.sh', 30],
  ]
    test(
      script_info[0],
      find_program('bash'),
      args : [files(script_info[1])],
      timeout : script_info[2],
      env : {'NIX' : meson.global_build_root() / 'src' / 'nix' / 'nix'},
    )
  endforeach

  # 2026-05-18: IR-CHECK MVP.  LLVM-FileCheck-compatible fixture
  # runner.  Walks test/ir-fixtures/*.nix and extracts # RUN: lines.
  # See lode/IR_CHECK_INFRASTRUCTURE_PLAN_2026-05-18.md.  Requires
  # both v3-eval and v3-check to be on PATH at runtime — the runner
  # script prepends the build's bin path via BUILD env var.
  test(
    'v3-ir-checks',
    find_program('bash'),
    args : [files('test/run-ir-checks.sh')],
    timeout : 60,
    env : {
      'V3_BUILD_BIN' : meson.global_build_root() / 'src' / 'libexpr-v3',
    },
  )

  # 2026-05-18: IR-dump determinism guard.  Runs a representative
  # corpus of expressions through `v3-eval --emit-ir{,-raw}` three
  # times each and diffs the outputs.  Codifies §1.7 of
  # IR_CHECK_INFRASTRUCTURE_PLAN_2026-05-18.md — catches any future
  # nondeterminism (unordered_map iteration, pointer-order sorting,
  # etc.) before fixtures become flaky.
  test(
    'v3-ir-dump-determinism',
    find_program('bash'),
    args : [files('test/run-ir-dump-determinism.sh')],
    timeout : 30,
    env : {
      'V3_BUILD_BIN' : meson.global_build_root() / 'src' / 'libexpr-v3',
    },
  )

endif
