#include "win16/Runtime.h"

#include <algorithm>
#include <cctype>
#include <cstdio>

namespace retro::win16 {
namespace {

std::string Upper(std::string s) {
    for (char& c : s) c = char(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

std::string Hex(unsigned v, int digits) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%0*X", digits, v);
    return buf;
}

const char* FaultName(FaultKind k) {
    switch (k) {
    case FaultKind::GeneralProtection: return "general protection fault";
    case FaultKind::InvalidOpcode: return "invalid opcode";
    case FaultKind::DivideError: return "divide error";
    case FaultKind::UnhandledInterrupt: return "unhandled interrupt";
    default: return "host error";
    }
}

std::vector<Runtime::BuiltinModule> MakeBuiltins() {
    return {
        {"KERNEL", KernelApi()},
        {"USER", UserApi()},
        {"GDI", GdiApi()},
    };
}

constexpr uint16_t kScratchBytes = 0x1000;

}  // namespace

const char* ToString(TaskExit::Kind kind) {
    switch (kind) {
    case TaskExit::Kind::Exited: return "exited";
    case TaskExit::Kind::FatalExit: return "fatal exit";
    case TaskExit::Kind::Unimplemented: return "stopped at an unimplemented API";
    case TaskExit::Kind::Blocked: return "blocked waiting for input";
    case TaskExit::Kind::Fault: return "CPU fault";
    default: return "instruction budget exhausted";
    }
}

class Runtime::Resolver : public ImportResolver {
public:
    explicit Resolver(Runtime& rt) : rt_(rt) {}

