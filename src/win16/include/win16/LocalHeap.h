#pragma once

// KERNEL's local heaps: LocalAlloc and friends, inside a data segment
// (normally DGROUP; the API works on the heap of the caller's DS).
//
// Block bookkeeping lives here, not in the segment, but handles follow
// Windows 3.x: a fixed block's handle is its near pointer, and a moveable
// block's handle is the offset of a 4-byte handle-table entry in the heap
// (WORD address, BYTE flags, BYTE lock count), so code that dereferences a
// handle to get the pointer works. Blocks never move once allocated except
// through LocalReAlloc. When the heap is full it grows the segment, up to
// 64 KB, if the heap ends at the end of the segment (as DGROUP's does).

#include <cstdint>
#include <map>

#include "win16/Memory.h"

namespace retro::win16 {

namespace lmem {
constexpr uint16_t Fixed = 0x0000, Moveable = 0x0002, NoCompact = 0x0010, ZeroInit = 0x0040,
                   Modify = 0x0080, Discardable = 0x0F00;
}  // namespace lmem

class LocalHeaps {
public:
    explicit LocalHeaps(Memory& memory) : mem_(memory) {}

    // LocalInit: a heap over [start, end) of the segment. start 0: the last
    // `end` bytes of the segment.
    bool Init(uint16_t sel, uint16_t start, uint16_t end);
    bool Has(uint16_t sel) const { return heaps_.count(sel) != 0; }

    uint16_t Alloc(uint16_t sel, uint16_t flags, uint16_t bytes);  // handle, 0 on failure
    uint16_t ReAlloc(uint16_t sel, uint16_t handle, uint16_t bytes, uint16_t flags);
    uint16_t Free(uint16_t sel, uint16_t handle);                   // 0 on success
    uint16_t Lock(uint16_t sel, uint16_t handle);                   // near pointer, 0 if bad
    bool Unlock(uint16_t sel, uint16_t handle);                     // true while still locked
    uint16_t Size(uint16_t sel, uint16_t handle) const;
    uint16_t Flags(uint16_t sel, uint16_t handle) const;            // lock count | LMEM_DISCARDABLE bits
    uint16_t HandleFor(uint16_t sel, uint16_t pointer) const;      // LocalHandle
    uint16_t Compact(uint16_t sel) const;                           // largest free block
    size_t Count(uint16_t sel) const;                               // live blocks

private:
    struct Block {
        uint16_t size = 0;    // as requested
        uint16_t span = 0;    // reserved (4-byte granular)
        uint16_t handle = 0;  // moveable: handle-table entry; fixed: 0
        uint16_t flags = 0;
        uint8_t locks = 0;
        bool entry = false;   // this block is a handle-table entry
    };
    struct Heap {
        uint16_t start = 0;
        uint32_t end = 0;                 // exclusive (may be 0x10000)
        std::map<uint16_t, Block> blocks;  // by address
    };

    Heap* Find(uint16_t sel);
    const Heap* Find(uint16_t sel) const;
    // Address of a free range of `span` bytes, growing the segment if needed; 0 if none.
    uint16_t Reserve(uint16_t sel, Heap& h, uint16_t span);
    // The data block of a handle (moveable or fixed); nullptr if not a live handle.
    Block* BlockOf(Heap& h, uint16_t handle, uint16_t& address);
    const Block* BlockOf(const Heap& h, uint16_t handle, uint16_t& address) const;
    void WriteEntry(uint16_t sel, uint16_t entry, uint16_t address, const Block& b);

    Memory& mem_;
    std::map<uint16_t, Heap> heaps_;
};

}  // namespace retro::win16
