// Win16 engine tests: NE parsing, loading (selectors, relocations, initial
// registers) and running synthetic programs to completion.

#include <cstdio>
#include <string>

#include "../Check.h"
#include "TestPrograms.h"
#include "win16/Memory.h"
#include "win16/NeImage.h"
#include "win16/Runtime.h"

using namespace retro::win16;
using namespace win16test;

namespace {

struct RunOutcome {
    bool loaded = false;
    std::string loadError;
    TaskExit exit;
    std::vector<std::string> output;
};

RunOutcome RunProgram(const NeProgram& program, uint64_t budget = 1'000'000) {
    RunOutcome o;
    Runtime rt;
    rt.SetOutput([&](const std::string& line) { o.output.push_back(line); });
    o.loaded = rt.Load(BuildNe(program), "arg1 arg2", o.loadError);
    if (o.loaded) o.exit = rt.Run(budget);
    std::printf("  -> %s code %u: %s (%llu instructions)\n", ToString(o.exit.kind), o.exit.code,
                o.loaded ? o.exit.message.c_str() : o.loadError.c_str(),
                static_cast<unsigned long long>(o.exit.instructions));
    return o;
}

// --- NE parsing -------------------------------------------------------------------------

void TestParseSelfTestImage() {
    const NeProgram program = SelfTestProgram();
    NeImage img;
    std::string error;
    CHECK(ParseNe(BuildNe(program), img, error));
    CHECK(error.empty());
    CHECK(img.moduleName == "TESTAPP");
    CHECK(img.segments.size() == 3);
    CHECK(!img.segments[0].IsData() && img.segments[1].IsData() && !img.segments[2].IsData());
    CHECK(img.segments[0].fileLength == program.segments[0].bytes.size());
    CHECK(img.segments[1].minAlloc == 0x100);
    CHECK(img.entrySegment == 1 && img.entryIp == 0);
    CHECK(img.autoDataSegment == 2 && img.heapSize == 0x100 && img.stackSize == 0x400);
    CHECK((img.moduleRefs == std::vector<std::string>{"KERNEL", "USER"}));
    CHECK(img.expectedWinVer == 0x030A && img.targetOS == 2 && !img.IsLibrary());

    // Relocations: InitTask, far call to segment 3, GetVersion.
    const auto& relocs = img.segments[0].relocations;
    CHECK(relocs.size() == 3);
    CHECK(relocs[0].target == RelocTarget::ImportOrdinal && relocs[0].module == 1 &&
          relocs[0].ordinal == 91 && relocs[0].source == RelocSource::FarPointer);
    CHECK(relocs[1].target == RelocTarget::Internal && relocs[1].segment == 3 &&
          relocs[1].targetOffset == 0);
    CHECK(relocs[2].ordinal == 3);
}

void TestParseByName() {
    NeImage img;
    std::string error;
    CHECK(ParseNe(BuildNe(ByNameProgram(false)), img, error));
    CHECK(img.segments[0].relocations.size() == 1);
    CHECK(img.segments[0].relocations[0].target == RelocTarget::ImportName);
    CHECK(img.segments[0].relocations[0].name == "GETVERSION");
}

void TestParseRejectsBadImages() {
    NeImage img;
    std::string error;
    CHECK(!ParseNe({'M', 'Z'}, img, error));
    std::vector<uint8_t> file = BuildNe(SelfTestProgram());
    file[0x40] = 'P';  // not NE any more
    CHECK(!ParseNe(file, img, error));
    file = BuildNe(SelfTestProgram());
    file.resize(file.size() - 20);  // cut into the last segment
    CHECK(!ParseNe(file, img, error));
    std::printf("  truncated image: %s\n", error.c_str());
}

// --- Memory / LDT --------------------------------------------------------------------------

void TestSelectorsAndProtection() {
    Memory mem(1 << 20);
    const uint16_t code = mem.Allocate(0x20, SegmentKind::Code);
    const uint16_t data = mem.Allocate(0x100, SegmentKind::Data);
    CHECK(code == 0x0107 && data == 0x010F);  // Windows-style LDT selectors
    CHECK(mem.Lookup(code)->kind == SegmentKind::Code);
    CHECK(mem.SegmentSize(data) == 0x100);

    mem.Write16(data, 0xFE, 0xBEEF);
    CHECK(mem.Read16(data, 0xFE) == 0xBEEF);

    auto faults = [&](auto fn) {
        try {
            fn();
        } catch (const ProtectionFault&) {
            return true;
        }
        return false;
    };
    CHECK(faults([&] { mem.Read16(data, 0xFF); }));        // straddles the limit
    CHECK(faults([&] { mem.Read8(data, 0x100); }));        // past the limit
    CHECK(faults([&] { mem.Write8(code, 0, 1); }));        // write to code
    CHECK(faults([&] { mem.Read8(0, 0); }));               // null selector
    CHECK(faults([&] { mem.Read8(0x0108, 0); }));          // GDT selector
    CHECK(faults([&] { mem.Read8(data, 0, Access::Execute); }));  // execute data
    mem.Free(data);
    CHECK(faults([&] { mem.Read8(data, 0); }));            // freed
    CHECK(mem.Lookup(data) == nullptr);
}

// --- Loading and running ----------------------------------------------------------------------

void TestLoaderSetsUpTask() {
    Runtime rt;
    std::string error;
    CHECK(rt.Load(BuildNe(SelfTestProgram()), "hello world", error));
    const LoadedModule& m = rt.Module();
    const Registers& r = rt.Processor().Regs();
    CHECK(m.selectors.size() == 3);
    CHECK(r.s[CS] == m.selectors[0] && r.ip == 0);
    CHECK(r.s[DS] == m.dgroup && m.dgroup == m.selectors[1]);
    CHECK(r.s[SS] == m.dgroup);
    CHECK(rt.Mem().SegmentSize(m.dgroup) == 0x100 + 0x100 + 0x400);  // + heap + stack
    CHECK(r.r[SP] == 0x600);
    CHECK(r.s[ES] == m.psp && r.r[DI] == m.dgroup && r.r[BX] == 0x400 && r.r[CX] == 0x100);

    // The far call to segment 3 was relocated to segment 3's selector.
    const uint8_t* code = rt.Mem().SegmentData(m.selectors[0]);
    bool found = false;
    for (uint32_t i = 0; i + 4 < rt.Mem().SegmentSize(m.selectors[0]); ++i) {
        if (code[i] == 0x9A && code[i + 3] == uint8_t(m.selectors[2]) &&
            code[i + 4] == uint8_t(m.selectors[2] >> 8)) {
            found = true;
        }
    }
    CHECK(found);

    // PSP: INT 20h and the command tail.
    const uint8_t* psp = rt.Mem().SegmentData(m.psp);
    CHECK(psp[0] == 0xCD && psp[1] == 0x20);
    CHECK(psp[0x80] == 12 && std::string(reinterpret_cast<const char*>(psp + 0x81), 12) == " hello world");
    CHECK(psp[0x8D] == 0x0D);
}

void TestSelfTestProgramPasses() {
    const RunOutcome o = RunProgram(SelfTestProgram());
    CHECK(o.loaded);
    CHECK(o.exit.kind == TaskExit::Kind::Exited);
    CHECK(o.exit.code == 0);  // otherwise: number of the failed check
    CHECK(o.exit.instructions > 100);
}

void TestFatalExit() {
    const RunOutcome o = RunProgram(FatalExitProgram(7));
    CHECK(o.exit.kind == TaskExit::Kind::FatalExit && o.exit.code == 7);
    CHECK(o.exit.message == "FatalExit(7)");
}

void TestMessageBoxAndDosOutput() {
    RunOutcome o = RunProgram(HelloProgram());
    CHECK(o.exit.kind == TaskExit::Kind::Exited && o.exit.code == 0);
    CHECK(o.output.size() == 1 && o.output[0] == "MessageBox [Project Sun]: Hello from Win16");

    o = RunProgram(DosPrintProgram());
    CHECK(o.exit.kind == TaskExit::Kind::Exited && o.exit.code == 5);
    CHECK(o.output.size() == 1 && o.output[0] == "Hi from DOS");
}

void TestImportsByName() {
    RunOutcome o = RunProgram(ByNameProgram(false));
    CHECK(o.exit.kind == TaskExit::Kind::Exited && o.exit.code == 3);  // AL = major version

    o = RunProgram(ByNameProgram(true));
    CHECK(o.exit.kind == TaskExit::Kind::Unimplemented);
    CHECK(o.exit.message == "KERNEL.FROBNICATE is not implemented yet");
}

void TestUnimplementedApiStopsCleanly() {
    const RunOutcome o = RunProgram(UnimplementedApiProgram());
    CHECK(o.exit.kind == TaskExit::Kind::Unimplemented);
    CHECK(o.exit.message == "USER.41 is not implemented yet");
}

void TestFaultsAreReported() {
    RunOutcome o = RunProgram(OutOfBoundsProgram());
    CHECK(o.exit.kind == TaskExit::Kind::Fault);
    CHECK(o.exit.message.find("general protection fault") != std::string::npos);
    CHECK(o.exit.registers.ip == 1);  // the faulting instruction, not the one after

    o = RunProgram(DivideByZeroProgram());
    CHECK(o.exit.kind == TaskExit::Kind::Fault);
    CHECK(o.exit.message.find("divide error") != std::string::npos);
}

void TestMissingModuleFailsToLoad() {
    const RunOutcome o = RunProgram(MissingModuleProgram());
    CHECK(!o.loaded);
    CHECK(o.loadError.find("SHELL") != std::string::npos);
}

void TestBudget() {
    // An endless loop stops at the budget instead of hanging the host.
    NeProgram p = BaseProgram();
    NeSeg code;
    code.bytes = {0xEB, 0xFE};  // jmp $
    p.segments = {code, DataSegment({}, 0x100)};
    const RunOutcome o = RunProgram(p, 5000);
    CHECK(o.exit.kind == TaskExit::Kind::BudgetExhausted && o.exit.instructions == 5000);
}

}  // namespace

int main() {
    const test::Case cases[] = {
        {"ParseSelfTestImage", TestParseSelfTestImage},
        {"ParseByName", TestParseByName},
        {"ParseRejectsBadImages", TestParseRejectsBadImages},
        {"SelectorsAndProtection", TestSelectorsAndProtection},
        {"LoaderSetsUpTask", TestLoaderSetsUpTask},
        {"SelfTestProgramPasses", TestSelfTestProgramPasses},
        {"FatalExit", TestFatalExit},
        {"MessageBoxAndDosOutput", TestMessageBoxAndDosOutput},
        {"ImportsByName", TestImportsByName},
        {"UnimplementedApiStopsCleanly", TestUnimplementedApiStopsCleanly},
        {"FaultsAreReported", TestFaultsAreReported},
        {"MissingModuleFailsToLoad", TestMissingModuleFailsToLoad},
        {"Budget", TestBudget},
    };
    return test::RunAll(cases);
}
