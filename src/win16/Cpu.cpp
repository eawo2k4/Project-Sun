#include "win16/Cpu.h"

#include <algorithm>
#include <cstdio>

namespace retro::win16 {
namespace {

// Faults raised inside an instruction; Run() turns them into CpuFault.
struct CpuException {
    FaultKind kind;
    std::string detail;
};

bool EvenParity(uint8_t v) {
    v ^= v >> 4;
    v ^= v >> 2;
    v ^= v >> 1;
    return (v & 1) == 0;
}

std::string Hex(unsigned v, int digits) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%0*X", digits, v);
    return buf;
}

}  // namespace

Cpu::Cpu(Memory& memory) : mem_(memory) {
    returnTrap_ = mem_.Allocate(0x10000, SegmentKind::Host);
}

void Cpu::MapHostSegment(uint16_t selector, HostHandler handler) {
    host_.push_back({selector, std::move(handler)});
}

void Cpu::LoadSegment(SegReg reg, uint16_t selector) {
    if (Memory::IsNull(selector)) {
        if (reg == CS || reg == SS) throw ProtectionFault("null selector loaded into CS/SS", selector, 0);
        regs_.s[reg] = selector;  // allowed for DS/ES; using it faults
        return;
    }
    const Descriptor* d = mem_.Lookup(selector);
    if (!d) throw ProtectionFault("invalid selector " + Hex(selector, 4), selector, 0);
    if (reg == CS && d->kind == SegmentKind::Data)
        throw ProtectionFault("CS loaded with data selector " + Hex(selector, 4), selector, 0);
    if (reg == SS && d->kind != SegmentKind::Data)
        throw ProtectionFault("SS loaded with non-writable selector " + Hex(selector, 4), selector, 0);
    if ((reg == DS || reg == ES) && d->kind == SegmentKind::Host)
        throw ProtectionFault("data register loaded with host selector " + Hex(selector, 4), selector, 0);
    regs_.s[reg] = selector;
}

RunResult Cpu::Run(uint64_t budget) {
    stop_ = false;
    budget_ = budget;
    try {
        while (!stop_) {
            if (budget_ == 0) return RunResult::BudgetExhausted;
            Tick();
        }
    } catch (const Unwind& u) {
        callDepth_ = 0;
        returnedDepth_ = 0;
        return u.result;
    }
    return RunResult::Stopped;
}

void Cpu::Tick() {
    // A callback returned to CallFar's trap: nothing to execute here.
    if (regs_.s[CS] == returnTrap_) {
        returnedDepth_ = regs_.ip;
        return;
    }

    // Control arrived in a host segment: run the C++ routine for it.
    const auto host = std::find_if(host_.begin(), host_.end(),
                                   [&](const HostSegment& h) { return h.selector == regs_.s[CS]; });
    if (host != host_.end()) {
        const uint16_t cs = regs_.s[CS], ip = regs_.ip;
        try {
            host->handler(*this, ip);
        } catch (const CpuException& e) {
            fault_ = {e.kind, cs, ip, e.detail};
            throw Unwind{RunResult::Faulted};
        } catch (const ProtectionFault& e) {
            fault_ = {FaultKind::GeneralProtection, cs, ip, e.what()};
            throw Unwind{RunResult::Faulted};
        }
        if (!stop_ && regs_.s[CS] == cs && regs_.ip == ip) {
            fault_ = {FaultKind::HostError, cs, ip, "host routine did not return"};
            throw Unwind{RunResult::Faulted};
        }
        return;
    }

    startIp_ = regs_.ip;
    try {
        Step();
    } catch (const ProtectionFault& e) {
        regs_.ip = startIp_;
        fault_ = {FaultKind::GeneralProtection, regs_.s[CS], startIp_, e.what()};
        throw Unwind{RunResult::Faulted};
    } catch (const CpuException& e) {
        regs_.ip = startIp_;
        fault_ = {e.kind, regs_.s[CS], startIp_, e.detail};
        throw Unwind{RunResult::Faulted};
    }
    ++instructions_;
    --budget_;
}

uint32_t Cpu::CallFar(uint16_t selector, uint16_t offset, std::initializer_list<uint16_t> args,
                      uint16_t ds, const std::function<void(Cpu&)>& setup) {
    const Registers saved = regs_;
    for (uint16_t w : args) Push(w);
    const int depth = ++callDepth_;
    Push(returnTrap_);
    Push(uint16_t(depth));  // the trap reads the depth back from IP
    LoadSegment(DS, ds);
    regs_.r[AX] = ds;
    if (setup) setup(*this);
    FarJump(selector, offset);

    while (returnedDepth_ != depth) {
        if (stop_) throw Unwind{RunResult::Stopped};  // task exited inside the callback
        if (budget_ == 0) throw Unwind{RunResult::BudgetExhausted};
        Tick();
    }
    returnedDepth_ = 0;
    --callDepth_;

    const uint32_t result = regs_.r[AX] | (uint32_t(regs_.r[DX]) << 16);
    regs_ = saved;
    return result;
}

void Cpu::Throw(FaultKind kind, const std::string& detail) { throw CpuException{kind, detail}; }

