cmake_minimum_required(VERSION 3.25)

project(RetroRunner
    VERSION 0.1.0
    DESCRIPTION "In-process compatibility runner for vintage Windows games"
    LANGUAGES CXX)

# Everything that gets injected into a game must match the game's bitness.
# Vintage games are 32-bit, so they run under WOW64 and the launcher, shim and
# Detours library must all be built as x86 (Win32).
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR
        "RetroRunner must be built for x86 (Win32) to target WOW64.\n"
        "Configure with: cmake --preset x86")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Static CRT: the shim is loaded into arbitrary game folders, so it must not
# depend on a VC++ runtime DLL being present. Must match the vcpkg triplet
# (x86-windows-static).
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# Put RetroLaunch.exe and RetroShim.dll side by side (the launcher finds the
# shim next to itself).
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

add_compile_definitions(
    UNICODE _UNICODE
    WIN32_LEAN_AND_MEAN NOMINMAX
    _WIN32_WINNT=0x0A00 WINVER=0x0A00)
add_compile_options(/W4 /permissive- /utf-8 /Zc:__cplusplus /MP)

# --- Microsoft Detours (from vcpkg) ------------------------------------------
# find_library alone would pick the release lib for Debug builds too, which
# fails with a /MT vs /MTd mismatch, so we locate both variants explicitly.
find_path(DETOURS_INCLUDE_DIR detours/detours.h REQUIRED)
get_filename_component(_detours_root "${DETOURS_INCLUDE_DIR}" DIRECTORY)
find_library(DETOURS_LIBRARY_RELEASE NAMES detours
    PATHS "${_detours_root}/lib" NO_DEFAULT_PATH REQUIRED)
find_library(DETOURS_LIBRARY_DEBUG NAMES detours
    PATHS "${_detours_root}/debug/lib" NO_DEFAULT_PATH)
if(NOT DETOURS_LIBRARY_DEBUG)
    set(DETOURS_LIBRARY_DEBUG "${DETOURS_LIBRARY_RELEASE}")
endif()

add_library(Detours::Detours INTERFACE IMPORTED)
target_include_directories(Detours::Detours INTERFACE "${DETOURS_INCLUDE_DIR}")
target_link_libraries(Detours::Detours INTERFACE
    "$<IF:$<CONFIG:Debug>,${DETOURS_LIBRARY_DEBUG},${DETOURS_LIBRARY_RELEASE}>")

add_subdirectory(third_party/d3d8to9)
add_subdirectory(src/core)
add_subdirectory(src/win16)
add_subdirectory(src/shim)
add_subdirectory(src/launcher)

include(CTest)
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
