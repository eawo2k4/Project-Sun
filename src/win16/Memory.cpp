#include "win16/Memory.h"

#include <cstdio>
#include <cstring>

namespace retro::win16 {
namespace {

std::string Hex16(uint16_t v) {
    char buf[8];
    std::snprintf(buf, sizeof(buf), "%04X", v);
    return buf;
}

}  // namespace

Memory::Memory(uint32_t arenaBytes) : arena_(arenaBytes), ldt_(kLdtEntries) {}

uint16_t Memory::Allocate(uint32_t size, SegmentKind kind) {
    if (size == 0 || size > 0x10000) return 0;

    // Look for a free LDT slot, starting where the last allocation left off.
    uint16_t index = 0;
    for (uint32_t i = 0; i < kLdtEntries - kFirstIndex; ++i) {
        const uint16_t candidate =
            static_cast<uint16_t>(kFirstIndex + (nextIndex_ - kFirstIndex + i) % (kLdtEntries - kFirstIndex));
        if (!ldt_[candidate].present) {
            index = candidate;
            break;
        }
    }
    if (index == 0) return 0;

    uint32_t base = next_;
    if (kind != SegmentKind::Host) {
        base = (next_ + 15u) & ~15u;  // paragraph aligned, like the real thing
        if (base + size > arena_.size()) return 0;
        std::memset(&arena_[base], 0, size);
        next_ = base + size;
    }

    ldt_[index] = Descriptor{base, size - 1, kind, true};
    nextIndex_ = static_cast<uint16_t>(index + 1);
    return static_cast<uint16_t>((index << 3) | 7);  // TI = LDT, RPL = 3
}

void Memory::Free(uint16_t selector) {
    const uint16_t index = selector >> 3;
    if ((selector & 4) && index >= kFirstIndex && index < kLdtEntries) ldt_[index].present = false;
}

const Descriptor* Memory::Lookup(uint16_t selector) const {
    if (IsNull(selector) || !(selector & 4)) return nullptr;  // null, or a GDT selector
    const uint16_t index = selector >> 3;
    if (index >= kLdtEntries || !ldt_[index].present) return nullptr;
    return &ldt_[index];
}

uint32_t Memory::Translate(uint16_t selector, uint16_t offset, uint32_t size, Access access) const {
    const Descriptor* d = Lookup(selector);
    if (!d) {
        throw ProtectionFault(IsNull(selector) ? "null selector" : "invalid selector " + Hex16(selector),
                              selector, offset);
    }
    if (static_cast<uint32_t>(offset) + size - 1 > d->limit) {
        throw ProtectionFault("offset " + Hex16(offset) + " beyond limit " +
                                  Hex16(static_cast<uint16_t>(d->limit)) + " of " + Hex16(selector),
                              selector, offset);
    }
    switch (access) {
    case Access::Write:
        if (d->kind != SegmentKind::Data)
            throw ProtectionFault("write to code segment " + Hex16(selector), selector, offset);
        break;
    case Access::Execute:
        if (d->kind == SegmentKind::Data)
            throw ProtectionFault("execute from data segment " + Hex16(selector), selector, offset);
        break;
    case Access::Read:
        break;
    }
    if (d->kind == SegmentKind::Host)
        throw ProtectionFault("access to host segment " + Hex16(selector), selector, offset);
    return d->base + offset;
}

uint8_t Memory::Read8(uint16_t sel, uint16_t off, Access access) const {
    return arena_[Translate(sel, off, 1, access)];
}

uint16_t Memory::Read16(uint16_t sel, uint16_t off, Access access) const {
    const uint32_t a = Translate(sel, off, 2, access);
    return static_cast<uint16_t>(arena_[a] | (arena_[a + 1] << 8));
}

void Memory::Write8(uint16_t sel, uint16_t off, uint8_t value) {
    arena_[Translate(sel, off, 1, Access::Write)] = value;
}

void Memory::Write16(uint16_t sel, uint16_t off, uint16_t value) {
    const uint32_t a = Translate(sel, off, 2, Access::Write);
    arena_[a] = static_cast<uint8_t>(value);
    arena_[a + 1] = static_cast<uint8_t>(value >> 8);
}

uint8_t* Memory::SegmentData(uint16_t selector) {
    const Descriptor* d = Lookup(selector);
    return d && d->kind != SegmentKind::Host ? &arena_[d->base] : nullptr;
}

const uint8_t* Memory::SegmentData(uint16_t selector) const {
    const Descriptor* d = Lookup(selector);
    return d && d->kind != SegmentKind::Host ? &arena_[d->base] : nullptr;
}

uint32_t Memory::SegmentSize(uint16_t selector) const {
    const Descriptor* d = Lookup(selector);
    return d ? d->limit + 1 : 0;
}

std::string Memory::ReadString(uint16_t sel, uint16_t off, size_t maxLength) const {
    std::string s;
    const uint8_t* data = SegmentData(sel);
    const uint32_t size = SegmentSize(sel);
    if (!data) return s;
    for (uint32_t i = off; i < size && s.size() < maxLength && data[i]; ++i)
        s.push_back(static_cast<char>(data[i]));
    return s;
}

}  // namespace retro::win16
