#pragma once

// Hook modules. Each Attach/Detach function runs inside a Detours transaction
// opened by the caller (dllmain.cpp), one transaction per module, so one module
// failing to attach doesn't take the others down with it.

#include <windows.h>

#include <detours/detours.h>

namespace retro::shim {

LONG AttachStorageHooks();  // GetDiskFreeSpace(Ex)A/W
LONG DetachStorageHooks();

LONG AttachMemoryHooks();   // GlobalMemoryStatus(Ex)
LONG DetachMemoryHooks();

LONG AttachProcessHooks();  // CreateProcessA/W -> propagate shim to children
LONG DetachProcessHooks();

LONG AttachDisplayHooks();  // display modes, window sandbox, cursor, input coordinates
LONG DetachDisplayHooks();

LONG AttachRenderHooks();   // DC scaling + presentation frame pacing
LONG DetachRenderHooks();

// Type-checked wrappers: `hook` must have exactly the signature (including
// calling convention) of the function `real` points at.
template <class Fn>
LONG AttachHook(Fn*& real, Fn* hook) {
    return DetourAttach(reinterpret_cast<PVOID*>(&real), reinterpret_cast<PVOID>(hook));
}

template <class Fn>
LONG DetachHook(Fn*& real, Fn* hook) {
    return DetourDetach(reinterpret_cast<PVOID*>(&real), reinterpret_cast<PVOID>(hook));
}

// One entry of a module's hook table, usable for both directions so a module
// lists its hooks once: HookStep(err, attach, Real_X, Hook_X) for each hook.
// Stops at the first error.
template <class Fn>
void HookStep(LONG& err, bool attach, Fn*& real, Fn* hook) {
    if (err == NO_ERROR) err = attach ? AttachHook(real, hook) : DetachHook(real, hook);
}

// Returns true exactly once per flag; used to log the first call to each hook
// without spamming the log from hot paths.
inline bool FirstCall(volatile LONG& flag) {
    return InterlockedExchange(&flag, 1) == 0;
}

}  // namespace retro::shim
