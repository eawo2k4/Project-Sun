// x86 target used by the end-to-end tests. Run under RetroLaunch, it checks
// what the injected shim actually did to this process.
//
//   ShimProbe [loaded]            exit 0 if RetroShim.dll is loaded
//   ShimProbe disk <capMiB>       GetDiskFreeSpace(Ex)A/W results are overflow-safe
//   ShimProbe memory <capMiB>     GlobalMemoryStatus(Ex) report exactly <capMiB>
//   ShimProbe spawn-w <mode...>   run ShimProbe <mode...> via CreateProcessW, return its code
//   ShimProbe spawn-a <mode...>   same via CreateProcessA
//   ShimProbe spawn-shell <mode...> same via ShellExecuteExW
//   ShimProbe spawn64             run 64-bit cmd.exe; it must still start and exit normally
//   ShimProbe module <name> <0|1> RetroShimIsModuleActive(name) matches
//   ShimProbe display             ChangeDisplaySettings/EnumDisplaySettings are sandboxed
//   ShimProbe window [windowed]   fullscreen window is sandboxed, scaled and input-mapped
//   ShimProbe adopt               windows sized to the virtual screen get adopted
//   ShimProbe render <fps>        whole-frame blits are paced at <fps> (0 = unpaced)

#include <windows.h>

#include <objbase.h>
#include <shellapi.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "ProbeCommon.h"

using probe::Expect;

