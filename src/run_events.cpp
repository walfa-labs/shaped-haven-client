#include "run_events.h"

namespace haven {

void RestartBoundary::arm(uint32_t dead_run_time_ms)
{
    dead_run_time_ms_ = dead_run_time_ms;
    armed_ = dead_run_time_ms > 0;
}

void RestartBoundary::clear()
{
    armed_ = false;
    dead_run_time_ms_ = 0;
}

bool RestartBoundary::accepts(uint32_t snapshot_run_time_ms)
{
    if (!armed_) return true;
    if (snapshot_run_time_ms >= dead_run_time_ms_) return false;
    armed_ = false;
    return true;
}

bool LevelUpGate::accept(uint16_t event_id)
{
    // The comparison is done in wrapping 16-bit space, like the sequence
    // numbers in protocol.h, so it stays correct across the id counter's wrap.
    if (has_seen_ && static_cast<int16_t>(event_id - newest_event_id_) <= 0) return false;
    has_seen_ = true;
    newest_event_id_ = event_id;
    return true;
}

void LevelUpGate::reset()
{
    has_seen_ = false;
    newest_event_id_ = 0;
}

}  // namespace haven
