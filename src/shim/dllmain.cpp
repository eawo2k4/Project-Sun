// RetroShim.dll: injected into the game by RetroLaunch (or by a shimmed parent
// process) via Detours.
//
// Detours adds this DLL to the target's import table while the process is
// suspended, so DllMain runs before the game's own entry point. That is where
// the config payload is read and the hook modules are attached.

#include <windows.h>

#include <detours/detours.h>

#include <cstddef>
#include <cstring>

#include "Log.h"
#include "Pacing.h"
#include "ShimState.h"
#include "gfx/Graphics.h"
#include "hooks/Hooks.h"
#include "retro/PathUtil.h"
#include "retro/ShimProtocol.h"

namespace retro::shim {
namespace {

ShimConfig g_config{};
HMODULE g_module = nullptr;

struct HookModule {
    const char* name;
    uint32_t features;  // installed if any of these feature bits is set
    LONG (*attach)();
    LONG (*detach)();
    bool installed;
};

HookModule g_modules[] = {
    {"storage", ShimFeature_ClampStorage, AttachStorageHooks, DetachStorageHooks, false},
    {"memory", ShimFeature_ClampMemory, AttachMemoryHooks, DetachMemoryHooks, false},
    {"child-process", ShimFeature_ChildProcesses, AttachProcessHooks, DetachProcessHooks, false},
    {"display", ShimFeature_DisplaySandbox, AttachDisplayHooks, DetachDisplayHooks, false},
    {"render", ShimFeature_DisplaySandbox | ShimFeature_FrameLimiter, AttachRenderHooks,
     DetachRenderHooks, false},
    {"graphics", ShimFeature_DisplaySandbox | ShimFeature_FrameLimiter, AttachGraphicsHooks,
     DetachGraphicsHooks, false},
};

void LoadConfig() {
    DWORD size = 0;
    const void* payload = DetourFindPayloadEx(kShimConfigGuid, &size);
    if (!payload || size < offsetof(ShimConfig, diskCapMiB)) {
        return;  // not launched by RetroLaunch (or a pre-v1 launcher): keep defaults
    }
    // Copy only what both sides understand, so launcher/shim version skew
    // degrades gracefully: fields the launcher didn't send keep their defaults.
    memcpy(&g_config, payload, (size < sizeof(g_config)) ? size : sizeof(g_config));
    g_config.logPath[MAX_PATH - 1] = L'\0';
}

// Runs one module's attach/detach in its own transaction.
LONG RunTransaction(LONG (*step)()) {
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    const LONG err = step();
    if (err != NO_ERROR) {
        DetourTransactionAbort();
        return err;
    }
    return DetourTransactionCommit();
}

void InstallHooks() {
    for (HookModule& m : g_modules) {
        if (!(g_config.features & m.features)) {
            log::Write("%s: disabled", m.name);
            continue;
        }
        const LONG err = RunTransaction(m.attach);
        m.installed = err == NO_ERROR;
        if (m.installed) {
            log::Write("%s: hooks installed", m.name);
        } else {
            log::Write("%s: FAILED to install hooks (error %ld)", m.name, err);
        }
    }
}

void RemoveHooks() {
    for (HookModule& m : g_modules) {
        if (m.installed) {
            RunTransaction(m.detach);
            m.installed = false;
        }
    }
}

void LogStartup() {
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    log::Write("Attached to %s", ToUtf8(exe).c_str());
    log::Write("Config v%u: features=0x%08X fpsCap=%u diskCap=%u MiB memoryCap=%u MiB",
               g_config.version, g_config.features, g_config.fpsCap, g_config.diskCapMiB,
               g_config.memoryCapMiB);
}

}  // namespace

const ShimConfig& Config() { return g_config; }

HMODULE Module() { return g_module; }

}  // namespace retro::shim

// Diagnostic export (by name, see RetroShim.def): lets tools and the test
// probe check a module really is active before relying on it, e.g. before
// calling ChangeDisplaySettings on a real desktop.
// "ddraw" and "d3d9" report whether that DLL's entry points are hooked.
extern "C" BOOL WINAPI RetroShimIsModuleActive(const char* name) {
    if (!name) return FALSE;
    for (const auto& m : retro::shim::g_modules) {
        if (strcmp(m.name, name) == 0) return m.installed ? TRUE : FALSE;
    }
    return retro::shim::gfx::IsApiHooked(name) ? TRUE : FALSE;
}

// Diagnostic export: what the DirectDraw presenter last put on screen.
extern "C" BOOL WINAPI RetroShimGetPresentStats(retro::PresentStats* stats) {
    if (!stats || stats->cbSize < sizeof(retro::PresentStats)) return FALSE;
    retro::shim::gfx::GetPresentStats(*stats);
    return TRUE;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
    using namespace retro::shim;

    // When Detours bridges bitness it loads this DLL into rundll32 as a helper;
    // do nothing there.
    if (DetourIsHelperProcess()) return TRUE;

    switch (reason) {
    case DLL_PROCESS_ATTACH:
        g_module = instance;
        DisableThreadLibraryCalls(instance);
        // Restore the import table Detours modified to get us loaded.
        DetourRestoreAfterWith();

        LoadConfig();
        retro::log::Open(g_config.logPath);
        LogStartup();
        // One pacer for every presentation path (GDI, DirectDraw, Direct3D).
        pacing::Configure((g_config.features & retro::ShimFeature_FrameLimiter) ? g_config.fpsCap
                                                                                 : 0);
        InstallHooks();
        break;

    case DLL_PROCESS_DETACH:
        // reserved != nullptr means the process is terminating: other threads
        // are already gone and unhooking is pointless (and risky).
        if (reserved == nullptr) RemoveHooks();
        retro::log::Write("Detached");
        retro::log::Close();
        break;
    }
    return TRUE;
}
