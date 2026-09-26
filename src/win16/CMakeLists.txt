# RetroWin16: the 16-bit Windows execution engine (NE loader, virtual LDT,
# 286-class interpreter, built-in KERNEL/USER/GDI). Platform-neutral C++: no
# Windows headers, so it can be unit-tested on its own. Real windows come
# from a WindowHost the embedding application provides.
add_library(RetroWin16 STATIC
    Cpu.cpp
    Gdi.cpp
    Kernel.cpp
    Memory.cpp
    NeImage.cpp
    NeLoader.cpp
    Runtime.cpp
    User.cpp
    include/win16/Api.h
    include/win16/Cpu.h
    include/win16/Kernel.h
    include/win16/Memory.h
    include/win16/NeImage.h
    include/win16/NeLoader.h
    include/win16/Runtime.h
    include/win16/User.h)

target_include_directories(RetroWin16 PUBLIC include)
# RetroCore: DisplayMath (which 16-bit windows count as fullscreen).
target_link_libraries(RetroWin16 PUBLIC RetroCore)
