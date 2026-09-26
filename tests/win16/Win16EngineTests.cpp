// Win16 engine tests: NE parsing, loading (selectors, relocations, initial
// registers) and running synthetic programs to completion.

#include <cstdio>
#include <string>

#include "../Check.h"
#include "retro/FramePacing.h"
#include "TestPrograms.h"
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

RunOutcome RunProgram(const NeProgram& program, uint64_t budget = 1'000'000) {
    RunOutcome o;
    Runtime rt;
    rt.SetOutput([&](const std::string& line) { o.output.push_back(line); });
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
    CHECK(r.r[SP] == 0x600);
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
    CHECK(o.exit.message == "KERNEL.FROBNICATE is not implemented yet");
}

void TestUnimplementedApiStopsCleanly() {
    const RunOutcome o = RunProgram(UnimplementedApiProgram());
    CHECK(o.exit.kind == TaskExit::Kind::Unimplemented);
    CHECK(o.exit.message == "USER.10 is not implemented yet");
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

void TestMissingModuleFailsToLoad() {
    const RunOutcome o = RunProgram(MissingModuleProgram());
    CHECK(!o.loaded);
    CHECK(o.loadError.find("SHELL") != std::string::npos);
}

// --- Windowing, messages, callbacks, global heap ----------------------------------------------

TaskExit RunWithHost(const NeProgram& program, HeadlessHost& host, Runtime& rt) {
    rt.SetWindowHost(&host);
    rt.SetOutput([](const std::string& line) { std::printf("  [win16] %s\n", line.c_str()); });
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
        {"MissingModuleFailsToLoad", TestMissingModuleFailsToLoad},
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
        {"Budget", TestBudget},
    };
    return test::RunAll(cases);
}
