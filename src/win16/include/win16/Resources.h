#pragma once

// The task's resources: FindResource / LoadResource / LockResource /
// FreeResource / SizeofResource over the NE resource table, and direct
// lookups for LoadBitmap and LoadString.
//
// As in Windows 3.x, an HRSRC is the offset of the resource's NAMEINFO record
// in the resource table, and loading a resource copies it into a moveable
// global block. Loading it again returns the same block with a usage count;
// FreeResource discards the block when the count drops to zero.

#include <cstdint>
#include <map>
#include <string>

#include "win16/Kernel.h"
#include "win16/NeImage.h"

namespace retro::win16 {

// A resource type or name as passed to the API: an integer id
// (MAKEINTRESOURCE: selector 0) or a string ("LOGO", or "#12" for id 12).
struct ResourceId {
    uint16_t id = 0;   // != 0: integer id
    std::string name;  // upper case

    static ResourceId FromFarPtr(const Memory& mem, uint16_t sel, uint16_t off);
    bool Matches(uint16_t resId, const std::string& resName) const {
        return id ? resId == id : resId == 0 && resName == name;
    }
    std::string Describe() const { return id ? "#" + std::to_string(id) : "\"" + name + "\""; }
};

class Resources {
public:
    Resources(const NeImage& image, GlobalHeap& heap, Memory& memory)
        : image_(image), heap_(heap), mem_(memory) {}

    // HRSRC, or 0 if there is no such resource.
    uint16_t Find(const ResourceId& type, const ResourceId& name) const;
    const NeResource* Get(uint16_t hrsrc) const;
    const NeResource* Lookup(const ResourceId& type, const ResourceId& name) const {
        return Get(Find(type, name));
    }

    // HGLOBAL with the resource's bytes, or 0.
    uint16_t Load(uint16_t hrsrc);
    // 0 on success, the handle itself on failure (Windows' convention).
    uint16_t Free(uint16_t hglobal);
    size_t LoadedCount() const { return loaded_.size(); }

    // String `id` of the string tables (RT_STRING block id / 16 + 1, entry
    // id % 16). False if there is no such string.
    bool String(uint16_t id, std::string& out) const;

private:
    struct Loaded {
        uint16_t handle = 0;
        uint16_t usage = 0;
    };

    const NeImage& image_;
    GlobalHeap& heap_;
    Memory& mem_;
    std::map<uint16_t, Loaded> loaded_;  // by HRSRC
};

}  // namespace retro::win16
