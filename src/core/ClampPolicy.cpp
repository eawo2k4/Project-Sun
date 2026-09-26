#include "retro/ClampPolicy.h"

#include <algorithm>

namespace retro {
namespace {

constexpr uint64_t kLowDwordFloor = 0x40000000ull;  // 1 GiB
constexpr uint64_t kAlign64K = ~0xFFFFull;

// Reduces `total` to `cap` and scales `avail` by the same ratio.
void ClampPair(uint64_t& total, uint64_t& avail, uint64_t cap) {
    if (total <= cap) return;
    // avail * cap can exceed 64 bits on large machines; a double's 53-bit
    // mantissa is exact for anything below 8 PiB, far beyond any real value.
    const double scaled = static_cast<double>(avail) * static_cast<double>(cap) /
                          static_cast<double>(total);
    avail = std::min(static_cast<uint64_t>(scaled), cap);
    total = cap;
}

}  // namespace

uint64_t TruncationSafeBytes(uint64_t v) {
    if (v <= kInt32SafeBytes) return v;

    uint64_t hi = v >> 32;
    uint64_t lo = v & 0xFFFFFFFFull;
    if (lo < kLowDwordFloor) {
        // hi >= 1 here: with hi == 0, v > kInt32SafeBytes implies lo >= 1 GiB.
        --hi;
        lo = kInt32SafeBytes;
    } else {
        lo = std::min(lo, kInt32SafeBytes) & kAlign64K;
    }
    return (hi << 32) | lo;
}

DiskPolicy MakeDiskPolicy(uint32_t capMiB) {
    capMiB = std::clamp(capMiB, kDiskCapMinMiB, kDiskCapMaxMiB);
    return DiskPolicy{capMiB * kMiB};
}

DiskGeometry ClampDiskGeometry(const DiskGeometry& in, const DiskPolicy& policy) {
    const uint64_t clusterBytes = static_cast<uint64_t>(in.sectorsPerCluster) * in.bytesPerSector;
    if (clusterBytes == 0) return in;  // nonsense geometry: nothing sensible to do

    const uint64_t capBytes = std::min(policy.capBytes, kInt32SafeBytes);
    const uint64_t maxClusters = capBytes / clusterBytes;
    if (maxClusters == 0) return in;  // clusters larger than the cap (not a real volume)

    DiskGeometry out = in;
    out.totalClusters = static_cast<uint32_t>(std::min<uint64_t>(in.totalClusters, maxClusters));
    out.freeClusters = std::min(in.freeClusters, out.totalClusters);
    return out;
}

DiskSpace ClampDiskSpace(const DiskSpace& in, const DiskPolicy& policy) {
    auto clamp = [&](uint64_t v) { return TruncationSafeBytes(std::min(v, policy.capBytes)); };

    DiskSpace out;
    out.totalBytes = clamp(in.totalBytes);
    out.totalFreeBytes = std::min(clamp(in.totalFreeBytes), out.totalBytes);
    out.availableToCaller = std::min(clamp(in.availableToCaller), out.totalFreeBytes);
    return out;
}

MemoryPolicy MakeMemoryPolicy(uint32_t capMiB) {
    capMiB = std::clamp(capMiB, kMemoryCapMinMiB, kMemoryCapMaxMiB);
    MemoryPolicy p;
    p.physCap = capMiB * kMiB;
    // Commit limit is physical + page file, so keep it >= physical; 2x is what
    // a typical late-90s machine reported.
    p.pageFileCap = std::min(p.physCap * 2, kInt32SafeBytes);
    p.virtualCap = kClassicUserSpaceBytes;
    return p;
}

MemoryFigures ClampMemory(const MemoryFigures& in, const MemoryPolicy& policy) {
    MemoryFigures out = in;
    ClampPair(out.totalPhys, out.availPhys, policy.physCap);
    ClampPair(out.totalPageFile, out.availPageFile, policy.pageFileCap);
    ClampPair(out.totalVirtual, out.availVirtual, policy.virtualCap);
    return out;
}

}  // namespace retro
