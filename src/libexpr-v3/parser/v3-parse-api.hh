#pragma once
/// @file
/// Public entry point for the v3-native parser (PARSER_PROJECT_PLAN
/// §5.3).  Wraps the flex/bison glue (scanner init + buffer + parse +
/// teardown) so call sites OUTSIDE the parser TU — the v3-eval CLI and
/// the import/flake parse sites in `primops.cc` / `v3_call_flake.cc` /
/// `bytecode_primops.cc` — can parse `.nix` source into a v3 AST without
/// depending on the generated flex/bison headers directly.
///
/// The caller owns the `ParserState` (it holds the AST arena and must
/// outlive any use of the returned node) and sets `basePath` / `homePath`
/// on it BEFORE calling, so relative / `~`-prefixed path literals resolve
/// the same way TW's parser resolves them.
///
/// Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
///   Input Output Group.
/// SPDX-License-Identifier: Apache-2.0

#include "parser-state.hh"  // nix::v3::ast::ParserState + Node (lightweight: AST + std only)

#include <string>

namespace nix::v3::parser {

/// Parse `text` into `st` (which owns the resulting AST).  Returns
/// `st.result`.  Throws `nix::Error` on a parse failure or if no
/// expression was produced.  Reentrant (a fresh flex scanner per call).
nix::v3::ast::Node * parseString(nix::v3::ast::ParserState & st, const std::string & text);

}  // namespace nix::v3::parser
