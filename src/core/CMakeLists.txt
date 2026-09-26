add_library(RetroCore STATIC
    ClampPolicy.cpp
    DisplayMath.cpp
    ExeFormat.cpp
    FramePacing.cpp
    PathUtil.cpp
    PixelConvert.cpp
    ShellIntegration.cpp
    include/retro/ClampPolicy.h
    include/retro/DisplayMath.h
    include/retro/ExeFormat.h
    include/retro/FramePacing.h
    include/retro/PathUtil.h
    include/retro/PixelConvert.h
    include/retro/ShellIntegration.h
    include/retro/ShimProtocol.h)

target_include_directories(RetroCore PUBLIC include)
# ShellIntegration: the registry (advapi32) and SHChangeNotify (shell32).
target_link_libraries(RetroCore PUBLIC advapi32 shell32)