void Cpu::HostFault(const std::string& detail) { Throw(FaultKind::HostError, detail); }

// --- Stack --------------------------------------------------------------------------------

void Cpu::Push(uint16_t value) {
    regs_.r[SP] = static_cast<uint16_t>(regs_.r[SP] - 2);
    Write16(SS, regs_.r[SP], value);
}

uint16_t Cpu::Pop() {
    const uint16_t v = Read16(SS, regs_.r[SP]);
    regs_.r[SP] = static_cast<uint16_t>(regs_.r[SP] + 2);
    return v;
}

uint16_t Cpu::StackArg(uint16_t byteOffset) const {
    return mem_.Read16(regs_.s[SS], static_cast<uint16_t>(regs_.r[SP] + 4 + byteOffset));
}

void Cpu::ReturnFar(uint16_t argBytes) {
    const uint16_t ip = Pop();
    const uint16_t cs = Pop();
    LoadSegment(CS, cs);
    regs_.ip = ip;
    regs_.r[SP] = static_cast<uint16_t>(regs_.r[SP] + argBytes);
}

// --- Fetch and decode ----------------------------------------------------------------------

uint8_t Cpu::Fetch8() {
    const uint8_t v = mem_.Read8(regs_.s[CS], regs_.ip, Access::Execute);
    regs_.ip = static_cast<uint16_t>(regs_.ip + 1);
    return v;
}

uint16_t Cpu::Fetch16() {
    const uint16_t v = mem_.Read16(regs_.s[CS], regs_.ip, Access::Execute);
    regs_.ip = static_cast<uint16_t>(regs_.ip + 2);
    return v;
}

Cpu::ModRM Cpu::DecodeModRM() {
    ModRM m;
    const uint8_t b = Fetch8();
    m.mod = b >> 6;
    m.reg = (b >> 3) & 7;
    m.rm = b & 7;
    if (m.mod == 3) return m;

    const uint16_t* r = regs_.r;
    uint16_t ea = 0;
    uint8_t seg = DS;
    switch (m.rm) {
    case 0: ea = uint16_t(r[BX] + r[SI]); break;
    case 1: ea = uint16_t(r[BX] + r[DI]); break;
    case 2: ea = uint16_t(r[BP] + r[SI]); seg = SS; break;
    case 3: ea = uint16_t(r[BP] + r[DI]); seg = SS; break;
    case 4: ea = r[SI]; break;
    case 5: ea = r[DI]; break;
    case 6:
        if (m.mod == 0) {
            ea = Fetch16();  // direct address
        } else {
            ea = r[BP];
            seg = SS;
        }
        break;
    default: ea = r[BX]; break;
    }
    if (m.mod == 1) ea = uint16_t(ea + int8_t(Fetch8()));
    if (m.mod == 2) ea = uint16_t(ea + Fetch16());
    m.ea = ea;
    m.seg = SegOr(seg);
    return m;
}

uint8_t Cpu::GetRM8(const ModRM& m) { return m.mod == 3 ? regs_.R8(m.rm) : Read8(m.seg, m.ea); }
uint16_t Cpu::GetRM16(const ModRM& m) { return m.mod == 3 ? regs_.r[m.rm] : Read16(m.seg, m.ea); }

void Cpu::SetRM8(const ModRM& m, uint8_t v) {
    if (m.mod == 3) regs_.R8(m.rm) = v; else Write8(m.seg, m.ea, v);
}

void Cpu::SetRM16(const ModRM& m, uint16_t v) {
    if (m.mod == 3) regs_.r[m.rm] = v; else Write16(m.seg, m.ea, v);
}

// --- Flags and arithmetic ----------------------------------------------------------------

void Cpu::SetSZP(uint16_t r, bool word) {
    const uint16_t sign = word ? 0x8000 : 0x80;
    const uint16_t mask = word ? 0xFFFF : 0xFF;
    SetFlag(flags::ZF, (r & mask) == 0);
    SetFlag(flags::SF, (r & sign) != 0);
    SetFlag(flags::PF, EvenParity(static_cast<uint8_t>(r)));
}

uint16_t Cpu::Add(uint32_t a, uint32_t b, uint32_t carry, bool word) {
    const uint32_t mask = word ? 0xFFFF : 0xFF, sign = word ? 0x8000 : 0x80;
    const uint32_t r = a + b + carry;
    SetFlag(flags::CF, r > mask);
    SetFlag(flags::OF, ((a ^ r) & (b ^ r) & sign) != 0);
    SetFlag(flags::AF, ((a ^ b ^ r) & 0x10) != 0);
    SetSZP(static_cast<uint16_t>(r & mask), word);
    return static_cast<uint16_t>(r & mask);
}

uint16_t Cpu::Sub(uint32_t a, uint32_t b, uint32_t borrow, bool word) {
    const uint32_t mask = word ? 0xFFFF : 0xFF, sign = word ? 0x8000 : 0x80;
    const uint32_t r = (a - b - borrow) & mask;
    SetFlag(flags::CF, b + borrow > a);
    SetFlag(flags::OF, ((a ^ b) & (a ^ r) & sign) != 0);
    SetFlag(flags::AF, ((a ^ b ^ r) & 0x10) != 0);
    SetSZP(static_cast<uint16_t>(r), word);
    return static_cast<uint16_t>(r);
}

