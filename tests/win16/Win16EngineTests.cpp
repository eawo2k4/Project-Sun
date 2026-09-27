// Win16 engine tests: NE parsing, loading (selectors, relocations, initial
// registers), resources, timers, and running synthetic programs to completion.

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include "../Check.h"
#include "retro/FramePacing.h"
#include "TestPrograms.h"
#include "win16/ApiCatalog.h"
#include "win16/Atoms.h"
#include "win16/Files.h"
#include "win16/LocalHeap.h"
#include "win16/Menus.h"
#include "win16/Memory.h"
#include "win16/NeImage.h"
#include "win16/Runtime.h"

using namespace retro::win16;
using namespace win16test;

namespace {

struct RunOutcome {
    bool loaded = false;
    std::string loadError;
    TaskExit exit;
    std::vector<std::string> output;
};

bool StartsWith(const std::string& s, const std::string& prefix) { return s.rfind(prefix, 0) == 0; }

struct RunSettings {
    bool trace = false;
    bool stubMissing = false;
    std::filesystem::path program;  // the .exe path (its directory is what files see)
};

RunOutcome RunProgram(const NeProgram& program, uint64_t budget = 1'000'000, const RunSettings& s = {}) {
    RunOutcome o;
    Runtime rt;
    rt.SetOutput([&](const std::string& line) { o.output.push_back(line); });
    rt.SetTrace(s.trace);
    rt.SetStubMissing(s.stubMissing);
    if (!s.program.empty()) rt.SetProgram(s.program);
    o.loaded = rt.Load(BuildNe(program), "arg1 arg2", o.loadError);
    if (o.loaded) o.exit = rt.Run(budget);
    std::printf("  -> %s code %u: %s (%llu instructions)\n", ToString(o.exit.kind), o.exit.code,
                o.loaded ? o.exit.message.c_str() : o.loadError.c_str(),
                static_cast<unsigned long long>(o.exit.instructions));
    return o;
}

// --- NE parsing -------------------------------------------------------------------------

void TestParseSelfTestImage() {
    const NeProgram program = SelfTestProgram();
    NeImage img;
    std::string error;
    CHECK(ParseNe(BuildNe(program), img, error));
    CHECK(error.empty());
    CHECK(img.moduleName == "TESTAPP");
    CHECK(img.segments.size() == 3);
    CHECK(!img.segments[0].IsData() && img.segments[1].IsData() && !img.segments[2].IsData());
    CHECK(img.segments[0].fileLength == program.segments[0].bytes.size());
    CHECK(img.segments[1].minAlloc == 0x100);
    CHECK(img.entrySegment == 1 && img.entryIp == 0);
    CHECK(img.autoDataSegment == 2 && img.heapSize == 0x100 && img.stackSize == 0x400);
    CHECK((img.moduleRefs == std::vector<std::string>{"KERNEL", "USER"}));
    CHECK(img.expectedWinVer == 0x030A && img.targetOS == 2 && !img.IsLibrary());

    // Relocations: InitTask, far call to segment 3, GetVersion.
    const auto& relocs = img.segments[0].relocations;
    CHECK(relocs.size() == 3);
    CHECK(relocs[0].target == RelocTarget::ImportOrdinal && relocs[0].module == 1 &&
          relocs[0].ordinal == 91 && relocs[0].source == RelocSource::FarPointer);
    CHECK(relocs[1].target == RelocTarget::Internal && relocs[1].segment == 3 &&
          relocs[1].targetOffset == 0);
    CHECK(relocs[2].ordinal == 3);
}

void TestParseByName() {
    NeImage img;
    std::string error;
    CHECK(ParseNe(BuildNe(ByNameProgram(false)), img, error));
    CHECK(img.segments[0].relocations.size() == 1);
    CHECK(img.segments[0].relocations[0].target == RelocTarget::ImportName);
    CHECK(img.segments[0].relocations[0].name == "GETVERSION");
}

void TestParseRejectsBadImages() {
    NeImage img;
    std::string error;
    CHECK(!ParseNe({'M', 'Z'}, img, error));
    std::vector<uint8_t> file = BuildNe(SelfTestProgram());
    file[0x40] = 'P';  // not NE any more
    CHECK(!ParseNe(file, img, error));
    file = BuildNe(SelfTestProgram());
    file.resize(file.size() - 20);  // cut into the last segment
    CHECK(!ParseNe(file, img, error));
    std::printf("  truncated image: %s\n", error.c_str());
}

// --- Memory / LDT --------------------------------------------------------------------------

void TestSelectorsAndProtection() {
    Memory mem(1 << 20);
    const uint16_t code = mem.Allocate(0x20, SegmentKind::Code);
    const uint16_t data = mem.Allocate(0x100, SegmentKind::Data);
    CHECK(code == 0x0107 && data == 0x010F);  // Windows-style LDT selectors
    CHECK(mem.Lookup(code)->kind == SegmentKind::Code);
    CHECK(mem.SegmentSize(data) == 0x100);

    mem.Write16(data, 0xFE, 0xBEEF);
    CHECK(mem.Read16(data, 0xFE) == 0xBEEF);

    auto faults = [&](auto fn) {
        try {
            fn();
        } catch (const ProtectionFault&) {
            return true;
        }
        return false;
    };
    CHECK(faults([&] { mem.Read16(data, 0xFF); }));        // straddles the limit
    CHECK(faults([&] { mem.Read8(data, 0x100); }));        // past the limit
    CHECK(faults([&] { mem.Write8(code, 0, 1); }));        // write to code
    CHECK(faults([&] { mem.Read8(0, 0); }));               // null selector
    CHECK(faults([&] { mem.Read8(0x0108, 0); }));          // GDT selector
    CHECK(faults([&] { mem.Read8(data, 0, Access::Execute); }));  // execute data
    mem.Free(data);
    CHECK(faults([&] { mem.Read8(data, 0); }));            // freed
    CHECK(mem.Lookup(data) == nullptr);
}

// --- Loading and running ----------------------------------------------------------------------

void TestLoaderSetsUpTask() {
    Runtime rt;
    std::string error;
    CHECK(rt.Load(BuildNe(SelfTestProgram()), "hello world", error));
    const LoadedModule& m = rt.Module();
    const Registers& r = rt.Processor().Regs();
    CHECK(m.selectors.size() == 3);
    CHECK(r.s[CS] == m.selectors[0] && r.ip == 0);
    CHECK(r.s[DS] == m.dgroup && m.dgroup == m.selectors[1]);
    CHECK(r.s[SS] == m.dgroup);
    CHECK(rt.Mem().SegmentSize(m.dgroup) == 0x100 + 0x100 + 0x400);  // + heap + stack
    // Like Windows: static data, then the stack, then the local heap.
    CHECK(r.r[SP] == 0x500 && m.heapStart == 0x500 && rt.Locals().Has(m.dgroup));
    // The environment is in the PSP; hModule is a copy of the NE header.
    CHECK(rt.Environment() && rt.Mem().Read16(m.psp, 0x2C) == rt.Environment());
    CHECK(rt.Mem().Read16(rt.ModuleHandle(), 0) == 0x454E);  // "NE"
    CHECK(r.s[ES] == m.psp && r.r[DI] == m.dgroup && r.r[BX] == 0x400 && r.r[CX] == 0x100);

    // The far call to segment 3 was relocated to segment 3's selector.
    const uint8_t* code = rt.Mem().SegmentData(m.selectors[0]);
    bool found = false;
    for (uint32_t i = 0; i + 4 < rt.Mem().SegmentSize(m.selectors[0]); ++i) {
        if (code[i] == 0x9A && code[i + 3] == uint8_t(m.selectors[2]) &&
            code[i + 4] == uint8_t(m.selectors[2] >> 8)) {
            found = true;
        }
    }
    CHECK(found);

    // PSP: INT 20h and the command tail.
    const uint8_t* psp = rt.Mem().SegmentData(m.psp);
    CHECK(psp[0] == 0xCD && psp[1] == 0x20);
    CHECK(psp[0x80] == 12 && std::string(reinterpret_cast<const char*>(psp + 0x81), 12) == " hello world");
    CHECK(psp[0x8D] == 0x0D);
}

void TestSelfTestProgramPasses() {
    const RunOutcome o = RunProgram(SelfTestProgram());
    CHECK(o.loaded);
    CHECK(o.exit.kind == TaskExit::Kind::Exited);
    CHECK(o.exit.code == 0);  // otherwise: number of the failed check
    CHECK(o.exit.instructions > 100);
}

void TestFatalExit() {
    const RunOutcome o = RunProgram(FatalExitProgram(7));
    CHECK(o.exit.kind == TaskExit::Kind::FatalExit && o.exit.code == 7);
    CHECK(o.exit.message == "FatalExit(7)");
}

void TestMessageBoxAndDosOutput() {
    RunOutcome o = RunProgram(HelloProgram());
    CHECK(o.exit.kind == TaskExit::Kind::Exited && o.exit.code == 0);
    CHECK(o.output.size() == 1 && o.output[0] == "MessageBox [Project Sun]: Hello from Win16");

    o = RunProgram(DosPrintProgram());
    CHECK(o.exit.kind == TaskExit::Kind::Exited && o.exit.code == 5);
    CHECK(o.output.size() == 1 && o.output[0] == "Hi from DOS");
}

void TestImportsByName() {
    RunOutcome o = RunProgram(ByNameProgram(false));
    CHECK(o.exit.kind == TaskExit::Kind::Exited && o.exit.code == 3);  // AL = major version

    o = RunProgram(ByNameProgram(true));
    CHECK(o.exit.kind == TaskExit::Kind::Unimplemented);
    CHECK(StartsWith(o.exit.message, "KERNEL.FROBNICATE is not implemented yet (returning to "));
}

void TestUnimplementedApiStopsCleanly() {
    const RunOutcome o = RunProgram(UnimplementedApiProgram());
    CHECK(o.exit.kind == TaskExit::Kind::Unimplemented);
    // Named from the catalog, with the return address (after the 5-byte CALL at 0).
    CHECK(StartsWith(o.exit.message, "USER.7 (ExitWindows) is not implemented yet (returning to "));
    CHECK(o.exit.message.find(":0005)") != std::string::npos);
}

void TestFaultsAreReported() {
    RunOutcome o = RunProgram(OutOfBoundsProgram());
    CHECK(o.exit.kind == TaskExit::Kind::Fault);
    CHECK(o.exit.message.find("general protection fault") != std::string::npos);
    CHECK(o.exit.registers.ip == 1);  // the faulting instruction, not the one after

    o = RunProgram(DivideByZeroProgram());
    CHECK(o.exit.kind == TaskExit::Kind::Fault);
    CHECK(o.exit.message.find("divide error") != std::string::npos);
}

void TestUnbuiltModulesLoadAsStubs() {
    // SHELL isn't implemented but is known: the program loads, and the call
    // stops it by name.
    RunOutcome o = RunProgram(MissingModuleProgram());
    CHECK(o.loaded && o.exit.kind == TaskExit::Kind::Unimplemented);
    CHECK(StartsWith(o.exit.message, "SHELL.22 (ShellAbout) is not implemented yet (returning to "));

    // A DLL nobody knows (the game's own): loads with a note, stops when called.
    o = RunProgram(MissingModuleProgram("GAMEDLL", 3));
    CHECK(o.loaded && o.exit.kind == TaskExit::Kind::Unimplemented);
    CHECK(StartsWith(o.exit.message, "GAMEDLL.3 is not implemented yet"));
    CHECK(o.exit.message.find("GAMEDLL is not built in") != std::string::npos);
    CHECK(!o.output.empty() && o.output[0].find("module GAMEDLL is not built in") != std::string::npos);

    // --stub-missing: known Pascal functions return 0 and the program goes on.
    o = RunProgram(MissingModuleProgram(), 1'000'000, {false, true, {}});
    CHECK(o.exit.kind == TaskExit::Kind::Exited && o.exit.code == 51);
    CHECK(o.output.size() == 1 && StartsWith(o.output[0], "SHELL.22 (ShellAbout) is not implemented yet: returning 0"));
    //   ...but not unknown ones: their arguments can't be removed.
    o = RunProgram(MissingModuleProgram("GAMEDLL", 3), 1'000'000, {false, true, {}});
    CHECK(o.exit.kind == TaskExit::Kind::Unimplemented);
}

// --- Windowing, messages, callbacks, global heap ----------------------------------------------

// Output of the last RunWithHost.
std::vector<std::string> g_output;

TaskExit RunWithHost(const NeProgram& program, HeadlessHost& host, Runtime& rt) {
    rt.SetWindowHost(&host);
    rt.SetMute(true);
    g_output.clear();
    rt.SetOutput([](const std::string& line) {
        std::printf("  [win16] %s\n", line.c_str());
        g_output.push_back(line);
        // Every API removes exactly the arguments the catalog lists.
        CHECK(line.find("internal error") == std::string::npos);
    });
    std::string error;
    if (!rt.Load(BuildNe(program), "", error)) {
        std::printf("  load failed: %s\n", error.c_str());
        return {};
    }
    const TaskExit e = rt.Run(1'000'000);
    std::printf("  -> %s code %u: %s (%llu instructions)\n", ToString(e.kind), e.code,
                e.message.c_str(), static_cast<unsigned long long>(e.instructions));
    return e;
}

void TestWindowProgramRunsToQuit() {
    HeadlessHost host;
    Runtime rt;
    const TaskExit e = RunWithHost(WindowProgram(false), host, rt);
    CHECK(e.kind == TaskExit::Kind::Exited && e.code == 0);  // else: number of the failed check

    // One top-level window reached the host, was shown, then destroyed.
    CHECK(host.windows.size() == 1);
    const HeadlessHost::Record& w = host.windows[0];
    CHECK(w.info.title == "Win16 Window" && w.info.width == 320 && w.info.height == 200);
    CHECK(!w.info.fullscreen && w.visible && w.destroyed);
    CHECK(rt.Windows().WindowCount() == 0 && rt.Windows().HwndForHost(w.id) == 0);
    CHECK(rt.Globals().Count() == 0);  // the block was freed
}

void TestHostCloseReachesWndProc() {
    // The program doesn't close its window: the host does (user clicked X).
    HeadlessHost host;
    host.events.push_back({1, wm::Close, 0, 0});
    Runtime rt;
    const TaskExit e = RunWithHost(WindowProgram(true), host, rt);
    CHECK(e.kind == TaskExit::Kind::Exited && e.code == 0);
    CHECK(host.windows.size() == 1 && host.windows[0].destroyed);
}

void TestHandleMappingWhileRunning() {
    // No close event: the task blocks in GetMessage with its window alive.
    HeadlessHost host;
    Runtime rt;
    const TaskExit e = RunWithHost(WindowProgram(true), host, rt);
    CHECK(e.kind == TaskExit::Kind::Blocked);
    CHECK(e.message.find("GetMessage") != std::string::npos);

    CHECK(host.windows.size() == 1);
    const uint64_t hostId = host.windows[0].id;
    const uint16_t hwnd = rt.Windows().HwndForHost(hostId);
    CHECK(hwnd != 0 && rt.Windows().HostForHwnd(hwnd) == hostId);  // both directions
    const User::Window* w = rt.Windows().Find(hwnd);
    CHECK(w && w->className == "RETROWIN" && w->visible && w->cx == 320);
    CHECK(rt.Windows().FindClass("RETROWIN") != nullptr);
    CHECK(host.windows[0].info.hwnd16 == hwnd);
}

void TestFullscreenWindowIsFlagged() {
    HeadlessHost host;
    Runtime rt;
    const TaskExit e = RunWithHost(WindowProgram(false, true), host, rt);
    CHECK(e.kind == TaskExit::Kind::Exited && e.code == 0);
    CHECK(host.windows.size() == 1);
    const HeadlessHost::Record& w = host.windows[0];
    CHECK(w.info.fullscreen && w.info.width == 640 && w.info.height == 480 && w.visible);
}

void TestFaultInsideCallbackIsReported() {
    // Divide by zero in the WndProc, during CreateWindow's WM_CREATE: the
    // fault unwinds through the nested callback and is reported where it hit.
    HeadlessHost host;
    Runtime rt;
    const TaskExit e = RunWithHost(WindowProgram(false, false, true), host, rt);
    CHECK(e.kind == TaskExit::Kind::Fault);
    CHECK(e.message.find("divide error") != std::string::npos);
    CHECK(host.windows.empty());  // never got as far as the host window
    CHECK(rt.Processor().CallbackDepth() == 0);
}

void TestGlobalHeap() {
    Memory mem(1 << 20);
    GlobalHeap heap(mem);
    const uint16_t fixed = heap.Alloc(gmem::Fixed, 100);
    const uint16_t moveable = heap.Alloc(gmem::Moveable | gmem::ZeroInit, 0x10000);
    CHECK(fixed && (fixed & 1));                    // fixed: the selector itself
    CHECK(moveable && !(moveable & 1));             // moveable: selector with bit 0 clear
    CHECK(heap.Size(fixed) == 100 && heap.Size(moveable) == 0x10000);
    CHECK(heap.Alloc(gmem::Fixed, 0x10001) == 0);   // huge blocks: not yet

    const uint32_t far = heap.Lock(moveable);
    const uint16_t sel = uint16_t(far >> 16);
    CHECK((far & 0xFFFF) == 0 && sel == (moveable | 1) && mem.SegmentSize(sel) == 0x10000);
    CHECK(heap.Lock(sel) == far);                   // the selector works as a handle too
    CHECK(heap.LockCount(moveable) == 2);
    CHECK(heap.Unlock(moveable) && !heap.Unlock(moveable));
    CHECK(!heap.Unlock(moveable));                  // not locked any more

    CHECK(heap.Free(moveable) == 0 && heap.Free(moveable) == moveable);  // second free fails
    CHECK(mem.Lookup(sel) == nullptr);              // the selector is gone
    CHECK(heap.Lock(0x1234) == 0);
    CHECK(heap.Count() == 1);
}

void TestFreedMemoryIsReused() {
    // A 1 MB arena holds ~15 64 KB blocks at once; allocating and freeing 500
    // of them only works if freed memory is reused.
    Memory mem(1 << 20);
    GlobalHeap heap(mem);
    for (int i = 0; i < 500; ++i) {
        const uint16_t h = heap.Alloc(gmem::Moveable, 0x10000);
        CHECK(h != 0);
        if (!h) return;
        const uint16_t sel = uint16_t(heap.Lock(h) >> 16);
        CHECK(mem.Read8(sel, 0xFFFF) == 0);  // reused memory is zero-filled again
        mem.Write8(sel, 0xFFFF, 0xAA);
        heap.Unlock(h);
        CHECK(heap.Free(h) == 0);
    }
    CHECK(mem.ArenaUsed() < 0x30000);
}

// --- GDI, painting, presentation ------------------------------------------------------------

void TestPaintProgramRendersAndReadsBack() {
    // The program checks its own pixels with GetPixel (exit code = failed check).
    HeadlessHost host;
    Runtime rt;
    rt.SetFrameCap(0);
    const TaskExit e = RunWithHost(PaintProgram(), host, rt);
    CHECK(e.kind == TaskExit::Kind::Exited && e.code == 0);

    // What reached the host: the same picture, presented as a whole frame.
    CHECK(host.presents >= 1);
    const HeadlessHost::Frame& f = host.lastFrame;
    CHECK(f.width == 64 && f.height == 48 && f.pixels.size() == 64u * 48u);
    if (f.pixels.size() != 64u * 48u) return;
    CHECK(f.At(5, 5) == 0xFF0000);     // red (BGRA in memory, 00RRGGBB as a dword)
    CHECK(f.At(48, 12) == 0x0000FF);   // blue
    CHECK(f.At(32, 12) == 0x000000);   // rectangle border
    CHECK(f.At(5, 30) == 0x00FF00);    // green
    CHECK(f.At(40, 30) == 0xFFFF00);   // yellow pixel
    CHECK(f.At(49, 33) == 0x000000 && f.At(53, 33) == 0xFFFFFF);  // BitBlt checkerboard
    CHECK(f.At(41, 41) == 0xFFFFFF && f.At(33, 41) == 0x000000);  // StretchBlt, 2x wide
    CHECK(f.At(60, 44) == 0x000000);   // background (BLACK_BRUSH)
}

void TestGdiHandleMapping() {
    Runtime rt;
    Gdi& g = rt.Graphics();
    const uint16_t dc = g.CreateCompatibleDc(0);
    const uint16_t bmp = g.CreateCompatibleBitmap(dc, 4, 4);
    CHECK(dc && bmp && g.HostDc(dc) && g.HostObject(bmp, Gdi::Kind::Bitmap));
    CHECK(!g.HostObject(bmp, Gdi::Kind::Brush));  // handles are typed

    // Selecting returns the DC's default bitmap, which the host created: it
    // gets a 16-bit handle of its own; selecting it back returns ours.
    const uint16_t original = g.Select(dc, bmp);
    CHECK(original != 0 && original != bmp);
    CHECK(g.HandleForHost(g.HostObject(original, Gdi::Kind::Bitmap)) == original);
    CHECK(!g.Delete(bmp));                       // still selected: refused, like Windows
    CHECK(g.Select(dc, original) == bmp);        // bidirectional
    CHECK(g.Delete(bmp));
    CHECK(!g.HostObject(bmp, Gdi::Kind::Bitmap));

    // Stock objects keep one handle, and deleting them is a harmless no-op.
    const uint16_t white = g.StockObject(0);
    CHECK(white && g.StockObject(0) == white && g.Delete(white) && g.StockObject(0) == white);
    CHECK(g.HostBrush(6) != nullptr);            // COLOR_WINDOW + 1 system brush
    CHECK(g.DeleteDc(dc) && !g.HostDc(dc));
}

void TestPaintLifecycle() {
    // WM_PAINT only while something is invalid; BeginPaint validates it.
    HeadlessHost host;
    Runtime rt;
    rt.SetFrameCap(0);
    std::string error;
    rt.SetWindowHost(&host);
    CHECK(rt.Load(BuildNe(WindowProgram(true)), "", error));
    rt.Run(1'000'000);  // blocks in GetMessage with its window alive (headless)
    User& u = rt.Windows();
    const uint16_t hwnd = u.HwndForHost(1);
    CHECK(hwnd && u.Find(hwnd)->update.Empty());  // the WM_PAINT was handled (by DefWindowProc)
    const Rect16 r{2, 3, 10, 12};
    u.Invalidate(hwnd, &r, true);
    const Rect16 r2{50, 50, 400, 400};            // clipped to the 320x200 client
    u.Invalidate(hwnd, &r2, false);
    const Rect16& upd = u.Find(hwnd)->update;
    CHECK(upd.left == 2 && upd.top == 3 && upd.right == 320 && upd.bottom == 200);
    Msg16 m;
    CHECK(u.Next(m, 0, 0, 0, true, false) == User::Fetch::Message && m.message == wm::Paint);
    CHECK(u.Next(m, 0, 0, 0, true, false) == User::Fetch::Message);  // still invalid: again
    u.Validate(hwnd, nullptr);
    CHECK(u.Next(m, 0, 0, 0, true, false) == User::Fetch::Empty);
}

double RunTimed(const NeProgram& program, uint32_t fps, HeadlessHost& host) {
    Runtime rt;
    rt.SetFrameCap(fps);
    const int64_t start = retro::QpcNow();
    const TaskExit e = RunWithHost(program, host, rt);
    const double seconds = double(retro::QpcNow() - start) / double(retro::QpcFrequency());
    CHECK(e.kind == TaskExit::Kind::Exited && e.code == 0);
    return seconds;
}

void TestAnimationIsPaced() {
    constexpr uint16_t kFrames = 20;
    HeadlessHost paced;
    const double t60 = RunTimed(AnimationProgram(kFrames), 60, paced);
    std::printf("  %u frames at 60 fps: %.1f ms, %u presents\n", kFrames, t60 * 1000, paced.presents);
    CHECK(paced.presents >= kFrames - 1);
    CHECK(t60 >= (kFrames - 2) / 60.0 * 0.99 && t60 < (kFrames + 1) / 60.0 * 1.5);

    HeadlessHost unpaced;
    const double t0 = RunTimed(AnimationProgram(kFrames), 0, unpaced);
    std::printf("  %u frames unpaced: %.1f ms, %u presents\n", kFrames, t0 * 1000, unpaced.presents);
    CHECK(unpaced.presents >= kFrames - 1 && t0 < 0.15);
}

// --- Resources, timers, text -----------------------------------------------------------------

void TestParseResources() {
    const NeProgram program = ResourceProgram();
    NeImage img;
    std::string error;
    CHECK(ParseNe(BuildNe(program), img, error));
    CHECK(img.resources.size() == 3);
    if (img.resources.size() != 3) return;
    const NeResource& bmp = img.resources[0];
    CHECK(bmp.typeId == res::Bitmap && bmp.id == 0 && bmp.name == "LOGO");
    CHECK(bmp.data.size() == 240);  // 232 bytes in 16-byte units
    CHECK(std::equal(program.resources[0].data.begin(), program.resources[0].data.end(), bmp.data.begin()));
    const NeResource& str = img.resources[1];
    CHECK(str.typeId == res::String && str.id == 1 && str.name.empty());
    const NeResource& custom = img.resources[2];
    CHECK(custom.typeId == 0 && custom.typeName == "LEVELS" && custom.id == 3);
    CHECK(custom.data.size() == 16 && custom.data[0] == 1 && custom.data[3] == 4);
    CHECK(DescribeResourceType(bmp) == "BITMAP" && DescribeResourceType(str) == "STRING" &&
          DescribeResourceType(custom) == "LEVELS");
    // HRSRCs are distinct NAMEINFO offsets.
    CHECK(bmp.tableOffset && bmp.tableOffset != str.tableOffset && str.tableOffset != custom.tableOffset);

    // No resources: an empty table.
    CHECK(ParseNe(BuildNe(SelfTestProgram()), img, error) && img.resources.empty());
    // A resource pointing past the end of the file is rejected.
    std::vector<uint8_t> file = BuildNe(program);
    const size_t ne = 0x40, table = ne + (file[ne + 0x24] | (file[ne + 0x25] << 8));
    const size_t firstOffset = table + 2 + 8;  // first NAMEINFO's offset field
    file[firstOffset] = 0xFF;
    file[firstOffset + 1] = 0x7F;
    CHECK(!ParseNe(file, img, error) && error.find("outside the file") != std::string::npos);
}

void TestResourceManager() {
    Runtime rt;
    std::string error;
    CHECK(rt.Load(BuildNe(ResourceProgram()), "", error));
    Resources& r = rt.Resource();
    const ResourceId bitmapType{res::Bitmap, {}};
    const uint16_t logo = r.Find(bitmapType, ResourceId{0, "LOGO"});
    CHECK(logo != 0 && r.Get(logo) && r.Get(logo)->name == "LOGO");
    CHECK(r.Find(ResourceId{0, "LEVELS"}, ResourceId{3, {}}) != 0);
    CHECK(r.Find(bitmapType, ResourceId{0, "NOPE"}) == 0);
    CHECK(r.Find(ResourceId{res::Icon, {}}, ResourceId{0, "LOGO"}) == 0);  // wrong type

    // "#6" means id 6; other strings are upper-cased names.
    Memory& mem = rt.Mem();
    const uint16_t sel = mem.Allocate(64, SegmentKind::Data);
    const char* texts[] = {"#6", "logo", "#x"};
    for (int i = 0; i < 3; ++i) {
        for (size_t j = 0; texts[i][j]; ++j) mem.Write8(sel, uint16_t(i * 16 + j), uint8_t(texts[i][j]));
    }
    const ResourceId six = ResourceId::FromFarPtr(mem, sel, 0);
    CHECK(six.id == 6 && six.name.empty());
    const ResourceId named = ResourceId::FromFarPtr(mem, sel, 16);
    CHECK(named.id == 0 && named.name == "LOGO");
    CHECK(ResourceId::FromFarPtr(mem, sel, 32).name == "#X");
    CHECK(ResourceId::FromFarPtr(mem, 0, 2).id == 2);  // MAKEINTRESOURCE

    // LoadResource shares one block while loaded; FreeResource counts down.
    const uint16_t h = r.Load(logo);
    CHECK(h != 0 && r.Load(logo) == h && r.LoadedCount() == 1);
    const uint16_t hsel = uint16_t(rt.Globals().Lock(h) >> 16);
    CHECK(hsel && mem.Read16(hsel, 0) == 40 && rt.Globals().Size(h) == 240);
    rt.Globals().Unlock(h);
    CHECK(r.Free(h) == 0 && r.LoadedCount() == 1);
    CHECK(r.Free(h) == 0 && r.LoadedCount() == 0 && rt.Globals().Size(h) == 0);
    CHECK(r.Free(h) == h);  // not loaded any more

    std::string s;
    CHECK(r.String(1, s) && s == kResourceText);
    CHECK(!r.String(0, s) && !r.String(2, s) && !r.String(17, s));  // empty / missing block
}

void TestBitmapsFromDibs() {
    Runtime rt;
    Gdi& g = rt.Graphics();
    const std::vector<uint8_t> dib = QuadrantDib();
    const uint16_t bmp = g.CreateBitmapFromDib(dib.data(), dib.size());
    CHECK(bmp != 0 && g.HostObject(bmp, Gdi::Kind::Bitmap));

    // It selects into a memory DC like any bitmap (its pixels are checked by
    // ResourceProgram, which blits it).
    const uint16_t dc = g.CreateCompatibleDc(0);
    const uint16_t old = g.Select(dc, bmp);
    CHECK(old != 0 && !g.Delete(bmp));  // selected: refused
    g.Select(dc, old);
    CHECK(g.Delete(bmp) && g.DeleteDc(dc));

    // Malformed or truncated DIBs are refused rather than over-read.
    CHECK(g.CreateBitmapFromDib(dib.data(), dib.size() - 1) == 0);
    CHECK(g.CreateBitmapFromDib(dib.data(), 20) == 0);
    std::vector<uint8_t> bad = dib;
    bad[14] = 7;  // 7 bits per pixel
    CHECK(g.CreateBitmapFromDib(bad.data(), bad.size()) == 0);
    bad = dib;
    bad[0] = 99;  // unknown header size
    CHECK(g.CreateBitmapFromDib(bad.data(), bad.size()) == 0);

    // A two-colour DIB (with its 2-entry colour table) loads too.
    std::vector<uint8_t> mono(40 + 8 + 8 * 4, 0);
    mono[0] = 40;
    mono[4] = 8;
    mono[8] = 8;
    mono[12] = 1;
    mono[14] = 1;
    mono[44] = mono[45] = mono[46] = 0xFF;  // entry 1: white
    const uint16_t monoBmp = g.CreateBitmapFromDib(mono.data(), mono.size());
    CHECK(monoBmp != 0 && g.Delete(monoBmp));
}

void TestTimerCadence() {
    Runtime rt;  // no program needed: drive USER directly
    User& u = rt.Windows();
    const uint16_t id = u.StartTimer(0, 0, 1, 0, 0);  // 1 ms asks for too much: 55 ms
    CHECK(id != 0 && u.TimerCount() == 1);
    Msg16 m;
    CHECK(u.Next(m, 0, 0, 0, true, false) == User::Fetch::Empty);  // not due yet

    // GetMessage-style waits: sleeps until each tick, never blocks for good.
    const int64_t start = retro::QpcNow();
    for (int i = 0; i < 4; ++i) {
        CHECK(u.Next(m, 0, 0, 0, true, true) == User::Fetch::Message);
        CHECK(m.message == wm::Timer && m.hwnd == 0 && m.wParam == id && m.lParam == 0);
    }
    const double ms = double(retro::QpcNow() - start) * 1000.0 / double(retro::QpcFrequency());
    std::printf("  4 ticks of a 55 ms timer: %.1f ms\n", ms);
    CHECK(ms >= 4 * 55 * 0.97 && ms < 4 * 55 + 40);

    // A filter no timer matches: nothing can ever arrive (headless).
    CHECK(u.Next(m, 0x2004, 0, 0, true, true) == User::Fetch::NoInput);
    CHECK(u.Next(m, 0, wm::Paint, wm::Paint, true, true) == User::Fetch::NoInput);

    // After a stall: one WM_TIMER, not a burst of the missed ones.
    retro::PreciseWaiter().WaitUntil(retro::QpcNow() + retro::QpcFrequency() / 4);
    CHECK(u.Next(m, 0, 0, 0, true, false) == User::Fetch::Message && m.message == wm::Timer);
    CHECK(u.Next(m, 0, 0, 0, true, false) == User::Fetch::Empty);

    // Each timer without a window gets its own id; KillTimer.
    const uint16_t id2 = u.StartTimer(0, 0, 55, 0, 0);
    CHECK(id2 != 0 && id2 != id);
    CHECK(u.StopTimer(0, id) && !u.StopTimer(0, id) && u.TimerCount() == 1);
    CHECK(u.Next(m, 0, 0, 0, true, true) == User::Fetch::Message && m.wParam == id2);
    CHECK(u.StopTimer(0, id2) && u.TimerCount() == 0);
    CHECK(u.Next(m, 0, 0, 0, true, true) == User::Fetch::NoInput);  // no timers left

    // A TIMERPROC must be code; timers of unknown windows are refused.
    CHECK(u.StartTimer(0, 0, 55, rt.Mem().Allocate(16, SegmentKind::Data), 0) == 0);
    CHECK(u.StartTimer(0x2004, 1, 55, 0, 0) == 0);
}

void TestResourceProgramRendersOnTimer() {
    // The program checks its own results (exit code = failed check); this
    // checks what reached the host and how long five 60 ms ticks took.
    HeadlessHost host;
    Runtime rt;
    rt.SetFrameCap(0);
    const int64_t start = retro::QpcNow();
    const TaskExit e = RunWithHost(ResourceProgram(), host, rt);
    const double ms = double(retro::QpcNow() - start) * 1000.0 / double(retro::QpcFrequency());
    std::printf("  5 ticks of a 60 ms timer: %.1f ms\n", ms);
    CHECK(e.kind == TaskExit::Kind::Exited && e.code == 0);
    CHECK(ms >= 5 * 60 * 0.97 && ms < 5 * 60 + 150);
    CHECK(rt.Windows().TimerCount() == 0 && rt.Resource().LoadedCount() == 0);

    CHECK(host.presents >= 5);  // a frame per tick
    const HeadlessHost::Frame& f = host.lastFrame;
    CHECK(f.width == 64 && f.height == 48);
    if (f.pixels.size() != 64u * 48u) return;
    for (int tick = 0; tick < 5; ++tick) {
        const int x = tick * 8;
        CHECK(f.At(x + 1, 1) == 0xFF0000 && f.At(x + 6, 1) == 0x00FF00);
        CHECK(f.At(x + 1, 6) == 0x0000FF && f.At(x + 6, 6) == 0xFFFFFF);
    }
    CHECK(f.At(41, 1) == 0x000000 && f.At(60, 40) == 0x000000);
    int yellow = 0, blue = 0, other = 0;
    for (int y = 16; y < 32; ++y) {
        for (int x = 0; x < 64; ++x) {
            const uint32_t c = f.At(x, y);
            if (c == 0xFFFF00) ++yellow;
            else if (c == 0x0000FF) ++blue;
            else ++other;
        }
    }
    std::printf("  text band: %d yellow, %d blue, %d other pixels\n", yellow, blue, other);
    CHECK(yellow > 20 && blue > yellow);
}

// --- Real-program support: catalog, trace, KERNEL services -----------------------------------

void TestCatalog() {
    const CatalogModule* user = FindCatalogModule("user");
    CHECK(user && FindCatalogModule("SHELL") && FindCatalogModule("MMSYSTEM") && !FindCatalogModule("NOPE"));
    if (!user) return;
    const CatalogEntry* cw = FindCatalogEntry(*user, 41);
    CHECK(cw && std::string(cw->name) == "CreateWindow" && cw->kind == CatalogKind::Pascal);
    CHECK(cw && ParamBytes(cw->params) == 30);  // matches the implementation's RETF 30
    const CatalogEntry* byName = FindCatalogEntry(*user, "getmessage");
    CHECK(byName && byName->ordinal == 108);
    const CatalogEntry* wsprintf = FindCatalogEntry(*user, 420);
    CHECK(wsprintf && wsprintf->kind == CatalogKind::Varargs);
    const CatalogModule* kernel = FindCatalogModule("KERNEL");
    const CatalogEntry* ahincr = kernel ? FindCatalogEntry(*kernel, 114) : nullptr;
    CHECK(ahincr && ahincr->kind == CatalogKind::Equate && std::string(ahincr->name) == "__AHINCR");
    const CatalogEntry* initTask = kernel ? FindCatalogEntry(*kernel, 91) : nullptr;
    CHECK(initTask && initTask->kind == CatalogKind::Register);
    // "Polygon (word ptr word)": Wine sometimes spaces the parameter list.
    const CatalogModule* gdi = FindCatalogModule("GDI");
    const CatalogEntry* polygon = gdi ? FindCatalogEntry(*gdi, 36) : nullptr;
    CHECK(polygon && polygon->params && std::string(polygon->params) == "wpw");

    // Every implemented function's ordinal names the same function in the catalog.
    for (const auto& [module, api] : {std::pair{"KERNEL", KernelApi()}, std::pair{"USER", UserApi()},
                                      std::pair{"GDI", GdiApi()}}) {
        const CatalogModule* m = FindCatalogModule(module);
        for (const ApiFunction& f : api) {
            const CatalogEntry* c = m ? FindCatalogEntry(*m, f.ordinal) : nullptr;
            std::string upper = c ? c->name : "";
            for (char& ch : upper) ch = char(std::toupper(static_cast<unsigned char>(ch)));
            if (upper != f.name) std::printf("  mismatch: %s.%u %s vs %s\n", module, f.ordinal, f.name, upper.c_str());
            CHECK(upper == f.name);
        }
    }
}

void TestTrace() {
    const RunOutcome o = RunProgram(HelloProgram(), 1'000'000, {true, false, {}});
    CHECK(o.exit.kind == TaskExit::Kind::Exited && o.exit.code == 0);
    for (const std::string& line : o.output) std::printf("  | %s\n", line.c_str());
    auto has = [&](const std::string& text) {
        return std::any_of(o.output.begin(), o.output.end(),
                           [&](const std::string& l) { return l.find(text) != std::string::npos; });
    };
    CHECK(has("[trace] ") && has(" KERNEL.91 InitTask(AX="));
    CHECK(has(" USER.1 MessageBox(0000, \"Hello from Win16\", \"Project Sun\", 0000) = 0001"));

    // Nested: DispatchMessage -> WndProc -> DefWindowProc -> DestroyWindow ...
    const RunOutcome w = RunProgram(WindowProgram(false), 1'000'000, {true, false, {}});
    CHECK(w.exit.kind == TaskExit::Kind::Exited && w.exit.code == 0);
    size_t dispatch = w.output.size(), nested = 0;
    for (size_t i = 0; i < w.output.size(); ++i) {
        if (dispatch == w.output.size() && w.output[i].find("USER.114 DispatchMessage(") != std::string::npos &&
            w.output[i].find(" = ") == std::string::npos)
            dispatch = i;  // printed before its callbacks' calls
        if (i > dispatch && StartsWith(w.output[i], "[trace]   ")) ++nested;  // indented: inside a callback
    }
    CHECK(dispatch < w.output.size() && nested > 0);
}

void TestMemoryResize() {
    Memory mem;
    const uint16_t a = mem.Allocate(32, SegmentKind::Data);
    const uint16_t b = mem.Allocate(32, SegmentKind::Data);  // a is no longer last
    mem.Write8(a, 31, 0xAB);
    const uint32_t before = mem.FreeBytes();
    CHECK(mem.Resize(a, 5000));  // moves
    CHECK(mem.SegmentSize(a) == 5000 && mem.Read8(a, 31) == 0xAB && mem.Read8(a, 4999) == 0);
    CHECK(mem.Resize(b, 64) && mem.SegmentSize(b) == 64);
    CHECK(mem.Resize(a, 16) && mem.SegmentSize(a) == 16);  // shrinks in place
    bool faulted = false;
    try {
        mem.Read8(a, 16);
    } catch (const ProtectionFault&) {
        faulted = true;
    }
    CHECK(faulted);
    CHECK(!mem.Resize(a, 0x10001) && !mem.Resize(0x1234, 16));
    CHECK(mem.FreeBytes() <= before);
}

void TestLocalHeap() {
    Memory mem;
    LocalHeaps heaps(mem);
    const uint16_t ds = mem.Allocate(0x200, SegmentKind::Data);
    CHECK(heaps.Init(ds, 0x100, 0x1FF));
    const uint16_t fixed = heaps.Alloc(ds, lmem::Fixed, 10);
    CHECK(fixed >= 0x100 && heaps.Lock(ds, fixed) == fixed && heaps.HandleFor(ds, fixed) == fixed);
    const uint16_t h = heaps.Alloc(ds, lmem::Moveable, 20);
    CHECK(h && h != fixed);
    const uint16_t p = heaps.Lock(ds, h);
    CHECK(p && mem.Read16(ds, h) == p);                  // *handle == pointer
    CHECK(heaps.Flags(ds, h) == 1 && mem.Read8(ds, uint16_t(h + 3)) == 1);  // lock count
    CHECK(heaps.HandleFor(ds, p) == h && heaps.Size(ds, h) == 20);
    CHECK(!heaps.Unlock(ds, h) && heaps.Flags(ds, h) == 0);
    mem.Write8(ds, p, 0x42);
    // Grow past the end: the segment grows (the heap ends where it does).
    const uint16_t grown = heaps.ReAlloc(ds, h, 2000, lmem::Moveable);
    CHECK(grown == h && heaps.Size(ds, h) == 2000 && mem.SegmentSize(ds) > 0x200);
    const uint16_t moved = heaps.Lock(ds, h);
    CHECK(mem.Read8(ds, moved) == 0x42 && mem.Read16(ds, h) == moved);
    // A fixed block can't move unless asked to.
    const uint16_t f2 = heaps.Alloc(ds, lmem::Fixed, 8);
    CHECK(heaps.ReAlloc(ds, fixed, 400, lmem::Fixed) == 0);
    CHECK(heaps.ReAlloc(ds, fixed, 4, lmem::Fixed) == fixed);  // shrinking stays put
    CHECK(heaps.Free(ds, fixed) == 0 && heaps.Free(ds, fixed) == fixed && heaps.Free(ds, f2) == 0);
    CHECK(heaps.Free(ds, h) == 0 && heaps.Count(ds) == 0);
    CHECK(heaps.Compact(ds) > 2000);
    CHECK(heaps.Alloc(0x1234, 0, 4) == 0);  // no heap there
}

std::filesystem::path MakeProgramDir(const std::string& name) {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / ("retro_win16_" + name);
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir / "DATA");
    for (const auto& [file, content] : CrtProgramFiles()) std::ofstream(dir / file, std::ios::binary) << content;
    std::ofstream(dir / "DATA" / "LEVEL1.DAT", std::ios::binary) << "L1";
    std::ofstream(dir.parent_path() / ("retro_win16_" + name + "_secret.txt")) << "secret";
    return dir;
}

