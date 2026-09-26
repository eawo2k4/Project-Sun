#pragma once

// What the Windows 3.x system DLLs export: ordinal, name, calling convention,
// parameter types and constants, whether or not the engine implements them.
// Generated (ApiCatalog.cpp, by tools/gen_win16_catalog.py).
//
// The engine uses it to:
//   * name the functions a program calls ("USER.216 (GetDlgItem)"), in traces
//     and when a call isn't implemented;
//   * decode arguments for --trace-win16;
//   * resolve imports of constants (equates such as __AHINCR, __WINFLAGS);
//   * let a program importing a DLL the engine doesn't have (SHELL, MMSYSTEM,
//     ...) load, stopping only if it actually calls into it;
//   * stub missing Pascal functions with known parameters (--stub-missing).

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace retro::win16 {

enum class CatalogKind : uint8_t {
    Pascal,    // callee pops `params`
    Register,  // arguments and results in registers
    Varargs,   // C convention: the caller pops
    Equate,    // a constant, imported like a function
    Variable,  // exported data
    Stub,      // exists; signature unknown
};

struct CatalogEntry {
    uint16_t ordinal;
    CatalogKind kind;
    bool ret16;       // returns AX only (otherwise DX:AX)
    uint16_t value;   // Equate
    const char* name;
    // One character per parameter, in declaration order, or nullptr if unknown:
    //   w WORD   s signed WORD   l LONG   p far pointer   P far pointer (segptr)
    //   z far string   Z far string (segstr)
    const char* params;
};

struct CatalogModule {
    const char* name;
    const CatalogEntry* entries;  // sorted by ordinal
    size_t count;
};

extern const CatalogModule kCatalog[];
extern const size_t kCatalogSize;

const CatalogModule* FindCatalogModule(std::string_view name);  // case-insensitive
const CatalogEntry* FindCatalogEntry(const CatalogModule& module, uint16_t ordinal);
const CatalogEntry* FindCatalogEntry(const CatalogModule& module, std::string_view name);

// Bytes of Pascal arguments for a parameter string.
uint16_t ParamBytes(const char* params);

}  // namespace retro::win16
