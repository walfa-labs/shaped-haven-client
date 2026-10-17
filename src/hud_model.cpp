#include "hud_model.h"

#include <algorithm>

namespace haven {

namespace {

/// A whole-percent value as a 0..1 fraction.
float clamped_fraction(uint8_t pct)
{
    return std::max(0.0f, std::min(1.0f, static_cast<float>(pct) / 100.0f));
}

}  // namespace

XpBarFill xp_bar_fill(const std::optional<Loadout>& loadout)
{
    XpBarFill fill;
    if (!loadout.has_value()) return fill;

    const Loadout& newest = *loadout;
    if (newest.xp_next == 0) return fill;

    fill.visible = true;
    fill.fraction = std::max(0.0f, std::min(1.0f, static_cast<float>(newest.xp) /
                                                      static_cast<float>(newest.xp_next)));
    return fill;
}

void EvolutionWatch::reset()
{
    last_evolved_.fill(0);
}

int EvolutionWatch::observe(const Loadout& loadout)
{
    int raised = -1;
    const size_t slots = std::min<size_t>(4, loadout.weapon_count);
    for (size_t i = 0; i < slots; ++i)
    {
        const bool evolved = loadout.weapons[i].evolved != 0;
        // The last slot to rise wins, which is the banner a caller showing one
        // per rise would be left with anyway.
        if (evolved && last_evolved_[i] == 0) raised = static_cast<int>(i);
        last_evolved_[i] = evolved ? 1 : 0;
    }
    return raised;
}

Vitals vitals_from(const PlayerNet& player)
{
    Vitals vitals;
    vitals.race_id = player.race_id;
    vitals.hp_pct = player.hp_pct;
    vitals.shield_pct = player.shield_pct;
    vitals.mana_pct = player.mana_pct;
    vitals.max_hp = player.max_hp;
    vitals.max_shield = player.max_shield;
    vitals.reported = true;
    return vitals;
}

Vitals vitals_from_player(const EntityDrawItem& item)
{
    Vitals vitals;
    vitals.race_id = item.race_id;
    vitals.hp_pct = item.hp_pct;
    vitals.shield_pct = item.shield_pct;
    vitals.mana_pct = item.mana_pct;
    vitals.max_hp = item.max_hp;
    vitals.max_shield = item.max_shield;
    vitals.reported = true;
    return vitals;
}

float BarAnimator::fraction_of(uint8_t pct)
{
    return clamped_fraction(pct);
}

void BarAnimator::observe(const Vitals& vitals)
{
    reported_ = vitals.reported;
    max_shield_ = vitals.max_shield;
    if (!vitals.reported)
    {
        hp_fraction_ = 0.0f;
        ghost_fraction_ = 0.0f;
        shield_fraction_ = 0.0f;
        mana_fraction_ = 0.0f;
        ghost_hold_ = 0.0f;
        return;
    }

    const float hp = clamped_fraction(vitals.hp_pct);
    if (hp < hp_fraction_)
    {
        // Damage. The ghost holds where it was, but only starts a hold it is
        // not already in: a second hit during the hold must not push the snap
        // back, or continuous damage would pin the ghost at its old value for
        // as long as the hits kept coming.
        if (ghost_hold_ <= 0.0f) ghost_hold_ = kGhostHoldSeconds;
    }
    else if (hp > hp_fraction_)
    {
        // Healing, or the first report. There is nothing to lag behind, so the
        // ghost snaps to the new value at once.
        ghost_fraction_ = hp;
        ghost_hold_ = 0.0f;
    }
    // Unchanged: leave both the fill and the ghost alone, so a hold already in
    // progress runs to its own deadline.
    hp_fraction_ = hp;

    shield_fraction_ = clamped_fraction(vitals.shield_pct);
    mana_fraction_ = clamped_fraction(vitals.mana_pct);
}

void BarAnimator::tick(float dt_seconds)
{
    if (ghost_hold_ > 0.0f)
    {
        ghost_hold_ -= dt_seconds;
        if (ghost_hold_ <= 0.0f)
        {
            ghost_hold_ = 0.0f;
            // The snap: the ghost drops to whatever HP is now, which is the
            // newest value rather than the one the hold started from.
            ghost_fraction_ = hp_fraction_;
        }
    }
    if (mana_flash_ > 0.0f) mana_flash_ = std::max(0.0f, mana_flash_ - dt_seconds);
}

bool dash_affordable(const Vitals& vitals, uint16_t max_mana, uint16_t dash_cost)
{
    if (!vitals.reported) return true;
    if (max_mana == 0) return true;
    const float mana = clamped_fraction(vitals.mana_pct) * static_cast<float>(max_mana);
    return mana >= static_cast<float>(dash_cost);
}

}  // namespace haven
