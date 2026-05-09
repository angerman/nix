#pragma once
/// @file
/// v3 IR text dumper — stable, line-oriented format suitable for
/// FileCheck-style assertions in tests.
///
/// The output is one binding (or terminal) per line, in a form like:
///
///   ; module n_funcs=N n_blocks=M
///   ; func 0 entry=B0 nUp=0 name="<top>"
///   B0:
///     v1 = LitInt 42
///     v2 = LitInt 1
///     v3 = Add v1 v2
///     return v3
///   ; func 1 entry=B1 nUp=1 freeVars=[v5] name="f"
///   B1:
///     ...
///
/// Tests build a Module manually, run an optimisation pass, then either
/// compare the dump to an expected string (substring or exact match) or
/// run "FileCheck"-style line-by-line `// CHECK:` directives via the
/// `checkIr` helper.
///
/// The format intentionally avoids absolute VarId / BlockId / FuncId
/// numbers in places where they would shift across pass runs.  Every
/// VarId is printed as `vN` and every BlockId as `BN` so dump output is
/// stable per IR shape.
///
/// Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
///   Input Output Group.
/// SPDX-License-Identifier: Apache-2.0

#include "v3/ir.hh"

#include <string>
#include <string_view>
#include <vector>

namespace nix::v3::ir {

/// Pretty-print the entire Module to a string.  Stable, deterministic
/// output suitable for FileCheck-style line-oriented assertion.
///
/// Symbol IDs are resolved via `globalSymbolTable()` (so names show as
/// `"foo"` rather than `S42`).  VarIds and BlockIds are printed
/// verbatim as `vN` / `BN` because their ordering is stable across
/// optimisation passes (the IR never re-numbers).
std::string dumpModule(const Module & m);

/// Convenience: dump just one block.  Used by per-pass diagnostics.
std::string dumpBlock(const Module & m, BlockId bid);

/// Convenience: dump one Expr.  Used by per-pass diagnostics.
std::string dumpExpr(const Module & m, const Expr & e);

// ---------------------------------------------------------------------------
// FileCheck-style assertion helper (line-oriented, ordered)
// ---------------------------------------------------------------------------
//
// Tests use `checkIr(actual, R"(
//   ; CHECK: v1 = LitInt 42
//   ; CHECK: v3 = Add v1 v2
//   ; CHECK-NOT: LitInt 99
//   ; CHECK: return v3
// )")` and the helper:
//
//   - Splits `actual` and `expected` into lines.
//   - For each `; CHECK: pat` directive in expected: search forward in
//     actual lines for a line CONTAINING `pat`.  Advance the actual
//     cursor past the matched line.  The CHECK directives must match
//     IN ORDER.
//   - For each `; CHECK-NOT: pat` directive: scan forward from the
//     current actual cursor up to (but not past) the next CHECK match's
//     position.  Fail if `pat` is found in any of those lines.
//   - Lines in expected that are blank or do not start with `; CHECK`
//     are ignored (lets you write the expected output near-literally
//     and just inject CHECK directives).
//
// Returns empty string on success; on failure, returns a diagnostic
// describing which directive failed and the surrounding context.

/// Run a list of `; CHECK:` / `; CHECK-NOT:` directives extracted from
/// `expected` against the actual output text.  Empty return = pass;
/// non-empty return = failure diagnostic.
std::string checkIr(std::string_view actual, std::string_view expected);

} // namespace nix::v3::ir
