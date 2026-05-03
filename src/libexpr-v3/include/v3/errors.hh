#pragma once
/// @file
/// Typed v3 evaluator exceptions.  These mirror tree-walker's
/// `nix::AssertionError` / `nix::ThrownError` hierarchy so that
/// `builtins.tryEval` can narrow its catch to the correct subset.
///
/// All classes derive from `std::runtime_error` so existing
/// `catch (const std::exception &)` paths still observe them.
///
/// Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
///   Input Output Group.
/// SPDX-License-Identifier: Apache-2.0

#include <stdexcept>
#include <string>

namespace nix::v3 {

/// `assert false; ...` — thrown by OP_ASSERT.  Matches tree-walker's
/// `nix::AssertionError` semantics: caught by `builtins.tryEval` and
/// turns into `{ success = false; value = false; }`.  Note we don't
/// derive from `nix::AssertionError` because that requires an
/// `EvalState &` constructor argument that v3's runtime doesn't carry.
class AssertionError : public std::runtime_error
{
public:
    using std::runtime_error::runtime_error;
};

/// `throw "msg"` — thrown by `primThrow`.  Tree-walker's
/// `nix::ThrownError` derives from `nix::AssertionError`; we mirror
/// the relationship.  `tryEval` catches these.
class ThrownError : public AssertionError
{
public:
    using AssertionError::AssertionError;
};

/// `abort "msg"` — thrown by `primAbort`.  Tree-walker's `nix::Abort`
/// inherits from `EvalError` not `AssertionError`, so `tryEval` does
/// NOT catch it.  Plain `std::runtime_error` is the appropriate type.
class AbortError : public std::runtime_error
{
public:
    using std::runtime_error::runtime_error;
};

} // namespace nix::v3
