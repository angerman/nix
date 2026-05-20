/// @file
/// v3 Value singletons.
///
/// Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.
/// SPDX-License-Identifier: Apache-2.0

#include "v3/value.hh"
#include "v3/alloc.hh"

namespace nix::v3 {

// Static empty containers — used as the payload of vEmptyAttrs /
// vEmptyList so callers that dereference `payload.bindings` /
// `payload.list` see a real `size = 0` object instead of dereferencing
// null.  Important now that we hand out the singletons from
// OP_ATTRS_INIT 0 / OP_LIST_INIT 0.  The trailing FAM `entries`/`elems`
// arrays are zero-sized so no extra bytes are needed.
namespace {
Bindings sEmptyBindings = []{ Bindings b; b.size = 0; b._pad = 0; return b; }();
ListVec  sEmptyList     = []{ ListVec l; l.size = 0; l._pad = 0; return l; }();
} // anonymous namespace

// #703: expose the empty-Bindings sentinel to `Alloc::allocBindings(0)`.
// Defined out-of-line here (rather than as an inline in alloc.hh) so
// the sentinel address is stable across translation units — every
// caller observes the same pointer.
Bindings * Alloc::emptyBindingsSentinel() noexcept
{
    return &sEmptyBindings;
}

Value Value::vTrue       = []() { Value v; v.tag_payload = static_cast<uint64_t>(Tag::Bool);      v.payload.i = 1; return v; }();
Value Value::vFalse      = []() { Value v; v.tag_payload = static_cast<uint64_t>(Tag::Bool);      v.payload.i = 0; return v; }();
Value Value::vNull       = []() { Value v; v.tag_payload = static_cast<uint64_t>(Tag::Null);      v.payload.raw = nullptr; return v; }();
Value Value::vBlackhole  = []() { Value v; v.tag_payload = static_cast<uint64_t>(Tag::Blackhole); v.payload.raw = nullptr; return v; }();
Value Value::vEmptyList  = []() { Value v; v.tag_payload = static_cast<uint64_t>(Tag::List);      v.payload.list = &sEmptyList; return v; }();
Value Value::vEmptyAttrs = []() { Value v; v.tag_payload = static_cast<uint64_t>(Tag::Attrs);     v.payload.bindings = &sEmptyBindings; return v; }();

} // namespace nix::v3