uint16_t Cpu::Logic(uint16_t r, bool word) {
    SetFlag(flags::CF, false);
    SetFlag(flags::OF, false);
    SetFlag(flags::AF, false);
    SetSZP(r, word);
    return word ? r : static_cast<uint16_t>(r & 0xFF);
}

uint16_t Cpu::Alu(int op, uint16_t a, uint16_t b, bool word) {
    const uint32_t carry = Flag(flags::CF) ? 1 : 0;
    switch (op) {
    case 0: return Add(a, b, 0, word);
    case 1: return Logic(uint16_t(a | b), word);
    case 2: return Add(a, b, carry, word);
    case 3: return Sub(a, b, carry, word);
    case 4: return Logic(uint16_t(a & b), word);
    case 5: return Sub(a, b, 0, word);
    case 6: return Logic(uint16_t(a ^ b), word);
    default: Sub(a, b, 0, word); return a;  // CMP: flags only
    }
}

uint16_t Cpu::IncDec(uint16_t v, bool inc, bool word) {
    const bool cf = Flag(flags::CF);  // INC/DEC leave CF alone
    const uint16_t r = inc ? Add(v, 1, 0, word) : Sub(v, 1, 0, word);
    SetFlag(flags::CF, cf);
    return r;
}

uint16_t Cpu::Shift(int op, uint16_t value, uint8_t count, bool word) {
    count &= 0x1F;  // 80186+ masks the count
    if (count == 0) return value;
    const int bits = word ? 16 : 8;
    const uint32_t mask = word ? 0xFFFF : 0xFF, sign = word ? 0x8000 : 0x80;
    uint32_t v = value & mask;
    bool cf = Flag(flags::CF);

    switch (op) {
    case 0:  // ROL
        for (int i = 0; i < count; ++i) v = ((v << 1) | ((v & sign) ? 1 : 0)) & mask;
        cf = (v & 1) != 0;
        SetFlag(flags::CF, cf);
        SetFlag(flags::OF, ((v & sign) != 0) != cf);
        return uint16_t(v);
    case 1:  // ROR
        for (int i = 0; i < count; ++i) v = (v >> 1) | ((v & 1) ? sign : 0);
        SetFlag(flags::CF, (v & sign) != 0);
        SetFlag(flags::OF, ((v ^ (v << 1)) & sign) != 0);
        return uint16_t(v);
    case 2:  // RCL
        for (int i = 0; i < count; ++i) {
            const bool out = (v & sign) != 0;
            v = ((v << 1) | (cf ? 1 : 0)) & mask;
            cf = out;
        }
        SetFlag(flags::CF, cf);
        SetFlag(flags::OF, ((v & sign) != 0) != cf);
        return uint16_t(v);
    case 3: {  // RCR
        SetFlag(flags::OF, ((v & sign) != 0) != cf);
        for (int i = 0; i < count; ++i) {
            const bool out = (v & 1) != 0;
            v = (v >> 1) | (cf ? sign : 0);
            cf = out;
        }
        SetFlag(flags::CF, cf);
        return uint16_t(v);
    }
    case 4:
    case 6: {  // SHL / SAL
        const uint32_t r = v << count;
        cf = count <= bits && ((r >> bits) & 1);
        v = r & mask;
        SetFlag(flags::CF, cf);
        SetFlag(flags::OF, ((v & sign) != 0) != cf);
        break;
    }
    case 5:  // SHR
        SetFlag(flags::CF, count <= bits && ((v >> (count - 1)) & 1));
        SetFlag(flags::OF, (v & sign) != 0);  // defined for count 1: the original sign
        v = count >= bits ? 0 : v >> count;
        break;
    default: {  // SAR
        const int32_t sv = word ? int32_t(int16_t(v)) : int32_t(int8_t(v));
        const int n = std::min<int>(count, bits - 1);
        SetFlag(flags::CF, ((sv >> std::min<int>(count - 1, bits - 1)) & 1) != 0);
        SetFlag(flags::OF, false);
        v = uint32_t(sv >> n) & mask;
        break;
    }
    }
    SetFlag(flags::AF, false);
    SetSZP(uint16_t(v), word);
    return uint16_t(v);
}

