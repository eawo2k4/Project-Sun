add_library(RetroShim SHARED
    dllmain.cpp
    DisplayContext.cpp
    DisplayContext.h
    Log.cpp
    Log.h
    ShimState.h
    RetroShim.def
    hooks/DisplayHooks.cpp
    hooks/Hooks.h
    hooks/MemoryHooks.cpp
    hooks/ProcessHooks.cpp
    hooks/RenderHooks.cpp
    hooks/StorageHooks.cpp)

target_include_directories(RetroShim PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(RetroShim PRIVATE RetroCore Detours::Detours)
