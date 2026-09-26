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

// A classic Win16 application skeleton, checking itself as it goes:
//   GlobalAlloc/Lock/Size/Unlock/Free on a 1000-byte block,
//   RegisterClass (with LoadIcon, LoadCursor, GetStockObject),
//   CreateWindow + ShowWindow + UpdateWindow (WM_CREATE seen by the WndProc),
//   SendMessage straight into the WndProc, PeekMessage on an empty queue,
//   a GetMessage/TranslateMessage/DispatchMessage loop until WM_QUIT.
// The window is closed either by the program (PostMessage WM_CLOSE) or, with
// hostCloses, by the host (as if the user clicked X). WM_CLOSE reaches
// DefWindowProc -> DestroyWindow -> WM_DESTROY -> PostQuitMessage(0).
// Exit code 0 = all checks passed; otherwise the number of the failed check.
//
// fullscreen: a visible WS_POPUP covering the 640x480 16-bit screen, which a
// host presents borderless; otherwise a 320x200 overlapped window.
// faultInCreate: the WndProc divides by zero on WM_CREATE (a fault inside a
// callback nested in CreateWindow).
inline NeProgram WindowProgram(bool hostCloses, bool fullscreen = false, bool faultInCreate = false) {
    NeProgram p = BaseProgram();
    p.modules = {"KERNEL", "USER", "GDI"};
    constexpr uint16_t KERNEL = 1, USER = 2, GDI = 3;

    // DGROUP layout.
    constexpr uint8_t kCreated = 0x00, kGot = 0x02, kHmem = 0x04, kHwnd = 0x06, kHinst = 0x08,
                      kDestroyed = 0x0A, kClassName = 0x10, kTitle = 0x20, kWndClass = 0x40,
                      kMsg = 0x60;
    std::vector<uint8_t> data(0x80, 0);
    const std::string cls = "RetroWin", title = "Win16 Window";
    std::copy(cls.begin(), cls.end(), data.begin() + kClassName);
    std::copy(title.begin(), title.end(), data.begin() + kTitle);

    NeSeg code;
    Asm16 a;
    auto call = [&](uint16_t module, uint16_t ordinal) {
        code.relocs.push_back(ImportOrdinal(a.CallFar(), module, ordinal));
    };
    auto pushMsgPtr = [&] { a.db({0x1E, 0x68, kMsg, 0x00}); };  // push ds / push offset MSG

    // 1. InitTask; keep hInstance.
    call(KERNEL, 91);
    a.db({0x85, 0xC0});
    FailUnless(a, JNZ, 1);
    a.db({0x89, 0x3E, kHinst, 0x00});  // mov [hInst], di

    // 2-8. Global heap: GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, 1000L).
    a.db({0x68, 0x42, 0x00, 0x6A, 0x00, 0x68, 0xE8, 0x03});  // push 42h / push 0 / push 1000
    call(KERNEL, 15);
    a.db({0x85, 0xC0});
    FailUnless(a, JNZ, 2);
    a.db({0xA3, kHmem, 0x00});  // mov [hmem], ax
    a.db({0x50});               // push ax
    call(KERNEL, 18);           // GlobalLock -> DX:AX
    a.db({0x85, 0xD2});
    FailUnless(a, JNZ, 3);
    a.db({0x85, 0xC0});
    FailUnless(a, JZ, 3);
    a.db({0x8E, 0xC2, 0x89, 0xC7});  // mov es, dx / mov di, ax
    a.db({0x26, 0x80, 0x3D, 0x00});  // cmp byte es:[di], 0      (zero-initialised)
    FailUnless(a, JZ, 4);
    a.db({0x26, 0x80, 0xBD, 0xE7, 0x03, 0x00});  // cmp byte es:[di+999], 0
    FailUnless(a, JZ, 4);
    a.db({0xFC, 0xB9, 0xE8, 0x03, 0xB0, 0xA5, 0xF3, 0xAA});  // cld / mov cx,1000 / mov al,A5h / rep stosb
    a.db({0x31, 0xFF, 0xB9, 0xE8, 0x03, 0xF3, 0xAE});        // xor di,di / mov cx,1000 / repe scasb
    FailUnless(a, JZ, 5);
    a.db({0xFF, 0x36, kHmem, 0x00});  // push [hmem]
    call(KERNEL, 20);                 // GlobalSize
    a.db({0x3D, 0xE8, 0x03});
    FailUnless(a, JZ, 6);
    a.db({0xFF, 0x36, kHmem, 0x00});
    call(KERNEL, 19);  // GlobalUnlock -> 0 (no longer locked)
    a.db({0x85, 0xC0});
    FailUnless(a, JZ, 7);
    a.db({0xFF, 0x36, kHmem, 0x00});
    call(KERNEL, 17);  // GlobalFree -> 0 (success)
    a.db({0x85, 0xC0});
    FailUnless(a, JZ, 8);
    a.db({0x1E, 0x07});  // push ds / pop es

    // 9. WNDCLASS: style, lpfnWndProc = CS:WndProc, extra, hInstance, hIcon,
    //    hCursor, hbrBackground, lpszMenuName, lpszClassName.
    a.db({0xC7, 0x06, kWndClass, 0x00, 0x00, 0x00});             // style = 0
    a.db({0xC7, 0x06, kWndClass + 2, 0x00}).Abs16("WndProc");    // lpfnWndProc offset
    a.db({0x8C, 0x0E, kWndClass + 4, 0x00});                     // lpfnWndProc selector = CS
    a.db({0xC7, 0x06, kWndClass + 6, 0x00, 0x00, 0x00});         // cbClsExtra
    a.db({0xC7, 0x06, kWndClass + 8, 0x00, 0x00, 0x00});         // cbWndExtra
    a.db({0xA1, kHinst, 0x00, 0xA3, kWndClass + 10, 0x00});      // hInstance
    a.db({0x6A, 0x00, 0x6A, 0x00, 0x68, 0x00, 0x7F});            // LoadIcon(NULL, IDI_APPLICATION)
    call(USER, 174);
    a.db({0xA3, kWndClass + 12, 0x00});
    a.db({0x6A, 0x00, 0x6A, 0x00, 0x68, 0x00, 0x7F});            // LoadCursor(NULL, IDC_ARROW)
    call(USER, 173);
    a.db({0xA3, kWndClass + 14, 0x00});
    a.db({0x6A, 0x00});                                          // GetStockObject(WHITE_BRUSH)
    call(GDI, 87);
    a.db({0xA3, kWndClass + 16, 0x00});
    a.db({0xC7, 0x06, kWndClass + 18, 0x00, 0x00, 0x00});        // no menu
    a.db({0xC7, 0x06, kWndClass + 20, 0x00, 0x00, 0x00});
    a.db({0xC7, 0x06, kWndClass + 22, 0x00, kClassName, 0x00});  // lpszClassName = DS:10h
    a.db({0x8C, 0x1E, kWndClass + 24, 0x00});
    a.db({0x1E, 0x68, kWndClass, 0x00});  // push ds / push offset WNDCLASS
    call(USER, 57);                       // RegisterClass -> atom
    a.db({0x85, 0xC0});
    FailUnless(a, JNZ, 9);

    // 10. CreateWindow(class, title, style, x, y, cx, cy, NULL, NULL, hInst, NULL)
    a.db({0x1E, 0x68, kClassName, 0x00, 0x1E, 0x68, kTitle, 0x00});
    if (fullscreen) {
        a.db({0x68, 0x00, 0x90, 0x6A, 0x00});                    // WS_POPUP | WS_VISIBLE
        a.db({0x6A, 0x00, 0x6A, 0x00, 0x68, 0x80, 0x02, 0x68, 0xE0, 0x01});  // 0, 0, 640, 480
    } else {
        a.db({0x68, 0xCF, 0x00, 0x6A, 0x00});                    // WS_OVERLAPPEDWINDOW
        a.db({0x68, 0x00, 0x80, 0x68, 0x00, 0x80, 0x68, 0x40, 0x01, 0x68, 0xC8, 0x00});
        // CW_USEDEFAULT, CW_USEDEFAULT, 320, 200
    }
    a.db({0x6A, 0x00, 0x6A, 0x00, 0xFF, 0x36, kHinst, 0x00, 0x6A, 0x00, 0x6A, 0x00});
    call(USER, 41);
    a.db({0x85, 0xC0});
    FailUnless(a, JNZ, 10);
    a.db({0xA3, kHwnd, 0x00});
    // 11. The WndProc saw WM_CREATE.
    a.db({0x83, 0x3E, kCreated, 0x00, 0x01});
    FailUnless(a, JZ, 11);
    // 12. ShowWindow returns the previous visibility.
    a.db({0x50, 0x6A, 0x01});  // push hwnd / push SW_SHOWNORMAL
    call(USER, 42);
    a.db({0x85, 0xC0});
    FailUnless(a, fullscreen ? JNZ : JZ, 12);
    a.db({0xFF, 0x36, kHwnd, 0x00});
    call(USER, 124);  // UpdateWindow

    // 13. SendMessage(hwnd, WM_USER+1, 7, 0L) runs the WndProc now: 55h back.
    a.db({0xFF, 0x36, kHwnd, 0x00, 0x68, 0x01, 0x04, 0x6A, 0x07, 0x6A, 0x00, 0x6A, 0x00});
    call(USER, 111);
    a.db({0x3D, 0x55, 0x00});
    FailUnless(a, JZ, 13);
    a.db({0x83, 0x3E, kGot, 0x00, 0x07});
    FailUnless(a, JZ, 13);

    // 14. PeekMessage(PM_REMOVE) on the empty queue returns FALSE.
    pushMsgPtr();
    a.db({0x6A, 0x00, 0x6A, 0x00, 0x6A, 0x00, 0x6A, 0x01});
    call(USER, 109);
    a.db({0x85, 0xC0});
    FailUnless(a, JZ, 14);

    // Post WM_USER+1 (wParam 9) and, unless the host does it, WM_CLOSE.
    a.db({0xFF, 0x36, kHwnd, 0x00, 0x68, 0x01, 0x04, 0x6A, 0x09, 0x6A, 0x00, 0x6A, 0x00});
    call(USER, 110);
    if (!hostCloses) {
        a.db({0xFF, 0x36, kHwnd, 0x00, 0x6A, 0x10, 0x6A, 0x00, 0x6A, 0x00, 0x6A, 0x00});
        call(USER, 110);
    }

    // The message loop.
    a.Label("loop");
    pushMsgPtr();
    a.db({0x6A, 0x00, 0x6A, 0x00, 0x6A, 0x00});
    call(USER, 108);  // GetMessage
    a.db({0x85, 0xC0});
    a.Short(JZ, "done");
    pushMsgPtr();
    call(USER, 113);  // TranslateMessage
    pushMsgPtr();
    call(USER, 114);  // DispatchMessage
    a.Short(0xEB, "loop");
    a.Label("done");
    // 15. The posted message was dispatched; 16. WM_DESTROY arrived;
    // 17. the loop ended on WM_QUIT.
    a.db({0x83, 0x3E, kGot, 0x00, 0x09});
    FailUnless(a, JZ, 15);
    a.db({0x83, 0x3E, kDestroyed, 0x00, 0x01});
    FailUnless(a, JZ, 16);
    a.db({0xA1, kMsg + 2, 0x00, 0x3D, 0x12, 0x00});  // cmp msg.message, WM_QUIT
    FailUnless(a, JZ, 17);
    a.db({0xA0, kMsg + 4, 0x00, 0xB4, 0x4C, 0xCD, 0x21});  // exit(msg.wParam)

    for (int n = 1; n <= 17; ++n) {
        a.Label("fail" + std::to_string(n));
        a.db({0xB0, n, 0xB4, 0x4C, 0xCD, 0x21});
    }

    // LRESULT FAR PASCAL WndProc(HWND, UINT msg, WPARAM, LPARAM)
    //   [bp+14] hwnd  [bp+12] msg  [bp+10] wParam  [bp+8]:[bp+6] lParam
    a.Label("WndProc");
    a.db({0x55, 0x89, 0xE5});        // push bp / mov bp, sp
    a.db({0x8B, 0x46, 0x0C});        // mov ax, [bp+12]
    a.db({0x3D, 0x01, 0x00});        // WM_CREATE?
    a.Short(JNZ, "wp_not_create");
    if (faultInCreate) a.db({0x31, 0xDB, 0xF7, 0xF3});  // xor bx, bx / div bx
    a.db({0xC7, 0x06, kCreated, 0x00, 0x01, 0x00});
    a.Short(0xEB, "wp_zero");
    a.Label("wp_not_create");
    a.db({0x3D, 0x01, 0x04});        // WM_USER+1?
    a.Short(JNZ, "wp_not_user");
    a.db({0x8B, 0x46, 0x0A, 0xA3, kGot, 0x00});  // [got] = wParam
    a.db({0xB8, 0x55, 0x00, 0x31, 0xD2});        // return 55h
    a.Short(0xEB, "wp_done");
    a.Label("wp_not_user");
    a.db({0x3D, 0x02, 0x00});        // WM_DESTROY?
    a.Short(JNZ, "wp_default");
    a.db({0xC7, 0x06, kDestroyed, 0x00, 0x01, 0x00});
    a.db({0x6A, 0x00});
    call(USER, 6);                   // PostQuitMessage(0)
    a.Short(0xEB, "wp_zero");
    a.Label("wp_default");
    a.db({0xFF, 0x76, 0x0E, 0xFF, 0x76, 0x0C, 0xFF, 0x76, 0x0A, 0xFF, 0x76, 0x08, 0xFF, 0x76, 0x06});
    call(USER, 107);                 // DefWindowProc(hwnd, msg, wParam, lParam)
    a.Short(0xEB, "wp_done");
    a.Label("wp_zero");
    a.db({0x31, 0xC0, 0x31, 0xD2});
    a.Label("wp_done");
    a.db({0x5D, 0xCA, 0x0A, 0x00});  // pop bp / retf 10

    code.bytes = a.Finish();
    p.segments = {code, DataSegment(data, 0x200)};
    return p;
}

