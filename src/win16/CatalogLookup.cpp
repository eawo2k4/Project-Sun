// Lookups in the generated API catalog (ApiCatalog.cpp).

#include <algorithm>
#include <cctype>

#include "win16/ApiCatalog.h"

namespace retro::win16 {
namespace {

bool EqualNoCase(std::string_view a, std::string_view b) {
    return a.size() == b.size() &&
           std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return std::toupper(static_cast<unsigned char>(x)) == std::toupper(static_cast<unsigned char>(y));
           });
}

}  // namespace

const CatalogModule* FindCatalogModule(std::string_view name) {
    for (size_t i = 0; i < kCatalogSize; ++i) {
        if (EqualNoCase(kCatalog[i].name, name)) return &kCatalog[i];
    }
    return nullptr;
}

const CatalogEntry* FindCatalogEntry(const CatalogModule& module, uint16_t ordinal) {
    const CatalogEntry* end = module.entries + module.count;
    const CatalogEntry* it = std::lower_bound(module.entries, end, ordinal,
                                              [](const CatalogEntry& e, uint16_t o) { return e.ordinal < o; });
    return it != end && it->ordinal == ordinal ? it : nullptr;
}

const CatalogEntry* FindCatalogEntry(const CatalogModule& module, std::string_view name) {
    for (size_t i = 0; i < module.count; ++i) {
        if (EqualNoCase(module.entries[i].name, name)) return &module.entries[i];
    }
    return nullptr;
}

uint16_t ParamBytes(const char* params) {
    uint16_t bytes = 0;
    for (const char* p = params; p && *p; ++p) bytes = uint16_t(bytes + (*p == 'w' || *p == 's' ? 2 : 4));
    return bytes;
}

}  // namespace retro::win16
