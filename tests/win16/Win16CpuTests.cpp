// Instruction-level tests of the 16-bit interpreter: each case runs a short
// hand-assembled snippet ending in INT 3 and checks registers, flags, memory
// or the fault it raised. Expected values follow real x86 behaviour.

#include <cstdio>
#include <initializer_list>
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
    Machine fpu({0xD9, 0xE8});  // fld1: no x87 emulation yet
    fpu.Run();
    CHECK(fpu.Faulted(FaultKind::InvalidOpcode));
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
    };
    return test::RunAll(cases);
}
