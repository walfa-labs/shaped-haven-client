#pragma once

#include "entity_frame.h"
#include "game_math.h"
#include "protocol.h"

#include <cstdint>
#include <deque>
#include <unordered_map>
#include <vector>

namespace haven {

/// What the renderer needs to know about an entity besides where it is. Only
/// the position is interpolated; the style is cosmetic, so the newest
/// snapshot that carried the entity supplies it.
struct EntityStyle {
    EntityKind kind = EntityKind::Player;
    uint8_t style = 0;   // enemy archetype, or projectile weapon_id
    uint8_t hp_pct = 0;  // enemies only
    uint8_t shape = 0;   // projectiles only; ProjNet shape byte
    float angle = 0.0f;  // facing, in radians
};

/// A snapshot reduced to what the renderer needs, keyed by the namespaced
/// entity id (see interpolation_key).
struct RenderSnapshot {
    uint32_t run_time_ms = 0;
    std::unordered_map<uint32_t, Vec2> positions;
    std::unordered_map<uint32_t, EntityStyle> styles;
};

/// Holds the newest snapshots and answers "where should entity `id` be drawn
/// right now?" for a render time 100 ms behind the newest snapshot
/// (DESIGN.md section 2).
///
/// The render time is anchored to a local clock that the caller advances once
/// per rendered frame via advance(), not to the newest snapshot's timestamp.
/// Anchoring it to the snapshot froze the render time between arrivals, so
/// sample() returned an identical position for every frame in an interval and
/// then jumped once per snapshot — visibly stepped remote motion. Anchoring it
/// to the render clock keeps remote motion continuous at the display rate.
class InterpolationBuffer {
public:
    static constexpr float kDelayMs = kInterpolationDelaySeconds * 1000.0f;
    static constexpr size_t kCapacity = 32;

    /// Advances the render clock. Call once per rendered frame with the frame
    /// delta in seconds.
    void advance(float dt_seconds);

    /// Records a snapshot as arriving at the current render-clock time.
    void push(const RenderSnapshot& snapshot);

    /// Render timestamp carried by the newest snapshot.
    uint32_t latest_time_ms() const;
    size_t size() const { return snapshots_.size(); }
    void clear();

    /// Positions carried by the newest snapshot, keyed by namespaced entity id.
    const std::unordered_map<uint32_t, Vec2>& latest_positions() const;

    /// Styles carried by the newest snapshot, keyed by the same ids.
    const std::unordered_map<uint32_t, EntityStyle>& latest_styles() const;

    /// Render time expressed in snapshot (server) milliseconds: the newest
    /// snapshot's timestamp plus the wall time elapsed since it arrived, minus
    /// the interpolation delay. Negative until the buffer has filled.
    int64_t render_time_ms() const;

    /// Interpolates the position of `entity_id` at `render_time_ms()`. Returns
    /// false when the buffer does not span that time yet or the entity is
    /// unknown.
    bool sample(uint32_t entity_id, Vec2& out) const;

    /// Position of `entity_id` from the oldest buffered snapshot that carries
    /// it, i.e. where it was when it first appeared. Used to draw an entity the
    /// interpolation window does not bracket yet: holding it at its newest
    /// position instead would make it jump backwards once it becomes bracketed,
    /// because the window renders 100 ms in the past.
    bool first_seen_position(uint32_t entity_id, Vec2& out) const;

    /// Interpolates between two explicit snapshots; exposed for testing.
    static bool interpolate_between(const RenderSnapshot& a, const RenderSnapshot& b, int64_t t_render,
                                    uint32_t entity_id, Vec2& out);

private:
    struct Snapshot {
        RenderSnapshot data;
        uint32_t arrival_ms;
    };

    std::deque<Snapshot> snapshots_;
    uint32_t clock_ms_ = 0;
};

/// Blend factor between two snapshots 50 ms apart.
float blend_alpha(int64_t t_render, int64_t t_a, int64_t t_b);

}  // namespace haven
