#pragma once

#include <cstdint>
#include <string>

namespace haven {

/// Formats one UpgradeOptionNet stat delta as the comparison string a level-up
/// card shows: the value the player holds now against the value the option
/// buys, with the change in parentheses — "12 ATK -> 15 ATK (+25%)",
/// "100 HP -> 125 HP (+25)", "1 -> 2 projectiles". The arrow renders as "->":
/// the HUD's baked 5x7 font has no arrow glyph.
///
/// `stat_type` is the wire's UpgradeOptionNet.stat_type (see StatType in
/// protocol.h) and the two fields are its fixed-point pair scaled by 100, so
/// 500 reads as 5.0. Returns an empty string when the option carries no stat
/// the wire reports (both fields zero), so the card layout can skip the line.
///
/// This is Ogre-free so the formatting is unit-tested without a renderer;
/// Hud::format_stat_delta delegates here.
std::string format_stat_delta(uint8_t stat_type, int16_t current_stat_fp, int16_t upgraded_stat_fp);

}  // namespace haven
