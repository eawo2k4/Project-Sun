#include "win16/Resources.h"

#include <cctype>
#include <cstring>

namespace retro::win16 {

ResourceId ResourceId::FromFarPtr(const Memory& mem, uint16_t sel, uint16_t off) {
    ResourceId r;
    if (sel == 0) {  // MAKEINTRESOURCE
        r.id = off;
        return r;
    }
    r.name = mem.ReadString(sel, off, 255);
    if (r.name.size() > 1 && r.name[0] == '#') {  // "#123" is id 123
        uint32_t v = 0;
        bool digits = true;
        for (size_t i = 1; i < r.name.size() && digits; ++i) {
            digits = std::isdigit(static_cast<unsigned char>(r.name[i])) != 0;
            v = v * 10 + uint32_t(r.name[i] - '0');
        }
        if (digits && v > 0 && v < 0x8000) {
            r.id = uint16_t(v);
            r.name.clear();
            return r;
        }
    }
    for (char& c : r.name) c = char(std::toupper(static_cast<unsigned char>(c)));
    return r;
}

uint16_t Resources::Find(const ResourceId& type, const ResourceId& name) const {
    for (const NeResource& r : image_.resources) {
        if (type.Matches(r.typeId, r.typeName) && name.Matches(r.id, r.name)) return r.tableOffset;
    }
    return 0;
}

const NeResource* Resources::Get(uint16_t hrsrc) const {
    if (!hrsrc) return nullptr;
    for (const NeResource& r : image_.resources) {
        if (r.tableOffset == hrsrc) return &r;
    }
    return nullptr;
}

uint16_t Resources::Load(uint16_t hrsrc) {
    const NeResource* r = Get(hrsrc);
    if (!r) return 0;
    if (const auto it = loaded_.find(hrsrc); it != loaded_.end()) {
        ++it->second.usage;
        return it->second.handle;
    }
    const uint16_t handle = heap_.Alloc(gmem::Moveable, uint32_t(r->data.size()));
    if (!handle) return 0;
    if (!r->data.empty()) {
        const uint16_t sel = uint16_t(heap_.Lock(handle) >> 16);
        std::memcpy(mem_.SegmentData(sel), r->data.data(), r->data.size());
        heap_.Unlock(handle);
    }
    loaded_[hrsrc] = Loaded{handle, 1};
    return handle;
}

uint16_t Resources::Free(uint16_t hglobal) {
    for (auto it = loaded_.begin(); it != loaded_.end(); ++it) {
        if ((it->second.handle | 1) != (hglobal | 1)) continue;
        if (--it->second.usage == 0) {
            heap_.Free(it->second.handle);
            loaded_.erase(it);
        }
        return 0;
    }
    return hglobal;
}

bool Resources::String(uint16_t id, std::string& out) const {
    const NeResource* table = Lookup(ResourceId{res::String, {}}, ResourceId{uint16_t(id / 16 + 1), {}});
    if (!table) return false;
    // 16 strings, each a length byte and that many characters.
    size_t at = 0;
    for (uint16_t i = 0; i < id % 16; ++i) {
        if (at >= table->data.size()) return false;
        at += 1 + table->data[at];
    }
    if (at >= table->data.size()) return false;
    const size_t length = table->data[at];
    if (length == 0 || at + 1 + length > table->data.size()) return false;
    out.assign(reinterpret_cast<const char*>(&table->data[at + 1]), length);
    return true;
}

}  // namespace retro::win16
