# RetroWin16: the 16-bit Windows execution engine (NE loader, virtual LDT,
# 286-class interpreter, built-in KERNEL/USER/GDI). Platform-neutral C++: no
# Windows headers, so it can be unit-tested on its own.
add_library(RetroWin16 STATIC
    Cpu.cpp
    Memory.cpp
    NeImage.cpp
    NeLoader.cpp
    Runtime.cpp
    include/win16/Cpu.h
    include/win16/Memory.h
    include/win16/NeImage.h
    include/win16/NeLoader.h
    include/win16/Runtime.h)

target_include_directories(RetroWin16 PUBLIC include)
