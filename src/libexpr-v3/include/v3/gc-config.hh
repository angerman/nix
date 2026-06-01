#pragma once
/// @file
/// v3-owned indirection for the `NIX_USE_BOEHMGC` build macro
/// (FFI_CONSOLIDATION_AUDIT_2026-06-01 §2.4 #4).
///
/// `NIX_USE_BOEHMGC` is a BUILD-CONFIG macro — not eval-state — that
/// happens to live in TW's generated `nix/expr/config.hh` (set from
/// `bdw_gc.found()`).  v3 GC-aware files (`alloc.hh`, `nursery.hh`,
/// `bridge_root_registry.cc`) only need this one macro; routing it
/// through this single v3-owned header keeps the TW include out of those
/// files (they `#include "v3/gc-config.hh"` instead).  The value is still
/// derived from the real generated header — no hardcoding, no build
/// change — so v3's `#if NIX_USE_BOEHMGC` non-Boehm fallback path stays
/// correct.
///
/// This header is itself a documented FFI leaf (one of the few v3 files
/// that pulls a `nix/...` header), centralizing what was scattered across
/// the GC-aware sources.
///
/// Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
///   Input Output Group.
/// SPDX-License-Identifier: Apache-2.0

#include "nix/expr/config.hh"  // NIX_USE_BOEHMGC (generated; bdw_gc.found())
