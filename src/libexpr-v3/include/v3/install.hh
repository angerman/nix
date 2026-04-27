#pragma once
/// @file
/// v3 cutover hook installer.  Provides an explicit symbol reference
/// from libnixexprv3 so the main `nix` binary's linker can't strip
/// the library — the static initializer that registers
/// `EvalState::v3EvalHook` would otherwise vanish under
/// `-dead_strip_dylibs`.
///
/// Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.
/// SPDX-License-Identifier: Apache-2.0

namespace nix::v3 {

/// Install the v3 evaluator hook in `nix::EvalState::v3EvalHook`.
/// Call this once at program start (e.g., from main).  Idempotent.
void installEvalHook();

} // namespace nix::v3
