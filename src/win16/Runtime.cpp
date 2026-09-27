#include "win16/Runtime.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iterator>

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

std::string FarAddr(uint16_t sel, uint16_t off) { return Hex(sel, 4) + ":" + Hex(off, 4); }

const char* FaultName(FaultKind k) {
    switch (k) {
    case FaultKind::GeneralProtection: return "general protection fault";
    case FaultKind::InvalidOpcode: return "invalid opcode";
    case FaultKind::DivideError: return "divide error";
    case FaultKind::UnhandledInterrupt: return "unhandled interrupt";
    default: return "host error";
    }
}

// "C:\GAMES\MMSYSTEM.DLL" -> "MMSYSTEM".
std::string ModuleBaseName(const std::string& name) {
    std::string base = name;
    const size_t slash = base.find_last_of("\\/:");
    if (slash != std::string::npos) base = base.substr(slash + 1);
    const size_t dot = base.rfind('.');
    if (dot != std::string::npos) base = base.substr(0, dot);
    return Upper(base);
}

// WIN87EM: the floating-point emulator library. Programs built with the
// emulator call __fpMath at startup (BX = 0) and exit (BX = 2); the real work
// happens through the INT 34h-3Eh emulation interrupts, not supported yet.
void FpMath(Runtime& rt, Cpu& cpu) {
    Registers& r = cpu.Regs();
    switch (r.r[BX]) {
    case 0:  // initialize
    case 1:  // reset
    case 2:  // terminate
        r.r[AX] = 0;
        cpu.ReturnFar(0);
        return;
    default:
        rt.Exit(TaskExit::Kind::Unimplemented, 0,
                "WIN87EM.1 (__fpMath) function " + std::to_string(r.r[BX]) +
                    " is not implemented yet (floating-point emulation)");
        return;
    }
}

std::vector<ApiFunction> Win87emApi() { return {{1, "__FPMATH", FpMath}}; }

// Built-in modules that are only set up when a program imports them.
std::vector<ApiFunction> OptionalModuleApi(const std::string& name) {
    if (name == "WIN87EM") return Win87emApi();
    if (name == "MMSYSTEM") return MmsystemApi();
    return {};
}

// Modules whose calls all do nothing and return 0.
bool IsSilentModule(const std::string& name) { return name == "SOUND"; }

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
        BuiltinModule* m = rt_.FindModule(moduleName);
        if (!m) {
            const std::string upper = Upper(moduleName);
            if (!FindCatalogModule(upper)) {
                // The program's own DLL, from its directory.
                DllModule* dll = rt_.FindDll(upper);
                std::string loadError;
                uint16_t code = 0;
                if (!dll) dll = rt_.LoadDll(upper, loadError, code);
                if (dll) return rt_.ResolveExport(*dll, ordinal, name, selector, offset, error);
                if (code != 2) {  // it's there, but can't be loaded
                    error = loadError;
                    return false;
                }
                rt_.Note("module:" + upper, "module " + upper + " is not built in, and " + upper +
                                                ".DLL isn't in the program's directory: calling into it will "
                                                "stop the task");
            }
            m = &rt_.builtins_[rt_.AddModule(upper, OptionalModuleApi(upper))];
        }
        selector = m->selector;

        const CatalogEntry* c = nullptr;
        uint16_t ord = ordinal;
        if (!name.empty()) {
            const std::string upperName = Upper(name);
            ord = 0;
            for (const ApiFunction& f : m->functions) {
                if (upperName == f.name) ord = f.ordinal;
            }
            if (!ord && m->catalog && (c = FindCatalogEntry(*m->catalog, name)) != nullptr) ord = c->ordinal;
            if (!ord) {
                // Unknown name: give it a private ordinal so a call reports the name.
                m->unknownNames.push_back(upperName);
                offset = uint16_t(0xF000 + m->unknownNames.size() - 1);
                return true;
            }
        }
        if (!c && m->catalog) c = FindCatalogEntry(*m->catalog, ord);
        if (c && c->kind == CatalogKind::Equate) {  // a constant, not a function
            selector = offset = rt_.EquateValue(*c);
            return true;
        }
        offset = ord;
        return true;
    }

private:
    Runtime& rt_;
};

Runtime::Runtime()
    : cpu_(memory_),
      globals_(memory_),
      user_(std::make_unique<User>(*this)),
      gdi_(std::make_unique<Gdi>(*this)) {
    scratchSel_ = memory_.Allocate(kScratchBytes, SegmentKind::Data);
}

