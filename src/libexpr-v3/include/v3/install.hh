#pragma once
/// @file
/// Dead-strip resistance for libnixexprv3.  The v3 evaluator is now
/// invoked directly via `nix::v3::runRootExpr` from
/// `src/nix/eval.cc` when NIX_V3_DIRECT_EVAL=1 — there is no longer
/// an installable hook.  This single function is kept so the main
/// `nix` binary can reference a symbol from libnixexprv3, preventing
/// the linker's `-dead_strip_dylibs` pass from dropping the library.
///
/// Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.
/// SPDX-License-Identifier: Apache-2.0

namespace nix::v3 {

/// Force-link the library.  Always returns true.  Call once at
/// program start.
bool keepLibAlive();

} // namespace nix::v3
