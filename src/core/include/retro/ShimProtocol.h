#pragma once

// Contract between RetroLaunch.exe and RetroShim.dll.
//
// The launcher copies a ShimConfig into the suspended target process with
// DetourCopyPayloadToProcess(); the shim retrieves it during DLL_PROCESS_ATTACH
// with DetourFindPayloadEx(). Bump kShimConfigVersion whenever the layout
// changes, and only ever append fields.

#include <windows.h>

#include <cstdint>
#include <type_traits>

namespace retro {

// {7F3C2A61-4E0B-4C5D-9A1E-52D8B3F6C701}
inline constexpr GUID kShimConfigGuid = {
    0x7f3c2a61, 0x4e0b, 0x4c5d, {0x9a, 0x1e, 0x52, 0xd8, 0xb3, 0xf6, 0xc7, 0x01}};

inline constexpr uint32_t kShimConfigVersion = 3;

inline constexpr uint32_t kDefaultDiskCapMiB = 8192;
inline constexpr uint32_t kDefaultMemoryCapMiB = 1024;
inline constexpr uint32_t kDefaultFpsCap = 60;
inline constexpr uint32_t kMaxFpsCap = 1000;

// Feature toggles for the hook modules.
enum ShimFeature : uint32_t {
    ShimFeature_ClampStorage   = 1u << 0,  // GetDiskFreeSpace(Ex)A/W overflow clamping
    ShimFeature_ClampMemory    = 1u << 1,  // GlobalMemoryStatus(Ex) overflow clamping
    ShimFeature_FrameLimiter   = 1u << 2,  // presentation-level pacing (fpsCap)
    ShimFeature_DisplaySandbox = 1u << 3,  // virtual display modes + sandboxed windows
    ShimFeature_ChildProcesses = 1u << 4,  // inject into 32-bit child processes
    ShimFeature_Default        = ShimFeature_ClampStorage | ShimFeature_ClampMemory |
                                 ShimFeature_FrameLimiter | ShimFeature_DisplaySandbox |
                                 ShimFeature_ChildProcesses,
};

// Presentation options for the display sandbox (ShimConfig::displayFlags).
enum DisplayFlag : uint32_t {
    DisplayFlag_Windowed         = 1u << 0,  // captioned window instead of borderless fullscreen
    DisplayFlag_NoIntegerScaling = 1u << 1,  // fill the screen with fractional scaling
};

struct ShimConfig {
    // --- v1 ---
    uint32_t cbSize = sizeof(ShimConfig);
    uint32_t version = kShimConfigVersion;
    uint32_t features = ShimFeature_Default;
    uint32_t fpsCap = kDefaultFpsCap;  // 0 = unlimited
    wchar_t  logPath[MAX_PATH] = {};   // empty = OutputDebugString only
    // --- v2 ---
    uint32_t diskCapMiB = kDefaultDiskCapMiB;      // see retro::MakeDiskPolicy
    uint32_t memoryCapMiB = kDefaultMemoryCapMiB;  // see retro::MakeMemoryPolicy
    // --- v3 ---
    uint32_t displayFlags = 0;  // DisplayFlag_*
};

static_assert(std::is_trivially_copyable_v<ShimConfig>,
              "ShimConfig is memcpy'd across process boundaries");

}  // namespace retro
