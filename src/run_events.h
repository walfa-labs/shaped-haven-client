#pragma once

#include <cstdint>

namespace haven {

/// Rejects the snapshots of the run the client has just left.
///
/// The client cannot tell one run's entities from another's by id — the server
/// restarts its pools at 1 — but it can tell their clocks apart: a new run's
/// run_time_ms restarts near zero while the dead run's snapshots carry its
/// final, larger value. Everything the socket still holds when C2S_RESTART
/// goes out was sent after the last snapshot the client processed, so all of it
/// carries the dead run's frozen clock and is rejected. The boundary disarms on
/// the first snapshot below it, and the ordinary server-tick ordering takes
/// over from there.
///
/// This is deliberately not a counter of "the next few snapshots": a fixed
/// budget accepts whatever arrives, which is exactly how the dead run's tail
/// gets mistaken for the new run's baseline.
class RestartBoundary {
public:
    /// Records the dead run's last observed run clock and arms the boundary. A
    /// clock of zero carries nothing to compare against, so the boundary stays
    /// disarmed and every snapshot passes.
    void arm(uint32_t dead_run_time_ms);

    void clear();

    bool armed() const { return armed_; }

    /// True when a snapshot carrying this run clock may be accepted. Disarms
    /// the boundary once one is.
    bool accepts(uint32_t snapshot_run_time_ms);

private:
    bool armed_ = false;
    uint32_t dead_run_time_ms_ = 0;
};

/// Suppresses a level-up offer the client has already seen.
///
/// The server numbers its offers from 1 and increments per offer, and renumbers
/// from 1 with the run. An offer whose event_id is at or before the newest one
/// seen is therefore a duplicate or an older event; opening its modal again
/// would submit a second choice for an event that is already resolved.
class LevelUpGate {
public:
    /// True when this offer is new, in which case it becomes the newest seen.
    bool accept(uint16_t event_id);

    /// A new run renumbers its offers from 1, so the gate starts over with it.
    void reset();

private:
    bool has_seen_ = false;
    uint16_t newest_event_id_ = 0;
};

}  // namespace haven
