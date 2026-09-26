#include "Log.h"

#include <windows.h>

#include <cstdarg>
#include <cstdio>

namespace retro::log {
namespace {

HANDLE g_file = INVALID_HANDLE_VALUE;
SRWLOCK g_lock = SRWLOCK_INIT;

}  // namespace

void Open(const wchar_t* path) {
    if (!path || !*path) return;
    AcquireSRWLockExclusive(&g_lock);
    if (g_file == INVALID_HANDLE_VALUE) {
        g_file = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                             nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    }
    ReleaseSRWLockExclusive(&g_lock);
}

void Close() {
    AcquireSRWLockExclusive(&g_lock);
    if (g_file != INVALID_HANDLE_VALUE) {
        CloseHandle(g_file);
        g_file = INVALID_HANDLE_VALUE;
    }
    ReleaseSRWLockExclusive(&g_lock);
}

void Write(const char* format, ...) {
    char line[1024];
    SYSTEMTIME t;
    GetLocalTime(&t);
    int n = _snprintf_s(line, _TRUNCATE, "%02u:%02u:%02u.%03u [RetroShim %lu:%lu] ",
                        t.wHour, t.wMinute, t.wSecond, t.wMilliseconds,
                        GetCurrentProcessId(), GetCurrentThreadId());
    if (n < 0) n = 0;

    va_list args;
    va_start(args, format);
    int m = _vsnprintf_s(line + n, sizeof(line) - n, _TRUNCATE, format, args);
    va_end(args);
    size_t len = (m < 0) ? sizeof(line) - 1 : static_cast<size_t>(n + m);

    // Always terminate with CRLF, even if the message was truncated.
    if (len > sizeof(line) - 3) len = sizeof(line) - 3;
    line[len++] = '\r';
    line[len++] = '\n';
    line[len] = '\0';

    OutputDebugStringA(line);

    AcquireSRWLockExclusive(&g_lock);
    if (g_file != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteFile(g_file, line, static_cast<DWORD>(len), &written, nullptr);
    }
    ReleaseSRWLockExclusive(&g_lock);
}

}  // namespace retro::log
