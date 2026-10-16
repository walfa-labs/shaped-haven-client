#pragma once

#include "entity_frame.h"
#include "interpolation.h"
#include "protocol.h"

#include <cstdint>
#include <optional>
#include <unordered_map>

namespace haven {

/// The client's authoritative view of the run: the one complete world state it
/// holds, and the server tick that state is complete for.
///
/// PROTOCOL.md "Baseline rules": the client holds exactly one complete state,
/// the one it last acknowledged with `C2S_ACK_SNAPSHOT`. A delta is applied on
/// top of it only when its `baseline_tick` matches; a full snapshot replaces it
/// outright. Interest management means the state only ever holds entities
/// within the server's interest radius — one that leaves is destroyed, one that
/// enters is spawned — so applying a delta is a plain map update and an entity
/// that vanishes leaves nothing stale behind.
///
/// This is deliberately Ogre-free so the unit tests can exercise the baseline
/// machinery without a renderer, the same reason `run_events` and `hud_model`
/// are their own translation units.
class SnapshotState {
public:
    /// Replaces the held state with a full snapshot's contents and makes its
    /// tick the new baseline. Returns the tick to acknowledge.
    uint32_t apply_full(const SnapshotFull& snapshot);

    /// Applies a delta on top of the held state. Returns false — and changes
    /// nothing — when the delta's `baseline_tick` is not the tick the client
    /// holds; the caller must then keep acknowledging the state it does hold
    /// and wait for the full snapshot the server answers with. On success
    /// returns true and sets `new_baseline` to the tick to acknowledge.
    bool apply_delta(const SnapshotDelta& delta, uint32_t& new_baseline);

    /// The tick the held state is complete for, or nothing when the client has
    /// no baseline yet.
    std::optional<uint32_t> baseline() const { return baseline_; }

    /// The held entity count, for tests and the verbose log.
    size_t size() const { return entities_.size(); }

    /// Drops the baseline and every held entity, so the next snapshot of a
    /// fresh run starts from a clean state.
    void reset()
    {
        baseline_.reset();
        entities_.clear();
    }

    /// Whether the held state carries the entity `id` of `kind`.
    bool contains(uint32_t id, EntityKind kind) const;

    /// Builds a RenderSnapshot of the held state at `run_time_ms`, which is what
    /// goes into the interpolation buffer as the newest snapshot.
    RenderSnapshot build_render(uint32_t run_time_ms) const;

private:
    std::optional<uint32_t> baseline_;
    std::unordered_map<uint32_t, EntityDrawItem> entities_;
};

}  // namespace haven
