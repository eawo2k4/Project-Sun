// The x87 instructions of the 16-bit interpreter (opcodes D8-DF), and the
// INT 34h-3Dh forms Microsoft's floating-point emulator rewrites them into.
// FPU state and number conversions live in Fpu.cpp.

#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <numbers>

#include "win16/Cpu.h"

namespace retro::win16 {
namespace {

double Nan() { return -std::numeric_limits<double>::quiet_NaN(); }

std::string Hex2(unsigned v) {
    char buf[8];
    std::snprintf(buf, sizeof(buf), "%02X", v & 0xFF);
    return buf;
}

}  // namespace

// --- Memory operands -------------------------------------------------------------------------

void Cpu::ReadOperand(const ModRM& m, uint8_t* out, uint32_t size) {
    const uint16_t sel = regs_.s[m.seg];
    mem_.Translate(sel, m.ea, size, Access::Read);  // checks the whole operand first
    std::memcpy(out, mem_.SegmentData(sel) + m.ea, size);
}

void Cpu::WriteOperand(const ModRM& m, const uint8_t* in, uint32_t size) {
    const uint16_t sel = regs_.s[m.seg];
    mem_.Translate(sel, m.ea, size, Access::Write);
    std::memcpy(mem_.SegmentData(sel) + m.ea, in, size);
}

double Cpu::LoadReal(const ModRM& m, int bytes) {
    uint8_t b[10];
    ReadOperand(m, b, uint32_t(bytes));
    if (bytes == 4) {
        float f;
        std::memcpy(&f, b, 4);
        return f;
    }
    if (bytes == 8) {
        double d;
        std::memcpy(&d, b, 8);
        return d;
    }
    return Fpu::FromExtended(b);
}

void Cpu::StoreReal(const ModRM& m, int bytes, double v) {
    uint8_t b[10];
    if (bytes == 4) {
        const float f = static_cast<float>(v);
        if (std::isinf(f) && std::isfinite(v)) fpu_.Raise(fpu::OE | fpu::PE);
        std::memcpy(b, &f, 4);
    } else if (bytes == 8) {
        std::memcpy(b, &v, 8);
    } else {
        Fpu::ToExtended(v, b);
    }
    WriteOperand(m, b, uint32_t(bytes));
}

int64_t Cpu::LoadInt(const ModRM& m, int bytes) {
    uint8_t b[8] = {};
    ReadOperand(m, b, uint32_t(bytes));
    uint64_t u = 0;
    for (int i = 0; i < bytes; ++i) u |= uint64_t(b[i]) << (8 * i);
    if (bytes == 2) return int16_t(u);
    if (bytes == 4) return int32_t(u);
    return int64_t(u);
}

void Cpu::StoreInt(const ModRM& m, int bytes, double v) {
    const int64_t n = fpu_.ToInteger(v, bytes * 8);
    uint8_t b[8];
    for (int i = 0; i < bytes; ++i) b[i] = uint8_t(uint64_t(n) >> (8 * i));
    WriteOperand(m, b, uint32_t(bytes));
}

// FSTENV/FLDENV in 16-bit protected mode: CW, SW, TW, IP, CS, operand offset, operand selector.
void Cpu::StoreFpuEnv(const ModRM& m) {
    const uint16_t words[7] = {fpu_.control, fpu_.StatusWord(), fpu_.TagWord(), fpuIp_, fpuCs_, fpuOperand_,
                               fpuOperandSeg_};
    uint8_t b[14];
    for (int i = 0; i < 7; ++i) {
        b[2 * i] = uint8_t(words[i]);
        b[2 * i + 1] = uint8_t(words[i] >> 8);
    }
    WriteOperand(m, b, sizeof(b));
}

void Cpu::LoadFpuEnv(const ModRM& m) {
    uint8_t b[14];
    ReadOperand(m, b, sizeof(b));
    auto word = [&](int i) { return uint16_t(b[2 * i] | (b[2 * i + 1] << 8)); };
    fpu_.control = word(0);
    fpu_.SetStatusWord(word(1));
    fpu_.SetTagWord(word(2));
}

// --- Arithmetic ------------------------------------------------------------------------------

// The eight arithmetic operations of the reg field: ADD MUL COM COMP SUB SUBR DIV DIVR,
// with `a` the destination operand and `b` the source.
double Cpu::FpuArith(int op, double a, double b) {
    double r;
    bool zeroDivide = false;
    switch (op) {
    case 0: r = a + b; break;
    case 1: r = a * b; break;
    case 4: r = a - b; break;
    case 5: r = b - a; break;
    default: {  // 6 DIV, 7 DIVR
        const double n = op == 6 ? a : b, d = op == 6 ? b : a;
        zeroDivide = d == 0 && n != 0 && !std::isnan(n) && !std::isinf(n);
        r = n / d;
        break;
    }
    }
    if (zeroDivide) {
        fpu_.Raise(fpu::ZE);
    } else if (std::isnan(r) && !std::isnan(a) && !std::isnan(b)) {
        fpu_.Raise(fpu::IE);  // inf - inf, 0 * inf, 0 / 0, inf / inf
        r = Nan();
    } else if (std::isinf(r) && std::isfinite(a) && std::isfinite(b)) {
        fpu_.Raise(fpu::OE | fpu::PE);
    }
    return r;
}

// D8/DA/DC/DE with a memory operand, and D8 with a register: ST(0) op= src.
void Cpu::FpuArithST0(int op, double src) {
    const double st0 = fpu_.Get(0);
    if (op == 2 || op == 3) {  // FCOM / FCOMP
        fpu_.Compare(st0, src);
        if (op == 3) fpu_.Pop();
        return;
    }
    fpu_.Set(0, FpuArith(op, st0, src));
}

// --- Dispatch --------------------------------------------------------------------------------

void Cpu::Escape(uint8_t op) {
    const ModRM m = DecodeModRM();
    fpuCs_ = regs_.s[CS];
    fpuIp_ = startIp_;
    if (m.mod == 3) {
        EscapeRegister(op, m.reg, m.rm);
        return;
    }
    fpuOperand_ = m.ea;
    fpuOperandSeg_ = regs_.s[m.seg];
    EscapeMemory(op, m);
}

void Cpu::EscapeMemory(uint8_t op, const ModRM& m) {
    auto invalid = [&]() {
        Throw(FaultKind::InvalidOpcode, "x87 instruction " + Hex2(op) + " /" + std::to_string(m.reg) +
                                            " (memory form) is not implemented");
    };
    switch (op) {
    case 0xD8: FpuArithST0(m.reg, LoadReal(m, 4)); return;
    case 0xDC: FpuArithST0(m.reg, LoadReal(m, 8)); return;
    case 0xDA: FpuArithST0(m.reg, double(LoadInt(m, 4))); return;
    case 0xDE: FpuArithST0(m.reg, double(LoadInt(m, 2))); return;

    case 0xD9:
        switch (m.reg) {
        case 0: fpu_.Push(LoadReal(m, 4)); return;
        case 2: StoreReal(m, 4, fpu_.Get(0)); return;
        case 3: StoreReal(m, 4, fpu_.Get(0)); fpu_.Pop(); return;
        case 4: LoadFpuEnv(m); return;                                    // FLDENV
        case 5: fpu_.control = uint16_t(LoadInt(m, 2)); return;           // FLDCW
        case 6: StoreFpuEnv(m); fpu_.control |= fpu::kExceptionMask; return;  // FNSTENV
        case 7: {                                                         // FNSTCW
            const uint8_t b[2] = {uint8_t(fpu_.control), uint8_t(fpu_.control >> 8)};
            WriteOperand(m, b, 2);
            return;
        }
        default: invalid();
        }
    case 0xDB:
        switch (m.reg) {
        case 0: fpu_.Push(double(LoadInt(m, 4))); return;
        case 2: StoreInt(m, 4, fpu_.Get(0)); return;
        case 3: StoreInt(m, 4, fpu_.Get(0)); fpu_.Pop(); return;
        case 5: fpu_.Push(LoadReal(m, 10)); return;
        case 7: StoreReal(m, 10, fpu_.Get(0)); fpu_.Pop(); return;
        default: invalid();
        }
    case 0xDD:
        switch (m.reg) {
        case 0: fpu_.Push(LoadReal(m, 8)); return;
        case 2: StoreReal(m, 8, fpu_.Get(0)); return;
        case 3: StoreReal(m, 8, fpu_.Get(0)); fpu_.Pop(); return;
        case 4: {  // FRSTOR: environment, then ST(0)..ST(7)
            LoadFpuEnv(m);
            uint8_t b[80];
            ModRM regs = m;
            regs.ea = uint16_t(m.ea + 14);
            ReadOperand(regs, b, sizeof(b));
            for (int i = 0; i < 8; ++i) fpu_.St(i) = Fpu::FromExtended(b + 10 * i);
            return;
        }
        case 6: {  // FNSAVE, then FINIT
            StoreFpuEnv(m);
            uint8_t b[80];
            for (int i = 0; i < 8; ++i) Fpu::ToExtended(fpu_.St(i), b + 10 * i);
            ModRM regs = m;
            regs.ea = uint16_t(m.ea + 14);
            WriteOperand(regs, b, sizeof(b));
            fpu_.Reset();
            return;
        }
        case 7: {  // FNSTSW m16
            const uint16_t sw = fpu_.StatusWord();
            const uint8_t b[2] = {uint8_t(sw), uint8_t(sw >> 8)};
            WriteOperand(m, b, 2);
            return;
        }
        default: invalid();
        }
    case 0xDF:
        switch (m.reg) {
        case 0: fpu_.Push(double(LoadInt(m, 2))); return;
        case 2: StoreInt(m, 2, fpu_.Get(0)); return;
        case 3: StoreInt(m, 2, fpu_.Get(0)); fpu_.Pop(); return;
        case 4: {  // FBLD
            uint8_t b[10];
            ReadOperand(m, b, sizeof(b));
            fpu_.Push(Fpu::FromBcd(b));
            return;
        }
        case 5: fpu_.Push(double(LoadInt(m, 8))); return;
        case 6: {  // FBSTP
            uint8_t b[10];
            fpu_.ToBcd(fpu_.Get(0), b);
            WriteOperand(m, b, sizeof(b));
            fpu_.Pop();
            return;
        }
        case 7: StoreInt(m, 8, fpu_.Get(0)); fpu_.Pop(); return;
        default: invalid();
        }
    default: invalid();
    }
}

void Cpu::EscapeRegister(uint8_t op, uint8_t reg, uint8_t i) {
    auto invalid = [&]() {
        Throw(FaultKind::InvalidOpcode,
              "x87 instruction " + Hex2(op) + " " + Hex2(0xC0 | (reg << 3) | i) + " is not implemented");
    };
    Fpu& f = fpu_;
    switch (op) {
    case 0xD8: FpuArithST0(reg, f.Get(i)); return;
    case 0xDC:
    case 0xDE: {  // ST(i) op= ST(0); the SUB/SUBR and DIV/DIVR encodings are swapped here
        const double st0 = f.Get(0);
        if (reg == 2 || reg == 3) {
            // DC D0+i/D8+i: FCOM/FCOMP aliases. DE D0+i: FCOMP alias; DE D9: FCOMPP.
            if (op == 0xDE && reg == 3 && i != 1) invalid();
            f.Compare(st0, f.Get(i));
            const int pops = op == 0xDC ? reg - 2 : reg - 1;
            for (int k = 0; k < pops; ++k) f.Pop();
            return;
        }
        const int arith = reg >= 4 ? (reg ^ 1) : reg;
        f.Set(i, FpuArith(arith, f.Get(i), st0));
        if (op == 0xDE) f.Pop();
        return;
    }
    case 0xDD:
        switch (reg) {
        case 0: f.Free(i); return;  // FFREE
        case 1: break;              // FXCH alias
        case 2: f.Set(i, f.Get(0)); return;
        case 3: f.Set(i, f.Get(0)); f.Pop(); return;
        case 4: f.Compare(f.Get(0), f.Get(i), true); return;  // FUCOM
        case 5: f.Compare(f.Get(0), f.Get(i), true); f.Pop(); return;
        default: invalid();
        }
        [[fallthrough]];
    case 0xD9:
        switch (reg) {
        case 0: f.Push(f.Get(i)); return;  // FLD ST(i)
        case 1: {                          // FXCH
            const double a = f.Get(0), b = f.Get(i);
            f.Set(0, b);
            f.Set(i, a);
            return;
        }
        case 2:
            if (i != 0) invalid();
            return;  // FNOP
        case 3: f.Set(i, f.Get(0)); f.Pop(); return;  // FSTP ST(i) alias
        case 4:
            switch (i) {
            case 0: f.Set(0, -f.Get(0)); return;             // FCHS
            case 1: f.Set(0, std::fabs(f.Get(0))); return;   // FABS
            case 4: f.Compare(f.Get(0), 0.0); return;        // FTST
            case 5: f.Examine(); return;                     // FXAM
            default: invalid();
            }
        case 5: {
            static const double kConstants[7] = {1.0,
                                                 std::numbers::log2e / std::numbers::log10e,  // log2(10)
                                                 std::numbers::log2e,
                                                 std::numbers::pi,
                                                 std::numbers::ln2 * std::numbers::log10e,  // log10(2)
                                                 std::numbers::ln2,
                                                 0.0};
            if (i == 7) invalid();
            f.Push(kConstants[i]);
            return;
        }
        case 6: FpuFunction(i); return;
        default: FpuFunction(8 + i); return;
        }
    case 0xDA:
        if (reg == 5 && i == 1) {  // FUCOMPP
            f.Compare(f.Get(0), f.Get(1), true);
            f.Pop();
            f.Pop();
            return;
        }
        invalid();
    case 0xDB:
        if (reg != 4) invalid();
        switch (i) {
        case 0:  // FENI
        case 1:  // FDISI
        case 4:  // FSETPM
            return;
        case 2: f.status &= ~(fpu::kExceptionMask | fpu::SF | 0x8000); return;  // FCLEX
        case 3: f.Reset(); return;                                             // FINIT
        default: invalid();
        }
    case 0xDF:
        if (reg == 4 && i == 0) {  // FNSTSW AX
            regs_.r[AX] = f.StatusWord();
            return;
        }
        if (reg == 0) {  // FFREEP
            f.Free(i);
            f.IncTop();
            return;
        }
        invalid();
    default: invalid();
    }
}

// D9 F0-FF: the transcendental and housekeeping functions.
void Cpu::FpuFunction(int n) {
    Fpu& f = fpu_;
    constexpr double kLn2 = std::numbers::ln2;
    auto checked = [&](double r, double x) {
        if (std::isnan(r) && !std::isnan(x)) {
            f.Raise(fpu::IE);
            return Nan();
        }
        return r;
    };
    switch (n) {
    case 0: {  // F2XM1
        const double x = f.Get(0);
        f.Set(0, std::expm1(x * kLn2));
        return;
    }
    case 1: {  // FYL2X
        const double x = f.Get(0), y = f.Get(1);
        if (x == 0 && y != 0 && !std::isnan(y)) f.Raise(fpu::ZE);
        f.Set(1, checked(y * std::log2(x), x));
        f.Pop();
        return;
    }
    case 2: {  // FPTAN
        const double x = f.Get(0);
        f.Set(0, checked(std::tan(x), x));
        f.Push(1.0);
        f.status &= ~fpu::C2;
        return;
    }
    case 3: {  // FPATAN
        const double x = f.Get(0), y = f.Get(1);
        f.Set(1, std::atan2(y, x));
        f.Pop();
        return;
    }
    case 4: {  // FXTRACT
        const double v = f.Get(0);
        if (v == 0) {
            f.Raise(fpu::ZE);
            f.Set(0, -std::numeric_limits<double>::infinity());
            f.Push(v);
            return;
        }
        if (!std::isfinite(v)) {
            f.Set(0, std::isinf(v) ? std::numeric_limits<double>::infinity() : v);
            f.Push(v);
            return;
        }
        int e = 0;
        const double significand = std::frexp(v, &e);  // [0.5, 1)
        f.Set(0, double(e - 1));
        f.Push(significand * 2);
        return;
    }
    case 5:    // FPREM1
    case 8: {  // FPREM
        const double x = f.Get(0), y = f.Get(1);
        f.status &= ~fpu::kConditionMask;
        if (y == 0 || std::isinf(x) || std::isnan(x) || std::isnan(y)) {
            f.Raise(fpu::IE);
            f.Set(0, Nan());
            return;
        }
        const double q = n == 8 ? std::trunc(x / y) : std::nearbyint(x / y);
        const double r = n == 8 ? std::fmod(x, y) : std::remainder(x, y);
        const uint64_t bits = uint64_t(std::fmod(std::fabs(q), 8.0));
        if (bits & 1) f.status |= fpu::C1;
        if (bits & 2) f.status |= fpu::C3;
        if (bits & 4) f.status |= fpu::C0;
        f.Set(0, r);
        return;
    }
    case 6: f.DecTop(); return;  // FDECSTP
    case 7: f.IncTop(); return;  // FINCSTP
    case 9: {                    // FYL2XP1
        const double x = f.Get(0), y = f.Get(1);
        f.Set(1, checked(y * std::log1p(x) / kLn2, x));
        f.Pop();
        return;
    }
    case 10: {  // FSQRT
        const double x = f.Get(0);
        f.Set(0, checked(std::sqrt(x), x));
        return;
    }
    case 11: {  // FSINCOS
        const double x = f.Get(0);
        f.Set(0, checked(std::sin(x), x));
        f.Push(checked(std::cos(x), x));
        f.status &= ~fpu::C2;
        return;
    }
    case 12: {  // FRNDINT
        const double x = f.Get(0);
        const double r = f.RoundInt(x);
        if (r != x && std::isfinite(x)) f.Raise(fpu::PE);
        f.Set(0, r);
        return;
    }
    case 13: {  // FSCALE
        const double x = f.Get(0), s = std::trunc(f.Get(1));
        const double clamped = std::fmax(-100000.0, std::fmin(100000.0, s));
        f.Set(0, std::isnan(s) ? Nan() : std::ldexp(x, int(clamped)));
        return;
    }
    case 14:    // FSIN
    case 15: {  // FCOS
        const double x = f.Get(0);
        f.Set(0, checked(n == 14 ? std::sin(x) : std::cos(x), x));
        f.status &= ~fpu::C2;
        return;
    }
    default:
        Throw(FaultKind::InvalidOpcode, "x87 function D9 " + Hex2(0xF0 + n));
    }
}

// Microsoft's emulator form: INT 34h-3Bh is FWAIT + ESC D8-DF, INT 3Ch the
// same with a segment override (the next byte: the override in its top two
// bits, SS DS CS ES, and the ESC opcode in the low three), INT 3Dh a lone FWAIT.
bool Cpu::EmulatorInterrupt(uint8_t vector) {
    if (vector >= 0x34 && vector <= 0x3B) {
        Escape(uint8_t(0xD8 + vector - 0x34));
        return true;
    }
    if (vector == 0x3C) {
        const uint8_t b = Fetch8();
        static const uint8_t kSegments[4] = {SS, DS, CS, ES};
        segOverride_ = kSegments[b >> 6];
        Escape(uint8_t(0xD8 | (b & 7)));
        return true;
    }
    return vector == 0x3D;
}

}  // namespace retro::win16
