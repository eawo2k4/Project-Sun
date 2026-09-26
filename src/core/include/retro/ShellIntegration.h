#pragma once

// "Run with RetroLaunch" in Explorer's context menu for .exe files: a shell
// verb under the exefile class,
//
//   <root>\Software\Classes\exefile\shell\RetroLaunch
//       (Default) = "Run with RetroLaunch"
//       Icon      = "<launcher>",0
//       command\(Default) = "<launcher>" --windowed "%1"
//
// <root> is HKEY_CURRENT_USER (just this user, no admin rights needed) or
// HKEY_LOCAL_MACHINE (all users, needs an elevated prompt). Either way the
// verb appears as HKEY_CLASSES_ROOT\exefile\shell\RetroLaunch, which merges
// both. Paths are quoted, so spaces in the launcher's or the program's path
// are fine.

#include <string>

namespace retro {

enum class ShellScope { CurrentUser, AllUsers };

inline constexpr wchar_t kShellVerbKey[] = L"Software\\Classes\\exefile\\shell\\RetroLaunch";
inline constexpr wchar_t kShellVerbLabel[] = L"Run with RetroLaunch";

// The verb's command line and icon location for a launcher path.
std::wstring ShellCommandFor(const std::wstring& launcher);  // "<launcher>" --windowed "%1"
std::wstring ShellIconFor(const std::wstring& launcher);     // "<launcher>",0

struct ShellVerb {
    std::wstring label, icon, command;
};

// Adds (or updates) the verb. `notifyShell` tells Explorer to refresh.
bool RegisterShellVerb(ShellScope scope, const std::wstring& launcher, std::wstring& error,
                       bool notifyShell = true);
// Removes the verb and its command key. Succeeds if it wasn't there
// (`existed` says which).
bool UnregisterShellVerb(ShellScope scope, bool& existed, std::wstring& error, bool notifyShell = true);
// Reads the registered verb, if any.
bool QueryShellVerb(ShellScope scope, ShellVerb& out);

}  // namespace retro
