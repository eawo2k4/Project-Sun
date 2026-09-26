#pragma once

// Full parse of a 16-bit NE ("New Executable") image: everything the loader
// needs, straight from the file bytes. Pure data, so it's unit-testable
// with synthetic images.

#include <cstdint>
#include <string>
#include <vector>

namespace retro::win16 {

// What a relocation patches at its source offset(s).
enum class RelocSource : uint8_t {
    LoByte = 0,      // low byte of an offset
    Selector = 2,    // 16-bit selector
    FarPointer = 3,  // offset:selector (4 bytes)
    Offset = 5,      // 16-bit offset
};

// What it points to.
enum class RelocTarget : uint8_t {
    Internal = 0,       // a segment of this module
    ImportOrdinal = 1,  // MODULE.ordinal
    ImportName = 2,     // MODULE.name
    OsFixup = 3,        // floating-point emulator fixups (WIN87EM)
};

struct NeRelocation {
    RelocSource source = RelocSource::Offset;
    RelocTarget target = RelocTarget::Internal;
    bool additive = false;  // add to the existing value instead of following a chain
    uint16_t offset = 0;    // first source offset in the segment

    // Internal: segment number (1-based); 0xFF = movable, via entry ordinal.
    uint8_t segment = 0;
    uint16_t targetOffset = 0;  // offset in that segment, or the entry ordinal

    // Imports: module (1-based index into NeImage::moduleRefs) and ordinal/name.
    uint16_t module = 0;
    uint16_t ordinal = 0;
    std::string name;
};

struct NeSegment {
    static constexpr uint16_t kData = 0x0001;
    static constexpr uint16_t kIterated = 0x0008;
    static constexpr uint16_t kMovable = 0x0010;
    static constexpr uint16_t kRelocInfo = 0x0100;

    uint32_t fileOffset = 0;  // 0: no data in the file (zero-filled)
    uint32_t fileLength = 0;
    uint16_t flags = 0;
    uint32_t minAlloc = 0;    // bytes to allocate (>= fileLength)
    std::vector<NeRelocation> relocations;

    bool IsData() const { return (flags & kData) != 0; }
};

struct NeEntry {
    uint16_t ordinal = 0;
    uint8_t segment = 0;  // 1-based; 0xFE = constant
    uint16_t offset = 0;
    bool exported = false;
};

// Resource types (RT_xxx) with integer ids.
namespace res {
constexpr uint16_t Cursor = 1, Bitmap = 2, Icon = 3, Menu = 4, Dialog = 5, String = 6, FontDir = 7,
                   Font = 8, Accelerator = 9, RcData = 10, GroupCursor = 12, GroupIcon = 14;
}  // namespace res

// A resource: type and name are each an integer id (id != 0) or a string
// (upper case, as the resource compiler stores them).
struct NeResource {
    uint16_t typeId = 0;
    std::string typeName;
    uint16_t id = 0;
    std::string name;
    uint16_t flags = 0;
    uint16_t tableOffset = 0;   // NAMEINFO offset in the resource table: the HRSRC
    std::vector<uint8_t> data;  // the file bytes (length rounded up to the alignment)
};

// Readable resource type: "BITMAP", "#15", or the type's name.
std::string DescribeResourceType(const NeResource& r);

struct NeImage {
    std::string moduleName;
    uint16_t flags = 0;
    uint8_t targetOS = 0;
    uint16_t expectedWinVer = 0;

    uint16_t autoDataSegment = 0;  // 1-based, 0 = none
    uint16_t heapSize = 0;
    uint16_t stackSize = 0;
    uint16_t entrySegment = 0;     // CS:IP, segment 1-based
    uint16_t entryIp = 0;
    uint16_t stackSegment = 0;     // SS:SP, segment 1-based
    uint16_t initialSp = 0;

    std::vector<NeSegment> segments;         // segment n is segments[n - 1]
    std::vector<std::string> moduleRefs;     // module n is moduleRefs[n - 1]
    std::vector<NeEntry> entries;
    std::vector<NeResource> resources;

    bool IsLibrary() const { return (flags & 0x8000) != 0; }
    const NeEntry* FindEntry(uint16_t ordinal) const;
};

// Parses `file` (a whole MZ+NE executable). False with `error` set if it isn't
// an NE image or any table points outside the file.
bool ParseNe(const std::vector<uint8_t>& file, NeImage& out, std::string& error);

}  // namespace retro::win16