void Cpu::MulDiv(int op, uint16_t src, bool word) {
    uint16_t* r = regs_.r;
    switch (op) {
    case 4: {  // MUL
        if (word) {
            const uint32_t p = uint32_t(r[AX]) * src;
            r[AX] = uint16_t(p);
            r[DX] = uint16_t(p >> 16);
            SetFlag(flags::CF, r[DX] != 0);
        } else {
            r[AX] = uint16_t(regs_.R8(0) * uint8_t(src));
            SetFlag(flags::CF, (r[AX] >> 8) != 0);
        }
        SetFlag(flags::OF, Flag(flags::CF));
        SetSZP(r[AX], word);
        break;
    }
    case 5: {  // IMUL
        if (word) {
            const int32_t p = int32_t(int16_t(r[AX])) * int16_t(src);
            r[AX] = uint16_t(p);
            r[DX] = uint16_t(uint32_t(p) >> 16);
            SetFlag(flags::CF, p != int16_t(p));
        } else {
            const int16_t p = int16_t(int8_t(regs_.R8(0)) * int8_t(src));
            r[AX] = uint16_t(p);
            SetFlag(flags::CF, p != int8_t(p));
        }
        SetFlag(flags::OF, Flag(flags::CF));
        SetSZP(r[AX], word);
        break;
    }
    case 6: {  // DIV
        if (src == 0) Throw(FaultKind::DivideError, "divide by zero");
        if (word) {
            const uint32_t n = (uint32_t(r[DX]) << 16) | r[AX];
            const uint32_t q = n / src;
            if (q > 0xFFFF) Throw(FaultKind::DivideError, "quotient overflow");
            r[AX] = uint16_t(q);
            r[DX] = uint16_t(n % src);
        } else {
            const uint16_t n = r[AX];
            const uint16_t q = n / uint8_t(src);
            if (q > 0xFF) Throw(FaultKind::DivideError, "quotient overflow");
            regs_.R8(0) = uint8_t(q);
            regs_.R8(4) = uint8_t(n % uint8_t(src));
        }
        break;
    }
    default: {  // IDIV
        if (src == 0) Throw(FaultKind::DivideError, "divide by zero");
        if (word) {
            const int32_t n = int32_t((uint32_t(r[DX]) << 16) | r[AX]);
            const int32_t d = int16_t(src);
            if (n == INT32_MIN && d == -1) Throw(FaultKind::DivideError, "quotient overflow");
            const int32_t q = n / d;
            if (q > 32767 || q < -32768) Throw(FaultKind::DivideError, "quotient overflow");
            r[AX] = uint16_t(q);
            r[DX] = uint16_t(n % d);
        } else {
            const int32_t n = int16_t(r[AX]);
            const int32_t d = int8_t(src);
            const int32_t q = n / d;
            if (q > 127 || q < -128) Throw(FaultKind::DivideError, "quotient overflow");
            regs_.R8(0) = uint8_t(q);
            regs_.R8(4) = uint8_t(n % d);
        }
        break;
    }
    }
}

void Cpu::Group3(const ModRM& m, bool word) {
    const uint16_t v = word ? GetRM16(m) : GetRM8(m);
    switch (m.reg) {
    case 0:
    case 1:  // TEST rm, imm
        Logic(uint16_t(v & (word ? Fetch16() : Fetch8())), word);
        break;
    case 2:  // NOT (no flags)
        if (word) SetRM16(m, uint16_t(~v)); else SetRM8(m, uint8_t(~v));
        break;
    case 3: {  // NEG
        const uint16_t r = Sub(0, v, 0, word);
        SetFlag(flags::CF, v != 0);
        if (word) SetRM16(m, r); else SetRM8(m, uint8_t(r));
        break;
    }
    default:
        MulDiv(m.reg, v, word);
        break;
    }
}

bool Cpu::Condition(int cc) const {
    const bool cf = Flag(flags::CF), zf = Flag(flags::ZF), sf = Flag(flags::SF), of = Flag(flags::OF);
    bool r;
    switch (cc >> 1) {
    case 0: r = of; break;
    case 1: r = cf; break;
    case 2: r = zf; break;
    case 3: r = cf || zf; break;
    case 4: r = sf; break;
    case 5: r = Flag(flags::PF); break;
    case 6: r = sf != of; break;
    default: r = zf || (sf != of); break;
    }
    return (cc & 1) ? !r : r;
}

// --- Control transfer ---------------------------------------------------------------------

void Cpu::FarJump(uint16_t selector, uint16_t offset) {
    LoadSegment(CS, selector);
    regs_.ip = offset;
}

void Cpu::Interrupt(uint8_t vector) {
    if (interrupt_ && interrupt_(*this, vector)) return;
    Throw(FaultKind::UnhandledInterrupt, "unhandled INT " + Hex(vector, 2) + "h (AX=" + Hex(regs_.r[AX], 4) + ")");
}

