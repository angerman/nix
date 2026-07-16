#pragma once
/// @file
/// WS5-D2a — `OwnedOrBorrowed<T>`: a contiguous POD array that is either
/// OWNED (backed by a private `std::vector<T>`, mutable) or BORROWED (a
/// read-only span into an external, process-lifetime buffer — the AOT
/// mmap).  It exposes the subset of the `std::vector<T>` API that the
/// `CompilationUnit` POD sections (`code`, `intConstants`, `floatConstants`,
/// `lambdaCodeOffsets`) use, so the ~270 access sites compile unchanged.
///
/// Why: on the process-per-job CI model, every `nix` invocation today
/// copies the AOT cache blob out of the mmap and deserializes it into
/// private per-process vectors, so the CU bytecode pages are never shared
/// across processes (WS5.0 measured Shared_Clean ≈ 0).  Borrowing the POD
/// sections in place from the `MAP_PRIVATE PROT_READ` AOT mmap turns those
/// pages into `Shared_Clean` across independent processes.
///
/// Two states, one invariant:
///   * OWNED    — `owned_` holds the data; `ptr_`/`len_` mirror
///                `owned_.data()`/`owned_.size()`.  The emitter, the
///                SQLite/fresh-compile path, and any mutation use this.
///   * BORROWED — `ptr_`/`len_` point into an immutable external buffer;
///                `owned_` is empty.  Reads only.  ANY mutating call is a
///                programming error (the deserializer must materialise —
///                switch to OWNED — before mutating a borrowed section).
///
/// LOAD-BEARING INVARIANT: `ptr_ == (borrowed ? external : owned_.data())`
/// and `len_ == (borrowed ? external-len : owned_.size())` at ALL times.
/// The const `operator[]` therefore reads `ptr_[i]` with NO branch — it
/// compiles to the SAME single load as `std::vector::operator[]`, so the
/// hot `code[ip]` fetch in the VM dispatch loop keeps its cost.  Every
/// mutator (and every copy/move) re-establishes the invariant via
/// `resync()`; this is the classic "pointer into own buffer" hazard and is
/// why the copy/move constructors are hand-written rather than defaulted.
///
/// Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
///   Input Output Group.
/// SPDX-License-Identifier: Apache-2.0

#include <cstddef>
#include <cstring>
#include <type_traits>
#include <utility>
#include <vector>

namespace nix::v3 {

template<typename T>
struct OwnedOrBorrowed
{
    static_assert(std::is_trivially_copyable_v<T>,
                  "OwnedOrBorrowed is for POD sections only");

    OwnedOrBorrowed() = default;

    // ---- copy: re-sync ptr_/len_ to THIS object's storage --------------
    OwnedOrBorrowed(const OwnedOrBorrowed & o)
        : borrowed_(o.borrowed_), owned_(o.owned_)
    {
        if (borrowed_) { ptr_ = o.ptr_; len_ = o.len_; }
        else resync();
    }
    OwnedOrBorrowed & operator=(const OwnedOrBorrowed & o)
    {
        if (this == &o) return *this;
        borrowed_ = o.borrowed_;
        owned_ = o.owned_;
        if (borrowed_) { ptr_ = o.ptr_; len_ = o.len_; }
        else resync();
        return *this;
    }

    // ---- move: steal owned_ (or the borrowed span) + re-sync ----------
    OwnedOrBorrowed(OwnedOrBorrowed && o) noexcept
        : borrowed_(o.borrowed_), owned_(std::move(o.owned_))
    {
        if (borrowed_) { ptr_ = o.ptr_; len_ = o.len_; }
        else resync();
        o.reset();
    }
    OwnedOrBorrowed & operator=(OwnedOrBorrowed && o) noexcept
    {
        if (this == &o) return *this;
        borrowed_ = o.borrowed_;
        owned_ = std::move(o.owned_);
        if (borrowed_) { ptr_ = o.ptr_; len_ = o.len_; }
        else resync();
        o.reset();
        return *this;
    }

