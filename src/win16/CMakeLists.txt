# RetroWin16: the 16-bit Windows execution engine (NE loader, virtual LDT,
# 286-class interpreter, built-in KERNEL/USER/GDI/MMSYSTEM).
#
# The CPU, loader, memory, KERNEL and USER are platform-neutral C++. GDI
# (Gdi.cpp, GdiDraw.cpp) draws with the host's Win32 GDI, sound (Sound.cpp)
# plays through winmm, and real windows come from a WindowHost the embedding
# application provides.
add_library(RetroWin16 STATIC
    ApiCatalog.cpp
    CatalogLookup.cpp
    Cpu.cpp
    Files.cpp
    Gdi.cpp
    Kernel.cpp
    GdiDraw.cpp
    LocalHeap.cpp
    Memory.cpp
    Menus.cpp
    NeImage.cpp
    NeLoader.cpp
    Resources.cpp
    Runtime.cpp
    Sound.cpp
    User.cpp
    UserWindow.cpp
    include/win16/Api.h
    include/win16/ApiCatalog.h
    include/win16/Cpu.h
    include/win16/Files.h
    include/win16/Gdi.h
    include/win16/Kernel.h
    include/win16/LocalHeap.h
    include/win16/Menus.h
    include/win16/Memory.h
    include/win16/NeImage.h
    include/win16/NeLoader.h
    include/win16/Resources.h
    include/win16/Runtime.h
    include/win16/User.h)

target_include_directories(RetroWin16 PUBLIC include)
# RetroCore: DisplayMath (fullscreen detection), FramePacing (present pacing).
target_link_libraries(RetroWin16 PUBLIC RetroCore gdi32 winmm)
