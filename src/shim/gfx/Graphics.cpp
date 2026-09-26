#include "Graphics.h"

#include <cstring>
#include <iterator>

#include "../Log.h"
#include "../hooks/Hooks.h"
#include "ModuleWatch.h"
#include "VtableHook.h"

namespace retro::shim {
namespace {

const gfx::ModuleCallbacks kWatched[] = {
    {L"ddraw.dll", gfx::ddraw::OnLoaded, gfx::ddraw::OnUnloaded},
    {L"d3d8.dll", gfx::d3d8::OnLoaded, gfx::d3d8::OnUnloaded},
    {L"d3d9.dll", gfx::d3d9::OnLoaded, gfx::d3d9::OnUnloaded},
};

}  // namespace

LONG AttachGraphicsHooks() {
    log::Write("graphics: watching ddraw.dll, d3d8.dll and d3d9.dll");
    gfx::StartModuleWatch(kWatched, std::size(kWatched));
    return NO_ERROR;
}

LONG DetachGraphicsHooks() {
    gfx::StopModuleWatch();
    gfx::RestoreAllVtables();
    LONG err = gfx::ddraw::Unhook();
    if (err == NO_ERROR) err = gfx::d3d8::Unhook();
    if (err == NO_ERROR) err = gfx::d3d9::Unhook();
    return err;
}

namespace gfx {

bool IsApiHooked(const char* name) {
    if (std::strcmp(name, "ddraw") == 0) return ddraw::Hooked();
    if (std::strcmp(name, "d3d8") == 0) return d3d8::Hooked();
    if (std::strcmp(name, "d3d9") == 0) return d3d9::Hooked();
    return false;
}

void PresentPendingFrames() { ddraw::PresentPending(); }

void GetPresentStats(PresentStats& out) {
    ddraw::GetStats(out);
    d3d8::AddStats(out);
    d3d9::AddStats(out);
}

}  // namespace gfx
}  // namespace retro::shim
