#pragma once

#include <string>

namespace retro {

// Converts a path to the ANSI code page for APIs that only take LPCSTR (such
// as Detours' DLL injection, which writes the name into the target's import
// table). Falls back to the 8.3 short name when the long path contains
// characters the code page cannot represent. Returns false if neither works.
bool ToAnsiPath(const std::wstring& path, std::string& out);

// UTF-8 conversion for logging and console output.
std::string ToUtf8(const std::wstring& s);

}  // namespace retro
