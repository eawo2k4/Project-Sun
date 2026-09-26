// Unit tests for the storage/memory clamping rules (retro/ClampPolicy.h).
//
// Most checks are phrased as "what would a 1990s game compute from this?",
// i.e. the value after 32-bit truncation or 32-bit multiplication.

#include <cstdint>
#include <vector>

#include "Check.h"
#include "retro/ClampPolicy.h"
#include "retro/ShimProtocol.h"

using namespace retro;

namespace {

constexpr uint64_t kGiB = 1024ull * kMiB;
constexpr uint64_t kTiB = 1024ull * kGiB;

// Stored in an `int`: must stay positive and unchanged.
bool FitsInt32(uint64_t v) {
    return v <= 0x7FFFFFFFull;
}

// Only the low DWORD kept (`int bytes = (int)li.LowPart`): must still read as
// a healthy positive amount unless the true value was already small.
bool LowDwordSafe(uint64_t v) {
    if (v <= kInt32SafeBytes) return true;
    const int32_t low = static_cast<int32_t>(static_cast<uint32_t>(v));
    return low >= 0x40000000;
}

// How a Win9x-era installer computes free space from GetDiskFreeSpace.
int32_t VintageDiskBytes(const DiskGeometry& g, uint32_t clusters) {
    return static_cast<int32_t>(g.sectorsPerCluster * g.bytesPerSector * clusters);
}

uint64_t RealDiskBytes(const DiskGeometry& g, uint32_t clusters) {
    return static_cast<uint64_t>(g.sectorsPerCluster) * g.bytesPerSector * clusters;
}

bool operator==(const DiskGeometry& a, const DiskGeometry& b) {
    return a.sectorsPerCluster == b.sectorsPerCluster && a.bytesPerSector == b.bytesPerSector &&
           a.freeClusters == b.freeClusters && a.totalClusters == b.totalClusters;
}

bool operator==(const DiskSpace& a, const DiskSpace& b) {
    return a.availableToCaller == b.availableToCaller && a.totalBytes == b.totalBytes &&
           a.totalFreeBytes == b.totalFreeBytes;
}

bool operator==(const MemoryFigures& a, const MemoryFigures& b) {
    return a.memoryLoad == b.memoryLoad && a.totalPhys == b.totalPhys &&
           a.availPhys == b.availPhys && a.totalPageFile == b.totalPageFile &&
           a.availPageFile == b.availPageFile && a.totalVirtual == b.totalVirtual &&
           a.availVirtual == b.availVirtual && a.availExtendedVirtual == b.availExtendedVirtual;
}

// Deterministic spread of values across the whole 64-bit range, plus the
// boundaries that matter.
std::vector<uint64_t> SampleValues() {
    std::vector<uint64_t> v = {0,
                               1,
                               kGiB - 1,
                               kGiB,
                               kInt32SafeBytes - 1,
                               kInt32SafeBytes,
                               kInt32SafeBytes + 1,
                               0x7FFFFFFFull,
                               0x80000000ull,
                               0xFFFFFFFFull,
                               0x100000000ull,
                               0x13FFFFFFFull,
                               0x140000000ull,
                               0x17FFF0000ull,
                               0x180000000ull,
                               8 * kGiB,
                               2 * kTiB,
                               ~0ull};
    uint64_t x = 0x9E3779B97F4A7C15ull;
    for (int i = 0; i < 20000; ++i) {
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        v.push_back(x >> (i % 40));  // bias towards small/medium magnitudes too
    }
    return v;
}

// --- TruncationSafeBytes ----------------------------------------------------

void TestTruncationSafeKnownValues() {
    CHECK(TruncationSafeBytes(0) == 0);
    CHECK(TruncationSafeBytes(12345) == 12345);
    CHECK(TruncationSafeBytes(kGiB) == kGiB);
    CHECK(TruncationSafeBytes(kInt32SafeBytes) == kInt32SafeBytes);

    // 2 GiB .. 4 GiB can't be both accurate and truncation-safe: report ~2 GiB.
    CHECK(TruncationSafeBytes(kInt32SafeBytes + 1) == kInt32SafeBytes);
    CHECK(TruncationSafeBytes(0x80000000ull) == kInt32SafeBytes);
    CHECK(TruncationSafeBytes(0xFFFFFFFFull) == kInt32SafeBytes);
    CHECK(TruncationSafeBytes(4 * kGiB) == kInt32SafeBytes);  // low DWORD would be 0

    CHECK(TruncationSafeBytes(5 * kGiB) == 5 * kGiB);  // low DWORD = 1 GiB: fine
    CHECK(TruncationSafeBytes(0x140001234ull) == 0x140000000ull);  // aligned to 64 KiB
    CHECK(TruncationSafeBytes(6 * kGiB) == 0x17FFF0000ull);
    CHECK(TruncationSafeBytes(8 * kGiB) == 0x17FFF0000ull);
    CHECK(TruncationSafeBytes(~0ull) == 0xFFFFFFFF7FFF0000ull);
}

void TestTruncationSafeProperties() {
    const std::vector<uint64_t> values = SampleValues();
    for (uint64_t v : values) {
        const uint64_t r = TruncationSafeBytes(v);
        CHECK(r <= v);
        CHECK(LowDwordSafe(r));
        CHECK(TruncationSafeBytes(r) == r);  // idempotent: safe to clamp twice
        CHECK(v - r < 3 * kGiB + 0x10000);   // never loses more than ~3 GiB
        if (v <= kInt32SafeBytes) CHECK(r == v);
    }

    // Monotonic: a bigger disk never reports less free space than a smaller one.
    uint64_t prev = 0;
    for (uint64_t v = 0; v < 12 * kGiB; v += 0x3FFF1) {
        const uint64_t r = TruncationSafeBytes(v);
        CHECK(r >= prev);
        prev = r;
    }
}

// --- GetDiskFreeSpace (cluster geometry) ------------------------------------

void TestDiskPolicyRange() {
    CHECK(MakeDiskPolicy(kDefaultDiskCapMiB).capBytes == 8 * kGiB);
    CHECK(MakeDiskPolicy(0).capBytes == kDiskCapMinMiB * kMiB);
    CHECK(MakeDiskPolicy(0xFFFFFFFFu).capBytes == kDiskCapMaxMiB * kMiB);
}

void TestDiskGeometryLargeNtfs() {
    // 2 TB NTFS volume, 4 KiB clusters, 1.5 TB free.
    const DiskGeometry in{8, 512, 393216000, 488281250};
    CHECK(VintageDiskBytes(in, in.totalClusters) <= 0);  // the bug: wraps negative

    const DiskGeometry out = ClampDiskGeometry(in, MakeDiskPolicy(kDefaultDiskCapMiB));
    CHECK(out.sectorsPerCluster == 8 && out.bytesPerSector == 512);  // geometry kept
    CHECK(out.totalClusters == kInt32SafeBytes / 4096);
    CHECK(out.freeClusters == out.totalClusters);

    // The same 32-bit arithmetic now gives the right, positive answer.
    CHECK(VintageDiskBytes(out, out.totalClusters) > 0);
    CHECK(static_cast<uint64_t>(VintageDiskBytes(out, out.totalClusters)) ==
          RealDiskBytes(out, out.totalClusters));
    CHECK(FitsInt32(RealDiskBytes(out, out.freeClusters)));
}

void TestDiskGeometryPartialFree() {
    // 2 TB disk with only 100 MiB free: the real free figure must survive.
    const DiskGeometry in{8, 512, 25600, 488281250};
    const DiskGeometry out = ClampDiskGeometry(in, MakeDiskPolicy(kDefaultDiskCapMiB));
    CHECK(out.freeClusters == 25600);
    CHECK(out.totalClusters == kInt32SafeBytes / 4096);
}

void TestDiskGeometrySmallDiskUntouched() {
    const DiskGeometry in{8, 512, 100000, 250000};  // ~1 GB volume
    CHECK(ClampDiskGeometry(in, MakeDiskPolicy(kDefaultDiskCapMiB)) == in);
}

void TestDiskGeometryLowCapAndOddClusters() {
    // User asked for a 512 MiB cap.
    const DiskGeometry in{8, 512, 393216000, 488281250};
    const DiskGeometry out = ClampDiskGeometry(in, MakeDiskPolicy(512));
    CHECK(RealDiskBytes(out, out.totalClusters) <= 512 * kMiB);

    // exFAT with 128 KiB clusters.
    const DiskGeometry exfat{256, 512, 30000000, 30500000};
    const DiskGeometry exOut = ClampDiskGeometry(exfat, MakeDiskPolicy(kDefaultDiskCapMiB));
    CHECK(VintageDiskBytes(exOut, exOut.totalClusters) > 0);
    CHECK(FitsInt32(RealDiskBytes(exOut, exOut.totalClusters)));

    // Degenerate inputs are passed through untouched rather than zeroed.
    const DiskGeometry zero{0, 512, 10, 20};
    CHECK(ClampDiskGeometry(zero, MakeDiskPolicy(kDefaultDiskCapMiB)) == zero);
    const DiskGeometry huge{0x10000, 0x10000, 10, 20};  // 4 GiB clusters
    CHECK(ClampDiskGeometry(huge, MakeDiskPolicy(kDefaultDiskCapMiB)) == huge);
}

void TestDiskGeometryIdempotent() {
    const DiskPolicy policy = MakeDiskPolicy(kDefaultDiskCapMiB);
    const DiskGeometry once = ClampDiskGeometry({8, 512, 393216000, 488281250}, policy);
    CHECK(ClampDiskGeometry(once, policy) == once);
}

// --- GetDiskFreeSpaceEx (64-bit byte counts) ---------------------------------

void CheckDiskSpaceInvariants(const DiskSpace& out, const DiskPolicy& policy) {
    CHECK(out.totalBytes <= policy.capBytes);
    CHECK(out.totalFreeBytes <= out.totalBytes);
    CHECK(out.availableToCaller <= out.totalFreeBytes);
    CHECK(LowDwordSafe(out.totalBytes));
    CHECK(LowDwordSafe(out.totalFreeBytes));
    CHECK(LowDwordSafe(out.availableToCaller));
}

void TestDiskSpaceLargeDisk() {
    const DiskPolicy policy = MakeDiskPolicy(kDefaultDiskCapMiB);
    const DiskSpace in{1536 * kGiB, 2 * kTiB, 1536 * kGiB};
    const DiskSpace out = ClampDiskSpace(in, policy);
    CheckDiskSpaceInvariants(out, policy);
    CHECK(out.totalBytes == 0x17FFF0000ull);  // ~6 GiB: 8 GiB cap made truncation-safe
    CHECK(out.totalFreeBytes == out.totalBytes);
    CHECK(out.availableToCaller == out.totalBytes);
}

void TestDiskSpaceSmallValuesUntouched() {
    const DiskPolicy policy = MakeDiskPolicy(kDefaultDiskCapMiB);
    const DiskSpace in{300 * kMiB, 1 * kGiB, 300 * kMiB};
    CHECK(ClampDiskSpace(in, policy) == in);
}

void TestDiskSpaceQuotaPreserved() {
    // Per-user quota: less available to the caller than is free on the volume.
    const DiskPolicy policy = MakeDiskPolicy(kDefaultDiskCapMiB);
    const DiskSpace in{100 * kMiB, 2 * kTiB, 1536 * kGiB};
    const DiskSpace out = ClampDiskSpace(in, policy);
    CheckDiskSpaceInvariants(out, policy);
    CHECK(out.availableToCaller == 100 * kMiB);
}

void TestDiskSpaceOrderingEnforced() {
    // Inconsistent input (more "available" than free) is brought into order.
    const DiskPolicy policy = MakeDiskPolicy(kDefaultDiskCapMiB);
    const DiskSpace out = ClampDiskSpace({900 * kMiB, 1 * kGiB, 500 * kMiB}, policy);
    CheckDiskSpaceInvariants(out, policy);
    CHECK(out.availableToCaller == 500 * kMiB);
}

void TestDiskSpaceSweep() {
    const std::vector<uint64_t> values = SampleValues();
    for (uint32_t capMiB : {kDiskCapMinMiB, 2048u, kDefaultDiskCapMiB, kDiskCapMaxMiB}) {
        const DiskPolicy policy = MakeDiskPolicy(capMiB);
        for (size_t i = 0; i + 2 < values.size(); i += 3) {
            const DiskSpace in{values[i], values[i + 1], values[i + 2]};
            const DiskSpace out = ClampDiskSpace(in, policy);
            CheckDiskSpaceInvariants(out, policy);
            CHECK(ClampDiskSpace(out, policy) == out);  // idempotent
        }
    }
}

// --- GlobalMemoryStatus(Ex) ----------------------------------------------------

MemoryFigures ModernMachine() {
    MemoryFigures f;
    f.memoryLoad = 37;
    f.totalPhys = 32 * kGiB;
    f.availPhys = 20 * kGiB;
    f.totalPageFile = 40 * kGiB;
    f.availPageFile = 25 * kGiB;
    f.totalVirtual = 4 * kGiB - 128 * 1024;  // large-address-aware process under WOW64
    f.availVirtual = 4 * kGiB - 200 * kMiB;
    f.availExtendedVirtual = 0;
    return f;
}

void CheckMemoryFitsLegacyStruct(const MemoryFigures& f) {
    // MEMORYSTATUS fields are 32-bit SIZE_T and games treat them as `int`.
    CHECK(FitsInt32(f.totalPhys) && FitsInt32(f.availPhys));
    CHECK(FitsInt32(f.totalPageFile) && FitsInt32(f.availPageFile));
    CHECK(FitsInt32(f.totalVirtual) && FitsInt32(f.availVirtual));
    CHECK(f.availPhys <= f.totalPhys);
    CHECK(f.availPageFile <= f.totalPageFile);
    CHECK(f.availVirtual <= f.totalVirtual);
}

void TestMemoryPolicyRange() {
    CHECK(MakeMemoryPolicy(kDefaultMemoryCapMiB).physCap == kGiB);
    CHECK(MakeMemoryPolicy(0).physCap == kMemoryCapMinMiB * kMiB);
    CHECK(MakeMemoryPolicy(99999).physCap == kMemoryCapMaxMiB * kMiB);
    for (uint32_t cap : {0u, 16u, 512u, 1024u, 2047u, 99999u}) {
        const MemoryPolicy p = MakeMemoryPolicy(cap);
        CHECK(FitsInt32(p.physCap) && FitsInt32(p.pageFileCap) && FitsInt32(p.virtualCap));
        CHECK(p.pageFileCap >= p.physCap);
    }
}

void TestMemoryModernMachine() {
    const MemoryFigures in = ModernMachine();
    const MemoryFigures out = ClampMemory(in, MakeMemoryPolicy(kDefaultMemoryCapMiB));
    CheckMemoryFitsLegacyStruct(out);

    CHECK(out.totalPhys == kGiB);
    CHECK(out.availPhys == 640 * kMiB);  // 20/32 of the cap
    CHECK(out.memoryLoad == in.memoryLoad);
    CHECK(out.totalPageFile == kInt32SafeBytes);
    const uint64_t expectedPage = kInt32SafeBytes / 8 * 5;  // 25/40 of the cap
    CHECK(out.availPageFile + 1 >= expectedPage && out.availPageFile <= expectedPage);
    CHECK(out.totalVirtual == kClassicUserSpaceBytes);
    CHECK(out.availExtendedVirtual == 0);

    // "Use a quarter of RAM for caches" in 32-bit math now works.
    const uint32_t legacyTotal = static_cast<uint32_t>(out.totalPhys);
    CHECK(static_cast<int32_t>(legacyTotal) / 4 == 256 * 1024 * 1024);
}

void TestMemorySmallMachineUntouched() {
    MemoryFigures in;
    in.memoryLoad = 60;
    in.totalPhys = 512 * kMiB;
    in.availPhys = 200 * kMiB;
    in.totalPageFile = 1024 * kMiB;
    in.availPageFile = 600 * kMiB;
    in.totalVirtual = kClassicUserSpaceBytes;
    in.availVirtual = 1800 * kMiB;
    CHECK(ClampMemory(in, MakeMemoryPolicy(kDefaultMemoryCapMiB)) == in);
}

void TestMemoryExtremes() {
    const MemoryPolicy policy = MakeMemoryPolicy(kDefaultMemoryCapMiB);

    // Petabyte-class figures must not overflow the proportional scaling.
    MemoryFigures in = ModernMachine();
    in.totalPhys = 1ull << 50;
    in.availPhys = 1ull << 49;
    MemoryFigures out = ClampMemory(in, policy);
    CheckMemoryFitsLegacyStruct(out);
    CHECK(out.availPhys == 512 * kMiB);

    // All free, and all used.
    in = ModernMachine();
    in.availPhys = in.totalPhys;
    CHECK(ClampMemory(in, policy).availPhys == kGiB);
    in.availPhys = 0;
    CHECK(ClampMemory(in, policy).availPhys == 0);

    // Idempotent: GlobalMemoryStatus sources from the Ex figures, and an app
    // might feed one result into another.
    out = ClampMemory(ModernMachine(), policy);
    CHECK(ClampMemory(out, policy) == out);
}

void TestMemoryMaxCap() {
    const MemoryFigures out = ClampMemory(ModernMachine(), MakeMemoryPolicy(kMemoryCapMaxMiB));
    CheckMemoryFitsLegacyStruct(out);
    CHECK(out.totalPhys == 2047 * kMiB);
    // The classic Windows 9x "total + pagefile" sum still fits in 32 bits.
    CHECK(out.totalPhys + out.totalPageFile <= 0xFFFFFFFFull);
}

}  // namespace