// Readable emission of Pascal calls into KERNEL/USER/GDI.
struct Emit {
    Asm16& a;
    NeSeg& code;
    static constexpr uint16_t KERNEL = 1, USER = 2, GDI = 3;

    void Call(uint16_t module, uint16_t ordinal) {
        code.relocs.push_back(ImportOrdinal(a.CallFar(), module, ordinal));
    }
    void Imm(uint16_t v) { a.db({0x68}).dw(v); }                     // push imm16
    void Long(uint32_t v) { Imm(uint16_t(v >> 16)); Imm(uint16_t(v)); }  // push a DWORD (hi, lo)
    void Mem(uint16_t off) { a.db({0xFF, 0x36}).dw(off); }           // push word [off]
    void Arg(uint8_t bpOff) { a.db({0xFF, 0x76, bpOff}); }           // push word [bp+off]
    void Far(uint16_t off) { a.db({0x1E}); Imm(off); }               // push ds / push off
    void StoreAx(uint16_t off) { a.db({0xA3}).dw(off); }             // mov [off], ax
    void Set(uint16_t off, uint16_t v) { a.db({0xC7, 0x06}).dw(off).dw(v); }  // mov word [off], v
    void CmpAx(uint16_t v) { a.db({0x3D}).dw(v); }                   // cmp ax, v
    void CmpMem(uint16_t off, uint16_t v) { a.db({0x81, 0x3E}).dw(off).dw(v); }  // cmp word [off], v

    // Registers a class (lpfnWndProc = CS:<wndProc label>) with a stock background brush.
    void RegisterClass(uint8_t wndClass, uint8_t className, uint8_t hinst, const std::string& wndProc,
                       uint16_t stockBrush, uint16_t wndExtra = 0) {
        Set(wndClass, 0);
        a.db({0xC7, 0x06, wndClass + 2, 0x00}).Abs16(wndProc);
        a.db({0x8C, 0x0E, wndClass + 4, 0x00});                    // selector = CS
        Set(uint8_t(wndClass + 6), 0);
        Set(uint8_t(wndClass + 8), wndExtra);
        a.db({0xA1, hinst, 0x00});
        StoreAx(uint8_t(wndClass + 10));
        Set(uint8_t(wndClass + 12), 0);
        Set(uint8_t(wndClass + 14), 0);
        Imm(stockBrush);
        Call(GDI, 87);                                             // GetStockObject
        StoreAx(uint8_t(wndClass + 16));
        Set(uint8_t(wndClass + 18), 0);
        Set(uint8_t(wndClass + 20), 0);
        Set(uint8_t(wndClass + 22), className);
        a.db({0x8C, 0x1E, wndClass + 24, 0x00});                   // selector = DS
        Far(wndClass);
        Call(USER, 57);                                            // RegisterClass
    }
    // CreateWindow(class, title, WS_POPUP | WS_VISIBLE, x, y, w, h, 0, 0, hInst, NULL)
    void CreatePopup(uint8_t className, uint8_t title, int16_t x, int16_t y, int16_t w, int16_t h,
                     uint8_t hinst) {
        Far(className);
        Far(title);
        Long(0x90000000);
        Imm(uint16_t(x));
        Imm(uint16_t(y));
        Imm(uint16_t(w));
        Imm(uint16_t(h));
        Imm(0);
        Imm(0);
        Mem(hinst);
        Long(0);
        Call(USER, 41);
    }
    // GetPixel(hdc, x, y) == expected, or exit with `failCode`.
    void CheckPixel(uint8_t hdcVar, int16_t x, int16_t y, uint32_t expected, int failCode) {
        Mem(hdcVar);
        Imm(uint16_t(x));
        Imm(uint16_t(y));
        Call(GDI, 83);
        a.db({0x3D}).dw(uint16_t(expected));         // cmp ax, lo
        FailUnless(a, JZ, failCode);
        a.db({0x81, 0xFA}).dw(uint16_t(expected >> 16));  // cmp dx, hi
        FailUnless(a, JZ, failCode);
    }
    void Exit0() { a.db({0xB8, 0x00, 0x4C, 0xCD, 0x21}); }
    void FailStubs(int count) {
        for (int n = 1; n <= count; ++n) {
            a.Label("fail" + std::to_string(n));
            a.db({0xB0, n, 0xB4, 0x4C, 0xCD, 0x21});
        }
    }
};

constexpr uint32_t RGB16(uint8_t r, uint8_t g, uint8_t b) {
    return uint32_t(r) | (uint32_t(g) << 8) | (uint32_t(b) << 16);
}
constexpr uint32_t kSrcCopy = 0x00CC0020, kPatCopy = 0x00F00021, kBlackness = 0x00000042,
                   kWhiteness = 0x00FF0062;

