#include "win16/NeImage.h"

#include <cctype>
#include <cstdio>
#include <iterator>

namespace retro::win16 {
namespace {

// NE header field offsets (relative to the "NE" signature).
constexpr uint32_t kFlags = 0x0C, kAutoData = 0x0E, kHeap = 0x10, kStack = 0x12, kCsIp = 0x14,
                   kSsSp = 0x18, kSegCount = 0x1C, kModCount = 0x1E, kSegTable = 0x22, kResTable = 0x24,
                   kResNames = 0x26, kModTable = 0x28, kImpNames = 0x2A, kEntryTable = 0x04,
                   kEntryBytes = 0x06, kAlign = 0x32, kTargetOS = 0x36, kExpVer = 0x3E;
constexpr uint32_t kHeaderSize = 0x40;

class Reader {
public:
    explicit Reader(const std::vector<uint8_t>& d) : d_(d) {}
    bool Has(uint64_t off, uint64_t n) const { return off + n <= d_.size(); }
    uint8_t U8(uint32_t off) const { return d_[off]; }
    uint16_t U16(uint32_t off) const { return uint16_t(d_[off] | (d_[off + 1] << 8)); }
    uint32_t U32(uint32_t off) const { return U16(off) | (uint32_t(U16(off + 2)) << 16); }
    const uint8_t* Data() const { return d_.data(); }
    // Pascal (length-prefixed) string.
    bool PString(uint32_t off, std::string& out) const {
        if (!Has(off, 1) || !Has(off + 1, d_[off])) return false;
        out.assign(reinterpret_cast<const char*>(&d_[off + 1]), d_[off]);
        return true;
    }

private:
    const std::vector<uint8_t>& d_;
};

std::string Fmt(const char* format, unsigned a, unsigned b = 0) {
    char buf[128];
    std::snprintf(buf, sizeof(buf), format, a, b);
    return buf;
}

bool ParseRelocations(const Reader& rd, uint32_t at, uint32_t impNames, NeSegment& seg,
                      std::string& error) {
    if (!rd.Has(at, 2)) {
        error = "truncated relocation count";
        return false;
    }
    const uint16_t count = rd.U16(at);
    at += 2;
    if (!rd.Has(at, uint64_t(count) * 8)) {
        error = "truncated relocation records";
        return false;
    }
    for (uint16_t i = 0; i < count; ++i, at += 8) {
        NeRelocation r;
        r.source = RelocSource(rd.U8(at));
        const uint8_t flags = rd.U8(at + 1);
        r.target = RelocTarget(flags & 3);
        r.additive = (flags & 4) != 0;
        r.offset = rd.U16(at + 2);
        switch (r.target) {
        case RelocTarget::Internal:
            r.segment = rd.U8(at + 4);
            r.targetOffset = rd.U16(at + 6);
            break;
        case RelocTarget::ImportOrdinal:
            r.module = rd.U16(at + 4);
            r.ordinal = rd.U16(at + 6);
            break;
        case RelocTarget::ImportName:
            r.module = rd.U16(at + 4);
            if (!rd.PString(impNames + rd.U16(at + 6), r.name)) {
                error = "relocation import name outside the file";
                return false;
            }
            break;
        case RelocTarget::OsFixup:
            break;
        }
        seg.relocations.push_back(std::move(r));
    }
    return true;
}

bool ParseEntries(const Reader& rd, uint32_t at, uint32_t size, NeImage& out, std::string& error) {
    const uint32_t end = at + size;
    uint16_t ordinal = 1;
    while (at < end && rd.Has(at, 1)) {
        const uint8_t count = rd.U8(at);
        if (count == 0) break;
        if (!rd.Has(at, 2)) break;
        const uint8_t indicator = rd.U8(at + 1);
        at += 2;
        if (indicator == 0) {  // unused ordinals
            ordinal = uint16_t(ordinal + count);
            continue;
        }
        const uint32_t entrySize = indicator == 0xFF ? 6 : 3;
        if (!rd.Has(at, uint64_t(count) * entrySize)) {
            error = "truncated entry table";
            return false;
        }
        for (uint8_t i = 0; i < count; ++i, at += entrySize, ++ordinal) {
            NeEntry e;
            e.ordinal = ordinal;
            e.exported = (rd.U8(at) & 1) != 0;
            if (indicator == 0xFF) {  // movable: flags, INT 3Fh, segment, offset
                e.segment = rd.U8(at + 3);
                e.offset = rd.U16(at + 4);
            } else {
                e.segment = indicator;
                e.offset = rd.U16(at + 1);
            }
            out.entries.push_back(e);
        }
    }
    return true;
}

// A type or name field of the resource table: 8000h | id, or the offset of a
// Pascal string relative to the table.
bool ResourceKey(const Reader& rd, uint32_t table, uint16_t field, uint16_t& id, std::string& name) {
    if (field & 0x8000) {
        id = uint16_t(field & 0x7FFF);
        return true;
    }
    if (!rd.PString(table + field, name)) return false;
    for (char& c : name) c = char(std::toupper(static_cast<unsigned char>(c)));
    return true;
}

// Resource table: alignment shift, then TYPEINFO records { type, count,
// reserved (4), NAMEINFO[count] { offset, length (both in alignment units),
// flags, id, handle, usage } } up to a zero type, then the name strings.
bool ParseResources(const Reader& rd, uint32_t table, NeImage& out, std::string& error) {
    if (!rd.Has(table, 2)) {
        error = "resource table outside the file";
        return false;
    }
    const uint16_t shift = rd.U16(table);
    if (shift > 15) {
        error = Fmt("bad resource alignment shift %u", shift);
        return false;
    }
    uint32_t at = table + 2;
    for (;;) {
        if (!rd.Has(at, 2)) {
            error = "truncated resource table";
            return false;
        }
        const uint16_t type = rd.U16(at);
        if (type == 0) break;
        if (!rd.Has(at, 8)) {
            error = "truncated resource table";
            return false;
        }
        const uint16_t count = rd.U16(at + 2);
        at += 8;
        if (!rd.Has(at, uint64_t(count) * 12)) {
            error = "truncated resource table";
            return false;
        }
        for (uint16_t i = 0; i < count; ++i, at += 12) {
            NeResource r;
            if (!ResourceKey(rd, table, type, r.typeId, r.typeName) ||
                !ResourceKey(rd, table, rd.U16(at + 6), r.id, r.name)) {
                error = "resource name outside the file";
                return false;
            }
            r.flags = rd.U16(at + 4);
            r.tableOffset = uint16_t(at - table);
            const uint64_t offset = uint64_t(rd.U16(at)) << shift;
            uint64_t length = uint64_t(rd.U16(at + 2)) << shift;
            if (length && !rd.Has(offset, 1)) {
                error = Fmt("resource %u data outside the file", unsigned(out.resources.size() + 1));
                return false;
            }
            // The last resource's alignment padding may be missing from the file.
            while (length && !rd.Has(offset, length)) --length;
            if (length) r.data.assign(rd.Data() + offset, rd.Data() + offset + length);
            out.resources.push_back(std::move(r));
        }
    }
    return true;
}

}  // namespace

std::string DescribeResourceType(const NeResource& r) {
    if (r.typeId == 0) return r.typeName;
    static const char* const kNames[] = {nullptr, "CURSOR", "BITMAP", "ICON", "MENU", "DIALOG", "STRING",
                                         "FONTDIR", "FONT", "ACCELERATOR", "RCDATA", nullptr,
                                         "GROUP_CURSOR", nullptr, "GROUP_ICON"};
    if (r.typeId < std::size(kNames) && kNames[r.typeId]) return kNames[r.typeId];
    return "#" + std::to_string(r.typeId);
}

const NeEntry* NeImage::FindEntry(uint16_t ordinal) const {
    for (const NeEntry& e : entries) {
        if (e.ordinal == ordinal) return &e;
    }
    return nullptr;
}

bool ParseNe(const std::vector<uint8_t>& file, NeImage& out, std::string& error) {
    out = NeImage{};
    const Reader rd(file);
    if (!rd.Has(0, 0x40) || rd.U16(0) != 0x5A4D) {
        error = "not an MZ executable";
        return false;
    }
    const uint32_t ne = rd.U32(0x3C);
    if (!rd.Has(ne, kHeaderSize) || rd.U16(ne) != 0x454E) {
        error = "no NE header";
        return false;
    }

    out.flags = rd.U16(ne + kFlags);
    out.targetOS = rd.U8(ne + kTargetOS);
    out.expectedWinVer = rd.U16(ne + kExpVer);
    out.autoDataSegment = rd.U16(ne + kAutoData);
    out.heapSize = rd.U16(ne + kHeap);
    out.stackSize = rd.U16(ne + kStack);
    out.entryIp = rd.U16(ne + kCsIp);
    out.entrySegment = rd.U16(ne + kCsIp + 2);
    out.initialSp = rd.U16(ne + kSsSp);
    out.stackSegment = rd.U16(ne + kSsSp + 2);

    const uint16_t segCount = rd.U16(ne + kSegCount);
    const uint16_t modCount = rd.U16(ne + kModCount);
    const uint32_t segTable = ne + rd.U16(ne + kSegTable);
    const uint32_t modTable = ne + rd.U16(ne + kModTable);
    const uint32_t impNames = ne + rd.U16(ne + kImpNames);
    const uint32_t resNames = ne + rd.U16(ne + kResNames);
    const uint32_t resTable = ne + rd.U16(ne + kResTable);
    uint16_t shift = rd.U16(ne + kAlign);
    if (shift == 0) shift = 9;  // 512-byte sectors
    if (shift > 15) {
        error = Fmt("bad alignment shift %u", shift);
        return false;
    }

    rd.PString(resNames, out.moduleName);  // first resident name is the module name

    if (!rd.Has(segTable, uint64_t(segCount) * 8)) {
        error = "segment table outside the file";
        return false;
    }
    for (uint16_t i = 0; i < segCount; ++i) {
        const uint32_t e = segTable + i * 8u;
        NeSegment s;
        const uint16_t sector = rd.U16(e);
        const uint16_t length = rd.U16(e + 2);
        s.flags = rd.U16(e + 4);
        const uint16_t minAlloc = rd.U16(e + 6);
        s.fileOffset = uint32_t(sector) << shift;
        s.fileLength = sector ? (length ? length : 0x10000u) : 0;
        s.minAlloc = minAlloc ? minAlloc : 0x10000u;
        if (s.minAlloc < s.fileLength) s.minAlloc = s.fileLength;
        if (s.fileLength && !rd.Has(s.fileOffset, s.fileLength)) {
            error = Fmt("segment %u data outside the file", i + 1u);
            return false;
        }
        if (s.flags & NeSegment::kIterated) {
            error = Fmt("segment %u uses iterated data (not supported yet)", i + 1u);
            return false;
        }
        if ((s.flags & NeSegment::kRelocInfo) && s.fileLength &&
            !ParseRelocations(rd, s.fileOffset + s.fileLength, impNames, s, error)) {
            error = Fmt("segment %u: ", i + 1u) + error;
            return false;
        }
        out.segments.push_back(std::move(s));
    }

    if (!rd.Has(modTable, uint64_t(modCount) * 2)) {
        error = "module reference table outside the file";
        return false;
    }
    for (uint16_t i = 0; i < modCount; ++i) {
        std::string name;
        if (!rd.PString(impNames + rd.U16(modTable + i * 2u), name)) {
            error = Fmt("module reference %u outside the file", i + 1u);
            return false;
        }
        out.moduleRefs.push_back(std::move(name));
    }

    const uint16_t entryBytes = rd.U16(ne + kEntryBytes);
    if (entryBytes && !ParseEntries(rd, ne + rd.U16(ne + kEntryTable), entryBytes, out, error))
        return false;

    // An empty resource table has the same offset as the resident names.
    if (resTable != resNames && !ParseResources(rd, resTable, out, error)) return false;

    auto validSegment = [&](uint16_t n) { return n >= 1 && n <= out.segments.size(); };
    if (!out.IsLibrary() && !validSegment(out.entrySegment)) {
        error = Fmt("entry point segment %u does not exist", out.entrySegment);
        return false;
    }
    if (out.autoDataSegment && !validSegment(out.autoDataSegment)) {
        error = Fmt("automatic data segment %u does not exist", out.autoDataSegment);
        return false;
    }
    return true;
}

}  // namespace retro::win16