namespace {

constexpr uint64_t kMiB = 1024ull * 1024ull;
constexpr uint64_t kInt32SafeBytes = 0x7FFF0000ull;

bool LowDwordSafe(uint64_t v) {
    return v <= kInt32SafeBytes || static_cast<int32_t>(static_cast<uint32_t>(v)) >= 0x40000000;
}

std::wstring SelfPath() {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    return path;
}

int ProbeLoaded() {
    const bool loaded = GetModuleHandleW(L"RetroShim.dll") != nullptr;
    std::printf("ShimProbe: RetroShim.dll %s\n", loaded ? "is loaded" : "is NOT loaded");
    return loaded ? 0 : 1;
}

template <class Char, class Fn>
void ProbeDiskGeometry(Fn getDiskFreeSpace, const Char* root, const char* api) {
    DWORD spc = 0, bps = 0, freeC = 0, totalC = 0;
    if (!getDiskFreeSpace(root, &spc, &bps, &freeC, &totalC)) {
        Expect(false, api);
        return;
    }
    // Exactly the arithmetic a Win9x installer does.
    const int32_t vintageTotal = static_cast<int32_t>(spc * bps * totalC);
    const int32_t vintageFree = static_cast<int32_t>(spc * bps * freeC);
    const uint64_t realTotal = static_cast<uint64_t>(spc) * bps * totalC;
    std::printf("%s: %lu x %lu bytes, %lu / %lu clusters -> 32-bit math: %ld / %ld\n", api, spc,
                bps, freeC, totalC, static_cast<long>(vintageFree), static_cast<long>(vintageTotal));
    Expect(vintageTotal > 0 && static_cast<uint64_t>(vintageTotal) == realTotal,
           "total bytes survive signed 32-bit math");
    Expect(vintageFree >= 0 && vintageFree <= vintageTotal, "free bytes in [0, total]");
}

template <class Char, class Fn>
void ProbeDiskSpaceEx(Fn getDiskFreeSpaceEx, const Char* root, const char* api, uint64_t cap) {
    ULARGE_INTEGER avail{}, total{}, free{};
    if (!getDiskFreeSpaceEx(root, &avail, &total, &free)) {
        Expect(false, api);
        return;
    }
    std::printf("%s: avail 0x%llX, free 0x%llX, total 0x%llX\n", api, avail.QuadPart,
                free.QuadPart, total.QuadPart);
    Expect(total.QuadPart <= cap, "total <= cap");
    Expect(avail.QuadPart <= free.QuadPart && free.QuadPart <= total.QuadPart,
           "avail <= free <= total");
    Expect(LowDwordSafe(total.QuadPart) && LowDwordSafe(free.QuadPart) &&
               LowDwordSafe(avail.QuadPart),
           "values survive truncation to 32 bits");

    // Optional outputs may be null.
    ULARGE_INTEGER onlyFree{};
    Expect(getDiskFreeSpaceEx(root, nullptr, nullptr, &onlyFree) &&
               onlyFree.QuadPart == free.QuadPart,
           "null optional outputs accepted");
}

int ProbeDisk(uint32_t capMiB) {
    const uint64_t cap = capMiB * kMiB;
    ProbeDiskGeometry(GetDiskFreeSpaceA, "C:\\", "GetDiskFreeSpaceA");
    ProbeDiskGeometry(GetDiskFreeSpaceW, L"C:\\", "GetDiskFreeSpaceW");
    ProbeDiskSpaceEx(GetDiskFreeSpaceExA, "C:\\", "GetDiskFreeSpaceExA", cap);
    ProbeDiskSpaceEx(GetDiskFreeSpaceExW, L"C:\\", "GetDiskFreeSpaceExW", cap);

    DWORD onlyTotal = 0;
    Expect(GetDiskFreeSpaceA(nullptr, nullptr, nullptr, nullptr, &onlyTotal) && onlyTotal > 0,
           "GetDiskFreeSpaceA(current dir, single output)");
    return probe::Result();
}

int ProbeMemory(uint32_t capMiB) {
    const uint64_t cap = capMiB * kMiB;

    MEMORYSTATUS ms{};
    ms.dwLength = sizeof(ms);
    GlobalMemoryStatus(&ms);
    std::printf("GlobalMemoryStatus: phys %llu/%llu MiB, pagefile %llu/%llu MiB, virtual %llu/%llu MiB, "
                "load %lu%%\n",
                ms.dwAvailPhys / kMiB, ms.dwTotalPhys / kMiB, ms.dwAvailPageFile / kMiB,
                ms.dwTotalPageFile / kMiB, ms.dwAvailVirtual / kMiB, ms.dwTotalVirtual / kMiB,
                ms.dwMemoryLoad);
    Expect(ms.dwTotalPhys == cap, "dwTotalPhys == cap");
    Expect(static_cast<int32_t>(ms.dwAvailPhys) >= 0 && ms.dwAvailPhys <= ms.dwTotalPhys,
           "dwAvailPhys in [0, total]");
    Expect(static_cast<int32_t>(ms.dwTotalPageFile) > 0, "dwTotalPageFile positive as int");
    Expect(static_cast<int32_t>(ms.dwTotalVirtual) > 0, "dwTotalVirtual positive as int");

    MEMORYSTATUSEX ex{};
    ex.dwLength = sizeof(ex);
    Expect(GlobalMemoryStatusEx(&ex) != FALSE, "GlobalMemoryStatusEx succeeds");
    std::printf("GlobalMemoryStatusEx: phys %llu/%llu MiB\n", ex.ullAvailPhys / kMiB,
                ex.ullTotalPhys / kMiB);
    Expect(ex.ullTotalPhys == cap, "ullTotalPhys == cap");
    Expect(ex.ullAvailPhys <= ex.ullTotalPhys, "ullAvailPhys <= total");
    Expect(ex.ullTotalPageFile <= kInt32SafeBytes, "ullTotalPageFile clamped");

    // A bad dwLength must still fail the way the real API does.
    MEMORYSTATUSEX bad{};
    bad.dwLength = 4;
    Expect(!GlobalMemoryStatusEx(&bad), "bad dwLength rejected");
    return probe::Result();
}

std::wstring JoinArgs(int argc, wchar_t** argv, int first) {
    std::wstring args;
    for (int i = first; i < argc; ++i) {
        if (!args.empty()) args += L' ';
        args += argv[i];  // test modes never contain spaces
    }
    return args;
}

int WaitAndGetExitCode(HANDLE process) {
    WaitForSingleObject(process, INFINITE);
    DWORD code = 100;
    GetExitCodeProcess(process, &code);
    CloseHandle(process);
    std::printf("child exited with %lu\n", code);
    return static_cast<int>(code);
}

int SpawnSelfW(const std::wstring& args) {
    std::wstring cmd = L"\"" + SelfPath() + L"\" " + args;
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si,
                        &pi)) {
        std::printf("CreateProcessW failed: %lu\n", GetLastError());
        return 100;
    }
    CloseHandle(pi.hThread);
    return WaitAndGetExitCode(pi.hProcess);
}

