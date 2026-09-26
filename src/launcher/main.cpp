// RetroLaunch.exe: inspects a vintage executable and launches it with
// RetroShim.dll injected.

#include <windows.h>

#include <cstdio>
#include <cwchar>
#include <filesystem>
#include <string>

#include "ProcessLauncher.h"
#include "retro/ClampPolicy.h"
#include "retro/ExeFormat.h"
#include "retro/PathUtil.h"

namespace fs = std::filesystem;
using retro::ToUtf8;

namespace {

enum ExitCode : int {
    Exit_Ok = 0,
    Exit_Usage = 2,
    Exit_BadImage = 3,
    Exit_Unsupported = 4,
    Exit_LaunchFailed = 5,
};

struct Options {
    fs::path program;
    std::wstring arguments;
    fs::path shimPath;
    fs::path workingDir;
    bool inspectOnly = false;
    bool noShim = false;
    bool wait = false;
    uint32_t fpsCap = 60;
    uint32_t diskCapMiB = retro::kDefaultDiskCapMiB;
    uint32_t memoryCapMiB = retro::kDefaultMemoryCapMiB;
    uint32_t features = retro::ShimFeature_Default;
};

void PrintUsage() {
    std::fputs(
        "RetroLaunch - vintage Windows game runner\n"
        "\n"
        "Usage: RetroLaunch [options] <program.exe> [program arguments...]\n"
        "\n"
        "Options:\n"
        "  --inspect         Print executable header info and exit\n"
        "  --wait            Wait for the program to exit and return its exit code\n"
        "  --no-shim         Launch without injecting RetroShim.dll (baseline)\n"
        "  --shim <path>     Shim DLL to inject (default: RetroShim.dll beside RetroLaunch)\n"
        "  --cwd <dir>       Working directory (default: the program's folder)\n"
        "  --fps <n>         Frame cap passed to the shim (default: 60)\n"
        "  --disk-cap <MiB>  Largest disk size/free space reported (64-16777216, default 8192)\n"
        "  --mem-cap <MiB>   Largest physical memory reported (16-2047, default 1024)\n"
        "  --no-clamp-disk   Don't hook GetDiskFreeSpace(Ex)\n"
        "  --no-clamp-mem    Don't hook GlobalMemoryStatus(Ex)\n"
        "  --no-propagate    Don't inject the shim into child processes\n"
        "  --                End of options\n",
        stderr);
}

bool ParseRange(const wchar_t* text, uint32_t min, uint32_t max, const char* option,
                uint32_t& out) {
    wchar_t* end = nullptr;
    const unsigned long v = std::wcstoul(text, &end, 10);
    if (end == text || *end != L'\0' || v < min || v > max) {
        std::fprintf(stderr, "%s must be a number between %u and %u\n", option, min, max);
        return false;
    }
    out = static_cast<uint32_t>(v);
    return true;
}

bool ParseArgs(int argc, wchar_t** argv, Options& opt) {
    int i = 1;
    for (; i < argc; ++i) {
        const std::wstring a = argv[i];
        if (a == L"--") {
            ++i;
            break;
        }
        if (a.rfind(L"--", 0) != 0) break;  // first non-option = the program

        auto needValue = [&]() -> const wchar_t* {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "Missing value for %s\n", ToUtf8(a).c_str());
                return nullptr;
            }
            return argv[++i];
        };

        if (a == L"--inspect") {
            opt.inspectOnly = true;
        } else if (a == L"--wait") {
            opt.wait = true;
        } else if (a == L"--no-shim") {
            opt.noShim = true;
        } else if (a == L"--shim") {
            const wchar_t* v = needValue();
            if (!v) return false;
            opt.shimPath = v;
        } else if (a == L"--cwd") {
            const wchar_t* v = needValue();
            if (!v) return false;
            opt.workingDir = v;
        } else if (a == L"--fps") {
            const wchar_t* v = needValue();
            if (!v || !ParseRange(v, 1, 1000, "--fps", opt.fpsCap)) return false;
        } else if (a == L"--disk-cap") {
            const wchar_t* v = needValue();
            if (!v || !ParseRange(v, retro::kDiskCapMinMiB, retro::kDiskCapMaxMiB, "--disk-cap",
                                  opt.diskCapMiB))
                return false;
        } else if (a == L"--mem-cap") {
            const wchar_t* v = needValue();
            if (!v || !ParseRange(v, retro::kMemoryCapMinMiB, retro::kMemoryCapMaxMiB,
                                  "--mem-cap", opt.memoryCapMiB))
                return false;
        } else if (a == L"--no-clamp-disk") {
            opt.features &= ~retro::ShimFeature_ClampStorage;
        } else if (a == L"--no-clamp-mem") {
            opt.features &= ~retro::ShimFeature_ClampMemory;
        } else if (a == L"--no-propagate") {
            opt.features &= ~retro::ShimFeature_ChildProcesses;
        } else if (a == L"--help" || a == L"-h") {
            return false;
        } else {
            std::fprintf(stderr, "Unknown option %s\n", ToUtf8(a).c_str());
            return false;
        }
    }

    if (i >= argc) return false;
    opt.program = argv[i++];

    for (; i < argc; ++i) {
        if (!opt.arguments.empty()) opt.arguments += L' ';
        opt.arguments += retro::QuoteArgument(argv[i]);
    }
    return true;
}

