// Sound: USER's MessageBeep and MMSYSTEM (sndPlaySound, multimedia timers,
// device queries). SOUND.DRV (PC-speaker voices) is a silent module, handled
// in Runtime.cpp.
//
// Played through the host (PlaySound). Wave, MIDI, auxiliary and joystick
// devices report "none", which programs handle by running without them; MCI
// (MIDI music, CD audio) fails politely. Real wave/MIDI output comes later.

#include <windows.h>
#include <mmsystem.h>

#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "win16/Api.h"
#include "win16/Runtime.h"

namespace retro::win16 {

void StopHostSound() { PlaySoundW(nullptr, nullptr, 0); }

namespace {

void Return(Cpu& cpu, const PascalArgs& a, uint32_t value) {
    SetResult(cpu, value);
    cpu.ReturnFar(a.Bytes());
}

void Beep(Runtime& rt, UINT type) {
    rt.CountSound();
    if (!rt.Muted()) MessageBeep(type);
}

void Api_MessageBeep(Runtime& rt, Cpu& cpu) {  // (UINT type)
    const PascalArgs a(cpu, {2});
    const uint16_t t = a.Word(0);
    Beep(rt, t == 0xFFFF ? 0xFFFFFFFF : t);
    cpu.ReturnFar(a.Bytes());
}

// --- sndPlaySound -------------------------------------------------------------------------------

constexpr uint16_t SND_ASYNC16 = 0x01, SND_NODEFAULT16 = 0x02, SND_MEMORY16 = 0x04, SND_LOOP16 = 0x08;

// A RIFF WAVE image at p in 16-bit memory (its size comes from the RIFF header).
std::vector<uint8_t> WaveInMemory(Runtime& rt, FarPtr p) {
    Memory& mem = rt.Mem();
    const uint32_t available = mem.SegmentSize(p.sel) > p.off ? mem.SegmentSize(p.sel) - p.off : 0;
    if (available < 12) return {};
    const uint8_t* d = mem.SegmentData(p.sel) + p.off;
    if (std::string(reinterpret_cast<const char*>(d), 4) != "RIFF") return {};
    const uint32_t size = uint32_t(d[4] | (d[5] << 8) | (d[6] << 16) | (uint32_t(d[7]) << 24)) + 8;
    if (size > available) {
        rt.Note("hugewave", "sndPlaySound: sounds over 64 KB in memory (huge pointers) aren't supported yet");
        return {};
    }
    return std::vector<uint8_t>(d, d + size);
}

std::vector<uint8_t> WaveFile(Runtime& rt, const std::string& name) {
    std::filesystem::path host;
    std::string why;
    if (!rt.Files().Resolve(name, host, why)) return {};
    std::error_code ec;
    if (!std::filesystem::is_regular_file(host, ec) || std::filesystem::file_size(host, ec) > (64u << 20)) return {};
    std::ifstream in(host, std::ios::binary);
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

void Api_sndPlaySound(Runtime& rt, Cpu& cpu) {  // (LPCSTR sound or WAV image, UINT flags) -> BOOL
    const PascalArgs a(cpu, {4, 2});
    const FarPtr p = a.Ptr(0);
    const uint16_t flags = a.Word(1);
    if (p.IsNull()) {  // stop
        StopHostSound();
        Return(cpu, a, 1);
        return;
    }
    std::string name;
    std::vector<uint8_t> wave;
    if (flags & SND_MEMORY16) {
        wave = WaveInMemory(rt, p);
    } else {
        name = rt.Mem().ReadString(p.sel, p.off, 128);
        wave = WaveFile(rt, name);
    }
    if (wave.size() < 12 || std::string(reinterpret_cast<const char*>(wave.data()), 4) != "RIFF") {
        if (!name.empty()) rt.Note("sound:" + name, "sndPlaySound(\"" + name + "\"): no such sound file");
        if (!(flags & SND_NODEFAULT16)) Beep(rt, 0);
        Return(cpu, a, 0);
        return;
    }
    rt.CountSound();
    if (!rt.Muted()) {
        // Stop the current sound before replacing the buffer it plays from.
        StopHostSound();
        std::vector<uint8_t>& buffer = rt.PlayingSound();
        buffer = std::move(wave);
        DWORD f = SND_MEMORY | SND_NODEFAULT;
        if (flags & (SND_ASYNC16 | SND_LOOP16)) f |= SND_ASYNC;
        if (flags & SND_LOOP16) f |= SND_LOOP;
        PlaySoundW(reinterpret_cast<LPCWSTR>(buffer.data()), nullptr, f);
    }
    Return(cpu, a, 1);
}

// --- Multimedia timers and time -----------------------------------------------------------------

void Api_timeGetTime(Runtime& rt, Cpu& cpu) {
    SetResult(cpu, rt.TickCount());
    cpu.ReturnFar(0);
}

void Api_timePeriod(Runtime&, Cpu& cpu) {  // timeBeginPeriod / timeEndPeriod (UINT) -> TIMERR_NOERROR
    const PascalArgs a(cpu, {2});
    Return(cpu, a, 0);
}

void Api_timeGetDevCaps(Runtime& rt, Cpu& cpu) {  // (TIMECAPS FAR*, UINT size) -> 0
    const PascalArgs a(cpu, {4, 2});
    const FarPtr p = a.Ptr(0);
    if (!p.IsNull() && a.Word(1) >= 4) {
        rt.Mem().Write16(p.sel, p.off, 1);                    // wPeriodMin
        rt.Mem().Write16(p.sel, uint16_t(p.off + 2), 65535);  // wPeriodMax
    }
    Return(cpu, a, 0);
}

void Api_timeGetSystemTime(Runtime& rt, Cpu& cpu) {  // (MMTIME FAR*, UINT size) -> 0
    const PascalArgs a(cpu, {4, 2});
    const FarPtr p = a.Ptr(0);
    if (!p.IsNull() && a.Word(1) >= 6) {
        const uint32_t ms = rt.TickCount();
        rt.Mem().Write16(p.sel, p.off, 1);  // TIME_MS
        rt.Mem().Write16(p.sel, uint16_t(p.off + 2), uint16_t(ms));
        rt.Mem().Write16(p.sel, uint16_t(p.off + 4), uint16_t(ms >> 16));
    }
    Return(cpu, a, 0);
}

// (UINT delay, UINT resolution, LPTIMECALLBACK, DWORD user, UINT flags) -> id, 0 on failure
void Api_timeSetEvent(Runtime& rt, Cpu& cpu) {
    const PascalArgs a(cpu, {2, 2, 4, 4, 2});
    const FarPtr proc = a.Ptr(2);
    Return(cpu, a, rt.Windows().StartMultimediaTimer(a.Word(0), proc.sel, proc.off, a.Long(3), (a.Word(4) & 1) != 0));
}

void Api_timeKillEvent(Runtime& rt, Cpu& cpu) {  // (UINT id) -> 0, MMSYSERR_INVALPARAM
    const PascalArgs a(cpu, {2});
    Return(cpu, a, rt.Windows().StopMultimediaTimer(a.Word(0)) ? 0 : 11);
}

// --- Devices: none ------------------------------------------------------------------------------

void Api_NoDevices(Runtime&, Cpu& cpu) {  // xxxGetNumDevs() -> 0
    SetResult(cpu, 0);
    cpu.ReturnFar(0);
}

// MMSYSERR_xxx, JOYERR_xxx and MCIERR_xxx have the same values in Win16 and Win32.

void Api_joyGetPos(Runtime&, Cpu& cpu) {  // (UINT id, JOYINFO FAR*)
    const PascalArgs a(cpu, {2, 4});
    Return(cpu, a, JOYERR_UNPLUGGED);
}

void Api_joyGetDevCaps(Runtime&, Cpu& cpu) {  // (UINT id, JOYCAPS FAR*, UINT size)
    const PascalArgs a(cpu, {2, 4, 2});
    Return(cpu, a, MMSYSERR_NODRIVER);
}

void Api_waveOutGetDevCaps(Runtime&, Cpu& cpu) {  // (UINT id, WAVEOUTCAPS FAR*, UINT size)
    const PascalArgs a(cpu, {2, 4, 2});
    Return(cpu, a, MMSYSERR_BADDEVICEID);
}

void Api_waveOutOpen(Runtime& rt, Cpu& cpu) {  // (LPHWAVEOUT, UINT id, format, callback, instance, flags)
    const PascalArgs a(cpu, {4, 2, 4, 4, 4, 4});
    rt.Note("waveout", "wave audio output (waveOut) isn't supported yet: the program runs without it");
    Return(cpu, a, MMSYSERR_NODRIVER);
}

void Api_midiOutOpen(Runtime& rt, Cpu& cpu) {  // (LPHMIDIOUT, UINT id, callback, instance, flags)
    const PascalArgs a(cpu, {4, 2, 4, 4, 4});
    rt.Note("midiout", "MIDI output isn't supported yet: the program runs without music");
    Return(cpu, a, MMSYSERR_NODRIVER);
}

// --- MCI: not available ------------------------------------------------------------------------

void Api_mciSendString(Runtime& rt, Cpu& cpu) {  // (LPCSTR command, LPSTR ret, UINT size, HWND)
    const PascalArgs a(cpu, {4, 4, 2, 2});
    const FarPtr cmd = a.Ptr(0), ret = a.Ptr(1);
    const std::string c = cmd.IsNull() ? "" : rt.Mem().ReadString(cmd.sel, cmd.off, 128);
    rt.Note("mci", "MCI (MIDI music, CD audio, video) isn't supported yet: mciSendString(\"" + c + "\") failed");
    if (!ret.IsNull() && a.Word(2)) rt.Mem().Write8(ret.sel, ret.off, 0);
    Return(cpu, a, MCIERR_DEVICE_NOT_INSTALLED);
}

void Api_mciSendCommand(Runtime& rt, Cpu& cpu) {  // (UINT device, UINT message, DWORD, DWORD)
    const PascalArgs a(cpu, {2, 2, 4, 4});
    rt.Note("mci", "MCI (MIDI music, CD audio, video) isn't supported yet: mciSendCommand failed");
    Return(cpu, a, MCIERR_DEVICE_NOT_INSTALLED);
}

void Api_mciGetErrorString(Runtime& rt, Cpu& cpu) {  // (DWORD error, LPSTR, UINT size) -> BOOL
    const PascalArgs a(cpu, {4, 4, 2});
    const FarPtr buf = a.Ptr(1);
    const std::string text = "The MCI device is not available.";
    const uint16_t size = a.Word(2);
    if (!buf.IsNull() && size) {
        const uint16_t n = uint16_t(std::min<size_t>(text.size(), size - 1u));
        for (uint16_t i = 0; i < n; ++i) rt.Mem().Write8(buf.sel, uint16_t(buf.off + i), uint8_t(text[i]));
        rt.Mem().Write8(buf.sel, uint16_t(buf.off + n), 0);
    }
    Return(cpu, a, 1);
}

void Api_mmsystemGetVersion(Runtime&, Cpu& cpu) {
    SetResult(cpu, 0x0101);  // 1.01, as in Windows 3.1
    cpu.ReturnFar(0);
}

}  // namespace

std::vector<ApiFunction> UserSoundApi() { return {{104, "MESSAGEBEEP", Api_MessageBeep}}; }

std::vector<ApiFunction> MmsystemApi() {
    return {
        {2, "SNDPLAYSOUND", Api_sndPlaySound},
        {5, "MMSYSTEMGETVERSION", Api_mmsystemGetVersion},
        {101, "JOYGETNUMDEVS", Api_NoDevices},
        {102, "JOYGETDEVCAPS", Api_joyGetDevCaps},
        {103, "JOYGETPOS", Api_joyGetPos},
        {201, "MIDIOUTGETNUMDEVS", Api_NoDevices},
        {204, "MIDIOUTOPEN", Api_midiOutOpen},
        {301, "MIDIINGETNUMDEVS", Api_NoDevices},
        {350, "AUXGETNUMDEVS", Api_NoDevices},
        {401, "WAVEOUTGETNUMDEVS", Api_NoDevices},
        {402, "WAVEOUTGETDEVCAPS", Api_waveOutGetDevCaps},
        {404, "WAVEOUTOPEN", Api_waveOutOpen},
        {501, "WAVEINGETNUMDEVS", Api_NoDevices},
        {601, "TIMEGETSYSTEMTIME", Api_timeGetSystemTime},
        {602, "TIMESETEVENT", Api_timeSetEvent},
        {603, "TIMEKILLEVENT", Api_timeKillEvent},
        {604, "TIMEGETDEVCAPS", Api_timeGetDevCaps},
        {605, "TIMEBEGINPERIOD", Api_timePeriod},
        {606, "TIMEENDPERIOD", Api_timePeriod},
        {607, "TIMEGETTIME", Api_timeGetTime},
        {701, "MCISENDCOMMAND", Api_mciSendCommand},
        {702, "MCISENDSTRING", Api_mciSendString},
        {706, "MCIGETERRORSTRING", Api_mciGetErrorString},
    };
}

}  // namespace retro::win16