void TestFileSystem() {
    const std::filesystem::path dir = MakeProgramDir("fs");
    FileSystem fs;
    fs.SetProgram(dir / "GAME.EXE");
    std::filesystem::path out;
    std::string why;
    CHECK(fs.Resolve("CRT.INI", out, why) && out == dir / "CRT.INI");
    CHECK(fs.Resolve("data\\level1.dat", out, why) && fs.Exists("DATA\\LEVEL1.DAT"));
    CHECK(fs.Resolve("C:\\WINDOWS\\CRT.INI", out, why) && out == dir / "CRT.INI");
    CHECK(fs.Resolve("c:\\windows\\system\\CRT.INI", out, why) && out == dir / "CRT.INI");
    CHECK(fs.Resolve((dir / "CRT.INI").string(), out, why));  // its own absolute path
    CHECK(!fs.Resolve("..\\retro_win16_fs_secret.txt", out, why));
    CHECK(!fs.Resolve("DATA\\..\\..\\retro_win16_fs_secret.txt", out, why));
    CHECK(!fs.Resolve("C:\\AUTOEXEC.BAT", out, why) && !fs.Resolve("\\\\server\\share\\x", out, why));
    CHECK(!fs.Exists("NOPE.TXT") && !fs.Exists("CON"));

    uint16_t error = 0;
    const int h = fs.Open("CRTDATA.BIN", 0, error);
    CHECK(h >= 5);
    uint8_t buf[16] = {};
    CHECK(fs.Read(h, buf, sizeof(buf)) == 8 && buf[0] == 'R' && buf[7] == '!');
    CHECK(fs.Read(h, buf, sizeof(buf)) == 0);  // end of file
    CHECK(fs.Seek(h, -2, 2) == 6 && fs.Read(h, buf, 1) == 1 && buf[0] == '6');
    CHECK(fs.Seek(h, -100, 1) == -1);
    CHECK(fs.Close(h) && !fs.Close(h) && fs.Read(h, buf, 1) == -1);
    CHECK(fs.Open("CRTDATA.BIN", 1, error) == -1 && error == dos::AccessDenied);  // writing
    CHECK(fs.Open("NOPE.BIN", 0, error) == -1 && error == dos::FileNotFound);

    Profiles ini(fs);
    std::string v;
    CHECK(ini.Get("CRT.INI", "game", "LEVEL", v) && v == "7");
    CHECK(ini.Get("CRT.INI", "Game", "Name", v) && v == "Sunny");
    CHECK(!ini.Get("CRT.INI", "Game", "Nope", v) && !ini.Get("", "windows", "load", v));
    CHECK(ini.Get("CRT.INI", "Game", "", v) && v == std::string("Level\0Name\0", 11));
    const std::string key = "Score", value = "99";
    ini.Write("crt.ini", "Game", &key, &value);
    CHECK(ini.Get("CRT.INI", "Game", "score", v) && v == "99");
    ini.Write("CRT.INI", "Game", &key, nullptr);  // delete the key
    CHECK(!ini.Get("CRT.INI", "Game", "Score", v));
    ini.Write("CRT.INI", "Game", nullptr, nullptr);  // delete the section
    CHECK(!ini.Get("CRT.INI", "Game", "Level", v));
    std::ifstream check(dir / "CRT.INI");
    std::string first;
    std::getline(check, first);
    CHECK(first.rfind("; test settings", 0) == 0);  // the file itself is untouched
}

