#pragma once

// Segmented memory for 16-bit protected-mode Windows programs.
//
// Programs only ever see selector:offset addresses. A selector indexes our
// virtual Local Descriptor Table; the descriptor gives the segment's base in
// a flat linear arena, its limit and its type. Every access is checked the way
// a 286 checks it: a null or absent selector, an offset past the limit, or a
// write to a code segment raises a general protection fault (#GP).
//
// Selectors look like the ones Windows 3.x hands out: TI = 1 (LDT), RPL = 3,
// first index 0x20, so the first segment is 0x0107, then 0x010F, ...

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace retro::win16 {

enum class SegmentKind : uint8_t {
    Code,  // executable + readable
    Data,  // readable + writable
    Host,  // executable, no bytes: control transfers here run host (C++) code
};

enum class Access : uint8_t { Read, Write, Execute };

struct Descriptor {
    uint32_t base = 0;
    uint32_t limit = 0;  // last valid offset
    SegmentKind kind = SegmentKind::Data;
    bool present = false;
};

// Thrown by checked accesses; the CPU turns it into a #GP fault.
struct ProtectionFault : std::runtime_error {
    uint16_t selector;
    uint16_t offset;
    ProtectionFault(const std::string& what, uint16_t sel, uint16_t off)
        : std::runtime_error(what), selector(sel), offset(off) {}
};

class Memory {
public:
    static constexpr uint16_t kFirstIndex = 0x20;
    static constexpr uint16_t kLdtEntries = 8192;

    explicit Memory(uint32_t arenaBytes = 16u * 1024u * 1024u);

    // Allocates a segment of `size` bytes (1..65536), zero-filled.
    // Returns its selector, or 0 if the arena or the LDT is full.
    uint16_t Allocate(uint32_t size, SegmentKind kind);

    // Marks the selector not present and returns its memory for reuse.
    void Free(uint16_t selector);

    // Descriptor for a selector, or nullptr if the selector is null, not an
    // LDT selector, out of range or not present.
    const Descriptor* Lookup(uint16_t selector) const;

    static bool IsNull(uint16_t selector) { return (selector & ~3u) == 0; }

    // Checked translation of selector:offset for `size` bytes.
    uint32_t Translate(uint16_t selector, uint16_t offset, uint32_t size, Access access) const;

    uint8_t Read8(uint16_t sel, uint16_t off, Access access = Access::Read) const;
    uint16_t Read16(uint16_t sel, uint16_t off, Access access = Access::Read) const;
    void Write8(uint16_t sel, uint16_t off, uint8_t value);
    void Write16(uint16_t sel, uint16_t off, uint16_t value);

    // Unchecked host access to a whole segment (loader, host API
    // implementations). Returns nullptr for an invalid selector.
    uint8_t* SegmentData(uint16_t selector);
    const uint8_t* SegmentData(uint16_t selector) const;
    uint32_t SegmentSize(uint16_t selector) const;

    // Reads a NUL-terminated string at sel:off (bounded by the segment).
    std::string ReadString(uint16_t sel, uint16_t off, size_t maxLength = 4096) const;

    uint32_t ArenaUsed() const { return next_; }

private:
    struct Range {
        uint32_t base;
        uint32_t size;
    };

    std::vector<uint8_t> arena_;
    std::vector<Descriptor> ldt_;
    std::vector<Range> free_;  // freed arena ranges, reused first-fit
    uint32_t next_ = 16;       // keep linear 0 unused
    uint16_t nextIndex_ = kFirstIndex;
};

}  // namespace retro::win16
