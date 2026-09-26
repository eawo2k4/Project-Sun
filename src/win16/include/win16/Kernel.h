#pragma once

// KERNEL's global heap: GlobalAlloc and friends on top of the virtual LDT.
//
// Every block is its own segment (selector). As in protected-mode Windows
// 3.x, a fixed block's handle is its selector, and a moveable block's handle
// is the selector with the low bit cleared (0117h -> 0116h). Either form is
// accepted everywhere. Blocks never move: "moveable" only changes the handle.

#include <cstdint>
#include <map>

#include "win16/Memory.h"

namespace retro::win16 {

namespace gmem {
constexpr uint16_t Fixed = 0x0000;
constexpr uint16_t Moveable = 0x0002;
constexpr uint16_t ZeroInit = 0x0040;
}  // namespace gmem

class GlobalHeap {
public:
    explicit GlobalHeap(Memory& memory) : mem_(memory) {}

    // Returns a handle, or 0 (out of memory, or over 64 KB: huge blocks aren't
    // supported yet).
    uint16_t Alloc(uint16_t flags, uint32_t bytes);
    // 0 on success, the handle itself on failure (Windows' convention).
    uint16_t Free(uint16_t handle);
    // Far pointer (selector:0000) as selector << 16, or 0 for a bad handle.
    uint32_t Lock(uint16_t handle);
    // True while still locked.
    bool Unlock(uint16_t handle);
    uint32_t Size(uint16_t handle) const;

    size_t Count() const { return blocks_.size(); }
    uint16_t LockCount(uint16_t handle) const;

private:
    struct Block {
        uint32_t size;
        uint16_t flags;
        uint16_t locks;
    };
    static uint16_t SelectorOf(uint16_t handle) { return uint16_t(handle | 1); }

    Memory& mem_;
    std::map<uint16_t, Block> blocks_;  // by selector
};

}  // namespace retro::win16