// Paints a 64x48 window in WM_PAINT with every primitive, off-screen
// composition included, then reads pixels back with GetPixel:
//   FillRect red        (0,0)-(32,24)      Rectangle blue brush (32,0)-(64,24)
//   PatBlt green        (0,24)-(32,48)     SetPixel yellow      (40,30)
//   memory DC + compatible 8x8 bitmap: a 4x4 checkerboard (PatBlt WHITENESS
//   and BLACKNESS), BitBlt to (48,32) and StretchBlt 2x wide to (32,40).
// Background brush: BLACK_BRUSH. Exit 0 if every check passes.
inline NeProgram PaintProgram() {
    NeProgram p = BaseProgram();
    p.modules = {"KERNEL", "USER", "GDI"};
    constexpr uint8_t kHinst = 0x00, kHwnd = 0x02, kPainted = 0x06, kRed = 0x08, kBlue = 0x0A,
                      kGreen = 0x0C, kMemDc = 0x0E, kBmp = 0x10, kOldBmp = 0x12, kHdc = 0x14,
                      kCheckDc = 0x16, kClass = 0x20, kTitle = 0x30, kWndClass = 0x40, kMsg = 0x60,
                      kPs = 0x80, kRedRect = 0xA0;
    std::vector<uint8_t> data(0xB0, 0);
    const std::string cls = "PaintWin", title = "Paint";
    std::copy(cls.begin(), cls.end(), data.begin() + kClass);
    std::copy(title.begin(), title.end(), data.begin() + kTitle);
    data[kRedRect + 4] = 32;  // RECT {0, 0, 32, 24}
    data[kRedRect + 6] = 24;

    NeSeg code;
    Asm16 a;
    Emit e{a, code};

    e.Call(Emit::KERNEL, 91);  // InitTask
    a.db({0x89, 0x3E, kHinst, 0x00});
    for (const auto& [var, color] : {std::pair{kRed, RGB16(255, 0, 0)}, std::pair{kBlue, RGB16(0, 0, 255)},
                                     std::pair{kGreen, RGB16(0, 255, 0)}}) {
        e.Long(color);
        e.Call(Emit::GDI, 66);  // CreateSolidBrush
        e.StoreAx(var);
    }
    e.RegisterClass(kWndClass, kClass, kHinst, "WndProc", 4 /* BLACK_BRUSH */);
    e.CreatePopup(kClass, kTitle, 0, 0, 64, 48, kHinst);
    a.db({0x85, 0xC0});
    FailUnless(a, JNZ, 1);
    e.StoreAx(kHwnd);
    e.Mem(kHwnd);
    e.Call(Emit::USER, 124);  // UpdateWindow: WM_PAINT now
    a.db({0x83, 0x3E, kPainted, 0x00, 0x01});
    FailUnless(a, JZ, 2);

    // Read the picture back through a fresh window DC.
    e.Mem(kHwnd);
    e.Call(Emit::USER, 66);  // GetDC
    e.StoreAx(kCheckDc);
    e.CheckPixel(kCheckDc, 5, 5, RGB16(255, 0, 0), 3);        // FillRect
    e.CheckPixel(kCheckDc, 48, 12, RGB16(0, 0, 255), 4);      // Rectangle interior
    e.CheckPixel(kCheckDc, 32, 12, RGB16(0, 0, 0), 5);        // Rectangle border (black pen)
    e.CheckPixel(kCheckDc, 5, 30, RGB16(0, 255, 0), 6);       // PatBlt
    e.CheckPixel(kCheckDc, 40, 30, RGB16(255, 255, 0), 7);    // SetPixel
    e.CheckPixel(kCheckDc, 49, 33, RGB16(0, 0, 0), 8);        // BitBlt: black square
    e.CheckPixel(kCheckDc, 53, 33, RGB16(255, 255, 255), 9);  //         white square
    e.CheckPixel(kCheckDc, 41, 41, RGB16(255, 255, 255), 10); // StretchBlt: src x 4 -> dst 41
    e.CheckPixel(kCheckDc, 60, 44, RGB16(0, 0, 0), 11);       // untouched: background brush
    e.Mem(kHwnd);
    e.Mem(kCheckDc);
    e.Call(Emit::USER, 68);  // ReleaseDC
    // The blue brush was selected into the paint DC; EndPaint restored the
    // DC, so it's no longer selected and can be deleted.
    e.Mem(kBlue);
    e.Call(Emit::GDI, 69);  // DeleteObject
    a.db({0x85, 0xC0});
    FailUnless(a, JNZ, 12);

    // Close, and pump until WM_QUIT (the pump presents what was painted).
    e.Mem(kHwnd);
    e.Imm(0x10);  // WM_CLOSE
    e.Imm(0);
    e.Long(0);
    e.Call(Emit::USER, 110);
    a.Label("loop");
    e.Far(kMsg);
    e.Imm(0);
    e.Imm(0);
    e.Imm(0);
    e.Call(Emit::USER, 108);  // GetMessage
    a.db({0x85, 0xC0});
    a.Short(JZ, "done");
    e.Far(kMsg);
    e.Call(Emit::USER, 114);  // DispatchMessage
    a.Short(0xEB, "loop");
    a.Label("done");
    e.Exit0();
    e.FailStubs(12);

    // WndProc: [bp+14] hwnd, [bp+12] msg, [bp+10] wParam, [bp+8]:[bp+6] lParam
    a.Label("WndProc");
    a.db({0x55, 0x89, 0xE5, 0x8B, 0x46, 0x0C});  // push bp / mov bp,sp / mov ax,[bp+12]
    a.db({0x3D, 0x0F, 0x00});                    // WM_PAINT?
    a.Short(JZ, "wp_paint");
    a.db({0x3D, 0x02, 0x00});                    // WM_DESTROY?
    a.Short(JZ, "wp_destroy");
    e.Arg(14); e.Arg(12); e.Arg(10); e.Arg(8); e.Arg(6);
    e.Call(Emit::USER, 107);                     // DefWindowProc
    a.Near(0xE9, "wp_done");
    a.Label("wp_destroy");
    e.Imm(0);
    e.Call(Emit::USER, 6);                       // PostQuitMessage(0)
    a.Near(0xE9, "wp_zero");

    a.Label("wp_paint");
    e.Arg(14); e.Far(kPs);
    e.Call(Emit::USER, 39);                      // BeginPaint
    e.StoreAx(kHdc);
    e.Mem(kHdc); e.Far(kRedRect); e.Mem(kRed);
    e.Call(Emit::USER, 81);                      // FillRect
    e.Mem(kHdc); e.Mem(kBlue);
    e.Call(Emit::GDI, 45);                       // SelectObject(blue brush)
    e.Mem(kHdc); e.Imm(32); e.Imm(0); e.Imm(64); e.Imm(24);
    e.Call(Emit::GDI, 27);                       // Rectangle
    e.Mem(kHdc); e.Mem(kGreen);
    e.Call(Emit::GDI, 45);                       // SelectObject(green brush)
    e.Mem(kHdc); e.Imm(0); e.Imm(24); e.Imm(32); e.Imm(24); e.Long(kPatCopy);
    e.Call(Emit::GDI, 29);                       // PatBlt
    e.Mem(kHdc); e.Imm(40); e.Imm(30); e.Long(RGB16(255, 255, 0));
    e.Call(Emit::GDI, 31);                       // SetPixel
    // Off-screen: memory DC + compatible bitmap, checkerboard, then blit.
    e.Mem(kHdc);
    e.Call(Emit::GDI, 52);                       // CreateCompatibleDC
    e.StoreAx(kMemDc);
    e.Mem(kHdc); e.Imm(8); e.Imm(8);
    e.Call(Emit::GDI, 51);                       // CreateCompatibleBitmap
    e.StoreAx(kBmp);
    e.Mem(kMemDc); e.Mem(kBmp);
    e.Call(Emit::GDI, 45);                       // SelectObject -> old (default 1x1) bitmap
    e.StoreAx(kOldBmp);
    e.Mem(kMemDc); e.Imm(0); e.Imm(0); e.Imm(8); e.Imm(8); e.Long(kWhiteness);
    e.Call(Emit::GDI, 29);
    e.Mem(kMemDc); e.Imm(0); e.Imm(0); e.Imm(4); e.Imm(4); e.Long(kBlackness);
    e.Call(Emit::GDI, 29);
    e.Mem(kMemDc); e.Imm(4); e.Imm(4); e.Imm(4); e.Imm(4); e.Long(kBlackness);
    e.Call(Emit::GDI, 29);
    e.Mem(kHdc); e.Imm(48); e.Imm(32); e.Imm(8); e.Imm(8); e.Mem(kMemDc); e.Imm(0); e.Imm(0);
    e.Long(kSrcCopy);
    e.Call(Emit::GDI, 34);                       // BitBlt
    e.Mem(kHdc); e.Imm(32); e.Imm(40); e.Imm(16); e.Imm(8); e.Mem(kMemDc); e.Imm(0); e.Imm(0);
    e.Imm(8); e.Imm(8); e.Long(kSrcCopy);
    e.Call(Emit::GDI, 35);                       // StretchBlt
    e.Mem(kMemDc); e.Mem(kOldBmp);
    e.Call(Emit::GDI, 45);                       // restore the default bitmap
    e.Mem(kBmp);
    e.Call(Emit::GDI, 69);                       // DeleteObject(bitmap)
    e.Mem(kMemDc);
    e.Call(Emit::GDI, 68);                       // DeleteDC
    e.Arg(14); e.Far(kPs);
    e.Call(Emit::USER, 40);                      // EndPaint
    e.Set(kPainted, 1);
    a.Label("wp_zero");
    a.db({0x31, 0xC0, 0x31, 0xD2});
    a.Label("wp_done");
    a.db({0x5D, 0xCA, 0x0A, 0x00});              // pop bp / retf 10

    code.bytes = a.Finish();
    p.segments = {code, DataSegment(data, 0x200)};
    return p;
}

// A PeekMessage game loop: `frames` times, dispatch pending messages, then
// redraw through GetDC / PatBlt / ReleaseDC. Every pass through the pump is
// a presentation, so the loop runs at the frame cap. Exit 0.
inline NeProgram AnimationProgram(uint16_t frames) {
    NeProgram p = BaseProgram();
    p.modules = {"KERNEL", "USER", "GDI"};
    constexpr uint8_t kHinst = 0x00, kHwnd = 0x02, kCount = 0x04, kHdc = 0x06, kClass = 0x20,
                      kTitle = 0x30, kWndClass = 0x40, kMsg = 0x60;
    std::vector<uint8_t> data(0x80, 0);
    const std::string cls = "AnimWin", title = "Anim";
    std::copy(cls.begin(), cls.end(), data.begin() + kClass);
    std::copy(title.begin(), title.end(), data.begin() + kTitle);

    NeSeg code;
    Asm16 a;
    Emit e{a, code};
    e.Call(Emit::KERNEL, 91);
    a.db({0x89, 0x3E, kHinst, 0x00});
    e.RegisterClass(kWndClass, kClass, kHinst, "WndProc", 4);
    e.CreatePopup(kClass, kTitle, 0, 0, 32, 32, kHinst);
    e.StoreAx(kHwnd);
    e.Set(kCount, 0);

    a.Label("frame");
    a.Label("pump");
    e.Far(kMsg); e.Imm(0); e.Imm(0); e.Imm(0); e.Imm(1);  // PM_REMOVE
    e.Call(Emit::USER, 109);                              // PeekMessage
    a.db({0x85, 0xC0});
    a.Short(JZ, "draw");
    e.Far(kMsg);
    e.Call(Emit::USER, 114);                              // DispatchMessage
    a.Short(0xEB, "pump");
    a.Label("draw");
    e.Mem(kHwnd);
    e.Call(Emit::USER, 66);                               // GetDC
    e.StoreAx(kHdc);
    e.Mem(kHdc); e.Imm(0); e.Imm(0); e.Imm(32); e.Imm(32); e.Long(kWhiteness);
    e.Call(Emit::GDI, 29);                                // PatBlt
    e.Mem(kHwnd); e.Mem(kHdc);
    e.Call(Emit::USER, 68);                               // ReleaseDC
    a.db({0xFF, 0x06, kCount, 0x00});                     // inc word [count]
    a.db({0x81, 0x3E, kCount, 0x00}).dw(frames);         // cmp word [count], frames
    a.Short(0x73, "done");                                // jae done
    a.Near(0xE9, "frame");
    a.Label("done");
    e.Exit0();

    a.Label("WndProc");  // everything to DefWindowProc
    a.db({0x55, 0x89, 0xE5});
    e.Arg(14); e.Arg(12); e.Arg(10); e.Arg(8); e.Arg(6);
    e.Call(Emit::USER, 107);
    a.db({0x5D, 0xCA, 0x0A, 0x00});

    code.bytes = a.Finish();
    p.segments = {code, DataSegment(data, 0x200)};
    return p;
}

// An 8x8 24-bit packed DIB (RT_BITMAP resource format) in quadrants:
// red top left, green top right, blue bottom left, white bottom right.
inline std::vector<uint8_t> QuadrantDib() {
    std::vector<uint8_t> d(40 + 8 * 8 * 3, 0);
    auto put32 = [&](size_t at, uint32_t v) {
        for (int i = 0; i < 4; ++i) d[at + i] = uint8_t(v >> (8 * i));
    };
    put32(0, 40);       // biSize
    put32(4, 8);        // biWidth
    put32(8, 8);        // biHeight (bottom-up)
    d[12] = 1;          // biPlanes
    d[14] = 24;         // biBitCount
    put32(20, 8 * 8 * 3);  // biSizeImage
    for (int y = 0; y < 8; ++y) {
        for (int x = 0; x < 8; ++x) {
            const uint32_t c = y < 4 ? (x < 4 ? RGB16(255, 0, 0) : RGB16(0, 255, 0))
                                     : (x < 4 ? RGB16(0, 0, 255) : RGB16(255, 255, 255));
            const size_t at = 40 + size_t(7 - y) * 24 + size_t(x) * 3;
            d[at] = uint8_t(c >> 16);  // B, G, R
            d[at + 1] = uint8_t(c >> 8);
            d[at + 2] = uint8_t(c);
        }
    }
    return d;
}

// An RT_STRING block: 16 length-prefixed strings (ids 16*(block-1) ...).
inline std::vector<uint8_t> StringBlock(const std::vector<std::string>& strings) {
    std::vector<uint8_t> d;
    for (size_t i = 0; i < 16; ++i) {
        const std::string s = i < strings.size() ? strings[i] : "";
        d.push_back(uint8_t(s.size()));
        d.insert(d.end(), s.begin(), s.end());
    }
    return d;
}

constexpr const char* kResourceText = "Hello, Win16!";