Runtime::~Runtime() {
    if (!playingSound_.empty()) StopHostSound();
}

void Runtime::Print(const std::string& line) const {
    if (output_) output_(line);
}

void Runtime::Note(const std::string& key, const std::string& line) {
    if (notes_.insert(key).second) Print(line);
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

// --- Modules ------------------------------------------------------------------------------

size_t Runtime::AddModule(const std::string& name, std::vector<ApiFunction> functions) {
    BuiltinModule m;
    m.name = name;
    m.functions = std::move(functions);
    m.catalog = FindCatalogModule(name);
    m.silent = IsSilentModule(name);
    m.selector = memory_.Allocate(0x10000, SegmentKind::Host);
    builtins_.push_back(std::move(m));
    const size_t index = builtins_.size() - 1;
    cpu_.MapHostSegment(builtins_[index].selector, [this, index](Cpu&, uint16_t ip) { CallApi(index, ip); });
    return index;
}

Runtime::BuiltinModule* Runtime::FindModule(const std::string& name) {
    const std::string wanted = ModuleBaseName(name);
    for (BuiltinModule& m : builtins_) {
        if (m.name == wanted) return &m;
    }
    return nullptr;
}

uint16_t Runtime::FindModuleHandle(const std::string& name) {
    const std::string base = ModuleBaseName(name);
    if (!base.empty() && base == Upper(image_.moduleName)) return moduleDb_;
    if (DllModule* dll = FindDll(base)) return dll->hModule;
    const BuiltinModule* m = FindModule(base);
    return m ? m->selector : 0;
}

uint16_t Runtime::LoadBuiltinModule(const std::string& name) {
    if (const uint16_t h = FindModuleHandle(name)) return h;
    const std::string base = ModuleBaseName(name);
    if (!FindCatalogModule(base)) return 0;
    return builtins_[AddModule(base, OptionalModuleApi(base))].selector;
}

uint32_t Runtime::ProcAddress(uint16_t module, uint16_t ordinal, const std::string& name) {
    if (DllModule* dll = FindDll(module)) {
        uint16_t sel = 0, off = 0;
        std::string error;
        return ResolveExport(*dll, ordinal, name, sel, off, error) ? (uint32_t(sel) << 16) | off : 0;
    }
    if (module && (module == moduleDb_ || module == module_.dgroup)) {
        // The program's own exports, by ordinal (its name tables aren't kept).
        const NeEntry* e = name.empty() ? image_.FindEntry(ordinal) : nullptr;
        if (!e || e->segment < 1 || e->segment > module_.selectors.size()) return 0;
        return (uint32_t(module_.selectors[e->segment - 1]) << 16) | e->offset;
    }
    for (const BuiltinModule& m : builtins_) {
        if (m.selector != module) continue;
        // Only what's implemented: a program probing for a function should see it missing.
        const std::string upperName = Upper(name);
        for (const ApiFunction& f : m.functions) {
            if (name.empty() ? f.ordinal == ordinal : upperName == f.name)
                return (uint32_t(m.selector) << 16) | f.ordinal;
        }
        return 0;
    }
    return 0;
}

uint16_t Runtime::EquateValue(const CatalogEntry& c) {
    const std::string name = c.name;
    if (name == "__WINFLAGS") return kWinFlags;
    if (name == "__AHINCR") return 8;   // selector increment between the parts of a huge block
    if (name == "__AHSHIFT") return 3;
    if (name == "__ROMBIOS" || (name.size() == 7 && name.compare(0, 2, "__") == 0 && name[6] == 'H')) {
        // __0040H, __A000H, ...: selectors for real-mode memory (BIOS data, video).
        Note("equate:" + name, "the program imports " + name +
                                   " (direct access to real-mode memory): not supported, it gets a null selector");
        return 0;
    }
    return c.value;
}

bool Runtime::Load(const std::vector<uint8_t>& file, const std::string& commandLine,
                   std::string& error) {
    if (!ParseNe(file, image_, error)) return false;
    if (image_.IsLibrary()) {
        error = "module " + image_.moduleName + " is a DLL, not a program";
        return false;
    }

    if (builtins_.empty()) {
        AddModule("KERNEL", KernelApi());
        AddModule("USER", UserApi());
        AddModule("GDI", GdiApi());
    }
    cpu_.SetInterruptHandler([this](Cpu&, uint8_t vector) { return Interrupt(vector); });

    Resolver resolver(*this);
    if (!LoadNe(image_, file, memory_, resolver, commandLine, module_, error)) return false;
    cpu_.Regs() = module_.initial;
    if (!pendingInit_.empty()) {
        // The DLLs it imports are initialized first, on the interpreter: start
        // at a host routine that runs their entry points, then the program's.
        bootstrapSel_ = memory_.Allocate(0x10000, SegmentKind::Host);
        cpu_.MapHostSegment(bootstrapSel_, [this](Cpu&, uint16_t) { Bootstrap(); });
        cpu_.Regs().s[CS] = bootstrapSel_;
        cpu_.Regs().ip = 0;
    }

    // hModule: a copy of the NE header, which programs occasionally read.
    const uint32_t ne = uint32_t(file[0x3C]) | (uint32_t(file[0x3D]) << 8) |
                        (uint32_t(file[0x3E]) << 16) | (uint32_t(file[0x3F]) << 24);
    moduleDb_ = memory_.Allocate(0x40, SegmentKind::Data);
    if (moduleDb_) std::memcpy(memory_.SegmentData(moduleDb_), &file[ne], 0x40);

    // The DOS environment (PSP:2Ch): variables, then the program's path.
    std::string env = std::string("PATH=C:\\WINDOWS;C:\\WINDOWS\\SYSTEM") + '\0' + "TEMP=C:\\WINDOWS\\TEMP" +
                      '\0' + "COMSPEC=C:\\COMMAND.COM" + '\0' + '\0' + '\x01' + '\0' +
                      files_.ProgramPath() + '\0';
    envSel_ = memory_.Allocate(uint32_t(env.size()), SegmentKind::Data);
    if (envSel_) {
        std::memcpy(memory_.SegmentData(envSel_), env.data(), env.size());
        memory_.Write16(module_.psp, 0x2C, envSel_);
    }

    // The local heap: from the end of the stack to the end of DGROUP.
    if (module_.dgroup && module_.heapStart && module_.heapStart < memory_.SegmentSize(module_.dgroup))
        locals_.Init(module_.dgroup, module_.heapStart, uint16_t(memory_.SegmentSize(module_.dgroup) - 1));
    return true;
}

TaskExit Runtime::Run(uint64_t budget) {
    const RunResult result = cpu_.Run(budget);
    TaskExit out;
    if (result == RunResult::Faulted) {
        const CpuFault& f = cpu_.Fault();
        out.kind = TaskExit::Kind::Fault;
        out.message = std::string(FaultName(f.kind)) + " at " + FarAddr(f.cs, f.ip) + ": " + f.detail;
    } else if (result == RunResult::BudgetExhausted && !exited_) {
        out.kind = TaskExit::Kind::BudgetExhausted;
    } else {
        out = exit_;
    }
    out.registers = cpu_.Regs();
    out.instructions = cpu_.Instructions();
    return out;
}

// --- The program's DLLs --------------------------------------------------------------------

Runtime::DllModule* Runtime::FindDll(uint16_t handle) {
    if (!handle) return nullptr;
    for (auto& d : dlls_) {
        if (d->hInstance == handle || d->hModule == handle) return d.get();
    }
    return nullptr;
}

Runtime::DllModule* Runtime::FindDll(const std::string& name) {
    const std::string base = ModuleBaseName(name);
    for (auto& d : dlls_) {
        if (d->name == base || ModuleBaseName(d->fileName) == base) return d.get();
    }
    return nullptr;
}

Runtime::DllModule* Runtime::LoadDll(const std::string& name, std::string& error, uint16_t& code) {
    const std::string base = ModuleBaseName(name);
    if (DllModule* loaded = FindDll(base)) return loaded;
    // "GAME" -> GAME.DLL; paths and other extensions as given (in the program's directory).
    const size_t slash = name.find_last_of("\\/:");
    const bool hasExtension = name.find('.', slash == std::string::npos ? 0 : slash + 1) != std::string::npos;
    const std::string fileName = hasExtension ? name : name + ".DLL";
    std::filesystem::path host;
    std::string why;
    std::error_code ec;
    if (!files_.Resolve(fileName, host, why) || !std::filesystem::is_regular_file(host, ec) ||
        std::filesystem::file_size(host, ec) > (16u << 20)) {
        code = 2;
        error = fileName + " is not in the program's directory";
        return nullptr;
    }
    if (loadingDlls_.count(base)) {
        code = 11;
        error = "DLLs that import each other in a circle (" + base + ") aren't supported";
        return nullptr;
    }
    std::ifstream in(host, std::ios::binary);
    const std::vector<uint8_t> file{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};

    auto dll = std::make_unique<DllModule>();
    std::string parseError;
    if (!ParseNe(file, dll->image, parseError) || !dll->image.IsLibrary()) {
        code = 11;
        error = fileName + (parseError.empty() ? " is a program, not a DLL" : ": " + parseError);
        return nullptr;
    }
    dll->name = dll->image.moduleName.empty() ? base : Upper(dll->image.moduleName);
    try {
        dll->fileName = Upper(host.string());
    } catch (const std::exception&) {
        dll->fileName = "C:\\WINDOWS\\" + base + ".DLL";
    }

    // Its segments, with its own imports resolved (loading the DLLs it needs first).
    loadingDlls_.insert(base);
    Resolver resolver(*this);
    std::string loadError;
    const bool ok = LoadNe(dll->image, file, memory_, resolver, "", dll->loaded, loadError);
    loadingDlls_.erase(base);
    if (!ok) {
        code = 11;
        error = fileName + ": " + loadError;
        return nullptr;
    }
    const uint32_t ne = uint32_t(file[0x3C]) | (uint32_t(file[0x3D]) << 8) | (uint32_t(file[0x3E]) << 16) |
                        (uint32_t(file[0x3F]) << 24);
    dll->hModule = memory_.Allocate(0x40, SegmentKind::Data);
    if (dll->hModule) std::memcpy(memory_.SegmentData(dll->hModule), &file[ne], 0x40);
    dll->hInstance = dll->loaded.dgroup ? dll->loaded.dgroup : dll->hModule;
    dll->resources = std::make_unique<Resources>(dll->image, globals_, memory_);
    const uint16_t dg = dll->loaded.dgroup;
    if (dg && dll->image.heapSize && dll->loaded.heapStart < memory_.SegmentSize(dg))
        locals_.Init(dg, dll->loaded.heapStart, uint16_t(memory_.SegmentSize(dg) - 1));

    DllModule* raw = dll.get();
    dlls_.push_back(std::move(dll));
    if (raw->image.entrySegment) {
        pendingInit_.push_back(raw);  // its entry point runs before it's used
    } else {
        raw->initialized = true;  // resource-only DLLs have none
    }
    if (trace_) Print("[trace] loaded " + raw->fileName + " (module " + raw->name + ")");
    return raw;
}

bool Runtime::ResolveExport(DllModule& dll, uint16_t ordinal, const std::string& name, uint16_t& selector,
                            uint16_t& offset, std::string& error) {
    uint16_t ord = ordinal;
    if (!name.empty()) {
        const auto it = dll.image.exportNames.find(Upper(name));
        if (it == dll.image.exportNames.end()) {
            error = dll.name + " has no export named " + name;
            return false;
        }
        ord = it->second;
    }
    const NeEntry* e = dll.image.FindEntry(ord);
    if (!e) {
        error = dll.name + " has no export " + std::to_string(ord);
        return false;
    }
    if (e->segment == 0xFE) {  // a constant
        selector = offset = e->offset;
        return true;
    }
    if (e->segment < 1 || e->segment > dll.loaded.selectors.size()) {
        error = dll.name + " export " + std::to_string(ord) + " is in a segment that doesn't exist";
        return false;
    }
    selector = dll.loaded.selectors[e->segment - 1];
    offset = e->offset;
    return true;
}

bool Runtime::InitializePendingDlls(std::string& error) {
    while (!pendingInit_.empty()) {
        DllModule* d = pendingInit_.front();
        pendingInit_.erase(pendingInit_.begin());
        d->initialized = true;
        if (d->image.entrySegment < 1 || d->image.entrySegment > d->loaded.selectors.size()) continue;
        // LibEntry: DI = hInstance, DS = DGROUP, CX = local heap size, ES:SI = command line (none).
        const uint16_t hInstance = d->hInstance, heap = d->image.heapSize;
        const uint32_t result = cpu_.CallFar(d->loaded.selectors[d->image.entrySegment - 1], d->image.entryIp, {},
                                             d->loaded.dgroup, [&](Cpu& c) {
                                                 c.Regs().r[DI] = hInstance;
                                                 c.Regs().r[CX] = heap;
                                                 c.Regs().r[SI] = 0;
                                                 c.LoadSegment(ES, 0);
                                             });
        if (exited_) return false;
        if ((result & 0xFFFF) == 0) {
            d->failed = true;
            error = d->name + " failed to initialize (its entry point returned 0)";
            return false;
        }
    }
    return true;
}

void Runtime::Bootstrap() {
    std::string error;
    if (!InitializePendingDlls(error)) {
        if (!exited_) Exit(TaskExit::Kind::FatalExit, 0, error);
        return;
    }
    cpu_.Regs() = module_.initial;  // now the program's own entry point
}

uint16_t Runtime::LoadLibraryModule(const std::string& name) {
    const std::string base = ModuleBaseName(name);
    if (FindModule(base) || FindCatalogModule(base)) return LoadBuiltinModule(base);
    std::string error;
    uint16_t code = 0;
    DllModule* dll = LoadDll(name, error, code);
    if (!dll) {
        Note("loadlibrary:" + Upper(name), "LoadLibrary(\"" + name + "\"): " + error);
        return code;
    }
    if (!pendingInit_.empty() && !InitializePendingDlls(error)) {
        if (!exited_) Note("loadlibrary:" + Upper(name), "LoadLibrary(\"" + name + "\"): " + error);
        return 20;
    }
    if (dll->failed) return 20;
    ++dll->usage;
    return dll->hInstance;
}

bool Runtime::FreeLibraryModule(uint16_t handle) {
    DllModule* dll = FindDll(handle);
    if (!dll) return false;
    if (dll->usage > 0) --dll->usage;  // DLLs stay loaded until the program ends
    return true;
}

Resources& Runtime::ResourcesFor(uint16_t handle) {
    if (DllModule* dll = FindDll(handle)) return *dll->resources;
    return resources_;
}

uint16_t Runtime::FreeResourceAny(uint16_t hglobal) {
    if (resources_.Free(hglobal) == 0) return 0;
    for (auto& d : dlls_) {
        if (d->resources->Free(hglobal) == 0) return 0;
    }
    return hglobal;
}

std::string Runtime::ModuleFileName(uint16_t handle) {
    if (handle == 0 || handle == module_.dgroup || handle == moduleDb_) return files_.ProgramPath();
    if (DllModule* dll = FindDll(handle)) return dll->fileName;
    for (const BuiltinModule& m : builtins_) {
        if (m.selector == handle) {
            const bool exe = m.name == "KERNEL" || m.name == "USER" || m.name == "GDI";
            return "C:\\WINDOWS\\SYSTEM\\" + m.name + (exe ? ".EXE" : ".DLL");
        }
    }
    return "";
}

// --- API dispatch and tracing ------------------------------------------------------------

std::string Runtime::ApiLabel(const BuiltinModule& m, uint16_t ip) const {
    if (ip >= 0xF000 && ip - 0xF000u < m.unknownNames.size()) return m.name + "." + m.unknownNames[ip - 0xF000];
    return m.name + "." + std::to_string(ip);
}

std::string Runtime::ApiFunctionName(const BuiltinModule& m, uint16_t ip, const CatalogEntry* c) const {
    if (c) return c->name;
    for (const ApiFunction& f : m.functions) {
        if (f.ordinal == ip) return f.name;
    }
    return "";
}

std::string Runtime::TraceArgs(const CatalogEntry* c) const {
    if (!c) return "?";
    const Registers& r = cpu_.Regs();
    if (c->kind == CatalogKind::Register) {
        return "AX=" + Hex(r.r[AX], 4) + " BX=" + Hex(r.r[BX], 4) + " CX=" + Hex(r.r[CX], 4) +
               " DX=" + Hex(r.r[DX], 4) + " ES=" + Hex(r.s[ES], 4);
    }
    if (!c->params) return "...";
    // Pascal: the last parameter is nearest the return address.
    const size_t n = std::strlen(c->params);
    std::vector<uint16_t> offsets(n);
    uint16_t off = 0;
    for (size_t i = n; i-- > 0;) {
        offsets[i] = off;
        off = uint16_t(off + (c->params[i] == 'w' || c->params[i] == 's' ? 2 : 4));
    }
    std::string s;
    for (size_t i = 0; i < n; ++i) {
        if (i) s += ", ";
        const uint16_t lo = cpu_.StackArg(offsets[i]);
        switch (c->params[i]) {
        case 'w': s += Hex(lo, 4); break;
        case 's': s += std::to_string(int16_t(lo)); break;
        case 'l': s += Hex(uint32_t(cpu_.StackArg(uint16_t(offsets[i] + 2))) << 16 | lo, 8); break;
        case 'z':
        case 'Z': {
            const uint16_t sel = cpu_.StackArg(uint16_t(offsets[i] + 2));
            if (sel == 0 && lo == 0) {
                s += "NULL";
            } else if (sel == 0) {
                s += "#" + std::to_string(lo);  // MAKEINTRESOURCE / atom
            } else if (!memory_.Lookup(sel) || memory_.Lookup(sel)->kind == SegmentKind::Host) {
                s += FarAddr(sel, lo);
            } else {
                std::string text = memory_.ReadString(sel, lo, 48);
                std::string quoted = "\"";
                for (char ch : text) {
                    if (ch == '"' || ch == '\\') quoted += '\\';
                    if (static_cast<unsigned char>(ch) < 0x20) {
                        quoted += "\\x" + Hex(static_cast<unsigned char>(ch), 2);
                    } else {
                        quoted += ch;
                    }
                }
                s += quoted + (text.size() == 48 ? "...\"" : "\"");
            }
            break;
        }
        default: s += FarAddr(cpu_.StackArg(uint16_t(offsets[i] + 2)), lo); break;  // far pointers
        }
    }
    return s;
}

std::string Runtime::TraceResult(const CatalogEntry* c) const {
    if (exited_) return "(task ended)";
    const Registers& r = cpu_.Regs();
    if (c && c->kind == CatalogKind::Register) return "AX=" + Hex(r.r[AX], 4) + " DX=" + Hex(r.r[DX], 4);
    if (c && c->ret16) return Hex(r.r[AX], 4);
    return FarAddr(r.r[DX], r.r[AX]);
}

void Runtime::TracePrint(const std::string& line) {
    // Calls still in progress (a WndProc sending messages...) are shown first.
    for (TraceFrame& f : traceStack_) {
        if (!f.printed) {
            Print("[trace] " + f.line);
            f.printed = true;
        }
    }
    Print("[trace] " + line);
}

void Runtime::CallApi(size_t moduleIndex, uint16_t ip) {
    BuiltinModule& module = builtins_[moduleIndex];
    const ApiFunction* impl = nullptr;
    for (const ApiFunction& f : module.functions) {
        if (f.ordinal == ip) impl = &f;
    }
    const CatalogEntry* c = module.catalog && ip < 0xF000 ? FindCatalogEntry(*module.catalog, ip) : nullptr;
    const std::string label = ApiLabel(module, ip), function = ApiFunctionName(module, ip, c);
    const std::string name = function.empty() ? label : label + " (" + function + ")";  // for messages
    const uint16_t retIp = cpu_.StackArg(uint16_t(-4)), retCs = cpu_.StackArg(uint16_t(-2));
    const std::string from = FarAddr(retCs, retIp);

    // Trace frame, popped (and printed if nothing else printed it) however
    // the call ends, including a fault unwinding out of a nested callback.
    struct Frame {
        Runtime& rt;
        bool active;
        ~Frame() {
            if (!active || rt.traceStack_.empty()) return;
            const TraceFrame f = rt.traceStack_.back();
            rt.traceStack_.pop_back();
            if (!f.printed) rt.Print("[trace] " + f.line + " ...");
        }
        void Finish(const std::string& result) {
            active = false;
            const TraceFrame f = rt.traceStack_.back();
            rt.traceStack_.pop_back();
            if (!f.printed) {
                rt.TracePrint(f.line + " = " + result);
            } else {
                rt.TracePrint(std::string(size_t(rt.cpu_.CallbackDepth()) * 2, ' ') + "  = " + result);
            }
        }
    } frame{*this, trace_};
    if (trace_) {
        traceStack_.push_back({std::string(size_t(cpu_.CallbackDepth()) * 2, ' ') + from + " " + label +
                                   (function.empty() ? "" : " " + function) + "(" + TraceArgs(c) + ")",
                               false});
    }

    if (impl) {
        const uint16_t sp = cpu_.Regs().r[SP];
        impl->impl(*this, cpu_);
        // Consistency check: a Pascal function removes exactly its arguments
        // (plus the return address). A mismatch is an engine bug.
        if (c && c->kind == CatalogKind::Pascal && c->params && !exited_ && cpu_.Regs().s[CS] == retCs &&
            cpu_.Regs().ip == retIp) {
            const uint16_t removed = uint16_t(cpu_.Regs().r[SP] - sp - 4);
            if (removed != ParamBytes(c->params))
                Note("stackcheck:" + name, "internal error: " + name + " removed " + std::to_string(removed) +
                                              " bytes of arguments, the catalog says " +
                                              std::to_string(ParamBytes(c->params)));
        }
        if (trace_) frame.Finish(TraceResult(c));
        return;
    }
    if (module.silent && c && c->kind == CatalogKind::Pascal && c->params) {
        Note("silent:" + module.name, module.name + " calls (PC-speaker music) are ignored");
        SetResult(cpu_, 0);
        cpu_.ReturnFar(ParamBytes(c->params));
        if (trace_) frame.Finish(TraceResult(c));
        return;
    }
    if (stubMissing_ && c &&
        ((c->kind == CatalogKind::Pascal && c->params) || c->kind == CatalogKind::Varargs)) {
        Note("stub:" + name, name + " is not implemented yet: returning 0 (--stub-missing)");
        SetResult(cpu_, 0);
        cpu_.ReturnFar(c->kind == CatalogKind::Pascal ? ParamBytes(c->params) : 0);
        if (trace_) frame.Finish(TraceResult(c) + " (stub)");
        return;
    }
    std::string message = name + " is not implemented yet (returning to " + from + ")";
    if (module.functions.empty() && !module.catalog)
        message += "; " + module.name + " is not built in, and " + module.name + ".DLL wasn't in the program's directory";
    if (trace_) frame.Finish("not implemented");
    Exit(TaskExit::Kind::Unimplemented, 0, message);
}

// --- Interrupts and DOS -------------------------------------------------------------------

bool Runtime::Interrupt(uint8_t vector) {
    switch (vector) {
    case 0x20: Exit(TaskExit::Kind::Exited, 0, "INT 20h"); return true;
    case 0x21: DosService(); return true;
    case 0x31: Int31(); return true;
    default:
        if (vector >= 0x34 && vector <= 0x3E) {  // x87 instructions, emulated through WIN87EM
            Exit(TaskExit::Kind::Unimplemented, 0,
                 "floating-point instruction (INT " + Hex(vector, 2) +
                     "h, x87 emulation) at " + FarAddr(cpu_.Regs().s[CS], cpu_.Regs().ip) +
                     " is not implemented yet");
            return true;
        }
        return false;
    }
}

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
        if (DosFileService(ah)) return;
        Print("INT 21h AH=" + Hex(ah, 2) + "h not supported yet (returned an error)");
        cpu_.SetFlag(flags::CF, true);
        r.r[AX] = 1;  // invalid function
        return;
    }
}

