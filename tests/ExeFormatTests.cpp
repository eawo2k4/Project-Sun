// Unit tests for the executable header parser, using synthetic in-memory
// images plus a couple of real system binaries.

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <vector>

#include "Check.h"
#include "retro/ExeFormat.h"

using namespace retro;

namespace {

using Image = std::vector<uint8_t>;

constexpr uint32_t kNewHeader = 0x80;

template <class T>
void Put(Image& img, size_t offset, const T& value) {
    if (img.size() < offset + sizeof(T)) img.resize(offset + sizeof(T));
    std::memcpy(img.data() + offset, &value, sizeof(T));
}

Image MakeDos(LONG lfanew = 0) {
    IMAGE_DOS_HEADER dos{};
    dos.e_magic = IMAGE_DOS_SIGNATURE;
    dos.e_lfarlc = 0x40;
    dos.e_lfanew = lfanew;
    Image img;
    Put(img, 0, dos);
    return img;
}

Image MakeNe(uint8_t targetOS, uint16_t flags, uint16_t expver) {
    Image img = MakeDos(kNewHeader);
    IMAGE_OS2_HEADER ne{};
    ne.ne_magic = IMAGE_OS2_SIGNATURE;
    ne.ne_ver = 5;
    ne.ne_rev = 10;
    ne.ne_flags = flags;
    ne.ne_exetyp = targetOS;
    ne.ne_expver = expver;
    ne.ne_cseg = 7;
    ne.ne_csip = 0x00010000;
    Put(img, kNewHeader, ne);
    return img;
}

template <class NtHeaders>
Image MakePe(WORD machine, WORD magic, WORD subsystem, WORD characteristics) {
    Image img = MakeDos(kNewHeader);
    NtHeaders nt{};
    nt.Signature = IMAGE_NT_SIGNATURE;
    nt.FileHeader.Machine = machine;
    nt.FileHeader.Characteristics = characteristics;
    nt.FileHeader.SizeOfOptionalHeader = sizeof(nt.OptionalHeader);
    nt.OptionalHeader.Magic = magic;
    nt.OptionalHeader.Subsystem = subsystem;
    nt.OptionalHeader.MajorSubsystemVersion = 4;
    nt.OptionalHeader.AddressOfEntryPoint = 0x1234;
    nt.OptionalHeader.ImageBase = 0x400000;
    nt.OptionalHeader.NumberOfRvaAndSizes = IMAGE_NUMBEROF_DIRECTORY_ENTRIES;
    Put(img, kNewHeader, nt);
    return img;
}

Image MakePe32(WORD subsystem = IMAGE_SUBSYSTEM_WINDOWS_GUI,
               WORD characteristics = IMAGE_FILE_EXECUTABLE_IMAGE,
               WORD machine = IMAGE_FILE_MACHINE_I386) {
    return MakePe<IMAGE_NT_HEADERS32>(machine, IMAGE_NT_OPTIONAL_HDR32_MAGIC, subsystem,
                                      characteristics);
}

bool Inspect(const Image& img, ExeInfo& info, std::string& error) {
    auto readAt = [&img](uint64_t offset, void* dst, size_t n) {
        std::memcpy(dst, img.data() + offset, n);  // bounds already checked by the parser
        return true;
    };
    return InspectImage(readAt, img.size(), info, error);
}

LaunchPath Classify(const ExeInfo& info) {
    std::string reason;
    return ClassifyLaunchPath(info, reason);
}

void TestNotExecutable() {
    Image img = {'H', 'e', 'l', 'l', 'o'};
    ExeInfo info;
    std::string error;
    CHECK(!Inspect(img, info, error));
    CHECK(info.format == ExeFormat::Unknown);
    CHECK(!error.empty());
    CHECK(!Inspect(Image{}, info, error));
}

void TestPlainDos() {
    ExeInfo info;
    std::string error;
    CHECK(Inspect(MakeDos(), info, error));
    CHECK(info.format == ExeFormat::DosMZ);
    CHECK(Classify(info) == LaunchPath::Unsupported);

    // Tiny DOS program shorter than a full header.
    Image tiny = {'M', 'Z', 0x10, 0x00, 0x01, 0x00};
    CHECK(Inspect(tiny, info, error));
    CHECK(info.format == ExeFormat::DosMZ);

    // Garbage e_lfanew pointing past EOF must not be treated as an error.
    CHECK(Inspect(MakeDos(0x7FFFFFF0), info, error));
    CHECK(info.format == ExeFormat::DosMZ);

    // Negative e_lfanew.
    CHECK(Inspect(MakeDos(-4), info, error));
    CHECK(info.format == ExeFormat::DosMZ);

    // e_lfanew pointing at bytes that aren't a known signature.
    Image junk = MakeDos(kNewHeader);
    Put(junk, kNewHeader, uint32_t{0xDEADBEEF});
    CHECK(Inspect(junk, info, error));
    CHECK(info.format == ExeFormat::DosMZ);
}

void TestWin16() {
    ExeInfo info;
    std::string error;
    CHECK(Inspect(MakeNe(2, 0x0302, 0x030A), info, error));
    CHECK(info.format == ExeFormat::Win16NE);
    CHECK(info.newHeaderOffset == kNewHeader);
    CHECK(info.ne.targetOS == 2);
    CHECK(info.ne.ExpectedWinMajor() == 3 && info.ne.ExpectedWinMinor() == 10);
    CHECK(info.ne.segmentCount == 7);
    CHECK(!info.ne.IsLibrary());
    CHECK(Classify(info) == LaunchPath::Win16Engine);

    // Windows 2.x/3.0-era: target OS left unspecified.
    CHECK(Inspect(MakeNe(0, 0x0302, 0), info, error));
    CHECK(Classify(info) == LaunchPath::Win16Engine);

    // 16-bit DLL.
    CHECK(Inspect(MakeNe(2, 0x8000, 0x030A), info, error));
    CHECK(info.ne.IsLibrary());
    CHECK(Classify(info) == LaunchPath::Unsupported);

    // OS/2 1.x program.
    CHECK(Inspect(MakeNe(1, 0x0302, 0), info, error));
    CHECK(Classify(info) == LaunchPath::Unsupported);

    // Truncated NE header is an error.
    Image cut = MakeNe(2, 0x0302, 0x030A);
    cut.resize(kNewHeader + 0x20);
    CHECK(!Inspect(cut, info, error));
}

void TestLinear() {
    ExeInfo info;
    std::string error;
    Image le = MakeDos(kNewHeader);
    Put(le, kNewHeader, uint16_t{0x454C});
    CHECK(Inspect(le, info, error));
    CHECK(info.format == ExeFormat::LinearLE);
    CHECK(Classify(info) == LaunchPath::Unsupported);

    Image lx = MakeDos(kNewHeader);
    Put(lx, kNewHeader, uint16_t{0x584C});
    CHECK(Inspect(lx, info, error));
    CHECK(info.format == ExeFormat::LinearLX);
}

void TestPe32() {
    ExeInfo info;
    std::string error;
    CHECK(Inspect(MakePe32(), info, error));
    CHECK(info.format == ExeFormat::PE32);
    CHECK(info.pe.machine == IMAGE_FILE_MACHINE_I386);
    CHECK(info.pe.subsystem == IMAGE_SUBSYSTEM_WINDOWS_GUI);
    CHECK(info.pe.imageBase == 0x400000);
    CHECK(info.pe.entryPointRva == 0x1234);
    CHECK(!info.pe.IsDll());
    CHECK(!info.pe.isManaged);
    CHECK(Classify(info) == LaunchPath::NativeWow64);

    CHECK(Inspect(MakePe32(IMAGE_SUBSYSTEM_WINDOWS_CUI), info, error));
    CHECK(Classify(info) == LaunchPath::NativeWow64);

    // DLL.
    CHECK(Inspect(MakePe32(IMAGE_SUBSYSTEM_WINDOWS_GUI, IMAGE_FILE_DLL), info, error));
    CHECK(info.pe.IsDll());
    CHECK(Classify(info) == LaunchPath::Unsupported);

    // NT 4.0 Alpha binary.
    CHECK(Inspect(MakePe32(IMAGE_SUBSYSTEM_WINDOWS_GUI, IMAGE_FILE_EXECUTABLE_IMAGE,
                           IMAGE_FILE_MACHINE_ALPHA), info, error));
    CHECK(Classify(info) == LaunchPath::Unsupported);

    // Native (driver) subsystem.
    CHECK(Inspect(MakePe32(IMAGE_SUBSYSTEM_NATIVE), info, error));
    CHECK(Classify(info) == LaunchPath::Unsupported);

    // .NET assembly.
    Image managed = MakePe32();
    const size_t clrDir = kNewHeader + offsetof(IMAGE_NT_HEADERS32, OptionalHeader) +
                          offsetof(IMAGE_OPTIONAL_HEADER32, DataDirectory) +
                          IMAGE_DIRECTORY_ENTRY_COM_DESCRIPTOR * sizeof(IMAGE_DATA_DIRECTORY);
    Put(managed, clrDir, IMAGE_DATA_DIRECTORY{0x2000, 0x48});
    CHECK(Inspect(managed, info, error));
    CHECK(info.pe.isManaged);
    CHECK(Classify(info) == LaunchPath::Unsupported);

    // Optional header declaring fewer data directories than the struct holds.
    Image shortOpt = MakePe32();
    const WORD fixedPart = offsetof(IMAGE_OPTIONAL_HEADER32, DataDirectory);
    Put(shortOpt, kNewHeader + 4 + offsetof(IMAGE_FILE_HEADER, SizeOfOptionalHeader), fixedPart);
    shortOpt.resize(kNewHeader + 4 + sizeof(IMAGE_FILE_HEADER) + fixedPart);
    CHECK(Inspect(shortOpt, info, error));
    CHECK(info.format == ExeFormat::PE32);
    CHECK(!info.pe.isManaged);

    // Truncated optional header is an error.
    Image cut = MakePe32();
    cut.resize(kNewHeader + 0x30);
    CHECK(!Inspect(cut, info, error));

    // Bogus optional header magic.
    Image badMagic = MakePe32();
    Put(badMagic, kNewHeader + 4 + sizeof(IMAGE_FILE_HEADER), uint16_t{0x1234});
    CHECK(!Inspect(badMagic, info, error));
}

void TestPe32Plus() {
    ExeInfo info;
    std::string error;
    Image img = MakePe<IMAGE_NT_HEADERS64>(IMAGE_FILE_MACHINE_AMD64, IMAGE_NT_OPTIONAL_HDR64_MAGIC,
                                           IMAGE_SUBSYSTEM_WINDOWS_GUI,
                                           IMAGE_FILE_EXECUTABLE_IMAGE);
    CHECK(Inspect(img, info, error));
    CHECK(info.format == ExeFormat::PE32Plus);
    CHECK(info.pe.machine == IMAGE_FILE_MACHINE_AMD64);
    CHECK(Classify(info) == LaunchPath::Native64);
}

void TestRealFiles() {
    ExeInfo info;
    std::string error;

    // This test is x86, so System32 is redirected to SysWOW64 by WOW64.
    // Sysnative is the escape hatch to see the real 64-bit System32.
    if (InspectFile(L"C:\\Windows\\SysWOW64\\kernel32.dll", info, error)) {
        CHECK(info.format == ExeFormat::PE32);
        CHECK(info.pe.machine == IMAGE_FILE_MACHINE_I386);
        CHECK(info.pe.IsDll());
    } else {
        std::printf("  (skipped SysWOW64 check: %s)\n", error.c_str());
    }

    if (InspectFile(L"C:\\Windows\\Sysnative\\kernel32.dll", info, error)) {
        CHECK(info.format == ExeFormat::PE32Plus);
        CHECK(info.pe.IsDll());
    } else {
        std::printf("  (skipped Sysnative check: %s)\n", error.c_str());
    }

    CHECK(!InspectFile(L"C:\\definitely\\not\\here.exe", info, error));
}

}  // namespace

int main() {
    const test::Case cases[] = {
        {"NotExecutable", TestNotExecutable},
        {"PlainDos", TestPlainDos},
        {"Win16", TestWin16},
        {"Linear", TestLinear},
        {"Pe32", TestPe32},
        {"Pe32Plus", TestPe32Plus},
        {"RealFiles", TestRealFiles},
    };
    return test::RunAll(cases);
}
