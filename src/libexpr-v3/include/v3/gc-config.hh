#pragma once
/// @file
/// v3-owned indirection for the Boehm / GC dependency
/// (FFI_CONSOLIDATION_AUDIT_2026-06-01 §2.4 #4).
///
/// The Boehm/GC bits v3 needs are BUILD/RUNTIME concerns — not eval-state
/// — that happen to live under `nix/expr/`:
///   * `NIX_USE_BOEHMGC` — build macro (generated `config.hh`, from
///     `bdw_gc.found()`); gates v3's Boehm-vs-malloc fallback.
///   * `nix::traceable_allocator<T>` — the GC-aware STL allocator used by
///     the VM's value/frame stacks.
///   * `nix::initGC()` — Boehm bootstrap.
/// All three come from `nix/expr/eval-gc.hh` (which itself pulls
/// `config.hh`).  v3 GC-aware files (`alloc.hh`, `nursery.hh`,
/// `bridge_root_registry.cc`, `vm.hh`) route through THIS single v3-owned
/// header instead of including the TW header directly.  Values/decls are
/// still the real generated/host ones — no hardcoding, no build change,
/// so the `#if NIX_USE_BOEHMGC` non-Boehm fallback stays correct.
///
/// This header is itself a documented FFI leaf, centralizing what was
/// scattered across the GC-aware sources.
///
/// Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
///   Input Output Group.
/// SPDX-License-Identifier: Apache-2.0

#include "nix/expr/eval-gc.hh"  // NIX_USE_BOEHMGC (via config.hh) + traceable_allocator + initGC
