#include "hud_model.h"

#include <algorithm>

namespace haven {

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

}  // namespace haven
