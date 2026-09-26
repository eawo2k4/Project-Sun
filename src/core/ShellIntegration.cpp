#include "retro/ShellIntegration.h"

#include <windows.h>
#include <shlobj.h>

namespace retro {
namespace {

HKEY RootFor(ShellScope scope) { return scope == ShellScope::AllUsers ? HKEY_LOCAL_MACHINE : HKEY_CURRENT_USER; }

std::wstring Describe(LSTATUS status, ShellScope scope) {
    if (status == ERROR_ACCESS_DENIED && scope == ShellScope::AllUsers)
        return L"access denied: registering for all users needs an elevated (administrator) prompt";
    wchar_t* text = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                   nullptr, DWORD(status), 0, reinterpret_cast<wchar_t*>(&text), 0, nullptr);
    std::wstring s = text ? text : L"error " + std::to_wstring(status);
    if (text) LocalFree(text);
    while (!s.empty() && (s.back() == L'\n' || s.back() == L'\r' || s.back() == L'.')) s.pop_back();
    return s;
}

LSTATUS SetString(HKEY key, const wchar_t* name, const std::wstring& value) {
    return RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
                          DWORD((value.size() + 1) * sizeof(wchar_t)));
}

bool GetString(HKEY key, const wchar_t* name, std::wstring& out) {
    DWORD type = 0, bytes = 0;
    if (RegQueryValueExW(key, name, nullptr, &type, nullptr, &bytes) != ERROR_SUCCESS || type != REG_SZ) return false;
    std::wstring s(bytes / sizeof(wchar_t) + 1, L'\0');
    if (RegQueryValueExW(key, name, nullptr, nullptr, reinterpret_cast<BYTE*>(s.data()), &bytes) != ERROR_SUCCESS)
        return false;
    s.resize(bytes / sizeof(wchar_t));
    while (!s.empty() && s.back() == L'\0') s.pop_back();
    out = s;
    return true;
}

void NotifyShell() { SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr); }

}  // namespace

std::wstring ShellCommandFor(const std::wstring& launcher) { return L"\"" + launcher + L"\" --windowed \"%1\""; }

std::wstring ShellIconFor(const std::wstring& launcher) { return L"\"" + launcher + L"\",0"; }

bool RegisterShellVerb(ShellScope scope, const std::wstring& launcher, std::wstring& error, bool notifyShell) {
    HKEY verb = nullptr, command = nullptr;
    LSTATUS s = RegCreateKeyExW(RootFor(scope), kShellVerbKey, 0, nullptr, 0, KEY_WRITE | KEY_WOW64_64KEY,
                                nullptr, &verb, nullptr);
    if (s == ERROR_SUCCESS) s = SetString(verb, nullptr, kShellVerbLabel);
    if (s == ERROR_SUCCESS) s = SetString(verb, L"Icon", ShellIconFor(launcher));
    if (s == ERROR_SUCCESS)
        s = RegCreateKeyExW(verb, L"command", 0, nullptr, 0, KEY_WRITE | KEY_WOW64_64KEY, nullptr, &command, nullptr);
    if (s == ERROR_SUCCESS) s = SetString(command, nullptr, ShellCommandFor(launcher));
    if (command) RegCloseKey(command);
    if (verb) RegCloseKey(verb);
    if (s != ERROR_SUCCESS) {
        error = Describe(s, scope);
        return false;
    }
    if (notifyShell) NotifyShell();
    return true;
}

bool UnregisterShellVerb(ShellScope scope, bool& existed, std::wstring& error, bool notifyShell) {
    existed = false;
    HKEY probe = nullptr;
    LSTATUS s = RegOpenKeyExW(RootFor(scope), kShellVerbKey, 0, KEY_READ | KEY_WOW64_64KEY, &probe);
    if (s == ERROR_FILE_NOT_FOUND) return true;  // nothing to remove
    if (s != ERROR_SUCCESS) {
        error = Describe(s, scope);
        return false;
    }
    RegCloseKey(probe);
    existed = true;
    s = RegDeleteTreeW(RootFor(scope), kShellVerbKey);  // the verb and its command subkey
    if (s != ERROR_SUCCESS) {
        error = Describe(s, scope);
        return false;
    }
    if (notifyShell) NotifyShell();
    return true;
}

bool QueryShellVerb(ShellScope scope, ShellVerb& out) {
    HKEY verb = nullptr;
    if (RegOpenKeyExW(RootFor(scope), kShellVerbKey, 0, KEY_READ | KEY_WOW64_64KEY, &verb) != ERROR_SUCCESS)
        return false;
    ShellVerb v;
    bool ok = GetString(verb, nullptr, v.label);
    GetString(verb, L"Icon", v.icon);
    HKEY command = nullptr;
    if (RegOpenKeyExW(verb, L"command", 0, KEY_READ | KEY_WOW64_64KEY, &command) == ERROR_SUCCESS) {
        ok = GetString(command, nullptr, v.command) && ok;
        RegCloseKey(command);
    } else {
        ok = false;
    }
    RegCloseKey(verb);
    if (ok) out = v;
    return ok;
}

}  // namespace retro
