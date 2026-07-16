# WS5-D2 — consume the AOT mmap in place (cross-process CU sharing): design

**Date:** 2026-07-16
**Status:** DESIGN (measure-first analysis done; build not started — it is core-VM-read-path surgery + a schema bump, scoped here to be turnkey).
**Goal:** turn the AOT file's CU bytes into `Shared_Clean` pages across independent `nix` processes (the process-per-job CI model), instead of deserializing them into private per-process vectors (today: WS5.0 measured `Shared_Clean` = only 12–20 MB → ~0 cross-process CU sharing).
**Copyright:** (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group. SPDX-License-Identifier: Apache-2.0.

---

## Where the CU footprint is (what's shareable)

`CompilationUnit` (bytecode.hh:591) sections, by shareability:

| section | type | runtime-mutable? | in-place shareable? |
|---|---|---|---|
| `code` | `vector<Instruction>` (u32) | **no** (VM only reads `code[ip]`) | **YES** — POD, contiguous |
| `intConstants` / `floatConstants` | `vector<i64>` / `vector<double>` | no | **YES** — POD |
| `lambdaCodeOffsets` | `vector<u32>` | no | **YES** — POD |
| `lambdas` (LambdaDescriptors) | `vector<LambdaDescriptor>` | **YES** (counters, `cu` back-ptr, `cachedSingletonClosure`) | only after **WS5-D1** side-arrays the mutables |
| `symbolTable` | `vector<std::string>` | no | needs contiguous char-blob+offset format |
| `stringConstants` | `vector<const std::string*>` | no, but **per-process intern pointers** | no (re-interned per process) |
| `primops` | `vector<const PrimOp*>` | re-resolved per process | no |
| `attrSelectCache` / `recSlotCache` | ICs | **YES** | no (WS5-D1 side-array) |

Per the #139 RCA, **`lambdas` is the largest chunk (~65 % of the firefox CU footprint)** — so the biggest win needs D1 first. The independent slice (`code` + POD constants + `lambdaCodeOffsets`) is D2a below.

## D2a — borrow `code` + POD arrays from the mmap (independent of D1)

**Representation.** Replace the raw `std::vector<Instruction> code` with a small vector-API-compatible wrapper so the 270 `.code` access sites compile unchanged:

```cpp
struct Bytecode {                 // POD-array, owned-or-borrowed
    const Instruction * ptr_ = nullptr;
    size_t len_ = 0;
    std::vector<Instruction> owned_;   // empty ⇒ borrowed (ptr_ into AOT mmap)
    Instruction operator[](size_t i) const { return ptr_[i]; }
    size_t size() const { return len_; }
    const Instruction * data() const { return ptr_; }
    const Instruction * begin() const { return ptr_; }
    const Instruction * end() const { return ptr_ + len_; }
    bool empty() const { return len_ == 0; }
    void push_back(Instruction x) { owned_.push_back(x); ptr_ = owned_.data(); len_ = owned_.size(); }
    void borrow(const Instruction * p, size_t n) { owned_.clear(); ptr_ = p; len_ = n; }
    // + reserve/resize/back/assign as the emitter needs (audit below)
};
```

**Blast radius (measured):** 270 `.code` sites — **186 in emit.cc** (the emitter, which only ever builds OWNED CUs during compile → push_back/reserve/back, all provided by the wrapper), **59 in vm.cc** (read path: `code[ip]`, `size()`, `data()` — all provided), 20 disasm, 8 serialize, 2 run. Do the same for `intConstants`/`floatConstants`/`lambdaCodeOffsets` (POD wrapper, or a shared template). `symbolTable`/`stringConstants`/`primops`/`lambdas` stay owned (D2a).

**Format + reader.** The AOT flat file (aot_cache.cc, `MAP_PRIVATE` `PROT_READ`) already returns a `string_view` into the mmap (aot_cache.cc:233/308). Bump the serialize schema (`serialize.hh kSchemaVersion`) so the POD sections are stored **8-byte-aligned + native-endian + length-prefixed contiguous**, so `Bytecode::borrow(reinterpret_cast<const Instruction*>(blob+off), n)` is valid directly. `deserializeCU` gets an AOT-path variant that borrows these sections (keeps owning the rest); the SQLite/fresh path still owns everything (copies). Alignment: `mmap` is page-aligned; ensure each POD section starts 8-aligned within the blob (pad in the writer).

**Lifetime.** The AOT mmap is process-lifetime (never unmapped) → borrowed spans stay valid. The moving GC never touches CU bytes (they're not arena cells). Safe.

## D1 + D2b — the descriptors (the bigger chunk)

Do **WS5-D1** first (side-array `forceCount`/`allocCount`/`callCount`/`cachedSingletonClosure` + the `cu` back-ptr + `fromImportCU` out of `LambdaDescriptor` into per-process arrays keyed `(cu,funcId)`), leaving `LambdaDescriptor` a pure-POD read-only record. Then D2b borrows the `lambdas` array from the mmap like the POD arrays above. This is the ~65 % chunk — the bulk of the cross-process sharing win — but it is gated on D1.

## Test + measurement plan (pre-committed gates)

1. **Byte-identity**: nixpkgs golden (hello/git/firefox/gcc drvPath) identical AOT-borrow vs owned — the VM must read borrowed `code` identically. Run on macOS + Linux.
2. **`--brute` 40/40** on both platforms (borrowed code under the 1 MB-nursery stress).
3. **Linux smaps win** (the point): two concurrent `nix eval` processes both using the same `NIX_V3_AOT_CACHE_FILE`; measure `Shared_Clean` on the CU region. Gate: **≥ 60 % of CU-bytecode (per WS5.0: ~127 MB of the 212 MB) becomes `Shared_Clean` across processes** (D2a: the POD slice; D2a+D2b: the full 212 MB).
4. **CPU ≤ +1 %** darwin-4 (the wrapper's `operator[]` must inline to the same load as `vector::operator[]` — verify the disasm of the hot `code[ip]` fetch is unchanged).

## Risk + recommendation

This touches the VM's hottest read path (`code[ip]`) + a serialization schema bump + 270 sites (mostly wrapper-compatible). The subtle correctness points are: (a) the emitter's `push_back` repointing `ptr_` after reallocation (must never hand out a stale span mid-emit), (b) alignment of the borrowed sections, (c) the owned-vs-borrowed branch in `deserializeCU`, (d) the `operator[]` inlining to preserve hot-path CPU. It is a focused multi-day build best executed with fresh context and the incremental gates above (D2a → measure → D1 → D2b → measure), NOT rushed. The measure-first + design here (and the `--cow-fork` harness from WS5.0, reused for gate 3) de-risk it; the ROI is the WS5.0-measured cross-process CU sharing (127–212 MB `Shared_Clean` per box shared across all concurrent evals).

## Sequencing note

WS5.0 showed the **zygote (D3)** already captures the sharing win *for forked children* with no layout change (27 MB/child). D2 is specifically for the **process-per-job** CI model (independent `nix` invocations), where fork-sharing doesn't apply. If the CI model is (or can be) a fork-server, D3 is the faster path to the same win; D2 is the model-agnostic one. Build order per the user's steer: D2 first (widest applicability), then D3.
