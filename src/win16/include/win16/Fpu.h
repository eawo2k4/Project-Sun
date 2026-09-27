#pragma once

// The x87 floating-point unit of the 16-bit interpreter.
//
// Windows programs built with Microsoft's floating-point emulator contain real
// x87 instructions: Windows only rewrites them into INT 34h-3Dh emulator calls
// (through the NE file's OS fixups) when the machine has no coprocessor. We
// report one (WF_80x87), so the instructions run here, on the Cpu (D8-DF
// opcodes; see CpuFpu.cpp). The stack registers are held as doubles: 80-bit
// loads and stores convert, and results carry double precision, which is what
// C programs of the time asked for anyway.
//
// Exceptions are always masked, the way the Windows C runtime sets up the
// control word: invalid operations give NaN, a division by zero infinity, and
// the status word records the exception flags.

#include <cstdint>

namespace retro::win16 {

namespace fpu {
// Status word.
constexpr uint16_t IE = 0x0001;  // invalid operation
constexpr uint16_t DE = 0x0002;  // denormal
constexpr uint16_t ZE = 0x0004;  // divide by zero
constexpr uint16_t OE = 0x0008;  // overflow
constexpr uint16_t UE = 0x0010;  // underflow
constexpr uint16_t PE = 0x0020;  // precision
constexpr uint16_t SF = 0x0040;  // stack fault
constexpr uint16_t C0 = 0x0100;
constexpr uint16_t C1 = 0x0200;
constexpr uint16_t C2 = 0x0400;
constexpr uint16_t C3 = 0x4000;
constexpr uint16_t kConditionMask = C0 | C1 | C2 | C3;
constexpr uint16_t kExceptionMask = 0x003F;

constexpr uint16_t kDefaultControl = 0x037F;  // FINIT: all masked, 64-bit precision, round to nearest
}  // namespace fpu

class Fpu {
public:
    Fpu() { Reset(); }

    void Reset();  // FINIT

    uint16_t control = fpu::kDefaultControl;
    // Status word without TOP (StatusWord() merges it in).
    uint16_t status = 0;

    uint16_t StatusWord() const;
    void SetStatusWord(uint16_t sw);  // also sets TOP
    uint16_t TagWord() const;
    void SetTagWord(uint16_t tw);

    int Top() const { return top_; }
    int Depth() const;  // registers in use

    bool IsEmpty(int i) const { return empty_[Phys(i)]; }
    double& St(int i) { return regs_[Phys(i)]; }
    // ST(i), or an invalid-operation NaN (and IE|SF) if that register is empty.
    double Get(int i);
    void Set(int i, double v);
    void Push(double v);  // stack overflow: IE|SF|C1, and the value is a NaN
    double Pop();
    void Free(int i) { empty_[Phys(i)] = true; }
    void IncTop() { top_ = (top_ + 1) & 7; }
    void DecTop() { top_ = (top_ + 7) & 7; }

    // Physical register r (0-7) for FSAVE/FRSTOR.
    double& Physical(int r) { return regs_[r & 7]; }
    bool PhysicalEmpty(int r) const { return empty_[r & 7]; }
    void SetPhysicalEmpty(int r, bool empty) { empty_[r & 7] = empty; }

    void Raise(uint16_t exceptions) { status |= exceptions; }

    // Compare a with b into C0/C2/C3 (FCOM, FTST, FUCOM). Unordered raises IE
    // unless `quiet`.
    void Compare(double a, double b, bool quiet = false);
    // FXAM on ST(0).
    void Examine();

    // Round to an integer with the control word's rounding mode.
    double RoundInt(double v) const;
    // FIST: the rounded value, or the integer indefinite (and IE) if out of range.
    int64_t ToInteger(double v, int bits);

    // 80-bit extended precision <-> double.
    static void ToExtended(double v, uint8_t out[10]);
    static double FromExtended(const uint8_t in[10]);
    // Packed BCD (FBLD/FBSTP), 18 digits.
    static double FromBcd(const uint8_t in[10]);
    void ToBcd(double v, uint8_t out[10]);

private:
    int Phys(int i) const { return (top_ + i) & 7; }

    double regs_[8] = {};
    bool empty_[8] = {};
    int top_ = 0;
};

}  // namespace retro::win16