fs::path LauncherDirectory() {
    std::wstring buf(MAX_PATH, L'\0');
    for (;;) {
        DWORD n = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
        if (n < buf.size()) {
            buf.resize(n);
            break;
        }
        buf.resize(buf.size() * 2);
    }
    return fs::path(buf).parent_path();
}

// The shim is x86 code, so it must itself be a 32-bit x86 DLL.
bool ValidateShim(const fs::path& shim, std::string& error) {
    retro::ExeInfo info;
    if (!retro::InspectFile(shim.wstring(), info, error)) {
        error = "Cannot read shim DLL " + ToUtf8(shim.wstring()) + ": " + error;
        return false;
    }
    if (info.format != retro::ExeFormat::PE32 || info.pe.machine != IMAGE_FILE_MACHINE_I386 ||
        !info.pe.IsDll()) {
        error = "Shim " + ToUtf8(shim.wstring()) + " is not a 32-bit x86 DLL";
        return false;
    }
    return true;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);

    Options opt;
    if (!ParseArgs(argc, argv, opt)) {
        PrintUsage();
        return Exit_Usage;
    }

    std::error_code ec;
    const fs::path exe = fs::absolute(opt.program, ec);
    if (ec || !fs::is_regular_file(exe, ec)) {
        std::fprintf(stderr, "Program not found: %s\n", ToUtf8(opt.program.wstring()).c_str());
        return Exit_BadImage;
    }

    retro::ExeInfo info;
    std::string error;
    if (!retro::InspectFile(exe.wstring(), info, error)) {
        std::fprintf(stderr, "%s: %s\n", ToUtf8(exe.wstring()).c_str(), error.c_str());
        return Exit_BadImage;
    }

    std::string reason;
    const retro::LaunchPath path = retro::ClassifyLaunchPath(info, reason);

    std::printf("Program     : %s\n%s", ToUtf8(exe.wstring()).c_str(),
                retro::Describe(info).c_str());
    std::printf("Launch path : %s\n", std::string(retro::LaunchPathName(path)).c_str());
    if (!reason.empty()) std::printf("              %s\n", reason.c_str());

    if (opt.inspectOnly) return Exit_Ok;

    bool inject = !opt.noShim;
    switch (path) {
    case retro::LaunchPath::NativeWow64:
        break;
    case retro::LaunchPath::Native64:
        inject = false;
        break;
    case retro::LaunchPath::Win16Engine:
        std::fputs("error: the Win16 execution engine is not implemented yet.\n", stderr);
        return Exit_Unsupported;
    case retro::LaunchPath::Unsupported:
    default:
        std::fprintf(stderr, "error: cannot launch: %s\n", reason.c_str());
        return Exit_Unsupported;
    }

    retro::LaunchRequest req;
    req.exePath = exe;
    req.arguments = opt.arguments;
    req.workingDir = opt.workingDir.empty() ? exe.parent_path() : fs::absolute(opt.workingDir, ec);
    req.injectShim = inject;

    const fs::path launcherDir = LauncherDirectory();
    if (inject) {
        req.shimPath = opt.shimPath.empty() ? launcherDir / L"RetroShim.dll"
                                            : fs::absolute(opt.shimPath, ec);
        if (!ValidateShim(req.shimPath, error)) {
            std::fprintf(stderr, "error: %s\n", error.c_str());
            return Exit_LaunchFailed;
        }

        req.config.fpsCap = opt.fpsCap;
        req.config.features = opt.features;
        req.config.diskCapMiB = opt.diskCapMiB;
        req.config.memoryCapMiB = opt.memoryCapMiB;
        const fs::path logDir = launcherDir / L"logs";
        fs::create_directories(logDir, ec);
        const std::wstring logPath = (logDir / (exe.stem().wstring() + L".log")).wstring();
        if (!ec && logPath.size() < MAX_PATH) {
            wcscpy_s(req.config.logPath, logPath.c_str());
        }
    }

    PROCESS_INFORMATION pi{};
    if (!retro::LaunchProcess(req, pi, error)) {
        std::fprintf(stderr, "error: %s\n", error.c_str());
        return Exit_LaunchFailed;
    }

    std::printf("Launched    : PID %lu%s\n", pi.dwProcessId,
                inject ? " (RetroShim injected)" : "");
    if (inject && req.config.logPath[0])
        std::printf("Shim log    : %s\n", ToUtf8(req.config.logPath).c_str());
    std::fflush(stdout);

    int result = Exit_Ok;
    if (opt.wait) {
        WaitForSingleObject(pi.hProcess, INFINITE);
        DWORD code = 0;
        GetExitCodeProcess(pi.hProcess, &code);
        std::printf("Exited      : code %lu (0x%08lX)\n", code, code);
        result = static_cast<int>(code);
    }

    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return result;
}