void TestCrtProgram() {
    const std::filesystem::path dir = MakeProgramDir("crt");
    const RunOutcome o = RunProgram(CrtProgram(), 1'000'000, {false, false, dir / "CRT.EXE"});
    CHECK(o.loaded && o.exit.kind == TaskExit::Kind::Exited && o.exit.code == 0);  // else the failed check
    for (const std::string& line : o.output) std::printf("  | %s\n", line.c_str());
}

void TestIteratedSegment() {
    NeProgram p = SelfTestProgram();
    // DGROUP as iterated records: 4 x "AB", then 1 x "xyz".
    NeSeg& d = p.segments[1];
    d.iterated = true;
    d.bytes = {4, 0, 2, 0, 'A', 'B', 1, 0, 3, 0, 'x', 'y', 'z'};
    NeImage img;
    std::string error;
    CHECK(ParseNe(BuildNe(p), img, error));
    CHECK(std::string(img.segments[1].expanded.begin(), img.segments[1].expanded.end()) == "ABABABABxyz");
    Runtime rt;
    CHECK(rt.Load(BuildNe(p), "", error));
    CHECK(std::string(reinterpret_cast<const char*>(rt.Mem().SegmentData(rt.Module().dgroup)), 11) == "ABABABABxyz");
    d.bytes = {4, 0, 9, 0, 'A'};  // record longer than the data
    CHECK(!ParseNe(BuildNe(p), img, error) && error.find("iterated") != std::string::npos);
}