    bool Resolve(const std::string& moduleName, uint16_t ordinal, const std::string& name,
                 uint16_t& selector, uint16_t& offset, std::string& error) override {
        const std::string wanted = Upper(moduleName);
        for (BuiltinModule& m : rt_.builtins_) {
            if (wanted != m.name) continue;
            selector = m.selector;
            if (name.empty()) {
                offset = ordinal;
                return true;
            }
            const std::string upperName = Upper(name);
            for (const ApiFunction& f : m.functions) {
                if (upperName == f.name) {
                    offset = f.ordinal;
                    return true;
                }
            }
            // Unknown name: give it a private ordinal so the call reports the name.
            m.unknownNames.push_back(upperName);
            offset = uint16_t(0xF000 + m.unknownNames.size() - 1);
            return true;
        }
        error = "module " + moduleName +
                " is not available (built in: KERNEL, USER, GDI; loading other DLLs comes later)";
        return false;
    }

private:
    Runtime& rt_;
};

Runtime::Runtime()
    : cpu_(memory_),
      globals_(memory_),
      user_(std::make_unique<User>(*this)),
      builtins_(MakeBuiltins()) {
    scratchSel_ = memory_.Allocate(kScratchBytes, SegmentKind::Data);
}

Runtime::~Runtime() = default;

void Runtime::Print(const std::string& line) const {
    if (output_) output_(line);
}

uint32_t Runtime::TickCount() const {
    return uint32_t(std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - start_)
                        .count());
}

Runtime::Scratch::Scratch(Runtime& rt, uint16_t bytes) : rt_(rt), offset_(rt.scratchTop_) {
    const uint32_t end = uint32_t(offset_) + ((bytes + 15u) & ~15u);
    if (end > kScratchBytes) rt.cpu_.HostFault("scratch memory exhausted (callbacks nested too deeply)");
    rt.scratchTop_ = uint16_t(end);
}

Runtime::Scratch::~Scratch() { rt_.scratchTop_ = offset_; }

void Runtime::Exit(TaskExit::Kind kind, uint16_t code, const std::string& message) {
    if (exited_) return;
    exited_ = true;
    exit_.kind = kind;
    exit_.code = code;
    exit_.message = message;
    cpu_.Stop();
}

bool Runtime::Load(const std::vector<uint8_t>& file, const std::string& commandLine,
                   std::string& error) {
    if (!ParseNe(file, image_, error)) return false;
    if (image_.IsLibrary()) {
        error = "module " + image_.moduleName + " is a DLL, not a program";
        return false;
    }

    for (size_t i = 0; i < builtins_.size(); ++i) {
        builtins_[i].selector = memory_.Allocate(0x10000, SegmentKind::Host);
        cpu_.MapHostSegment(builtins_[i].selector,
                            [this, i](Cpu&, uint16_t ip) { CallApi(builtins_[i], ip); });
    }
    cpu_.SetInterruptHandler([this](Cpu&, uint8_t vector) { return Interrupt(vector); });

    Resolver resolver(*this);
    if (!LoadNe(image_, file, memory_, resolver, commandLine, module_, error)) return false;
    cpu_.Regs() = module_.initial;
    return true;
}

TaskExit Runtime::Run(uint64_t budget) {
    const RunResult result = cpu_.Run(budget);
    TaskExit out;
    if (result == RunResult::Faulted) {
        const CpuFault& f = cpu_.Fault();
        out.kind = TaskExit::Kind::Fault;
        out.message = std::string(FaultName(f.kind)) + " at " + Hex(f.cs, 4) + ":" + Hex(f.ip, 4) +
                      ": " + f.detail;
    } else if (result == RunResult::BudgetExhausted && !exited_) {
        out.kind = TaskExit::Kind::BudgetExhausted;
    } else {
        out = exit_;
    }
    out.registers = cpu_.Regs();
    out.instructions = cpu_.Instructions();
    return out;
}

void Runtime::CallApi(BuiltinModule& module, uint16_t ip) {
    for (const ApiFunction& f : module.functions) {
        if (f.ordinal == ip) {
            f.impl(*this, cpu_);
            return;
        }
    }
    std::string what = std::string(module.name) + ".";
    if (ip >= 0xF000 && ip - 0xF000u < module.unknownNames.size()) {
        what += module.unknownNames[ip - 0xF000];
    } else {
        what += std::to_string(ip);
    }
    Exit(TaskExit::Kind::Unimplemented, 0, what + " is not implemented yet");
}

bool Runtime::Interrupt(uint8_t vector) {
    switch (vector) {
    case 0x20: Exit(TaskExit::Kind::Exited, 0, "INT 20h"); return true;
    case 0x21: DosService(); return true;
    case 0x31: Int31(); return true;
    default: return false;
    }
}

namespace {

// KERNEL.DOS3Call: INT 21h as a far call, same registers in and out.
void Dos3Call(Runtime& rt, Cpu& cpu) {
    rt.DosService();
    if (!rt.HasExited()) cpu.ReturnFar(0);
}

}  // namespace

void Runtime::DosService() {
    Registers& r = cpu_.Regs();
    const uint8_t ah = r.R8(4);
    cpu_.SetFlag(flags::CF, false);
    switch (ah) {
    case 0x4C:
        Exit(TaskExit::Kind::Exited, r.R8(0), "");
        return;
    case 0x30:  // DOS version 5.00
        r.r[AX] = 0x0005;
        r.r[BX] = 0;
        r.r[CX] = 0;
        return;
    case 0x02:
        Print(std::string(1, char(r.R8(2))));
        return;
    case 0x09: {  // $-terminated string at DS:DX
        std::string s;
        for (uint16_t off = r.r[DX]; s.size() < 4096; ++off) {
            const char c = char(memory_.Read8(r.s[DS], off));
            if (c == '$') break;
            s.push_back(c);
        }
        Print(s);
        return;
    }
    case 0x19: r.R8(0) = 2; return;  // current drive: C:
    case 0x25: return;               // set interrupt vector: ignored
    case 0x35:                        // get interrupt vector: none installed
        cpu_.LoadSegment(ES, 0);
        r.r[BX] = 0;
        return;
    case 0x51:
    case 0x62: r.r[BX] = module_.psp; return;
    default:
        Print("INT 21h AH=" + Hex(ah, 2) + "h not supported yet (returned an error)");
        cpu_.SetFlag(flags::CF, true);
        r.r[AX] = 1;  // invalid function
        return;
    }
}

void Runtime::Int31() {
    Registers& r = cpu_.Regs();
    cpu_.SetFlag(flags::CF, false);
    switch (r.r[AX]) {
    case 0x0006: {  // get segment base address
        const Descriptor* d = memory_.Lookup(r.r[BX]);
        if (!d) {
            cpu_.SetFlag(flags::CF, true);
            r.r[AX] = 0x8022;  // invalid selector
            return;
        }
        r.r[CX] = uint16_t(d->base >> 16);
        r.r[DX] = uint16_t(d->base);
        return;
    }
    case 0x0400:  // DPMI version 0.90 on a 386
        r.r[AX] = 0x005A;
        r.r[BX] = 0x0005;
        r.R8(1) = 3;
        r.r[DX] = 0x0870;
        return;
    default:
        Print("INT 31h AX=" + Hex(r.r[AX], 4) + "h not supported yet (returned an error)");
        cpu_.SetFlag(flags::CF, true);
        r.r[AX] = 0x8001;  // unsupported function
        return;
    }
}

}  // namespace retro::win16
