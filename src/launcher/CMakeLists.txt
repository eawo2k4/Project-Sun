add_executable(RetroLaunch
    main.cpp
    ProcessLauncher.cpp
    ProcessLauncher.h)

target_link_libraries(RetroLaunch PRIVATE RetroCore Detours::Detours)

# The launcher expects RetroShim.dll next to itself.
add_dependencies(RetroLaunch RetroShim)
