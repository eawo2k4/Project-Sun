#pragma once

// Executable header inspection: tells a DOS MZ program apart from a Windows
// 16-bit NE, a linear LE/LX module, or a 32/64-bit PE image.
//
// The parser itself (InspectImage) does no I/O: it pulls bytes through a
// caller-supplied reader, so it is fully unit-testable with in-memory images
// and never reads more of a (possibly huge) game executable than it needs.

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

namespace retro {

enum class ExeFormat {
    Unknown,   // No MZ signature: not a DOS/Windows executable
    DosMZ,     // Plain real-mode DOS program (no recognised extended header)
    Win16NE,   // "New Executable": Windows 1.x-3.x (or OS/2 1.x)
    LinearLE,  // Linear Executable: Win3.x/9x VxDs, DOS extenders (DOS/4GW)
    LinearLX,  // OS/2 2.x+ 32-bit linear executable
    PE32,      // Portable Executable, 32-bit (Win32s / 9x / NT / XP)
    PE32Plus,  // Portable Executable, 64-bit
};

// Selected fields of the Windows NE header (IMAGE_OS2_HEADER).
struct NeDetails {
    uint8_t  linkerMajor = 0;
    uint8_t  linkerMinor = 0;
    uint16_t flags = 0;           // ne_flags
    uint8_t  targetOS = 0;        // ne_exetyp: 0 unknown, 1 OS/2, 2 Windows, ...
    uint8_t  otherFlags = 0;      // ne_flagsothers
    uint16_t expectedWinVer = 0;  // ne_expver: 0x030A == Windows 3.10
    uint16_t segmentCount = 0;
    uint16_t moduleRefCount = 0;
    uint32_t entryCsIp = 0;       // segment:offset of the entry point

    bool IsLibrary() const { return (flags & 0x8000) != 0; }
    uint8_t ExpectedWinMajor() const { return static_cast<uint8_t>(expectedWinVer >> 8); }
    uint8_t ExpectedWinMinor() const { return static_cast<uint8_t>(expectedWinVer & 0xFF); }
};

// Selected fields of the PE file + optional headers.
struct PeDetails {
    uint16_t machine = 0;             // IMAGE_FILE_MACHINE_*
    uint16_t characteristics = 0;     // IMAGE_FILE_*
    uint16_t subsystem = 0;           // IMAGE_SUBSYSTEM_*
    uint16_t subsystemMajor = 0;
    uint16_t subsystemMinor = 0;
    uint16_t osMajor = 0;
    uint16_t osMinor = 0;
    uint16_t dllCharacteristics = 0;  // IMAGE_DLLCHARACTERISTICS_*
    uint32_t entryPointRva = 0;
    uint64_t imageBase = 0;
    uint32_t sizeOfImage = 0;
    bool     isManaged = false;       // has a CLR (.NET) header

    bool IsDll() const;
    bool IsLargeAddressAware() const;
};

struct ExeInfo {
    ExeFormat format = ExeFormat::Unknown;
    uint64_t  fileSize = 0;
    uint32_t  newHeaderOffset = 0;  // e_lfanew for NE/LE/LX/PE, else 0
    NeDetails ne{};                 // valid when format == Win16NE
    PeDetails pe{};                 // valid when format == PE32 / PE32Plus
};

// Reads exactly `size` bytes at `offset`; returns false on any short read.
using ReadAtFn = std::function<bool(uint64_t offset, void* dst, size_t size)>;

// Core parser. Returns false (with `error` set) when the data is not an MZ
// executable or an extended header is truncated/corrupt.
bool InspectImage(const ReadAtFn& readAt, uint64_t fileSize, ExeInfo& out, std::string& error);

// Convenience wrapper that reads from a file on disk.
bool InspectFile(const std::wstring& path, ExeInfo& out, std::string& error);

// How the runner intends to execute a given image.
enum class LaunchPath {
    NativeWow64,  // 32-bit x86 PE: run natively under WOW64 with RetroShim injected
    Native64,     // 64-bit PE: runs natively, no shim (RetroShim is x86-only)
    Win16Engine,  // 16-bit Windows NE: needs the embedded Win16 execution engine
    Unsupported,  // DOS, OS/2, VxD, non-x86 NT binaries, DLLs, ...
};

LaunchPath ClassifyLaunchPath(const ExeInfo& info, std::string& reason);

std::string_view FormatName(ExeFormat format);
std::string_view LaunchPathName(LaunchPath path);
std::string_view MachineName(uint16_t machine);
std::string_view SubsystemName(uint16_t subsystem);
std::string_view NeTargetOSName(uint8_t targetOS);

// Multi-line, human-readable summary of an inspected image.
std::string Describe(const ExeInfo& info);

}  // namespace retro