    // ---- borrow: point at an external immutable buffer ----------------
    /// After borrow(), this object is BORROWED: read-only, `owned_` freed.
    /// `p` must remain valid for as long as this object (or any copy that
    /// inherited the span) is alive — the AOT mmap is process-lifetime.
    void borrow(const T * p, std::size_t n) noexcept
    {
        std::vector<T>().swap(owned_);   // release any owned storage
        borrowed_ = true;
        ptr_ = p;
        len_ = n;
    }
    bool isBorrowed() const noexcept { return borrowed_; }

    // ---- reads (const, branch-free — the hot path) --------------------
    T operator[](std::size_t i) const noexcept { return ptr_[i]; }
    const T * data()  const noexcept { return ptr_; }
    const T * begin() const noexcept { return ptr_; }
    const T * end()   const noexcept { return ptr_ + len_; }
    std::size_t size() const noexcept { return len_; }
    bool empty()       const noexcept { return len_ == 0; }
    T back()           const noexcept { return ptr_[len_ - 1]; }

    // ---- element comparison (deserialize-verify path) -----------------
    bool operator==(const OwnedOrBorrowed & o) const noexcept
    {
        return len_ == o.len_
            && (len_ == 0 || std::memcmp(ptr_, o.ptr_, len_ * sizeof(T)) == 0);
    }
    bool operator!=(const OwnedOrBorrowed & o) const noexcept { return !(*this == o); }

    // ---- non-const accessors -----------------------------------------
    // These route through `ptr_` (the single source of truth for the live
    // data), NOT `owned_`, so that a READ through a non-const wrapper (e.g.
    // `cu.code[i]` / `cu.code.data()` on a non-const CU in the verify /
    // disasm / dedup paths) is correct even when the wrapper is BORROWED.
    // The mutable overloads exist for the emitter's jump/peephole backpatch
    // and the owning deserializer's `readBytes(data(), …)`; WRITING is only
    // ever done on an OWNED wrapper, where `ptr_ == owned_.data()`, so the
    // const_cast targets real writable storage.  It is UB to WRITE through
    // these on a borrowed wrapper (its bytes are the read-only mmap) — the
    // deserializer materialise()s before any remap, so it never does.
    T & operator[](std::size_t i) noexcept { return const_cast<T &>(ptr_[i]); }
    T * data()  noexcept { return const_cast<T *>(ptr_); }
    T & back()  noexcept { return const_cast<T &>(ptr_[len_ - 1]); }
    std::size_t capacity() const noexcept { return borrowed_ ? len_ : owned_.capacity(); }

    /// WS5-B2 — adopt a fully-built vector as the OWNED backing store (moves,
    /// no copy).  Used by LambdaTable::finalize to install the packed lambda
    /// block.  After this the object is OWNED.
    void adopt(std::vector<T> && v) noexcept
    { borrowed_ = false; owned_ = std::move(v); resync(); }

    void push_back(T v) { owned_.push_back(v); resync(); }
    void pop_back()     { owned_.pop_back();   resync(); }
    void resize(std::size_t n) { owned_.resize(n); resync(); }
    void reserve(std::size_t n) { owned_.reserve(n); resync(); }
    void clear()        { owned_.clear(); resync(); }

    /// Materialise a private OWNED copy of the current contents (whether
    /// currently borrowed or owned).  After this the object is OWNED and
    /// safely mutable — used by the AOT-borrow deserializer when a section
    /// needs per-process rewriting (e.g. the `code` symbol/pos remap) and
    /// therefore cannot stay a read-only borrow.
    void materialise()
    {
        if (!borrowed_) return;
        std::vector<T> v(ptr_, ptr_ + len_);
        borrowed_ = false;
        owned_ = std::move(v);
        resync();
    }

private:
    void resync() noexcept { ptr_ = owned_.data(); len_ = owned_.size(); }
    void reset()  noexcept { borrowed_ = false; ptr_ = nullptr; len_ = 0; }

    const T *      ptr_ = nullptr;   ///< current data (owned_ or external)
    std::size_t    len_ = 0;         ///< current element count
    bool           borrowed_ = false;
    std::vector<T> owned_;           ///< backing store when OWNED (else empty)
};

} // namespace nix::v3
