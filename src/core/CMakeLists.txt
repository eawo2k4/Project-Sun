add_library(RetroCore STATIC
    ClampPolicy.cpp
    DisplayMath.cpp
    ExeFormat.cpp
    FramePacing.cpp
    PathUtil.cpp
    include/retro/ClampPolicy.h
    include/retro/DisplayMath.h
    include/retro/ExeFormat.h
    include/retro/FramePacing.h
    include/retro/PathUtil.h
    include/retro/ShimProtocol.h)

target_include_directories(RetroCore PUBLIC include)
