/// @file
/// v3 Value singletons.
///
/// Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.
/// SPDX-License-Identifier: Apache-2.0

#include "v3/value.hh"

namespace nix::v3 {

Value Value::vTrue       = []() { Value v; v.tag_payload = static_cast<uint64_t>(Tag::Bool);      v.payload.i = 1; return v; }();
Value Value::vFalse      = []() { Value v; v.tag_payload = static_cast<uint64_t>(Tag::Bool);      v.payload.i = 0; return v; }();
Value Value::vNull       = []() { Value v; v.tag_payload = static_cast<uint64_t>(Tag::Null);      v.payload.raw = nullptr; return v; }();
Value Value::vBlackhole  = []() { Value v; v.tag_payload = static_cast<uint64_t>(Tag::Blackhole); v.payload.raw = nullptr; return v; }();
Value Value::vEmptyList  = []() { Value v; v.tag_payload = static_cast<uint64_t>(Tag::List);      v.payload.list = nullptr; return v; }();
Value Value::vEmptyAttrs = []() { Value v; v.tag_payload = static_cast<uint64_t>(Tag::Attrs);     v.payload.bindings = nullptr; return v; }();

} // namespace nix::v3
