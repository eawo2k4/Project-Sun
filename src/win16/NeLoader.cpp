#include "win16/NeLoader.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace retro::win16 {
namespace {

std::string Fmt(const char* format, unsigned a, unsigned b = 0, unsigned c = 0) {
    char buf[160];
    std::snprintf(buf, sizeof(buf), format, a, b, c);
    return buf;
}

uint16_t Get16(const uint8_t* p) { return uint16_t(p[0] | (p[1] << 8)); }
void Put16(uint8_t* p, uint16_t v) {
    p[0] = uint8_t(v);
    p[1] = uint8_t(v >> 8);
}

uint32_t SourceSize(RelocSource s) {
    switch (s) {
    case RelocSource::LoByte: return 1;
    case RelocSource::FarPointer: return 4;
    default: return 2;
    }
}

// Writes the target into one fixup site.
void Patch(uint8_t* site, RelocSource source, bool additive, uint16_t sel, uint16_t off) {
    switch (source) {
    case RelocSource::LoByte:
        site[0] = uint8_t(additive ? site[0] + off : off);
        break;
    case RelocSource::Selector:
        Put16(site, additive ? uint16_t(Get16(site) + sel) : sel);
        break;
    case RelocSource::FarPointer:
        Put16(site, additive ? uint16_t(Get16(site) + off) : off);
        Put16(site + 2, sel);
        break;
    default:  // Offset
        Put16(site, additive ? uint16_t(Get16(site) + off) : off);
        break;
    }
}

bool ApplyRelocations(const NeImage& image, uint16_t segIndex, Memory& memory,
                      ImportResolver& imports, const LoadedModule& module, std::string& error) {
    const NeSegment& seg = image.segments[segIndex];
    const uint16_t sel = module.selectors[segIndex];
    uint8_t* data = memory.SegmentData(sel);
    const uint32_t size = memory.SegmentSize(sel);

    for (const NeRelocation& r : seg.relocations) {
        uint16_t targetSel = 0, targetOff = 0;
        switch (r.target) {
        case RelocTarget::Internal:
            if (r.segment == 0xFF) {  // movable segment: through the entry table
                const NeEntry* e = image.FindEntry(r.targetOffset);
                if (!e || e->segment < 1 || e->segment > image.segments.size()) {
                    error = Fmt("relocation to missing entry ordinal %u", r.targetOffset);
                    return false;
                }
                targetSel = module.selectors[e->segment - 1];
                targetOff = e->offset;
            } else {
                if (r.segment < 1 || r.segment > image.segments.size()) {
                    error = Fmt("relocation to missing segment %u", r.segment);
                    return false;
                }
                targetSel = module.selectors[r.segment - 1];
                targetOff = r.targetOffset;
            }
            break;
        case RelocTarget::ImportOrdinal:
        case RelocTarget::ImportName: {
            if (r.module < 1 || r.module > image.moduleRefs.size()) {
                error = Fmt("relocation to missing module reference %u", r.module);
                return false;
            }
            const std::string& module_ = image.moduleRefs[r.module - 1];
            if (!imports.Resolve(module_, r.target == RelocTarget::ImportOrdinal ? r.ordinal : 0,
                                 r.name, targetSel, targetOff, error)) {
                return false;
            }
            break;
        }
        case RelocTarget::OsFixup:
            continue;  // floating-point emulator hooks: nothing to do without WIN87EM
        }

        const uint32_t width = SourceSize(r.source);
        if (r.additive) {
            if (uint32_t(r.offset) + width > size) {
                error = Fmt("fixup at %04X outside segment %u", r.offset, segIndex + 1u);
                return false;
            }
            Patch(data + r.offset, r.source, true, targetSel, targetOff);
            continue;
        }
        // Chained: each site holds the offset of the next one (FFFF ends it).
        uint16_t off = r.offset;
        for (int guard = 0;; ++guard) {
            if (uint32_t(off) + std::max<uint32_t>(width, 2) > size || guard > 0x10000) {
                error = Fmt("fixup chain at %04X outside segment %u", off, segIndex + 1u);
                return false;
            }
            const uint16_t next = Get16(data + off);
            Patch(data + off, r.source, false, targetSel, targetOff);
            if (next == 0xFFFF) break;
            off = next;
        }
    }
    return true;
}

}  // namespace

