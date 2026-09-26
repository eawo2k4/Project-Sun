#pragma once

// USER's menus and accelerator tables.
//
// Menus are kept as a model (items with ids, text, check/enable state and
// popups), loaded from RT_MENU templates or built with CreateMenu/AppendMenu,
// so programs that check, enable and query items work. The menu bar isn't
// drawn yet; accelerators (TranslateAccelerator) turn keys into WM_COMMAND,
// which is how the commands are reachable meanwhile.

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace retro::win16 {

namespace mf {
constexpr uint16_t ByCommand = 0x0000, ByPosition = 0x0400, Grayed = 0x0001, Disabled = 0x0002,
                   Bitmap = 0x0004, Checked = 0x0008, Popup = 0x0010, End = 0x0080, OwnerDraw = 0x0100,
                   Separator = 0x0800;
}  // namespace mf

class Menus {
public:
    struct Item {
        uint16_t flags = 0;  // MF_xxx state and type (without MF_END)
        uint16_t id = 0;
        std::string text;
        uint16_t popup = 0;  // MF_POPUP: its menu
    };
    struct Accelerator {
        uint8_t flags = 0;  // FVIRTKEY 01h, FNOINVERT 02h, FSHIFT 04h, FCONTROL 08h, FALT 10h
        uint16_t key = 0;
        uint16_t id = 0;
    };

    uint16_t Create(bool popup);
    bool Destroy(uint16_t menu);  // and its popups
    bool Exists(uint16_t menu) const { return menus_.count(menu) != 0; }
    // RT_MENU template (header, then items; popups nest) -> a menu, or 0 if malformed.
    uint16_t FromTemplate(const std::vector<uint8_t>& data);
    bool Append(uint16_t menu, uint16_t flags, uint16_t idOrPopup, const std::string& text);
    bool Insert(uint16_t menu, uint16_t position, uint16_t flags, uint16_t idOrPopup, const std::string& text);
    bool Remove(uint16_t menu, uint16_t item, uint16_t flags, bool destroyPopup);
    const std::vector<Item>* Items(uint16_t menu) const;

    // CheckMenuItem / EnableMenuItem: the previous state bits, or -1 if no such item.
    int Check(uint16_t menu, uint16_t item, uint16_t flags);
    int EnableItem(uint16_t menu, uint16_t item, uint16_t flags);
    // GetMenuState: the item's flags (popups: item count in the high byte), or -1.
    int State(uint16_t menu, uint16_t item, uint16_t flags);
    Item* Find(uint16_t menu, uint16_t item, uint16_t flags);  // by command (nested) or position

    // RT_ACCELERATOR table (5-byte entries, the last flagged 80h) -> handle, 0 if malformed.
    uint16_t LoadAccelerators(const std::vector<uint8_t>& data);
    const std::vector<Accelerator>* Accelerators(uint16_t table) const;

    size_t Count() const { return menus_.size(); }

private:
    struct Menu {
        bool popup = false;
        std::vector<Item> items;
    };
    bool ParseLevel(const std::vector<uint8_t>& data, size_t& at, uint16_t menu, int depth);

    std::map<uint16_t, Menu> menus_;
    std::map<uint16_t, std::vector<Accelerator>> accelerators_;
    uint16_t nextMenu_ = 0x5004;
    uint16_t nextAccel_ = 0x5802;
};

}  // namespace retro::win16
