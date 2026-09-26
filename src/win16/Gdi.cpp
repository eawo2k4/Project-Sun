// GDI: only what window setup needs so far. Drawing comes with painting.

#include "win16/Api.h"
#include "win16/Runtime.h"

namespace retro::win16 {
namespace {

// Programs fill WNDCLASS.hbrBackground with GetStockObject(WHITE_BRUSH) etc.
// Nothing draws with it yet, so a stable placeholder handle is enough.
void GetStockObject(Runtime&, Cpu& cpu) {
    cpu.Regs().r[AX] = uint16_t(0x0E00 + (cpu.StackArg(0) & 0xFF));
    cpu.ReturnFar(2);
}

}  // namespace

std::vector<ApiFunction> GdiApi() {
    return {
        {87, "GETSTOCKOBJECT", GetStockObject},
    };
}

}  // namespace retro::win16