bool Runtime::DosFileService(uint8_t ah) {
    Registers& r = cpu_.Regs();
    auto fail = [&](uint16_t code) {
        cpu_.SetFlag(flags::CF, true);
        r.r[AX] = code;
    };
    switch (ah) {
    case 0x0E: r.R8(0) = 26; return true;  // select drive: 26 drive letters
    case 0x1A: return true;                // set DTA: find first/next always come up empty
    case 0x2F:                              // get DTA: the PSP's default one
        cpu_.LoadSegment(ES, module_.psp);
        r.r[BX] = 0x80;
        return true;
    case 0x2A: {  // date
        const std::time_t now = std::time(nullptr);
        std::tm t{};
        localtime_s(&t, &now);
        r.r[CX] = uint16_t(t.tm_year + 1900);
        r.R8(6) = uint8_t(t.tm_mon + 1);  // DH
        r.R8(2) = uint8_t(t.tm_mday);     // DL
        r.R8(0) = uint8_t(t.tm_wday);     // AL
        return true;
    }
    case 0x2C: {  // time
        const std::time_t now = std::time(nullptr);
        std::tm t{};
        localtime_s(&t, &now);
        r.R8(5) = uint8_t(t.tm_hour);  // CH
        r.R8(1) = uint8_t(t.tm_min);   // CL
        r.R8(6) = uint8_t(t.tm_sec);   // DH
        r.R8(2) = 0;                   // DL: hundredths
        return true;
    }
    case 0x36:  // free disk space: plenty
        r.r[AX] = 8;
        r.r[BX] = 0x7FFF;
        r.r[CX] = 512;
        r.r[DX] = 0xFFFF;
        return true;
    case 0x3B: return true;  // change directory: everything is the program's directory
    case 0x47:               // current directory (DS:SI): the root
        memory_.Write8(r.s[DS], r.r[SI], 0);
        return true;
    case 0x3C:  // create
    case 0x41:  // delete
    case 0x56:  // rename
        Note("dos-write", "the program tried to create, delete or rename a file: "
                          "writing files isn't supported yet (access denied)");
        fail(dos::AccessDenied);
        return true;
    case 0x3D: {  // open DS:DX, AL = mode
        const std::string path = memory_.ReadString(r.s[DS], r.r[DX], 128);
        uint16_t err = 0;
        const int h = files_.Open(path, r.R8(0), err);
        if (h < 0) {
            if (err == dos::AccessDenied && (r.R8(0) & 3))
                Note("write:" + Upper(path), "opening \"" + path + "\" for writing: not supported yet (access denied)");
            fail(err);
        } else {
            r.r[AX] = uint16_t(h);
        }
        return true;
    }
    case 0x3E:  // close BX
        if (r.r[BX] >= 5 && !files_.Close(r.r[BX])) fail(dos::InvalidHandle);
        return true;
    case 0x3F: {  // read BX, CX bytes to DS:DX
        const uint16_t h = r.r[BX], n = r.r[CX];
        if (h == 0) {  // stdin: end of file
            r.r[AX] = 0;
            return true;
        }
        if (!files_.IsOpen(h)) {
            fail(dos::InvalidHandle);
            return true;
        }
        if (n) {
            memory_.Translate(r.s[DS], r.r[DX], n, Access::Write);  // #GP if the buffer is bad
            r.r[AX] = uint16_t(files_.Read(h, memory_.SegmentData(r.s[DS]) + r.r[DX], n));
        } else {
            r.r[AX] = 0;
        }
        return true;
    }
    case 0x40: {  // write BX, CX bytes from DS:DX
        const uint16_t h = r.r[BX], n = r.r[CX];
        if (h == 1 || h == 2) {  // stdout, stderr: the console
            std::string s;
            for (uint16_t i = 0; i < n; ++i) s.push_back(char(memory_.Read8(r.s[DS], uint16_t(r.r[DX] + i))));
            while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
            if (!s.empty()) Print(s);
            r.r[AX] = n;
            return true;
        }
        if (!files_.IsOpen(h)) {
            fail(dos::InvalidHandle);
            return true;
        }
        Note("dos-write", "the program tried to write a file: not supported yet (access denied)");
        fail(dos::AccessDenied);
        return true;
    }
    case 0x42: {  // seek BX to CX:DX from AL
        const int32_t offset = int32_t((uint32_t(r.r[CX]) << 16) | r.r[DX]);
        const int32_t pos = files_.Seek(r.r[BX], offset, r.R8(0));
        if (pos < 0) {
            fail(files_.IsOpen(r.r[BX]) ? 0x19 /* seek error */ : dos::InvalidHandle);
        } else {
            r.r[AX] = uint16_t(pos);
            r.r[DX] = uint16_t(uint32_t(pos) >> 16);
        }
        return true;
    }
    case 0x43:  // get attributes (AL = 0) of DS:DX
        if (r.R8(0) != 0) {
            fail(dos::AccessDenied);
        } else if (files_.Exists(memory_.ReadString(r.s[DS], r.r[DX], 128))) {
            r.r[CX] = 0x20;  // archive
        } else {
            fail(dos::FileNotFound);
        }
        return true;
    case 0x44:  // IOCTL: only "get device information"
        if (r.R8(0) != 0) {
            fail(1);
        } else if (r.r[BX] <= 2) {
            r.r[DX] = 0x80D3;  // character device: console
        } else if (r.r[BX] <= 4) {
            r.r[DX] = 0x80C0;
        } else if (files_.IsOpen(r.r[BX])) {
            r.r[DX] = 0x0002;  // a file on drive C:
        } else {
            fail(dos::InvalidHandle);
        }
        return true;
    case 0x4E:  // find first / next: nothing
    case 0x4F:
        fail(0x12);  // no more files
        return true;
    default:
        return false;
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
