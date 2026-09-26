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
        {"win16_window.exe", win16test::WindowProgram(false)},
        {"win16_fullscreen.exe", win16test::WindowProgram(false, true)},
        {"win16_paint.exe", win16test::PaintProgram()},
        {"win16_anim.exe", win16test::AnimationProgram(30)},
        {"win16_resources.exe", win16test::ResourceProgram()},
        {"win16_crt.exe", win16test::CrtProgram()},
        {"win16_shell.exe", win16test::MissingModuleProgram()},
        {"win16_ui.exe", win16test::UiProgram()},
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
    // win16_crt.exe reads these from its directory.
    for (const auto& [file, content] : win16test::CrtProgramFiles()) {
        std::ofstream out(dir + "/" + file, std::ios::binary);
        out << content;
        if (!out) {
            std::fprintf(stderr, "cannot write %s\n", file.c_str());
            return 1;
        }
    }
    return 0;
}
