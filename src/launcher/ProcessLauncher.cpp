#include "ProcessLauncher.h"

#include <detours/detours.h>

#include <format>
#include <vector>

#include "retro/PathUtil.h"

namespace retro {

std::wstring QuoteArgument(const std::wstring& arg) {
    if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring::npos) return arg;

    std::wstring out = L"\"";
    for (auto it = arg.begin();; ++it) {
        size_t backslashes = 0;
        while (it != arg.end() && *it == L'\\') {
            ++it;
            ++backslashes;
        }
        if (it == arg.end()) {
            // Double trailing backslashes so they don't escape the closing quote.
            out.append(backslashes * 2, L'\\');
            break;
        }
        if (*it == L'"') {
            out.append(backslashes * 2 + 1, L'\\');
        } else {
            out.append(backslashes, L'\\');
        }
        out.push_back(*it);
    }
    out.push_back(L'"');
    return out;
}

std::string Win32ErrorMessage(DWORD code) {
    char* buffer = nullptr;
    DWORD len = FormatMessageA(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, 0, reinterpret_cast<char*>(&buffer), 0, nullptr);
    std::string msg = len ? std::string(buffer, len) : "unknown error";
    LocalFree(buffer);
    while (!msg.empty() && (msg.back() == '\n' || msg.back() == '\r' || msg.back() == ' '))
        msg.pop_back();
    return std::format("{} (Win32 error {})", msg, code);
}

bool LaunchProcess(const LaunchRequest& request, PROCESS_INFORMATION& pi, std::string& error) {
    pi = {};

    // CreateProcess may write into the command line buffer, so it must be mutable.
    std::wstring cmd = QuoteArgument(request.exePath.wstring());
    if (!request.arguments.empty()) cmd += L" " + request.arguments;
    std::vector<wchar_t> cmdLine(cmd.begin(), cmd.end());
    cmdLine.push_back(L'\0');

    const std::wstring workDir = request.workingDir.wstring();
    STARTUPINFOW si{};
    si.cb = sizeof(si);

    // Always start suspended: the shim payload must be in place before the
    // shim's DllMain runs, which happens as soon as the loader starts.
    const DWORD flags = CREATE_SUSPENDED | CREATE_DEFAULT_ERROR_MODE;

    BOOL ok;
    if (request.injectShim) {
        std::string dllPath;
        if (!ToAnsiPath(request.shimPath.wstring(), dllPath)) {
            error = "Shim path cannot be represented in the ANSI code page; move RetroShim.dll "
                    "to a plain ASCII path";
            return false;
        }
        ok = DetourCreateProcessWithDllExW(
            request.exePath.c_str(), cmdLine.data(), nullptr, nullptr, FALSE, flags,
            nullptr, workDir.c_str(), &si, &pi, dllPath.c_str(), nullptr);
    } else {
        ok = CreateProcessW(request.exePath.c_str(), cmdLine.data(), nullptr, nullptr, FALSE,
                            flags, nullptr, workDir.c_str(), &si, &pi);
    }
    if (!ok) {
        error = std::format("Failed to create process: {}", Win32ErrorMessage(GetLastError()));
        return false;
    }

    if (request.injectShim &&
        !DetourCopyPayloadToProcess(pi.hProcess, kShimConfigGuid, &request.config,
                                    sizeof(request.config))) {
        error = std::format("Failed to copy shim config into process: {}",
                            Win32ErrorMessage(GetLastError()));
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        pi = {};
        return false;
    }

    if (ResumeThread(pi.hThread) == static_cast<DWORD>(-1)) {
        error = std::format("Failed to resume process: {}", Win32ErrorMessage(GetLastError()));
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        pi = {};
        return false;
    }
    return true;
}

}  // namespace retro
