#pragma once

#include "protocol.h"

#include <array>
#include <cstdint>
#include <optional>

namespace haven {

/// How much of the XP bar to light for a loadout.
struct XpBarFill {
    bool visible = false;
    /// Fraction of the bar, 0..1. Meaningful only when `visible`.
    float fraction = 0.0f;
};

/// The XP bar fill for the newest S2C_LOADOUT.
///
/// `loadout` is empty until the first S2C_LOADOUT arrives — and again after a
/// restart, which clears the HUD. GameSession::update() asks for this on every
/// frame including those, so the empty case is answered before the loadout is
/// read: binding a reference to a disengaged std::optional is undefined
/// behaviour, and the first frame used to do exactly that.
XpBarFill xp_bar_fill(const std::optional<Loadout>& loadout);

/// Raises the evolution banner on the rising edge of a weapon slot's `evolved`
/// flag.
///
/// The previous flags live here rather than in the HUD so the edge is computed
/// from the wire, and reset() clears them with the run. Without that reset a
/// slot that was already evolved when the previous run ended never reports a
/// rising edge again, so an evolution in the new run is silently not announced.
class EvolutionWatch {
public:
    void reset();

    /// Slot index of the last weapon whose `evolved` flag just rose, or -1 when
    /// none did. Call once per S2C_LOADOUT.
    int observe(const Loadout& loadout);

private:
    std::array<uint8_t, 4> last_evolved_{};
};

}  // namespace haven
