#pragma once

// Atom tables: strings <-> 16-bit atoms, as KERNEL (local atoms) and USER
// (global atoms, registered window messages, class names) keep them.
//
// Integer atoms ("#123", or a MAKEINTATOM pointer, selector 0) are the numbers
// 1..BFFFh themselves and need no entry. String atoms are C000h and up,
// compared case-insensitively and reference counted: each Add of an existing
// name returns the same atom, and it goes away after as many Deletes.

#include <cstdint>
#include <map>
#include <string>

namespace retro::win16 {

class AtomTable {
public:
    static constexpr uint16_t kFirstStringAtom = 0xC000;

    // The atom for `name`, adding it (or a reference to it); 0 if the name is
    // empty, an invalid integer atom, or the table is full.
    uint16_t Add(const std::string& name);
    // The atom for `name` if it's in the table (integer atoms always are), else 0.
    uint16_t Find(const std::string& name) const;
    // Drops a reference. Returns 0 on success, else the atom (DeleteAtom's result).
    uint16_t Delete(uint16_t atom);
    // The name of an atom ("#123" for an integer atom); false if there's none.
    bool Name(uint16_t atom, std::string& out) const;

    // "#123" -> 123 (1..BFFFh).
    static bool IntegerAtom(const std::string& name, uint16_t& atom);

private:
    struct Entry {
        uint16_t atom;
        uint16_t refs;
        std::string name;  // as first added
    };
    std::map<std::string, Entry> byKey_;  // upper-cased name
    std::map<uint16_t, std::string> byAtom_;
    uint16_t next_ = kFirstStringAtom;
};

}  // namespace retro::win16
