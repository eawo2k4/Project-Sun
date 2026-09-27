// The PC BIOS as a Windows 3.x program sees it: the BIOS data area through
// selector 0040h (with a live timer tick count, which games poll for timing),
// and the BIOS interrupts Windows passes through to it - the clock (INT 1Ah),
// equipment and memory size (INT 11h/12h), and the keyboard status calls of
// INT 16h. INT 2Fh answers the Windows mode queries.

#include <chrono>
#include <cstdio>
#include <ctime>

#include "win16/Runtime.h"

namespace retro::win16 {
namespace {

constexpr uint32_t kMsPerDay = 24u * 60u * 60u * 1000u;
constexpr uint16_t kEquipment = 0x0023;  // a floppy drive, a math coprocessor, 80x25 color video
constexpr uint16_t kBaseMemoryKb = 640;

void Put16(uint8_t* p, uint16_t v) {
    p[0] = uint8_t(v);
    p[1] = uint8_t(v >> 8);
}

void Put32(uint8_t* p, uint32_t v) {
    Put16(p, uint16_t(v));
    Put16(p + 2, uint16_t(v >> 16));
}

uint8_t Bcd(int v) { return uint8_t(((v / 10) << 4) | (v % 10)); }

}  // namespace

void Runtime::SetUpBiosData() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm local{};
    localtime_s(&local, &t);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000;
    startMsOfDay_ = uint32_t(((local.tm_hour * 60 + local.tm_min) * 60 + local.tm_sec) * 1000 + ms);

    // 300h bytes, the size of Windows' 0040h segment (the BIOS data area and what follows it).
    memory_.DefineFixed(kBiosDataSelector, 0x300, [this](uint8_t* bda) { RefreshBiosData(bda); });
    uint8_t* bda = memory_.SegmentData(kBiosDataSelector);
    Put16(bda + 0x10, kEquipment);
    Put16(bda + 0x13, kBaseMemoryKb);
    Put16(bda + 0x1A, 0x1E);  // keyboard buffer head = tail: empty
    Put16(bda + 0x1C, 0x1E);
    Put16(bda + 0x80, 0x1E);  // keyboard buffer start and end
    Put16(bda + 0x82, 0x3E);
    bda[0x49] = 3;            // video mode 3: 80x25 text
    Put16(bda + 0x4A, 80);    // columns
    Put16(bda + 0x4C, 4000);  // page size
    Put16(bda + 0x63, 0x3D4); // CRT controller port
    bda[0x75] = 1;            // one hard disk
    bda[0x84] = 24;           // rows - 1
    bda[0x85] = 16;           // character height
    RefreshBiosData(bda);
}

uint32_t Runtime::BiosTicks(bool& newDay) {
    const uint64_t total = uint64_t(startMsOfDay_) + TickCount();
    newDay = total / kMsPerDay > reportedDays_;
    // 1193182 Hz / 65536: 18.2065 ticks a second, 1800B0h a day.
    return uint32_t(uint64_t(total % kMsPerDay) * 1193182u / (65536u * 1000u));
}

void Runtime::RefreshBiosData(uint8_t* bda) {
    bool newDay = false;
    Put32(bda + 0x6C, BiosTicks(newDay));
    bda[0x70] = newDay ? 1 : 0;
}

bool Runtime::BiosInterrupt(uint8_t vector) {
    Registers& r = cpu_.Regs();
    const uint8_t ah = r.R8(4);
    switch (vector) {
    case 0x11: r.r[AX] = kEquipment; return true;
    case 0x12: r.r[AX] = kBaseMemoryKb; return true;
    case 0x16:  // keyboard: Windows owns it, so there's never a key waiting here
        if (ah == 0x01 || ah == 0x11) {
            cpu_.SetFlag(flags::ZF, true);
            return true;
        }
        if (ah == 0x02 || ah == 0x12) {
            r.R8(0) = 0;
            if (ah == 0x12) r.R8(4) = 0;
            return true;
        }
        return false;
    case 0x1A: {
        cpu_.SetFlag(flags::CF, false);
        if (ah == 0x00) {  // tick count since midnight; AL: a midnight passed since the last call
            bool newDay = false;
            const uint32_t ticks = BiosTicks(newDay);
            r.r[CX] = uint16_t(ticks >> 16);
            r.r[DX] = uint16_t(ticks);
            r.R8(0) = newDay ? 1 : 0;
            reportedDays_ = uint32_t((uint64_t(startMsOfDay_) + TickCount()) / kMsPerDay);
            return true;
        }
        if (ah == 0x02 || ah == 0x04) {  // real-time clock: time or date, in BCD
            const std::time_t now = std::time(nullptr);
            std::tm t{};
            localtime_s(&t, &now);
            if (ah == 0x02) {
                r.R8(5) = Bcd(t.tm_hour);  // CH
                r.R8(1) = Bcd(t.tm_min);   // CL
                r.R8(6) = Bcd(t.tm_sec);   // DH
                r.R8(2) = 0;               // DL: standard time
            } else {
                const int year = t.tm_year + 1900;
                r.R8(5) = Bcd(year / 100);        // CH: century
                r.R8(1) = Bcd(year % 100);        // CL
                r.R8(6) = Bcd(t.tm_mon + 1);      // DH
                r.R8(2) = Bcd(t.tm_mday);         // DL
            }
            return true;
        }
        if (ah == 0x01 || ah == 0x03 || ah == 0x05) return true;  // setting the clock: ignored
        char line[64];
        std::snprintf(line, sizeof(line), "INT 1Ah AH=%02Xh is not supported (returned an error)", ah);
        Note("int1a:" + std::to_string(ah), line);
        cpu_.SetFlag(flags::CF, true);
        return true;
    }
    case 0x2F:  // multiplex
        if (r.r[AX] == 0x1600) {  // enhanced mode Windows? no: standard mode
            r.R8(0) = 0;
        } else if (r.r[AX] == 0x1686) {  // running in protected mode? yes
            r.r[AX] = 0;
        }
        // Anything else: no handler installed, registers unchanged.
        return true;
    default:
        return false;
    }
}

}  // namespace retro::win16
