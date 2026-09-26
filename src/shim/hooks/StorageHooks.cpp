// Storage clamping: GetDiskFreeSpace(A/W) and GetDiskFreeSpaceEx(A/W).
//
// Vintage installers commonly compute free space in 32-bit ints and refuse to
// install ("0 bytes free", "needs -1834 MB") on modern multi-terabyte disks.
// The math lives in retro/ClampPolicy.h; these hooks only marshal values.

#include <string>

#include "Hooks.h"
#include "Log.h"
#include "ShimState.h"
#include "retro/ClampPolicy.h"
#include "retro/PathUtil.h"

namespace retro::shim {
namespace {

decltype(&GetDiskFreeSpaceA) Real_GetDiskFreeSpaceA = GetDiskFreeSpaceA;
decltype(&GetDiskFreeSpaceW) Real_GetDiskFreeSpaceW = GetDiskFreeSpaceW;
decltype(&GetDiskFreeSpaceExA) Real_GetDiskFreeSpaceExA = GetDiskFreeSpaceExA;
decltype(&GetDiskFreeSpaceExW) Real_GetDiskFreeSpaceExW = GetDiskFreeSpaceExW;

DiskPolicy g_policy;

volatile LONG g_loggedA = 0;
volatile LONG g_loggedW = 0;
volatile LONG g_loggedExA = 0;
volatile LONG g_loggedExW = 0;

std::string RootName(LPCSTR root) { return root ? root : "(current)"; }
std::string RootName(LPCWSTR root) { return root ? ToUtf8(root) : "(current)"; }

uint64_t Bytes(const DiskGeometry& g, uint32_t clusters) {
    return static_cast<uint64_t>(g.sectorsPerCluster) * g.bytesPerSector * clusters;
}

template <class Char, class RealFn>
BOOL ClampedGetDiskFreeSpace(RealFn real, volatile LONG& logged, const char* api,
                             const Char* root, LPDWORD sectorsPerCluster,
                             LPDWORD bytesPerSector, LPDWORD freeClusters,
                             LPDWORD totalClusters) {
    // Every output is optional; query all of them so the clamp sees the full
    // geometry, then write back only what the caller asked for.
    DWORD spc = 0, bps = 0, freeC = 0, totalC = 0;
    if (!real(root, &spc, &bps, &freeC, &totalC)) return FALSE;

    const DiskGeometry in{spc, bps, freeC, totalC};
    const DiskGeometry out = ClampDiskGeometry(in, g_policy);

    if (FirstCall(logged)) {
        log::Write("%s(%s): free %llu / total %llu MiB -> %llu / %llu MiB", api,
                   RootName(root).c_str(), Bytes(in, in.freeClusters) / kMiB,
                   Bytes(in, in.totalClusters) / kMiB, Bytes(out, out.freeClusters) / kMiB,
                   Bytes(out, out.totalClusters) / kMiB);
    }

    if (sectorsPerCluster) *sectorsPerCluster = out.sectorsPerCluster;
    if (bytesPerSector) *bytesPerSector = out.bytesPerSector;
    if (freeClusters) *freeClusters = out.freeClusters;
    if (totalClusters) *totalClusters = out.totalClusters;
    return TRUE;
}

template <class Char, class RealFn>
BOOL ClampedGetDiskFreeSpaceEx(RealFn real, volatile LONG& logged, const char* api,
                               const Char* directory, PULARGE_INTEGER availableToCaller,
                               PULARGE_INTEGER totalBytes, PULARGE_INTEGER totalFreeBytes) {
    ULARGE_INTEGER avail{}, total{}, free{};
    if (!real(directory, &avail, &total, &free)) return FALSE;

    const DiskSpace in{avail.QuadPart, total.QuadPart, free.QuadPart};
    const DiskSpace out = ClampDiskSpace(in, g_policy);

    if (FirstCall(logged)) {
        log::Write("%s(%s): free %llu / total %llu MiB -> 0x%llX / 0x%llX bytes", api,
                   RootName(directory).c_str(), in.totalFreeBytes / kMiB, in.totalBytes / kMiB,
                   out.totalFreeBytes, out.totalBytes);
    }

    if (availableToCaller) availableToCaller->QuadPart = out.availableToCaller;
    if (totalBytes) totalBytes->QuadPart = out.totalBytes;
    if (totalFreeBytes) totalFreeBytes->QuadPart = out.totalFreeBytes;
    return TRUE;
}

BOOL WINAPI Hook_GetDiskFreeSpaceA(LPCSTR root, LPDWORD spc, LPDWORD bps, LPDWORD freeC,
                                   LPDWORD totalC) {
    return ClampedGetDiskFreeSpace(Real_GetDiskFreeSpaceA, g_loggedA, "GetDiskFreeSpaceA", root,
                                   spc, bps, freeC, totalC);
}

BOOL WINAPI Hook_GetDiskFreeSpaceW(LPCWSTR root, LPDWORD spc, LPDWORD bps, LPDWORD freeC,
                                   LPDWORD totalC) {
    return ClampedGetDiskFreeSpace(Real_GetDiskFreeSpaceW, g_loggedW, "GetDiskFreeSpaceW", root,
                                   spc, bps, freeC, totalC);
}

BOOL WINAPI Hook_GetDiskFreeSpaceExA(LPCSTR dir, PULARGE_INTEGER avail, PULARGE_INTEGER total,
                                     PULARGE_INTEGER free) {
    return ClampedGetDiskFreeSpaceEx(Real_GetDiskFreeSpaceExA, g_loggedExA,
                                     "GetDiskFreeSpaceExA", dir, avail, total, free);
}

BOOL WINAPI Hook_GetDiskFreeSpaceExW(LPCWSTR dir, PULARGE_INTEGER avail, PULARGE_INTEGER total,
                                     PULARGE_INTEGER free) {
    return ClampedGetDiskFreeSpaceEx(Real_GetDiskFreeSpaceExW, g_loggedExW,
                                     "GetDiskFreeSpaceExW", dir, avail, total, free);
}

}  // namespace

LONG AttachStorageHooks() {
    g_policy = MakeDiskPolicy(Config().diskCapMiB);
    log::Write("storage: cap %llu MiB", g_policy.capBytes / kMiB);

    LONG err = AttachHook(Real_GetDiskFreeSpaceA, Hook_GetDiskFreeSpaceA);
    if (err == NO_ERROR) err = AttachHook(Real_GetDiskFreeSpaceW, Hook_GetDiskFreeSpaceW);
    if (err == NO_ERROR) err = AttachHook(Real_GetDiskFreeSpaceExA, Hook_GetDiskFreeSpaceExA);
    if (err == NO_ERROR) err = AttachHook(Real_GetDiskFreeSpaceExW, Hook_GetDiskFreeSpaceExW);
    return err;
}

LONG DetachStorageHooks() {
    LONG err = DetachHook(Real_GetDiskFreeSpaceA, Hook_GetDiskFreeSpaceA);
    if (err == NO_ERROR) err = DetachHook(Real_GetDiskFreeSpaceW, Hook_GetDiskFreeSpaceW);
    if (err == NO_ERROR) err = DetachHook(Real_GetDiskFreeSpaceExA, Hook_GetDiskFreeSpaceExA);
    if (err == NO_ERROR) err = DetachHook(Real_GetDiskFreeSpaceExW, Hook_GetDiskFreeSpaceExW);
    return err;
}

}  // namespace retro::shim
