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

subdir('nix-meson-build-support/deps-lists')

deps_private_maybe_subproject = [
  dependency('nix-util'),
  dependency('nix-expr'),
  dependency('nix-main'),
]
deps_public_maybe_subproject = []
subdir('nix-meson-build-support/subprojects')

# TOML parser used by builtins.fromTOML — tree-walker depends on it
# the same way.
toml11 = dependency('toml11', version : '>=3.7.0', method : 'cmake', modules : ['toml11::toml11'])
deps_other += toml11

subdir('nix-meson-build-support/export-all-symbols')
subdir('nix-meson-build-support/windows-version')

subdir('nix-meson-build-support/common')

include_dirs = [ include_directories('include') ]

lib_sources = files(
  'value.cc',
  'ir.cc',
  'emit.cc',
  'vm.cc',
  'primops.cc',
  'lower.cc',
  'opt_const_fold.cc',
  'opt_cse.cc',
  'opt_dce.cc',
  'opt_inline.cc',
  'serialize.cc',
  'disk_cache.cc',
  'fiber.cc',
  'bridge_yield.cc',
  'v3_hook.cc',
  'disasm.cc',
)

this_library = library(
  'nixexprv3',
  lib_sources,
  dependencies : deps_public + deps_private + deps_other,
  include_directories : include_dirs,
  install : true,
  prelink : true,  # For the static-init that registers v3EvalHook.
)

# Expose as a meson dependency so the main `nix` binary can link
# against us.  The cutover hook in v3_hook.cc relies on its static
# initializer firing — that requires the binary to actually pull in
# this library.
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
    'include/v3/lower.hh',
    'include/v3/primop.hh',
    'include/v3/serialize.hh',
    'include/v3/value.hh',
    'include/v3/vm.hh',
  ),
  subdir : 'v3',
)

# Tiny CLI: `v3-eval FILE.nix` — parses, lowers, runs.
v3_eval = executable(
  'v3-eval',
  'cli/v3-eval.cc',
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
