// Child process propagation.
//
// Many games start from a launcher/autorun exe that spawns the real game.
// Every child is created suspended; if it has the same architecture as us
// (x86 under WOW64), RetroShim is added to its import table and the config
// payload is copied over before it runs, the same as RetroLaunch does.
//
// This does the equivalent of DetourCreateProcessWithDllExW, but checks the
// architecture itself first. For a 64-bit child, DetourCreateProcessWithDllEx
// would go looking for a "RetroShim64.dll" helper and kill the child when it
// can't find one, breaking e.g. a launcher that opens a modern web browser.
//
// Hooked entry points:
//   CreateProcessA / CreateProcessW    the documented APIs (primary path)
//   kernelbase!CreateProcessInternalW  undocumented funnel under every
//                                      creation API; catches ShellExecuteEx,
//                                      CreateProcessAsUser, WinExec, etc.
// A thread-local guard makes the outermost hook the only one that acts.

#include <string>

#include "Hooks.h"
#include "Log.h"
#include "ShimState.h"
#include "retro/PathUtil.h"

namespace retro::shim {
namespace {

using CreateProcessInternalWFn = BOOL WINAPI(
    HANDLE userToken, LPCWSTR applicationName, LPWSTR commandLine,
    LPSECURITY_ATTRIBUTES processAttributes, LPSECURITY_ATTRIBUTES threadAttributes,
    BOOL inheritHandles, DWORD creationFlags, LPVOID environment, LPCWSTR currentDirectory,
    LPSTARTUPINFOW startupInfo, LPPROCESS_INFORMATION processInformation,
    PHANDLE newToken);

decltype(&CreateProcessA) Real_CreateProcessA = CreateProcessA;
decltype(&CreateProcessW) Real_CreateProcessW = CreateProcessW;
CreateProcessInternalWFn* Real_CreateProcessInternalW = nullptr;  // resolved at attach

std::string g_shimPathAnsi;

// Set while a hook is handling a creation, so the nested internal call made
// by the real CreateProcessA/W passes straight through.
thread_local bool t_creating = false;

std::string Describe(LPCSTR app, LPCSTR cmd) {
    return cmd ? cmd : (app ? app : "(null)");
}

std::string Describe(LPCWSTR app, LPCWSTR cmd) {
    return cmd ? ToUtf8(cmd) : (app ? ToUtf8(app) : "(null)");
}

void InjectIntoChild(const PROCESS_INFORMATION& pi) {
    USHORT selfMachine = 0, childMachine = 0, nativeMachine = 0;
    if (!IsWow64Process2(GetCurrentProcess(), &selfMachine, &nativeMachine) ||
        !IsWow64Process2(pi.hProcess, &childMachine, &nativeMachine)) {
        log::Write("  child %lu: cannot determine architecture (error %lu); not injecting",
                   pi.dwProcessId, GetLastError());
        return;
    }
    if (childMachine != selfMachine) {
        log::Write("  child %lu: different architecture; running without shim", pi.dwProcessId);
        return;
    }

    LPCSTR dlls[] = {g_shimPathAnsi.c_str()};
    if (!DetourUpdateProcessWithDll(pi.hProcess, dlls, 1)) {
        log::Write("  child %lu: DetourUpdateProcessWithDll failed (error %lu)", pi.dwProcessId,
                   GetLastError());
        return;
    }
    const ShimConfig& config = Config();
    if (!DetourCopyPayloadToProcess(pi.hProcess, kShimConfigGuid, &config, sizeof(config))) {
        // The shim still loads, with default settings.
        log::Write("  child %lu: copying config failed (error %lu); child uses defaults",
                   pi.dwProcessId, GetLastError());
        return;
    }
    log::Write("  child %lu: RetroShim injected", pi.dwProcessId);
}

// Shared body of all hooks. `create(flags)` calls the real function with the
// caller's arguments and the given creation flags.
template <class App, class Cmd, class CreateFn>
BOOL CreateWithShim(const char* api, App app, Cmd cmd, DWORD flags, LPPROCESS_INFORMATION pi,
                    CreateFn create) {
    if (t_creating || !pi) return create(flags);

    t_creating = true;
    log::Write("%s: %s", api, Describe(app, cmd).c_str());

    const BOOL ok = create(flags | CREATE_SUSPENDED);
    const DWORD lastError = GetLastError();

    if (ok) {
        InjectIntoChild(*pi);
        if (!(flags & CREATE_SUSPENDED)) ResumeThread(pi->hThread);
    } else if (lastError == ERROR_BAD_EXE_FORMAT) {
        // Most likely a 16-bit NE child, which x64 Windows cannot run.
        // Routing these to the Win16 engine hooks in here later.
        log::Write("  failed: bad exe format (16-bit child?)");
    } else {
        log::Write("  failed: error %lu", lastError);
    }

    t_creating = false;
    SetLastError(lastError);
    return ok;
}

BOOL WINAPI Hook_CreateProcessA(LPCSTR app, LPSTR cmd, LPSECURITY_ATTRIBUTES pa,
                                LPSECURITY_ATTRIBUTES ta, BOOL inherit, DWORD flags, LPVOID env,
                                LPCSTR cwd, LPSTARTUPINFOA si, LPPROCESS_INFORMATION pi) {
    return CreateWithShim("CreateProcessA", app, cmd, flags, pi, [&](DWORD f) {
        return Real_CreateProcessA(app, cmd, pa, ta, inherit, f, env, cwd, si, pi);
    });
}

BOOL WINAPI Hook_CreateProcessW(LPCWSTR app, LPWSTR cmd, LPSECURITY_ATTRIBUTES pa,
                                LPSECURITY_ATTRIBUTES ta, BOOL inherit, DWORD flags, LPVOID env,
                                LPCWSTR cwd, LPSTARTUPINFOW si, LPPROCESS_INFORMATION pi) {
    return CreateWithShim("CreateProcessW", app, cmd, flags, pi, [&](DWORD f) {
        return Real_CreateProcessW(app, cmd, pa, ta, inherit, f, env, cwd, si, pi);
    });
}

BOOL WINAPI Hook_CreateProcessInternalW(HANDLE token, LPCWSTR app, LPWSTR cmd,
                                        LPSECURITY_ATTRIBUTES pa, LPSECURITY_ATTRIBUTES ta,
                                        BOOL inherit, DWORD flags, LPVOID env, LPCWSTR cwd,
                                        LPSTARTUPINFOW si, LPPROCESS_INFORMATION pi,
                                        PHANDLE newToken) {
    return CreateWithShim("CreateProcessInternalW", app, cmd, flags, pi, [&](DWORD f) {
        return Real_CreateProcessInternalW(token, app, cmd, pa, ta, inherit, f, env, cwd, si, pi,
                                           newToken);
    });
}

}  // namespace

LONG AttachProcessHooks() {
    wchar_t path[MAX_PATH];
    const DWORD n = GetModuleFileNameW(Module(), path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH || !ToAnsiPath(path, g_shimPathAnsi)) {
        log::Write("child-process: cannot resolve an ANSI path to RetroShim.dll");
        return ERROR_BAD_PATHNAME;
    }

    LONG err = AttachHook(Real_CreateProcessA, Hook_CreateProcessA);
    if (err == NO_ERROR) err = AttachHook(Real_CreateProcessW, Hook_CreateProcessW);

    // Optional: if a future Windows drops the export, the A/W hooks still work.
    if (HMODULE kernelBase = GetModuleHandleW(L"kernelbase.dll")) {
        Real_CreateProcessInternalW = reinterpret_cast<CreateProcessInternalWFn*>(
            GetProcAddress(kernelBase, "CreateProcessInternalW"));
    }
    if (err == NO_ERROR && Real_CreateProcessInternalW) {
        err = AttachHook(Real_CreateProcessInternalW, Hook_CreateProcessInternalW);
    } else if (!Real_CreateProcessInternalW) {
        log::Write("child-process: CreateProcessInternalW not found; ShellExecute children "
                   "will not be shimmed");
    }
    return err;
}

LONG DetachProcessHooks() {
    LONG err = DetachHook(Real_CreateProcessA, Hook_CreateProcessA);
    if (err == NO_ERROR) err = DetachHook(Real_CreateProcessW, Hook_CreateProcessW);
    if (err == NO_ERROR && Real_CreateProcessInternalW)
        err = DetachHook(Real_CreateProcessInternalW, Hook_CreateProcessInternalW);
    return err;
}

}  // namespace retro::shim
