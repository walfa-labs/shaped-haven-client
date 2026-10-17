#pragma once

#include "entity_frame.h"
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

/// The local player's vitals, reduced from the newest snapshot's `PlayerNet`.
///
/// The three percentages are the wire's whole-percent form (0..100, 100 being
/// full), so `hp_pct` is already `hp / max_hp`. `max_shield` is not redundant
/// with `shield_pct`: it is what says whether the race has a shield at all,
/// since the server reports `shield_pct` as 0 in that case too.
struct Vitals {
    uint8_t race_id = 0;
    uint8_t hp_pct = 0;
    uint8_t shield_pct = 0;
    uint8_t mana_pct = 0;
    uint16_t max_hp = 0;
    uint16_t max_shield = 0;
    /// False until a snapshot has carried this player. The bars stay hidden
    /// until then rather than showing an empty 0% bar for the first frames.
    bool reported = false;
};

/// The HUD's view of one player record. `reported` is always true: a caller
/// only builds one for a player the snapshot actually carried.
Vitals vitals_from(const PlayerNet& player);

/// The same, from the renderer's own draw item, which is what the game session
/// holds for the local player: it is not drawn, but the HUD still needs its
/// vitals.
Vitals vitals_from_player(const EntityDrawItem& item);

/// The shield bar's segment size, in points of the 100-point scale the bar
/// displays. One chevron divider goes at every multiple of this, so the bar
/// reads as four segments.
constexpr int kShieldSegmentPoints = 25;

/// The animated state behind the three status bars.
///
/// Two things animate. The health bar carries a white *ghost* bar behind its
/// fill: when HP drops, the ghost holds at the old value for
/// `kGhostHoldSeconds` and then snaps down to the new one, so a hit reads as a
/// white chunk draining away. Healing snaps the ghost up immediately, because
/// there is nothing to show. The mana bar flashes white for
/// `kManaFlashSeconds` after a dash the player could not pay for.
///
/// Everything here is arithmetic over the wire's whole-percent values, so the
/// unit tests drive it without a renderer.
class BarAnimator {
public:
    /// How long the ghost bar holds after HP drops before it snaps down.
    static constexpr float kGhostHoldSeconds = 0.45f;
    /// How long the mana bar flashes white after an unaffordable dash.
    static constexpr float kManaFlashSeconds = 0.20f;

    /// Feeds the newest vitals. Call once per frame.
    void observe(const Vitals& vitals);

    /// Advances the two timers. Call once per rendered frame.
    void tick(float dt_seconds);

    /// The player tried to dash without enough mana: flash the mana bar.
    void signal_dash_denied() { mana_flash_ = kManaFlashSeconds; }

    /// Whether the bars are on screen at all. False until the first snapshot
    /// has reported this player, and again after a restart clears them.
    bool visible() const { return reported_; }

    float hp_fraction() const { return hp_fraction_; }
    float ghost_fraction() const { return ghost_fraction_; }
    float shield_fraction() const { return shield_fraction_; }
    float mana_fraction() const { return mana_fraction_; }

    /// Whether the shield bar exists: a race with no shield has nothing to
    /// show, and `shield_pct` is 0 for it either way.
    bool shield_visible() const { return reported_ && max_shield_ > 0; }
    bool mana_flashing() const { return mana_flash_ > 0.0f; }

private:
    /// A whole-percent value as a 0..1 fraction, clamped so a malformed
    /// payload cannot push a bar past its track.
    static float fraction_of(uint8_t pct);

    bool reported_ = false;
    uint16_t max_shield_ = 0;
    float hp_fraction_ = 0.0f;
    float ghost_fraction_ = 0.0f;
    float shield_fraction_ = 0.0f;
    float mana_fraction_ = 0.0f;
    float ghost_hold_ = 0.0f;
    float mana_flash_ = 0.0f;
};

/// True when a dash costing `dash_cost` mana is payable from `vitals`.
///
/// `max_mana` is the scale `mana_pct` is taken against; it is not on the wire,
/// so the caller supplies it from the character catalog. Vitals that have not
/// been reported are affordable, so a player whose snapshot has not arrived yet
/// is never flashed.
bool dash_affordable(const Vitals& vitals, uint16_t max_mana, uint16_t dash_cost);

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
