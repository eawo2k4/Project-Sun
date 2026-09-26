add_library(RetroShim SHARED
    dllmain.cpp
    DisplayContext.cpp
    DisplayContext.h
    Log.cpp
    Log.h
    Pacing.cpp
    Pacing.h
    ShimState.h
    RetroShim.def
    gfx/D3D8.cpp
    gfx/D3D9.cpp
    gfx/D3DCommon.h
    gfx/DDraw.cpp
    gfx/Graphics.cpp
    gfx/Graphics.h
    gfx/ModuleWatch.cpp
    gfx/ModuleWatch.h
    gfx/VtableHook.cpp
    gfx/VtableHook.h
    hooks/DisplayHooks.cpp
    hooks/Hooks.h
    hooks/MemoryHooks.cpp
    hooks/ProcessHooks.cpp
    hooks/RenderHooks.cpp
    hooks/StorageHooks.cpp)

target_include_directories(RetroShim PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
# dxguid: DirectDraw interface IDs. ddraw/d3d8/d3d9.dll themselves are never
# linked: the shim hooks them when (and if) the game loads them.
# d3d8to9: vendored D3D8 -> D3D9 bridge (third_party/d3d8to9), used with --d3d8to9.
target_link_libraries(RetroShim PRIVATE RetroCore Detours::Detours dxguid d3d8to9)
