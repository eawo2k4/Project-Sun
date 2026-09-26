#include "retro/ExeFormat.h"

#include <windows.h>

#include <algorithm>
#include <cstddef>
#include <format>

namespace retro {
namespace {

constexpr uint16_t kSigMZ = 0x5A4D;      // "MZ"
constexpr uint16_t kSigZM = 0x4D5A;      // "ZM" (some very early DOS linkers)
constexpr uint16_t kSigNE = 0x454E;      // "NE"
constexpr uint16_t kSigLE = 0x454C;      // "LE"
constexpr uint16_t kSigLX = 0x584C;      // "LX"
constexpr uint32_t kSigPE = 0x00004550;  // "PE\0\0"

constexpr uint8_t kNeTargetWindows = 2;

static_assert(sizeof(IMAGE_DOS_HEADER) == 0x40);
static_assert(sizeof(IMAGE_OS2_HEADER) == 0x40);  // the Windows NE header

// Bounds-checked view over the caller's reader.
class Reader {
public:
    Reader(const ReadAtFn& fn, uint64_t size) : fn_(fn), size_(size) {}

    bool ReadBytes(uint64_t offset, void* dst, size_t n) const {
        if (offset > size_ || n > size_ - offset) return false;
        return fn_(offset, dst, n);
    }

