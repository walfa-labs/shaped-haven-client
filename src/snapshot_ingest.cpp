#include "snapshot_ingest.h"

namespace haven {

bool SnapshotIngest::offer(uint32_t server_tick, uint32_t run_time_ms, GameState game_state, bool baseline_ok, bool is_full)
{
    // A delta the client refuses must change nothing at all, so its verdict
    // comes before anything with a side effect — including the restart
    // boundary, whose acceptance disarms it. Ingesting the header first is
    // exactly the bug PROTOCOL.md "Baseline rules" rules out: a refused delta
    // would move the HUD clock and the game-state gating on its way past.
    if (!baseline_ok) return false;

    // A full snapshot the client did not ask for, carrying a tick below the one
    // it holds, is the server having restarted its run on its own. This is safe
    // to read as a new run rather than a stale leftover because the transport is
    // ordered: over TCP a snapshot that arrives after the one that set the held
    // tick was sent after it, so a tick below the held one cannot be a delayed
    // snapshot of the run the client is on — the server's counter only goes
    // backwards on a restart. The client therefore does explicitly what a
    // client-requested restart does: it resets the run's tick and arms the
    // boundary against the dead run's in-flight tail, so the new run's
    // snapshots are accepted and the dead run's are refused. A delta is never
    // this signal — only a full snapshot can restart a run — so `is_full` is
    // false and the ordering check below still refuses it.
    if (is_full && server_tick_ != 0 && server_tick < server_tick_)
        begin_run(run_time_ms_);

    // Everything still in the socket when C2S_RESTART went out was sent after
    // the last snapshot the client processed, so it carries the dead run's
    // frozen clock and its game_over state. Only a snapshot that reports the
    // run running again, or one below that clock, can be the new run's; see
    // run_events.h for why both signals are needed.
    if (!boundary_.accepts(run_time_ms, game_state))
    {
        ++stale_snapshots_;
        return false;
    }

    // Snapshots must otherwise arrive in order. A stale one carries an older
    // authoritative position, and applying it would drag the prediction
    // backwards.
    if (server_tick_ != 0 && server_tick < server_tick_)
    {
        ++stale_snapshots_;
        return false;
    }

    server_tick_ = server_tick;
    run_time_ms_ = run_time_ms;
    game_state_ = game_state;
    return true;
}

void SnapshotIngest::begin_run(uint32_t dead_run_time_ms)
{
    boundary_.arm(dead_run_time_ms);
    server_tick_ = 0;
    run_time_ms_ = 0;
}

}  // namespace haven
