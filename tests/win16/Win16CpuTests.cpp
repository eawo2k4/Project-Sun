// Instruction-level tests of the 16-bit interpreter: each case runs a short
// hand-assembled snippet ending in INT 3 and checks registers, flags, memory
// or the fault it raised. Expected values follow real x86 behaviour (for the
// x87, real results rounded to double precision).

#include <cmath>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <limits>
#include <numbers>
#include <string>
#include <vector>

#include "../Check.h"
#include "win16/Cpu.h"
#include "win16/Memory.h"

using namespace retro::win16;

namespace {

// Code, data and stack segments; INT 3 ends the snippet.
struct Machine {
    Memory mem{1 << 20};
    Cpu cpu{mem};
    uint16_t code = 0, data = 0, stack = 0;
    RunResult result = RunResult::Stopped;

    explicit Machine(std::initializer_list<int> bytes) {
        std::vector<uint8_t> program;
        for (int b : bytes) program.push_back(uint8_t(b));
        program.push_back(0xCC);  // int 3 -> stop
        code = mem.Allocate(uint32_t(program.size()), SegmentKind::Code);
        data = mem.Allocate(0x100, SegmentKind::Data);
        stack = mem.Allocate(0x400, SegmentKind::Data);
        std::copy(program.begin(), program.end(), mem.SegmentData(code));
        Registers& r = cpu.Regs();
        r.s[CS] = code;
        r.s[DS] = data;
        r.s[ES] = data;
        r.s[SS] = stack;
        r.r[SP] = 0x400;
        cpu.SetInterruptHandler([](Cpu& c, uint8_t v) {
            if (v != 3) return false;
            c.Stop();
            return true;
        });
    }

