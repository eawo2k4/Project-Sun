add_executable(RetroLaunch
    main.cpp
    RetroLaunch.rc
    ProcessLauncher.cpp
    ProcessLauncher.h
    Win16Host.cpp
    Win16Host.h)

target_link_libraries(RetroLaunch PRIVATE RetroCore RetroWin16 Detours::Detours)

# The launcher expects RetroShim.dll next to itself.
add_dependencies(RetroLaunch RetroShim)
