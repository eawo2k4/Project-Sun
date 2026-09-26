#include "win16/LocalHeap.h"

#include <algorithm>
#include <cstring>

namespace retro::win16 {
namespace {

uint16_t SpanOf(uint32_t bytes) { return uint16_t(std::max<uint32_t>(4, (bytes + 3u) & ~3u)); }

}  // namespace

LocalHeaps::Heap* LocalHeaps::Find(uint16_t sel) {
    const auto it = heaps_.find(sel);
    return it == heaps_.end() ? nullptr : &it->second;
}

const LocalHeaps::Heap* LocalHeaps::Find(uint16_t sel) const {
    const auto it = heaps_.find(sel);
    return it == heaps_.end() ? nullptr : &it->second;
}

bool LocalHeaps::Init(uint16_t sel, uint16_t start, uint16_t end) {
    const uint32_t size = mem_.SegmentSize(sel);
    const Descriptor* d = mem_.Lookup(sel);
    if (!d || d->kind != SegmentKind::Data) return false;
    Heap h;
    if (start == 0) {  // the last `end` bytes of the segment
        if (end == 0 || end > size) return false;
        h.start = uint16_t(size - end);
        h.end = size;
    } else {
        h.start = start;
        h.end = std::min<uint32_t>(uint32_t(end) + 1, size);  // `end` is the last byte
    }
    h.start = uint16_t(std::max<uint32_t>(4, (h.start + 3u) & ~3u));  // never hand out offset 0
    if (h.start >= h.end) return false;
    heaps_[sel] = std::move(h);
    return true;
}

uint16_t LocalHeaps::Reserve(uint16_t sel, Heap& h, uint16_t span) {
    for (;;) {
        uint32_t at = h.start;
        for (const auto& [address, b] : h.blocks) {
            if (address - at >= span) break;
            at = uint32_t(address) + b.span;
        }
        if (at + span <= h.end) return uint16_t(at);
        // Full: grow the segment if the heap ends where the segment does.
        const uint32_t size = mem_.SegmentSize(sel);
        if (h.end != size || size >= 0x10000) return 0;
        const uint32_t grown = std::min<uint32_t>(0x10000, ((at + span + 0x3FF) & ~0x3FFu));
        if (grown <= size || !mem_.Resize(sel, grown)) return 0;
        h.end = grown;
    }
}

void LocalHeaps::WriteEntry(uint16_t sel, uint16_t entry, uint16_t address, const Block& b) {
    uint8_t* data = mem_.SegmentData(sel);
    data[entry] = uint8_t(address);
    data[entry + 1] = uint8_t(address >> 8);
    data[entry + 2] = uint8_t(b.flags >> 8);  // discard flags
    data[entry + 3] = b.locks;
}

LocalHeaps::Block* LocalHeaps::BlockOf(Heap& h, uint16_t handle, uint16_t& address) {
    return const_cast<Block*>(static_cast<const LocalHeaps*>(this)->BlockOf(h, handle, address));
}

const LocalHeaps::Block* LocalHeaps::BlockOf(const Heap& h, uint16_t handle, uint16_t& address) const {
    const auto it = h.blocks.find(handle);
    if (it == h.blocks.end()) return nullptr;
    if (!it->second.entry) {  // a fixed block's handle is its address
        if (it->second.handle) return nullptr;  // moveable data: not a handle
        address = handle;
        return &it->second;
    }
    for (const auto& [a, b] : h.blocks) {
        if (!b.entry && b.handle == handle) {
            address = a;
            return &b;
        }
    }
    return nullptr;
}

uint16_t LocalHeaps::Alloc(uint16_t sel, uint16_t flags, uint16_t bytes) {
    Heap* h = Find(sel);
    if (!h) return 0;
    const bool moveable = (flags & lmem::Moveable) != 0;
    uint16_t entry = 0;
    if (moveable) {
        entry = Reserve(sel, *h, 4);
        if (!entry) return 0;
        Block e;
        e.span = 4;
        e.entry = true;
        h->blocks[entry] = e;
    }
    const uint16_t span = SpanOf(bytes);
    const uint16_t address = Reserve(sel, *h, span);
    if (!address) {
        if (entry) h->blocks.erase(entry);
        return 0;
    }
    Block b;
    b.size = bytes;
    b.span = span;
    b.handle = entry;
    b.flags = flags;
    h->blocks[address] = b;
    std::memset(mem_.SegmentData(sel) + address, 0, span);  // zeroed whether or not LMEM_ZEROINIT
    if (entry) WriteEntry(sel, entry, address, b);
    return entry ? entry : address;
}

uint16_t LocalHeaps::ReAlloc(uint16_t sel, uint16_t handle, uint16_t bytes, uint16_t flags) {
    Heap* h = Find(sel);
    uint16_t address = 0;
    Block* b = h ? BlockOf(*h, handle, address) : nullptr;
    if (!b) return 0;
    if (flags & lmem::Modify) {  // flags only
        b->flags = uint16_t((b->flags & ~lmem::Discardable) | (flags & lmem::Discardable));
        if (b->handle) WriteEntry(sel, b->handle, address, *b);
        return handle;
    }
    const uint16_t span = SpanOf(bytes);
    // In place: shrinking, or the bytes after the block are free.
    auto next = h->blocks.upper_bound(address);
    const uint32_t room = next == h->blocks.end() ? h->end - address : uint32_t(next->first) - address;
    if (span <= room) {
        uint8_t* data = mem_.SegmentData(sel);
        if (span > b->span) std::memset(data + address + b->span, 0, span - b->span);
        b->size = bytes;
        b->span = span;
        return handle;
    }
    // Moving: moveable blocks may always move; fixed ones only with LMEM_MOVEABLE.
    if (!b->handle && !(flags & lmem::Moveable)) return 0;
    const Block old = *b;
    h->blocks.erase(address);  // so its space counts as free
    const uint16_t moved = Reserve(sel, *h, span);
    if (!moved) {
        h->blocks[address] = old;
        return 0;
    }
    uint8_t* data = mem_.SegmentData(sel);  // Reserve may have moved the segment
    std::memmove(data + moved, data + address, old.span);
    std::memset(data + moved + old.span, 0, span - old.span);
    Block nb = old;
    nb.size = bytes;
    nb.span = span;
    h->blocks[moved] = nb;
    if (nb.handle) {
        WriteEntry(sel, nb.handle, moved, nb);
        return handle;
    }
    return moved;  // a fixed block's handle is its new address
}

uint16_t LocalHeaps::Free(uint16_t sel, uint16_t handle) {
    Heap* h = Find(sel);
    uint16_t address = 0;
    Block* b = h ? BlockOf(*h, handle, address) : nullptr;
    if (!b) return handle;
    const uint16_t entry = b->handle;
    h->blocks.erase(address);
    if (entry) h->blocks.erase(entry);
    return 0;
}

uint16_t LocalHeaps::Lock(uint16_t sel, uint16_t handle) {
    Heap* h = Find(sel);
    uint16_t address = 0;
    Block* b = h ? BlockOf(*h, handle, address) : nullptr;
    if (!b) return 0;
    if (b->handle && b->locks < 0xFF) {
        ++b->locks;
        WriteEntry(sel, b->handle, address, *b);
    }
    return address;
}

bool LocalHeaps::Unlock(uint16_t sel, uint16_t handle) {
    Heap* h = Find(sel);
    uint16_t address = 0;
    Block* b = h ? BlockOf(*h, handle, address) : nullptr;
    if (!b || !b->handle || b->locks == 0) return false;
    --b->locks;
    WriteEntry(sel, b->handle, address, *b);
    return b->locks > 0;
}

uint16_t LocalHeaps::Size(uint16_t sel, uint16_t handle) const {
    const Heap* h = Find(sel);
    uint16_t address = 0;
    const Block* b = h ? BlockOf(*h, handle, address) : nullptr;
    return b ? b->size : 0;
}

uint16_t LocalHeaps::Flags(uint16_t sel, uint16_t handle) const {
    const Heap* h = Find(sel);
    uint16_t address = 0;
    const Block* b = h ? BlockOf(*h, handle, address) : nullptr;
    return b ? uint16_t((b->flags & lmem::Discardable) | b->locks) : 0;
}

uint16_t LocalHeaps::HandleFor(uint16_t sel, uint16_t pointer) const {
    const Heap* h = Find(sel);
    if (!h) return 0;
    const auto it = h->blocks.find(pointer);
    if (it == h->blocks.end() || it->second.entry) return 0;
    return it->second.handle ? it->second.handle : pointer;
}

uint16_t LocalHeaps::Compact(uint16_t sel) const {
    const Heap* h = Find(sel);
    if (!h) return 0;
    uint32_t largest = 0, at = h->start;
    for (const auto& [address, b] : h->blocks) {
        largest = std::max<uint32_t>(largest, address - at);
        at = uint32_t(address) + b.span;
    }
    largest = std::max<uint32_t>(largest, h->end - at);
    return uint16_t(std::min<uint32_t>(largest, 0xFFFF));
}

size_t LocalHeaps::Count(uint16_t sel) const {
    const Heap* h = Find(sel);
    if (!h) return 0;
    return size_t(std::count_if(h->blocks.begin(), h->blocks.end(), [](const auto& kv) { return !kv.second.entry; }));
}

}  // namespace retro::win16
