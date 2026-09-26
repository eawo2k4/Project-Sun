#pragma once

// A Win16 task: loads an NE program into its own segmented address space and
// runs it on the interpreter until it exits.
//
// System DLLs are built in (Kernel.cpp, User.cpp, Gdi.cpp, WIN87EM). Each
// one is a host segment; the loader resolves an import MODULE.ordinal to the
// far address module-selector:ordinal, so a call lands in the matching C++
// routine. Every other imported DLL (SHELL, MMSYSTEM, a game's own DLL, ...)
// also gets a host segment, so the program loads; calling into it, or any
// API that isn't implemented yet, stops the task cleanly with e.g.
// "USER.216 (GetDlgItem) is not implemented yet (returning to 0127:04A2)".
// Imports of constants (__AHINCR, __WINFLAGS, ...) resolve to their values.
// Names, parameters and constants come from the API catalog (ApiCatalog.h).
//
// Diagnostics: SetTrace logs every API call with decoded arguments and the
// result; SetStubMissing makes unimplemented Pascal functions with known
// parameters return 0 instead of stopping (a triage aid, logged once each).
//
// Interrupts: INT 21h (the DOS calls Windows programs use: exit, version,
// console output, files, date/time), INT 31h (DPMI queries), INT 20h
// (terminate).

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <limits>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "win16/Api.h"
#include "win16/ApiCatalog.h"
#include "win16/Files.h"
#include "win16/Cpu.h"
#include "win16/Gdi.h"
#include "win16/Kernel.h"
#include "win16/LocalHeap.h"
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
    // Log every API call (arguments, result, return address) to the output.
    void SetTrace(bool on) { trace_ = on; }
    // Unimplemented Pascal functions with known parameters return 0 instead of
    // stopping the task.
    void SetStubMissing(bool on) { stubMissing_ = on; }
    // Timers fire at the requested interval instead of Windows 3.x's 55 ms minimum.
    void SetExactTimers(bool on) { user_->SetExactTimers(on); }
    // The program's .exe on the host: its directory is what the task's file
    // APIs see (call before Load).
    void SetProgram(const std::filesystem::path& exe) { files_.SetProgram(exe); }

    // The Windows flags reported by GetWinFlags / __WINFLAGS: protected mode,
    // 286, standard mode, no coprocessor (the interpreter is a 286 without x87).
    static constexpr uint16_t kWinFlags = 0x0013;

    bool Load(const std::vector<uint8_t>& file, const std::string& commandLine, std::string& error);
    TaskExit Run(uint64_t budget = std::numeric_limits<uint64_t>::max());

    Memory& Mem() { return memory_; }
    Cpu& Processor() { return cpu_; }
    const NeImage& Image() const { return image_; }
    const LoadedModule& Module() const { return module_; }
    GlobalHeap& Globals() { return globals_; }
    Resources& Resource() { return resources_; }
    LocalHeaps& Locals() { return locals_; }
    FileSystem& Files() { return files_; }
    Profiles& Profile() { return profiles_; }
    uint16_t Environment() const { return envSel_; }
    uint16_t ModuleHandle() const { return moduleDb_; }  // the program's hModule
    User& Windows() { return *user_; }
    Gdi& Graphics() { return *gdi_; }

    // Used by the built-in API implementations.
    void Exit(TaskExit::Kind kind, uint16_t code, const std::string& message);
    bool HasExited() const { return exited_; }
    void Print(const std::string& line) const;
    void DosService();  // INT 21h with the current registers (also KERNEL.DOS3Call)
    uint32_t TickCount() const;  // milliseconds since the task started
    // Prints `line` the first time `key` is seen (limitations worth knowing once).
    void Note(const std::string& key, const std::string& line);

    // Modules by name ("USER", "MMSYSTEM.DLL", "C:\\X\\GAME.DLL"): the built-in or
    // stub module's handle, the program's own hModule, or 0.
    uint16_t FindModuleHandle(const std::string& name);
    // A built-in module (loaded as a stub if the catalog knows it); 0 if unknown.
    uint16_t LoadBuiltinModule(const std::string& name);
    // GetProcAddress: implemented functions of built-in modules, exported
    // entries of the program (by ordinal). 0 if not available.
    uint32_t ProcAddress(uint16_t module, uint16_t ordinal, const std::string& name);

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
        std::string name;
        std::vector<ApiFunction> functions;  // empty: a stub module
        const CatalogModule* catalog = nullptr;
        uint16_t selector = 0;
        std::vector<std::string> unknownNames;  // by-name imports we don't know
    };

private:
    class Resolver;

    struct TraceFrame {
        std::string line;
        bool printed = false;
    };

    size_t AddModule(const std::string& name, std::vector<ApiFunction> functions);
    BuiltinModule* FindModule(const std::string& name);
    void CallApi(size_t module, uint16_t ip);
    // "USER.216" (or "KERNEL.FROBNICATE" for an unknown by-name import) and
    // the function's name if known ("GetDlgItem").
    std::string ApiLabel(const BuiltinModule& m, uint16_t ip) const;
    std::string ApiFunctionName(const BuiltinModule& m, uint16_t ip, const CatalogEntry* c) const;
    std::string TraceArgs(const CatalogEntry* c) const;
    std::string TraceResult(const CatalogEntry* c) const;
    void TracePrint(const std::string& line);
    uint16_t EquateValue(const CatalogEntry& c);
    bool Interrupt(uint8_t vector);
    void Int31();
    bool DosFileService(uint8_t ah);

    Memory memory_;
    Cpu cpu_;
    GlobalHeap globals_;
    LocalHeaps locals_{memory_};
    FileSystem files_;
    Profiles profiles_{files_};
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
    uint16_t envSel_ = 0;
    uint16_t moduleDb_ = 0;  // hModule: a copy of the NE header, like Windows'
    bool trace_ = false;
    bool stubMissing_ = false;
    std::vector<TraceFrame> traceStack_;
    std::set<std::string> notes_;
    std::chrono::steady_clock::time_point start_ = std::chrono::steady_clock::now();
};

}  // namespace retro::win16
