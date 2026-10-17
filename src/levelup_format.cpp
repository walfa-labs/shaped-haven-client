#include "levelup_format.h"

#include "protocol.h"

#include <cmath>
#include <cstdio>

namespace haven {

namespace {

/// A stat value the way the comparison strings print it: whole numbers
/// without a decimal point, fractions trimmed to at most two decimals.
std::string stat_points(float value)
{
    char buf[32];
    if (value == std::floor(value))
    {
        std::snprintf(buf, sizeof(buf), "%.0f", value);
        return buf;
    }
    std::snprintf(buf, sizeof(buf), "%.2f", value);
    std::string text(buf);
    while (!text.empty() && text.back() == '0') text.pop_back();
    if (!text.empty() && text.back() == '.') text.pop_back();
    return text;
}

/// A multiplier stat as the "1.08x" the card shows: always two decimals.
std::string multiplier_text(float multiplier)
{
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.2fx", static_cast<double>(multiplier));
    return buf;
}

/// The relative change between two values, as the "(+8%)" or "(-9%)" a
/// comparison string carries. Omitted when the current value is zero (a
/// from-zero upgrade, where the percentage is infinite) or nothing moves.
std::string ratio_for(float current, float upgraded)
{
    if (current == 0.0f) return "";
    const int percent = static_cast<int>(std::lround((upgraded - current) / std::fabs(current) * 100.0f));
    if (percent == 0) return "";
    char paren[16];
    std::snprintf(paren, sizeof(paren), " (%+d%%)", percent);
    return paren;
}

}  // namespace

std::string format_stat_delta(uint8_t stat_type, int16_t current_stat_fp, int16_t upgraded_stat_fp)
{
    // The server sends an all-zero pair for an option whose effect the wire's
    // stat list has no slot for (projectile speed, duration, pickup radius,
    // regen); the card shows no comparison then.
    if (current_stat_fp == 0 && upgraded_stat_fp == 0) return "";

    const float current = static_cast<float>(current_stat_fp) / 100.0f;
    const float upgraded = static_cast<float>(upgraded_stat_fp) / 100.0f;

    switch (stat_type)
    {
    case static_cast<uint8_t>(StatType::Damage):
    {
        // Weapons and evolutions report absolute ATK (1200 fp reads as 12
        // ATK), with the relative gain. The damage passive instead reports
        // the accumulated multiplier bonus (10 fp reads as +10%), and the
        // Overclock fallback reports the multiplier itself (100 as 1.00x);
        // the fixed-point magnitudes separate the three, as no value in the
        // 100..1000 band is a plausible ATK count and no passive reaches 100.
        if (current_stat_fp < 100 && upgraded_stat_fp < 100)
        {
            const float c = 1.0f + current;
            const float u = 1.0f + upgraded;
            return multiplier_text(c) + " -> " + multiplier_text(u) + ratio_for(c, u) + " damage";
        }
        if (current_stat_fp >= 100 && current_stat_fp < 1000 && upgraded_stat_fp < 1000)
        {
            return multiplier_text(current) + " -> " + multiplier_text(upgraded) + ratio_for(current, upgraded) +
                   " damage";
        }
        return stat_points(current) + " ATK -> " + stat_points(upgraded) + " ATK" + ratio_for(current, upgraded);
    }
    case static_cast<uint8_t>(StatType::Armor):
    case static_cast<uint8_t>(StatType::MaxHP):
    {
        // Flat points: the delta is the difference itself.
        const char* unit = stat_type == static_cast<uint8_t>(StatType::Armor) ? " Armor" : " HP";
        std::string text = stat_points(current) + unit + " -> " + stat_points(upgraded) + unit;
        if (upgraded != current)
        {
            char paren[16];
            std::snprintf(paren, sizeof(paren), " (%+g)", static_cast<double>(upgraded - current));
            text += paren;
        }
        return text;
    }
    case static_cast<uint8_t>(StatType::MoveSpeed):
    case static_cast<uint8_t>(StatType::Area):
        // A multiplier on the base value: the fixed-point pair is the
        // accumulated bonus, so 8 reads as 1.08x.
    {
        const float c = 1.0f + current;
        const float u = 1.0f + upgraded;
        return multiplier_text(c) + " -> " + multiplier_text(u) + ratio_for(c, u);
    }
    case static_cast<uint8_t>(StatType::Cooldown):
    {
        // The wire carries the attack-speed multiplier, not seconds. The one
        // passive that reduces cooldown reports it as the accumulated delta
        // from a 1.0x base (0 reads as 1.00x, -8 as 0.92x), while the
        // Overclock fallback reports the multiplier itself; a positive
        // current value tells the two apart.
        const float c = current_stat_fp > 0 ? current : 1.0f + current;
        const float u = current_stat_fp > 0 ? upgraded : 1.0f + upgraded;
        return multiplier_text(c) + " CD -> " + multiplier_text(u) + " CD" + ratio_for(c, u);
    }
    case static_cast<uint8_t>(StatType::Projectiles):
        return stat_points(current) + " -> " + stat_points(upgraded) + " projectiles";
    case static_cast<uint8_t>(StatType::CritChance):
        return stat_points(current) + "% -> " + stat_points(upgraded) + "% crit";
    case static_cast<uint8_t>(StatType::MaxMana):
        return stat_points(current) + " -> " + stat_points(upgraded) + " mana";
    case static_cast<uint8_t>(StatType::MaxShield):
        return stat_points(current) + " -> " + stat_points(upgraded) + " shield";
    }

    // A stat type this client does not know: show the numbers rather than
    // nothing, so a newer server's option is still readable.
    return stat_points(current) + " -> " + stat_points(upgraded);
}

}  // namespace haven