    template <class T>
    bool Read(uint64_t offset, T& value) const {
        return ReadBytes(offset, &value, sizeof(T));
    }

private:
    const ReadAtFn& fn_;
    uint64_t size_;
};

bool ParseNe(const Reader& rd, uint64_t offset, ExeInfo& out, std::string& error) {
    IMAGE_OS2_HEADER ne{};
    if (!rd.Read(offset, ne)) {
        error = "Truncated NE header";
        return false;
    }
    out.format = ExeFormat::Win16NE;
    out.ne.linkerMajor = static_cast<uint8_t>(ne.ne_ver);
    out.ne.linkerMinor = static_cast<uint8_t>(ne.ne_rev);
    out.ne.flags = ne.ne_flags;
    out.ne.targetOS = ne.ne_exetyp;
    out.ne.otherFlags = ne.ne_flagsothers;
    out.ne.expectedWinVer = ne.ne_expver;
    out.ne.segmentCount = ne.ne_cseg;
    out.ne.moduleRefCount = ne.ne_cmod;
    out.ne.entryCsIp = static_cast<uint32_t>(ne.ne_csip);
    return true;
}

// Fills PeDetails from a 32- or 64-bit optional header. SizeOfOptionalHeader
// may legitimately be smaller than the struct (fewer data directories), so we
// only read what the file declares and zero the rest.
template <class OptionalHeader>
bool ParseOptionalHeader(const Reader& rd, uint64_t offset, uint16_t declaredSize,
                         PeDetails& pe, std::string& error) {
    constexpr size_t kFixedPart = offsetof(OptionalHeader, DataDirectory);
    const size_t n = std::min<size_t>(declaredSize, sizeof(OptionalHeader));
    if (n < kFixedPart) {
        error = std::format("PE optional header too small ({} bytes)", declaredSize);
        return false;
    }

    OptionalHeader oh{};
    if (!rd.ReadBytes(offset, &oh, n)) {
        error = "Truncated PE optional header";
        return false;
    }

    pe.subsystem = oh.Subsystem;
    pe.subsystemMajor = oh.MajorSubsystemVersion;
    pe.subsystemMinor = oh.MinorSubsystemVersion;
    pe.osMajor = oh.MajorOperatingSystemVersion;
    pe.osMinor = oh.MinorOperatingSystemVersion;
    pe.dllCharacteristics = oh.DllCharacteristics;
    pe.entryPointRva = oh.AddressOfEntryPoint;
    pe.imageBase = oh.ImageBase;
    pe.sizeOfImage = oh.SizeOfImage;

    const size_t dirsInFile = (n - kFixedPart) / sizeof(IMAGE_DATA_DIRECTORY);
    const size_t dirCount = std::min<size_t>(oh.NumberOfRvaAndSizes, dirsInFile);
    pe.isManaged = dirCount > IMAGE_DIRECTORY_ENTRY_COM_DESCRIPTOR &&
                   oh.DataDirectory[IMAGE_DIRECTORY_ENTRY_COM_DESCRIPTOR].VirtualAddress != 0;
    return true;
}

bool ParsePe(const Reader& rd, uint64_t offset, ExeInfo& out, std::string& error) {
    IMAGE_FILE_HEADER fh{};
    if (!rd.Read(offset + sizeof(uint32_t), fh)) {
        error = "Truncated PE file header";
        return false;
    }
    out.pe.machine = fh.Machine;
    out.pe.characteristics = fh.Characteristics;

    const uint64_t optOffset = offset + sizeof(uint32_t) + sizeof(IMAGE_FILE_HEADER);
    uint16_t magic = 0;
    if (fh.SizeOfOptionalHeader < sizeof(magic) || !rd.Read(optOffset, magic)) {
        error = "PE image has no optional header";
        return false;
    }

    switch (magic) {
    case IMAGE_NT_OPTIONAL_HDR32_MAGIC:
        out.format = ExeFormat::PE32;
        return ParseOptionalHeader<IMAGE_OPTIONAL_HEADER32>(
            rd, optOffset, fh.SizeOfOptionalHeader, out.pe, error);
    case IMAGE_NT_OPTIONAL_HDR64_MAGIC:
        out.format = ExeFormat::PE32Plus;
        return ParseOptionalHeader<IMAGE_OPTIONAL_HEADER64>(
            rd, optOffset, fh.SizeOfOptionalHeader, out.pe, error);
    default:
        error = std::format("Unknown PE optional header magic 0x{:04X}", magic);
        return false;
    }
}

}  // namespace

bool PeDetails::IsDll() const {
    return (characteristics & IMAGE_FILE_DLL) != 0;
}

bool PeDetails::IsLargeAddressAware() const {
    return (characteristics & IMAGE_FILE_LARGE_ADDRESS_AWARE) != 0;
}

bool InspectImage(const ReadAtFn& readAt, uint64_t fileSize, ExeInfo& out, std::string& error) {
    out = ExeInfo{};
    out.fileSize = fileSize;
    error.clear();

    const Reader rd(readAt, fileSize);

    uint16_t sig = 0;
    if (!rd.Read(0, sig) || (sig != kSigMZ && sig != kSigZM)) {
        error = "No MZ signature: not a DOS/Windows executable";
        return false;
    }
    out.format = ExeFormat::DosMZ;

    // Tiny DOS programs can be shorter than a full 64-byte header.
    IMAGE_DOS_HEADER dos{};
    if (!rd.Read(0, dos)) return true;

    // e_lfanew only means something for "new" executables; plain DOS programs
    // often have code or junk at 0x3C. Treat anything that doesn't point past
    // the DOS header at a known signature as plain DOS.
    if (dos.e_lfanew < static_cast<LONG>(sizeof(IMAGE_DOS_HEADER))) return true;
    const uint64_t hdr = static_cast<uint32_t>(dos.e_lfanew);

    uint16_t newSig = 0;
    if (!rd.Read(hdr, newSig)) return true;

    switch (newSig) {
    case kSigNE:
        out.newHeaderOffset = static_cast<uint32_t>(hdr);
        return ParseNe(rd, hdr, out, error);
    case kSigLE:
        out.newHeaderOffset = static_cast<uint32_t>(hdr);
        out.format = ExeFormat::LinearLE;
        return true;
    case kSigLX:
        out.newHeaderOffset = static_cast<uint32_t>(hdr);
        out.format = ExeFormat::LinearLX;
        return true;
    case static_cast<uint16_t>(kSigPE): {
        uint32_t fullSig = 0;
        if (rd.Read(hdr, fullSig) && fullSig == kSigPE) {
            out.newHeaderOffset = static_cast<uint32_t>(hdr);
            return ParsePe(rd, hdr, out, error);
        }
        return true;
    }
    default:
        return true;
    }
}

bool InspectFile(const std::wstring& path, ExeInfo& out, std::string& error) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        error = std::format("Cannot open file (Win32 error {})", GetLastError());
        return false;
    }
    struct Closer {
        HANDLE h;
        ~Closer() { CloseHandle(h); }
    } closer{file};

    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size)) {
        error = std::format("Cannot query file size (Win32 error {})", GetLastError());
        return false;
    }

    auto readAt = [file](uint64_t offset, void* dst, size_t n) {
        OVERLAPPED ov{};
        ov.Offset = static_cast<DWORD>(offset);
        ov.OffsetHigh = static_cast<DWORD>(offset >> 32);
        DWORD got = 0;
        return ReadFile(file, dst, static_cast<DWORD>(n), &got, &ov) && got == n;
    };
    return InspectImage(readAt, static_cast<uint64_t>(size.QuadPart), out, error);
}

