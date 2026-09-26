add_library(RetroCore STATIC
    ClampPolicy.cpp
    ExeFormat.cpp
    PathUtil.cpp
    include/retro/ClampPolicy.h
    include/retro/ExeFormat.h
    include/retro/PathUtil.h
    include/retro/ShimProtocol.h)

target_include_directories(RetroCore PUBLIC include)