void TestExactTimers() {
    Runtime rt;
    User& u = rt.Windows();
    rt.SetExactTimers(true);
    CHECK(u.MinTimerMs() == 1);
    const uint16_t id = u.StartTimer(0, 0, 5, 0, 0);
    const int64_t start = retro::QpcNow();
    Msg16 m;
    for (int i = 0; i < 4; ++i) CHECK(u.Next(m, 0, 0, 0, true, true) == User::Fetch::Message && m.wParam == id);
    const double ms = double(retro::QpcNow() - start) * 1000.0 / double(retro::QpcFrequency());
    std::printf("  4 ticks of an exact 5 ms timer: %.1f ms\n", ms);
    CHECK(ms >= 4 * 5 * 0.9 && ms < 55);
    rt.SetExactTimers(false);
    CHECK(u.MinTimerMs() == 55);
}

// --- USER breadth, GDI, sound -------------------------------------------------------------

void TestUiProgram() {
    HeadlessHost host;
    Runtime rt;
    rt.SetFrameCap(0);
    const TaskExit e = RunWithHost(UiProgram(), host, rt);
    CHECK(e.kind == TaskExit::Kind::Exited && e.code == 0);  // else the failed check
    // MoveWindow to 640x480 made the host window fullscreen; SetWindowText retitled it.
    CHECK(host.windows.size() == 1);
    if (host.windows.empty()) return;
    const HeadlessHost::Record& w = host.windows[0];
    CHECK(w.info.fullscreen && w.info.width == 640 && w.info.height == 480 && w.info.title == "Renamed");
    CHECK(host.cursorShape == 32514 && host.captured == 0);  // IDC_WAIT; capture released
    CHECK(rt.SoundsPlayed() >= 1);                              // MessageBeep (muted)
    auto has = [](const std::string& text) {
        return std::any_of(g_output.begin(), g_output.end(),
                           [&](const std::string& l) { return l.find(text) != std::string::npos; });
    };
    CHECK(has("menu bar isn't drawn yet") && has("DialogBox(\"ABOUT\") answers IDCANCEL"));
    CHECK(has("SOUND calls (PC-speaker music) are ignored"));
    CHECK(rt.Windows().TimerCount() == 0);  // the multimedia timer killed itself
}