LaunchPath ClassifyLaunchPath(const ExeInfo& info, std::string& reason) {
    reason.clear();
    switch (info.format) {
    case ExeFormat::PE32: {
        const PeDetails& pe = info.pe;
        if (pe.machine != IMAGE_FILE_MACHINE_I386) {
            reason = std::format("32-bit PE for a non-x86 CPU ({})", MachineName(pe.machine));
            return LaunchPath::Unsupported;
        }
        if (pe.IsDll()) {
            reason = "Image is a DLL, not a program";
            return LaunchPath::Unsupported;
        }
        if (pe.isManaged) {
            reason = ".NET assembly (may run as 64-bit; x86 shim cannot be injected reliably)";
            return LaunchPath::Unsupported;
        }
        if (pe.subsystem != IMAGE_SUBSYSTEM_WINDOWS_GUI &&
            pe.subsystem != IMAGE_SUBSYSTEM_WINDOWS_CUI) {
            reason = std::format("Unsupported subsystem ({})", SubsystemName(pe.subsystem));
            return LaunchPath::Unsupported;
        }
        return LaunchPath::NativeWow64;
    }
    case ExeFormat::PE32Plus:
        if (info.pe.IsDll()) {
            reason = "Image is a DLL, not a program";
            return LaunchPath::Unsupported;
        }
        reason = "64-bit program; runs natively without the x86 shim";
        return LaunchPath::Native64;

    case ExeFormat::Win16NE:
        if (info.ne.IsLibrary()) {
            reason = "16-bit library (DLL/DRV), not a program";
            return LaunchPath::Unsupported;
        }
        // Windows 2.x/3.0-era linkers often left ne_exetyp as 0 (unknown).
        if (info.ne.targetOS != kNeTargetWindows && info.ne.targetOS != 0) {
            reason = std::format("16-bit NE for {}, not Windows", NeTargetOSName(info.ne.targetOS));
            return LaunchPath::Unsupported;
        }
        reason = "16-bit Windows program: WOW64 has no NTVDM, needs the Win16 engine";
        return LaunchPath::Win16Engine;

    case ExeFormat::DosMZ:
        reason = "MS-DOS program (DOS support is out of scope)";
        return LaunchPath::Unsupported;
    case ExeFormat::LinearLE:
        reason = "LE module (VxD or DOS-extended program)";
        return LaunchPath::Unsupported;
    case ExeFormat::LinearLX:
        reason = "OS/2 LX module";
        return LaunchPath::Unsupported;
    case ExeFormat::Unknown:
    default:
        reason = "Not a recognised executable";
        return LaunchPath::Unsupported;
    }
}

std::string_view FormatName(ExeFormat format) {
    switch (format) {
    case ExeFormat::DosMZ:    return "DOS MZ";
    case ExeFormat::Win16NE:  return "16-bit NE";
    case ExeFormat::LinearLE: return "Linear LE";
    case ExeFormat::LinearLX: return "Linear LX";
    case ExeFormat::PE32:     return "PE32 (32-bit)";
    case ExeFormat::PE32Plus: return "PE32+ (64-bit)";
    default:                  return "Unknown";
    }
}

std::string_view LaunchPathName(LaunchPath path) {
    switch (path) {
    case LaunchPath::NativeWow64: return "native x86 under WOW64 + RetroShim";
    case LaunchPath::Native64:    return "native x64 (no shim)";
    case LaunchPath::Win16Engine: return "Win16 execution engine";
    default:                      return "unsupported";
    }
}

