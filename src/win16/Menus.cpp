// USER: menus, accelerator tables and dialog boxes (not shown yet).

#include "win16/Menus.h"

#include <algorithm>

#include "win16/Api.h"
#include "win16/Runtime.h"

namespace retro::win16 {

// --- Menus ------------------------------------------------------------------------------

uint16_t Menus::Create(bool popup) {
    const uint16_t h = nextMenu_;
    nextMenu_ = uint16_t(nextMenu_ + 4);
    menus_[h].popup = popup;
    return h;
}

bool Menus::Destroy(uint16_t menu) {
    const auto it = menus_.find(menu);
    if (it == menus_.end()) return false;
    const std::vector<Item> items = it->second.items;
    menus_.erase(it);
    for (const Item& i : items) {
        if (i.popup) Destroy(i.popup);
    }
    return true;
}

bool Menus::ParseLevel(const std::vector<uint8_t>& data, size_t& at, uint16_t menu, int depth) {
    if (depth > 16) return false;
    auto u16 = [&](size_t p) { return uint16_t(data[p] | (data[p + 1] << 8)); };
    for (;;) {
        if (at + 2 > data.size()) return false;
        Item item;
        const uint16_t flags = u16(at);
        at += 2;
        if (!(flags & mf::Popup)) {
            if (at + 2 > data.size()) return false;
            item.id = u16(at);
            at += 2;
        }
        while (at < data.size() && data[at]) item.text += char(data[at++]);
        if (at++ >= data.size()) return false;  // the NUL
        item.flags = uint16_t(flags & ~mf::End);
        if (flags & mf::Popup) {
            item.popup = Create(true);
            if (!ParseLevel(data, at, item.popup, depth + 1)) return false;
        }
        menus_[menu].items.push_back(item);
        if (flags & mf::End) return true;
    }
}

uint16_t Menus::FromTemplate(const std::vector<uint8_t>& data) {
    if (data.size() < 4) return 0;
    const uint16_t version = uint16_t(data[0] | (data[1] << 8));
    const uint16_t offset = uint16_t(data[2] | (data[3] << 8));  // bytes to the first item
    if (version != 0) return 0;
    size_t at = 4 + size_t(offset);
    const uint16_t menu = Create(false);
    if (!ParseLevel(data, at, menu, 0)) {
        Destroy(menu);
        return 0;
    }
    return menu;
}

bool Menus::Insert(uint16_t menu, uint16_t position, uint16_t flags, uint16_t idOrPopup, const std::string& text) {
    const auto it = menus_.find(menu);
    if (it == menus_.end()) return false;
    Item item;
    item.flags = uint16_t(flags & ~(mf::End | mf::ByPosition));
    if (flags & mf::Popup) {
        item.popup = idOrPopup;
    } else {
        item.id = idOrPopup;
    }
    if (!(flags & (mf::Bitmap | mf::OwnerDraw | mf::Separator))) item.text = text;
    std::vector<Item>& items = it->second.items;
    items.insert(items.begin() + std::min<size_t>(position, items.size()), item);
    return true;
}

bool Menus::Append(uint16_t menu, uint16_t flags, uint16_t idOrPopup, const std::string& text) {
    const auto it = menus_.find(menu);
    return it != menus_.end() && Insert(menu, uint16_t(it->second.items.size()), flags, idOrPopup, text);
}

Menus::Item* Menus::Find(uint16_t menu, uint16_t item, uint16_t flags) {
    const auto it = menus_.find(menu);
    if (it == menus_.end()) return nullptr;
    std::vector<Item>& items = it->second.items;
    if (flags & mf::ByPosition) return item < items.size() ? &items[item] : nullptr;
    for (Item& i : items) {
        if (!i.popup && i.id == item) return &i;
    }
    for (Item& i : items) {  // then inside popups
        if (i.popup) {
            if (Item* found = Find(i.popup, item, flags)) return found;
        }
    }
    return nullptr;
}

bool Menus::Remove(uint16_t menu, uint16_t item, uint16_t flags, bool destroyPopup) {
    Item* target = Find(menu, item, flags);
    if (!target) return false;
    for (auto& [h, m] : menus_) {
        for (auto i = m.items.begin(); i != m.items.end(); ++i) {
            if (&*i != target) continue;
            const uint16_t popup = i->popup;
            m.items.erase(i);
            if (destroyPopup && popup) Destroy(popup);
            return true;
        }
    }
    return false;
}

const std::vector<Menus::Item>* Menus::Items(uint16_t menu) const {
    const auto it = menus_.find(menu);
    return it == menus_.end() ? nullptr : &it->second.items;
}

int Menus::Check(uint16_t menu, uint16_t item, uint16_t flags) {
    Item* i = Find(menu, item, flags);
    if (!i) return -1;
    const int previous = i->flags & mf::Checked;
    i->flags = uint16_t((i->flags & ~mf::Checked) | (flags & mf::Checked));
    return previous;
}

int Menus::EnableItem(uint16_t menu, uint16_t item, uint16_t flags) {
    Item* i = Find(menu, item, flags);
    if (!i) return -1;
    const int previous = i->flags & (mf::Grayed | mf::Disabled);
    i->flags = uint16_t((i->flags & ~(mf::Grayed | mf::Disabled)) | (flags & (mf::Grayed | mf::Disabled)));
    return previous;
}

int Menus::State(uint16_t menu, uint16_t item, uint16_t flags) {
    Item* i = Find(menu, item, flags);
    if (!i) return -1;
    if (i->popup) {
        const std::vector<Item>* sub = Items(i->popup);
        return int(((sub ? sub->size() : 0) << 8) | (i->flags & 0xFF));
    }
    return i->flags;
}

uint16_t Menus::LoadAccelerators(const std::vector<uint8_t>& data) {
    std::vector<Accelerator> table;
    for (size_t at = 0; at + 5 <= data.size(); at += 5) {
        Accelerator a;
        a.flags = data[at];
        a.key = uint16_t(data[at + 1] | (data[at + 2] << 8));
        a.id = uint16_t(data[at + 3] | (data[at + 4] << 8));
        table.push_back(a);
        if (a.flags & 0x80) break;  // the last entry
    }
    if (table.empty()) return 0;
    const uint16_t h = nextAccel_;
    nextAccel_ = uint16_t(nextAccel_ + 4);
    accelerators_[h] = std::move(table);
    return h;
}

const std::vector<Menus::Accelerator>* Menus::Accelerators(uint16_t table) const {
    const auto it = accelerators_.find(table);
    return it == accelerators_.end() ? nullptr : &it->second;
}

// --- API ------------------------------------------------------------------------------------

namespace {

void Return(Cpu& cpu, const PascalArgs& a, uint32_t value) {
    SetResult(cpu, value);
    cpu.ReturnFar(a.Bytes());
}

std::string ItemText(Runtime& rt, uint16_t flags, FarPtr p) {
    if ((flags & (mf::Bitmap | mf::OwnerDraw | mf::Separator)) || p.IsNull() || p.sel == 0) return "";
    return rt.Mem().ReadString(p.sel, p.off, 256);
}

void LoadMenu(Runtime& rt, Cpu& cpu) {  // (HINSTANCE, LPCSTR name) -> HMENU
    const PascalArgs a(cpu, {2, 4});
    const FarPtr name = a.Ptr(1);
    const NeResource* r = rt.Resource().Lookup(ResourceId{res::Menu, {}},
                                               ResourceId::FromFarPtr(rt.Mem(), name.sel, name.off));
    Return(cpu, a, r ? rt.MenuTable().FromTemplate(r->data) : 0);
}

void CreateMenu(Runtime& rt, Cpu& cpu) { SetResult(cpu, rt.MenuTable().Create(false)); cpu.ReturnFar(0); }
void CreatePopupMenu(Runtime& rt, Cpu& cpu) { SetResult(cpu, rt.MenuTable().Create(true)); cpu.ReturnFar(0); }

void DestroyMenu(Runtime& rt, Cpu& cpu) {  // (HMENU) -> BOOL
    const PascalArgs a(cpu, {2});
    Return(cpu, a, rt.MenuTable().Destroy(a.Word(0)) ? 1 : 0);
}

void SetMenu(Runtime& rt, Cpu& cpu) {  // (HWND, HMENU) -> BOOL
    const PascalArgs a(cpu, {2, 2});
    User::Window* w = rt.Windows().Edit(a.Word(0));
    bool ok = false;
    if (w && !(w->style & ws::Child) && (a.Word(1) == 0 || rt.MenuTable().Exists(a.Word(1)))) {
        w->menu = a.Word(1);
        ok = true;
        if (a.Word(1))
            rt.Note("menubar", "the menu bar isn't drawn yet: its commands are reachable only through "
                               "the program's keyboard shortcuts (accelerators)");
    }
    Return(cpu, a, ok ? 1 : 0);
}

void GetMenu(Runtime& rt, Cpu& cpu) {  // (HWND) -> HMENU
    const PascalArgs a(cpu, {2});
    const User::Window* w = rt.Windows().Find(a.Word(0));
    Return(cpu, a, w && !(w->style & ws::Child) ? w->menu : 0);
}

void DrawMenuBar(Runtime&, Cpu& cpu) {  // (HWND)
    const PascalArgs a(cpu, {2});
    cpu.ReturnFar(a.Bytes());
}

void CheckMenuItem(Runtime& rt, Cpu& cpu) {  // (HMENU, item, flags) -> previous, -1
    const PascalArgs a(cpu, {2, 2, 2});
    Return(cpu, a, uint16_t(int16_t(rt.MenuTable().Check(a.Word(0), a.Word(1), a.Word(2)))));
}

void EnableMenuItem(Runtime& rt, Cpu& cpu) {  // (HMENU, item, flags) -> previous, -1
    const PascalArgs a(cpu, {2, 2, 2});
    Return(cpu, a, uint16_t(int16_t(rt.MenuTable().EnableItem(a.Word(0), a.Word(1), a.Word(2)))));
}

void GetMenuState(Runtime& rt, Cpu& cpu) {  // (HMENU, item, flags) -> flags, -1
    const PascalArgs a(cpu, {2, 2, 2});
    Return(cpu, a, uint16_t(int16_t(rt.MenuTable().State(a.Word(0), a.Word(1), a.Word(2)))));
}

void GetSubMenu(Runtime& rt, Cpu& cpu) {  // (HMENU, position) -> HMENU
    const PascalArgs a(cpu, {2, 2});
    const Menus::Item* i = rt.MenuTable().Find(a.Word(0), a.Word(1), mf::ByPosition);
    Return(cpu, a, i ? i->popup : 0);
}

void GetMenuItemCount(Runtime& rt, Cpu& cpu) {  // (HMENU) -> count, -1
    const PascalArgs a(cpu, {2});
    const std::vector<Menus::Item>* items = rt.MenuTable().Items(a.Word(0));
    Return(cpu, a, items ? uint16_t(items->size()) : 0xFFFF);
}

void GetMenuItemID(Runtime& rt, Cpu& cpu) {  // (HMENU, position) -> id, -1 for popups
    const PascalArgs a(cpu, {2, 2});
    const Menus::Item* i = rt.MenuTable().Find(a.Word(0), a.Word(1), mf::ByPosition);
    Return(cpu, a, i && !i->popup ? i->id : 0xFFFF);
}

void GetMenuString(Runtime& rt, Cpu& cpu) {  // (HMENU, item, LPSTR, int max, flags) -> length
    const PascalArgs a(cpu, {2, 2, 4, 2, 2});
    const Menus::Item* i = rt.MenuTable().Find(a.Word(0), a.Word(1), a.Word(4));
    const FarPtr buf = a.Ptr(2);
    uint16_t n = 0;
    if (i && !buf.IsNull() && a.Int(3) > 0) {
        n = uint16_t(std::min<size_t>(i->text.size(), size_t(a.Int(3) - 1)));
        for (uint16_t k = 0; k < n; ++k) rt.Mem().Write8(buf.sel, uint16_t(buf.off + k), uint8_t(i->text[k]));
        rt.Mem().Write8(buf.sel, uint16_t(buf.off + n), 0);
    }
    Return(cpu, a, n);
}

void AppendMenu(Runtime& rt, Cpu& cpu) {  // (HMENU, flags, id or HMENU, LPCSTR) -> BOOL
    const PascalArgs a(cpu, {2, 2, 2, 4});
    Return(cpu, a, rt.MenuTable().Append(a.Word(0), a.Word(1), a.Word(2), ItemText(rt, a.Word(1), a.Ptr(3))) ? 1 : 0);
}

void InsertMenu(Runtime& rt, Cpu& cpu) {  // (HMENU, position/id, flags, id or HMENU, LPCSTR) -> BOOL
    const PascalArgs a(cpu, {2, 2, 2, 2, 4});
    Menus& m = rt.MenuTable();
    uint16_t position = a.Word(1);
    uint16_t menu = a.Word(0);
    if (!(a.Word(2) & mf::ByPosition)) {  // before the item with that command
        const std::vector<Menus::Item>* items = m.Items(menu);
        position = items ? uint16_t(items->size()) : 0;
        for (size_t k = 0; items && k < items->size(); ++k) {
            if (!(*items)[k].popup && (*items)[k].id == a.Word(1)) position = uint16_t(k);
        }
    }
    Return(cpu, a, m.Insert(menu, position, a.Word(2), a.Word(3), ItemText(rt, a.Word(2), a.Ptr(4))) ? 1 : 0);
}

void ModifyMenu(Runtime& rt, Cpu& cpu) {  // (HMENU, item, flags, id or HMENU, LPCSTR) -> BOOL
    const PascalArgs a(cpu, {2, 2, 2, 2, 4});
    Menus::Item* i = rt.MenuTable().Find(a.Word(0), a.Word(1), a.Word(2));
    if (i) {
        const uint16_t flags = a.Word(2);
        i->flags = uint16_t(flags & ~(mf::End | mf::ByPosition));
        i->popup = (flags & mf::Popup) ? a.Word(3) : 0;
        i->id = (flags & mf::Popup) ? 0 : a.Word(3);
        i->text = ItemText(rt, flags, a.Ptr(4));
    }
    Return(cpu, a, i ? 1 : 0);
}

void RemoveMenu(Runtime& rt, Cpu& cpu) {  // (HMENU, item, flags) -> BOOL
    const PascalArgs a(cpu, {2, 2, 2});
    Return(cpu, a, rt.MenuTable().Remove(a.Word(0), a.Word(1), a.Word(2), false) ? 1 : 0);
}

void DeleteMenu(Runtime& rt, Cpu& cpu) {  // (HMENU, item, flags) -> BOOL (destroys popups)
    const PascalArgs a(cpu, {2, 2, 2});
    Return(cpu, a, rt.MenuTable().Remove(a.Word(0), a.Word(1), a.Word(2), true) ? 1 : 0);
}

void GetSystemMenu(Runtime& rt, Cpu& cpu) {  // (HWND, BOOL revert) -> HMENU
    const PascalArgs a(cpu, {2, 2});
    User::Window* w = rt.Windows().Edit(a.Word(0));
    uint16_t h = 0;
    if (w) {
        if (a.Word(1)) {  // revert to the standard one: we keep none
            if (w->sysMenu) rt.MenuTable().Destroy(w->sysMenu);
            w->sysMenu = 0;
        } else {
            if (!w->sysMenu) w->sysMenu = rt.MenuTable().Create(true);
            h = w->sysMenu;
        }
    }
    Return(cpu, a, h);
}

void TrackPopupMenu(Runtime& rt, Cpu& cpu) {  // (HMENU, flags, x, y, reserved, HWND, RECT) -> BOOL
    const PascalArgs a(cpu, {2, 2, 2, 2, 2, 2, 4});
    rt.Note("trackpopupmenu", "popup menus aren't shown yet (TrackPopupMenu does nothing)");
    Return(cpu, a, 0);
}

void LoadAccelerators(Runtime& rt, Cpu& cpu) {  // (HINSTANCE, LPCSTR name) -> HACCEL
    const PascalArgs a(cpu, {2, 4});
    const FarPtr name = a.Ptr(1);
    const NeResource* r = rt.Resource().Lookup(ResourceId{res::Accelerator, {}},
                                               ResourceId::FromFarPtr(rt.Mem(), name.sel, name.off));
    Return(cpu, a, r ? rt.MenuTable().LoadAccelerators(r->data) : 0);
}

// (HWND, HACCEL, MSG FAR*) -> BOOL: a matching key becomes WM_COMMAND (lParam
// high word 1: from an accelerator), sent to the window.
void TranslateAccelerator(Runtime& rt, Cpu& cpu) {
    const PascalArgs a(cpu, {2, 2, 4});
    const std::vector<Menus::Accelerator>* table = rt.MenuTable().Accelerators(a.Word(1));
    const FarPtr m = a.Ptr(2);
    const uint16_t hwnd = a.Word(0);
    uint16_t handled = 0;
    if (table && !m.IsNull() && rt.Windows().Find(hwnd)) {
        const uint16_t message = rt.Mem().Read16(m.sel, uint16_t(m.off + 2));
        const uint16_t key = rt.Mem().Read16(m.sel, uint16_t(m.off + 4));
        User& u = rt.Windows();
        const bool shift = u.KeyState(0x10) < 0, control = u.KeyState(0x11) < 0,
                   alt = u.KeyState(0x12) < 0 || message == wm::SysKeyDown || message == 0x0106;
        const bool keyDown = message == wm::KeyDown || message == wm::SysKeyDown;
        const bool character = message == wm::Char || message == 0x0106;  // WM_SYSCHAR
        for (const Menus::Accelerator& acc : *table) {
            bool match = false;
            if (acc.flags & 0x01) {  // FVIRTKEY: key and modifiers
                match = keyDown && acc.key == key && shift == bool(acc.flags & 0x04) &&
                        control == bool(acc.flags & 0x08) && alt == bool(acc.flags & 0x10);
            } else {  // a character
                match = character && acc.key == key && alt == bool(acc.flags & 0x10);
            }
            if (!match) continue;
            handled = 1;
            // A command whose menu item is disabled isn't sent.
            const User::Window* w = u.Find(hwnd);
            const Menus::Item* item = w && w->menu ? rt.MenuTable().Find(w->menu, acc.id, mf::ByCommand) : nullptr;
            if (!item || !(item->flags & (mf::Grayed | mf::Disabled)))
                u.Send(hwnd, wm::Command, acc.id, 0x00010000);
            break;
        }
    }
    if (!rt.HasExited()) Return(cpu, a, handled);
}

// --- Dialog boxes: not shown yet ---

void DialogBoxCommon(Runtime& rt, Cpu& cpu, const PascalArgs& a) {
    const FarPtr name = a.Ptr(1);
    const ResourceId id = ResourceId::FromFarPtr(rt.Mem(), name.sel, name.off);
    const bool exists = rt.Resource().Lookup(ResourceId{res::Dialog, {}}, id) != nullptr;
    if (exists)
        rt.Note("dialog:" + id.Describe(), "dialog boxes aren't shown yet: DialogBox(" + id.Describe() +
                                               ") answers IDCANCEL");
    Return(cpu, a, exists ? 2 /* IDCANCEL */ : 0xFFFF);
}

void DialogBox(Runtime& rt, Cpu& cpu) {  // (HINSTANCE, LPCSTR template, HWND, DLGPROC) -> result
    const PascalArgs a(cpu, {2, 4, 2, 4});
    DialogBoxCommon(rt, cpu, a);
}

void DialogBoxParam(Runtime& rt, Cpu& cpu) {  // (..., LPARAM) -> result
    const PascalArgs a(cpu, {2, 4, 2, 4, 4});
    DialogBoxCommon(rt, cpu, a);
}

void CreateDialogCommon(Runtime& rt, Cpu& cpu, const PascalArgs& a) {
    const FarPtr name = a.Ptr(1);
    const ResourceId id = ResourceId::FromFarPtr(rt.Mem(), name.sel, name.off);
    rt.Note("createdialog:" + id.Describe(), "modeless dialogs aren't supported yet: CreateDialog(" +
                                                 id.Describe() + ") failed");
    Return(cpu, a, 0);
}

void CreateDialog(Runtime& rt, Cpu& cpu) {  // (HINSTANCE, LPCSTR, HWND, DLGPROC) -> HWND
    const PascalArgs a(cpu, {2, 4, 2, 4});
    CreateDialogCommon(rt, cpu, a);
}

void CreateDialogParam(Runtime& rt, Cpu& cpu) {
    const PascalArgs a(cpu, {2, 4, 2, 4, 4});
    CreateDialogCommon(rt, cpu, a);
}

void EndDialog(Runtime&, Cpu& cpu) {  // (HWND, int result)
    const PascalArgs a(cpu, {2, 2});
    Return(cpu, a, 1);
}

void IsDialogMessage(Runtime&, Cpu& cpu) {  // (HWND, MSG FAR*) -> FALSE: no dialogs exist
    const PascalArgs a(cpu, {2, 4});
    Return(cpu, a, 0);
}

void GetDlgItem(Runtime&, Cpu& cpu) {  // (HWND, id) -> no such control
    const PascalArgs a(cpu, {2, 2});
    Return(cpu, a, 0);
}

}  // namespace

std::vector<ApiFunction> MenuApi() {
    return {
        {87, "DIALOGBOX", DialogBox},
        {88, "ENDDIALOG", EndDialog},
        {89, "CREATEDIALOG", CreateDialog},
        {90, "ISDIALOGMESSAGE", IsDialogMessage},
        {91, "GETDLGITEM", GetDlgItem},
        {150, "LOADMENU", LoadMenu},
        {151, "CREATEMENU", CreateMenu},
        {152, "DESTROYMENU", DestroyMenu},
        {154, "CHECKMENUITEM", CheckMenuItem},
        {155, "ENABLEMENUITEM", EnableMenuItem},
        {156, "GETSYSTEMMENU", GetSystemMenu},
        {157, "GETMENU", GetMenu},
        {158, "SETMENU", SetMenu},
        {159, "GETSUBMENU", GetSubMenu},
        {160, "DRAWMENUBAR", DrawMenuBar},
        {161, "GETMENUSTRING", GetMenuString},
        {177, "LOADACCELERATORS", LoadAccelerators},
        {178, "TRANSLATEACCELERATOR", TranslateAccelerator},
        {239, "DIALOGBOXPARAM", DialogBoxParam},
        {241, "CREATEDIALOGPARAM", CreateDialogParam},
        {250, "GETMENUSTATE", GetMenuState},
        {263, "GETMENUITEMCOUNT", GetMenuItemCount},
        {264, "GETMENUITEMID", GetMenuItemID},
        {410, "INSERTMENU", InsertMenu},
        {411, "APPENDMENU", AppendMenu},
        {412, "REMOVEMENU", RemoveMenu},
        {413, "DELETEMENU", DeleteMenu},
        {414, "MODIFYMENU", ModifyMenu},
        {415, "CREATEPOPUPMENU", CreatePopupMenu},
        {416, "TRACKPOPUPMENU", TrackPopupMenu},
    };
}

}  // namespace retro::win16