void TestMenuModel() {
    Menus m;
    const uint16_t bar = m.FromTemplate(GameMenuTemplate());
    CHECK(bar != 0);
    const std::vector<Menus::Item>* top = m.Items(bar);
    CHECK(top && top->size() == 2);
    if (!top || top->size() != 2) return;
    CHECK((*top)[0].text == "&Game" && (*top)[0].popup && (*top)[1].id == 200 && (*top)[1].text == "&Help");
    const std::vector<Menus::Item>* game = m.Items((*top)[0].popup);
    CHECK(game && game->size() == 2 && (*game)[0].id == 100 && (*game)[0].text == "&New\tF2");
    // By command (found inside the popup) or by position.
    CHECK(m.Find(bar, 101, mf::ByCommand) && m.Find(bar, 101, mf::ByCommand)->text == "E&xit");
    CHECK(m.Find(bar, 1, mf::ByPosition)->id == 200 && !m.Find(bar, 5, mf::ByPosition));
    CHECK(m.Check(bar, 101, mf::Checked) == 0 && m.Check(bar, 101, 0) == mf::Checked);
    CHECK(m.EnableItem(bar, 100, mf::Grayed) == 0 && m.State(bar, 100, 0) == mf::Grayed);
    CHECK(m.State(bar, 0, mf::ByPosition) == ((2 << 8) | mf::Popup));  // a popup: its item count
    CHECK(m.Check(bar, 999, mf::Checked) == -1);
    // Built by hand: append, insert before, remove.
    const uint16_t popup = m.Create(true);
    CHECK(m.Append(popup, 0, 1, "One") && m.Append(popup, 0, 3, "Three") && m.Insert(popup, 1, 0, 2, "Two"));
    CHECK(m.Items(popup)->size() == 3 && (*m.Items(popup))[1].text == "Two");
    CHECK(m.Remove(popup, 2, mf::ByCommand, false) && m.Items(popup)->size() == 2);
    const size_t before = m.Count();
    CHECK(m.Destroy(bar) && m.Count() == before - 2);  // with its popup
    // Truncated or wrong templates are refused.
    std::vector<uint8_t> t = GameMenuTemplate();
    t.resize(t.size() - 3);
    CHECK(m.FromTemplate(t) == 0 && m.FromTemplate({1, 0, 0, 0}) == 0);
    // Accelerators: 5-byte entries up to the one flagged 80h.
    const uint16_t acc = m.LoadAccelerators({0x01, 0x71, 0, 100, 0, 0x8D, 'N', 0, 101, 0, 0x01, 1, 0, 1, 0});
    CHECK(acc && m.Accelerators(acc)->size() == 2 && (*m.Accelerators(acc))[1].flags == 0x8D);
    CHECK(m.LoadAccelerators({}) == 0);
}