    Machine& Run() {
        result = cpu.Run(10'000);
        if (result == RunResult::Faulted) std::printf("  fault: %s\n", cpu.Fault().detail.c_str());
        return *this;
    }
    uint16_t R(Reg reg) const { return cpu.Regs().r[reg]; }
    // Data segment accessors.
    void PutDouble(uint16_t off, double v) { std::memcpy(mem.SegmentData(data) + off, &v, 8); }
    void PutWord(uint16_t off, uint16_t v) { std::memcpy(mem.SegmentData(data) + off, &v, 2); }
    void PutLong(uint16_t off, uint32_t v) { std::memcpy(mem.SegmentData(data) + off, &v, 4); }
    double Double(uint16_t off) const {
        double v;
        std::memcpy(&v, mem.SegmentData(data) + off, 8);
        return v;
    }
    uint16_t Word(uint16_t off) const { return uint16_t(mem.SegmentData(data)[off] | (mem.SegmentData(data)[off + 1] << 8)); }
    uint32_t Long(uint16_t off) const { return Word(off) | (uint32_t(Word(uint16_t(off + 2))) << 16); }
    const uint8_t* Bytes(uint16_t off) const { return mem.SegmentData(data) + off; }
    bool F(uint16_t f) const { return cpu.Flag(f); }
    bool Ok() const { return result == RunResult::Stopped; }
    bool Faulted(FaultKind k) const { return result == RunResult::Faulted && cpu.Fault().kind == k; }
};

void TestAddFlags() {
    Machine m({0xB8, 0xFF, 0x7F, 0x05, 0x01, 0x00});  // mov ax, 7FFFh / add ax, 1
    m.Run();
    CHECK(m.Ok() && m.R(AX) == 0x8000);
    CHECK(m.F(flags::OF) && m.F(flags::SF) && !m.F(flags::CF) && !m.F(flags::ZF) && m.F(flags::AF));

    Machine c({0xB8, 0xFF, 0xFF, 0x05, 0x01, 0x00});  // mov ax, FFFFh / add ax, 1
    c.Run();
    CHECK(c.R(AX) == 0 && c.F(flags::CF) && c.F(flags::ZF) && c.F(flags::PF) && !c.F(flags::OF));

    Machine b({0xB0, 0x0F, 0x04, 0x01});  // mov al, 0Fh / add al, 1 -> 10h: AF, no CF
    b.Run();
    CHECK((b.R(AX) & 0xFF) == 0x10 && b.F(flags::AF) && !b.F(flags::CF) && !b.F(flags::PF));
}

void TestSubAndCompare() {
    Machine m({0xB8, 0x00, 0x00, 0x2D, 0x01, 0x00});  // mov ax, 0 / sub ax, 1
    m.Run();
    CHECK(m.R(AX) == 0xFFFF && m.F(flags::CF) && m.F(flags::SF) && !m.F(flags::OF));

    Machine o({0xB8, 0x00, 0x80, 0x2D, 0x01, 0x00});  // 8000h - 1: signed overflow
    o.Run();
    CHECK(o.R(AX) == 0x7FFF && o.F(flags::OF) && !o.F(flags::CF));

    Machine c({0xB8, 0x05, 0x00, 0x3D, 0x05, 0x00});  // cmp ax, 5: ZF, AX kept
    c.Run();
    CHECK(c.R(AX) == 5 && c.F(flags::ZF) && !c.F(flags::CF));
}

void TestCarryChains() {
    // 32-bit add: 0001FFFFh + 00000001h = 00020000h via ADD/ADC.
    Machine a({0xB8, 0xFF, 0xFF, 0xBA, 0x01, 0x00, 0x05, 0x01, 0x00, 0x83, 0xD2, 0x00});
    a.Run();  // mov ax,FFFF / mov dx,1 / add ax,1 / adc dx,0
    CHECK(a.R(AX) == 0 && a.R(DX) == 2);
    // 32-bit sub: 00020000h - 1 = 0001FFFFh via SUB/SBB.
    Machine s({0xB8, 0x00, 0x00, 0xBA, 0x02, 0x00, 0x2D, 0x01, 0x00, 0x83, 0xDA, 0x00});
    s.Run();
    CHECK(s.R(AX) == 0xFFFF && s.R(DX) == 1);
}

void TestIncDecKeepCarry() {
    Machine m({0xF9, 0xB8, 0xFF, 0xFF, 0x40});  // stc / mov ax, FFFFh / inc ax
    m.Run();
    CHECK(m.R(AX) == 0 && m.F(flags::ZF) && m.F(flags::CF));  // CF untouched by INC
    Machine d({0xF8, 0xB8, 0x00, 0x80, 0x48});  // clc / mov ax, 8000h / dec ax
    d.Run();
    CHECK(d.R(AX) == 0x7FFF && d.F(flags::OF) && !d.F(flags::CF));
}

void TestLogicNegNot() {
    Machine m({0xF9, 0xB8, 0x0F, 0xF0, 0x25, 0xFF, 0x00});  // stc / mov ax,F00Fh / and ax,FFh
    m.Run();
    CHECK(m.R(AX) == 0x000F && !m.F(flags::CF) && !m.F(flags::OF) && m.F(flags::PF));
    Machine n({0xB8, 0x05, 0x00, 0xF7, 0xD8});  // mov ax, 5 / neg ax
    n.Run();
    CHECK(n.R(AX) == 0xFFFB && n.F(flags::CF) && n.F(flags::SF));
    Machine z({0x31, 0xC0, 0xF7, 0xD8});  // xor ax, ax / neg ax: CF = 0 for zero
    z.Run();
    CHECK(z.R(AX) == 0 && !z.F(flags::CF) && z.F(flags::ZF));
    Machine t({0xB8, 0x0F, 0x0F, 0xF7, 0xD0});  // not ax
    t.Run();
    CHECK(t.R(AX) == 0xF0F0);
}

void TestShiftsAndRotates() {
    Machine shl({0xB8, 0x01, 0x80, 0xD1, 0xE0});  // shl ax, 1
    shl.Run();
    CHECK(shl.R(AX) == 2 && shl.F(flags::CF) && shl.F(flags::OF));
    Machine shr({0xB8, 0x03, 0x00, 0xD1, 0xE8});  // shr ax, 1
    shr.Run();
    CHECK(shr.R(AX) == 1 && shr.F(flags::CF));
    Machine sar({0xB8, 0xFC, 0xFF, 0xB1, 0x02, 0xD3, 0xF8});  // mov ax,-4 / mov cl,2 / sar ax,cl
    sar.Run();
    CHECK(sar.R(AX) == 0xFFFF && !sar.F(flags::CF));
    Machine rol({0xB8, 0x01, 0x80, 0xD1, 0xC0});  // rol ax, 1
    rol.Run();
    CHECK(rol.R(AX) == 0x0003 && rol.F(flags::CF));
    Machine ror({0xB8, 0x01, 0x00, 0xD1, 0xC8});  // ror ax, 1
    ror.Run();
    CHECK(ror.R(AX) == 0x8000 && ror.F(flags::CF) && ror.F(flags::OF));
    Machine rcl({0xF9, 0xB8, 0x00, 0x80, 0xD1, 0xD0});  // stc / rcl ax, 1
    rcl.Run();
    CHECK(rcl.R(AX) == 0x0001 && rcl.F(flags::CF));
    Machine rcr({0xF9, 0xB8, 0x00, 0x00, 0xD1, 0xD8});  // stc / rcr ax, 1
    rcr.Run();
    CHECK(rcr.R(AX) == 0x8000 && !rcr.F(flags::CF));
    Machine imm({0xB8, 0x01, 0x00, 0xC1, 0xE0, 0x04});  // shl ax, 4 (80186)
    imm.Run();
    CHECK(imm.R(AX) == 0x10);
}

void TestMultiplyDivide() {
    Machine mul8({0xB0, 0x10, 0xB3, 0x20, 0xF6, 0xE3});  // mov al,10h / mov bl,20h / mul bl
    mul8.Run();
    CHECK(mul8.R(AX) == 0x0200 && mul8.F(flags::CF));
    Machine imul({0xB8, 0xFE, 0xFF, 0xBB, 0x03, 0x00, 0xF7, 0xEB});  // -2 * 3
    imul.Run();
    CHECK(imul.R(AX) == 0xFFFA && imul.R(DX) == 0xFFFF && !imul.F(flags::CF));
    Machine imm3({0xBB, 0x07, 0x00, 0x6B, 0xC3, 0xFA});  // mov bx,7 / imul ax, bx, -6
    imm3.Run();
    CHECK(imm3.R(AX) == uint16_t(-42));
    Machine div8({0xB8, 0x64, 0x00, 0xB3, 0x07, 0xF6, 0xF3});  // 100 / 7
    div8.Run();
    CHECK((div8.R(AX) & 0xFF) == 14 && (div8.R(AX) >> 8) == 2);
    Machine idiv({0xB8, 0xF6, 0xFF, 0x99, 0xBB, 0x03, 0x00, 0xF7, 0xFB});  // -10 / 3
    idiv.Run();
    CHECK(idiv.R(AX) == uint16_t(-3) && idiv.R(DX) == uint16_t(-1));

    Machine zero({0x31, 0xDB, 0xF7, 0xF3});  // div by zero
    zero.Run();
    CHECK(zero.Faulted(FaultKind::DivideError));
    CHECK(zero.cpu.Regs().ip == 2);  // IP of the faulting DIV
    Machine over({0xBA, 0x01, 0x00, 0xB8, 0x00, 0x00, 0xBB, 0x01, 0x00, 0xF7, 0xF3});
    over.Run();  // 10000h / 1 doesn't fit AX
    CHECK(over.Faulted(FaultKind::DivideError));
}

void TestConversionsAndBcd() {
    Machine cbw({0xB0, 0x80, 0x98, 0x99});  // mov al,80h / cbw / cwd
    cbw.Run();
    CHECK(cbw.R(AX) == 0xFF80 && cbw.R(DX) == 0xFFFF);
    Machine daa({0xB0, 0x19, 0x04, 0x28, 0x27});  // BCD 19 + 28 = 47
    daa.Run();
    CHECK((daa.R(AX) & 0xFF) == 0x47 && !daa.F(flags::CF));
    Machine daa2({0xB0, 0x99, 0x04, 0x01, 0x27});  // BCD 99 + 1 = (1)00
    daa2.Run();
    CHECK((daa2.R(AX) & 0xFF) == 0x00 && daa2.F(flags::CF));
    Machine das({0xB0, 0x47, 0x2C, 0x28, 0x2F});  // BCD 47 - 28 = 19
    das.Run();
    CHECK((das.R(AX) & 0xFF) == 0x19);
    Machine aam({0xB0, 0x3F, 0xD4, 0x0A});  // 63 -> AH 6, AL 3
    aam.Run();
    CHECK(aam.R(AX) == 0x0603);
}

void TestConditionsAndLoops() {
    // Every Jcc pair, after CMP 1, 2 (below, not equal, less, sign set).
    struct Case {
        uint8_t opcode;
        bool taken;
    } cases[] = {{0x72, true},  {0x73, false}, {0x74, false}, {0x75, true},
                 {0x76, true},  {0x77, false}, {0x78, true},  {0x79, false},
                 {0x7C, true},  {0x7D, false}, {0x7E, true},  {0x7F, false},
                 {0x70, false}, {0x71, true}};
    for (const Case& c : cases) {
        // mov ax,1 / cmp ax,2 / jcc +3 / mov bx,1 / (target:)
        Machine m({0xB8, 0x01, 0x00, 0x3D, 0x02, 0x00, c.opcode, 0x03, 0xBB, 0x01, 0x00});
        m.Run();
        CHECK((m.R(BX) == 0) == c.taken);
    }
    Machine loop({0x31, 0xC0, 0xB9, 0x05, 0x00, 0x40, 0xE2, 0xFD});  // ax += 1, 5 times
    loop.Run();
    CHECK(loop.R(AX) == 5 && loop.R(CX) == 0);
    Machine jcxz({0x31, 0xC9, 0xE3, 0x03, 0xBB, 0x01, 0x00});  // cx = 0 -> skip
    jcxz.Run();
    CHECK(jcxz.R(BX) == 0);
}

void TestStringInstructions() {
    // Fill 6 words with 1234h, copy them 32 bytes on, compare.
    Machine m({0xFC,                    // cld
               0xB8, 0x34, 0x12,        // mov ax, 1234h
               0x31, 0xFF,              // xor di, di
               0xB9, 0x06, 0x00,        // mov cx, 6
               0xF3, 0xAB,              // rep stosw
               0x31, 0xF6,              // xor si, si
               0xBF, 0x20, 0x00,        // mov di, 20h
               0xB9, 0x06, 0x00,        // mov cx, 6
               0xF3, 0xA5,              // rep movsw
               0x31, 0xF6,              // xor si, si
               0xBF, 0x20, 0x00,        // mov di, 20h
               0xB9, 0x0C, 0x00,        // mov cx, 12
               0xF3, 0xA6});            // repe cmpsb
    m.Run();
    CHECK(m.Ok() && m.F(flags::ZF) && m.R(CX) == 0);
    CHECK(m.mem.Read16(m.data, 0x2A) == 0x1234 && m.mem.Read16(m.data, 0x2C) == 0);

    // REPNE SCASB finds the first 0 in "ABC\0".
    Machine scan({0xC7, 0x06, 0x00, 0x00, 0x41, 0x42,  // mov word [0], 4241h
                  0xC7, 0x06, 0x02, 0x00, 0x43, 0x00,  // mov word [2], 0043h
                  0xFC, 0x31, 0xFF, 0x30, 0xC0,        // cld / xor di,di / xor al,al
                  0xB9, 0x10, 0x00, 0xF2, 0xAE});      // mov cx,16 / repne scasb
    scan.Run();
    CHECK(scan.R(DI) == 4 && scan.F(flags::ZF));
    Machine back({0xFD, 0xBF, 0x05, 0x00, 0xB0, 0x07, 0xAA});  // std / mov di,5 / stosb
    back.Run();
    CHECK(back.R(DI) == 4 && back.mem.Read8(back.data, 5) == 7);
}

void TestStackAndCalls() {
    // call near sub / sub: mov ax, 42 / ret
    Machine call({0xE8, 0x01, 0x00, 0xCC, 0xB8, 0x2A, 0x00, 0xC3});
    call.Run();
    CHECK(call.R(AX) == 42 && call.R(SP) == 0x400);
    Machine pusha({0xB8, 0x01, 0x00, 0xBB, 0x02, 0x00, 0x60,  // ax=1 bx=2 / pusha
                   0x31, 0xC0, 0x31, 0xDB, 0x61});            // clear / popa
    pusha.Run();
    CHECK(pusha.R(AX) == 1 && pusha.R(BX) == 2 && pusha.R(SP) == 0x400);
    // enter 4, 1 / leave: nested frame restores SP and BP.
    Machine enter({0xBD, 0x00, 0x03, 0xC8, 0x04, 0x00, 0x01, 0x89, 0xE1, 0xC9});
    enter.Run();  // mov bp,300h / enter 4,1 / mov cx, sp / leave
    CHECK(enter.R(CX) == 0x400 - 2 - 2 - 4 && enter.R(SP) == 0x400 && enter.R(BP) == 0x300);
    Machine push286({0x54, 0x58});  // push sp / pop ax: 286 pushes the old SP
    push286.Run();
    CHECK(push286.R(AX) == 0x400);
}

void TestSegmentsAndAddressing() {
    Machine m({0x8C, 0xD8, 0x8E, 0xC0,                        // mov ax, ds / mov es, ax
               0xC7, 0x06, 0x10, 0x00, 0xCD, 0xAB,            // mov word [10h], ABCDh
               0xBB, 0x0E, 0x00, 0xBE, 0x02, 0x00,            // mov bx, 0Eh / mov si, 2
               0x8B, 0x00,                                    // mov ax, [bx+si]
               0x26, 0x8B, 0x57, 0x02,                        // mov dx, es:[bx+2]
               0x8D, 0x78, 0x05});                            // lea di, [bx+si+5]
    m.Run();
    CHECK(m.Ok() && m.R(AX) == 0xABCD && m.R(DX) == 0xABCD && m.R(DI) == 0x15);

    // LES loads a far pointer (offset, selector) from memory.
    Machine les({0xC7, 0x06, 0x00, 0x00, 0x34, 0x12,  // mov word [0], 1234h
                 0x8C, 0x1E, 0x02, 0x00,              // mov [2], ds
                 0x31, 0xC0, 0x8E, 0xC0,              // xor ax,ax / mov es,ax (null ES)
                 0xC4, 0x1E, 0x00, 0x00});            // les bx, [0]
    les.Run();
    CHECK(les.Ok() && les.R(BX) == 0x1234 && les.cpu.Regs().s[ES] == les.data);

    Machine xlat({0xC6, 0x06, 0x07, 0x00, 0x99, 0x31, 0xDB, 0xB0, 0x07, 0xD7});
    xlat.Run();  // table[7] = 99h / bx = 0 / al = 7 / xlat
    CHECK((xlat.R(AX) & 0xFF) == 0x99);
}

void TestProtectionFaults() {
    Machine write({0x2E, 0xA3, 0x00, 0x00});  // mov cs:[0], ax
    write.Run();
    CHECK(write.Faulted(FaultKind::GeneralProtection));
    Machine limit({0xA1, 0xFF, 0x00});  // mov ax, [FFh]: word straddles the 100h limit
    limit.Run();
    CHECK(limit.Faulted(FaultKind::GeneralProtection));
    Machine bad({0xB8, 0x77, 0x7F, 0x8E, 0xD8});  // mov ds, 7F77h (no such selector)
    bad.Run();
    CHECK(bad.Faulted(FaultKind::GeneralProtection));
    Machine null({0x31, 0xC0, 0x8E, 0xD8, 0xA1, 0x00, 0x00});  // null DS is fine until used
    null.Run();
    CHECK(null.Faulted(FaultKind::GeneralProtection) && null.cpu.Regs().ip == 4);
    Machine io({0xE4, 0x60});  // in al, 60h
    io.Run();
    CHECK(io.Faulted(FaultKind::GeneralProtection));
    Machine ud({0x0F, 0x0B});  // 286 system / 0F opcodes: not yet
    ud.Run();
    CHECK(ud.Faulted(FaultKind::InvalidOpcode));
    Machine fpu({0xD9, 0xD1});  // D9 D1: an undefined x87 encoding
    fpu.Run();
    CHECK(fpu.Faulted(FaultKind::InvalidOpcode));
}

// --- x87 ---------------------------------------------------------------------------------------
// Operands live in the data segment: [00] [08] [10] [18] ... Encodings use
// direct addressing (modrm mod 00, rm 110, disp16).

void TestFpuArithmetic() {
    // fld [0]; fld [8]; faddp; fstp [10] -> 1.5 + 2.25
    Machine add({0xDD, 0x06, 0x00, 0x00, 0xDD, 0x06, 0x08, 0x00, 0xDE, 0xC1, 0xDD, 0x1E, 0x10, 0x00});
    add.PutDouble(0x00, 1.5);
    add.PutDouble(0x08, 2.25);
    add.Run();
    CHECK(add.Ok() && add.Double(0x10) == 3.75);
    CHECK(add.cpu.FpuState().Depth() == 0);

    // The operand order of the reversed forms: a = 10, b = 4.
    //   fld a; fld b; fsubp  -> st1 - st0 = 6      fld a; fld b; fsubrp -> st0 - st1 = -6
    //   fld a; fld b; fdivp  -> 2.5               fld a; fld b; fdivrp -> 0.4
    //   fld a; fsub m64 b -> 6;  fld a; fsubr m64 b -> -6;  fld a; fdivr m64 b -> 0.4
    Machine rev({0xDD, 0x06, 0x00, 0x00, 0xDD, 0x06, 0x08, 0x00, 0xDE, 0xE9, 0xDD, 0x1E, 0x10, 0x00,
                 0xDD, 0x06, 0x00, 0x00, 0xDD, 0x06, 0x08, 0x00, 0xDE, 0xE1, 0xDD, 0x1E, 0x18, 0x00,
                 0xDD, 0x06, 0x00, 0x00, 0xDD, 0x06, 0x08, 0x00, 0xDE, 0xF9, 0xDD, 0x1E, 0x20, 0x00,
                 0xDD, 0x06, 0x00, 0x00, 0xDD, 0x06, 0x08, 0x00, 0xDE, 0xF1, 0xDD, 0x1E, 0x28, 0x00,
                 0xDD, 0x06, 0x00, 0x00, 0xDC, 0x26, 0x08, 0x00, 0xDD, 0x1E, 0x30, 0x00,
                 0xDD, 0x06, 0x00, 0x00, 0xDC, 0x2E, 0x08, 0x00, 0xDD, 0x1E, 0x38, 0x00,
                 0xDD, 0x06, 0x00, 0x00, 0xDC, 0x3E, 0x08, 0x00, 0xDD, 0x1E, 0x40, 0x00});
    rev.PutDouble(0x00, 10);
    rev.PutDouble(0x08, 4);
    rev.Run();
    CHECK(rev.Ok());
    CHECK(rev.Double(0x10) == 6 && rev.Double(0x18) == -6);
    CHECK(rev.Double(0x20) == 2.5 && rev.Double(0x28) == 0.4);
    CHECK(rev.Double(0x30) == 6 && rev.Double(0x38) == -6 && rev.Double(0x40) == 0.4);

    // fld1; fldz; fdivp -> +inf with ZE; fnstsw ax
    Machine zero({0xD9, 0xE8, 0xD9, 0xEE, 0xDE, 0xF9, 0xDD, 0x1E, 0x10, 0x00, 0xDF, 0xE0});
    zero.Run();
    CHECK(zero.Ok() && std::isinf(zero.Double(0x10)) && zero.Double(0x10) > 0);
    CHECK((zero.R(AX) & fpu::ZE) != 0);

    // fld m32 [0] (0.1f), fstp m32 [4]: single precision survives the round trip
    Machine single({0xD9, 0x06, 0x00, 0x00, 0xD9, 0x1E, 0x04, 0x00});
    const float tenth = 0.1f;
    uint32_t bits;
    std::memcpy(&bits, &tenth, 4);
    single.PutLong(0, bits);
    single.Run();
    CHECK(single.Ok() && single.Long(4) == bits);
}

void TestFpuIntegersAndRounding() {
    // fild word [0] (-7); fild dword [2] (100000); faddp; fistp dword [8] -> 99993
    Machine ints({0xDF, 0x06, 0x00, 0x00, 0xDB, 0x06, 0x02, 0x00, 0xDE, 0xC1, 0xDB, 0x1E, 0x08, 0x00});
    ints.PutWord(0, uint16_t(-7));
    ints.PutLong(2, 100000);
    ints.Run();
    CHECK(ints.Ok() && ints.Long(8) == 99993);

    // Rounding modes, through fldcw: 2.5 and -2.5 stored as integers.
    const struct {
        uint16_t control;
        int16_t up, down;  // results for 2.5, -2.5
    } kModes[] = {{0x037F, 2, -2}, {0x077F, 2, -3}, {0x0B7F, 3, -2}, {0x0F7F, 2, -2}};
    for (const auto& mode : kModes) {
        // fldcw [20]; fld [0]; fistp word [10]; fld [8]; fistp word [12]
        Machine r({0xD9, 0x2E, 0x20, 0x00, 0xDD, 0x06, 0x00, 0x00, 0xDF, 0x1E, 0x10, 0x00,
                   0xDD, 0x06, 0x08, 0x00, 0xDF, 0x1E, 0x12, 0x00});
        r.PutDouble(0x00, 2.5);
        r.PutDouble(0x08, -2.5);
        r.PutWord(0x20, mode.control);
        r.Run();
        CHECK(r.Ok() && int16_t(r.Word(0x10)) == mode.up && int16_t(r.Word(0x12)) == mode.down);
    }

    // Out of range for 16 bits: the integer indefinite (8000h) and IE.
    Machine big({0xDD, 0x06, 0x00, 0x00, 0xDF, 0x1E, 0x10, 0x00, 0xDF, 0xE0});
    big.PutDouble(0, 40000);
    big.Run();
    CHECK(big.Ok() && big.Word(0x10) == 0x8000 && (big.R(AX) & fpu::IE));

    // 64-bit integers: fild qword [0]; fistp qword [8]
    Machine wide({0xDF, 0x2E, 0x00, 0x00, 0xDF, 0x3E, 0x08, 0x00});
    wide.PutLong(0, 0x89ABCDEF);
    wide.PutLong(4, 0x00012345);  // 0x0001234589ABCDEF: exact in a double
    wide.Run();
    CHECK(wide.Ok() && wide.Long(8) == 0x89ABCDEF && wide.Long(12) == 0x00012345);

    // BCD: fild dword 1234567 (negated); fbstp [10]; fbld [10]; fistp dword [20]
    Machine bcd({0xDB, 0x06, 0x00, 0x00, 0xD9, 0xE0, 0xDF, 0x36, 0x10, 0x00, 0xDF, 0x26, 0x10, 0x00,
                 0xDB, 0x1E, 0x20, 0x00});
    bcd.PutLong(0, 1234567);
    bcd.Run();
    const uint8_t* packed = bcd.Bytes(0x10);
    CHECK(bcd.Ok() && packed[0] == 0x67 && packed[1] == 0x45 && packed[2] == 0x23 && packed[3] == 0x01 &&
          packed[9] == 0x80);
    CHECK(int32_t(bcd.Long(0x20)) == -1234567);
}

void TestFpuCompareAndBranch() {
    // fld [0] (1); fld [8] (2); fcompp (2 vs 1); fnstsw ax; sahf; ja -> bl = 1
    Machine gt({0xDD, 0x06, 0x00, 0x00, 0xDD, 0x06, 0x08, 0x00, 0xDE, 0xD9, 0xDF, 0xE0, 0x9E,
                0xB3, 0x00, 0x76, 0x02, 0xB3, 0x01});
    gt.PutDouble(0, 1);
    gt.PutDouble(8, 2);
    gt.Run();
    CHECK(gt.Ok() && (gt.R(BX) & 0xFF) == 1 && gt.cpu.FpuState().Depth() == 0);

    // Equal: C3 (ZF after sahf); less: C0 (CF).
    Machine eq({0xD9, 0xE8, 0xD9, 0xE8, 0xDE, 0xD9, 0xDF, 0xE0});
    eq.Run();
    CHECK(eq.Ok() && (eq.R(AX) & (fpu::C3 | fpu::C2 | fpu::C0)) == fpu::C3);
    Machine lt({0xD9, 0xE8, 0xD9, 0xEE, 0xDE, 0xD9, 0xDF, 0xE0});  // fld1; fldz; fcompp: 0 < 1
    lt.Run();
    CHECK(lt.Ok() && (lt.R(AX) & (fpu::C3 | fpu::C2 | fpu::C0)) == fpu::C0);

    // fcom m64 against a NaN: unordered (C3 C2 C0) and IE; fucompp: unordered, no IE
    Machine nan({0xD9, 0xE8, 0xDC, 0x16, 0x00, 0x00, 0xDF, 0xE0});
    nan.PutDouble(0, std::numeric_limits<double>::quiet_NaN());
    nan.Run();
    CHECK(nan.Ok() && (nan.R(AX) & (fpu::C3 | fpu::C2 | fpu::C0)) == (fpu::C3 | fpu::C2 | fpu::C0));
    CHECK((nan.R(AX) & fpu::IE) != 0);
    Machine quiet({0xDD, 0x06, 0x00, 0x00, 0xD9, 0xE8, 0xDA, 0xE9, 0xDF, 0xE0});
    quiet.PutDouble(0, std::numeric_limits<double>::quiet_NaN());
    quiet.Run();
    CHECK(quiet.Ok() && (quiet.R(AX) & fpu::IE) == 0 && (quiet.R(AX) & fpu::C2));

    // ftst on -3; fxam on +0 (C3) and on an empty register (C3 C0)
    Machine tst({0xDD, 0x06, 0x00, 0x00, 0xD9, 0xE4, 0xDF, 0xE0});
    tst.PutDouble(0, -3);
    tst.Run();
    CHECK(tst.Ok() && (tst.R(AX) & (fpu::C3 | fpu::C2 | fpu::C0)) == fpu::C0);
    Machine xam({0xD9, 0xEE, 0xD9, 0xE5, 0xDF, 0xE0});
    xam.Run();
    CHECK(xam.Ok() && (xam.R(AX) & (fpu::C3 | fpu::C2 | fpu::C0)) == fpu::C3);
    Machine empty({0xDB, 0xE3, 0xD9, 0xE5, 0xDF, 0xE0});  // fninit; fxam
    empty.Run();
    CHECK(empty.Ok() && (empty.R(AX) & (fpu::C3 | fpu::C2 | fpu::C0)) == (fpu::C3 | fpu::C0));
}

void TestFpuStackAndState() {
    // fld1; fldpi; fxch; fstp [0] (1); fstp [8] (pi)
    Machine xch({0xD9, 0xE8, 0xD9, 0xEB, 0xD9, 0xC9, 0xDD, 0x1E, 0x00, 0x00, 0xDD, 0x1E, 0x08, 0x00});
    xch.Run();
    CHECK(xch.Ok() && xch.Double(0) == 1 && xch.Double(8) == std::numbers::pi);

    // fstp from an empty stack: stack underflow (IE, SF), a NaN stored
    Machine under({0xDD, 0x1E, 0x00, 0x00, 0xDF, 0xE0});
    under.Run();
    CHECK(under.Ok() && std::isnan(under.Double(0)));
    CHECK((under.R(AX) & (fpu::IE | fpu::SF)) == (fpu::IE | fpu::SF));

    // Nine pushes overflow the eight registers: IE, SF and C1
    Machine over({0xD9, 0xE8, 0xD9, 0xE8, 0xD9, 0xE8, 0xD9, 0xE8, 0xD9, 0xE8, 0xD9, 0xE8, 0xD9, 0xE8,
                  0xD9, 0xE8, 0xD9, 0xE8, 0xDF, 0xE0});
    over.Run();
    CHECK(over.Ok() && (over.R(AX) & (fpu::IE | fpu::SF | fpu::C1)) == (fpu::IE | fpu::SF | fpu::C1));

    // 80-bit: fldpi; fstp tbyte [0]; fld tbyte [0]; fstp [10]
    Machine ext({0xD9, 0xEB, 0xDB, 0x3E, 0x00, 0x00, 0xDB, 0x2E, 0x00, 0x00, 0xDD, 0x1E, 0x10, 0x00});
    ext.Run();
    const uint8_t kPi80[10] = {0x00, 0xC0, 0x68, 0x21, 0xA2, 0xDA, 0x0F, 0xC9, 0x00, 0x40};
    CHECK(ext.Ok() && std::memcmp(ext.Bytes(0), kPi80, 10) == 0 && ext.Double(0x10) == std::numbers::pi);

    // fnstcw after finit: 037F; fsave [20] empties the stack, frstor [20] brings it back
    Machine save({0x9B, 0xDB, 0xE3, 0xD9, 0x3E, 0x00, 0x00, 0xD9, 0xE8, 0xD9, 0xEB, 0xDD, 0x36, 0x20, 0x00,
                  0xD9, 0xE5, 0xDF, 0xE0, 0x89, 0xC3,              // fxam; fnstsw ax; mov bx, ax
                  0xDD, 0x26, 0x20, 0x00, 0xDD, 0x1E, 0x08, 0x00, 0xDD, 0x1E, 0x10, 0x00});
    save.Run();
    CHECK(save.Ok() && save.Word(0) == 0x037F);
    CHECK((save.R(BX) & (fpu::C3 | fpu::C2 | fpu::C0)) == (fpu::C3 | fpu::C0));  // empty after fsave
    CHECK(save.Double(8) == std::numbers::pi && save.Double(0x10) == 1);
}

void TestFpuFunctions() {
    auto near = [](double a, double b) { return std::fabs(a - b) < 1e-12; };
    // sqrt(2): fld1; fld1; faddp; fsqrt; fstp [10]
    Machine sqrt2({0xD9, 0xE8, 0xD9, 0xE8, 0xDE, 0xC1, 0xD9, 0xFA, 0xDD, 0x1E, 0x10, 0x00});
    sqrt2.Run();
    CHECK(sqrt2.Ok() && near(sqrt2.Double(0x10), std::sqrt(2.0)));

    // fpatan(1, 1) = pi/4; fptan pushes 1.0 after tan(x)
    Machine atan({0xD9, 0xE8, 0xD9, 0xE8, 0xD9, 0xF3, 0xDD, 0x1E, 0x10, 0x00});
    atan.Run();
    CHECK(atan.Ok() && near(atan.Double(0x10), std::numbers::pi / 4));
    Machine tan({0xDD, 0x06, 0x00, 0x00, 0xD9, 0xF2, 0xDD, 0x1E, 0x10, 0x00, 0xDD, 0x1E, 0x18, 0x00});
    tan.PutDouble(0, std::numbers::pi / 4);
    tan.Run();
    CHECK(tan.Ok() && tan.Double(0x10) == 1.0 && near(tan.Double(0x18), 1.0));

    // fyl2x: 1 * log2(8) = 3; fscale: 3 * 2^4 = 48; f2xm1(0.5) = sqrt(2) - 1
    Machine log({0xD9, 0xE8, 0xDD, 0x06, 0x00, 0x00, 0xD9, 0xF1, 0xDD, 0x1E, 0x10, 0x00});
    log.PutDouble(0, 8);
    log.Run();
    CHECK(log.Ok() && near(log.Double(0x10), 3));
    Machine scale({0xDD, 0x06, 0x08, 0x00, 0xDD, 0x06, 0x00, 0x00, 0xD9, 0xFD, 0xDD, 0x1E, 0x10, 0x00});
    scale.PutDouble(0, 3);
    scale.PutDouble(8, 4);
    scale.Run();
    CHECK(scale.Ok() && scale.Double(0x10) == 48);
    Machine exp({0xDD, 0x06, 0x00, 0x00, 0xD9, 0xF0, 0xDD, 0x1E, 0x10, 0x00});
    exp.PutDouble(0, 0.5);
    exp.Run();
    CHECK(exp.Ok() && near(exp.Double(0x10), std::sqrt(2.0) - 1));

    // fprem: 10 mod 3 = 1, quotient 3 -> C1 (bit 0) and C3 (bit 1)
    Machine rem({0xDD, 0x06, 0x08, 0x00, 0xDD, 0x06, 0x00, 0x00, 0xD9, 0xF8, 0xDF, 0xE0,
                 0xDD, 0x1E, 0x10, 0x00});
    rem.PutDouble(0, 10);
    rem.PutDouble(8, 3);
    rem.Run();
    CHECK(rem.Ok() && rem.Double(0x10) == 1);
    CHECK((rem.R(AX) & (fpu::C0 | fpu::C1 | fpu::C2 | fpu::C3)) == (fpu::C1 | fpu::C3));

    // frndint with the default rounding: 2.5 -> 2; fsincos(0): sin 0, then cos 1 on top
    Machine rnd({0xDD, 0x06, 0x00, 0x00, 0xD9, 0xFC, 0xDD, 0x1E, 0x10, 0x00});
    rnd.PutDouble(0, 2.5);
    rnd.Run();
    CHECK(rnd.Ok() && rnd.Double(0x10) == 2);
    Machine sincos({0xD9, 0xEE, 0xD9, 0xFB, 0xDD, 0x1E, 0x10, 0x00, 0xDD, 0x1E, 0x18, 0x00});
    sincos.Run();
    CHECK(sincos.Ok() && sincos.Double(0x10) == 1 && sincos.Double(0x18) == 0);
}

void TestFpuEmulatorInterrupts() {
    // Microsoft's emulator encoding: INT 35h E8 = fld1 (D9 E8); INT 3Bh 1E 10 00 = fistp word [10] (DF);
    // INT 3Ch C5 1E 20 00 = fstp qword es:[20] (segment ES, opcode DD); INT 3Dh = fwait.
    Machine emu({0xD9, 0xE8, 0xD9, 0xE8, 0xDE, 0xC1,  // fld1; fld1; faddp -> 2
                 0xCD, 0x35, 0xE8,                    // fld1 (emulated)
                 0xCD, 0x3B, 0x1E, 0x10, 0x00,        // fistp word [10] -> 1
                 0xCD, 0x3D,                          // fwait
                 0xCD, 0x3C, 0xC5, 0x1E, 0x20, 0x00});  // fstp qword es:[20] -> 2
    emu.Run();
    CHECK(emu.Ok() && emu.Word(0x10) == 1 && emu.Double(0x20) == 2);
    CHECK(emu.cpu.FpuState().Depth() == 0);
}

void TestFpuConversions() {
    const double kValues[] = {0.0, -0.0, 1.0, -2.5, std::numbers::pi, 1e300, -1e-300, 4.9e-324 /* denormal */,
                              std::numeric_limits<double>::max(), std::numeric_limits<double>::infinity(),
                              -std::numeric_limits<double>::infinity()};
    for (double v : kValues) {
        uint8_t ext[10];
        Fpu::ToExtended(v, ext);
        const double back = Fpu::FromExtended(ext);
        CHECK(back == v && std::signbit(back) == std::signbit(v));
    }
    uint8_t ext[10];
    Fpu::ToExtended(std::numeric_limits<double>::quiet_NaN(), ext);
    CHECK(std::isnan(Fpu::FromExtended(ext)));
    // 1.0 in extended precision: 3FFF 8000000000000000
    Fpu::ToExtended(1.0, ext);
    const uint8_t kOne[10] = {0, 0, 0, 0, 0, 0, 0, 0x80, 0xFF, 0x3F};
    CHECK(std::memcmp(ext, kOne, 10) == 0);

    Fpu f;
    CHECK(f.RoundInt(2.5) == 2 && f.RoundInt(3.5) == 4 && f.RoundInt(-2.5) == -2 && f.RoundInt(-0.4) == 0);
    CHECK(f.ToInteger(32767.4, 16) == 32767 && (f.status & fpu::PE) && !(f.status & fpu::IE));
    CHECK(f.ToInteger(32768, 16) == -32768 && (f.status & fpu::IE));

    uint8_t bcd[10];
    f.ToBcd(-90210, bcd);
    CHECK(bcd[0] == 0x10 && bcd[1] == 0x02 && bcd[2] == 0x09 && bcd[9] == 0x80 && Fpu::FromBcd(bcd) == -90210);
    f.status = 0;
    f.ToBcd(1e19, bcd);
    CHECK((f.status & fpu::IE) && bcd[9] == 0xFF && bcd[8] == 0xFF && bcd[7] == 0xC0);
}

void TestHostSegmentCalls() {
    // A far call into a host segment runs C++ with Pascal arguments.
    Memory mem(1 << 20);
    Cpu cpu(mem);
    const uint16_t host = mem.Allocate(0x10000, SegmentKind::Host);
    const uint8_t program[] = {0x6A, 0x05, 0x6A, 0x07,               // push 5 / push 7
                               0x9A, 0x2A, 0x00, 0x00, 0x00,         // call far host:002A
                               0xCC};
    const uint16_t code = mem.Allocate(sizeof(program), SegmentKind::Code);
    std::copy(std::begin(program), std::end(program), mem.SegmentData(code));
    mem.SegmentData(code)[7] = uint8_t(host);
    mem.SegmentData(code)[8] = uint8_t(host >> 8);
    const uint16_t stack = mem.Allocate(0x100, SegmentKind::Data);

    uint16_t seenIp = 0;
    cpu.MapHostSegment(host, [&](Cpu& c, uint16_t ip) {
        seenIp = ip;
        // Pascal: the last argument pushed is nearest the return address.
        c.Regs().r[AX] = uint16_t(c.StackArg(2) * 10 + c.StackArg(0));  // 5*10 + 7
        c.ReturnFar(4);
    });
    cpu.SetInterruptHandler([](Cpu& c, uint8_t) { c.Stop(); return true; });
    cpu.Regs().s[CS] = code;
    cpu.Regs().s[SS] = stack;
    cpu.Regs().r[SP] = 0x100;
    CHECK(cpu.Run(100) == RunResult::Stopped);
    CHECK(seenIp == 0x2A && cpu.Regs().r[AX] == 57 && cpu.Regs().r[SP] == 0x100);
}

}  // namespace

int main() {
    const test::Case cases[] = {
        {"AddFlags", TestAddFlags},
        {"SubAndCompare", TestSubAndCompare},
        {"CarryChains", TestCarryChains},
        {"IncDecKeepCarry", TestIncDecKeepCarry},
        {"LogicNegNot", TestLogicNegNot},
        {"ShiftsAndRotates", TestShiftsAndRotates},
        {"MultiplyDivide", TestMultiplyDivide},
        {"ConversionsAndBcd", TestConversionsAndBcd},
        {"ConditionsAndLoops", TestConditionsAndLoops},
        {"StringInstructions", TestStringInstructions},
        {"StackAndCalls", TestStackAndCalls},
        {"SegmentsAndAddressing", TestSegmentsAndAddressing},
        {"ProtectionFaults", TestProtectionFaults},
        {"HostSegmentCalls", TestHostSegmentCalls},
        {"FpuArithmetic", TestFpuArithmetic},
        {"FpuIntegersAndRounding", TestFpuIntegersAndRounding},
        {"FpuCompareAndBranch", TestFpuCompareAndBranch},
        {"FpuStackAndState", TestFpuStackAndState},
        {"FpuFunctions", TestFpuFunctions},
        {"FpuEmulatorInterrupts", TestFpuEmulatorInterrupts},
        {"FpuConversions", TestFpuConversions},
    };
    return test::RunAll(cases);
}
