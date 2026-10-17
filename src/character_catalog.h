#pragma once

#include <cstdint>
#include <string>

namespace haven {

// ---------------------------------------------------------------------------
// The ten playable races and the four starting weapons, mirroring the server's
// src/races.zig (`RACE_SPECS`) and its weapon table. `C2S_HELLO` carries the two
// ids the player picks on the character select screen, and the server rejects
// an id it does not know rather than clamping it, so this table has to stay in
// step with that one.
// ---------------------------------------------------------------------------

constexpr int kRaceCount = 10;
constexpr int kStartingWeaponCount = 4;

/// One race's base stats, exactly as the server applies them at level 1.
struct RaceInfo {
    uint8_t id;
    const char* name;
    uint16_t max_hp;
    uint16_t max_shield;
    uint16_t max_mana;
    float move_speed_mult;
    float damage_mult;
    /// Mana a dash costs. Not on the wire — `PlayerNet` carries only the
    /// current mana percentage — so the HUD reads it from here to decide
    /// whether a dash attempt can be paid for.
    uint16_t dash_mana_cost;
};

struct StartingWeaponInfo {
    uint8_t id;
    const char* name;
    /// How the weapon lands its damage, for the select screen's detail line.
    const char* description;
};

/// The race rows, indexed by race id.
const RaceInfo* race_table();
/// The starting weapon rows, indexed by weapon id.
const StartingWeaponInfo* starting_weapon_table();

/// The row for `race_id`. An id outside the table reads as the first race
/// rather than past the end of it.
const RaceInfo& race_info(int race_id);
const StartingWeaponInfo& starting_weapon_info(int weapon_id);

/// Steps an id by `delta`, wrapping at both ends: what the Left/Right keys do.
int cycle_race(int race_id, int delta);
int cycle_weapon(int weapon_id, int delta);

/// A stat multiplier as the select screen prints it, e.g. "1.00X".
std::string format_multiplier(float mult);

}  // namespace haven
