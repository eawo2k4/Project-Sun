#include "Win16Host.h"

#include <cstdio>
#include <fstream>
#include <iterator>
#include <vector>

#include "win16/Runtime.h"

namespace retro {

int RunWin16Program(const std::filesystem::path& exe, const std::string& commandLine) {
    std::ifstream in(exe, std::ios::binary);
    const std::vector<uint8_t> file{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    if (!in.good() && !in.eof()) {
        std::fputs("error: cannot read the program\n", stderr);
        return kWin16Stopped;
    }

    win16::Runtime runtime;
    runtime.SetOutput([](const std::string& line) {
        std::printf("[win16] %s\n", line.c_str());
        std::fflush(stdout);
    });

    std::string error;
    if (!runtime.Load(file, commandLine, error)) {
        std::fprintf(stderr, "error: Win16 load failed: %s\n", error.c_str());
        return kWin16Stopped;
    }
    const win16::NeImage& image = runtime.Image();
    std::printf("Win16 task  : %s, %zu segments, entry %u:%04X\n", image.moduleName.c_str(),
                image.segments.size(), image.entrySegment, image.entryIp);
    std::fflush(stdout);

    const win16::TaskExit result = runtime.Run();
    std::printf("Win16 exit  : %s", win16::ToString(result.kind));
    if (result.kind == win16::TaskExit::Kind::Exited || result.kind == win16::TaskExit::Kind::FatalExit)
        std::printf(", code %u", result.code);
    if (!result.message.empty()) std::printf(" - %s", result.message.c_str());
    std::printf(" (%llu instructions)\n", static_cast<unsigned long long>(result.instructions));

    const win16::Registers& r = result.registers;
    if (result.kind == win16::TaskExit::Kind::Fault) {
        std::printf("              AX=%04X BX=%04X CX=%04X DX=%04X SI=%04X DI=%04X BP=%04X SP=%04X\n"
                    "              CS:IP=%04X:%04X DS=%04X ES=%04X SS=%04X FLAGS=%04X\n",
                    r.r[win16::AX], r.r[win16::BX], r.r[win16::CX], r.r[win16::DX], r.r[win16::SI],
                    r.r[win16::DI], r.r[win16::BP], r.r[win16::SP], r.s[win16::CS], r.ip,
                    r.s[win16::DS], r.s[win16::ES], r.s[win16::SS], r.flags);
    }

    switch (result.kind) {
    case win16::TaskExit::Kind::Exited:
    case win16::TaskExit::Kind::FatalExit:
        return result.code;
    default:
        return kWin16Stopped;
    }
}

}  // namespace retro