void TestInputState() {
    HeadlessHost host;
    Runtime rt;
    rt.SetFrameCap(0);
    std::string error;
    rt.SetWindowHost(&host);
    CHECK(rt.Load(BuildNe(WindowProgram(true)), "", error));
    rt.Run(1'000'000);  // blocks in GetMessage with its window (320x200) alive
    User& u = rt.Windows();
    const uint16_t hwnd = u.HwndForHost(1);
    CHECK(hwnd && u.Active() == hwnd && u.Focus() == hwnd);
    host.events.push_back({1, wm::KeyDown, 0x10, 0});                 // shift down
    host.events.push_back({1, wm::LButtonDown, 0x0001, 0x00060005});  // (5, 6), MK_LBUTTON
    Msg16 m;
    CHECK(u.Next(m, 0, 0, 0, true, true) == User::Fetch::Message);
    CHECK(u.KeyState(0x10) < 0 && (u.KeyState(0x10) & 1) && u.KeyState(0x11) == 0);
    CHECK(u.KeyState(1) < 0);  // VK_LBUTTON
    const Rect16 r = u.WindowRect(hwnd);
    CHECK(u.MouseX() == r.left + 5 && u.MouseY() == r.top + 6);
    host.events.push_back({1, wm::KeyUp, 0x10, 0});
    host.events.push_back({1, wm::LButtonUp, 0, 0x00060005});
    while (u.Next(m, 0, 0, 0, true, false) == User::Fetch::Message) {}
    u.Next(m, 0, 0, 0, true, true);
    CHECK(u.KeyState(0x10) >= 0 && (u.KeyState(0x10) & 1) && u.KeyState(1) >= 0);  // up; still toggled

    // Capture: the host is told; mouse input goes to the capturing window.
    CHECK(u.SetCaptureTo(hwnd) == 0 && host.captured == 1 && u.Captured() == hwnd);
    CHECK(u.SetCaptureTo(0) == hwnd && host.captured == 0);
    // Cursor: hidden while ShowCursor's count is negative, SetCursor(NULL) hides it.
    CHECK(u.ShowCursorCount(false) == -1 && host.cursorShape == 0);
    CHECK(u.ShowCursorCount(true) == 0 && host.cursorShape == kArrowCursor);
    const uint16_t cross = u.CursorHandle(32515);
    CHECK(cross && u.CursorHandle(32515) == cross);
    u.SetCursorHandle(cross);
    CHECK(host.cursorShape == 32515);
    u.SetCursorHandle(0);
    CHECK(host.cursorShape == 0);
    // Message boxes without a real host: the default button of the set.
    CHECK(u.ShowMessageBox(hwnd, "c", "t", 0x0000) == 1);          // MB_OK
    CHECK(u.ShowMessageBox(hwnd, "c", "t", 0x0101) == 2);          // MB_OKCANCEL, DEFBUTTON2
    CHECK(u.ShowMessageBox(hwnd, "c", "t", 0x0203) == 2);          // MB_YESNOCANCEL, DEFBUTTON3
    CHECK(u.ShowMessageBox(hwnd, "c", "t", 0x0002) == 3);          // MB_ABORTRETRYIGNORE
    CHECK(u.ShowMessageBox(hwnd, "c", "t", 0x0005) == 4);          // MB_RETRYCANCEL
    // Repositioning into fullscreen and back.
    CHECK(u.Reposition(hwnd, 0, 0, 640, 480, swp::NoZOrder) && host.windows[0].info.fullscreen);
    CHECK(u.Reposition(hwnd, 5, 5, 100, 80, swp::NoZOrder) && !host.windows[0].info.fullscreen);
    int w = 0, h = 0;
    CHECK(rt.Graphics().SurfacePixels(hwnd, w, h) && w == 100 && h == 80);
    CHECK(u.WindowRect(kDesktopHwnd).right == 640 && u.ClientRect(kDesktopHwnd).bottom == 480);
}

