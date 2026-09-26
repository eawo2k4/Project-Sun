#pragma once

// Pure clamping math behind the storage and memory shims. Nothing in here
// touches the OS, so the rules can be unit-tested exhaustively; the hooks in
// src/shim/hooks just marshal API structs in and out of these functions.
//
// The bugs being defended against, all from 1990s-era code:
//   * sectorsPerCluster * bytesPerSector * clusters computed in a 32-bit int
//     overflows (negative, or wraps to a tiny number) on disks >= 2 GiB.
//   * 64-bit results truncated to their low DWORD: 8 GiB becomes 0 bytes.
//   * dwTotalPhys / dwAvailPhys stored in an `int`: 2 GiB+ goes negative.
//   * "total + avail" or "total * 2" style arithmetic that wraps at 4 GiB.

#include <cstdint>

namespace retro {

inline constexpr uint64_t kMiB = 1024ull * 1024ull;

// Largest byte count that stays positive in a signed 32-bit int, rounded
// down to a 64 KiB boundary so it's also a whole number of clusters/pages.
inline constexpr uint64_t kInt32SafeBytes = 0x7FFF0000ull;

// What a non-large-address-aware 32-bit process sees as its address space.
inline constexpr uint64_t kClassicUserSpaceBytes = 0x7FFE0000ull;

// Accepted ranges for the caps (values outside are pulled into range).
inline constexpr uint32_t kDiskCapMinMiB = 64;
inline constexpr uint32_t kDiskCapMaxMiB = 16u * 1024u * 1024u;  // 16 TiB
inline constexpr uint32_t kMemoryCapMinMiB = 16;
inline constexpr uint32_t kMemoryCapMaxMiB = 2047;               // must stay < 2 GiB

// Returns the largest value <= v that survives truncation to 32 bits:
//   * values <= kInt32SafeBytes are returned unchanged;
//   * otherwise the low DWORD is forced into [1 GiB, kInt32SafeBytes], so
//     code that keeps only the low 32 bits (signed or unsigned) still sees
//     at least 1 GiB, while 64-bit-aware code sees roughly the real size.
// The function is monotonic and idempotent.
uint64_t TruncationSafeBytes(uint64_t v);

// --- Storage ------------------------------------------------------------------

struct DiskPolicy {
    uint64_t capBytes = 0;  // upper bound for any reported byte count
};
DiskPolicy MakeDiskPolicy(uint32_t capMiB);

// GetDiskFreeSpace(A/W) output.
struct DiskGeometry {
    uint32_t sectorsPerCluster = 0;
    uint32_t bytesPerSector = 0;
    uint32_t freeClusters = 0;
    uint32_t totalClusters = 0;
};

// Keeps the real cluster size and reduces the cluster counts so that
// sectorsPerCluster * bytesPerSector * clusters fits in a signed 32-bit int
// (and never exceeds the policy cap).
DiskGeometry ClampDiskGeometry(const DiskGeometry& in, const DiskPolicy& policy);

// GetDiskFreeSpaceEx(A/W) output.
struct DiskSpace {
    uint64_t availableToCaller = 0;
    uint64_t totalBytes = 0;
    uint64_t totalFreeBytes = 0;
};

// Caps every value at the policy cap, makes it truncation-safe, and keeps the
// ordering availableToCaller <= totalFreeBytes <= totalBytes.
DiskSpace ClampDiskSpace(const DiskSpace& in, const DiskPolicy& policy);

// --- Memory -------------------------------------------------------------------

struct MemoryPolicy {
    uint64_t physCap = 0;      // total physical memory reported
    uint64_t pageFileCap = 0;  // total commit limit reported
    uint64_t virtualCap = 0;   // total user address space reported
};
MemoryPolicy MakeMemoryPolicy(uint32_t capMiB);

// Superset of MEMORYSTATUS / MEMORYSTATUSEX.
struct MemoryFigures {
    uint32_t memoryLoad = 0;
    uint64_t totalPhys = 0;
    uint64_t availPhys = 0;
    uint64_t totalPageFile = 0;
    uint64_t availPageFile = 0;
    uint64_t totalVirtual = 0;
    uint64_t availVirtual = 0;
    uint64_t availExtendedVirtual = 0;
};

// Each total above its cap is reduced to the cap and the matching "avail"
// value is scaled by the same ratio, so memoryLoad stays truthful. Totals at
// or below their cap are left untouched.
MemoryFigures ClampMemory(const MemoryFigures& in, const MemoryPolicy& policy);

}  // namespace retro
