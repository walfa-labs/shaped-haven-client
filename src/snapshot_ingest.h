#pragma once

#include "protocol.h"
#include "run_events.h"

#include <cstdint>

namespace haven {

/// The client's model of the run as the snapshot headers carry it — the newest
/// tick it has accepted, the run clock and the game state — and the rules that
/// decide whether a header may advance it.
///
/// PROTOCOL.md "Baseline rules" is why the decision and the state live
/// together. A delta the client refuses must change nothing at all: not the HUD
/// clock, not the game-state gating, not prediction. The refusal is decided by
/// whether the delta's `baseline_tick` is the tick the client holds, and that
/// verdict has to be reached *before* a header is ingested — an ingest that ran
/// first would let a refused delta move the run clock and the game state.
/// Folding the two together here is what makes that ordering testable without a
/// renderer, the same reason `run_events` and `snapshot_state` are their own
/// translation units.
///
/// A full snapshot at a tick *below* the one the client holds is a second,
/// independent rule: the server restarted its run without the client asking,
/// so its counter went backwards. That is what `offer`'s `is_full` arm decides,
/// and it is kept here with the rest of the run state for the same testability
/// reason.
class SnapshotIngest {
public:
    /// Offers a snapshot header. `baseline_ok` is true for a full snapshot, and
    /// for a delta whose `baseline_tick` is the tick the client holds; it is
    /// false for a delta the client refuses. `is_full` tells a full snapshot
    /// from an applicable delta, which the baseline verdict alone cannot — both
    /// carry `baseline_ok` true.
    ///
    /// Returns true when the header was accepted and the run state advanced to
    /// it. Returns false — and changes nothing, not even the restart boundary —
    /// when `baseline_ok` is false, when the restart boundary rejects the run
    /// clock, or when the tick is older than the newest accepted one. A full
    /// snapshot below the newest accepted tick is the one exception: it is an
    /// unsolicited new run and is accepted as one.
    bool offer(uint32_t server_tick, uint32_t run_time_ms, GameState game_state, bool baseline_ok, bool is_full);

    uint32_t server_tick() const { return server_tick_; }
    uint32_t run_time_ms() const { return run_time_ms_; }
    GameState game_state() const { return game_state_; }

    /// Snapshots the restart boundary or the ordering check rejected, for the
    /// verbose log.
    uint32_t stale_snapshots() const { return stale_snapshots_; }

    /// Begins a fresh run: the boundary is armed against the dead run's clock
    /// and the tick and clock return to zero. The game state is left where it
    /// is, so a scoreboard already up stays up until the new run's first
    /// snapshot says otherwise.
    void begin_run(uint32_t dead_run_time_ms);

private:
    RestartBoundary boundary_;
    uint32_t server_tick_ = 0;
    uint32_t run_time_ms_ = 0;
    GameState game_state_ = GameState::Running;
    uint32_t stale_snapshots_ = 0;
};

}  // namespace haven
