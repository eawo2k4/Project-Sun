#pragma once

// Synthetic Win16 programs for the engine tests, assembled with Asm16 and
// packaged as real NE executables with NeBuilder.

#include <string>

#include "Asm16.h"
#include "NeBuilder.h"

namespace win16test {

// Relocation helpers.
inline NeReloc ImportOrdinal(uint16_t site, uint16_t module, uint16_t ordinal) {
    return {3, 1, site, module, ordinal};
}
inline NeReloc ImportName(uint16_t site, uint16_t module, uint16_t nameOffset) {
    return {3, 2, site, module, nameOffset};
}
inline NeReloc FarToSegment(uint16_t site, uint16_t segment, uint16_t offset) {
    return {3, 0, site, segment, offset};
}

// "Continue if <okJcc>, otherwise exit with `code`": Jcc over a near JMP to a
// shared failure stub, so the failure path can be anywhere in the segment.
inline void FailUnless(Asm16& a, uint8_t okJcc, int code) {
    a.db({okJcc, 3});
    a.Near(0xE9, "fail" + std::to_string(code));
}
constexpr uint8_t JZ = 0x74, JNZ = 0x75, JC = 0x72;

inline NeSeg DataSegment(std::vector<uint8_t> bytes, uint32_t minAlloc) {
    NeSeg s;
    s.bytes = std::move(bytes);
    s.data = true;
    s.minAlloc = minAlloc;
    return s;
}

inline NeProgram BaseProgram() {
    NeProgram p;
    p.modules = {"KERNEL", "USER"};
    p.autoData = 2;
    return p;
}

// Exit code 0 if every check passes; otherwise the number of the first
// failed check.
inline NeProgram SelfTestProgram() {
    NeProgram p = BaseProgram();
    NeSeg code;
    Asm16 a;

    // 1. KERNEL.InitTask, the first call of every Win16 program. AX = 1.
    code.relocs.push_back(ImportOrdinal(a.CallFar(), 1, 91));
    a.db({0x85, 0xC0});  // test ax, ax
    FailUnless(a, JNZ, 1);
    // 2. The loader passes hInstance (= DGROUP) in DI.
    a.db({0x8C, 0xD8});  // mov ax, ds
    a.db({0x39, 0xF8});  // cmp ax, di
    FailUnless(a, JZ, 2);
    // 3. Sum 1..100 with LOOP.
    a.db({0x31, 0xC0});        // xor ax, ax
    a.db({0xB9, 100, 0});      // mov cx, 100
    a.Label("sum");
    a.db({0x01, 0xC8});        // add ax, cx
    a.Short(0xE2, "sum");      // loop sum
    a.db({0x3D, 0xBA, 0x13});  // cmp ax, 5050
    FailUnless(a, JZ, 3);
    // 4. Store and reload through DS (DGROUP).
    a.db({0xA3, 0x00, 0x00});              // mov [0000], ax
    a.db({0x8B, 0x1E, 0x00, 0x00});        // mov bx, [0000]
    a.db({0x81, 0xFB, 0xBA, 0x13});        // cmp bx, 5050
    FailUnless(a, JZ, 4);
    // 5. 1234 * 56 = 69104 = 1:0DF0h.
    a.db({0xB8, 0xD2, 0x04});  // mov ax, 1234
    a.db({0xBB, 0x38, 0x00});  // mov bx, 56
    a.db({0xF7, 0xE3});        // mul bx
    a.db({0x83, 0xFA, 0x01});  // cmp dx, 1
    FailUnless(a, JZ, 5);
    a.db({0x3D, 0xF0, 0x0D});  // cmp ax, 0DF0h
    FailUnless(a, JZ, 5);
    // 6. ... / 56 = 1234 remainder 0.
    a.db({0xF7, 0xF3});        // div bx
    a.db({0x3D, 0xD2, 0x04});  // cmp ax, 1234
    FailUnless(a, JZ, 6);
    a.db({0x85, 0xD2});        // test dx, dx
    FailUnless(a, JZ, 6);
    // 7. -7 / 2 = -3 remainder -1 (IDIV truncates toward zero).
    a.db({0xB8, 0xF9, 0xFF});  // mov ax, -7
    a.db({0x99});              // cwd
    a.db({0xBB, 0x02, 0x00});  // mov bx, 2
    a.db({0xF7, 0xFB});        // idiv bx
    a.db({0x3D, 0xFD, 0xFF});  // cmp ax, -3
    FailUnless(a, JZ, 7);
    a.db({0x83, 0xFA, 0xFF});  // cmp dx, -1
    FailUnless(a, JZ, 7);
    // 8. Far call into segment 3 (internal relocation); it returns 55AAh.
    code.relocs.push_back(FarToSegment(a.CallFar(), 3, 0));
    a.db({0x3D, 0xAA, 0x55});  // cmp ax, 55AAh
    FailUnless(a, JZ, 8);
    // 9. REP STOSB then REPE SCASB over the same 8 bytes.
    a.db({0x1E, 0x07, 0xFC});   // push ds / pop es / cld
    a.db({0xBF, 0x10, 0x00});   // mov di, 10h
    a.db({0xB9, 0x08, 0x00});   // mov cx, 8
    a.db({0xB0, 0x5A});         // mov al, 5Ah
    a.db({0xF3, 0xAA});         // rep stosb
    a.db({0xBF, 0x10, 0x00});   // mov di, 10h
    a.db({0xB9, 0x08, 0x00});   // mov cx, 8
    a.db({0xF3, 0xAE});         // repe scasb
    FailUnless(a, JZ, 9);
    a.db({0x85, 0xC9});         // test cx, cx
    FailUnless(a, JZ, 9);
    // 10. Near call with an ENTER/LEAVE frame: 5! = 120.
    a.db({0xB8, 0x05, 0x00});  // mov ax, 5
    a.Near(0xE8, "factorial");
    a.db({0x3D, 0x78, 0x00});  // cmp ax, 120
    FailUnless(a, JZ, 10);
    // 11. KERNEL.GetVersion = 3.10.
    code.relocs.push_back(ImportOrdinal(a.CallFar(), 1, 3));
    a.db({0x3D, 0x03, 0x0A});  // cmp ax, 0A03h
    FailUnless(a, JZ, 11);
    // 12. SHL sets CF from the bit shifted out.
    a.db({0xB8, 0x01, 0x80});  // mov ax, 8001h
    a.db({0xD1, 0xE0});        // shl ax, 1
    FailUnless(a, JC, 12);
    a.db({0x3D, 0x02, 0x00});  // cmp ax, 2
    FailUnless(a, JZ, 12);
    // Success: exit(0).
    a.db({0xB8, 0x00, 0x4C, 0xCD, 0x21});  // mov ax, 4C00h / int 21h

    a.Label("factorial");  // AX = n -> AX = n!
    a.db({0xC8, 0x02, 0x00, 0x00});  // enter 2, 0
    a.db({0x89, 0x46, 0xFE});        // mov [bp-2], ax
    a.db({0xB8, 0x01, 0x00});        // mov ax, 1
    a.Label("factorial_loop");
    a.db({0xF7, 0x66, 0xFE});        // mul word [bp-2]
    a.db({0xFF, 0x4E, 0xFE});        // dec word [bp-2]
    a.Short(JNZ, "factorial_loop");
    a.db({0xC9, 0xC3});              // leave / ret

    for (int n = 1; n <= 12; ++n) {
        a.Label("fail" + std::to_string(n));
        a.db({0xB0, n, 0xB4, 0x4C, 0xCD, 0x21});  // mov al, n / mov ah, 4Ch / int 21h
    }
    code.bytes = a.Finish();

    NeSeg farRoutine;
    farRoutine.bytes = {0xB8, 0xAA, 0x55, 0xCB};  // mov ax, 55AAh / retf

    p.segments = {code, DataSegment(std::vector<uint8_t>(0x40), 0x100), farRoutine};
    return p;
}

// Calls KERNEL.FatalExit(code).
inline NeProgram FatalExitProgram(uint16_t exitCode) {
    NeProgram p = BaseProgram();
    NeSeg code;
    Asm16 a;
    a.db({0x68}).dw(exitCode);  // push exitCode
    code.relocs.push_back(ImportOrdinal(a.CallFar(), 1, 1));
    code.bytes = a.Finish();
    p.segments = {code, DataSegment({}, 0x100)};
    return p;
}

// USER.MessageBox(NULL, "Hello from Win16", "Project Sun", MB_OK), then exit(0).
inline NeProgram HelloProgram() {
    NeProgram p = BaseProgram();
    std::vector<uint8_t> data(0x40, 0);
    const std::string text = "Hello from Win16", caption = "Project Sun";
    std::copy(text.begin(), text.end(), data.begin());
    std::copy(caption.begin(), caption.end(), data.begin() + 0x20);

    NeSeg code;
    Asm16 a;
    code.relocs.push_back(ImportOrdinal(a.CallFar(), 1, 91));  // InitTask
    a.db({0x6A, 0x00});              // push 0          ; hwnd
    a.db({0x1E, 0x68, 0x00, 0x00});  // push ds / push 0  ; text
    a.db({0x1E, 0x68, 0x20, 0x00});  // push ds / push 20h ; caption
    a.db({0x6A, 0x00});              // push 0          ; MB_OK
    code.relocs.push_back(ImportOrdinal(a.CallFar(), 2, 1));  // MessageBox
    a.db({0xB8, 0x00, 0x4C, 0xCD, 0x21});
    code.bytes = a.Finish();
    p.segments = {code, DataSegment(data, 0x100)};
    return p;
}

// Calls an API the engine doesn't have (USER.41 = CreateWindow).
inline NeProgram UnimplementedApiProgram() {
    NeProgram p = BaseProgram();
    NeSeg code;
    Asm16 a;
    code.relocs.push_back(ImportOrdinal(a.CallFar(), 2, 41));
    code.bytes = a.Finish();
    p.segments = {code, DataSegment({}, 0x100)};
    return p;
}

// Imports by name: KERNEL.GETVERSION, then exit with AL = major version (3);
// or, with `unknown`, a name KERNEL doesn't export.
inline NeProgram ByNameProgram(bool unknown) {
    NeProgram p = BaseProgram();
    p.importNames = {"GETVERSION", "FROBNICATE"};
    NeSeg code;
    Asm16 a;
    code.relocs.push_back(
        ImportName(a.CallFar(), 1, p.ImportNameOffset(unknown ? "FROBNICATE" : "GETVERSION")));
    a.db({0xB4, 0x4C, 0xCD, 0x21});  // mov ah, 4Ch / int 21h  (AL = 3)
    code.bytes = a.Finish();
    p.segments = {code, DataSegment({}, 0x100)};
    return p;
}

// INT 21h/09h prints "Hi from DOS", then exit(5).
inline NeProgram DosPrintProgram() {
    NeProgram p = BaseProgram();
    const std::string msg = "Hi from DOS$";
    NeSeg code;
    Asm16 a;
    a.db({0xBA, 0x00, 0x00, 0xB4, 0x09, 0xCD, 0x21});  // mov dx, 0 / mov ah, 9 / int 21h
    a.db({0xB8, 0x05, 0x4C, 0xCD, 0x21});              // mov ax, 4C05h / int 21h
    code.bytes = a.Finish();
    p.segments = {code, DataSegment(std::vector<uint8_t>(msg.begin(), msg.end()), 0x100)};
    return p;
}

// Reads past the end of DGROUP: #GP.
inline NeProgram OutOfBoundsProgram() {
    NeProgram p = BaseProgram();
    NeSeg code;
    code.bytes = {0x90, 0xA1, 0xF0, 0xFF};  // nop / mov ax, [FFF0h]
    p.segments = {code, DataSegment({}, 0x100)};
    return p;
}

// DIV by zero: #DE.
inline NeProgram DivideByZeroProgram() {
    NeProgram p = BaseProgram();
    NeSeg code;
    code.bytes = {0x31, 0xDB, 0xF7, 0xF3};  // xor bx, bx / div bx
    p.segments = {code, DataSegment({}, 0x100)};
    return p;
}

// Imports from a DLL that isn't built in.
inline NeProgram MissingModuleProgram() {
    NeProgram p = BaseProgram();
    p.modules = {"KERNEL", "SHELL"};
    NeSeg code;
    Asm16 a;
    code.relocs.push_back(ImportOrdinal(a.CallFar(), 2, 1));
    code.bytes = a.Finish();
    p.segments = {code, DataSegment({}, 0x100)};
    return p;
}

}  // namespace win16test
