// KERNEL: task startup, termination, version, DOS calls and the global heap.

#include "win16/Kernel.h"

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

}  // namespace

std::vector<ApiFunction> KernelApi() {
    return {
        {1, "FATALEXIT", FatalExit},
        {3, "GETVERSION", GetVersion},
        {15, "GLOBALALLOC", GlobalAlloc},
        {17, "GLOBALFREE", GlobalFree},
        {18, "GLOBALLOCK", GlobalLock},
        {19, "GLOBALUNLOCK", GlobalUnlock},
        {20, "GLOBALSIZE", GlobalSize},
        {30, "WAITEVENT", WaitEvent},
        {91, "INITTASK", InitTask},
        {102, "DOS3CALL", Dos3Call},
        {137, "FATALAPPEXIT", FatalAppExit},
    };
}

}  // namespace retro::win16
