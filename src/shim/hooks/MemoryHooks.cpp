// Memory clamping: GlobalMemoryStatus and GlobalMemoryStatusEx.
//
// Games that store dwTotalPhys/dwAvailPhys in an `int` see negative memory at
// 2 GiB+, and many size caches as "a fraction of RAM" with 32-bit math. The
// real GlobalMemoryStatus already saturates on big machines, but at values
// (0x7FFFFFFF / 0xFFFFFFFF) that still break that arithmetic.

#include "Hooks.h"
#include "Log.h"
#include "ShimState.h"
#include "retro/ClampPolicy.h"

namespace retro::shim {
namespace {

decltype(&GlobalMemoryStatus) Real_GlobalMemoryStatus = GlobalMemoryStatus;
decltype(&GlobalMemoryStatusEx) Real_GlobalMemoryStatusEx = GlobalMemoryStatusEx;

MemoryPolicy g_policy;

volatile LONG g_logged = 0;
volatile LONG g_loggedEx = 0;

MemoryFigures FromEx(const MEMORYSTATUSEX& ms) {
    MemoryFigures f;
    f.memoryLoad = ms.dwMemoryLoad;
    f.totalPhys = ms.ullTotalPhys;
    f.availPhys = ms.ullAvailPhys;
    f.totalPageFile = ms.ullTotalPageFile;
    f.availPageFile = ms.ullAvailPageFile;
    f.totalVirtual = ms.ullTotalVirtual;
    f.availVirtual = ms.ullAvailVirtual;
    f.availExtendedVirtual = ms.ullAvailExtendedVirtual;
    return f;
}

void LogClamp(volatile LONG& logged, const char* api, const MemoryFigures& in,
              const MemoryFigures& out) {
    if (!FirstCall(logged)) return;
    log::Write("%s: phys %llu/%llu MiB -> %llu/%llu, pagefile %llu -> %llu, virtual %llu -> %llu "
               "(avail/total MiB)",
               api, in.availPhys / kMiB, in.totalPhys / kMiB, out.availPhys / kMiB,
               out.totalPhys / kMiB, in.totalPageFile / kMiB, out.totalPageFile / kMiB,
               in.totalVirtual / kMiB, out.totalVirtual / kMiB);
}

VOID WINAPI Hook_GlobalMemoryStatus(LPMEMORYSTATUS status) {
    // Source the figures from the 64-bit API: the legacy one has already
    // saturated its fields on large machines, which loses the avail/total
    // ratio needed for proportional scaling.
    MEMORYSTATUSEX ex{};
    ex.dwLength = sizeof(ex);
    if (!status || !Real_GlobalMemoryStatusEx(&ex)) {
        Real_GlobalMemoryStatus(status);
        return;
    }

    const MemoryFigures in = FromEx(ex);
    const MemoryFigures out = ClampMemory(in, g_policy);
    LogClamp(g_logged, "GlobalMemoryStatus", in, out);

    // Every clamped value is < 2 GiB, so it fits a 32-bit SIZE_T. Values that
    // were already under their cap are also < 2 GiB because every cap is.
    status->dwLength = sizeof(MEMORYSTATUS);
    status->dwMemoryLoad = out.memoryLoad;
    status->dwTotalPhys = static_cast<SIZE_T>(out.totalPhys);
    status->dwAvailPhys = static_cast<SIZE_T>(out.availPhys);
    status->dwTotalPageFile = static_cast<SIZE_T>(out.totalPageFile);
    status->dwAvailPageFile = static_cast<SIZE_T>(out.availPageFile);
    status->dwTotalVirtual = static_cast<SIZE_T>(out.totalVirtual);
    status->dwAvailVirtual = static_cast<SIZE_T>(out.availVirtual);
}

BOOL WINAPI Hook_GlobalMemoryStatusEx(LPMEMORYSTATUSEX status) {
    if (!Real_GlobalMemoryStatusEx(status)) return FALSE;

    const MemoryFigures in = FromEx(*status);
    const MemoryFigures out = ClampMemory(in, g_policy);
    LogClamp(g_loggedEx, "GlobalMemoryStatusEx", in, out);

    status->dwMemoryLoad = out.memoryLoad;
    status->ullTotalPhys = out.totalPhys;
    status->ullAvailPhys = out.availPhys;
    status->ullTotalPageFile = out.totalPageFile;
    status->ullAvailPageFile = out.availPageFile;
    status->ullTotalVirtual = out.totalVirtual;
    status->ullAvailVirtual = out.availVirtual;
    status->ullAvailExtendedVirtual = out.availExtendedVirtual;
    return TRUE;
}

}  // namespace

LONG AttachMemoryHooks() {
    g_policy = MakeMemoryPolicy(Config().memoryCapMiB);
    log::Write("memory: phys cap %llu MiB, pagefile cap %llu MiB, virtual cap %llu MiB",
               g_policy.physCap / kMiB, g_policy.pageFileCap / kMiB, g_policy.virtualCap / kMiB);

    LONG err = AttachHook(Real_GlobalMemoryStatus, Hook_GlobalMemoryStatus);
    if (err == NO_ERROR) err = AttachHook(Real_GlobalMemoryStatusEx, Hook_GlobalMemoryStatusEx);
    return err;
}

LONG DetachMemoryHooks() {
    LONG err = DetachHook(Real_GlobalMemoryStatus, Hook_GlobalMemoryStatus);
    if (err == NO_ERROR) err = DetachHook(Real_GlobalMemoryStatusEx, Hook_GlobalMemoryStatusEx);
    return err;
}

}  // namespace retro::shim
