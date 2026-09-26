// KERNEL: task startup, termination, version, DOS calls, the global heap and
// resources.

#include "win16/Kernel.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <string>

#include "win16/Api.h"
#include "win16/Runtime.h"

namespace retro::win16 {

// --- GlobalHeap --------------------------------------------------------------------------

uint16_t GlobalHeap::Alloc(uint16_t flags, uint32_t bytes) {
    if (bytes > 0x10000) return 0;       // huge (tiled) blocks: not yet
    const uint32_t size = bytes ? bytes : 16;  // zero-size blocks still get a selector
    const uint16_t sel = mem_.Allocate(size, SegmentKind::Data);  // always zero-filled
    if (!sel) return 0;
    blocks_[sel] = Block{size, flags, 0};
    return (flags & gmem::Moveable) ? uint16_t(sel & ~1u) : sel;
}

uint16_t GlobalHeap::Free(uint16_t handle) {
    const auto it = blocks_.find(SelectorOf(handle));
    if (it == blocks_.end()) return handle;
    mem_.Free(it->first);
    blocks_.erase(it);
    return 0;
}

uint32_t GlobalHeap::Lock(uint16_t handle) {
    const auto it = blocks_.find(SelectorOf(handle));
    if (it == blocks_.end()) return 0;
    ++it->second.locks;
    return uint32_t(it->first) << 16;
}

bool GlobalHeap::Unlock(uint16_t handle) {
    const auto it = blocks_.find(SelectorOf(handle));
    if (it == blocks_.end() || it->second.locks == 0) return false;
    return --it->second.locks > 0;
}

uint16_t GlobalHeap::ReAlloc(uint16_t handle, uint32_t bytes, uint16_t flags) {
    const auto it = blocks_.find(SelectorOf(handle));
    if (it == blocks_.end()) return 0;
    if (flags & gmem::Modify) {
        it->second.flags = uint16_t((it->second.flags & ~gmem::Moveable) | (flags & gmem::Moveable));
        return HandleOf(it->first);
    }
    if (bytes > 0x10000) return 0;
    const uint32_t size = bytes ? bytes : 16;
    if (!mem_.Resize(it->first, size)) return 0;
    it->second.size = size;
    return handle;
}

uint16_t GlobalHeap::HandleOf(uint16_t selector) const {
    const auto it = blocks_.find(SelectorOf(selector));
    if (it == blocks_.end()) return 0;
    return (it->second.flags & gmem::Moveable) ? uint16_t(it->first & ~1u) : it->first;
}

uint32_t GlobalHeap::Size(uint16_t handle) const {
    const auto it = blocks_.find(SelectorOf(handle));
    return it == blocks_.end() ? 0 : it->second.size;
}

uint16_t GlobalHeap::LockCount(uint16_t handle) const {
    const auto it = blocks_.find(SelectorOf(handle));
    return it == blocks_.end() ? 0 : it->second.locks;
}

namespace {

// --- Task --------------------------------------------------------------------------------

void FatalExit(Runtime& rt, Cpu& cpu) {
    const uint16_t code = cpu.StackArg(0);
    rt.Exit(TaskExit::Kind::FatalExit, code, "FatalExit(" + std::to_string(code) + ")");
}

void FatalAppExit(Runtime& rt, Cpu& cpu) {
    const FarPtr msg = ArgPtr(cpu, 0);
    rt.Exit(TaskExit::Kind::FatalExit, 0, "FatalAppExit: " + rt.Mem().ReadString(msg.sel, msg.off));
}

void GetVersion(Runtime&, Cpu& cpu) {
    SetResult(cpu, 0x0500'0A03);  // Windows 3.10 in AX (major in AL), MS-DOS 5.00 in DX
    cpu.ReturnFar(0);
}

void WaitEvent(Runtime&, Cpu& cpu) {
    cpu.Regs().r[AX] = 0;
    cpu.ReturnFar(2);
}

// Register-based: the first call of every Win16 program's startup code.
void InitTask(Runtime& rt, Cpu& cpu) {
    Registers& r = cpu.Regs();
    r.r[AX] = 1;                    // success
    r.r[BX] = 0x81;                 // command line (in the PSP)
    r.r[CX] = rt.Image().stackSize;
    r.r[DX] = 1;                    // nCmdShow = SW_SHOWNORMAL
    r.r[SI] = 0;                    // hPrevInstance
    r.r[DI] = rt.Module().dgroup;   // hInstance
    cpu.LoadSegment(ES, rt.Module().psp);
    cpu.ReturnFar(0);
}

// INT 21h as a far call, same registers in and out.
void Dos3Call(Runtime& rt, Cpu& cpu) {
    rt.DosService();
    if (!rt.HasExited()) cpu.ReturnFar(0);
}

// --- Global heap ----------------------------------------------------------------------------

void GlobalAlloc(Runtime& rt, Cpu& cpu) {
    const uint32_t bytes = ArgLong(cpu, 0);
    const uint16_t flags = cpu.StackArg(4);
    const uint16_t handle = rt.Globals().Alloc(flags, bytes);
    if (!handle && bytes > 0x10000)
        rt.Print("GlobalAlloc(" + std::to_string(bytes) + "): blocks over 64 KB are not supported yet");
    cpu.Regs().r[AX] = handle;
    cpu.ReturnFar(6);
}

void GlobalFree(Runtime& rt, Cpu& cpu) {
    cpu.Regs().r[AX] = rt.Globals().Free(cpu.StackArg(0));
    cpu.ReturnFar(2);
}

void GlobalLock(Runtime& rt, Cpu& cpu) {
    SetResult(cpu, rt.Globals().Lock(cpu.StackArg(0)));
    cpu.ReturnFar(2);
}

void GlobalUnlock(Runtime& rt, Cpu& cpu) {
    cpu.Regs().r[AX] = rt.Globals().Unlock(cpu.StackArg(0)) ? 1 : 0;
    cpu.ReturnFar(2);
}

void GlobalSize(Runtime& rt, Cpu& cpu) {
    SetResult(cpu, rt.Globals().Size(cpu.StackArg(0)));
    cpu.ReturnFar(2);
}

void GlobalReAlloc(Runtime& rt, Cpu& cpu) {  // (HGLOBAL, DWORD bytes, UINT flags) -> HGLOBAL
    const PascalArgs a(cpu, {2, 4, 2});
    cpu.Regs().r[AX] = rt.Globals().ReAlloc(a.Word(0), a.Long(1), a.Word(2));
    cpu.ReturnFar(a.Bytes());
}

void GlobalHandle(Runtime& rt, Cpu& cpu) {  // (selector) -> handle | selector << 16
    const PascalArgs a(cpu, {2});
    const uint16_t h = rt.Globals().HandleOf(a.Word(0));
    SetResult(cpu, h ? (uint32_t(a.Word(0) | 1u) << 16) | h : 0);
    cpu.ReturnFar(a.Bytes());
}

void GlobalFlags(Runtime& rt, Cpu& cpu) {  // (HGLOBAL) -> lock count
    const PascalArgs a(cpu, {2});
    cpu.Regs().r[AX] = rt.Globals().LockCount(a.Word(0)) & 0xFF;
    cpu.ReturnFar(a.Bytes());
}

void GlobalCompact(Runtime& rt, Cpu& cpu) {  // (DWORD wanted) -> largest free block
    const PascalArgs a(cpu, {4});
    SetResult(cpu, rt.Mem().FreeBytes());
    cpu.ReturnFar(a.Bytes());
}

void GetFreeSpace(Runtime& rt, Cpu& cpu) {  // (UINT) -> DWORD bytes
    const PascalArgs a(cpu, {2});
    SetResult(cpu, rt.Mem().FreeBytes());
    cpu.ReturnFar(a.Bytes());
}

void LockSegment(Runtime&, Cpu& cpu) {  // (selector or -1 = DS) -> selector
    const PascalArgs a(cpu, {2});
    cpu.Regs().r[AX] = a.Word(0) == 0xFFFF ? cpu.Regs().s[DS] : a.Word(0);
    cpu.ReturnFar(a.Bytes());
}

// --- Local heap (the one in the caller's DS) --------------------------------------------------

void LocalInit(Runtime& rt, Cpu& cpu) {  // (segment or 0 = DS, start, end) -> BOOL
    const PascalArgs a(cpu, {2, 2, 2});
    const uint16_t sel = a.Word(0) ? a.Word(0) : cpu.Regs().s[DS];
    cpu.Regs().r[AX] = rt.Locals().Init(sel, a.Word(1), a.Word(2)) ? 1 : 0;
    cpu.ReturnFar(a.Bytes());
}

void LocalAlloc(Runtime& rt, Cpu& cpu) {  // (flags, bytes) -> HLOCAL
    const PascalArgs a(cpu, {2, 2});
    cpu.Regs().r[AX] = rt.Locals().Alloc(cpu.Regs().s[DS], a.Word(0), a.Word(1));
    cpu.ReturnFar(a.Bytes());
}

void LocalReAlloc(Runtime& rt, Cpu& cpu) {  // (HLOCAL, bytes, flags) -> HLOCAL
    const PascalArgs a(cpu, {2, 2, 2});
    cpu.Regs().r[AX] = rt.Locals().ReAlloc(cpu.Regs().s[DS], a.Word(0), a.Word(1), a.Word(2));
    cpu.ReturnFar(a.Bytes());
}

void LocalFree(Runtime& rt, Cpu& cpu) {  // (HLOCAL) -> 0 on success
    const PascalArgs a(cpu, {2});
    cpu.Regs().r[AX] = rt.Locals().Free(cpu.Regs().s[DS], a.Word(0));
    cpu.ReturnFar(a.Bytes());
}

void LocalLock(Runtime& rt, Cpu& cpu) {  // (HLOCAL) -> near pointer (DX = DS)
    const PascalArgs a(cpu, {2});
    const uint16_t p = rt.Locals().Lock(cpu.Regs().s[DS], a.Word(0));
    cpu.Regs().r[AX] = p;
    cpu.Regs().r[DX] = p ? cpu.Regs().s[DS] : 0;
    cpu.ReturnFar(a.Bytes());
}

void LocalUnlock(Runtime& rt, Cpu& cpu) {  // (HLOCAL) -> still locked
    const PascalArgs a(cpu, {2});
    cpu.Regs().r[AX] = rt.Locals().Unlock(cpu.Regs().s[DS], a.Word(0)) ? 1 : 0;
    cpu.ReturnFar(a.Bytes());
}

void LocalSize(Runtime& rt, Cpu& cpu) {  // (HLOCAL) -> bytes
    const PascalArgs a(cpu, {2});
    cpu.Regs().r[AX] = rt.Locals().Size(cpu.Regs().s[DS], a.Word(0));
    cpu.ReturnFar(a.Bytes());
}

void LocalHandle(Runtime& rt, Cpu& cpu) {  // (near pointer) -> HLOCAL
    const PascalArgs a(cpu, {2});
    cpu.Regs().r[AX] = rt.Locals().HandleFor(cpu.Regs().s[DS], a.Word(0));
    cpu.ReturnFar(a.Bytes());
}

void LocalFlags(Runtime& rt, Cpu& cpu) {  // (HLOCAL) -> lock count | discard flags
    const PascalArgs a(cpu, {2});
    cpu.Regs().r[AX] = rt.Locals().Flags(cpu.Regs().s[DS], a.Word(0));
    cpu.ReturnFar(a.Bytes());
}

void LocalCompact(Runtime& rt, Cpu& cpu) {  // (bytes wanted) -> largest free block
    const PascalArgs a(cpu, {2});
    cpu.Regs().r[AX] = rt.Locals().Compact(cpu.Regs().s[DS]);
    cpu.ReturnFar(a.Bytes());
}

// --- Strings ------------------------------------------------------------------------------------

// A string argument; `present` false for NULL.
std::string ReadArgString(Runtime& rt, FarPtr p, bool* present = nullptr, size_t max = 4096) {
    if (present) *present = !p.IsNull();
    return p.IsNull() ? std::string() : rt.Mem().ReadString(p.sel, p.off, max);
}

// Copies `s` (at most size - 1 characters) and a NUL to `dst`; returns the characters copied.
uint16_t CopyOut(Runtime& rt, FarPtr dst, const std::string& s, uint16_t size) {
    if (dst.IsNull() || size == 0) return 0;
    const uint16_t n = uint16_t(std::min<size_t>(s.size(), size - 1u));
    for (uint16_t i = 0; i < n; ++i) rt.Mem().Write8(dst.sel, uint16_t(dst.off + i), uint8_t(s[i]));
    rt.Mem().Write8(dst.sel, uint16_t(dst.off + n), 0);
    return n;
}

void lstrcpy(Runtime& rt, Cpu& cpu) {  // (LPSTR dst, LPCSTR src) -> dst
    const PascalArgs a(cpu, {4, 4});
    const FarPtr dst = a.Ptr(0);
    CopyOut(rt, dst, ReadArgString(rt, a.Ptr(1), nullptr, 0xFFFF), 0xFFFF);
    SetResult(cpu, (uint32_t(dst.sel) << 16) | dst.off);
    cpu.ReturnFar(a.Bytes());
}

void lstrcpyn(Runtime& rt, Cpu& cpu) {  // (LPSTR dst, LPCSTR src, int n) -> dst
    const PascalArgs a(cpu, {4, 4, 2});
    const FarPtr dst = a.Ptr(0);
    CopyOut(rt, dst, ReadArgString(rt, a.Ptr(1), nullptr, 0xFFFF), a.Word(2));
    SetResult(cpu, (uint32_t(dst.sel) << 16) | dst.off);
    cpu.ReturnFar(a.Bytes());
}

void lstrcat(Runtime& rt, Cpu& cpu) {  // (LPSTR dst, LPCSTR src) -> dst
    const PascalArgs a(cpu, {4, 4});
    const FarPtr dst = a.Ptr(0);
    const std::string existing = ReadArgString(rt, dst, nullptr, 0xFFFF);
    CopyOut(rt, {uint16_t(dst.off + existing.size()), dst.sel}, ReadArgString(rt, a.Ptr(1), nullptr, 0xFFFF),
            0xFFFF);
    SetResult(cpu, (uint32_t(dst.sel) << 16) | dst.off);
    cpu.ReturnFar(a.Bytes());
}

void lstrlen(Runtime& rt, Cpu& cpu) {  // (LPCSTR) -> length
    const PascalArgs a(cpu, {4});
    cpu.Regs().r[AX] = uint16_t(ReadArgString(rt, a.Ptr(0), nullptr, 0xFFFF).size());
    cpu.ReturnFar(a.Bytes());
}

void OutputDebugString(Runtime& rt, Cpu& cpu) {  // (LPCSTR)
    const PascalArgs a(cpu, {4});
    std::string s = ReadArgString(rt, a.Ptr(0));
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
    rt.Print("OutputDebugString: " + s);
    cpu.ReturnFar(a.Bytes());
}

// --- Modules and the task -------------------------------------------------------------------

void GetModuleHandle(Runtime& rt, Cpu& cpu) {  // (LPCSTR name, or an instance handle) -> HMODULE
    const PascalArgs a(cpu, {4});
    const FarPtr p = a.Ptr(0);
    uint16_t h = 0;
    if (p.sel == 0) {  // MAKELP(0, hInstance)
        h = p.off == rt.Module().dgroup ? rt.ModuleHandle() : rt.FindModuleHandle(std::string());
    } else {
        h = rt.FindModuleHandle(ReadArgString(rt, p, nullptr, 128));
    }
    cpu.Regs().r[AX] = h;
    cpu.Regs().r[DX] = h == rt.ModuleHandle() ? rt.Module().dgroup : 0;
    cpu.ReturnFar(a.Bytes());
}

void GetModuleFileName(Runtime& rt, Cpu& cpu) {  // (HINSTANCE, LPSTR buffer, int size) -> length
    const PascalArgs a(cpu, {2, 4, 2});
    const uint16_t h = a.Word(0);
    std::string path;
    if (h == 0 || h == rt.Module().dgroup || h == rt.ModuleHandle()) {
        path = rt.Files().ProgramPath();
    } else {
        for (const char* m : {"KERNEL", "USER", "GDI"}) {
            if (rt.FindModuleHandle(m) == h) path = std::string("C:\\WINDOWS\\SYSTEM\\") + m + ".EXE";
        }
    }
    const int size = a.Int(2);
    cpu.Regs().r[AX] = size > 0 ? CopyOut(rt, a.Ptr(1), path, uint16_t(size)) : 0;
    cpu.ReturnFar(a.Bytes());
}

void GetProcAddress(Runtime& rt, Cpu& cpu) {  // (HMODULE, LPCSTR name or MAKEINTRESOURCE(ordinal))
    const PascalArgs a(cpu, {2, 4});
    const FarPtr p = a.Ptr(1);
    const std::string name = p.sel ? ReadArgString(rt, p, nullptr, 128) : std::string();
    SetResult(cpu, rt.ProcAddress(a.Word(0), p.sel ? 0 : p.off, name));
    cpu.ReturnFar(a.Bytes());
}

// Exported callbacks set up their own DS from AX (Cpu::CallFar provides it),
// so an instance thunk isn't needed: the procedure itself is returned.
void MakeProcInstance(Runtime&, Cpu& cpu) {  // (FARPROC, HINSTANCE) -> FARPROC
    const PascalArgs a(cpu, {4, 2});
    SetResult(cpu, a.Long(0));
    cpu.ReturnFar(a.Bytes());
}

void FreeProcInstance(Runtime&, Cpu& cpu) {  // (FARPROC)
    const PascalArgs a(cpu, {4});
    cpu.ReturnFar(a.Bytes());
}

void LoadLibrary(Runtime& rt, Cpu& cpu) {  // (LPCSTR) -> HINSTANCE, or an error < 32
    const PascalArgs a(cpu, {4});
    const std::string name = ReadArgString(rt, a.Ptr(0), nullptr, 128);
    uint16_t h = rt.LoadBuiltinModule(name);
    if (!h) {
        rt.Note("loadlibrary:" + name, "LoadLibrary(\"" + name + "\"): loading NE DLLs isn't supported yet (file not found)");
        h = 2;
    }
    cpu.Regs().r[AX] = h;
    cpu.ReturnFar(a.Bytes());
}

void FreeLibrary(Runtime&, Cpu& cpu) {  // (HINSTANCE)
    const PascalArgs a(cpu, {2});
    cpu.ReturnFar(a.Bytes());
}

void GetInstanceData(Runtime&, Cpu& cpu) {  // (HINSTANCE prev, offset, count): no previous instance
    const PascalArgs a(cpu, {2, 2, 2});
    cpu.Regs().r[AX] = 0;
    cpu.ReturnFar(a.Bytes());
}

void GetCurrentTask(Runtime& rt, Cpu& cpu) {  // () -> HTASK (the PSP stands in for the task database)
    SetResult(cpu, rt.Module().psp);
    cpu.ReturnFar(0);
}

void GetCurrentPDB(Runtime& rt, Cpu& cpu) {  // () -> PSP selector
    SetResult(cpu, rt.Module().psp);
    cpu.ReturnFar(0);
}

void GetDOSEnvironment(Runtime& rt, Cpu& cpu) {  // () -> LPSTR
    SetResult(cpu, uint32_t(rt.Environment()) << 16);
    cpu.ReturnFar(0);
}

void GetWinFlags(Runtime&, Cpu& cpu) {  // () -> DWORD
    SetResult(cpu, Runtime::kWinFlags);
    cpu.ReturnFar(0);
}

void Yield(Runtime&, Cpu& cpu) { cpu.ReturnFar(0); }

void SetErrorMode(Runtime&, Cpu& cpu) {  // (UINT) -> previous
    static uint16_t mode = 0;
    const PascalArgs a(cpu, {2});
    cpu.Regs().r[AX] = mode;
    mode = a.Word(0);
    cpu.ReturnFar(a.Bytes());
}

void GetWindowsDirectory(Runtime& rt, Cpu& cpu) {  // (LPSTR, UINT size) -> length
    const PascalArgs a(cpu, {4, 2});
    const std::string dir = "C:\\WINDOWS";  // maps to the program's directory
    cpu.Regs().r[AX] = a.Word(1) > dir.size() ? CopyOut(rt, a.Ptr(0), dir, a.Word(1)) : uint16_t(dir.size() + 1);
    cpu.ReturnFar(a.Bytes());
}

void GetSystemDirectory(Runtime& rt, Cpu& cpu) {  // (LPSTR, UINT size) -> length
    const PascalArgs a(cpu, {4, 2});
    const std::string dir = "C:\\WINDOWS\\SYSTEM";
    cpu.Regs().r[AX] = a.Word(1) > dir.size() ? CopyOut(rt, a.Ptr(0), dir, a.Word(1)) : uint16_t(dir.size() + 1);
    cpu.ReturnFar(a.Bytes());
}

// --- Profiles (INI files) -----------------------------------------------------------------------

int16_t ParseProfileInt(const std::string& s) {
    size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
    const bool negative = i < s.size() && s[i] == '-';
    if (negative || (i < s.size() && s[i] == '+')) ++i;
    int32_t v = 0;
    for (; i < s.size() && std::isdigit(static_cast<unsigned char>(s[i])); ++i) v = v * 10 + (s[i] - '0');
    return int16_t(negative ? -v : v);
}

uint16_t ProfileInt(Runtime& rt, const std::string& file, FarPtr app, FarPtr key, int16_t def) {
    std::string value;
    if (app.IsNull() || key.IsNull() ||
        !rt.Profile().Get(file, ReadArgString(rt, app), ReadArgString(rt, key), value))
        return uint16_t(def);
    return uint16_t(ParseProfileInt(value));
}

uint16_t ProfileString(Runtime& rt, const std::string& file, FarPtr app, FarPtr key, FarPtr def, FarPtr buf,
                       uint16_t size) {
    std::string value;
    const bool list = app.IsNull() || key.IsNull();  // section or key names
    const bool found = rt.Profile().Get(file, ReadArgString(rt, app), ReadArgString(rt, key), value);
    if (!found && !list) value = ReadArgString(rt, def);
    if (buf.IsNull() || size == 0) return 0;
    if (!list) return CopyOut(rt, buf, value, size);
    // NUL-separated names ending with a double NUL.
    if (size < 2) return 0;
    const uint16_t n = uint16_t(std::min<size_t>(value.size(), size - 2u));
    for (uint16_t i = 0; i < n; ++i) rt.Mem().Write8(buf.sel, uint16_t(buf.off + i), uint8_t(value[i]));
    rt.Mem().Write8(buf.sel, uint16_t(buf.off + n), 0);
    rt.Mem().Write8(buf.sel, uint16_t(buf.off + n + 1), 0);
    return n;
}

void ProfileWrite(Runtime& rt, const std::string& file, FarPtr app, FarPtr key, FarPtr value) {
    rt.Note("ini-write", "INI changes are kept while the program runs; saving them isn't supported yet");
    if (app.IsNull()) return;
    const std::string k = ReadArgString(rt, key), v = ReadArgString(rt, value);
    rt.Profile().Write(file, ReadArgString(rt, app), key.IsNull() ? nullptr : &k, value.IsNull() ? nullptr : &v);
}

void GetProfileInt(Runtime& rt, Cpu& cpu) {  // (app, key, default)
    const PascalArgs a(cpu, {4, 4, 2});
    cpu.Regs().r[AX] = ProfileInt(rt, "", a.Ptr(0), a.Ptr(1), a.Int(2));
    cpu.ReturnFar(a.Bytes());
}

void GetPrivateProfileInt(Runtime& rt, Cpu& cpu) {  // (app, key, default, file)
    const PascalArgs a(cpu, {4, 4, 2, 4});
    cpu.Regs().r[AX] = ProfileInt(rt, ReadArgString(rt, a.Ptr(3), nullptr, 128), a.Ptr(0), a.Ptr(1), a.Int(2));
    cpu.ReturnFar(a.Bytes());
}

void GetProfileString(Runtime& rt, Cpu& cpu) {  // (app, key, default, buffer, size)
    const PascalArgs a(cpu, {4, 4, 4, 4, 2});
    cpu.Regs().r[AX] = ProfileString(rt, "", a.Ptr(0), a.Ptr(1), a.Ptr(2), a.Ptr(3), a.Word(4));
    cpu.ReturnFar(a.Bytes());
}

void GetPrivateProfileString(Runtime& rt, Cpu& cpu) {  // (app, key, default, buffer, size, file)
    const PascalArgs a(cpu, {4, 4, 4, 4, 2, 4});
    cpu.Regs().r[AX] = ProfileString(rt, ReadArgString(rt, a.Ptr(5), nullptr, 128), a.Ptr(0), a.Ptr(1), a.Ptr(2),
                                     a.Ptr(3), a.Word(4));
    cpu.ReturnFar(a.Bytes());
}

void WriteProfileString(Runtime& rt, Cpu& cpu) {  // (app, key, value) -> BOOL
    const PascalArgs a(cpu, {4, 4, 4});
    ProfileWrite(rt, "", a.Ptr(0), a.Ptr(1), a.Ptr(2));
    cpu.Regs().r[AX] = 1;
    cpu.ReturnFar(a.Bytes());
}

void WritePrivateProfileString(Runtime& rt, Cpu& cpu) {  // (app, key, value, file) -> BOOL
    const PascalArgs a(cpu, {4, 4, 4, 4});
    ProfileWrite(rt, ReadArgString(rt, a.Ptr(3), nullptr, 128), a.Ptr(0), a.Ptr(1), a.Ptr(2));
    cpu.Regs().r[AX] = 1;
    cpu.ReturnFar(a.Bytes());
}

// --- Files ----------------------------------------------------------------------------------------

constexpr uint16_t kHfileError = 0xFFFF;

void NoteWrite(Runtime& rt, const std::string& path) {
    rt.Note("write:" + path, "opening \"" + path + "\" for writing: not supported yet (access denied)");
}

void _lopen(Runtime& rt, Cpu& cpu) {  // (LPCSTR path, mode) -> HFILE
    const PascalArgs a(cpu, {4, 2});
    const std::string path = ReadArgString(rt, a.Ptr(0), nullptr, 128);
    uint16_t error = 0;
    const int h = rt.Files().Open(path, a.Word(1), error);
    if (h < 0 && error == dos::AccessDenied && (a.Word(1) & 3)) NoteWrite(rt, path);
    cpu.Regs().r[AX] = h < 0 ? kHfileError : uint16_t(h);
    cpu.ReturnFar(a.Bytes());
}

void _lcreat(Runtime& rt, Cpu& cpu) {  // (LPCSTR path, attributes) -> HFILE
    const PascalArgs a(cpu, {4, 2});
    NoteWrite(rt, ReadArgString(rt, a.Ptr(0), nullptr, 128));
    cpu.Regs().r[AX] = kHfileError;
    cpu.ReturnFar(a.Bytes());
}

void _lclose(Runtime& rt, Cpu& cpu) {  // (HFILE) -> 0, or HFILE_ERROR
    const PascalArgs a(cpu, {2});
    cpu.Regs().r[AX] = rt.Files().Close(a.Word(0)) ? 0 : kHfileError;
    cpu.ReturnFar(a.Bytes());
}

void _lread(Runtime& rt, Cpu& cpu) {  // (HFILE, void FAR* buffer, UINT bytes) -> bytes read
    const PascalArgs a(cpu, {2, 4, 2});
    const FarPtr buf = a.Ptr(1);
    const uint16_t n = a.Word(2);
    uint16_t result = kHfileError;
    if (rt.Files().IsOpen(a.Word(0))) {
        if (n == 0) {
            result = 0;
        } else {
            rt.Mem().Translate(buf.sel, buf.off, n, Access::Write);  // #GP if the buffer is bad
            result = uint16_t(rt.Files().Read(a.Word(0), rt.Mem().SegmentData(buf.sel) + buf.off, n));
        }
    }
    cpu.Regs().r[AX] = result;
    cpu.ReturnFar(a.Bytes());
}

void _lwrite(Runtime& rt, Cpu& cpu) {  // (HFILE, const void FAR*, UINT) -> bytes written
    const PascalArgs a(cpu, {2, 4, 2});
    rt.Note("lwrite", "the program tried to write a file: not supported yet (the write failed)");
    cpu.Regs().r[AX] = kHfileError;
    cpu.ReturnFar(a.Bytes());
}

void _llseek(Runtime& rt, Cpu& cpu) {  // (HFILE, LONG offset, int origin) -> new position
    const PascalArgs a(cpu, {2, 4, 2});
    const int32_t pos = rt.Files().Seek(a.Word(0), int32_t(a.Long(1)), a.Int(2));
    SetResult(cpu, pos < 0 ? 0xFFFFFFFFu : uint32_t(pos));
    cpu.ReturnFar(a.Bytes());
}

// OFSTRUCT: cBytes, fFixedDisk, nErrCode (WORD), reserved[4], szPathName[128].
void OpenFile(Runtime& rt, Cpu& cpu) {  // (LPCSTR path, OFSTRUCT FAR*, UINT style) -> HFILE
    const PascalArgs a(cpu, {4, 4, 2});
    constexpr uint16_t kParse = 0x0100, kDelete = 0x0200, kCreate = 0x1000, kExist = 0x4000, kReopen = 0x8000;
    const FarPtr of = a.Ptr(1);
    const uint16_t style = a.Word(2);
    Memory& mem = rt.Mem();
    std::string path = (style & kReopen) && !of.IsNull() ? mem.ReadString(of.sel, uint16_t(of.off + 8), 127)
                                                          : ReadArgString(rt, a.Ptr(0), nullptr, 128);
    std::filesystem::path host;
    std::string why, full = path;
    if (rt.Files().Resolve(path, host, why)) {
        try {
            full = host.string();
        } catch (const std::exception&) {
        }
    }
    uint16_t error = 0;
    int h = -1;
    if (style & kParse) {
        h = 0;
    } else if (style & (kDelete | kCreate)) {
        NoteWrite(rt, path);
        error = dos::AccessDenied;
    } else {
        h = rt.Files().Open(path, style, error);
        if (h < 0 && error == dos::AccessDenied && (style & 3)) NoteWrite(rt, path);
        if (h >= 0 && (style & kExist)) rt.Files().Close(h);  // just checking
    }
    if (!of.IsNull()) {
        mem.Write8(of.sel, of.off, 136);
        mem.Write8(of.sel, uint16_t(of.off + 1), 1);  // fixed disk
        mem.Write16(of.sel, uint16_t(of.off + 2), h < 0 ? error : 0);
        for (uint16_t i = 4; i < 8; ++i) mem.Write8(of.sel, uint16_t(of.off + i), 0);
        CopyOut(rt, {uint16_t(of.off + 8), of.sel}, full, 128);
    }
    cpu.Regs().r[AX] = h < 0 ? kHfileError : uint16_t(h);
    cpu.ReturnFar(a.Bytes());
}

// --- Resources -------------------------------------------------------------------------------

void FindResource(Runtime& rt, Cpu& cpu) {  // (HINSTANCE, LPCSTR name, LPCSTR type) -> HRSRC
    const PascalArgs a(cpu, {2, 4, 4});
    const FarPtr name = a.Ptr(1), type = a.Ptr(2);
    cpu.Regs().r[AX] = rt.Resource().Find(ResourceId::FromFarPtr(rt.Mem(), type.sel, type.off),
                                          ResourceId::FromFarPtr(rt.Mem(), name.sel, name.off));
    cpu.ReturnFar(a.Bytes());
}

void LoadResource(Runtime& rt, Cpu& cpu) {  // (HINSTANCE, HRSRC) -> HGLOBAL
    const PascalArgs a(cpu, {2, 2});
    cpu.Regs().r[AX] = rt.Resource().Load(a.Word(1));
    cpu.ReturnFar(a.Bytes());
}

void LockResource(Runtime& rt, Cpu& cpu) {  // (HGLOBAL) -> void FAR*
    const PascalArgs a(cpu, {2});
    SetResult(cpu, rt.Globals().Lock(a.Word(0)));
    cpu.ReturnFar(a.Bytes());
}

void FreeResource(Runtime& rt, Cpu& cpu) {  // (HGLOBAL) -> 0 on success
    const PascalArgs a(cpu, {2});
    cpu.Regs().r[AX] = rt.Resource().Free(a.Word(0));
    cpu.ReturnFar(a.Bytes());
}

void SizeofResource(Runtime& rt, Cpu& cpu) {  // (HINSTANCE, HRSRC) -> DWORD
    const PascalArgs a(cpu, {2, 2});
    const NeResource* r = rt.Resource().Get(a.Word(1));
    SetResult(cpu, r ? uint32_t(r->data.size()) : 0);
    cpu.ReturnFar(a.Bytes());
}

}  // namespace

std::vector<ApiFunction> KernelApi() {
    return {
        {1, "FATALEXIT", FatalExit},
        {3, "GETVERSION", GetVersion},
        {4, "LOCALINIT", LocalInit},
        {5, "LOCALALLOC", LocalAlloc},
        {6, "LOCALREALLOC", LocalReAlloc},
        {7, "LOCALFREE", LocalFree},
        {8, "LOCALLOCK", LocalLock},
        {9, "LOCALUNLOCK", LocalUnlock},
        {10, "LOCALSIZE", LocalSize},
        {11, "LOCALHANDLE", LocalHandle},
        {12, "LOCALFLAGS", LocalFlags},
        {13, "LOCALCOMPACT", LocalCompact},
        {15, "GLOBALALLOC", GlobalAlloc},
        {16, "GLOBALREALLOC", GlobalReAlloc},
        {17, "GLOBALFREE", GlobalFree},
        {18, "GLOBALLOCK", GlobalLock},
        {19, "GLOBALUNLOCK", GlobalUnlock},
        {20, "GLOBALSIZE", GlobalSize},
        {21, "GLOBALHANDLE", GlobalHandle},
        {22, "GLOBALFLAGS", GlobalFlags},
        {23, "LOCKSEGMENT", LockSegment},
        {24, "UNLOCKSEGMENT", LockSegment},
        {25, "GLOBALCOMPACT", GlobalCompact},
        {29, "YIELD", Yield},
        {30, "WAITEVENT", WaitEvent},
        {36, "GETCURRENTTASK", GetCurrentTask},
        {37, "GETCURRENTPDB", GetCurrentPDB},
        {47, "GETMODULEHANDLE", GetModuleHandle},
        {49, "GETMODULEFILENAME", GetModuleFileName},
        {50, "GETPROCADDRESS", GetProcAddress},
        {51, "MAKEPROCINSTANCE", MakeProcInstance},
        {52, "FREEPROCINSTANCE", FreeProcInstance},
        {54, "GETINSTANCEDATA", GetInstanceData},
        {57, "GETPROFILEINT", GetProfileInt},
        {58, "GETPROFILESTRING", GetProfileString},
        {59, "WRITEPROFILESTRING", WriteProfileString},
        {60, "FINDRESOURCE", FindResource},
        {61, "LOADRESOURCE", LoadResource},
        {62, "LOCKRESOURCE", LockResource},
        {63, "FREERESOURCE", FreeResource},
        {65, "SIZEOFRESOURCE", SizeofResource},
        {74, "OPENFILE", OpenFile},
        {81, "_LCLOSE", _lclose},
        {82, "_LREAD", _lread},
        {83, "_LCREAT", _lcreat},
        {84, "_LLSEEK", _llseek},
        {85, "_LOPEN", _lopen},
        {86, "_LWRITE", _lwrite},
        {88, "LSTRCPY", lstrcpy},
        {89, "LSTRCAT", lstrcat},
        {90, "LSTRLEN", lstrlen},
        {91, "INITTASK", InitTask},
        {95, "LOADLIBRARY", LoadLibrary},
        {96, "FREELIBRARY", FreeLibrary},
        {102, "DOS3CALL", Dos3Call},
        {107, "SETERRORMODE", SetErrorMode},
        {115, "OUTPUTDEBUGSTRING", OutputDebugString},
        {127, "GETPRIVATEPROFILEINT", GetPrivateProfileInt},
        {128, "GETPRIVATEPROFILESTRING", GetPrivateProfileString},
        {129, "WRITEPRIVATEPROFILESTRING", WritePrivateProfileString},
        {131, "GETDOSENVIRONMENT", GetDOSEnvironment},
        {132, "GETWINFLAGS", GetWinFlags},
        {134, "GETWINDOWSDIRECTORY", GetWindowsDirectory},
        {135, "GETSYSTEMDIRECTORY", GetSystemDirectory},
        {137, "FATALAPPEXIT", FatalAppExit},
        {169, "GETFREESPACE", GetFreeSpace},
        {353, "LSTRCPYN", lstrcpyn},
    };
}

}  // namespace retro::win16
