#pragma once

#include "entity_frame.h"

#include <cstdint>
#include <string>

namespace haven {

// ---------------------------------------------------------------------------
// PROTOCOL.md's single item id space: weapons 0-3, passives 16-25 (DESIGN.md
// section 3 order), evolutions 32-39 (DESIGN.md section 3 order), Overclock
// 48-50, 0xFF = no item.
// ---------------------------------------------------------------------------

constexpr uint8_t kItemIdNone = 0xFF;
constexpr uint8_t kPassiveBase = 16;  // passive i (0..9) lives at 16 + i
constexpr uint8_t kEvoBase = 32;      // evolution i (0..7) lives at 32 + i
constexpr uint8_t kOverclockBase = 48;

enum class ItemKind : uint8_t {
    None,
    Weapon,
    Passive,
    Evolution,
    Overclock,
};

ItemKind item_kind(uint8_t item_id);

/// Long name for the modal and the scoreboard banner, e.g. "Gravity Cleaver".
const char* item_name(uint8_t item_id);

/// Short name for the item tray, at most 6 characters.
const char* item_short_name(uint8_t item_id);

/// Max level from the DESIGN.md section 3 tables: weapons 8, passives 5
/// (Prism Splitter 3), evolutions 1, Overclock 0 meaning uncapped.
uint8_t item_max_level(uint8_t item_id);

/// Colour the item's tray box and card accent are tinted with.
Rgba item_colour(uint8_t item_id);

/// The two items an evolution fuses, for the modal's subtitle line
/// ("Sword + Clockwork Gear"). Returns false for a non-evolution id.
bool evo_components(uint8_t evo_item_id, uint8_t& weapon_id, uint8_t& passive_id);

/// One-line description of what picking an item does, for the modal's detail
/// line. `next_level` is the level the option offers (1 for a new item).
std::string item_blurb(uint8_t item_id, uint8_t next_level);

}  // namespace haven
