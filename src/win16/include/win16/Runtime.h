#pragma once

// A Win16 task: loads an NE program into its own segmented address space and
// runs it on the interpreter until it exits.
//
// System DLLs are built in (Kernel.cpp, User.cpp, Gdi.cpp). Each one is a
// host segment; the loader resolves an import MODULE.ordinal to the far
// address module-selector:ordinal, so a call lands in the matching C++
// routine. Calling an API that isn't implemented yet stops the task cleanly
// with "USER.39 is not implemented yet" instead of crashing.
//
// Interrupts: INT 21h (the DOS calls Windows programs use: exit, version,
// output), INT 31h (DPMI queries), INT 20h (terminate).

#include <chrono>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include "win16/Api.h"
#include "win16/Cpu.h"
#include "win16/Gdi.h"
#include "win16/Kernel.h"
#include "win16/Memory.h"
#include "win16/NeImage.h"
#include "win16/NeLoader.h"
#include "win16/Resources.h"
#include "win16/User.h"

namespace retro::win16 {

struct TaskExit {
    enum class Kind {
        Exited,          // INT 21h/4Ch, INT 20h
        FatalExit,       // KERNEL.FatalExit / FatalAppExit
        Unimplemented,   // called an API or interrupt service we don't have yet
        Blocked,         // waiting for input that can never arrive (headless host)
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
    // Where top-level windows appear (default: headless, no real windows).
    void SetWindowHost(WindowHost* host) { user_->SetHost(host); }
    // Frames presented per second at most (0 = unpaced). Default 60.
    void SetFrameCap(uint32_t fps) { gdi_->SetFrameCap(fps); }

    bool Load(const std::vector<uint8_t>& file, const std::string& commandLine, std::string& error);
    TaskExit Run(uint64_t budget = std::numeric_limits<uint64_t>::max());

    Memory& Mem() { return memory_; }
    Cpu& Processor() { return cpu_; }
    const NeImage& Image() const { return image_; }
    const LoadedModule& Module() const { return module_; }
    GlobalHeap& Globals() { return globals_; }
    Resources& Resource() { return resources_; }
    User& Windows() { return *user_; }
    Gdi& Graphics() { return *gdi_; }

    // Used by the built-in API implementations.
    void Exit(TaskExit::Kind kind, uint16_t code, const std::string& message);
    bool HasExited() const { return exited_; }
    void Print(const std::string& line) const;
    void DosService();  // INT 21h with the current registers (also KERNEL.DOS3Call)
    uint32_t TickCount() const;  // milliseconds since the task started

    // Short-lived 16-bit memory for structures handed to a callback (the
    // CREATESTRUCT of WM_CREATE, ...). Nested scopes stack; released on scope exit.
    class Scratch {
    public:
        Scratch(Runtime& rt, uint16_t bytes);
        ~Scratch();
        Scratch(const Scratch&) = delete;
        Scratch& operator=(const Scratch&) = delete;
        uint16_t Selector() const { return rt_.scratchSel_; }
        uint16_t Offset() const { return offset_; }

    private:
        Runtime& rt_;
        uint16_t offset_;
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
    GlobalHeap globals_;
    NeImage image_;
    Resources resources_{image_, globals_, memory_};
    std::unique_ptr<User> user_;
    std::unique_ptr<Gdi> gdi_;  // after user_: destroyed first
    LoadedModule module_;
    std::vector<BuiltinModule> builtins_;
    Output output_;
    TaskExit exit_;
    bool exited_ = false;
    uint16_t scratchSel_ = 0;
    uint16_t scratchTop_ = 0;
    std::chrono::steady_clock::time_point start_ = std::chrono::steady_clock::now();
};

}  // namespace retro::win16