int main() {
    const test::Case cases[] = {
        {"TruncationSafeKnownValues", TestTruncationSafeKnownValues},
        {"TruncationSafeProperties", TestTruncationSafeProperties},
        {"DiskPolicyRange", TestDiskPolicyRange},
        {"DiskGeometryLargeNtfs", TestDiskGeometryLargeNtfs},
        {"DiskGeometryPartialFree", TestDiskGeometryPartialFree},
        {"DiskGeometrySmallDiskUntouched", TestDiskGeometrySmallDiskUntouched},
        {"DiskGeometryLowCapAndOddClusters", TestDiskGeometryLowCapAndOddClusters},
        {"DiskGeometryIdempotent", TestDiskGeometryIdempotent},
        {"DiskSpaceLargeDisk", TestDiskSpaceLargeDisk},
        {"DiskSpaceSmallValuesUntouched", TestDiskSpaceSmallValuesUntouched},
        {"DiskSpaceQuotaPreserved", TestDiskSpaceQuotaPreserved},
        {"DiskSpaceOrderingEnforced", TestDiskSpaceOrderingEnforced},
        {"DiskSpaceSweep", TestDiskSpaceSweep},
        {"MemoryPolicyRange", TestMemoryPolicyRange},
        {"MemoryModernMachine", TestMemoryModernMachine},
        {"MemorySmallMachineUntouched", TestMemorySmallMachineUntouched},
        {"MemoryExtremes", TestMemoryExtremes},
        {"MemoryMaxCap", TestMemoryMaxCap},
    };
    return test::RunAll(cases);
}
