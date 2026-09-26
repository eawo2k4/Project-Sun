#pragma once

// A tiny 16-bit x86 code emitter for building test programs: raw bytes plus
// labels for short/near jumps and placeholders for far-call relocations.
// Instructions are hand-encoded at the call sites, each with its mnemonic.

#include <cstdint>
#include <initializer_list>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace win16test {

class Asm16 {
public:
    Asm16& db(std::initializer_list<int> bytes) {
        for (int b : bytes) code_.push_back(static_cast<uint8_t>(b));
        return *this;
    }
    Asm16& dw(uint16_t w) { return db({w & 0xFF, w >> 8}); }
    Asm16& str(const std::string& s) {
        for (char c : s) code_.push_back(static_cast<uint8_t>(c));
        return *this;
    }

    uint16_t Here() const { return static_cast<uint16_t>(code_.size()); }
    Asm16& Label(const std::string& name) {
        labels_[name] = Here();
        return *this;
    }

    // opcode + rel8 to `label` (Jcc 70-7F, JMP EB, LOOP E0-E2, JCXZ E3).
    Asm16& Short(uint8_t opcode, const std::string& label) {
        db({opcode, 0});
        fixups_.push_back({Here() - 1u, label, 1});
        return *this;
    }
    // CALL near rel16 (E8) or JMP near rel16 (E9).
    Asm16& Near(uint8_t opcode, const std::string& label) {
        db({opcode, 0, 0});
        fixups_.push_back({Here() - 2u, label, 2});
        return *this;
    }
    // A 16-bit immediate holding the offset of `label` (e.g. a callback's address).
    Asm16& Abs16(const std::string& label) {
        db({0, 0});
        fixups_.push_back({Here() - 2u, label, 0});
        return *this;
    }
    // CALL FAR ptr16:16 with an empty relocation chain (offset FFFF ends it).
    // Returns the offset of the pointer, for the relocation record.
    uint16_t CallFar() {
        db({0x9A});
        const uint16_t site = Here();
        db({0xFF, 0xFF, 0x00, 0x00});
        return site;
    }

    std::vector<uint8_t> Finish() const {
        std::vector<uint8_t> out = code_;
        for (const Fixup& f : fixups_) {
            const auto it = labels_.find(f.label);
            if (it == labels_.end()) throw std::runtime_error("undefined label " + f.label);
            if (f.size == 0) {  // absolute offset
                out[f.at] = static_cast<uint8_t>(it->second);
                out[f.at + 1] = static_cast<uint8_t>(it->second >> 8);
                continue;
            }
            const int rel = int(it->second) - int(f.at + f.size);
            if (f.size == 1) {
                if (rel < -128 || rel > 127) throw std::runtime_error("short jump out of range: " + f.label);
                out[f.at] = static_cast<uint8_t>(rel);
            } else {
                out[f.at] = static_cast<uint8_t>(rel);
                out[f.at + 1] = static_cast<uint8_t>(rel >> 8);
            }
        }
        return out;
    }

private:
    struct Fixup {
        uint32_t at;
        std::string label;
        int size;
    };
    std::vector<uint8_t> code_;
    std::map<std::string, uint16_t> labels_;
    std::vector<Fixup> fixups_;
};

}  // namespace win16test
