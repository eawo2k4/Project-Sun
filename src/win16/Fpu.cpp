#include "win16/Fpu.h"

#include <cmath>
#include <cstring>
#include <limits>

namespace retro::win16 {
namespace {

// The "real indefinite": the NaN an invalid operation produces.
double Indefinite() { return -std::numeric_limits<double>::quiet_NaN(); }

}  // namespace

void Fpu::Reset() {
    control = fpu::kDefaultControl;
    status = 0;
    top_ = 0;
    for (int i = 0; i < 8; ++i) {
        regs_[i] = 0;
        empty_[i] = true;
    }
}

uint16_t Fpu::StatusWord() const {
    uint16_t sw = uint16_t((status & ~0x3800) | (top_ << 11));
    if (status & control & fpu::kExceptionMask) sw |= 0x0080;  // ES: an unmasked exception is pending
    return sw;
}

void Fpu::SetStatusWord(uint16_t sw) {
    status = uint16_t(sw & ~0x3880);
    top_ = (sw >> 11) & 7;
}

uint16_t Fpu::TagWord() const {
    uint16_t tw = 0;
    for (int r = 0; r < 8; ++r) {
        uint16_t tag;
        if (empty_[r]) {
            tag = 3;
        } else if (regs_[r] == 0) {
            tag = 1;
        } else if (!std::isfinite(regs_[r])) {
            tag = 2;
        } else {
            tag = 0;
        }
        tw = uint16_t(tw | (tag << (2 * r)));
    }
    return tw;
}

void Fpu::SetTagWord(uint16_t tw) {
    for (int r = 0; r < 8; ++r) empty_[r] = ((tw >> (2 * r)) & 3) == 3;
}

int Fpu::Depth() const {
    int n = 0;
    for (bool e : empty_) n += e ? 0 : 1;
    return n;
}

double Fpu::Get(int i) {
    if (IsEmpty(i)) {
        status = uint16_t((status | fpu::IE | fpu::SF) & ~fpu::C1);  // stack underflow
        return Indefinite();
    }
    return St(i);
}

void Fpu::Set(int i, double v) {
    regs_[Phys(i)] = v;
    empty_[Phys(i)] = false;
}

void Fpu::Push(double v) {
    DecTop();
    if (!empty_[top_]) {
        status |= fpu::IE | fpu::SF | fpu::C1;  // stack overflow
        v = Indefinite();
    }
    regs_[top_] = v;
    empty_[top_] = false;
}

double Fpu::Pop() {
    const double v = Get(0);
    empty_[top_] = true;
    IncTop();
    return v;
}

void Fpu::Compare(double a, double b, bool quiet) {
    status &= ~fpu::kConditionMask;
    if (std::isnan(a) || std::isnan(b)) {
        status |= fpu::C0 | fpu::C2 | fpu::C3;
        if (!quiet) status |= fpu::IE;
    } else if (a < b) {
        status |= fpu::C0;
    } else if (a == b) {
        status |= fpu::C3;
    }
}

void Fpu::Examine() {
    status &= ~fpu::kConditionMask;
    if (IsEmpty(0)) {
        status |= fpu::C3 | fpu::C0;
        return;
    }
    const double v = St(0);
    if (std::signbit(v)) status |= fpu::C1;
    switch (std::fpclassify(v)) {
    case FP_NAN: status |= fpu::C0; break;
    case FP_INFINITE: status |= fpu::C2 | fpu::C0; break;
    case FP_ZERO: status |= fpu::C3; break;
    case FP_SUBNORMAL: status |= fpu::C3 | fpu::C2; break;
    default: status |= fpu::C2; break;
    }
}

double Fpu::RoundInt(double v) const {
    if (!std::isfinite(v)) return v;
    switch ((control >> 10) & 3) {
    case 1: return std::floor(v);
    case 2: return std::ceil(v);
    case 3: return std::trunc(v);
    default: {  // to nearest, ties to even
        double r = std::floor(v);
        const double diff = v - r;
        if (diff > 0.5 || (diff == 0.5 && std::fmod(r, 2.0) != 0)) r += 1;
        return r;
    }
    }
}

int64_t Fpu::ToInteger(double v, int bits) {
    const int64_t low = -(int64_t(1) << (bits - 1));
    const double r = RoundInt(v);
    // 2^(bits-1) is exact as a double, so the range test is too.
    if (std::isnan(r) || r < double(low) || r >= -double(low)) {
        status |= fpu::IE;
        return low;  // the integer indefinite
    }
    if (r != v) status |= fpu::PE;
    return int64_t(r);
}

void Fpu::ToExtended(double v, uint8_t out[10]) {
    uint64_t bits;
    std::memcpy(&bits, &v, 8);
    const uint16_t sign = (bits >> 63) ? 0x8000 : 0;
    const int exp = int((bits >> 52) & 0x7FF);
    const uint64_t frac = bits & ((uint64_t(1) << 52) - 1);
    uint64_t mant = 0;
    int e = 0;
    if (exp == 0x7FF) {  // infinity or NaN
        e = 0x7FFF;
        mant = (uint64_t(1) << 63) | (frac << 11);
    } else if (exp == 0) {
        if (frac != 0) {  // a double denormal is a normal extended number
            int msb = 51;
            while (!((frac >> msb) & 1)) --msb;
            mant = frac << (63 - msb);
            e = msb - 1074 + 16383;
        }
    } else {
        mant = (uint64_t(1) << 63) | (frac << 11);
        e = exp - 1023 + 16383;
    }
    for (int i = 0; i < 8; ++i) out[i] = uint8_t(mant >> (8 * i));
    const uint16_t se = uint16_t(sign | e);
    out[8] = uint8_t(se);
    out[9] = uint8_t(se >> 8);
}

double Fpu::FromExtended(const uint8_t in[10]) {
    uint64_t mant = 0;
    for (int i = 0; i < 8; ++i) mant |= uint64_t(in[i]) << (8 * i);
    const uint16_t se = uint16_t(in[8] | (in[9] << 8));
    const bool negative = (se & 0x8000) != 0;
    const int e = se & 0x7FFF;
    double v;
    if (e == 0x7FFF) {
        v = (mant << 1) == 0 ? std::numeric_limits<double>::infinity()
                             : std::numeric_limits<double>::quiet_NaN();
    } else if (mant == 0) {
        v = 0;
    } else {
        v = std::ldexp(double(mant), e - 16383 - 63);
    }
    return negative ? -v : v;
}

double Fpu::FromBcd(const uint8_t in[10]) {
    double v = 0;
    for (int i = 8; i >= 0; --i) v = v * 100 + (in[i] >> 4) * 10 + (in[i] & 0x0F);
    return (in[9] & 0x80) ? -v : v;
}

void Fpu::ToBcd(double v, uint8_t out[10]) {
    const double r = RoundInt(v);
    std::memset(out, 0, 10);
    if (!std::isfinite(r) || std::fabs(r) >= 1e18) {
        status |= fpu::IE;
        out[7] = 0xC0;  // the packed BCD indefinite
        out[8] = out[9] = 0xFF;
        return;
    }
    if (r != v) status |= fpu::PE;
    uint64_t n = uint64_t(std::fabs(r));
    for (int i = 0; i < 9; ++i) {
        const unsigned low = unsigned(n % 10);
        n /= 10;
        const unsigned high = unsigned(n % 10);
        n /= 10;
        out[i] = uint8_t((high << 4) | low);
    }
    if (std::signbit(r)) out[9] = 0x80;
}

}  // namespace retro::win16
