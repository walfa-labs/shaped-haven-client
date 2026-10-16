#include "run_events.h"

namespace haven {

void RestartBoundary::arm(uint32_t dead_run_time_ms)
{
    dead_run_time_ms_ = dead_run_time_ms;
    armed_ = true;
}

void RestartBoundary::clear()
{
    armed_ = false;
    dead_run_time_ms_ = 0;
}

bool RestartBoundary::accepts(uint32_t snapshot_run_time_ms, GameState snapshot_state)
{
    if (!armed_) return true;

    // A run clock below the dead run's last observed one is the new run's: the
    // dead run's clock only ever moved forward and then froze, so nothing it
    // sent can carry one.
    if (snapshot_run_time_ms < dead_run_time_ms_)
    {
        armed_ = false;
        return true;
    }

    // A snapshot that reports the run running again proves the server has
    // processed the restart, whatever clock it carries. This is what keeps the
    // boundary from latching shut on a dead run that ended before its own
    // second snapshot; see the class comment.
    if (snapshot_state != GameState::GameOver)
    {
        armed_ = false;
        return true;
    }

    // At or above the dead run's clock and still reporting the run over: the
    // dead run's tail.
    return false;
}

bool LevelUpGate::accept(uint16_t event_id)
{
    // One offer at a time: an offer arriving while one is open is a duplicate or
    // a corrupt packet, and replacing the open one with it would leave the event
    // it displaced unanswered. See the class comment.
    if (open_) return false;

    // The comparison is done in wrapping 16-bit space, like the sequence
    // numbers in protocol.h, so it stays correct across the id counter's wrap.
    if (has_answered_ && static_cast<int16_t>(event_id - newest_answered_) <= 0) return false;

    open_ = true;
    open_event_id_ = event_id;
    return true;
}

void LevelUpGate::resolve()
{
    if (!open_) return;
    open_ = false;
    has_answered_ = true;
    newest_answered_ = open_event_id_;
}

void LevelUpGate::reset()
{
    has_answered_ = false;
    newest_answered_ = 0;
    open_ = false;
    open_event_id_ = 0;
}

bool RestartGate::take(InputState& input, bool& script_mismatch)
{
    script_mismatch = false;

    // The keyboard is the manual path and the script the unattended one; a
    // human at the machine wins, as in GameSession::resolve_levelup_choice().
    bool restart = input.restart_requested();
    input.consume_restart();

    // A restart already in flight owns this game over. The keypress above is
    // consumed anyway, so it cannot fire once the gate clears, and the script is
    // not touched at all, so its marker survives for the next game over instead
    // of being spent on this one twice.
    if (pending_) return false;

    if (!restart && input.has_choice_script())
    {
        // A choice index is not a restart. Consuming it here would throw the
        // pick away and leave the run stuck at the scoreboard with no line left
        // to answer the level-up it was written for.
        restart = input.take_scripted_restart();
        if (!restart && input.peek_scripted_line() >= 0) script_mismatch = true;
    }
    if (!restart) return false;

    pending_ = true;
    return true;
}

}  // namespace haven
