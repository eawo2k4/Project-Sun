#pragma once

// COM method interception by vtable patching.
//
// DirectDraw and Direct3D objects are reached through interface pointers
// whose methods live in shared, per-interface-version vtables inside
// ddraw.dll / d3d9.dll. Patching a vtable slot intercepts that method for
// every object of that interface version, and only that version: unlike
// inline (Detours) hooks it is unaffected by several versions sharing one
// implementation function. Vtables are patched lazily, the first time an
// object using them is seen.

#include <windows.h>

#include <cstdint>
#include <initializer_list>

namespace retro::shim::gfx {

struct SlotHook {
    int slot;
    void* hook;
};

// Patches the vtable of `object` with `hooks`, once per distinct vtable
// (later calls for objects sharing it are no-ops). Returns false on failure.
bool PatchVtable(void* object, std::initializer_list<SlotHook> hooks);

// The pre-patch function in `slot` of `object`'s vtable, or nullptr.
void* OriginalSlot(void* object, int slot);

template <class Fn>
Fn Original(void* object, int slot) {
    return reinterpret_cast<Fn>(OriginalSlot(object, slot));
}

// Forget vtables inside [base, base + size): the module was unloaded, and a
// reload at the same address brings back fresh, unpatched vtables.
void ForgetVtablesInRange(const void* base, size_t size);

// Restores every patched slot (shim unload).
void RestoreAllVtables();

// Set while the shim itself calls into DirectDraw/Direct3D, so hooked
// methods pass straight through to the originals.
class InternalCall {
public:
    InternalCall() { ++depth_; }
    ~InternalCall() { --depth_; }
    InternalCall(const InternalCall&) = delete;
    InternalCall& operator=(const InternalCall&) = delete;
    static bool Active() { return depth_ > 0; }

private:
    static thread_local int depth_;
};

}  // namespace retro::shim::gfx
