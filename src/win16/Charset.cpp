// Character sets: the KEYBOARD driver's OEM <-> ANSI conversions and keyboard
// queries, and USER's ANSI character functions (AnsiUpper, IsCharAlpha, ...).
//
// ANSI is Windows-1252 and OEM is code page 437, as on a US Windows 3.1
// machine; the host does the conversions and the case mapping.

#include <windows.h>

#include <string>

#include "win16/Api.h"
#include "win16/Runtime.h"

namespace retro::win16 {
namespace {

constexpr UINT kAnsiCodePage = 1252;
constexpr UINT kOemCodePage = 437;

// Converts `count` bytes from one single-byte code page to the other.
std::string Convert(const std::string& in, UINT from, UINT to) {
    if (in.empty()) return in;
    std::wstring wide(in.size(), L'\0');
    const int n = MultiByteToWideChar(from, 0, in.data(), int(in.size()), wide.data(), int(wide.size()));
    std::string out(in.size(), '?');
    if (n > 0) WideCharToMultiByte(to, 0, wide.data(), n, out.data(), int(out.size()), "?", nullptr);
    return out;
}

std::string ReadBytes(Runtime& rt, FarPtr p, uint16_t count) {
    std::string s(count, '\0');
    for (uint16_t i = 0; i < count; ++i) s[i] = char(rt.Mem().Read8(p.sel, uint16_t(p.off + i)));
    return s;
}

void WriteBytes(Runtime& rt, FarPtr p, const std::string& s) {
    for (size_t i = 0; i < s.size(); ++i) rt.Mem().Write8(p.sel, uint16_t(p.off + i), uint8_t(s[i]));
}

// AnsiToOem / OemToAnsi: a NUL-terminated string (the two may be the same buffer).
void ConvertString(Runtime& rt, Cpu& cpu, UINT from, UINT to) {  // (LPCSTR src, LPSTR dst) -> TRUE
    const PascalArgs a(cpu, {4, 4});
    const FarPtr src = a.Ptr(0), dst = a.Ptr(1);
    const std::string s = rt.Mem().ReadString(src.sel, src.off, 0xFFFF);
    WriteBytes(rt, dst, Convert(s, from, to) + '\0');
    cpu.Regs().r[AX] = 0xFFFF;
    cpu.ReturnFar(a.Bytes());
}

void ConvertBuffer(Runtime& rt, Cpu& cpu, UINT from, UINT to) {  // (LPCSTR src, LPSTR dst, UINT count)
    const PascalArgs a(cpu, {4, 4, 2});
    WriteBytes(rt, a.Ptr(1), Convert(ReadBytes(rt, a.Ptr(0), a.Word(2)), from, to));
    cpu.Regs().r[AX] = 0xFFFF;
    cpu.ReturnFar(a.Bytes());
}

void Api_AnsiToOem(Runtime& rt, Cpu& cpu) { ConvertString(rt, cpu, kAnsiCodePage, kOemCodePage); }
void Api_OemToAnsi(Runtime& rt, Cpu& cpu) { ConvertString(rt, cpu, kOemCodePage, kAnsiCodePage); }
void Api_AnsiToOemBuff(Runtime& rt, Cpu& cpu) { ConvertBuffer(rt, cpu, kAnsiCodePage, kOemCodePage); }
void Api_OemToAnsiBuff(Runtime& rt, Cpu& cpu) { ConvertBuffer(rt, cpu, kOemCodePage, kAnsiCodePage); }

void Api_GetKeyboardType(Runtime&, Cpu& cpu) {  // (int which) -> type, subtype or function keys
    static const uint16_t kAnswers[3] = {4, 0, 12};  // an enhanced 101/102-key keyboard
    const uint16_t which = cpu.StackArg(0);
    cpu.Regs().r[AX] = which < 3 ? kAnswers[which] : 0;
    cpu.ReturnFar(2);
}

void Api_GetKBCodePage(Runtime&, Cpu& cpu) {
    cpu.Regs().r[AX] = uint16_t(kOemCodePage);
    cpu.ReturnFar(0);
}

void Api_MapVirtualKey(Runtime&, Cpu& cpu) {  // (UINT code, UINT mapType) -> UINT
    const PascalArgs a(cpu, {2, 2});
    cpu.Regs().r[AX] = uint16_t(MapVirtualKeyW(a.Word(0), a.Word(1)));
    cpu.ReturnFar(a.Bytes());
}

void Api_VkKeyScan(Runtime&, Cpu& cpu) {  // (UINT ch) -> virtual key | shift state << 8
    cpu.Regs().r[AX] = uint16_t(VkKeyScanA(char(cpu.StackArg(0))));
    cpu.ReturnFar(2);
}

// --- USER's ANSI character functions -----------------------------------------------------------

char CaseOf(char c, bool upper) {
    char s[2] = {c, 0};
    if (upper) CharUpperBuffA(s, 1); else CharLowerBuffA(s, 1);
    return s[0];
}

// AnsiUpper / AnsiLower: a string in place, or (selector 0) one character.
void ChangeCase(Runtime& rt, Cpu& cpu, bool upper) {  // (LPSTR) -> the string, or the character
    const PascalArgs a(cpu, {4});
    const FarPtr p = a.Ptr(0);
    if (p.sel == 0) {
        SetResult(cpu, uint8_t(CaseOf(char(p.off), upper)));
    } else {
        const std::string s = rt.Mem().ReadString(p.sel, p.off, 0xFFFF);
        for (size_t i = 0; i < s.size(); ++i)
            rt.Mem().Write8(p.sel, uint16_t(p.off + i), uint8_t(CaseOf(s[i], upper)));
        SetResult(cpu, (uint32_t(p.sel) << 16) | p.off);
    }
    cpu.ReturnFar(a.Bytes());
}

void ChangeCaseBuff(Runtime& rt, Cpu& cpu, bool upper) {  // (LPSTR, UINT count) -> count
    const PascalArgs a(cpu, {4, 2});
    const FarPtr p = a.Ptr(0);
    for (uint16_t i = 0; i < a.Word(1); ++i) {
        const uint16_t at = uint16_t(p.off + i);
        rt.Mem().Write8(p.sel, at, uint8_t(CaseOf(char(rt.Mem().Read8(p.sel, at)), upper)));
    }
    cpu.Regs().r[AX] = a.Word(1);
    cpu.ReturnFar(a.Bytes());
}

void Api_AnsiUpper(Runtime& rt, Cpu& cpu) { ChangeCase(rt, cpu, true); }
void Api_AnsiLower(Runtime& rt, Cpu& cpu) { ChangeCase(rt, cpu, false); }
void Api_AnsiUpperBuff(Runtime& rt, Cpu& cpu) { ChangeCaseBuff(rt, cpu, true); }
void Api_AnsiLowerBuff(Runtime& rt, Cpu& cpu) { ChangeCaseBuff(rt, cpu, false); }

void Api_AnsiNext(Runtime& rt, Cpu& cpu) {  // (LPCSTR) -> the next character (single-byte: +1, not past the NUL)
    const FarPtr p = ArgPtr(cpu, 0);
    const bool end = rt.Mem().Read8(p.sel, p.off) == 0;
    SetResult(cpu, (uint32_t(p.sel) << 16) | uint16_t(end ? p.off : p.off + 1));
    cpu.ReturnFar(4);
}

void Api_AnsiPrev(Runtime&, Cpu& cpu) {  // (LPCSTR start, LPCSTR current) -> the previous character
    const PascalArgs a(cpu, {4, 4});
    const FarPtr start = a.Ptr(0), cur = a.Ptr(1);
    SetResult(cpu, (uint32_t(cur.sel) << 16) | uint16_t(cur.off > start.off ? cur.off - 1 : cur.off));
    cpu.ReturnFar(a.Bytes());
}

template <BOOL(WINAPI* Test)(CHAR)>
void CharTest(Runtime&, Cpu& cpu) {  // (char) -> BOOL
    cpu.Regs().r[AX] = Test(CHAR(cpu.StackArg(0))) ? 1 : 0;
    cpu.ReturnFar(2);
}

}  // namespace

std::vector<ApiFunction> KeyboardApi() {
    return {
        {5, "ANSITOOEM", Api_AnsiToOem},
        {6, "OEMTOANSI", Api_OemToAnsi},
        {129, "VKKEYSCAN", Api_VkKeyScan},
        {130, "GETKEYBOARDTYPE", Api_GetKeyboardType},
        {131, "MAPVIRTUALKEY", Api_MapVirtualKey},
        {132, "GETKBCODEPAGE", Api_GetKBCodePage},
        {134, "ANSITOOEMBUFF", Api_AnsiToOemBuff},
        {135, "OEMTOANSIBUFF", Api_OemToAnsiBuff},
    };
}

std::vector<ApiFunction> UserCharsetApi() {
    return {
        {431, "ANSIUPPER", Api_AnsiUpper},
        {432, "ANSILOWER", Api_AnsiLower},
        {433, "ISCHARALPHA", CharTest<IsCharAlphaA>},
        {434, "ISCHARALPHANUMERIC", CharTest<IsCharAlphaNumericA>},
        {435, "ISCHARUPPER", CharTest<IsCharUpperA>},
        {436, "ISCHARLOWER", CharTest<IsCharLowerA>},
        {437, "ANSIUPPERBUFF", Api_AnsiUpperBuff},
        {438, "ANSILOWERBUFF", Api_AnsiLowerBuff},
        {472, "ANSINEXT", Api_AnsiNext},
        {473, "ANSIPREV", Api_AnsiPrev},
    };
}

}  // namespace retro::win16
