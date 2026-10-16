#pragma once

#include "input_state.h"
#include "protocol.h"

#include <cstdint>

namespace haven {

/// Rejects the snapshots of the run the client has just left.
///
/// The client cannot tell one run's entities from another's by id — the server
/// restarts its pools at 1 — so it has to prove the run changed. The proof is
/// the snapshot's own game_state: the server holds `game_over` and stops
/// advancing the run clock until it has processed C2S_RESTART, so every packet
/// still in flight when the client queues the restart carries `game_over`, and
/// the first snapshot that reports the run running again is the new run's. A
/// dead-run snapshot can therefore never arrive carrying a running state.
///
/// The run clock is a second, narrower filter. Within a run the server's clock
/// only ever moves forward and it freezes at the end, so everything the server
/// sent after the last snapshot the client processed carries a clock at or
/// above that snapshot's; over TCP, anything still in flight was sent after it.
/// A clock below the dead run's last observed one therefore cannot be a leftover
/// from the dead run, and a clock at or above it is not accepted on its own
/// either.
///
/// The two together are what keep the boundary from latching shut. The clock
/// alone cannot: the client only sees the run clock at the 20 Hz snapshot
/// boundary, so the new run's first snapshot carries the clock of the first
/// broadcast after the restart, not zero. A dead run that ended before its own
/// second snapshot leaves that value at or above its last observed clock, and a
/// boundary that could only disarm on a strictly lower clock would then reject
/// every snapshot of the new run for the rest of the session. The state is what
/// lets it disarm there, and a clock of zero — a dead run the client never saw a
/// snapshot of — is handled by the same two rules rather than by giving up.
///
/// This does assume the server resets its run clock with the run and holds the
/// state at `game_over` until it has restarted; a server that kept counting, or
/// that returned to running on its own, would need a different signal.
class RestartBoundary {
public:
    /// Records the dead run's last observed run clock and arms the boundary.
    void arm(uint32_t dead_run_time_ms);

    void clear();

    bool armed() const { return armed_; }

    /// True when a snapshot carrying this run clock and game_state may be
    /// accepted. Disarms the boundary once one is.
    bool accepts(uint32_t snapshot_run_time_ms, GameState snapshot_state);

private:
    bool armed_ = false;
    uint32_t dead_run_time_ms_ = 0;
};

/// Tracks the level-up offer lifecycle: which offer is open, and which events
/// have already been answered.
///
/// The server pauses the whole simulation while an offer is unresolved, so it
/// never has two offers outstanding for one player: `checkLevelUps` runs once
/// per tick and only while the run is running, and the first offer it raises
/// moves the run into the pause that stops it running again (`checkLevelUps`
/// and `advance` in the server's sim.zig). A second offer for a different event
/// therefore cannot arrive before the first is answered.
///
/// That makes an offer arriving while one is still open a duplicate or a
/// corrupt packet, and replacing the open offer with it would be worse than
/// dropping it: the modal would change under the player's hand, and the event it
/// displaced would be left unanswered with the server paused on it. So the gate
/// ignores every offer while one is open, and compares event ids only against
/// the newest offer that has actually been answered.
///
/// Ignoring it does assume the client notices that an offer was answered before
/// the next one arrives, which it does in both ways one can be answered: it
/// calls `resolve()` itself when its choice goes out, and the run resumes
/// between two offers, which `GameSession::apply_run_state` sees in a snapshot.
/// The one sequence that slips through is a run that resumes and pauses again
/// inside a single 20 Hz snapshot interval, so the resume is never observed: the
/// client then holds the older modal until it is answered, and the newer event
/// falls to the server's 15 s timeout. That costs a pick, not a hang, and it is
/// the safe side of the trade — the alternative is answering an event twice.
class LevelUpGate {
public:
    /// True when this offer opens a new modal. False — and ignored — when one is
    /// already open, or when the event is no newer than the last answered one.
    bool accept(uint16_t event_id);

    /// The open offer has been answered, so a later event may open. Called when
    /// the choice goes out, and when the run resumes without one because the
    /// server's 15 s timeout auto-selected option 0.
    void resolve();

    /// A new run renumbers its offers from 1, so the gate starts over with it.
    void reset();

private:
    bool has_answered_ = false;
    uint16_t newest_answered_ = 0;
    bool open_ = false;
    uint16_t open_event_id_ = 0;
};

/// The one-shot guard around C2S_RESTART.
///
/// The client cannot see the new run until the server has processed the request
/// and sent a snapshot of it, so the client stays at `GameOver` for at least a
/// round trip after the request goes out. Without a guard, a second keypress or
/// the next `restart` marker in a choice script queues a second C2S_RESTART in
/// that window: a scripted run spends its whole script on consecutive frames at
/// one scoreboard, and the second request arms the run boundary from a clock the
/// first request has already cleared to zero, which disarms it. The gate stays
/// pending until the client has actually observed the new run.
class RestartGate {
public:
    /// Resolves this frame's restart request, from the keyboard and the choice
    /// script. Returns true when the caller must send C2S_RESTART, and leaves
    /// the gate pending until clear().
    ///
    /// While pending, every request is dropped rather than queued: the keyboard
    /// flag is consumed (it was pressed at this scoreboard) and the script's
    /// marker is left where it is, because the line after it names the game over
    /// after this one. `script_mismatch` is set when the script's head line
    /// names a level-up instead, which a game over must not spend.
    bool take(InputState& input, bool& script_mismatch);

    bool pending() const { return pending_; }

    /// No restart is in flight any more: the new run has been observed, or the
    /// request could not be sent and the game over is still unanswered.
    void clear() { pending_ = false; }

private:
    bool pending_ = false;
};

}  // namespace haven
