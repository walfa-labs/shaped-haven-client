#include "character_catalog.h"

#include <array>
#include <cstdio>

namespace haven {

namespace {

// The ten rows of the server's RACE_SPECS, in race-id order.
const std::array<RaceInfo, kRaceCount> kRaces{{
    {0, "Terran Kin", 110, 25, 100, 1.00f, 1.00f},
    {1, "Mantid Vex", 85, 15, 110, 1.15f, 1.18f},
    {2, "Cephalon Tide", 95, 50, 140, 0.95f, 1.05f},
    {3, "Aerie Seraph", 75, 20, 120, 1.22f, 1.08f},
    {4, "Saurian Scute", 145, 40, 80, 0.90f, 1.12f},
    {5, "Abyssal Pelag", 100, 35, 150, 1.08f, 1.00f},
    {6, "Weave Widow", 80, 30, 130, 1.05f, 1.20f},
    {7, "Felis Nyx", 85, 15, 100, 1.18f, 1.15f},
    {8, "Canid Vanguard", 130, 30, 90, 1.06f, 1.10f},
    {9, "Cervid Sylvan", 105, 25, 135, 1.16f, 0.98f},
}};

// DESIGN.md section 3's four weapons, which are also the four ids
// `C2S_HELLO.starting_weapon_id` accepts.
const std::array<StartingWeaponInfo, kStartingWeaponCount> kWeapons{{
    {0, "Sword", "MELEE ARC"},
    {1, "Axe", "HEAVY SWEEP"},
    {2, "Bow", "PIERCING ARROW"},
    {3, "Gun", "RAPID KINETIC"},
}};

int wrap(int value, int count)
{
    const int wrapped = value % count;
    return wrapped < 0 ? wrapped + count : wrapped;
}

}  // namespace

const RaceInfo* race_table()
{
    return kRaces.data();
}

const StartingWeaponInfo* starting_weapon_table()
{
    return kWeapons.data();
}

const RaceInfo& race_info(int race_id)
{
    return kRaces[static_cast<size_t>(wrap(race_id, kRaceCount))];
}

const StartingWeaponInfo& starting_weapon_info(int weapon_id)
{
    return kWeapons[static_cast<size_t>(wrap(weapon_id, kStartingWeaponCount))];
}

int cycle_race(int race_id, int delta)
{
    return wrap(race_id + delta, kRaceCount);
}

int cycle_weapon(int weapon_id, int delta)
{
    return wrap(weapon_id + delta, kStartingWeaponCount);
}

std::string format_multiplier(float mult)
{
    char buffer[16];
    std::snprintf(buffer, sizeof(buffer), "%.2fX", static_cast<double>(mult));
    return buffer;
}

}  // namespace haven
