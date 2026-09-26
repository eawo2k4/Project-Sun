#pragma once

// Maps an NE image into segmented memory: one LDT selector per segment,
// segment data copied from the file, relocations applied (including chained
// fixups), PSP built, and the initial register state the Windows loader
// hands a task:
//
//   CS:IP = entry point      SS:SP = stack (after DGROUP's static data if SP was 0)
//   DS = DGROUP (automatic data segment)   ES = PSP
//   AX = 0  BX = stack size  CX = heap size  SI = hPrevInstance (0)
//   DI = hInstance (the DGROUP selector)    BP = 0

#include <cstdint>
#include <string>
#include <vector>

#include "win16/Cpu.h"
#include "win16/Memory.h"
#include "win16/NeImage.h"

namespace retro::win16 {

// Supplies far addresses for imported functions.
class ImportResolver {
public:
    virtual ~ImportResolver() = default;
    // `name` is set for by-name imports (ordinal 0 then).
    virtual bool Resolve(const std::string& module, uint16_t ordinal, const std::string& name,
                         uint16_t& selector, uint16_t& offset, std::string& error) = 0;
};

struct LoadedModule {
    std::vector<uint16_t> selectors;  // selector of segment n is selectors[n - 1]
    uint16_t dgroup = 0;              // automatic data segment (0 if none)
    // DGROUP is laid out like Windows does it: static data, stack, local heap.
    // The heap runs from heapStart to the end of the segment (InitTask sets it up).
    uint16_t heapStart = 0;
    uint16_t psp = 0;
    Registers initial;                // register state at the entry point
};

// For a library (DLL) there's no PSP and no register state: the caller runs
// its entry point.
bool LoadNe(const NeImage& image, const std::vector<uint8_t>& file, Memory& memory,
            ImportResolver& imports, const std::string& commandLine, LoadedModule& out,
            std::string& error);

}  // namespace retro::win16
