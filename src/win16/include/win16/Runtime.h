#pragma once

// A Win16 task: loads an NE program into its own segmented address space and
// runs it on the interpreter until it exits.
//
// System DLLs (KERNEL, USER, GDI) are built in. Each one is a host segment;
// the loader resolves an import MODULE.ordinal to the far address
// module-selector:ordinal, so a call lands in the matching C++ routine.
// Calling an API that isn't implemented yet stops the task cleanly with
// "USER.41 (CREATEWINDOW) is not implemented" instead of crashing.
//
// Interrupts: INT 21h (the DOS calls Windows programs use: exit, version,
// output), INT 31h (DPMI queries), INT 20h (terminate).

#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include "win16/Cpu.h"
#include "win16/Memory.h"
#include "win16/NeImage.h"
#include "win16/NeLoader.h"

namespace retro::win16 {

struct TaskExit {
    enum class Kind {
        Exited,          // INT 21h/4Ch, INT 20h
        FatalExit,       // KERNEL.FatalExit / FatalAppExit
        Unimplemented,   // called an API or interrupt service we don't have yet
        Fault,           // CPU fault (#GP, #DE, invalid opcode, ...)
        BudgetExhausted,
    };
    Kind kind = Kind::Exited;
    uint16_t code = 0;
    std::string message;
    Registers registers;       // at the point the task stopped
    uint64_t instructions = 0;
};

const char* ToString(TaskExit::Kind kind);

class Runtime {
public:
    using Output = std::function<void(const std::string& line)>;

    Runtime();
    ~Runtime();

    // Program output (DOS console writes, MessageBox text) and diagnostics.
    void SetOutput(Output output) { output_ = std::move(output); }

    bool Load(const std::vector<uint8_t>& file, const std::string& commandLine, std::string& error);
    TaskExit Run(uint64_t budget = std::numeric_limits<uint64_t>::max());

    Memory& Mem() { return memory_; }
    Cpu& Processor() { return cpu_; }
    const NeImage& Image() const { return image_; }
    const LoadedModule& Module() const { return module_; }

    // Used by the built-in API implementations.
    void Exit(TaskExit::Kind kind, uint16_t code, const std::string& message);
    bool HasExited() const { return exited_; }
    void Print(const std::string& line) const;
    void DosService();  // INT 21h with the current registers (also KERNEL.DOS3Call)

    struct ApiFunction {
        uint16_t ordinal;
        const char* name;
        void (*impl)(Runtime&, Cpu&);
    };
    struct BuiltinModule {
        const char* name;
        std::vector<ApiFunction> functions;
        uint16_t selector = 0;
        std::vector<std::string> unknownNames;  // by-name imports we don't know
    };

private:
    class Resolver;

    void CallApi(BuiltinModule& module, uint16_t ip);
    bool Interrupt(uint8_t vector);
    void Int31();

    Memory memory_;
    Cpu cpu_;
    NeImage image_;
    LoadedModule module_;
    std::vector<BuiltinModule> builtins_;
    Output output_;
    TaskExit exit_;
    bool exited_ = false;
};

}  // namespace retro::win16
