#include "retro/PathUtil.h"

#include <windows.h>

namespace retro {

bool ToAnsiPath(const std::wstring& path, std::string& out) {
    const bool utf8Acp = GetACP() == CP_UTF8;  // "Beta: use UTF-8" system setting
    auto convert = [&](const wchar_t* p) {
        // WC_NO_BEST_FIT_CHARS and the used-default flag are invalid for CP_UTF8.
        BOOL usedDefault = FALSE;
        const DWORD flags = utf8Acp ? 0 : WC_NO_BEST_FIT_CHARS;
        BOOL* pUsedDefault = utf8Acp ? nullptr : &usedDefault;
        int n = WideCharToMultiByte(CP_ACP, flags, p, -1, nullptr, 0, nullptr, pUsedDefault);
        if (n <= 0 || usedDefault) return false;
        out.resize(static_cast<size_t>(n));
        WideCharToMultiByte(CP_ACP, flags, p, -1, out.data(), n, nullptr, pUsedDefault);
        out.resize(static_cast<size_t>(n) - 1);
        return !usedDefault;
    };

    if (convert(path.c_str())) return true;

    wchar_t shortPath[MAX_PATH];
    DWORD n = GetShortPathNameW(path.c_str(), shortPath, MAX_PATH);
    return n > 0 && n < MAX_PATH && convert(shortPath);
}

std::string ToUtf8(const std::wstring& s) {
    if (s.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0,
                                nullptr, nullptr);
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n, nullptr,
                        nullptr);
    return out;
}

}  // namespace retro
