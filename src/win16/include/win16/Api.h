#pragma once

// Plumbing shared by the built-in system DLLs (Kernel.cpp, User.cpp, Gdi.cpp).
//
// A built-in API function reads its Pascal-convention arguments off the 16-bit
// stack (the last argument is nearest the return address, at StackArg(0)),
// does its work, sets AX (or DX:AX) and returns with Cpu::ReturnFar(argBytes).

#include <cstdint>
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

inline void SetResult(Cpu& cpu, uint32_t value) {
    cpu.Regs().r[AX] = uint16_t(value);
    cpu.Regs().r[DX] = uint16_t(value >> 16);
}

}  // namespace retro::win16
