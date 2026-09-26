// Writes the synthetic Win16 test programs as real NE .exe files, for the
// end-to-end tests that run them through RetroLaunch.
//
//   MakeWin16Samples <output directory>

#include <cstdio>
#include <fstream>
#include <string>

#include "TestPrograms.h"

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: MakeWin16Samples <output directory>\n");
        return 2;
    }
    const std::string dir = argv[1];
    const struct {
        const char* file;
        win16test::NeProgram program;
    } samples[] = {
        {"win16_selftest.exe", win16test::SelfTestProgram()},
        {"win16_hello.exe", win16test::HelloProgram()},
        {"win16_fatalexit.exe", win16test::FatalExitProgram(7)},
        {"win16_unimplemented.exe", win16test::UnimplementedApiProgram()},
    };
    for (const auto& s : samples) {
        const std::vector<uint8_t> bytes = win16test::BuildNe(s.program);
        std::ofstream out(dir + "/" + s.file, std::ios::binary);
        out.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
        if (!out) {
            std::fprintf(stderr, "cannot write %s\n", s.file);
            return 1;
        }
        std::printf("wrote %s (%zu bytes)\n", s.file, bytes.size());
    }
    return 0;
}