bool LoadNe(const NeImage& image, const std::vector<uint8_t>& file, Memory& memory,
            ImportResolver& imports, const std::string& commandLine, LoadedModule& out,
            std::string& error) {
    out = LoadedModule{};

    for (uint16_t i = 0; i < image.segments.size(); ++i) {
        const NeSegment& seg = image.segments[i];
        uint32_t size = seg.minAlloc;
        if (i + 1u == image.autoDataSegment) {
            // DGROUP grows by the local heap and the stack.
            size = std::min<uint32_t>(0x10000, size + image.heapSize + image.stackSize);
        }
        const uint16_t sel = memory.Allocate(size, seg.IsData() ? SegmentKind::Data : SegmentKind::Code);
        if (!sel) {
            error = Fmt("out of memory allocating segment %u (%u bytes)", i + 1u, size);
            return false;
        }
        if (!seg.expanded.empty()) {
            std::memcpy(memory.SegmentData(sel), seg.expanded.data(), seg.expanded.size());
        } else if (seg.fileLength) {
            std::memcpy(memory.SegmentData(sel), &file[seg.fileOffset], seg.fileLength);
        }
        out.selectors.push_back(sel);
    }
    if (image.autoDataSegment) out.dgroup = out.selectors[image.autoDataSegment - 1];

    for (uint16_t i = 0; i < image.segments.size(); ++i) {
        if (!ApplyRelocations(image, i, memory, imports, out, error)) return false;
    }
    if (image.IsLibrary()) {
        if (out.dgroup) {  // the local heap is at the end of a DLL's DGROUP
            const uint32_t size = memory.SegmentSize(out.dgroup);
            out.heapStart = uint16_t(size - std::min<uint32_t>(size, image.heapSize));
        }
        return true;
    }

    // PSP: INT 20h at 0, command tail at 80h.
    out.psp = memory.Allocate(0x100, SegmentKind::Data);
    if (!out.psp) {
        error = "out of memory allocating the PSP";
        return false;
    }
    uint8_t* psp = memory.SegmentData(out.psp);
    psp[0] = 0xCD;
    psp[1] = 0x20;
    const std::string tail = commandLine.empty() ? "" : " " + commandLine.substr(0, 125);
    psp[0x80] = uint8_t(tail.size());
    std::memcpy(psp + 0x81, tail.data(), tail.size());
    psp[0x81 + tail.size()] = 0x0D;

    Registers& r = out.initial;
    r.s[CS] = out.selectors[image.entrySegment - 1];
    r.ip = image.entryIp;
    const uint16_t stackSeg = image.stackSegment ? image.stackSegment : image.autoDataSegment;
    if (!stackSeg || stackSeg > image.segments.size()) {
        error = "no stack segment";
        return false;
    }
    r.s[SS] = out.selectors[stackSeg - 1];
    const uint32_t ssSize = memory.SegmentSize(r.s[SS]);
    if (image.initialSp) {
        r.r[SP] = image.initialSp;
    } else if (stackSeg == image.autoDataSegment) {
        // The stack follows the static data; the local heap follows the stack.
        const uint32_t data = image.segments[stackSeg - 1].minAlloc;
        r.r[SP] = uint16_t(std::min<uint32_t>(data + image.stackSize, std::min<uint32_t>(ssSize, 0xFFFE)) & ~1u);
    } else {
        r.r[SP] = uint16_t(std::min<uint32_t>(ssSize, 0xFFFE) & ~1u);
    }
    if (out.dgroup) {
        const uint32_t dgSize = memory.SegmentSize(out.dgroup);
        uint32_t heapStart = dgSize - std::min<uint32_t>(dgSize, image.heapSize);
        if (out.dgroup == r.s[SS] && !image.initialSp) heapStart = r.r[SP];
        out.heapStart = uint16_t(std::min<uint32_t>(heapStart, 0xFFFF));
    }
    r.s[DS] = out.dgroup;
    r.s[ES] = out.psp;
    r.r[AX] = 0;
    r.r[BX] = image.stackSize;
    r.r[CX] = image.heapSize;
    r.r[SI] = 0;
    r.r[DI] = out.dgroup;
    r.r[BP] = 0;
    return true;
}

}  // namespace retro::win16
