#include "interpolation.h"

#include <algorithm>
#include <cmath>

namespace haven {

float blend_alpha(int64_t t_render, int64_t t_a, int64_t t_b)
{
    if (t_b <= t_a) return 1.0f;
    const float alpha = static_cast<float>(t_render - t_a) / static_cast<float>(t_b - t_a);
    return std::clamp(alpha, 0.0f, 1.0f);
}

void InterpolationBuffer::advance(float dt_seconds)
{
    // Clamp so a long stall (breakpoint, Alt-Tab) does not fling the render
    // time forward; the buffer catches up on the next snapshots instead.
    const float clamped = dt_seconds > 0.0f ? std::min(dt_seconds, 0.25f) : 0.0f;
    clock_ms_ += static_cast<uint32_t>(clamped * 1000.0f);
}

void InterpolationBuffer::push(const RenderSnapshot& snapshot)
{
    snapshots_.push_back(Snapshot{snapshot, clock_ms_});
    while (snapshots_.size() > kCapacity) snapshots_.pop_front();
}

uint32_t InterpolationBuffer::latest_time_ms() const
{
    return snapshots_.empty() ? 0 : snapshots_.back().data.run_time_ms;
}

void InterpolationBuffer::clear()
{
    snapshots_.clear();
}

const std::unordered_map<uint32_t, Vec2>& InterpolationBuffer::latest_positions() const
{
    static const std::unordered_map<uint32_t, Vec2> empty;
    return snapshots_.empty() ? empty : snapshots_.back().data.positions;
}

int64_t InterpolationBuffer::render_time_ms() const
{
    if (snapshots_.empty()) return -static_cast<int64_t>(kDelayMs);

    const Snapshot& newest = snapshots_.back();
    const int64_t elapsed = static_cast<int64_t>(clock_ms_) - static_cast<int64_t>(newest.arrival_ms);
    const int64_t now_server = static_cast<int64_t>(newest.data.run_time_ms) + (elapsed > 0 ? elapsed : 0);
    return now_server - static_cast<int64_t>(kDelayMs);
}

bool InterpolationBuffer::sample(uint32_t entity_id, Vec2& out) const
{
    if (snapshots_.size() < 2) return false;

    const int64_t t_render = render_time_ms();

    // Find the first snapshot at or after t_render; the one before it brackets
    // the render time together with it.
    size_t upper = snapshots_.size();
    for (size_t i = 0; i < snapshots_.size(); ++i)
    {
        if (static_cast<int64_t>(snapshots_[i].data.run_time_ms) >= t_render)
        {
            upper = i;
            break;
        }
    }
    if (upper == 0) upper = 1;
    if (upper >= snapshots_.size()) return false;

    return interpolate_between(snapshots_[upper - 1].data, snapshots_[upper].data, t_render, entity_id, out);
}

bool InterpolationBuffer::interpolate_between(const RenderSnapshot& a, const RenderSnapshot& b, int64_t t_render,
                                              uint32_t entity_id, Vec2& out)
{
    const auto it_a = a.positions.find(entity_id);
    const auto it_b = b.positions.find(entity_id);
    if (it_a == a.positions.end() || it_b == b.positions.end()) return false;

    const float alpha = blend_alpha(t_render, static_cast<int64_t>(a.run_time_ms), static_cast<int64_t>(b.run_time_ms));
    out = lerp(it_a->second, it_b->second, alpha);
    return true;
}

}  // namespace haven