int SpawnSelfA(const std::wstring& args) {
    char self[MAX_PATH];
    GetModuleFileNameA(nullptr, self, MAX_PATH);
    std::string narrowArgs;
    for (wchar_t c : args) narrowArgs += static_cast<char>(c);  // test modes are ASCII
    std::string cmd = std::string("\"") + self + "\" " + narrowArgs;
    STARTUPINFOA si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessA(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si,
                        &pi)) {
        std::printf("CreateProcessA failed: %lu\n", GetLastError());
        return 100;
    }
    CloseHandle(pi.hThread);
    return WaitAndGetExitCode(pi.hProcess);
}

int SpawnSelfShell(const std::wstring& args) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    const std::wstring self = SelfPath();
    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
    sei.lpFile = self.c_str();
    sei.lpParameters = args.c_str();
    sei.nShow = SW_HIDE;
    if (!ShellExecuteExW(&sei) || !sei.hProcess) {
        std::printf("ShellExecuteExW failed: %lu\n", GetLastError());
        return 100;
    }
    return WaitAndGetExitCode(sei.hProcess);
}

int Spawn64() {
    // We're 32-bit, so System32 would redirect to SysWOW64; Sysnative is the real one.
    wchar_t cmd[] = L"cmd.exe /c exit 42";
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(L"C:\\Windows\\Sysnative\\cmd.exe", cmd, nullptr, nullptr, FALSE, 0,
                        nullptr, nullptr, &si, &pi)) {
        std::printf("CreateProcessW(64-bit cmd) failed: %lu\n", GetLastError());
        return 100;
    }
    CloseHandle(pi.hThread);
    return WaitAndGetExitCode(pi.hProcess) == 42 ? 0 : 1;
}

uint32_t ArgOr(int argc, wchar_t** argv, int index, uint32_t fallback) {
    return index < argc ? static_cast<uint32_t>(std::wcstoul(argv[index], nullptr, 10)) : fallback;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    const std::wstring mode = argc > 1 ? argv[1] : L"loaded";

    if (mode == L"loaded") return ProbeLoaded();
    if (mode == L"disk") return ProbeDisk(ArgOr(argc, argv, 2, 8192));
    if (mode == L"memory") return ProbeMemory(ArgOr(argc, argv, 2, 1024));
    if (mode == L"spawn-w") return SpawnSelfW(JoinArgs(argc, argv, 2));
    if (mode == L"spawn-a") return SpawnSelfA(JoinArgs(argc, argv, 2));
    if (mode == L"spawn-shell") return SpawnSelfShell(JoinArgs(argc, argv, 2));
    if (mode == L"spawn64") return Spawn64();
    if (mode == L"module" && argc > 3) {
        char name[64];
        WideCharToMultiByte(CP_ACP, 0, argv[2], -1, name, sizeof(name), nullptr, nullptr);
        return probe::ProbeModule(name, argv[3][0] == L'1');
    }
    if (mode == L"display") return probe::ProbeDisplayModes();
    if (mode == L"window") return probe::ProbeWindow(argc > 2 && std::wstring(argv[2]) == L"windowed");
    if (mode == L"adopt") return probe::ProbeAdoption();
    if (mode == L"render") return probe::ProbeRender(ArgOr(argc, argv, 2, 60));

    std::printf("unknown mode\n");
    return 2;
}