std::string_view MachineName(uint16_t machine) {
    switch (machine) {
    case IMAGE_FILE_MACHINE_I386:      return "x86";
    case IMAGE_FILE_MACHINE_AMD64:     return "x64";
    case IMAGE_FILE_MACHINE_ARM64:     return "ARM64";
    case IMAGE_FILE_MACHINE_ARMNT:     return "ARM Thumb-2";
    case IMAGE_FILE_MACHINE_IA64:      return "Itanium";
    case IMAGE_FILE_MACHINE_ALPHA:     return "Alpha AXP";
    case IMAGE_FILE_MACHINE_R4000:     return "MIPS R4000";
    case IMAGE_FILE_MACHINE_POWERPC:   return "PowerPC";
    default:                           return "other";
    }
}

std::string_view SubsystemName(uint16_t subsystem) {
    switch (subsystem) {
    case IMAGE_SUBSYSTEM_NATIVE:         return "native";
    case IMAGE_SUBSYSTEM_WINDOWS_GUI:    return "Windows GUI";
    case IMAGE_SUBSYSTEM_WINDOWS_CUI:    return "Windows console";
    case IMAGE_SUBSYSTEM_OS2_CUI:        return "OS/2 console";
    case IMAGE_SUBSYSTEM_POSIX_CUI:      return "POSIX console";
    case IMAGE_SUBSYSTEM_WINDOWS_CE_GUI: return "Windows CE GUI";
    case IMAGE_SUBSYSTEM_EFI_APPLICATION: return "EFI application";
    default:                             return "other";
    }
}

std::string_view NeTargetOSName(uint8_t targetOS) {
    switch (targetOS) {
    case 0:  return "unspecified";
    case 1:  return "OS/2";
    case 2:  return "Windows";
    case 3:  return "European MS-DOS 4.x";
    case 4:  return "Windows 386";
    case 5:  return "BOSS";
    default: return "other";
    }
}

std::string Describe(const ExeInfo& info) {
    std::string s = std::format("Format      : {}\nFile size   : {} bytes\n",
                                FormatName(info.format), info.fileSize);
    if (info.newHeaderOffset != 0)
        s += std::format("Header at   : 0x{:X}\n", info.newHeaderOffset);

    if (info.format == ExeFormat::Win16NE) {
        const NeDetails& ne = info.ne;
        s += std::format("Target OS   : {}\n", NeTargetOSName(ne.targetOS));
        if (ne.expectedWinVer != 0)
            s += std::format("Expects Win : {}.{:02}\n", ne.ExpectedWinMajor(), ne.ExpectedWinMinor());
        s += std::format("Linker      : {}.{}\n", ne.linkerMajor, ne.linkerMinor);
        s += std::format("Module type : {}\n", ne.IsLibrary() ? "library" : "application");
        s += std::format("Segments    : {}  Imports: {}\n", ne.segmentCount, ne.moduleRefCount);
        s += std::format("Entry CS:IP : {:04X}:{:04X}\n", ne.entryCsIp >> 16, ne.entryCsIp & 0xFFFF);
    } else if (info.format == ExeFormat::PE32 || info.format == ExeFormat::PE32Plus) {
        const PeDetails& pe = info.pe;
        s += std::format("Machine     : {} (0x{:04X})\n", MachineName(pe.machine), pe.machine);
        s += std::format("Subsystem   : {} {}.{:02}\n", SubsystemName(pe.subsystem),
                         pe.subsystemMajor, pe.subsystemMinor);
        s += std::format("OS version  : {}.{}\n", pe.osMajor, pe.osMinor);
        s += std::format("Image base  : 0x{:X}  size 0x{:X}\n", pe.imageBase, pe.sizeOfImage);
        s += std::format("Entry RVA   : 0x{:X}\n", pe.entryPointRva);
        s += std::format("Flags       : {}{}{}{}\n",
                         pe.IsDll() ? "DLL " : "EXE ",
                         pe.IsLargeAddressAware() ? "LARGE_ADDRESS_AWARE " : "",
                         (pe.dllCharacteristics & IMAGE_DLLCHARACTERISTICS_DYNAMIC_BASE) ? "ASLR " : "",
                         pe.isManaged ? ".NET" : "");
    }
    return s;
}

}  // namespace retro
