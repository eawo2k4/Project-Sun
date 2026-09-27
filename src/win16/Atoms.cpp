// Atom tables, KERNEL's local atom functions, USER's global atom functions,
// and window properties (SetProp/GetProp), which are named by atoms or strings.

#include "win16/Atoms.h"

#include <algorithm>
#include <cctype>
#include <string>

#include "win16/Api.h"
#include "win16/Runtime.h"

namespace retro::win16 {
namespace {

std::string Upper(std::string s) {
    for (char& c : s) c = char(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

}  // namespace

// --- AtomTable -------------------------------------------------------------------------------

bool AtomTable::IntegerAtom(const std::string& name, uint16_t& atom) {
    if (name.size() < 2 || name[0] != '#' || name.size() > 6) return false;
    uint32_t v = 0;
    for (size_t i = 1; i < name.size(); ++i) {
        if (!std::isdigit(static_cast<unsigned char>(name[i]))) return false;
        v = v * 10 + uint32_t(name[i] - '0');
    }
    if (v == 0 || v >= kFirstStringAtom) return false;
    atom = uint16_t(v);
    return true;
}

uint16_t AtomTable::Add(const std::string& name) {
    uint16_t atom = 0;
    if (!name.empty() && name[0] == '#') return IntegerAtom(name, atom) ? atom : 0;
    if (name.empty() || name.size() > 255) return 0;
    const std::string key = Upper(name);
    const auto it = byKey_.find(key);
    if (it != byKey_.end()) {
        if (it->second.refs < 0xFFFF) ++it->second.refs;
        return it->second.atom;
    }
    if (next_ == 0) return 0;  // all of C000h-FFFFh used
    atom = next_++;
    byKey_[key] = Entry{atom, 1, name};
    byAtom_[atom] = key;
    return atom;
}

uint16_t AtomTable::Find(const std::string& name) const {
    uint16_t atom = 0;
    if (!name.empty() && name[0] == '#') return IntegerAtom(name, atom) ? atom : 0;
    const auto it = byKey_.find(Upper(name));
    return it == byKey_.end() ? 0 : it->second.atom;
}

uint16_t AtomTable::Delete(uint16_t atom) {
    if (atom != 0 && atom < kFirstStringAtom) return 0;  // integer atoms: nothing to delete
    const auto a = byAtom_.find(atom);
    if (a == byAtom_.end()) return atom;
    const auto e = byKey_.find(a->second);
    if (--e->second.refs == 0) {
        byKey_.erase(e);
        byAtom_.erase(a);
    }
    return 0;
}

bool AtomTable::Name(uint16_t atom, std::string& out) const {
    if (atom != 0 && atom < kFirstStringAtom) {
        out = "#" + std::to_string(atom);
        return true;
    }
    const auto a = byAtom_.find(atom);
    if (a == byAtom_.end()) return false;
    out = byKey_.at(a->second).name;
    return true;
}

// --- API -------------------------------------------------------------------------------------

namespace {

// The name an atom argument gives: a string, or MAKEINTATOM(n) (selector 0).
std::string AtomArgName(Runtime& rt, FarPtr p) {
    if (p.sel == 0) return "#" + std::to_string(p.off);
    return rt.Mem().ReadString(p.sel, p.off);
}

void AddAtomTo(AtomTable& table, Runtime& rt, Cpu& cpu) {  // (LPCSTR) -> ATOM
    cpu.Regs().r[AX] = table.Add(AtomArgName(rt, ArgPtr(cpu, 0)));
    cpu.ReturnFar(4);
}

void FindAtomIn(AtomTable& table, Runtime& rt, Cpu& cpu) {  // (LPCSTR) -> ATOM
    cpu.Regs().r[AX] = table.Find(AtomArgName(rt, ArgPtr(cpu, 0)));
    cpu.ReturnFar(4);
}

void DeleteAtomFrom(AtomTable& table, Cpu& cpu) {  // (ATOM) -> 0, or the atom on failure
    cpu.Regs().r[AX] = table.Delete(cpu.StackArg(0));
    cpu.ReturnFar(2);
}

void AtomNameOf(AtomTable& table, Runtime& rt, Cpu& cpu) {  // (ATOM, LPSTR, int size) -> length
    const PascalArgs a(cpu, {2, 4, 2});
    const FarPtr buf = a.Ptr(1);
    const int16_t size = a.Int(2);
    std::string name;
    uint16_t length = 0;
    if (table.Name(a.Word(0), name) && !buf.IsNull() && size > 0) {
        length = uint16_t(std::min<size_t>(name.size(), size_t(size - 1)));
        for (uint16_t i = 0; i < length; ++i) rt.Mem().Write8(buf.sel, uint16_t(buf.off + i), uint8_t(name[i]));
        rt.Mem().Write8(buf.sel, uint16_t(buf.off + length), 0);
    }
    cpu.Regs().r[AX] = length;
    cpu.ReturnFar(a.Bytes());
}

void InitAtomTable(Runtime&, Cpu& cpu) {  // (int size) -> BOOL: the table grows as needed
    cpu.Regs().r[AX] = 1;
    cpu.ReturnFar(2);
}
void AddAtom(Runtime& rt, Cpu& cpu) { AddAtomTo(rt.LocalAtoms(), rt, cpu); }
void FindAtom(Runtime& rt, Cpu& cpu) { FindAtomIn(rt.LocalAtoms(), rt, cpu); }
void DeleteAtom(Runtime& rt, Cpu& cpu) { DeleteAtomFrom(rt.LocalAtoms(), cpu); }
void GetAtomName(Runtime& rt, Cpu& cpu) { AtomNameOf(rt.LocalAtoms(), rt, cpu); }

void GlobalAddAtom(Runtime& rt, Cpu& cpu) { AddAtomTo(rt.Windows().GlobalAtoms(), rt, cpu); }
void GlobalFindAtom(Runtime& rt, Cpu& cpu) { FindAtomIn(rt.Windows().GlobalAtoms(), rt, cpu); }
void GlobalDeleteAtom(Runtime& rt, Cpu& cpu) { DeleteAtomFrom(rt.Windows().GlobalAtoms(), cpu); }
void GlobalGetAtomName(Runtime& rt, Cpu& cpu) { AtomNameOf(rt.Windows().GlobalAtoms(), rt, cpu); }

// Window properties are keyed by name; an atom names the global atom's string.
std::string PropertyKey(Runtime& rt, FarPtr p) {
    std::string name;
    if (p.sel == 0) {
        if (!rt.Windows().GlobalAtoms().Name(p.off, name)) name = "#" + std::to_string(p.off);
    } else {
        name = rt.Mem().ReadString(p.sel, p.off);
    }
    return Upper(name);
}

void SetProp(Runtime& rt, Cpu& cpu) {  // (HWND, LPCSTR, HANDLE) -> BOOL
    const PascalArgs a(cpu, {2, 4, 2});
    cpu.Regs().r[AX] = rt.Windows().SetProperty(a.Word(0), PropertyKey(rt, a.Ptr(1)), a.Word(2)) ? 1 : 0;
    cpu.ReturnFar(a.Bytes());
}

void GetProp(Runtime& rt, Cpu& cpu) {  // (HWND, LPCSTR) -> HANDLE, or 0
    const PascalArgs a(cpu, {2, 4});
    cpu.Regs().r[AX] = rt.Windows().Property(a.Word(0), PropertyKey(rt, a.Ptr(1)));
    cpu.ReturnFar(a.Bytes());
}

void RemoveProp(Runtime& rt, Cpu& cpu) {  // (HWND, LPCSTR) -> the HANDLE removed, or 0
    const PascalArgs a(cpu, {2, 4});
    cpu.Regs().r[AX] = rt.Windows().RemoveProperty(a.Word(0), PropertyKey(rt, a.Ptr(1)));
    cpu.ReturnFar(a.Bytes());
}

}  // namespace

std::vector<ApiFunction> KernelAtomApi() {
    return {
        {68, "INITATOMTABLE", InitAtomTable},
        {69, "FINDATOM", FindAtom},
        {70, "ADDATOM", AddAtom},
        {71, "DELETEATOM", DeleteAtom},
        {72, "GETATOMNAME", GetAtomName},
    };
}

std::vector<ApiFunction> UserAtomApi() {
    return {
        {24, "REMOVEPROP", RemoveProp},
        {25, "GETPROP", GetProp},
        {26, "SETPROP", SetProp},
        {268, "GLOBALADDATOM", GlobalAddAtom},
        {269, "GLOBALDELETEATOM", GlobalDeleteAtom},
        {270, "GLOBALFINDATOM", GlobalFindAtom},
        {271, "GLOBALGETATOMNAME", GlobalGetAtomName},
    };
}

}  // namespace retro::win16