void TestGdiDefaults() {
    Runtime rt;
    Gdi& g = rt.Graphics();
    // Every DC starts with the engine's 96-DPI SYSTEM_FONT, a stock object.
    const uint16_t dc = g.CreateCompatibleDc(0);
    const uint16_t system = g.StockObject(13), ansiVar = g.StockObject(12);
    CHECK(system && ansiVar && system != ansiVar && g.StockObject(13) == system);
    CHECK(g.Select(dc, ansiVar) == system);
    CHECK(g.Delete(system) && g.StockObject(13) == system);  // stock: deleting is a no-op
    CHECK(g.DeleteDc(dc));
    // COLOR_xxx + 1 brushes use the Windows 3.1 colours.
    CHECK(ClassicSysColor(15) == 0xC0C0C0 && ClassicSysColor(2) == 0x800000 && ClassicSysColor(99) == 0);
    CHECK(g.CreateSurface(0x2100, 4, 4));
    const uint16_t wdc = g.GetWindowDc(0x2100);
    CHECK(g.Fill(wdc, {0, 0, 4, 4}, 16));  // COLOR_BTNFACE + 1
    g.ReleaseWindowDc(wdc);
    int w = 0, h = 0;
    const uint32_t* px = g.SurfacePixels(0x2100, w, h);
    CHECK(px && (px[0] & 0xFFFFFF) == 0xC0C0C0);
    // Resizing keeps the picture at the top left.
    CHECK(g.ResizeSurface(0x2100, 8, 2));
    px = g.SurfacePixels(0x2100, w, h);
    CHECK(px && w == 8 && h == 2 && (px[0] & 0xFFFFFF) == 0xC0C0C0 && (px[7] & 0xFFFFFF) == 0);
    g.DestroySurface(0x2100);
}

// --- The program's DLLs --------------------------------------------------------------------

std::filesystem::path MakeDllDir(const std::string& name) {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / ("retro_win16_" + name);
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    for (const auto& [file, bytes] : DllProgramFiles()) {
        std::ofstream out(dir / file, std::ios::binary);
        out.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
    }
    return dir;
}

void TestParseExports() {
    NeImage img;
    std::string error;
    CHECK(ParseNe(BuildNe(LibraryProgram("TESTDLL", 1, true)), img, error));
    CHECK(img.IsLibrary() && img.moduleName == "TESTDLL");
    CHECK(img.exportNames.size() == 4 && img.exportNames.at("ADDTWO") == 1 && img.exportNames.at("GETHEAP") == 5);
    CHECK(img.FindEntry(1) && img.FindEntry(1)->segment == 1 && img.FindEntry(1)->exported);
    CHECK(img.FindEntry(5) && !img.FindEntry(4));  // the gap
}

void TestDllProgram() {
    const std::filesystem::path dir = MakeDllDir("dlls");
    const RunOutcome o = RunProgram(DllProgram(), 1'000'000, {false, false, dir / "DLLAPP.EXE"});
    for (const std::string& line : o.output) std::printf("  | %s\n", line.c_str());
    CHECK(o.loaded && o.exit.kind == TaskExit::Kind::Exited && o.exit.code == 0);  // else the failed check
    auto has = [&](const std::string& text) {
        return std::any_of(o.output.begin(), o.output.end(),
                           [&](const std::string& l) { return l.find(text) != std::string::npos; });
    };
    CHECK(has("BADINIT failed to initialize") && has("MISSING.DLL is not in the program's directory"));
}

void TestDllLoadFailures() {
    const std::filesystem::path dir = MakeDllDir("dllfail");
    // A DLL the program imports fails to initialize: the task stops before the program runs.
    RunOutcome o = RunProgram(BadDllImportProgram("BADINIT", 2), 1'000'000, {false, false, dir / "APP.EXE"});
    CHECK(o.loaded && o.exit.kind == TaskExit::Kind::FatalExit);
    CHECK(o.exit.message == "BADINIT failed to initialize (its entry point returned 0)");
    // An import the DLL doesn't export: the program doesn't load.
    o = RunProgram(BadDllImportProgram("TESTDLL", 9), 1'000'000, {false, false, dir / "APP.EXE"});
    CHECK(!o.loaded && o.loadError.find("TESTDLL has no export 9") != std::string::npos);
    // A DLL that isn't there is still a stub module.
    o = RunProgram(BadDllImportProgram("NOTHERE", 1), 1'000'000, {false, false, dir / "APP.EXE"});
    CHECK(o.loaded && o.exit.kind == TaskExit::Kind::Unimplemented);
    // A program file where a DLL should be.
    {
        const std::vector<uint8_t> exe = BuildNe(HelloProgram());
        std::ofstream(dir / "NOTADLL.DLL", std::ios::binary).write(reinterpret_cast<const char*>(exe.data()),
                                                                   std::streamsize(exe.size()));
    }
    o = RunProgram(BadDllImportProgram("NOTADLL", 1), 1'000'000, {false, false, dir / "APP.EXE"});
    CHECK(!o.loaded && o.loadError.find("is a program, not a DLL") != std::string::npos);
}

