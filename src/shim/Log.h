#pragma once

// Minimal logger for the injected shim. Safe to call from DllMain: it only
// uses kernel32 (no CRT stdio, no loader-lock-sensitive work).

namespace retro::log {

void Open(const wchar_t* path);  // nullptr/empty = OutputDebugString only
void Close();
void Write(const char* format, ...);

}  // namespace retro::log
