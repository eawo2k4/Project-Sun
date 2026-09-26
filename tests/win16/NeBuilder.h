#pragma once

// Builds real NE executables in memory for tests: MZ stub, NE header, segment
// table, resource table, resident names, module reference and imported names
// tables, entry table, segment data with relocation records, and resource
// data, laid out on 16-byte sectors like a linker and resource compiler would.

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace win16test {

// Relocation record fields as stored in the file.
struct NeReloc {
    uint8_t source;  // 0 LOBYTE, 2 SEGMENT, 3 FAR_ADDR, 5 OFFSET
    uint8_t flags;   // 0 internal, 1 import ordinal, 2 import name; | 4 additive
    uint16_t offset;
    uint16_t a;      // internal: segment number | import: module index
    uint16_t b;      // internal: offset          | import: ordinal / name offset
};

struct NeSeg {
    std::vector<uint8_t> bytes;
    bool data = false;
    uint32_t minAlloc = 0;  // 0 = size of bytes
    std::vector<NeReloc> relocs;
};

// A resource: integer type/name ids (non-zero) or strings.
struct NeRes {
    uint16_t type = 0;
    std::string typeName;
    uint16_t id = 0;
    std::string name;
    std::vector<uint8_t> data;
};

struct NeProgram {
    std::string name = "TESTAPP";
    std::vector<NeSeg> segments;
    std::vector<std::string> modules;      // module reference table, in order
    std::vector<std::string> importNames;  // extra names for by-name imports
    std::vector<NeRes> resources;
    uint16_t entrySegment = 1;
    uint16_t entryIp = 0;
    uint16_t autoData = 0;
    uint16_t heap = 0x100;
    uint16_t stack = 0x400;
    uint16_t stackSegment = 0;
    uint16_t sp = 0;
    uint16_t flags = 0x0302;  // application, Windows API, multiple data

    // Offset of a name in the imported names table (byte 0 is an empty name).
    uint16_t ImportNameOffset(const std::string& wanted) const {
        uint16_t off = 1;
        for (const std::string& m : modules) {
            if (m == wanted) return off;
            off = uint16_t(off + 1 + m.size());
        }
        for (const std::string& n : importNames) {
            if (n == wanted) return off;
            off = uint16_t(off + 1 + n.size());
        }
        return 0;
    }
};

inline void Put16(std::vector<uint8_t>& v, size_t at, uint16_t x) {
    v[at] = uint8_t(x);
    v[at + 1] = uint8_t(x >> 8);
}
inline void Append16(std::vector<uint8_t>& v, uint16_t x) {
    v.push_back(uint8_t(x));
    v.push_back(uint8_t(x >> 8));
}
inline void AppendPString(std::vector<uint8_t>& v, const std::string& s) {
    v.push_back(uint8_t(s.size()));
    v.insert(v.end(), s.begin(), s.end());
}

inline std::vector<uint8_t> BuildNe(const NeProgram& p) {
    constexpr uint16_t kShift = 4;
    std::vector<uint8_t> f(0x40, 0);
    f[0] = 'M';
    f[1] = 'Z';
    Put16(f, 0x18, 0x40);  // e_lfarlc
    Put16(f, 0x3C, 0x40);  // e_lfanew
    const size_t ne = f.size();
    f.resize(ne + 0x40, 0);

    const size_t segTab = f.size();
    f.resize(segTab + p.segments.size() * 8, 0);

    // Resource table: shift, TYPEINFO { type, count, reserved, NAMEINFO[] },
    // 0, then the names. Data offsets are patched once the data is placed.
    const size_t rsrcTab = f.size();
    std::vector<size_t> nameInfo(p.resources.size());
    if (!p.resources.empty()) {
        std::vector<std::vector<size_t>> groups;  // resources by type, in order
        for (size_t i = 0; i < p.resources.size(); ++i) {
            bool placed = false;
            for (auto& g : groups) {
                const NeRes& first = p.resources[g[0]];
                if (first.type == p.resources[i].type && first.typeName == p.resources[i].typeName) {
                    g.push_back(i);
                    placed = true;
                    break;
                }
            }
            if (!placed) groups.push_back({i});
        }
        size_t tableSize = 2 + 2;
        for (const auto& g : groups) tableSize += 8 + 12 * g.size();
        std::vector<uint8_t> names;
        auto key = [&](uint16_t id, const std::string& name) {
            if (id) return uint16_t(0x8000 | id);
            const uint16_t at = uint16_t(tableSize + names.size());
            AppendPString(names, name);
            return at;
        };
        Append16(f, kShift);
        for (const auto& g : groups) {
            const NeRes& first = p.resources[g[0]];
            Append16(f, key(first.type, first.typeName));
            Append16(f, uint16_t(g.size()));
            Append16(f, 0);
            Append16(f, 0);
            for (size_t i : g) {
                const NeRes& r = p.resources[i];
                nameInfo[i] = f.size();
                Append16(f, 0);  // offset: patched below
                Append16(f, uint16_t((r.data.size() + (1u << kShift) - 1) >> kShift));
                Append16(f, 0x0030);  // moveable | pure
                Append16(f, key(r.id, r.name));
                Append16(f, 0);
                Append16(f, 0);
            }
        }
        Append16(f, 0);
        f.insert(f.end(), names.begin(), names.end());
        f.push_back(0);
    }

    const size_t resNames = f.size();
    AppendPString(f, p.name);
    Append16(f, 0);
    f.push_back(0);

    const size_t modTab = f.size();
    for (const std::string& m : p.modules) Append16(f, p.ImportNameOffset(m));

    const size_t impNames = f.size();
    f.push_back(0);
    for (const std::string& m : p.modules) AppendPString(f, m);
    for (const std::string& n : p.importNames) AppendPString(f, n);

    const size_t entTab = f.size();
    f.push_back(0);  // no exports

    // Segment data (+ relocations), each on a 16-byte sector.
    for (size_t i = 0; i < p.segments.size(); ++i) {
        const NeSeg& s = p.segments[i];
        while (f.size() % (1u << kShift)) f.push_back(0);
        const size_t at = f.size();
        f.insert(f.end(), s.bytes.begin(), s.bytes.end());
        if (!s.relocs.empty()) {
            Append16(f, uint16_t(s.relocs.size()));
            for (const NeReloc& r : s.relocs) {
                f.push_back(r.source);
                f.push_back(r.flags);
                Append16(f, r.offset);
                Append16(f, r.a);
                Append16(f, r.b);
            }
        }
        const size_t e = segTab + i * 8;
        Put16(f, e, s.bytes.empty() ? 0 : uint16_t(at >> kShift));
        Put16(f, e + 2, uint16_t(s.bytes.size()));
        Put16(f, e + 4, uint16_t((s.data ? 0x0001 : 0) | (s.relocs.empty() ? 0 : 0x0100)));
        const uint32_t minAlloc = s.minAlloc ? s.minAlloc : uint32_t(s.bytes.size());
        Put16(f, e + 6, uint16_t(minAlloc == 0x10000 ? 0 : minAlloc));
    }

    // Resource data, each on a sector and padded to a whole one.
    for (size_t i = 0; i < p.resources.size(); ++i) {
        while (f.size() % (1u << kShift)) f.push_back(0);
        Put16(f, nameInfo[i], uint16_t(f.size() >> kShift));
        f.insert(f.end(), p.resources[i].data.begin(), p.resources[i].data.end());
    }
    while (f.size() % (1u << kShift)) f.push_back(0);

    f[ne] = 'N';
    f[ne + 1] = 'E';
    f[ne + 2] = 5;
    f[ne + 3] = 10;
    Put16(f, ne + 0x04, uint16_t(entTab - ne));
    Put16(f, ne + 0x06, 1);
    Put16(f, ne + 0x0C, p.flags);
    Put16(f, ne + 0x0E, p.autoData);
    Put16(f, ne + 0x10, p.heap);
    Put16(f, ne + 0x12, p.stack);
    Put16(f, ne + 0x14, p.entryIp);
    Put16(f, ne + 0x16, p.entrySegment);
    Put16(f, ne + 0x18, p.sp);
    Put16(f, ne + 0x1A, p.stackSegment);
    Put16(f, ne + 0x1C, uint16_t(p.segments.size()));
    Put16(f, ne + 0x1E, uint16_t(p.modules.size()));
    Put16(f, ne + 0x22, uint16_t(segTab - ne));
    Put16(f, ne + 0x24, uint16_t(rsrcTab - ne));  // == resident names when empty
    Put16(f, ne + 0x26, uint16_t(resNames - ne));
    Put16(f, ne + 0x28, uint16_t(modTab - ne));
    Put16(f, ne + 0x2A, uint16_t(impNames - ne));
    Put16(f, ne + 0x32, kShift);
    Put16(f, ne + 0x34, uint16_t(p.resources.size()));
    f[ne + 0x36] = 2;  // Windows
    Put16(f, ne + 0x3E, 0x030A);
    return f;
}

}  // namespace win16test