void Cpu::StringOp(uint8_t op) {
    const bool word = op & 1;
    const int16_t step = int16_t((Flag(flags::DF) ? -1 : 1) * (word ? 2 : 1));
    const uint8_t src = SegOr(DS);
    uint16_t* r = regs_.r;

    auto once = [&] {
        switch (op & 0xFE) {
        case 0xA4:  // MOVS
            if (word) Write16(ES, r[DI], Read16(src, r[SI])); else Write8(ES, r[DI], Read8(src, r[SI]));
            r[SI] = uint16_t(r[SI] + step);
            r[DI] = uint16_t(r[DI] + step);
            break;
        case 0xA6:  // CMPS
            if (word) Sub(Read16(src, r[SI]), Read16(ES, r[DI]), 0, true);
            else Sub(Read8(src, r[SI]), Read8(ES, r[DI]), 0, false);
            r[SI] = uint16_t(r[SI] + step);
            r[DI] = uint16_t(r[DI] + step);
            break;
        case 0xAA:  // STOS
            if (word) Write16(ES, r[DI], r[AX]); else Write8(ES, r[DI], regs_.R8(0));
            r[DI] = uint16_t(r[DI] + step);
            break;
        case 0xAC:  // LODS
            if (word) r[AX] = Read16(src, r[SI]); else regs_.R8(0) = Read8(src, r[SI]);
            r[SI] = uint16_t(r[SI] + step);
            break;
        default:  // 0xAE SCAS
            if (word) Sub(r[AX], Read16(ES, r[DI]), 0, true); else Sub(regs_.R8(0), Read8(ES, r[DI]), 0, false);
            r[DI] = uint16_t(r[DI] + step);
            break;
        }
    };

    if (!rep_) {
        once();
        return;
    }
    const bool compares = (op & 0xFE) == 0xA6 || (op & 0xFE) == 0xAE;
    while (r[CX] != 0) {
        once();
        r[CX] = uint16_t(r[CX] - 1);
        if (compares && (rep_ == 0xF3) != Flag(flags::ZF)) break;  // REPE stops on !ZF, REPNE on ZF
    }
}

// --- Execution ----------------------------------------------------------------------------

void Cpu::Step() {
    segOverride_ = -1;
    rep_ = 0;
    for (;;) {
        const uint8_t op = Fetch8();
        switch (op) {
        case 0x26: segOverride_ = ES; continue;
        case 0x2E: segOverride_ = CS; continue;
        case 0x36: segOverride_ = SS; continue;
        case 0x3E: segOverride_ = DS; continue;
        case 0xF0: continue;  // LOCK
        case 0xF2:
        case 0xF3: rep_ = op; continue;
        default: Execute(op); return;
        }
    }
}