// Resources, timers and text. Resources: bitmap "LOGO" (QuadrantDib), the
// string table (string 1 = kResourceText), and custom type "LEVELS" #3.
//   1-7  FindResource("logo", RT_BITMAP) / SizeofResource / LoadResource /
//        LockResource (reads the BITMAPINFOHEADER) / FreeResource /
//        LoadString(1) / LoadBitmap("logo")
//   8-10 a 64x48 window; SetTimer(hwnd, 1, 60 ms) posting WM_TIMER, and
//        SetTimer(NULL, ..., 100 ms, TimerProc) with a callback
//   On each WM_TIMER, through GetDC: BitBlt the bitmap to (8 * (tick - 1), 0),
//   then TextOut the string at (0, 16) in yellow on opaque blue. After 5
//   ticks: KillTimer and PostQuitMessage.
//   11-20 the tick counts, callbacks, KillTimer results, and the picture read
//   back with GetPixel (bitmap quadrants, text colours in the text band).
// Exit 0 if every check passes.
inline NeProgram ResourceProgram() {
    NeProgram p = BaseProgram();
    p.modules = {"KERNEL", "USER", "GDI"};
    p.resources = {
        {2, "", 0, "LOGO", QuadrantDib()},
        {6, "", 1, "", StringBlock({"", kResourceText})},
        {0, "LEVELS", 3, "", {1, 2, 3, 4}},
    };
    constexpr uint8_t kHinst = 0x00, kHwnd = 0x02, kRsrc = 0x04, kHglobal = 0x06, kBmp = 0x08,
                      kMemDc = 0x0A, kOldBmp = 0x0C, kTicks = 0x0E, kProcTicks = 0x10,
                      kProcMsg = 0x12, kProcTimer = 0x14, kHdc = 0x16, kLen = 0x18, kX = 0x1A,
                      kY = 0x1C, kYellow = 0x1E, kBlue = 0x20, kTextOk = 0x22, kPrev = 0x24,
                      kKill = 0x26, kCheckDc = 0x28, kClass = 0x30, kTitle = 0x40, kLogo = 0x50,
                      kWndClass = 0x60, kMsg = 0x80, kText = 0xA0;
    std::vector<uint8_t> data(0xC0, 0);
    const std::string cls = "ResWin", title = "Resources", logo = "logo";
    std::copy(cls.begin(), cls.end(), data.begin() + kClass);
    std::copy(title.begin(), title.end(), data.begin() + kTitle);
    std::copy(logo.begin(), logo.end(), data.begin() + kLogo);  // names are case-insensitive
    constexpr uint32_t kYellowRgb = RGB16(255, 255, 0), kBlueRgb = RGB16(0, 0, 255);

    NeSeg code;
    Asm16 a;
    Emit e{a, code};
    e.Call(Emit::KERNEL, 91);  // InitTask
    a.db({0x89, 0x3E, kHinst, 0x00});

    // 1. FindResource(hInst, "logo", MAKEINTRESOURCE(RT_BITMAP))
    e.Mem(kHinst); e.Far(kLogo); e.Long(2);
    e.Call(Emit::KERNEL, 60);
    a.db({0x85, 0xC0});
    FailUnless(a, JNZ, 1);
    e.StoreAx(kRsrc);
    // 2. SizeofResource = 240 (232 bytes rounded up to the 16-byte alignment)
    e.Mem(kHinst); e.Mem(kRsrc);
    e.Call(Emit::KERNEL, 65);
    a.db({0x3D}).dw(240);
    FailUnless(a, JZ, 2);
    a.db({0x85, 0xD2});  // test dx, dx
    FailUnless(a, JZ, 2);
    // 3. LoadResource
    e.Mem(kHinst); e.Mem(kRsrc);
    e.Call(Emit::KERNEL, 61);
    a.db({0x85, 0xC0});
    FailUnless(a, JNZ, 3);
    e.StoreAx(kHglobal);
    // 4. LockResource: the bytes are the DIB (biSize 40, biWidth 8)
    e.Mem(kHglobal);
    e.Call(Emit::KERNEL, 62);
    a.db({0x8E, 0xC2, 0x89, 0xC3});             // mov es, dx / mov bx, ax
    a.db({0x26, 0x81, 0x3F}).dw(40);            // cmp word es:[bx], 40
    FailUnless(a, JZ, 4);
    a.db({0x26, 0x81, 0x7F, 0x04}).dw(8);       // cmp word es:[bx+4], 8
    FailUnless(a, JZ, 4);
    // 5. FreeResource = 0
    e.Mem(kHglobal);
    e.Call(Emit::KERNEL, 63);
    a.db({0x85, 0xC0});
    FailUnless(a, JZ, 5);
    // 6. LoadString(hInst, 1, text, 32) = 13
    e.Mem(kHinst); e.Imm(1); e.Far(kText); e.Imm(32);
    e.Call(Emit::USER, 176);
    a.db({0x3D}).dw(13);
    FailUnless(a, JZ, 6);
    e.StoreAx(kLen);
    // 7. LoadBitmap(hInst, "logo"), selected into a memory DC
    e.Mem(kHinst); e.Far(kLogo);
    e.Call(Emit::USER, 175);
    a.db({0x85, 0xC0});
    FailUnless(a, JNZ, 7);
    e.StoreAx(kBmp);
    e.Imm(0);
    e.Call(Emit::GDI, 52);  // CreateCompatibleDC(NULL)
    e.StoreAx(kMemDc);
    e.Mem(kMemDc); e.Mem(kBmp);
    e.Call(Emit::GDI, 45);
    e.StoreAx(kOldBmp);
    // 8. The window
    e.RegisterClass(kWndClass, kClass, kHinst, "WndProc", 4 /* BLACK_BRUSH */);
    e.CreatePopup(kClass, kTitle, 0, 0, 64, 48, kHinst);
    a.db({0x85, 0xC0});
    FailUnless(a, JNZ, 8);
    e.StoreAx(kHwnd);
    // 9. SetTimer(hwnd, 1, 60, NULL) = 1: WM_TIMER to the window
    e.Mem(kHwnd); e.Imm(1); e.Imm(60); e.Long(0);
    e.Call(Emit::USER, 10);
    a.db({0x3D}).dw(1);
    FailUnless(a, JZ, 9);
    // 10. SetTimer(NULL, 0, 100, TimerProc): a new timer id, callback style
    e.Imm(0); e.Imm(0); e.Imm(100);
    a.db({0x0E, 0x68}).Abs16("TimerProc");      // push cs / push TimerProc
    e.Call(Emit::USER, 10);
    a.db({0x85, 0xC0});
    FailUnless(a, JNZ, 10);
    e.StoreAx(kProcTimer);

    a.Label("loop");
    e.Far(kMsg); e.Imm(0); e.Imm(0); e.Imm(0);
    e.Call(Emit::USER, 108);  // GetMessage
    a.db({0x85, 0xC0});
    a.Short(JZ, "done");
    e.Far(kMsg);
    e.Call(Emit::USER, 114);  // DispatchMessage
    a.Short(0xEB, "loop");
    a.Label("done");

    // 11. Five ticks, then KillTimer(hwnd, 1) succeeded
    a.db({0x83, 0x3E, kTicks, 0x00, 0x05});
    FailUnless(a, JZ, 11);
    a.db({0x83, 0x3E, kKill, 0x00, 0x01});
    FailUnless(a, JZ, 11);
    // 12. The TIMERPROC ran, called with (NULL, WM_TIMER, ...)
    a.db({0x83, 0x3E, kProcTicks, 0x00, 0x00});
    FailUnless(a, JNZ, 12);
    a.db({0x81, 0x3E, kProcMsg, 0x00}).dw(0x0113);
    FailUnless(a, JZ, 12);
    // 13. KillTimer(NULL, procTimer) = 1; KillTimer(hwnd, 1) again = 0
    e.Imm(0); e.Mem(kProcTimer);
    e.Call(Emit::USER, 12);
    a.db({0x3D}).dw(1);
    FailUnless(a, JZ, 13);
    e.Mem(kHwnd); e.Imm(1);
    e.Call(Emit::USER, 12);
    a.db({0x85, 0xC0});
    FailUnless(a, JZ, 13);
    // 14. Every TextOut succeeded, each on a DC whose text colour started black
    a.db({0x83, 0x3E, kTextOk, 0x00, 0x05});
    FailUnless(a, JZ, 14);

    // Read the picture back.
    e.Mem(kHwnd);
    e.Call(Emit::USER, 66);  // GetDC
    e.StoreAx(kCheckDc);
    e.CheckPixel(kCheckDc, 1, 1, RGB16(255, 0, 0), 15);      // first blit, 4 quadrants
    e.CheckPixel(kCheckDc, 6, 1, RGB16(0, 255, 0), 15);
    e.CheckPixel(kCheckDc, 1, 6, RGB16(0, 0, 255), 15);
    e.CheckPixel(kCheckDc, 6, 6, RGB16(255, 255, 255), 15);
    e.CheckPixel(kCheckDc, 33, 1, RGB16(255, 0, 0), 16);     // fifth blit at x = 32
    e.CheckPixel(kCheckDc, 38, 6, RGB16(255, 255, 255), 16);
    e.CheckPixel(kCheckDc, 41, 1, RGB16(0, 0, 0), 17);       // no sixth
    // 18. GetTextColor: black on a fresh DC, then what SetTextColor set
    e.Mem(kCheckDc);
    e.Call(Emit::GDI, 90);
    a.db({0x09, 0xD0});  // or ax, dx
    FailUnless(a, JZ, 18);
    e.Mem(kCheckDc); e.Long(kYellowRgb);
    e.Call(Emit::GDI, 9);
    e.Mem(kCheckDc);
    e.Call(Emit::GDI, 90);
    a.db({0x3D}).dw(uint16_t(kYellowRgb));
    FailUnless(a, JZ, 18);
    a.db({0x81, 0xFA}).dw(uint16_t(kYellowRgb >> 16));
    FailUnless(a, JZ, 18);
    // 19. The text band (0,16)-(64,32) has yellow glyph pixels on blue
    e.Set(kY, 16);
    a.Label("scan_y");
    e.Set(kX, 0);
    a.Label("scan_x");
    e.Mem(kCheckDc); e.Mem(kX); e.Mem(kY);
    e.Call(Emit::GDI, 83);  // GetPixel
    a.db({0x3D}).dw(uint16_t(kYellowRgb));
    a.Short(JNZ, "not_yellow");
    a.db({0x81, 0xFA}).dw(uint16_t(kYellowRgb >> 16));
    a.Short(JNZ, "not_yellow");
    a.db({0xFF, 0x06, kYellow, 0x00});  // inc word [yellow]
    a.Short(0xEB, "scan_next");
    a.Label("not_yellow");
    a.db({0x3D}).dw(uint16_t(kBlueRgb & 0xFFFF));
    a.Short(JNZ, "scan_next");
    a.db({0x81, 0xFA}).dw(uint16_t(kBlueRgb >> 16));
    a.Short(JNZ, "scan_next");
    a.db({0xFF, 0x06, kBlue, 0x00});    // inc word [blue]
    a.Label("scan_next");
    a.db({0xFF, 0x06, kX, 0x00});       // inc word [x]
    a.db({0x83, 0x3E, kX, 0x00, 64});   // cmp word [x], 64
    a.Short(0x72, "scan_x");            // jb
    a.db({0xFF, 0x06, kY, 0x00});
    a.db({0x83, 0x3E, kY, 0x00, 32});
    a.Short(0x72, "scan_y");
    a.db({0x83, 0x3E, kYellow, 0x00, 0x00});
    FailUnless(a, JNZ, 19);
    a.db({0x83, 0x3E, kBlue, 0x00, 0x00});
    FailUnless(a, JNZ, 19);
    e.Mem(kHwnd); e.Mem(kCheckDc);
    e.Call(Emit::USER, 68);  // ReleaseDC
    // 20. Clean up: the bitmap can be deleted once deselected
    e.Mem(kMemDc); e.Mem(kOldBmp);
    e.Call(Emit::GDI, 45);
    e.Mem(kBmp);
    e.Call(Emit::GDI, 69);
    a.db({0x85, 0xC0});
    FailUnless(a, JNZ, 20);
    e.Mem(kMemDc);
    e.Call(Emit::GDI, 68);
    e.Exit0();
    e.FailStubs(20);

    // WndProc: [bp+14] hwnd, [bp+12] msg, [bp+10] wParam, [bp+8]:[bp+6] lParam
    a.Label("WndProc");
    a.db({0x55, 0x89, 0xE5, 0x8B, 0x46, 0x0C});  // push bp / mov bp,sp / mov ax,[bp+12]
    a.db({0x3D}).dw(0x0113);                     // WM_TIMER?
    a.Short(JZ, "wp_timer");
    e.Arg(14); e.Arg(12); e.Arg(10); e.Arg(8); e.Arg(6);
    e.Call(Emit::USER, 107);                     // DefWindowProc
    a.Near(0xE9, "wp_done");

    a.Label("wp_timer");
    a.db({0xFF, 0x06, kTicks, 0x00});            // inc word [ticks]
    e.Arg(14);
    e.Call(Emit::USER, 66);                      // GetDC
    e.StoreAx(kHdc);
    e.Mem(kHdc);
    a.db({0xA1, kTicks, 0x00, 0x48});            // mov ax, [ticks] / dec ax
    a.db({0xC1, 0xE0, 0x03, 0x50});              // shl ax, 3 / push ax
    e.Imm(0); e.Imm(8); e.Imm(8); e.Mem(kMemDc); e.Imm(0); e.Imm(0); e.Long(kSrcCopy);
    e.Call(Emit::GDI, 34);                       // BitBlt
    e.Mem(kHdc); e.Long(kYellowRgb);
    e.Call(Emit::GDI, 9);                        // SetTextColor -> previous
    a.db({0x09, 0xD0});                          // or ax, dx (0: it was black)
    e.StoreAx(kPrev);
    e.Mem(kHdc); e.Long(kBlueRgb);
    e.Call(Emit::GDI, 1);                        // SetBkColor
    e.Mem(kHdc); e.Imm(2);
    e.Call(Emit::GDI, 2);                        // SetBkMode(OPAQUE)
    e.Mem(kHdc); e.Imm(0); e.Imm(16); e.Far(kText); e.Mem(kLen);
    e.Call(Emit::GDI, 33);                       // TextOut
    a.db({0x3D}).dw(1);
    a.Short(JNZ, "wp_release");
    a.db({0x83, 0x3E, kPrev, 0x00, 0x00});
    a.Short(JNZ, "wp_release");
    a.db({0xFF, 0x06, kTextOk, 0x00});           // inc word [textOk]
    a.Label("wp_release");
    e.Arg(14); e.Mem(kHdc);
    e.Call(Emit::USER, 68);                      // ReleaseDC
    a.db({0x83, 0x3E, kTicks, 0x00, 0x05});      // cmp word [ticks], 5
    a.Short(0x72, "wp_zero");                    // jb
    e.Arg(14); e.Imm(1);
    e.Call(Emit::USER, 12);                      // KillTimer(hwnd, 1)
    e.StoreAx(kKill);
    e.Imm(0);
    e.Call(Emit::USER, 6);                       // PostQuitMessage(0)
    a.Label("wp_zero");
    a.db({0x31, 0xC0, 0x31, 0xD2});
    a.Label("wp_done");
    a.db({0x5D, 0xCA, 0x0A, 0x00});              // pop bp / retf 10

    // TimerProc(hwnd, msg, idEvent, dwTime): same frame layout as WndProc.
    a.Label("TimerProc");
    a.db({0x55, 0x89, 0xE5});
    a.db({0xFF, 0x06, kProcTicks, 0x00});        // inc word [procTicks]
    a.db({0x8B, 0x46, 0x0C});                    // mov ax, [bp+12]
    e.StoreAx(kProcMsg);
    a.db({0x83, 0x7E, 0x0E, 0x00});              // cmp word [bp+14], 0 (hwnd)
    a.Short(JZ, "tp_done");
    e.Set(kProcMsg, 0xFFFF);                     // wrong hwnd: fails check 12
    a.Label("tp_done");
    a.db({0x5D, 0xCA, 0x0A, 0x00});              // pop bp / retf 10

    code.bytes = a.Finish();
    p.segments = {code, DataSegment(data, 0x200)};
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

// Calls an API the engine doesn't have yet (USER.7 = ExitWindows).
inline NeProgram UnimplementedApiProgram() {
    NeProgram p = BaseProgram();
    NeSeg code;
    Asm16 a;
    code.relocs.push_back(ImportOrdinal(a.CallFar(), 2, 7));
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

// Calls module.ordinal (a DLL the engine doesn't implement) with four words
// of arguments, then exits with 51 if the call returned.
inline NeProgram MissingModuleProgram(const std::string& module = "SHELL", uint16_t ordinal = 22) {
    NeProgram p = BaseProgram();
    p.modules = {"KERNEL", module};
    NeSeg code;
    Asm16 a;
    a.db({0x6A, 0x00, 0x1E, 0x6A, 0x00, 0x1E, 0x6A, 0x00, 0x6A, 0x00});  // push 0 / ds:0 / ds:0 / 0
    code.relocs.push_back(ImportOrdinal(a.CallFar(), 2, ordinal));
    a.db({0xB8, 51, 0x4C, 0xCD, 0x21});  // exit(51)
    code.bytes = a.Finish();
    p.segments = {code, DataSegment({}, 0x100)};
    return p;
}

// What a C runtime's startup and a typical game's initialization do with
// KERNEL, run against a program directory holding CRT.INI ([Game] Level=7,
// Name=Sunny) and CRTDATA.BIN ("RETRO16!"). Exit 0, or the failed check:
//   1 InitTask            2 WIN87EM __fpMath init     3 __AHINCR / __WINFLAGS
//   4-6 local heap: moveable blocks (handle -> pointer), growth past the
//       initial heap, fixed blocks          7 GlobalReAlloc to 40000 bytes
//   8 lstrcpy/lstrcat/lstrlen   9-10 private profile reads, a write read back
//   11 _lopen/_lread/_llseek   12 files outside the directory / for writing
//   13 INT 21h open/read/close  14 GetModuleHandle/GetProcAddress (WINHELP: missing)
//   15 GetModuleFileName, GetDOSEnvironment   16 LoadLibrary
// It imports SHELL.ShellAbout but never calls it.
inline NeProgram CrtProgram() {
    NeProgram p = BaseProgram();
    p.modules = {"KERNEL", "USER", "WIN87EM", "SHELL"};
    constexpr uint8_t kHinst = 0x00, kH = 0x02, kP = 0x04, kG = 0x06, kFile = 0x08, kUser = 0x0A,
                      kIni = 0x10, kGame = 0x18, kLevel = 0x20, kName = 0x28, kMissing = 0x30,
                      kScore = 0x38, k99 = 0x40, kX = 0x44, kData = 0x48, kSecret = 0x58, kUserName = 0x68,
                      kGetMessage = 0x70, kDialogBox = 0x7C, kMmsystem = 0x88, kNoSuch = 0x98, kAbc = 0xA4,
                      kDef = 0xA8, kBuf = 0xB0;
    std::vector<uint8_t> data(0x100, 0);
    auto put = [&](uint8_t at, const std::string& s) { std::copy(s.begin(), s.end(), data.begin() + at); };
    put(kIni, "CRT.INI");
    put(kGame, "Game");
    put(kLevel, "Level");
    put(kName, "Name");
    put(kMissing, "Missing");
    put(kScore, "Score");
    put(k99, "99");
    put(kX, "x");
    put(kData, "CRTDATA.BIN");
    put(kSecret, "..\\SECRET.TXT");
    put(kUserName, "USER");
    put(kGetMessage, "GETMESSAGE");
    put(kDialogBox, "WINHELP");
    put(kMmsystem, "MMSYSTEM.DLL");
    put(kNoSuch, "NOSUCH.DLL");
    put(kAbc, "abc");
    put(kDef, "def");

    NeSeg code;
    Asm16 a;
    Emit e{a, code};
    constexpr uint16_t WIN87EM = 3, SHELL = 4;
    auto cmpAx = [&](uint16_t v) { a.db({0x3D}).dw(v); };
    auto dxZero = [&](int fail) { a.db({0x85, 0xD2}); FailUnless(a, JZ, fail); };

    // 1. InitTask
    e.Call(Emit::KERNEL, 91);
    a.db({0x85, 0xC0});
    FailUnless(a, JNZ, 1);
    a.db({0x89, 0x3E, kHinst, 0x00});
    // 2. __fpMath(BX = 0): initialize the emulator
    a.db({0x31, 0xDB});  // xor bx, bx
    e.Call(WIN87EM, 1);
    a.db({0x85, 0xC0});
    FailUnless(a, JZ, 2);
    // 3. Imported constants: mov ax, __AHINCR (8); mov ax, __WINFLAGS; GetWinFlags()
    a.db({0xB8});
    code.relocs.push_back({5, 1, a.Here(), 1, 114});  // offset fixup to KERNEL.114
    a.db({0xFF, 0xFF});
    cmpAx(8);
    FailUnless(a, JZ, 3);
    a.db({0xB8});
    code.relocs.push_back({5, 1, a.Here(), 1, 178});  // KERNEL.178 __WINFLAGS
    a.db({0xFF, 0xFF});
    cmpAx(0x0013);
    FailUnless(a, JZ, 3);
    e.Call(Emit::KERNEL, 132);  // GetWinFlags
    cmpAx(0x0013);
    FailUnless(a, JZ, 3);

    // 4. LocalAlloc(LMEM_MOVEABLE, 100); LocalLock; *handle == pointer
    e.Imm(0x0002); e.Imm(100);
    e.Call(Emit::KERNEL, 5);
    a.db({0x85, 0xC0});
    FailUnless(a, JNZ, 4);
    e.StoreAx(kH);
    e.Mem(kH);
    e.Call(Emit::KERNEL, 8);  // LocalLock
    a.db({0x85, 0xC0});
    FailUnless(a, JNZ, 4);
    e.StoreAx(kP);
    a.db({0x8B, 0x1E, kH, 0x00, 0x8B, 0x07});  // mov bx, [h] / mov ax, [bx]
    a.db({0x3B, 0x06, kP, 0x00});              // cmp ax, [p]
    FailUnless(a, JZ, 4);
    a.db({0x8B, 0x1E, kP, 0x00, 0xC6, 0x07, 0x5A});  // mov bx, [p] / mov byte [bx], 5Ah
    e.Mem(kH);
    e.Call(Emit::KERNEL, 9);  // LocalUnlock
    // 5. LocalReAlloc(h, 3000, LMEM_MOVEABLE): past the initial 256-byte heap
    e.Mem(kH); e.Imm(3000); e.Imm(0x0002);
    e.Call(Emit::KERNEL, 6);
    a.db({0x3B, 0x06, kH, 0x00});  // same handle
    FailUnless(a, JZ, 5);
    e.Mem(kH);
    e.Call(Emit::KERNEL, 8);  // LocalLock
    a.db({0x89, 0xC3, 0x80, 0x3F, 0x5A});  // mov bx, ax / cmp byte [bx], 5Ah (kept)
    FailUnless(a, JZ, 5);
    a.db({0xC6, 0x87}).dw(2999).db({0x77});  // mov byte [bx+2999], 77h (inside DGROUP now)
    e.Mem(kH);
    e.Call(Emit::KERNEL, 10);  // LocalSize
    cmpAx(3000);
    FailUnless(a, JZ, 5);
    e.Mem(kH);
    e.Call(Emit::KERNEL, 9);  // LocalUnlock
    e.Mem(kH);
    e.Call(Emit::KERNEL, 7);  // LocalFree
    a.db({0x85, 0xC0});
    FailUnless(a, JZ, 5);
    // 6. LocalAlloc(LMEM_FIXED, 16): the handle is the pointer
    e.Imm(0); e.Imm(16);
    e.Call(Emit::KERNEL, 5);
    a.db({0x85, 0xC0});
    FailUnless(a, JNZ, 6);
    e.StoreAx(kP);
    e.Mem(kP);
    e.Call(Emit::KERNEL, 11);  // LocalHandle
    a.db({0x3B, 0x06, kP, 0x00});
    FailUnless(a, JZ, 6);
    e.Mem(kP);
    e.Call(Emit::KERNEL, 7);
    a.db({0x85, 0xC0});
    FailUnless(a, JZ, 6);

    // 7. GlobalAlloc(GMEM_MOVEABLE, 16) -> GlobalReAlloc(40000) keeps the handle
    e.Imm(0x0002); e.Long(16);
    e.Call(Emit::KERNEL, 15);
    e.StoreAx(kG);
    e.Mem(kG); e.Long(40000); e.Imm(0x0002);
    e.Call(Emit::KERNEL, 16);
    a.db({0x3B, 0x06, kG, 0x00});
    FailUnless(a, JZ, 7);
    e.Mem(kG);
    e.Call(Emit::KERNEL, 20);  // GlobalSize
    cmpAx(40000);
    FailUnless(a, JZ, 7);
    dxZero(7);

    // 8. lstrcpy(buf, "abc"); lstrcat(buf, "def"); lstrlen(buf) = 6
    e.Far(kBuf); e.Far(kAbc);
    e.Call(Emit::KERNEL, 88);
    e.Far(kBuf); e.Far(kDef);
    e.Call(Emit::KERNEL, 89);
    e.Far(kBuf);
    e.Call(Emit::KERNEL, 90);
    cmpAx(6);
    FailUnless(a, JZ, 8);

    // 9. GetPrivateProfileInt("Game", "Level", 1, "CRT.INI") = 7; "Missing" -> 42
    e.Far(kGame); e.Far(kLevel); e.Imm(1); e.Far(kIni);
    e.Call(Emit::KERNEL, 127);
    cmpAx(7);
    FailUnless(a, JZ, 9);
    e.Far(kGame); e.Far(kMissing); e.Imm(42); e.Far(kIni);
    e.Call(Emit::KERNEL, 127);
    cmpAx(42);
    FailUnless(a, JZ, 9);
    //    GetPrivateProfileString("Game", "Name", "x", buf, 32, "CRT.INI") = 5 ("Sunny")
    e.Far(kGame); e.Far(kName); e.Far(kX); e.Far(kBuf); e.Imm(32); e.Far(kIni);
    e.Call(Emit::KERNEL, 128);
    cmpAx(5);
    FailUnless(a, JZ, 9);
    a.db({0x80, 0x3E, kBuf, 0x00, 'S'});
    FailUnless(a, JZ, 9);
    // 10. WritePrivateProfileString("Game", "Score", "99"), read back as 99
    e.Far(kGame); e.Far(kScore); e.Far(k99); e.Far(kIni);
    e.Call(Emit::KERNEL, 129);
    e.Far(kGame); e.Far(kScore); e.Imm(0); e.Far(kIni);
    e.Call(Emit::KERNEL, 127);
    cmpAx(99);
    FailUnless(a, JZ, 10);

    // 11. _lopen("CRTDATA.BIN", OF_READ); _lread 16 -> 8 bytes; _llseek(2); _lread 1 -> 'T'
    e.Far(kData); e.Imm(0);
    e.Call(Emit::KERNEL, 85);
    cmpAx(0xFFFF);
    FailUnless(a, JNZ, 11);
    e.StoreAx(kFile);
    e.Mem(kFile); e.Far(kBuf); e.Imm(16);
    e.Call(Emit::KERNEL, 82);
    cmpAx(8);
    FailUnless(a, JZ, 11);
    a.db({0x80, 0x3E, kBuf, 0x00, 'R'});
    FailUnless(a, JZ, 11);
    e.Mem(kFile); e.Long(2); e.Imm(0);
    e.Call(Emit::KERNEL, 84);
    cmpAx(2);
    FailUnless(a, JZ, 11);
    e.Mem(kFile); e.Far(kBuf); e.Imm(1);
    e.Call(Emit::KERNEL, 82);
    a.db({0x80, 0x3E, kBuf, 0x00, 'T'});
    FailUnless(a, JZ, 11);
    e.Mem(kFile);
    e.Call(Emit::KERNEL, 81);  // _lclose
    a.db({0x85, 0xC0});
    FailUnless(a, JZ, 11);
    // 12. "..\SECRET.TXT" is outside; opening for writing is refused
    e.Far(kSecret); e.Imm(0);
    e.Call(Emit::KERNEL, 85);
    cmpAx(0xFFFF);
    FailUnless(a, JZ, 12);
    e.Far(kData); e.Imm(2);  // OF_READWRITE
    e.Call(Emit::KERNEL, 85);
    cmpAx(0xFFFF);
    FailUnless(a, JZ, 12);

    // 13. INT 21h: open (3Dh), read 4 bytes (3Fh), close (3Eh)
    a.db({0xBA, kData, 0x00, 0xB8, 0x00, 0x3D, 0xCD, 0x21});  // mov dx, data / mov ax, 3D00h / int 21h
    FailUnless(a, 0x73 /* JNC */, 13);
    a.db({0x89, 0xC3, 0xB4, 0x3F, 0xB9, 0x04, 0x00, 0xBA, kBuf, 0x00, 0xCD, 0x21});  // read 4 to buf
    FailUnless(a, 0x73, 13);
    cmpAx(4);
    FailUnless(a, JZ, 13);
    a.db({0xB4, 0x3E, 0xCD, 0x21});  // close BX
    FailUnless(a, 0x73, 13);

    // 14. GetModuleHandle("USER"); GetProcAddress: GETMESSAGE yes, WINHELP (not implemented) no
    e.Far(kUserName);
    e.Call(Emit::KERNEL, 47);
    a.db({0x85, 0xC0});
    FailUnless(a, JNZ, 14);
    e.StoreAx(kUser);
    e.Mem(kUser); e.Far(kGetMessage);
    e.Call(Emit::KERNEL, 50);
    a.db({0x09, 0xD0});  // or ax, dx
    FailUnless(a, JNZ, 14);
    e.Mem(kUser); e.Far(kDialogBox);
    e.Call(Emit::KERNEL, 50);
    a.db({0x09, 0xD0});
    FailUnless(a, JZ, 14);

    // 15. GetModuleFileName(hInst, buf, 64) > 0; GetDOSEnvironment starts with "PATH="
    e.Mem(kHinst); e.Far(kBuf); e.Imm(64);
    e.Call(Emit::KERNEL, 49);
    a.db({0x85, 0xC0});
    FailUnless(a, JNZ, 15);
    e.Call(Emit::KERNEL, 131);
    a.db({0x8E, 0xC2, 0x89, 0xC3});             // mov es, dx / mov bx, ax
    a.db({0x26, 0x80, 0x3F, 'P'});              // cmp byte es:[bx], 'P'
    FailUnless(a, JZ, 15);

    // 16. LoadLibrary: MMSYSTEM.DLL (a stub module) >= 32, NOSUCH.DLL < 32
    e.Far(kMmsystem);
    e.Call(Emit::KERNEL, 95);
    cmpAx(32);
    FailUnless(a, 0x73 /* JAE */, 16);
    e.Far(kNoSuch);
    e.Call(Emit::KERNEL, 95);
    cmpAx(32);
    FailUnless(a, JC /* JB */, 16);

    e.Exit0();
    // Never reached: ShellAbout(hwnd, app, other, icon), importing SHELL.
    e.Imm(0); e.Far(kX); e.Far(kX); e.Imm(0);
    e.Call(SHELL, 22);
    e.FailStubs(16);

    code.bytes = a.Finish();
    p.segments = {code, DataSegment(data, 0x100)};
    return p;
}

// Menu template (RT_MENU): "&Game" popup {"&New\tF2" 100, "E&xit" 101}, "&Help" 200.
inline std::vector<uint8_t> GameMenuTemplate() {
    std::vector<uint8_t> d = {0, 0, 0, 0};  // version 0, no extra header
    auto item = [&](uint16_t flags, int id, const std::string& text) {
        d.push_back(uint8_t(flags));
        d.push_back(uint8_t(flags >> 8));
        if (id >= 0) {
            d.push_back(uint8_t(id));
            d.push_back(uint8_t(id >> 8));
        }
        d.insert(d.end(), text.begin(), text.end());
        d.push_back(0);
    };
    item(0x0010, -1, "&Game");          // MF_POPUP
    item(0x0000, 100, "&New\tF2");
    item(0x0080, 101, "E&xit");         // MF_END: last in the popup
    item(0x0080, 200, "&Help");         // MF_END: last at the top
    return d;
}

// USER breadth, GDI drawing and sound, in a 64x48 window that becomes
// fullscreen. Exit 0, or the failed check:
//   1 window words (cbWndExtra 4)   2 GetWindowRect, ClientToScreen
//   3 MoveWindow to 640x480: WM_SIZE, GetClientRect   4 activation and focus
//   5 SetWindowText / GetWindowTextLength   6-7 menus from RT_MENU
//   8 wsprintf   9 CreateFont, GetTextMetrics, GetTextExtent, DrawText(DT_CALCRECT)
//   10 GetDeviceCaps   11 GetObject(bitmap)   12 Ellipse, MoveTo/LineTo
//   13 StretchDIBits (a 2x2 DIB, 4x)   14 GetSysColor   15 rectangles
//   16 cursor and capture   17 MessageBox default button, DialogBox
//   18 sound: SOUND.DRV silent, no wave devices, missing WAV
//   19 timeSetEvent callbacks -> WM_USER -> a posted F2 -> TranslateAccelerator
//      -> WM_COMMAND 100 -> quit   20 the timer callback's dwUser
inline NeProgram UiProgram() {
    NeProgram p = BaseProgram();
    p.modules = {"KERNEL", "USER", "GDI", "MMSYSTEM", "SOUND"};
    std::vector<uint8_t> dib(40 + 16, 0);  // 2x2, 24-bit, bottom-up
    dib[0] = 40;
    dib[4] = 2;
    dib[8] = 2;
    dib[12] = 1;
    dib[14] = 24;
    const uint8_t bits[16] = {0, 255, 0, 255, 0, 0, 0, 0,          // bottom row: green, blue (BGR)
                              0, 0, 255, 255, 255, 255, 0, 0};     // top row: red, white
    std::copy(std::begin(bits), std::end(bits), dib.begin() + 40);
    p.resources = {
        {4, "", 1, "", GameMenuTemplate()},
        {9, "", 1, "", {0x81, 0x71, 0x00, 100, 0x00}},  // VK_F2 -> 100 (FVIRTKEY, last)
        {5, "", 0, "ABOUT", std::vector<uint8_t>(16, 0)},
    };
    constexpr uint16_t kHinst = 0x00, kHwnd = 0x02, kMenu = 0x04, kAccel = 0x06, kHdc = 0x08, kFont = 0x0A,
                       kBrush = 0x0C, kBmp = 0x0E, kTicks = 0x12, kSizeLo = 0x14, kSizeHi = 0x16,
                       kActivated = 0x18, kCommand = 0x1A, kUserSeen = 0x1C, kBadUser = 0x1E,
                       kClass = 0x20, kTitle = 0x28, kRenamed = 0x2C, kNoWav = 0x34, kAbout = 0x40,
                       kQuit = 0x48, kHi = 0x50, kFace = 0x54, kWndClass = 0x60, kPoint = 0x7C, kMsg = 0x80,
                       kRect = 0x94, kTm = 0xA0, kObj = 0xC0, kFmt = 0xD0, kOk = 0xE4, kExpected = 0xE8,
                       kBuf = 0x100, kDib = 0x140;
    std::vector<uint8_t> data(0x180, 0);
    auto put = [&](uint16_t at, const std::string& s) { std::copy(s.begin(), s.end(), data.begin() + at); };
    put(kClass, "UiWin");
    put(kTitle, "UI");
    put(kRenamed, "Renamed");
    put(kNoWav, "NOSUCH.WAV");
    put(kAbout, "ABOUT");
    put(kQuit, "Quit?");
    put(kHi, "Hi");
    put(kFace, "Courier New");
    put(kFmt, "%d-%04x-%s-%c-%ld");
    put(kOk, "ok");
    put(kExpected, "-5-00ab-ok-Z-70000");
    std::copy(dib.begin(), dib.end(), data.begin() + kDib);

    NeSeg code;
    Asm16 a;
    Emit e{a, code};
    constexpr uint16_t MMSYSTEM = 4, SOUND = 5;
    auto ok = [&](int fail) { a.db({0x85, 0xC0}); FailUnless(a, JNZ, fail); };  // AX != 0
    auto eq = [&](uint16_t v, int fail) { e.CmpAx(v); FailUnless(a, JZ, fail); };

    e.Call(Emit::KERNEL, 91);
    a.db({0x89, 0x3E, kHinst, 0x00});
    e.RegisterClass(kWndClass, kClass, kHinst, "WndProc", 4 /* BLACK_BRUSH */, 4);
    e.CreatePopup(kClass, kTitle, 10, 20, 64, 48, kHinst);
    ok(1);
    e.StoreAx(kHwnd);

    // 1. SetWindowWord(hwnd, 0, 1234h) = 0; GetWindowWord = 1234h; GetWindowLong(0) = 00001234h
    e.Mem(kHwnd); e.Imm(0); e.Imm(0x1234);
    e.Call(Emit::USER, 134);
    eq(0, 1);
    e.Mem(kHwnd); e.Imm(0);
    e.Call(Emit::USER, 133);
    eq(0x1234, 1);
    e.Mem(kHwnd); e.Imm(0);
    e.Call(Emit::USER, 135);
    eq(0x1234, 1);
    a.db({0x85, 0xD2});
    FailUnless(a, JZ, 1);
    // 2. GetWindowRect = (10, 20, 74, 68); ClientToScreen(5, 5) = (15, 25)
    e.Mem(kHwnd); e.Far(kRect);
    e.Call(Emit::USER, 32);
    e.CmpMem(kRect, 10); FailUnless(a, JZ, 2);
    e.CmpMem(kRect + 6, 68); FailUnless(a, JZ, 2);
    e.Set(kPoint, 5); e.Set(kPoint + 2, 5);
    e.Mem(kHwnd); e.Far(kPoint);
    e.Call(Emit::USER, 28);
    e.CmpMem(kPoint, 15); FailUnless(a, JZ, 2);
    e.CmpMem(kPoint + 2, 25); FailUnless(a, JZ, 2);
    // 3. MoveWindow(hwnd, 0, 0, 640, 480, TRUE): WM_SIZE 640x480, GetClientRect
    e.Mem(kHwnd); e.Imm(0); e.Imm(0); e.Imm(640); e.Imm(480); e.Imm(1);
    e.Call(Emit::USER, 56);
    ok(3);
    e.CmpMem(kSizeLo, 640); FailUnless(a, JZ, 3);
    e.CmpMem(kSizeHi, 480); FailUnless(a, JZ, 3);
    e.Mem(kHwnd); e.Far(kRect);
    e.Call(Emit::USER, 33);
    e.CmpMem(kRect + 4, 640); FailUnless(a, JZ, 3);
    // 4. Shown at creation: WM_ACTIVATEAPP seen, active and focused
    e.CmpMem(kActivated, 1); FailUnless(a, JZ, 4);
    e.Call(Emit::USER, 60);  // GetActiveWindow
    a.db({0x3B, 0x06}).dw(kHwnd); FailUnless(a, JZ, 4);
    e.Call(Emit::USER, 23);  // GetFocus
    a.db({0x3B, 0x06}).dw(kHwnd); FailUnless(a, JZ, 4);
    // 5. SetWindowText("Renamed"); GetWindowTextLength = 7
    e.Mem(kHwnd); e.Far(kRenamed);
    e.Call(Emit::USER, 37);
    e.Mem(kHwnd);
    e.Call(Emit::USER, 38);
    eq(7, 5);
    // 6. LoadMenu(#1), SetMenu, GetMenu; 2 items, a popup first
    e.Mem(kHinst); e.Long(1);
    e.Call(Emit::USER, 150);
    ok(6);
    e.StoreAx(kMenu);
    e.Mem(kHwnd); e.Mem(kMenu);
    e.Call(Emit::USER, 158);
    ok(6);
    e.Mem(kHwnd);
    e.Call(Emit::USER, 157);
    a.db({0x3B, 0x06}).dw(kMenu); FailUnless(a, JZ, 6);
    e.Mem(kMenu);
    e.Call(Emit::USER, 263);  // GetMenuItemCount
    eq(2, 6);
    e.Mem(kMenu); e.Imm(0);
    e.Call(Emit::USER, 159);  // GetSubMenu
    ok(6);
    // 7. CheckMenuItem(101, MF_CHECKED) = 0 then GetMenuState has MF_CHECKED; EnableMenuItem(200, MF_GRAYED) = 0
    e.Mem(kMenu); e.Imm(101); e.Imm(0x0008);
    e.Call(Emit::USER, 154);
    eq(0, 7);
    e.Mem(kMenu); e.Imm(101); e.Imm(0);
    e.Call(Emit::USER, 250);
    eq(0x0008, 7);
    e.Mem(kMenu); e.Imm(200); e.Imm(0x0001);
    e.Call(Emit::USER, 155);
    eq(0, 7);

    // 8. wsprintf(buf, "%d-%04x-%s-%c-%ld", -5, 0xAB, "ok", 'Z', 70000L) = "-5-00ab-ok-Z-70000"
    e.Long(70000); e.Imm('Z'); e.Far(kOk); e.Imm(0xAB); e.Imm(uint16_t(-5)); e.Far(kFmt); e.Far(kBuf);
    e.Call(Emit::USER, 420);
    a.db({0x83, 0xC4, 22});  // add sp, 22: C convention, the caller removes the arguments
    eq(18, 8);
    a.db({0x1E, 0x07, 0xBE}).dw(kBuf).db({0xBF}).dw(kExpected).db({0xB9, 19, 0, 0xFC, 0xF3, 0xA6});
    FailUnless(a, JZ, 8);  // push ds / pop es / mov si / mov di / mov cx, 19 / cld / repe cmpsb

    // Drawing, through a window DC.
    e.Mem(kHwnd);
    e.Call(Emit::USER, 66);
    e.StoreAx(kHdc);
    // 9. CreateFont(-16, ..., "Courier New"); GetTextMetrics; GetTextExtent("Hi"); DrawText(DT_CALCRECT)
    e.Imm(uint16_t(-16));
    for (int i = 0; i < 3; ++i) e.Imm(0);
    e.Imm(400);
    for (int i = 0; i < 8; ++i) e.Imm(0);
    e.Far(kFace);
    e.Call(Emit::GDI, 56);
    ok(9);
    e.StoreAx(kFont);
    e.Mem(kHdc); e.Mem(kFont);
    e.Call(Emit::GDI, 45);
    e.Mem(kHdc); e.Far(kTm);
    e.Call(Emit::GDI, 93);
    ok(9);
    e.CmpMem(kTm, 0); FailUnless(a, 0x7F /* JG */, 9);  // tmHeight > 0
    e.Mem(kHdc); e.Far(kHi); e.Imm(2);
    e.Call(Emit::GDI, 91);
    ok(9);   // cx
    a.db({0x85, 0xD2}); FailUnless(a, JNZ, 9);  // cy
    e.Set(kRect, 0); e.Set(kRect + 2, 0); e.Set(kRect + 4, 0); e.Set(kRect + 6, 0);
    e.Mem(kHdc); e.Far(kHi); e.Imm(0xFFFF); e.Far(kRect); e.Imm(0x0400);
    e.Call(Emit::USER, 85);
    ok(9);
    e.CmpMem(kRect + 4, 0); FailUnless(a, JNZ, 9);  // right grew
    // 10. GetDeviceCaps: HORZRES 640, BITSPIXEL 24
    e.Mem(kHdc); e.Imm(8);
    e.Call(Emit::GDI, 80);
    eq(640, 10);
    e.Mem(kHdc); e.Imm(12);
    e.Call(Emit::GDI, 80);
    eq(24, 10);
    // 11. GetObject(CreateCompatibleBitmap(hdc, 8, 4)): 14 bytes, 8 x 4
    e.Mem(kHdc); e.Imm(8); e.Imm(4);
    e.Call(Emit::GDI, 51);
    ok(11);
    e.StoreAx(kBmp);
    e.Mem(kBmp); e.Imm(14); e.Far(kObj);
    e.Call(Emit::GDI, 82);
    eq(14, 11);
    e.CmpMem(kObj + 2, 8); FailUnless(a, JZ, 11);
    e.CmpMem(kObj + 4, 4); FailUnless(a, JZ, 11);
    e.Mem(kBmp);
    e.Call(Emit::GDI, 69);
    // 12. Ellipse with a red brush; a white line with MoveTo/LineTo
    e.Long(RGB16(255, 0, 0));
    e.Call(Emit::GDI, 66);
    e.StoreAx(kBrush);
    e.Mem(kHdc); e.Mem(kBrush);
    e.Call(Emit::GDI, 45);
    e.Mem(kHdc); e.Imm(200); e.Imm(200); e.Imm(240); e.Imm(240);
    e.Call(Emit::GDI, 24);
    ok(12);
    e.Imm(6);               // WHITE_PEN
    e.Call(Emit::GDI, 87);
    e.StoreAx(kBmp);
    e.Mem(kHdc); e.Mem(kBmp);
    e.Call(Emit::GDI, 45);
    e.Mem(kHdc); e.Imm(300); e.Imm(10);
    e.Call(Emit::GDI, 20);  // MoveTo
    e.Mem(kHdc); e.Imm(340); e.Imm(10);
    e.Call(Emit::GDI, 19);  // LineTo
    ok(12);
    e.CheckPixel(uint8_t(kHdc), 220, 220, RGB16(255, 0, 0), 12);
    e.CheckPixel(uint8_t(kHdc), 320, 10, RGB16(255, 255, 255), 12);
    // 13. StretchDIBits: the 2x2 DIB at (100,100), 8x8
    e.Mem(kHdc); e.Imm(100); e.Imm(100); e.Imm(8); e.Imm(8); e.Imm(0); e.Imm(0); e.Imm(2); e.Imm(2);
    e.Far(kDib + 40); e.Far(kDib); e.Imm(0); e.Long(kSrcCopy);
    e.Call(Emit::GDI, 439);
    eq(2, 13);
    e.CheckPixel(uint8_t(kHdc), 101, 101, RGB16(255, 0, 0), 13);
    e.CheckPixel(uint8_t(kHdc), 105, 101, RGB16(255, 255, 255), 13);
    e.CheckPixel(uint8_t(kHdc), 101, 105, RGB16(0, 255, 0), 13);
    e.CheckPixel(uint8_t(kHdc), 105, 105, RGB16(0, 0, 255), 13);
    e.Mem(kHwnd); e.Mem(kHdc);
    e.Call(Emit::USER, 68);  // ReleaseDC
    // 14. GetSysColor(COLOR_BTNFACE) = C0C0C0h (Windows 3.1)
    e.Imm(15);
    e.Call(Emit::USER, 180);
    eq(0xC0C0, 14);
    a.db({0x81, 0xFA}).dw(0x00C0); FailUnless(a, JZ, 14);
    // 15. SetRect(1,2,3,4), InflateRect(1,1) -> (0,1,4,5); PtInRect((3,4)) = TRUE
    e.Far(kRect); e.Imm(1); e.Imm(2); e.Imm(3); e.Imm(4);
    e.Call(Emit::USER, 72);
    e.Far(kRect); e.Imm(1); e.Imm(1);
    e.Call(Emit::USER, 78);
    e.CmpMem(kRect, 0); FailUnless(a, JZ, 15);
    e.CmpMem(kRect + 6, 5); FailUnless(a, JZ, 15);
    e.Far(kRect); e.Long(0x00040003);
    e.Call(Emit::USER, 76);
    ok(15);
    // 16. ShowCursor(FALSE) = -1, (TRUE) = 0; SetCursor(IDC_WAIT); SetCapture / GetCapture / ReleaseCapture
    e.Imm(0);
    e.Call(Emit::USER, 71);
    eq(0xFFFF, 16);
    e.Imm(1);
    e.Call(Emit::USER, 71);
    eq(0, 16);
    e.Imm(0); e.Long(32514);
    e.Call(Emit::USER, 173);  // LoadCursor(NULL, IDC_WAIT)
    ok(16);
    a.db({0x50});             // push ax
    e.Call(Emit::USER, 69);   // SetCursor
    e.Mem(kHwnd);
    e.Call(Emit::USER, 18);   // SetCapture -> previous (none)
    eq(0, 16);
    e.Call(Emit::USER, 236);  // GetCapture
    a.db({0x3B, 0x06}).dw(kHwnd); FailUnless(a, JZ, 16);
    e.Call(Emit::USER, 19);   // ReleaseCapture
    e.Call(Emit::USER, 236);
    eq(0, 16);
    // 17. MessageBox(MB_YESNO | MB_DEFBUTTON2) = IDNO without a real box; DialogBox("ABOUT") = IDCANCEL
    e.Mem(kHwnd); e.Far(kQuit); e.Far(kTitle); e.Imm(0x0104);
    e.Call(Emit::USER, 1);
    eq(7, 17);
    e.Mem(kHinst); e.Far(kAbout); e.Mem(kHwnd);
    a.db({0x0E, 0x68}).Abs16("WndProc");
    e.Call(Emit::USER, 87);
    eq(2, 17);
    // 18. SOUND.DRV is silent; no wave devices; a missing WAV fails; MessageBeep
    e.Call(SOUND, 1);  // OpenSound
    e.Imm(1); e.Imm(60); e.Imm(4); e.Imm(0);
    e.Call(SOUND, 4);  // SetVoiceNote
    e.Call(MMSYSTEM, 401);
    eq(0, 18);
    e.Far(kNoWav); e.Imm(0x0003);  // SND_ASYNC | SND_NODEFAULT
    e.Call(MMSYSTEM, 2);
    eq(0, 18);
    e.Imm(0);
    e.Call(Emit::USER, 104);

    // 19. timeSetEvent(10 ms, periodic, TimeProc, dwUser 1234h), accelerators, then the loop
    e.Mem(kHinst); e.Long(1);
    e.Call(Emit::USER, 177);  // LoadAccelerators(#1)
    ok(19);
    e.StoreAx(kAccel);
    e.Imm(10); e.Imm(1);
    a.db({0x0E, 0x68}).Abs16("TimeProc");
    e.Long(0x1234); e.Imm(1);
    e.Call(MMSYSTEM, 602);
    ok(19);
    a.Label("loop");
    e.Far(kMsg); e.Imm(0); e.Imm(0); e.Imm(0);
    e.Call(Emit::USER, 108);
    a.db({0x85, 0xC0});
    a.Short(JZ, "done");
    e.Mem(kHwnd); e.Mem(kAccel); e.Far(kMsg);
    e.Call(Emit::USER, 178);  // TranslateAccelerator
    a.db({0x85, 0xC0});
    a.Short(JNZ, "loop");
    e.Far(kMsg);
    e.Call(Emit::USER, 114);
    a.Short(0xEB, "loop");
    a.Label("done");
    e.CmpMem(kUserSeen, 1); FailUnless(a, JZ, 19);
    e.CmpMem(kTicks, 3); FailUnless(a, JZ, 19);
    e.CmpMem(kCommand, 100); FailUnless(a, JZ, 19);
    // 20. The callback got dwUser = 1234h every time
    e.CmpMem(kBadUser, 0); FailUnless(a, JZ, 20);
    e.Exit0();
    e.FailStubs(20);

    // WndProc: [bp+14] hwnd, [bp+12] msg, [bp+10] wParam, [bp+8]:[bp+6] lParam
    a.Label("WndProc");
    a.db({0x55, 0x89, 0xE5, 0x8B, 0x46, 0x0C});  // push bp / mov bp,sp / mov ax,[bp+12]
    e.CmpAx(0x001C); a.Short(JZ, "wp_activateapp");
    e.CmpAx(0x0005); a.Short(JZ, "wp_size");
    e.CmpAx(0x0400); a.Short(JZ, "wp_user");
    e.CmpAx(0x0111); a.Short(JZ, "wp_command");
    a.Label("wp_default");
    e.Arg(14); e.Arg(12); e.Arg(10); e.Arg(8); e.Arg(6);
    e.Call(Emit::USER, 107);                     // DefWindowProc
    a.Near(0xE9, "wp_done");
    a.Label("wp_activateapp");
    e.Set(kActivated, 1);
    a.Short(0xEB, "wp_default");
    a.Label("wp_size");                          // lParam = cx | cy << 16
    a.db({0x8B, 0x46, 0x06});
    e.StoreAx(kSizeLo);
    a.db({0x8B, 0x46, 0x08});
    e.StoreAx(kSizeHi);
    a.Near(0xE9, "wp_zero");
    a.Label("wp_user");                          // post ourselves an F2 key press
    e.Set(kUserSeen, 1);
    e.Arg(14); e.Imm(0x0100); e.Imm(0x71); e.Long(0);
    e.Call(Emit::USER, 110);                     // PostMessage(WM_KEYDOWN, VK_F2)
    a.Near(0xE9, "wp_zero");
    a.Label("wp_command");                       // from the accelerator: the command id
    a.db({0x8B, 0x46, 0x0A});
    e.StoreAx(kCommand);
    e.Imm(0);
    e.Call(Emit::USER, 6);                       // PostQuitMessage(0)
    a.Label("wp_zero");
    a.db({0x31, 0xC0, 0x31, 0xD2});
    a.Label("wp_done");
    a.db({0x5D, 0xCA, 0x0A, 0x00});              // pop bp / retf 10

    // TimeProc(UINT id, UINT msg, DWORD dwUser, DWORD dw1, DWORD dw2): 16 bytes.
    // [bp+20] id, [bp+18] msg, [bp+16]:[bp+14] dwUser, [bp+12]:[bp+10] dw1, [bp+8]:[bp+6] dw2
    a.Label("TimeProc");
    a.db({0x55, 0x89, 0xE5});
    a.db({0x81, 0x7E, 14}).dw(0x1234);           // cmp word [bp+14], 1234h
    a.Short(JZ, "tp_user_ok");
    e.Set(kBadUser, 1);
    a.Label("tp_user_ok");
    a.db({0xFF, 0x06}).dw(kTicks);               // inc word [ticks]
    e.CmpMem(kTicks, 3);
    a.Short(JNZ, "tp_done");
    e.Arg(20);
    e.Call(MMSYSTEM, 603);                       // timeKillEvent(id)
    e.Mem(kHwnd); e.Imm(0x0400); e.Imm(0); e.Long(0);
    e.Call(Emit::USER, 110);                     // PostMessage(WM_USER)
    a.Label("tp_done");
    a.db({0x5D, 0xCA, 0x10, 0x00});              // pop bp / retf 16

    code.bytes = a.Finish();
    p.segments = {code, DataSegment(data, 0x200)};
    return p;
}

// CrtProgram's files: CRT.INI and CRTDATA.BIN, for the program's directory.
inline std::vector<std::pair<std::string, std::string>> CrtProgramFiles() {
    return {{"CRT.INI", "; test settings\r\n[Game]\r\nLevel=7\r\nName = Sunny\r\n"}, {"CRTDATA.BIN", "RETRO16!"}};
}

}  // namespace win16test
