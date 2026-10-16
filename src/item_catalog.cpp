#include "item_catalog.h"

#include <array>
#include <cstdio>

namespace haven {

namespace {

struct ItemInfo {
    const char* name;
    const char* short_name;
    uint8_t max_level;
    Rgba colour;
};

// Indexed by item id; gaps are "None" so an unknown id degrades to a blank
// entry instead of reading out of bounds.
std::array<ItemInfo, 256> make_item_table();

const ItemInfo& item_info(uint8_t item_id)
{
    static const std::array<ItemInfo, 256> kItems = make_item_table();
    return kItems[item_id & 0xFF];
}

std::array<ItemInfo, 256> make_item_table()
{
    const ItemInfo none = {"", "", 0, Rgba{0.5f, 0.5f, 0.5f, 1.0f}};
    std::array<ItemInfo, 256> items{};
    for (auto& item : items) item = none;

        // Weapons, DESIGN.md section 3 order.
        items[0] = {"SWORD", "SWORD", 8, Rgba{1.00f, 0.78f, 0.30f, 1.0f}};
        items[1] = {"AXE", "AXE", 8, Rgba{1.00f, 0.42f, 0.35f, 1.0f}};
        items[2] = {"BOW", "BOW", 8, Rgba{0.40f, 0.90f, 1.00f, 1.0f}};
        items[3] = {"GUN", "GUN", 8, Rgba{1.00f, 0.92f, 0.35f, 1.0f}};

        // Passives P0..P9, DESIGN.md section 3 order.
        items[16] = {"SPINACH CORE", "SPIN", 5, Rgba{0.35f, 0.85f, 0.40f, 1.0f}};
        items[17] = {"AMPLIFIER LENS", "LENS", 5, Rgba{0.35f, 0.55f, 0.95f, 1.0f}};
        items[18] = {"CLOCKWORK GEAR", "CLOCK", 5, Rgba{0.65f, 0.65f, 0.70f, 1.0f}};
        items[19] = {"ACCELERATOR COIL", "ACCEL", 5, Rgba{0.60f, 0.45f, 0.95f, 1.0f}};
        items[20] = {"STASIS CAPACITOR", "STAS", 5, Rgba{0.45f, 0.80f, 0.95f, 1.0f}};
        items[21] = {"PRISM SPLITTER", "PRISM", 3, Rgba{0.85f, 0.40f, 0.80f, 1.0f}};
        items[22] = {"MAGNET ARRAY", "MAG", 5, Rgba{0.90f, 0.35f, 0.45f, 1.0f}};
        items[23] = {"THRUSTER BOOTS", "THRUST", 5, Rgba{0.95f, 0.60f, 0.30f, 1.0f}};
        items[24] = {"TITANIUM PLATING", "PLATE", 5, Rgba{0.55f, 0.60f, 0.70f, 1.0f}};
        items[25] = {"NANO REPAIR", "NANO", 5, Rgba{0.40f, 0.90f, 0.70f, 1.0f}};

        // Evolutions, DESIGN.md section 3 order.
        items[32] = {"VORPAL BLENDER", "VORPAL", 1, Rgba{1.00f, 0.80f, 0.35f, 1.0f}};
        items[33] = {"PHASE KATANA", "KATANA", 1, Rgba{0.55f, 0.95f, 0.85f, 1.0f}};
        items[34] = {"GRAVITY CLEAVER", "CLEAVER", 1, Rgba{1.00f, 0.55f, 0.30f, 1.0f}};
        items[35] = {"JUGGERNAUT HALBERD", "JUGGER", 1, Rgba{0.75f, 0.85f, 1.00f, 1.0f}};
        items[36] = {"RAILGUN LONGBOW", "RAIL", 1, Rgba{0.50f, 0.95f, 0.95f, 1.0f}};
        items[37] = {"STORM VOLLEY", "STORM", 1, Rgba{1.00f, 0.75f, 0.45f, 1.0f}};
        items[38] = {"VULCAN MINIGUN", "VULCAN", 1, Rgba{1.00f, 0.60f, 0.55f, 1.0f}};
        items[39] = {"CRYO FLECHETTE", "CRYO", 1, Rgba{0.60f, 0.90f, 1.00f, 1.0f}};

        // Overclock, the uncapped endgame.
        items[48] = {"+2% DAMAGE", "+2%DMG", 0, Rgba{1.00f, 0.40f, 0.35f, 1.0f}};
        items[49] = {"+1.5% SPEED", "+SPD", 0, Rgba{0.40f, 0.80f, 1.00f, 1.0f}};
        items[50] = {"+25 MAX HP", "+25HP", 0, Rgba{0.45f, 0.90f, 0.45f, 1.0f}};

        return items;
}

}  // namespace

ItemKind item_kind(uint8_t item_id)
{
    if (item_id == kItemIdNone) return ItemKind::None;
    if (item_id <= 3) return ItemKind::Weapon;
    if (item_id >= 16 && item_id <= 25) return ItemKind::Passive;
    if (item_id >= 32 && item_id <= 39) return ItemKind::Evolution;
    if (item_id >= 48 && item_id <= 50) return ItemKind::Overclock;
    return ItemKind::None;
}

const char* item_name(uint8_t item_id)
{
    return item_info(item_id).name;
}

const char* item_short_name(uint8_t item_id)
{
    return item_info(item_id).short_name;
}

uint8_t item_max_level(uint8_t item_id)
{
    return item_info(item_id).max_level;
}

Rgba item_colour(uint8_t item_id)
{
    return item_info(item_id).colour;
}

bool evo_components(uint8_t evo_item_id, uint8_t& weapon_id, uint8_t& passive_id)
{
    // DESIGN.md section 3: each evolution pairs one weapon at level 8 with one
    // named passive at max level, in this order.
    switch (evo_item_id)
    {
        case 32: weapon_id = 0; passive_id = 18; return true;  // Sword + Clockwork Gear
        case 33: weapon_id = 0; passive_id = 23; return true;  // Sword + Thruster Boots
        case 34: weapon_id = 1; passive_id = 17; return true;  // Axe + Amplifier Lens
        case 35: weapon_id = 1; passive_id = 24; return true;  // Axe + Titanium Plating
        case 36: weapon_id = 2; passive_id = 19; return true;  // Bow + Accelerator Coil
        case 37: weapon_id = 2; passive_id = 21; return true;  // Bow + Prism Splitter
        case 38: weapon_id = 3; passive_id = 16; return true;  // Gun + Spinach Core
        case 39: weapon_id = 3; passive_id = 20; return true;  // Gun + Stasis Capacitor
    }
    return false;
}

std::string item_blurb(uint8_t item_id, uint8_t next_level)
{
    char buf[32];
    switch (item_kind(item_id))
    {
        case ItemKind::Weapon:
            std::snprintf(buf, sizeof(buf), "LEVEL %u", static_cast<unsigned>(next_level));
            break;
        case ItemKind::Passive:
            std::snprintf(buf, sizeof(buf), "LEVEL %u", static_cast<unsigned>(next_level));
            break;
        case ItemKind::Evolution:
        {
            uint8_t weapon = 0;
            uint8_t passive = 0;
            evo_components(item_id, weapon, passive);
            std::snprintf(buf, sizeof(buf), "%s + %s", item_short_name(weapon), item_short_name(passive));
            break;
        }
        case ItemKind::Overclock:
            std::snprintf(buf, sizeof(buf), "LEVEL %u", static_cast<unsigned>(next_level));
            break;
        default:
            buf[0] = '\0';
            break;
    }
    return buf;
}

}  // namespace haven