void Cpu::Execute(uint8_t op) {
    uint16_t* r = regs_.r;

    // ALU block 00-3F: ADD OR ADC SBB AND SUB XOR CMP in six operand forms.
    if (op < 0x40 && (op & 7) < 6) {
        const int alu = op >> 3;
        switch (op & 7) {
        case 0: {  // rm8, r8
            const ModRM m = DecodeModRM();
            const uint8_t res = uint8_t(Alu(alu, GetRM8(m), regs_.R8(m.reg), false));
            if (alu != 7) SetRM8(m, res);
            return;
        }
        case 1: {  // rm16, r16
            const ModRM m = DecodeModRM();
            const uint16_t res = Alu(alu, GetRM16(m), r[m.reg], true);
            if (alu != 7) SetRM16(m, res);
            return;
        }
        case 2: {  // r8, rm8
            const ModRM m = DecodeModRM();
            const uint8_t res = uint8_t(Alu(alu, regs_.R8(m.reg), GetRM8(m), false));
            if (alu != 7) regs_.R8(m.reg) = res;
            return;
        }
        case 3: {  // r16, rm16
            const ModRM m = DecodeModRM();
            const uint16_t res = Alu(alu, r[m.reg], GetRM16(m), true);
            if (alu != 7) r[m.reg] = res;
            return;
        }
        case 4: {  // AL, imm8
            const uint8_t res = uint8_t(Alu(alu, regs_.R8(0), Fetch8(), false));
            if (alu != 7) regs_.R8(0) = res;
            return;
        }
        default: {  // AX, imm16
            const uint16_t res = Alu(alu, r[AX], Fetch16(), true);
            if (alu != 7) r[AX] = res;
            return;
        }
        }
    }

    switch (op) {
    // Segment register push/pop.
    case 0x06: Push(regs_.s[ES]); return;
    case 0x07: LoadSegment(ES, Pop()); return;
    case 0x0E: Push(regs_.s[CS]); return;
    case 0x16: Push(regs_.s[SS]); return;
    case 0x17: LoadSegment(SS, Pop()); return;
    case 0x1E: Push(regs_.s[DS]); return;
    case 0x1F: LoadSegment(DS, Pop()); return;

    // BCD adjust.
    case 0x27: case 0x2F: {  // DAA / DAS
        // Intel SDM: the final CF comes only from the high-digit adjustment.
        const uint8_t old = regs_.R8(0);
        const bool oldCf = Flag(flags::CF);
        const bool sub = op == 0x2F;
        uint8_t al = old;
        const bool lowAdjust = (al & 0x0F) > 9 || Flag(flags::AF);
        if (lowAdjust) al = uint8_t(sub ? al - 6 : al + 6);
        const bool highAdjust = old > 0x99 || oldCf;
        if (highAdjust) al = uint8_t(sub ? al - 0x60 : al + 0x60);
        regs_.R8(0) = al;
        SetFlag(flags::AF, lowAdjust);
        SetFlag(flags::CF, highAdjust);
        SetSZP(al, false);
        return;
    }
    case 0x37: case 0x3F: {  // AAA / AAS
        if ((regs_.R8(0) & 0x0F) > 9 || Flag(flags::AF)) {
            r[AX] = uint16_t(op == 0x37 ? r[AX] + 0x106 : r[AX] - 6);
            if (op == 0x3F) regs_.R8(4) = uint8_t(regs_.R8(4) - 1);
            SetFlag(flags::AF, true);
            SetFlag(flags::CF, true);
        } else {
            SetFlag(flags::AF, false);
            SetFlag(flags::CF, false);
        }
        regs_.R8(0) &= 0x0F;
        return;
    }

    default: break;
    }

    if (op >= 0x40 && op <= 0x4F) {  // INC/DEC r16
        r[op & 7] = IncDec(r[op & 7], op < 0x48, true);
        return;
    }
    if (op >= 0x50 && op <= 0x57) {  // PUSH r16 (286: PUSH SP pushes the old SP)
        Push(r[op & 7]);
        return;
    }
    if (op >= 0x58 && op <= 0x5F) {
        const uint16_t v = Pop();
        r[op & 7] = v;
        return;
    }
    if (op >= 0x70 && op <= 0x7F) {  // Jcc rel8
        const int8_t d = int8_t(Fetch8());
        if (Condition(op & 0x0F)) regs_.ip = uint16_t(regs_.ip + d);
        return;
    }
    if (op >= 0x91 && op <= 0x97) {  // XCHG AX, r16
        std::swap(r[AX], r[op & 7]);
        return;
    }
    if (op >= 0xB0 && op <= 0xB7) {
        regs_.R8(op & 7) = Fetch8();
        return;
    }
    if (op >= 0xB8 && op <= 0xBF) {
        r[op & 7] = Fetch16();
        return;
    }
    if (op >= 0xA4 && op <= 0xAF && op != 0xA8 && op != 0xA9) {
        StringOp(op);
        return;
    }
    if (op >= 0xD8 && op <= 0xDF) {
        Escape(op);
        return;
    }

    switch (op) {
    case 0x60: {  // PUSHA
        const uint16_t sp = r[SP];
        Push(r[AX]); Push(r[CX]); Push(r[DX]); Push(r[BX]);
        Push(sp); Push(r[BP]); Push(r[SI]); Push(r[DI]);
        return;
    }
    case 0x61:  // POPA
        r[DI] = Pop(); r[SI] = Pop(); r[BP] = Pop(); Pop();
        r[BX] = Pop(); r[DX] = Pop(); r[CX] = Pop(); r[AX] = Pop();
        return;
    case 0x68: Push(Fetch16()); return;
    case 0x6A: Push(uint16_t(int8_t(Fetch8()))); return;
    case 0x69:
    case 0x6B: {  // IMUL r16, rm16, imm
        const ModRM m = DecodeModRM();
        const int32_t a = int16_t(GetRM16(m));
        const int32_t b = op == 0x69 ? int16_t(Fetch16()) : int8_t(Fetch8());
        const int32_t p = a * b;
        r[m.reg] = uint16_t(p);
        SetFlag(flags::CF, p != int16_t(p));
        SetFlag(flags::OF, Flag(flags::CF));
        SetSZP(uint16_t(p), true);
        return;
    }

    case 0x80:
    case 0x82: {
        const ModRM m = DecodeModRM();
        const uint8_t res = uint8_t(Alu(m.reg, GetRM8(m), Fetch8(), false));
        if (m.reg != 7) SetRM8(m, res);
        return;
    }
    case 0x81: {
        const ModRM m = DecodeModRM();
        const uint16_t res = Alu(m.reg, GetRM16(m), Fetch16(), true);
        if (m.reg != 7) SetRM16(m, res);
        return;
    }
    case 0x83: {
        const ModRM m = DecodeModRM();
        const uint16_t res = Alu(m.reg, GetRM16(m), uint16_t(int8_t(Fetch8())), true);
        if (m.reg != 7) SetRM16(m, res);
        return;
    }
    case 0x84: { const ModRM m = DecodeModRM(); Logic(uint16_t(GetRM8(m) & regs_.R8(m.reg)), false); return; }
    case 0x85: { const ModRM m = DecodeModRM(); Logic(uint16_t(GetRM16(m) & r[m.reg]), true); return; }
    case 0x86: {
        const ModRM m = DecodeModRM();
        const uint8_t t = GetRM8(m);
        SetRM8(m, regs_.R8(m.reg));
        regs_.R8(m.reg) = t;
        return;
    }
    case 0x87: {
        const ModRM m = DecodeModRM();
        const uint16_t t = GetRM16(m);
        SetRM16(m, r[m.reg]);
        r[m.reg] = t;
        return;
    }
    case 0x88: { const ModRM m = DecodeModRM(); SetRM8(m, regs_.R8(m.reg)); return; }
    case 0x89: { const ModRM m = DecodeModRM(); SetRM16(m, r[m.reg]); return; }
    case 0x8A: { const ModRM m = DecodeModRM(); regs_.R8(m.reg) = GetRM8(m); return; }
    case 0x8B: { const ModRM m = DecodeModRM(); r[m.reg] = GetRM16(m); return; }
    case 0x8C: {
        const ModRM m = DecodeModRM();
        if (m.reg > 3) Throw(FaultKind::InvalidOpcode, "MOV from segment register " + Hex(m.reg, 1));
        SetRM16(m, regs_.s[m.reg]);
        return;
    }
    case 0x8D: {
        const ModRM m = DecodeModRM();
        if (m.mod == 3) Throw(FaultKind::InvalidOpcode, "LEA with register operand");
        r[m.reg] = m.ea;
        return;
    }
    case 0x8E: {
        const ModRM m = DecodeModRM();
        if (m.reg == CS || m.reg > 3) Throw(FaultKind::InvalidOpcode, "MOV to CS");
        LoadSegment(SegReg(m.reg), GetRM16(m));
        return;
    }
    case 0x8F: {
        const ModRM m = DecodeModRM();
        const uint16_t v = Pop();
        SetRM16(m, v);
        return;
    }
    case 0x90: return;
    case 0x98: r[AX] = uint16_t(int8_t(regs_.R8(0))); return;
    case 0x99: r[DX] = (r[AX] & 0x8000) ? 0xFFFF : 0; return;
    case 0x9A: {  // CALL far ptr16:16
        const uint16_t off = Fetch16(), sel = Fetch16();
        const uint16_t retCs = regs_.s[CS], retIp = regs_.ip;
        FarJump(sel, off);
        Push(retCs);
        Push(retIp);
        return;
    }
    case 0x9B: return;  // WAIT
    case 0x9C: Push(regs_.flags); return;
    case 0x9D: regs_.flags = uint16_t((Pop() & 0x0FD5) | 0x0002); return;
    case 0x9E: regs_.flags = uint16_t((regs_.flags & 0xFF2A) | (regs_.R8(4) & 0xD5)); return;
    case 0x9F: regs_.R8(4) = uint8_t(regs_.flags); return;
    case 0xA0: regs_.R8(0) = Read8(SegOr(DS), Fetch16()); return;
    case 0xA1: r[AX] = Read16(SegOr(DS), Fetch16()); return;
    case 0xA2: Write8(SegOr(DS), Fetch16(), regs_.R8(0)); return;
    case 0xA3: Write16(SegOr(DS), Fetch16(), r[AX]); return;
    case 0xA8: Logic(uint16_t(regs_.R8(0) & Fetch8()), false); return;
    case 0xA9: Logic(uint16_t(r[AX] & Fetch16()), true); return;

    case 0xC0:
    case 0xC1: {
        const ModRM m = DecodeModRM();
        const bool word = op == 0xC1;
        const uint16_t v = word ? GetRM16(m) : GetRM8(m);
        const uint16_t res = Shift(m.reg, v, Fetch8(), word);
        if (word) SetRM16(m, res); else SetRM8(m, uint8_t(res));
        return;
    }
    case 0xC2: { const uint16_t n = Fetch16(); regs_.ip = Pop(); r[SP] = uint16_t(r[SP] + n); return; }
    case 0xC3: regs_.ip = Pop(); return;
    case 0xC4:
    case 0xC5: {  // LES / LDS
        const ModRM m = DecodeModRM();
        if (m.mod == 3) Throw(FaultKind::InvalidOpcode, "LES/LDS with register operand");
        const uint16_t off = Read16(m.seg, m.ea);
        const uint16_t sel = Read16(m.seg, uint16_t(m.ea + 2));
        LoadSegment(op == 0xC4 ? ES : DS, sel);
        r[m.reg] = off;
        return;
    }
    case 0xC6: { const ModRM m = DecodeModRM(); SetRM8(m, Fetch8()); return; }
    case 0xC7: { const ModRM m = DecodeModRM(); SetRM16(m, Fetch16()); return; }
    case 0xC8: {  // ENTER
        const uint16_t size = Fetch16();
        const uint8_t level = Fetch8() & 31;
        Push(r[BP]);
        const uint16_t frame = r[SP];
        for (int i = 1; i < level; ++i) {
            r[BP] = uint16_t(r[BP] - 2);
            Push(Read16(SS, r[BP]));
        }
        if (level > 0) Push(frame);
        r[BP] = frame;
        r[SP] = uint16_t(r[SP] - size);
        return;
    }
    case 0xC9: r[SP] = r[BP]; r[BP] = Pop(); return;  // LEAVE
    case 0xCA:
    case 0xCB: {  // RETF [imm16]
        const uint16_t n = op == 0xCA ? Fetch16() : 0;
        const uint16_t ip = Pop(), cs = Pop();
        FarJump(cs, ip);
        r[SP] = uint16_t(r[SP] + n);
        return;
    }
    case 0xCC: Interrupt(3); return;
    case 0xCD: {
        const uint8_t vector = Fetch8();
        if (!EmulatorInterrupt(vector)) Interrupt(vector);
        return;
    }
    case 0xCE: if (Flag(flags::OF)) Interrupt(4); return;
    case 0xCF: {  // IRET
        const uint16_t ip = Pop(), cs = Pop(), f = Pop();
        FarJump(cs, ip);
        regs_.flags = uint16_t((f & 0x0FD5) | 0x0002);
        return;
    }

    case 0xD0: case 0xD1: case 0xD2: case 0xD3: {
        const ModRM m = DecodeModRM();
        const bool word = op & 1;
        const uint8_t count = op >= 0xD2 ? regs_.R8(1) : 1;
        const uint16_t v = word ? GetRM16(m) : GetRM8(m);
        const uint16_t res = Shift(m.reg, v, count, word);
        if (word) SetRM16(m, res); else SetRM8(m, uint8_t(res));
        return;
    }
    case 0xD4: {  // AAM
        const uint8_t base = Fetch8();
        if (base == 0) Throw(FaultKind::DivideError, "AAM with base 0");
        const uint8_t al = regs_.R8(0);
        regs_.R8(4) = uint8_t(al / base);
        regs_.R8(0) = uint8_t(al % base);
        SetSZP(regs_.R8(0), false);
        return;
    }
    case 0xD5: {  // AAD
        const uint8_t base = Fetch8();
        regs_.R8(0) = uint8_t(regs_.R8(4) * base + regs_.R8(0));
        regs_.R8(4) = 0;
        SetSZP(regs_.R8(0), false);
        return;
    }
    case 0xD7: regs_.R8(0) = Read8(SegOr(DS), uint16_t(r[BX] + regs_.R8(0))); return;  // XLAT

    case 0xE0: case 0xE1: case 0xE2: {  // LOOPNZ / LOOPZ / LOOP
        const int8_t d = int8_t(Fetch8());
        r[CX] = uint16_t(r[CX] - 1);
        bool take = r[CX] != 0;
        if (op == 0xE0) take = take && !Flag(flags::ZF);
        if (op == 0xE1) take = take && Flag(flags::ZF);
        if (take) regs_.ip = uint16_t(regs_.ip + d);
        return;
    }
    case 0xE3: {  // JCXZ
        const int8_t d = int8_t(Fetch8());
        if (r[CX] == 0) regs_.ip = uint16_t(regs_.ip + d);
        return;
    }
    case 0xE8: {
        const int16_t d = int16_t(Fetch16());
        Push(regs_.ip);
        regs_.ip = uint16_t(regs_.ip + d);
        return;
    }
    case 0xE9: { const int16_t d = int16_t(Fetch16()); regs_.ip = uint16_t(regs_.ip + d); return; }
    case 0xEA: { const uint16_t off = Fetch16(), sel = Fetch16(); FarJump(sel, off); return; }
    case 0xEB: { const int8_t d = int8_t(Fetch8()); regs_.ip = uint16_t(regs_.ip + d); return; }

    case 0xE4: case 0xE5: case 0xE6: case 0xE7: case 0xEC: case 0xED: case 0xEE: case 0xEF:
    case 0x6C: case 0x6D: case 0x6E: case 0x6F:
        throw ProtectionFault("I/O port access from a Windows application", regs_.s[CS], startIp_);
    case 0xF4:
        throw ProtectionFault("HLT is privileged", regs_.s[CS], startIp_);

    case 0xF5: SetFlag(flags::CF, !Flag(flags::CF)); return;
    case 0xF6: { const ModRM m = DecodeModRM(); Group3(m, false); return; }
    case 0xF7: { const ModRM m = DecodeModRM(); Group3(m, true); return; }
    case 0xF8: SetFlag(flags::CF, false); return;
    case 0xF9: SetFlag(flags::CF, true); return;
    case 0xFA: SetFlag(flags::IF, false); return;
    case 0xFB: SetFlag(flags::IF, true); return;
    case 0xFC: SetFlag(flags::DF, false); return;
    case 0xFD: SetFlag(flags::DF, true); return;
    case 0xFE: {
        const ModRM m = DecodeModRM();
        if (m.reg > 1) Throw(FaultKind::InvalidOpcode, "FE /" + Hex(m.reg, 1));
        SetRM8(m, uint8_t(IncDec(GetRM8(m), m.reg == 0, false)));
        return;
    }
    case 0xFF: {
        const ModRM m = DecodeModRM();
        switch (m.reg) {
        case 0:
        case 1: SetRM16(m, IncDec(GetRM16(m), m.reg == 0, true)); return;
        case 2: {  // CALL near rm16
            const uint16_t target = GetRM16(m);
            Push(regs_.ip);
            regs_.ip = target;
            return;
        }
        case 3:
        case 5: {  // CALL / JMP far m16:16
            if (m.mod == 3) Throw(FaultKind::InvalidOpcode, "far CALL/JMP with register operand");
            const uint16_t off = Read16(m.seg, m.ea), sel = Read16(m.seg, uint16_t(m.ea + 2));
            const uint16_t retCs = regs_.s[CS], retIp = regs_.ip;
            FarJump(sel, off);
            if (m.reg == 3) {
                Push(retCs);
                Push(retIp);
            }
            return;
        }
        case 4: regs_.ip = GetRM16(m); return;
        case 6: Push(GetRM16(m)); return;
        default: Throw(FaultKind::InvalidOpcode, "FF /7");
        }
    }
    default:
        Throw(FaultKind::InvalidOpcode, "opcode " + Hex(op, 2) + "h not implemented");
    }
}

}  // namespace retro::win16
