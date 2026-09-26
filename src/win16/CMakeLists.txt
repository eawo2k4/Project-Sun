# RetroWin16: the 16-bit Windows execution engine (NE loader, virtual LDT,
# 286-class interpreter, built-in KERNEL/USER/GDI).
#
# The CPU, loader, memory, KERNEL and USER are platform-neutral C++. GDI
# (Gdi.cpp) draws with the host's Win32 GDI, and real windows come from a
# WindowHost the embedding application provides.
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
    include/win16/Gdi.h
    include/win16/Kernel.h
    include/win16/Memory.h
    include/win16/NeImage.h
    include/win16/NeLoader.h
    include/win16/Runtime.h
    include/win16/User.h)

target_include_directories(RetroWin16 PUBLIC include)
# RetroCore: DisplayMath (fullscreen detection), FramePacing (present pacing).
target_link_libraries(RetroWin16 PUBLIC RetroCore gdi32)
