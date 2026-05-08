#pragma once
/// @file
/// v3 Value pretty-printing, JSON rendering, and deep-forcing helpers.
///
/// Lifted from `cli/v3-eval.cc` so the integrated `nix` CLI can reuse
/// the same surface form when running v3-direct (the inversion: TW as
/// leaf, v3 as host — see `lode/INVERSION_PLAN_2026-05-08.md`).
///
/// Output format matches `nix-instantiate --eval --strict` byte-for-
/// byte; the lang-test golden suite (`test/run-lang-tests.sh`) depends
/// on it.
///
/// Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.
/// SPDX-License-Identifier: Apache-2.0
#include "v3/value.hh"
#include "v3/vm.hh"

#include <nlohmann/json_fwd.hpp>

#include <iosfwd>
#include <set>
#include <string>
#include <vector>

namespace nix::v3 {

/// Recursively force `v` to its full normal form.  Forces lists and
/// attrset entries depth-first.  Tracks visited list/attrset pointers
/// in `seen` so cyclic values like `let x = [x]; in x` print as
/// `«repeated»` instead of looping.  The single-arg overload allocates
/// its own visited set.
Value forceDeep(VMState & vm, Value v, std::set<const void *> & seen);
Value forceDeep(VMState & vm, Value v);

/// Pretty-print `v` to `out` using the same surface form as
/// `nix-instantiate --eval --strict`:
///   - strings get backslash-escaped (`"`, `\`, `\n`, `\r`, `\t`,
///     `${`).
///   - attrs are sorted by symbol name; bare-identifier keys stay
///     unquoted, anything else is `"quoted"`.
///   - cyclic lists/attrsets print `«repeated»` on revisit.
///   - lambdas print `<LAMBDA>`, primops `<PRIMOP>`, etc.
///
/// The byte-exact match against `nix-instantiate` output is what the
/// lang-test golden suite checks; do NOT rephrase tokens here without
/// updating `tests/functional/lang/eval-okay-*.exp` accordingly.
///
/// `symTab` is the global symbol table (`ir::globalSymbolTable()`);
/// passed in so the caller controls lifetime.  `seen` is shared across
/// the recursion to track cyclic values.
void printNixValue(std::ostream & out, const Value & v,
                   const std::vector<std::string> & symTab,
                   std::set<const void *> & seen);
void printNixValue(std::ostream & out, const Value & v,
                   const std::vector<std::string> & symTab);

/// v3 Value → JSON, matching `builtins.toJSON` semantics:
///   - scalars / lists / attrs serialise normally.
///   - functions throw `runtime_error("cannot convert a function to
///     JSON")` (parity with tree-walker's `toJSON`).
///   - thunks render as the literal string `"<thunk>"` (caller is
///     expected to forceDeep first if a real value is wanted).
///
/// `nlohmann::json` is forward-declared in the header to keep the
/// libnixexprv3 ABI surface light; consumers must include
/// `<nlohmann/json.hpp>` themselves before using the result.
nlohmann::json toJsonValue(const Value & v,
                           const std::vector<std::string> & symTab);

} // namespace nix::v3