void TestDllModules() {
    const std::filesystem::path dir = MakeDllDir("dllmods");
    Runtime rt;
    rt.SetProgram(dir / "DLLAPP.EXE");
    std::string error;
    CHECK(rt.Load(BuildNe(DllProgram()), "", error));
    // TESTDLL was loaded with the program; its entry point runs when the task starts.
    CHECK(rt.DllCount() == 1);
    Runtime::DllModule* dll = rt.FindDll("testdll.dll");
    CHECK(dll && !dll->initialized && dll->hInstance == dll->loaded.dgroup && dll->hModule);
    if (!dll) return;
    CHECK(rt.FindDll(dll->hInstance) == dll && rt.FindDll(dll->hModule) == dll);
    CHECK(rt.FindModuleHandle("TESTDLL") == dll->hModule);
    CHECK(rt.ModuleFileName(dll->hInstance).find("TESTDLL.DLL") != std::string::npos);
    // Resources by module: the DLL's string, not the program's.
    std::string s;
    CHECK(rt.ResourcesFor(dll->hInstance).String(1, s) && s == "From the DLL");
    CHECK(!rt.ResourcesFor(0).String(1, s));
    CHECK(rt.ProcAddress(dll->hModule, 0, "GETINIT") == ((uint32_t(dll->loaded.selectors[0]) << 16) |
                                                         dll->image.FindEntry(2)->offset));
    CHECK(rt.ProcAddress(dll->hInstance, 4, "") == 0);  // no ordinal 4
}

void TestKernelServices() {
    const RunOutcome o = RunProgram(KernelServicesProgram(), 1'000'000);
    CHECK(o.loaded && o.exit.kind == TaskExit::Kind::Exited && o.exit.code == 0);
}

void TestWindowServices() {
    const RunOutcome o = RunProgram(WindowServicesProgram(), 1'000'000);
    CHECK(o.loaded && o.exit.kind == TaskExit::Kind::Exited && o.exit.code == 0);
}

void TestSelectorAliases() {
    Memory mem(1 << 20);
    const uint16_t data = mem.Allocate(0x100, SegmentKind::Data);
    mem.Write8(data, 0x10, 0xAB);
    // A code alias shares the bytes, and can't write them.
    const uint16_t code = mem.Alias(data, SegmentKind::Code);
    CHECK(code && code != data && mem.Read8(code, 0x10, Access::Execute) == 0xAB);
    bool faulted = false;
    try {
        mem.Write8(code, 0x10, 1);
    } catch (const ProtectionFault&) {
        faulted = true;
    }
    CHECK(faulted);
    mem.Write8(data, 0x11, 0xCD);
    CHECK(mem.Read8(code, 0x11) == 0xCD);
    // Freeing an alias leaves the memory to its owner.
    mem.Free(code);
    CHECK(!mem.Lookup(code) && mem.Read8(data, 0x10) == 0xAB);
    // A bare descriptor pointed at the same bytes.
    const uint16_t bare = mem.AllocateDescriptor();
    CHECK(bare && mem.SetBase(bare, mem.Lookup(data)->base) && mem.SetLimit(bare, 0xFF));
    CHECK(mem.Read8(bare, 0x10) == 0xAB && !mem.Resize(bare, 0x200));
    CHECK(!mem.SetBase(bare, 0xFFFFFF00u));  // past the arena
    CHECK(!mem.SetBase(data, 0));             // an owner's base doesn't move
    // PrestoChangoSelector onto itself: the type flips, it still owns its memory.
    CHECK(mem.CopyDescriptor(data, data, SegmentKind::Code));
    CHECK(mem.Lookup(data)->kind == SegmentKind::Code && mem.Lookup(data)->owner);
    // No aliases of host segments.
    CHECK(mem.Alias(mem.Allocate(0x10000, SegmentKind::Host), SegmentKind::Data) == 0);
    // A fixed GDT selector, refreshed on every access, any RPL.
    int refreshed = 0;
    CHECK(mem.DefineFixed(0x40, 0x300, [&](uint8_t* p) { p[0x6C] = uint8_t(++refreshed); }));
    CHECK(mem.Read8(0x40, 0x6C) == 1 && mem.Read8(0x43, 0x6C) == 2);
    CHECK(!mem.DefineFixed(0x40, 0x10) && !mem.DefineFixed(0x47, 0x10));  // taken; not a GDT selector
    CHECK(!mem.Lookup(0x48) && !mem.Resize(0x40, 0x400));
}

void TestAtomTable() {
    AtomTable t;
    const uint16_t hello = t.Add("Hello");
    CHECK(hello >= 0xC000 && t.Add("HELLO") == hello && t.Find("hello") == hello);
    std::string name;
    CHECK(t.Name(hello, name) && name == "Hello");
    CHECK(t.Delete(hello) == 0 && t.Find("Hello") == hello);  // one reference left
    CHECK(t.Delete(hello) == 0 && t.Find("Hello") == 0 && t.Delete(hello) == hello);
    CHECK(t.Add("#123") == 123 && t.Find("#123") == 123 && t.Name(123, name) && name == "#123");
    CHECK(t.Delete(123) == 0);
    CHECK(t.Add("#49152") == 0 && t.Add("#0") == 0 && t.Add("") == 0 && t.Add("#12x") == 0);
    CHECK(t.Add("World") != hello && t.Add("World") >= 0xC000);
}

void TestBiosData() {
    Runtime rt;
    Memory& mem = rt.Mem();
    CHECK((mem.Read16(Runtime::kBiosDataSelector, 0x10) & 0x0002) != 0);  // a coprocessor
    CHECK(mem.Read16(Runtime::kBiosDataSelector, 0x13) == 640);
    auto ticks = [&] {
        return mem.Read16(Runtime::kBiosDataSelector, 0x6C) |
               (uint32_t(mem.Read16(Runtime::kBiosDataSelector, 0x6E)) << 16);
    };
    const uint32_t before = ticks();
    CHECK(before < 0x1800B0);  // ticks in a day
    // The counter runs at 18.2 Hz while the program polls it.
    const auto start = std::chrono::steady_clock::now();
    while (ticks() == before && std::chrono::steady_clock::now() - start < std::chrono::seconds(1)) {
    }
    CHECK(ticks() != before);
}

void TestBudget() {
    // An endless loop stops at the budget instead of hanging the host.
    NeProgram p = BaseProgram();
    NeSeg code;
    code.bytes = {0xEB, 0xFE};  // jmp $
    p.segments = {code, DataSegment({}, 0x100)};
    const RunOutcome o = RunProgram(p, 5000);
    CHECK(o.exit.kind == TaskExit::Kind::BudgetExhausted && o.exit.instructions == 5000);
}

}  // namespace

int main() {
    const test::Case cases[] = {
        {"ParseSelfTestImage", TestParseSelfTestImage},
        {"ParseByName", TestParseByName},
        {"ParseRejectsBadImages", TestParseRejectsBadImages},
        {"SelectorsAndProtection", TestSelectorsAndProtection},
        {"LoaderSetsUpTask", TestLoaderSetsUpTask},
        {"SelfTestProgramPasses", TestSelfTestProgramPasses},
        {"FatalExit", TestFatalExit},
        {"MessageBoxAndDosOutput", TestMessageBoxAndDosOutput},
        {"ImportsByName", TestImportsByName},
        {"UnimplementedApiStopsCleanly", TestUnimplementedApiStopsCleanly},
        {"FaultsAreReported", TestFaultsAreReported},
        {"UnbuiltModulesLoadAsStubs", TestUnbuiltModulesLoadAsStubs},
        {"WindowProgramRunsToQuit", TestWindowProgramRunsToQuit},
        {"HostCloseReachesWndProc", TestHostCloseReachesWndProc},
        {"HandleMappingWhileRunning", TestHandleMappingWhileRunning},
        {"FullscreenWindowIsFlagged", TestFullscreenWindowIsFlagged},
        {"FaultInsideCallbackIsReported", TestFaultInsideCallbackIsReported},
        {"GlobalHeap", TestGlobalHeap},
        {"FreedMemoryIsReused", TestFreedMemoryIsReused},
        {"PaintProgramRendersAndReadsBack", TestPaintProgramRendersAndReadsBack},
        {"GdiHandleMapping", TestGdiHandleMapping},
        {"PaintLifecycle", TestPaintLifecycle},
        {"AnimationIsPaced", TestAnimationIsPaced},
        {"ParseResources", TestParseResources},
        {"ResourceManager", TestResourceManager},
        {"BitmapsFromDibs", TestBitmapsFromDibs},
        {"TimerCadence", TestTimerCadence},
        {"ResourceProgramRendersOnTimer", TestResourceProgramRendersOnTimer},
        {"Catalog", TestCatalog},
        {"Trace", TestTrace},
        {"MemoryResize", TestMemoryResize},
        {"LocalHeap", TestLocalHeap},
        {"FileSystem", TestFileSystem},
        {"CrtProgram", TestCrtProgram},
        {"IteratedSegment", TestIteratedSegment},
        {"ExactTimers", TestExactTimers},
        {"UiProgram", TestUiProgram},
        {"MenuModel", TestMenuModel},
        {"InputState", TestInputState},
        {"GdiDefaults", TestGdiDefaults},
        {"ParseExports", TestParseExports},
        {"DllProgram", TestDllProgram},
        {"DllLoadFailures", TestDllLoadFailures},
        {"DllModules", TestDllModules},
        {"KernelServices", TestKernelServices},
        {"WindowServices", TestWindowServices},
        {"SelectorAliases", TestSelectorAliases},
        {"AtomTable", TestAtomTable},
        {"BiosData", TestBiosData},
        {"Budget", TestBudget},
    };
    return test::RunAll(cases);
}
