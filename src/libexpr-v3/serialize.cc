/// @file
/// v3 CompilationUnit serialization.  See include/v3/serialize.hh.
///
/// Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
///   Input Output Group.
/// SPDX-License-Identifier: Apache-2.0

#include "v3/serialize.hh"
#include "v3/primop.hh"

#include <cstring>

namespace nix::v3::serialize {

namespace {

/// Tiny streaming writer that appends to an std::string.
struct Writer {
    std::string & out;
    void writeBytes(const void * data, size_t n) {
        out.append(static_cast<const char *>(data), n);
    }
    void u32(uint32_t v) { writeBytes(&v, 4); }
    void u64(uint64_t v) { writeBytes(&v, 8); }
    void i64(int64_t v)  { writeBytes(&v, 8); }
    void f64(double v)   { writeBytes(&v, 8); }
    void u8(uint8_t v)   { out.push_back(static_cast<char>(v)); }
    void str(std::string_view s) {
        u32(static_cast<uint32_t>(s.size()));
        writeBytes(s.data(), s.size());
    }
};

/// Tiny streaming reader from a string_view.  Throws on
/// out-of-bounds reads — the format is fixed-layout so we know
/// the exact byte counts at every step.
struct Reader {
    std::string_view buf;
    size_t pos = 0;
    void readBytes(void * dst, size_t n) {
        if (pos + n > buf.size())
            throw SerializationError("v3 deserialize: truncated blob");
        std::memcpy(dst, buf.data() + pos, n);
        pos += n;
    }
    uint32_t u32() { uint32_t v; readBytes(&v, 4); return v; }
    uint64_t u64() { uint64_t v; readBytes(&v, 8); return v; }
    int64_t  i64() { int64_t v;  readBytes(&v, 8); return v; }
    double   f64() { double v;   readBytes(&v, 8); return v; }
    uint8_t  u8()  {
        if (pos >= buf.size())
            throw SerializationError("v3 deserialize: truncated blob");
        return static_cast<uint8_t>(buf[pos++]);
    }
    std::string str() {
        uint32_t n = u32();
        if (pos + n > buf.size())
            throw SerializationError("v3 deserialize: truncated string");
        std::string s(buf.data() + pos, n);
        pos += n;
        return s;
    }
};

} // namespace

bool isCacheable(const CompilationUnit & /*cu*/)
{
    // Every CU the lowerer + emitter produce is cacheable today —
    // the only opaque data we don't serialize directly (primops,
    // attr-select IC) has clean round-trip representations
    // (resolve-by-name and zero-on-load respectively).
    return true;
}

std::string serializeCU(const CompilationUnit & cu)
{
    if (!isCacheable(cu))
        throw SerializationError("v3 serialize: CU is not cacheable");

    std::string out;
    // Reserve a generous head-of-line allocation.  Most CUs are
    // small (~1-50 KB).  Growing reserves later is fine.
    out.reserve(64 * 1024);
    Writer w{out};

    // Header: magic + schema version.
    w.writeBytes(kMagic, sizeof(kMagic));
    w.u32(kSchemaVersion);

    // Section: code.
    w.u32(static_cast<uint32_t>(cu.code.size()));
    w.writeBytes(cu.code.data(), cu.code.size() * sizeof(uint32_t));

    // Section: intConstants.
    w.u32(static_cast<uint32_t>(cu.intConstants.size()));
    for (auto v : cu.intConstants) w.i64(v);

    // Section: floatConstants.
    w.u32(static_cast<uint32_t>(cu.floatConstants.size()));
    for (auto v : cu.floatConstants) w.f64(v);

    // Section: stringConstants.
    w.u32(static_cast<uint32_t>(cu.stringConstants.size()));
    for (auto & s : cu.stringConstants) w.str(s);

    // Section: symbolTable.
    w.u32(static_cast<uint32_t>(cu.symbolTable.size()));
    for (auto & s : cu.symbolTable) w.str(s);

    // Section: lambdas (LambdaDescriptor with Formal vector).
    w.u32(static_cast<uint32_t>(cu.lambdas.size()));
    for (auto & l : cu.lambdas) {
        w.u32(l.codeOffset);
        w.u32(l.prologueOffset);
        w.u32(l.nUpvalues);
        w.u32(l.nLocals);
        w.u8(l.arity);
        w.u8(l.hasFormals);
        w.u8(l.ellipsis);
        w.u8(0);  // _pad
        w.u32(static_cast<uint32_t>(l.formals.size()));
        for (auto & f : l.formals) {
            w.u32(f.name);
            w.u8(f.hasDefault ? 1 : 0);
            w.u8(0); w.u8(0); w.u8(0);  // pad to align pos
            w.u32(f.pos);
        }
    }

    // Section: lambdaCodeOffsets.
    w.u32(static_cast<uint32_t>(cu.lambdaCodeOffsets.size()));
    w.writeBytes(cu.lambdaCodeOffsets.data(),
                  cu.lambdaCodeOffsets.size() * sizeof(uint32_t));

    // Section: primops (resolved by name on load).  PrimOp::name is
    // a string_view referencing the registered name table; copy it
    // out as a length-prefixed string.
    w.u32(static_cast<uint32_t>(cu.primops.size()));
    for (auto * po : cu.primops) {
        if (!po) {
            // Sentinel — should not happen.  Emit empty string and
            // resolve to throw at load time if encountered.
            w.str("");
        } else {
            w.str(po->name);
        }
    }

    // Section: attrSelectCache size (entries are zeroed on load).
    w.u32(static_cast<uint32_t>(cu.attrSelectCache.size()));

    // Section: entryOffset.
    w.u32(cu.entryOffset);

    return out;
}

CompilationUnit deserializeCU(std::string_view blob)
{
    Reader r{blob};

    // Verify magic.
    char magic[sizeof(kMagic)];
    r.readBytes(magic, sizeof(magic));
    if (std::memcmp(magic, kMagic, sizeof(kMagic)) != 0)
        throw SerializationError("v3 deserialize: bad magic");

    // Verify schema version.
    uint32_t schema = r.u32();
    if (schema != kSchemaVersion)
        throw SerializationError("v3 deserialize: schema mismatch (got "
            + std::to_string(schema) + ", want "
            + std::to_string(kSchemaVersion) + ")");

    CompilationUnit cu;

    // Section: code.
    {
        uint32_t n = r.u32();
        cu.code.resize(n);
        r.readBytes(cu.code.data(), n * sizeof(uint32_t));
    }

    // Section: intConstants.
    {
        uint32_t n = r.u32();
        cu.intConstants.reserve(n);
        for (uint32_t i = 0; i < n; ++i) cu.intConstants.push_back(r.i64());
    }

    // Section: floatConstants.
    {
        uint32_t n = r.u32();
        cu.floatConstants.reserve(n);
        for (uint32_t i = 0; i < n; ++i) cu.floatConstants.push_back(r.f64());
    }

    // Section: stringConstants.
    {
        uint32_t n = r.u32();
        cu.stringConstants.reserve(n);
        for (uint32_t i = 0; i < n; ++i) cu.stringConstants.push_back(r.str());
    }

    // Section: symbolTable.
    {
        uint32_t n = r.u32();
        cu.symbolTable.reserve(n);
        for (uint32_t i = 0; i < n; ++i) cu.symbolTable.push_back(r.str());
    }

    // Section: lambdas.
    {
        uint32_t n = r.u32();
        cu.lambdas.reserve(n);
        for (uint32_t i = 0; i < n; ++i) {
            LambdaDescriptor l;
            l.codeOffset    = r.u32();
            l.prologueOffset = r.u32();
            l.nUpvalues     = static_cast<uint16_t>(r.u32());
            l.nLocals       = static_cast<uint16_t>(r.u32());
            l.arity         = r.u8();
            l.hasFormals    = r.u8();
            l.ellipsis      = r.u8();
            r.u8();  // _pad
            uint32_t nFormals = r.u32();
            l.formals.reserve(nFormals);
            for (uint32_t j = 0; j < nFormals; ++j) {
                LambdaDescriptor::Formal f;
                f.name       = r.u32();
                f.hasDefault = r.u8() != 0;
                r.u8(); r.u8(); r.u8();  // pad
                f.pos        = r.u32();
                l.formals.push_back(f);
            }
            cu.lambdas.push_back(std::move(l));
        }
    }

    // Section: lambdaCodeOffsets.
    {
        uint32_t n = r.u32();
        cu.lambdaCodeOffsets.resize(n);
        r.readBytes(cu.lambdaCodeOffsets.data(), n * sizeof(uint32_t));
    }

    // Section: primops (resolve by name).
    {
        uint32_t n = r.u32();
        cu.primops.reserve(n);
        for (uint32_t i = 0; i < n; ++i) {
            std::string name = r.str();
            if (name.empty()) {
                throw SerializationError(
                    "v3 deserialize: primop slot has empty name");
            }
            const PrimOp * po = findPrimOp(name);
            if (!po) {
                throw SerializationError(
                    "v3 deserialize: unknown primop '" + name + "'");
            }
            cu.primops.push_back(po);
        }
    }

    // Section: attrSelectCache size (zeroed entries on load).
    {
        uint32_t n = r.u32();
        cu.attrSelectCache.resize(n);
    }

    // Section: entryOffset.
    cu.entryOffset = r.u32();

    if (r.pos != blob.size())
        throw SerializationError(
            "v3 deserialize: trailing bytes after end-of-stream");

    return cu;
}

} // namespace nix::v3::serialize
