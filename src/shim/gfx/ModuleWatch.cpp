#include "ModuleWatch.h"

#include <winternl.h>

#include "../Log.h"

namespace retro::shim::gfx {
namespace {

// Loader notification API (ntdll, documented on MSDN but without a header).
constexpr ULONG kReasonLoaded = 1;
constexpr ULONG kReasonUnloaded = 2;

struct DllNotificationData {
    ULONG flags;
    const UNICODE_STRING* fullDllName;
    const UNICODE_STRING* baseDllName;
    void* dllBase;
    ULONG sizeOfImage;
};

using NotificationFn = VOID(CALLBACK*)(ULONG reason, const DllNotificationData* data, PVOID ctx);
using RegisterFn = NTSTATUS(NTAPI*)(ULONG flags, NotificationFn fn, PVOID ctx, PVOID* cookie);
using UnregisterFn = NTSTATUS(NTAPI*)(PVOID cookie);

const ModuleCallbacks* g_callbacks = nullptr;
size_t g_count = 0;
PVOID g_cookie = nullptr;

bool NameMatches(const UNICODE_STRING* name, const wchar_t* expected) {
    if (!name || !name->Buffer) return false;
    const int len = static_cast<int>(name->Length / sizeof(wchar_t));
    const int expectedLen = static_cast<int>(wcslen(expected));
    return len == expectedLen &&
           CompareStringOrdinal(name->Buffer, len, expected, expectedLen, TRUE) == CSTR_EQUAL;
}

VOID CALLBACK OnDllNotification(ULONG reason, const DllNotificationData* data, PVOID) {
    for (size_t i = 0; i < g_count; ++i) {
        const ModuleCallbacks& c = g_callbacks[i];
        if (!NameMatches(data->baseDllName, c.baseName)) continue;
        if (reason == kReasonLoaded && c.loaded) {
            c.loaded(static_cast<HMODULE>(data->dllBase));
        } else if (reason == kReasonUnloaded && c.unloaded) {
            c.unloaded(data->dllBase, data->sizeOfImage);
        }
    }
}

}  // namespace

bool StartModuleWatch(const ModuleCallbacks* callbacks, size_t count) {
    g_callbacks = callbacks;
    g_count = count;

    const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    const auto registerFn =
        reinterpret_cast<RegisterFn>(GetProcAddress(ntdll, "LdrRegisterDllNotification"));
    const bool watching = registerFn && registerFn(0, OnDllNotification, nullptr, &g_cookie) >= 0;
    if (!watching) log::Write("graphics: DLL load notifications unavailable; late loads missed");

    for (size_t i = 0; i < count; ++i) {
        if (HMODULE m = GetModuleHandleW(callbacks[i].baseName)) callbacks[i].loaded(m);
    }
    return watching;
}

void StopModuleWatch() {
    if (!g_cookie) return;
    const auto unregisterFn = reinterpret_cast<UnregisterFn>(
        GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "LdrUnregisterDllNotification"));
    if (unregisterFn) unregisterFn(g_cookie);
    g_cookie = nullptr;
}

}  // namespace retro::shim::gfx
