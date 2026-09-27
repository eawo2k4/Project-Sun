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
//
// A few fixed GDT selectors exist as well, like Windows' 0040h, which maps the
// BIOS data area (DefineFixed).

#include <cstdint>
#include <functional>
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
    bool owner = true;  // owns its arena range (false: an alias, or a bare descriptor)
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

    // Changes a segment's size (1..65536), keeping its selector and contents;
    // new bytes are zero. The segment may move in the arena (its base changes,
    // which programs never see). False if out of memory or not a data/code segment.
    bool Resize(uint16_t selector, uint32_t size);
    // Selector management (KERNEL's AllocSelector & co, DPMI). These
    // descriptors don't own memory: freeing one leaves the bytes alone.
    // A new descriptor: base 0, limit 0, until SetBase/SetLimit or CopyDescriptor.
    uint16_t AllocateDescriptor(SegmentKind kind = SegmentKind::Data);
    // A second selector for a segment's bytes, of another type (a code alias
    // of data, say). 0 if `selector` isn't a code or data segment.
    uint16_t Alias(uint16_t selector, SegmentKind kind);
    // `to` takes `from`'s base and limit, with type `kind` (PrestoChangoSelector).
    bool CopyDescriptor(uint16_t from, uint16_t to, SegmentKind kind);
    bool SetBase(uint16_t selector, uint32_t base);    // false if it would reach past the arena
    bool SetLimit(uint16_t selector, uint32_t limit);  // limits above FFFFh are clamped
    bool SetKind(uint16_t selector, SegmentKind kind);

    // Defines a fixed GDT selector (0x0040, say: any RPL selects it) of `size`
    // bytes of zero-filled data. `refresh`, if given, runs before every access
    // through the selector, to keep live fields (a tick counter) current.
    // False if the selector isn't a GDT one, is already defined, or no memory.
    bool DefineFixed(uint16_t selector, uint32_t size, std::function<void(uint8_t*)> refresh = {});

    // Arena bytes not yet handed out (GetFreeSpace, GlobalCompact).
    uint32_t FreeBytes() const;

    // Descriptor for a selector, or nullptr if the selector is null, not
    // present, or a GDT selector that isn't one of the fixed ones.
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
    // Paragraph-aligned arena range of `span` bytes; UINT32_MAX if none.
    uint32_t AllocRange(uint32_t span);
    // A free LDT index, or 0.
    uint16_t FreeIndex() const;
    // The LDT descriptor of a code or data selector (not host, not GDT), or nullptr.
    Descriptor* Editable(uint16_t selector);

    std::vector<uint8_t> arena_;
    std::vector<Descriptor> ldt_;
    struct Fixed {
        uint16_t index;
        Descriptor descriptor;
        std::function<void(uint8_t*)> refresh;
    };
    const Fixed* FindFixed(uint16_t selector) const;

    std::vector<Range> free_;  // freed arena ranges, reused first-fit
    std::vector<Fixed> fixed_;
    uint32_t next_ = 16;       // keep linear 0 unused
    uint16_t nextIndex_ = kFirstIndex;
};

}  // namespace retro::win16
