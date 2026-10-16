#pragma once

#include "game_math.h"
#include "protocol.h"

#include <cstdint>
#include <deque>
#include <unordered_map>
#include <vector>

namespace haven {

/// A snapshot reduced to what the renderer needs, keyed by entity id.
struct RenderSnapshot {
    uint32_t run_time_ms = 0;
    std::unordered_map<uint32_t, Vec2> positions;
};

/// Holds the newest snapshots and answers "where should entity `id` be drawn
/// right now?" for a render time 100 ms behind the newest snapshot
/// (DESIGN.md section 2).
class InterpolationBuffer {
public:
    static constexpr float kDelayMs = kInterpolationDelaySeconds * 1000.0f;
    static constexpr size_t kCapacity = 32;

    void push(const RenderSnapshot& snapshot);

    /// Render timestamp used for the newest snapshot in the buffer.
    uint32_t latest_time_ms() const;
    size_t size() const { return snapshots_.size(); }
    void clear();

    /// Positions carried by the newest snapshot, keyed by entity id.
    const std::unordered_map<uint32_t, Vec2>& latest_positions() const;

    /// Interpolates the position of `entity_id` at
    /// `t_render = latest_time_ms - kDelayMs`. Returns false when the buffer
    /// does not span that time yet or the entity is unknown.
    bool sample(uint32_t entity_id, Vec2& out) const;

    /// Interpolates between two explicit snapshots; exposed for testing.
    static bool interpolate_between(const RenderSnapshot& a, const RenderSnapshot& b, int64_t t_render,
                                    uint32_t entity_id, Vec2& out);

private:
    std::deque<RenderSnapshot> snapshots_;
};

/// Blend factor between two snapshots 50 ms apart.
float blend_alpha(int64_t t_render, int64_t t_a, int64_t t_b);

}  // namespace haven
