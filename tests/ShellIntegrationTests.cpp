// Explorer integration: the registry keys --register-shell writes and
// --unregister-shell removes. HKEY_CURRENT_USER is redirected into a
// throwaway key (RegOverridePredefKey), so the real context menu is never touched.

#include <windows.h>

#include <string>

#include "Check.h"
#include "retro/ShellIntegration.h"

namespace {

const std::wstring kLauncher = L"C:\\Program Files\\Retro Runner\\RetroLaunch.exe";  // spaces on purpose

void TestCommandAndIconQuoting() {
    CHECK(retro::ShellCommandFor(kLauncher) ==
          L"\"C:\\Program Files\\Retro Runner\\RetroLaunch.exe\" --windowed \"%1\"");
    CHECK(retro::ShellIconFor(kLauncher) == L"\"C:\\Program Files\\Retro Runner\\RetroLaunch.exe\",0");
    CHECK(std::wstring(retro::kShellVerbKey) == L"Software\\Classes\\exefile\\shell\\RetroLaunch");
}

// Runs `body` with HKEY_CURRENT_USER pointing at a fresh, empty key.
template <typename F>
void WithScratchCurrentUser(F body) {
    const std::wstring scratch = L"Software\\RetroRunnerTests\\Shell" + std::to_wstring(GetCurrentProcessId());
    HKEY key = nullptr;
    CHECK(RegCreateKeyExW(HKEY_CURRENT_USER, scratch.c_str(), 0, nullptr, 0, KEY_ALL_ACCESS, nullptr, &key,
                          nullptr) == ERROR_SUCCESS);
    if (!key) return;
    CHECK(RegOverridePredefKey(HKEY_CURRENT_USER, key) == ERROR_SUCCESS);
    body(key);
    RegOverridePredefKey(HKEY_CURRENT_USER, nullptr);
    RegCloseKey(key);
    RegDeleteTreeW(HKEY_CURRENT_USER, scratch.c_str());
    RegDeleteKeyW(HKEY_CURRENT_USER, L"Software\\RetroRunnerTests");  // only if now empty
}

void TestRegisterQueryUnregister() {
    WithScratchCurrentUser([](HKEY scratch) {
        retro::ShellVerb verb;
        CHECK(!retro::QueryShellVerb(retro::ShellScope::CurrentUser, verb));

        std::wstring error;
        CHECK(retro::RegisterShellVerb(retro::ShellScope::CurrentUser, kLauncher, error, false));
        CHECK(error.empty());
        CHECK(retro::QueryShellVerb(retro::ShellScope::CurrentUser, verb));
        CHECK(verb.label == L"Run with RetroLaunch");
        CHECK(verb.icon == retro::ShellIconFor(kLauncher));
        CHECK(verb.command == retro::ShellCommandFor(kLauncher));

        // The keys really are under the (redirected) user's classes.
        HKEY command = nullptr;
        CHECK(RegOpenKeyExW(scratch, L"Software\\Classes\\exefile\\shell\\RetroLaunch\\command", 0, KEY_READ,
                            &command) == ERROR_SUCCESS);
        if (command) RegCloseKey(command);

        // Registering again (e.g. after moving RetroLaunch) just updates the entry.
        const std::wstring moved = L"D:\\Tools\\RetroLaunch.exe";
        CHECK(retro::RegisterShellVerb(retro::ShellScope::CurrentUser, moved, error, false));
        CHECK(retro::QueryShellVerb(retro::ShellScope::CurrentUser, verb) &&
              verb.command == retro::ShellCommandFor(moved));

        bool existed = false;
        CHECK(retro::UnregisterShellVerb(retro::ShellScope::CurrentUser, existed, error, false) && existed);
        CHECK(!retro::QueryShellVerb(retro::ShellScope::CurrentUser, verb));
        HKEY gone = nullptr;
        CHECK(RegOpenKeyExW(scratch, L"Software\\Classes\\exefile\\shell\\RetroLaunch", 0, KEY_READ, &gone) ==
              ERROR_FILE_NOT_FOUND);
        // The parent keys other programs share are left alone.
        HKEY shell = nullptr;
        CHECK(RegOpenKeyExW(scratch, L"Software\\Classes\\exefile\\shell", 0, KEY_READ, &shell) == ERROR_SUCCESS);
        if (shell) RegCloseKey(shell);

        // Unregistering when nothing is there succeeds and says so.
        CHECK(retro::UnregisterShellVerb(retro::ShellScope::CurrentUser, existed, error, false) && !existed);
    });
}

}  // namespace

int main() {
    const test::Case cases[] = {
        {"CommandAndIconQuoting", TestCommandAndIconQuoting},
        {"RegisterQueryUnregister", TestRegisterQueryUnregister},
    };
    return test::RunAll(cases);
}
