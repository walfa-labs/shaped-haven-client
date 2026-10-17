#pragma once

#include "game_math.h"
#include "hud.h"
#include "hud_model.h"
#include "input_state.h"
#include "interpolation.h"
#include "net_client.h"
#include "prediction.h"
#include "renderer.h"
#include "run_events.h"
#include "snapshot_ingest.h"
#include "snapshot_state.h"

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace haven {

/// Where the client believes the run is. The snapshot header's game_state is
/// the authority; this mirrors it for input gating and HUD visibility, and is
/// driven by the S2C_LEVELUP_OFFER / S2C_GAME_OVER packets first.
enum class RunState : uint8_t {
    Connecting,
    Running,
    LevelUp,
    GameOver,
};

struct SessionStats {
    bool welcomed = false;
    uint8_t player_id = 0;
    uint32_t client_tick = 0;
    uint32_t server_tick = 0;
    uint32_t last_acked_seq = 0;
    uint32_t inputs_sent = 0;
    uint32_t pre_welcome_steps = 0;
    uint32_t disconnected_steps = 0;
    uint32_t stale_snapshots = 0;
    uint32_t send_failures = 0;
    uint32_t snapshots_received = 0;
    /// Level-up offers for this player that were ignored: one that named an
    /// event already answered, or a second offer while one was still open.
    uint32_t stale_offers = 0;
    /// Packets whose header disagreed with the bytes actually delivered, or
    /// whose snapshot payload was rejected by the decoder.
    uint32_t malformed_packets = 0;
    uint32_t reconcile_count = 0;
    float last_prediction_error = 0.0f;
    Vec2 predicted;
    Vec2 last_move;
    Vec2 authoritative;
    size_t interpolation_depth = 0;
    size_t remote_player_count = 0;
    size_t enemy_count = 0;
    size_t projectile_count = 0;
    size_t gem_count = 0;
    /// Phase-4 snapshot accounting, for the verbose log.
    uint32_t snapshots_full = 0;
    uint32_t snapshots_delta = 0;
    /// Deltas applied on top of the held baseline, and deltas refused because
    /// their `baseline_tick` was not the tick the client holds.
    uint32_t deltas_applied = 0;
    uint32_t deltas_rejected = 0;
    uint32_t acks_sent = 0;
    /// Acknowledgement sends the bounded queue refused. The tick stays pending
    /// and is retried, so this counts retries rather than lost acks.
    uint32_t ack_retries = 0;
    /// The server's interest radius, from the 21 byte `S2C_WELCOME`. Zero when
    /// the server still speaks the 17 byte phase-3 layout.
    uint16_t interest_radius = 0;
    /// Wall-clock cost of one GameSession::update(), exponentially averaged.
    /// The frame rate main() prints at exit also includes the GL render.
    float update_ms_ema = 0.0f;
    /// Phase 3 progression, mirrored from S2C_LOADOUT for the verbose log.
    uint16_t level = 1;
    uint32_t xp = 0;
    uint32_t xp_next = 0;
    uint16_t choices_made = 0;
    uint16_t restarts_sent = 0;
    uint16_t evolutions_seen = 0;
    GameState game_state = GameState::Running;
};

/// One frame of the client: predict, send input, ingest snapshots, reconcile,
/// interpolate the remote players and hand the result to the renderer.
class GameSession {
public:
    GameSession(HavenRenderer& renderer, NetClient& net, InputState& input, Hud& hud);

    /// Connects, sends C2S_HELLO and starts the socket thread. Retries a few
    /// times so a server that is still starting up does not kill the client.
    bool connect(const std::string& host, uint16_t port, const std::string& name);

    void update(float dt_seconds);
    void shutdown();

    const SessionStats& stats() const { return stats_; }

private:
    void step_fixed();
    void drain_packets();
    void handle_welcome(const uint8_t* payload, size_t len);
    void handle_snapshot(const uint8_t* payload, size_t len);
    void handle_delta(const uint8_t* payload, size_t len);

    /// Shared snapshot-header handling for both message types: the restart
    /// boundary and the ordering check, then the header fields the run state
    /// and HUD read. Returns false when the snapshot is a stale leftover.
    ///
    /// `baseline_ok` is true for a full snapshot and for a delta whose
    /// `baseline_tick` is the tick the client holds, and false for a delta the
    /// client refuses. A refusal must advance nothing at all (PROTOCOL.md
    /// "Baseline rules"), so the caller computes `baseline_ok` *before* this and
    /// returns on a false without reconciling or rendering.
    bool ingest_header(uint32_t server_tick, uint32_t run_time_ms, GameState game_state, bool baseline_ok);

    /// Answers a snapshot with C2S_ACK_SNAPSHOT for `tick`, which is what lets
    /// the server pick a baseline to diff against. The tick is remembered until
    /// a send for it succeeds; see flush_ack().
    void send_ack(uint32_t tick);

    /// Tries to send the acknowledgement the client still owes, if any. Called
    /// once per frame so a tick whose send the bounded queue refused is retried
    /// rather than forgotten.
    void flush_ack();

    /// Folds the snapshot's own record of the local player into the predictor.
    void reconcile_from(const PlayerNet* self, const std::array<uint32_t, kMaxPlayers>& acked_seq);

    /// Pushes the held state into the interpolation buffer as the newest
    /// snapshot, so the render clock and the draw items track the run.
    /// `replacing` is set for a full snapshot, which is the server's recovery
    /// path and the one place its clock can go backwards; see
    /// InterpolationBuffer::push_replacing.
    void push_render(bool replacing);

    void handle_levelup_offer(const uint8_t* payload, size_t len);
    void handle_game_over(const uint8_t* payload, size_t len);
    void handle_loadout(const uint8_t* payload, size_t len);

    /// Draws the interpolated remote players, enemies and projectiles. Remote
    /// players are keyed to SceneNodes; enemies and projectiles go into the
    /// per-frame combat batches.
    void update_entities();

    /// Input gating and HUD visibility, from the snapshot the server last
    /// told us about.
    void apply_run_state();

    /// Answers the open level-up offer, from the keyboard or the choice
    /// script.
    void resolve_levelup_choice();

    /// Sends C2S_RESTART from the keyboard or the choice script.
    void maybe_restart();

    /// Prints, once per event, that the choice script's next line names the
    /// other event and is therefore not consumed. `event` and `marker` name the
    /// event being waited on and the marker found in its place.
    void report_script_mismatch(const char* event, const char* marker);

    HavenRenderer& renderer_;
    NetClient& net_;
    InputState& input_;
    Hud& hud_;
    ClientPredictor predictor_;
    InterpolationBuffer interpolation_;
    SessionStats stats_;
    RunState run_state_ = RunState::Connecting;
    std::optional<LevelUpOffer> pending_offer_;
    GameOverStats game_over_{};
    EvolutionWatch evolution_watch_;
    /// Suppresses a level-up offer the client has already seen, so a duplicate
    /// or older one cannot reopen the modal for an event that is answered, and
    /// a second offer cannot displace one that is still open. See run_events.h.
    LevelUpGate levelup_gate_;
    /// Keeps a second C2S_RESTART from being queued while the first is still
    /// unacknowledged; see run_events.h.
    RestartGate restart_gate_;
    /// The run state the snapshot headers carry — the accepted tick, the run
    /// clock and the game state — and the rule that a refused delta advances
    /// none of it. The restart boundary lives inside it. See snapshot_ingest.h.
    SnapshotIngest ingest_;
    /// The acknowledgement the client still owes the server, retried until a
    /// send for it succeeds; see run_events.h.
    AckRetry ack_retry_;

    /// Set when the choice script's head line names the other event, so the
    /// stall is reported once per event rather than once per frame.
    bool script_mismatch_reported_ = false;

    /// Server tick of the newest snapshot when the level-up offer arrived:
    /// the pause begins at that tick, so snapshots at or below it describe the
    /// pre-pause run even though they arrive after the offer. Zero means no
    /// offer in this run, which is what a restart restores.
    uint32_t offer_tick_ = 0;

    /// Server tick of the newest snapshot when the run ended. A snapshot at or
    /// below it still describes the dead run, so it must not dismiss the
    /// scoreboard the moment it arrives. Zero means no game over in this run.
    uint32_t game_over_tick_ = 0;

    /// Remote player ids drawn on the previous frame, so a player who left
    /// has its SceneNode destroyed.
    std::unordered_set<uint32_t> known_remotes_;

    // Per-frame scratch, owned by the session rather than built on the stack:
    // at 1,000 enemies a fresh vector or set every frame is a megabyte a second
    // of pure allocator traffic. Every one of these is cleared, never rebuilt,
    // so its capacity survives from frame to frame.
    std::vector<EntityDrawItem> draw_items_;
    std::vector<EntityDrawItem> combat_items_;
    std::unordered_set<uint32_t> present_remotes_;
    std::vector<std::vector<uint8_t>> packets_;

    /// The one complete world state the client holds and the tick it is
    /// complete for. A delta is applied on top of it only when its
    /// baseline_tick matches; a full snapshot replaces it outright. See
    /// PROTOCOL.md "Baseline rules" and snapshot_state.h.
    SnapshotState snapshot_state_;

    float accumulator_ = 0.0f;
    uint64_t session_token_ = 0;
    uint32_t client_tick_ = 0;
};

}  // namespace haven
