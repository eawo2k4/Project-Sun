#pragma once

// Plumbing shared by the built-in system DLLs (Kernel.cpp, User.cpp, Gdi.cpp).
//
// A built-in API function reads its Pascal-convention arguments off the 16-bit
// stack (the last argument is nearest the return address, at StackArg(0)),
// does its work, sets AX (or DX:AX) and returns with Cpu::ReturnFar(argBytes).

#include <cstdint>
#include <initializer_list>
#include <vector>

#include "win16/Cpu.h"

namespace retro::win16 {

class Runtime;

struct ApiFunction {
    uint16_t ordinal;
    const char* name;  // upper case, as imported by name
    void (*impl)(Runtime&, Cpu&);
};

std::vector<ApiFunction> KernelApi();
std::vector<ApiFunction> UserApi();
std::vector<ApiFunction> GdiApi();
std::vector<ApiFunction> MmsystemApi();
std::vector<ApiFunction> ToolhelpApi();  // Kernel.cpp
std::vector<ApiFunction> KeyboardApi();  // Charset.cpp
std::vector<ApiFunction> UserCharsetApi();  // Charset.cpp: AnsiUpper, IsCharAlpha, ...
std::vector<ApiFunction> KernelAtomApi();  // Atoms.cpp: local atoms
std::vector<ApiFunction> UserAtomApi();    // Atoms.cpp: global atoms, window properties
// Parts of the USER and GDI tables, by source file.
std::vector<ApiFunction> UserWindowApi();  // UserWindow.cpp
std::vector<ApiFunction> MenuApi();        // Menus.cpp
std::vector<ApiFunction> UserDrawApi();    // GdiDraw.cpp: DrawText, FrameRect, ...
std::vector<ApiFunction> UserSoundApi();   // Sound.cpp: MessageBeep
std::vector<ApiFunction> GdiDrawApi();     // GdiDraw.cpp

// Stops sndPlaySound's sound (before its buffer goes away).
void StopHostSound();

// A 16:16 far pointer argument.
struct FarPtr {
    uint16_t off = 0;
    uint16_t sel = 0;
    bool IsNull() const { return off == 0 && sel == 0; }
};

inline FarPtr ArgPtr(const Cpu& cpu, uint16_t at) { return {cpu.StackArg(at), cpu.StackArg(uint16_t(at + 2))}; }
inline uint32_t ArgLong(const Cpu& cpu, uint16_t at) {
    return cpu.StackArg(at) | (uint32_t(cpu.StackArg(uint16_t(at + 2))) << 16);
}
inline int16_t ArgInt(const Cpu& cpu, uint16_t at) { return int16_t(cpu.StackArg(at)); }

// Pascal arguments by position, from the declaration's parameter sizes (2 for
// WORD/int/handles, 4 for LONG/COLORREF/far pointers), so offsets are never
// computed by hand. Parameter 0 is the first declared (the deepest on the stack).
//   PascalArgs a(cpu, {2, 2, 2, 2, 4});   // (HDC, x, y, w, COLORREF)
class PascalArgs {
public:
    PascalArgs(const Cpu& cpu, std::initializer_list<uint8_t> sizes) : cpu_(cpu) {
        uint16_t off = 0;
        std::vector<uint8_t> s(sizes);
        offsets_.resize(s.size());
        for (size_t i = s.size(); i-- > 0;) {
            offsets_[i] = off;
            off = uint16_t(off + s[i]);
        }
        bytes_ = off;
    }
    uint16_t Word(size_t i) const { return cpu_.StackArg(offsets_[i]); }
    int16_t Int(size_t i) const { return int16_t(Word(i)); }
    uint32_t Long(size_t i) const { return ArgLong(cpu_, offsets_[i]); }
    FarPtr Ptr(size_t i) const { return ArgPtr(cpu_, offsets_[i]); }
    uint16_t Bytes() const { return bytes_; }  // for ReturnFar

private:
    const Cpu& cpu_;
    std::vector<uint16_t> offsets_;
    uint16_t bytes_ = 0;
};

inline void SetResult(Cpu& cpu, uint32_t value) {
    cpu.Regs().r[AX] = uint16_t(value);
    cpu.Regs().r[DX] = uint16_t(value >> 16);
}

}  // namespace retro::win16
