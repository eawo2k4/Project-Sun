#pragma once

// A 286-class 16-bit x86 interpreter for protected-mode Win16 programs.
//
// Covers the 8086/80186 integer instruction set (ALU, shifts/rotates,
// MUL/DIV, BCD adjust, string ops with REP, near/far CALL/JMP/RET, ENTER/
// LEAVE, PUSHA/POPA, IMUL immediate, LES/LDS, INT/IRET) with 16-bit
// addressing and 286 protected-mode segment loads through the virtual LDT
// (win16/Memory.h). Not yet: 386 operand/address-size prefixes, the 286
// system instructions (0F xx), x87, I/O ports.
//
// Host integration:
//   * INT n goes to the interrupt handler (DOS INT 21h, DPMI INT 31h, ...).
//   * A control transfer into a host segment (Memory SegmentKind::Host) runs
//     that segment's C++ handler instead of fetching code. The loader points
//     imported KERNEL/USER/GDI functions there; the handler reads Pascal-
//     convention arguments off the stack and returns with ReturnFar().

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "win16/Memory.h"

namespace retro::win16 {

enum Reg : uint8_t { AX, CX, DX, BX, SP, BP, SI, DI };
enum SegReg : uint8_t { ES, CS, SS, DS };

namespace flags {
constexpr uint16_t CF = 0x0001;
constexpr uint16_t PF = 0x0004;
constexpr uint16_t AF = 0x0010;
constexpr uint16_t ZF = 0x0040;
constexpr uint16_t SF = 0x0080;
constexpr uint16_t TF = 0x0100;
constexpr uint16_t IF = 0x0200;
constexpr uint16_t DF = 0x0400;
constexpr uint16_t OF = 0x0800;
constexpr uint16_t kReset = 0x0202;  // reserved bit 1 + interrupts enabled
}  // namespace flags

struct Registers {
    uint16_t r[8] = {};
    uint16_t s[4] = {};
    uint16_t ip = 0;
    uint16_t flags = flags::kReset;

    uint8_t& R8(int index) {  // AL CL DL BL AH CH DH BH
        return reinterpret_cast<uint8_t*>(&r[index & 3])[index >> 2];
    }
};

enum class FaultKind { GeneralProtection, InvalidOpcode, DivideError, UnhandledInterrupt, HostError };

struct CpuFault {
    FaultKind kind = FaultKind::GeneralProtection;
    uint16_t cs = 0;
    uint16_t ip = 0;
    std::string detail;
};

enum class RunResult { Stopped, Faulted, BudgetExhausted };

class Cpu {
public:
    using InterruptHandler = std::function<bool(Cpu&, uint8_t vector)>;
    using HostHandler = std::function<void(Cpu&, uint16_t ip)>;

    explicit Cpu(Memory& memory) : mem_(memory) {}

    Registers& Regs() { return regs_; }
    const Registers& Regs() const { return regs_; }
    Memory& Mem() { return mem_; }

    void SetInterruptHandler(InterruptHandler handler) { interrupt_ = std::move(handler); }
    void MapHostSegment(uint16_t selector, HostHandler handler);

    // Checked protected-mode segment load (throws ProtectionFault).
    void LoadSegment(SegReg reg, uint16_t selector);

    // Executes until Stop(), a fault, or `budget` instructions.
    RunResult Run(uint64_t budget);
    void Stop() { stop_ = true; }

    const CpuFault& Fault() const { return fault_; }
    uint64_t Instructions() const { return instructions_; }

    bool Flag(uint16_t f) const { return (regs_.flags & f) != 0; }
    void SetFlag(uint16_t f, bool on) { regs_.flags = on ? (regs_.flags | f) : (regs_.flags & ~f); }

    // Helpers for host routines.
    void Push(uint16_t value);
    uint16_t Pop();
    // Word argument `byteOffset` bytes above the far return address.
    uint16_t StackArg(uint16_t byteOffset) const;
    // Returns to the far caller, removing `argBytes` of Pascal arguments.
    void ReturnFar(uint16_t argBytes);
    // Raises a fault from a host routine (unimplemented API, bad argument...).
    [[noreturn]] void HostFault(const std::string& detail);

private:
    struct ModRM {
        uint8_t mod = 0, reg = 0, rm = 0;
        uint8_t seg = DS;
        uint16_t ea = 0;
    };

    void Step();
    void Execute(uint8_t op);

    uint8_t Fetch8();
    uint16_t Fetch16();
    ModRM DecodeModRM();
    uint8_t GetRM8(const ModRM& m);
    uint16_t GetRM16(const ModRM& m);
    void SetRM8(const ModRM& m, uint8_t v);
    void SetRM16(const ModRM& m, uint16_t v);
    uint8_t SegOr(uint8_t defaultSeg) const { return segOverride_ >= 0 ? uint8_t(segOverride_) : defaultSeg; }

    uint8_t Read8(uint8_t seg, uint16_t off) { return mem_.Read8(regs_.s[seg], off); }
    uint16_t Read16(uint8_t seg, uint16_t off) { return mem_.Read16(regs_.s[seg], off); }
    void Write8(uint8_t seg, uint16_t off, uint8_t v) { mem_.Write8(regs_.s[seg], off, v); }
    void Write16(uint8_t seg, uint16_t off, uint16_t v) { mem_.Write16(regs_.s[seg], off, v); }

    // Arithmetic with flags; `word` selects 16- vs 8-bit.
    uint16_t Alu(int op, uint16_t a, uint16_t b, bool word);
    uint16_t Add(uint32_t a, uint32_t b, uint32_t carry, bool word);
    uint16_t Sub(uint32_t a, uint32_t b, uint32_t borrow, bool word);
    uint16_t Logic(uint16_t r, bool word);
    uint16_t IncDec(uint16_t v, bool inc, bool word);
    uint16_t Shift(int op, uint16_t v, uint8_t count, bool word);
    void SetSZP(uint16_t r, bool word);
    void Group3(const ModRM& m, bool word);
    void MulDiv(int op, uint16_t src, bool word);
    void StringOp(uint8_t op);
    void Interrupt(uint8_t vector);
    void FarJump(uint16_t selector, uint16_t offset);
    bool Condition(int cc) const;

    [[noreturn]] void Throw(FaultKind kind, const std::string& detail);

    Memory& mem_;
    Registers regs_;
    InterruptHandler interrupt_;
    struct HostSegment {
        uint16_t selector;
        HostHandler handler;
    };
    std::vector<HostSegment> host_;

    int segOverride_ = -1;
    int rep_ = 0;  // 0, 0xF2 (REPNE) or 0xF3 (REP/REPE)
    uint16_t startIp_ = 0;
    bool stop_ = false;
    uint64_t instructions_ = 0;
    CpuFault fault_;
};

}  // namespace retro::win16
