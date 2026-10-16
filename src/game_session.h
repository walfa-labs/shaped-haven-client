#pragma once

#include "game_math.h"
#include "input_state.h"
#include "interpolation.h"
#include "net_client.h"
#include "prediction.h"
#include "renderer.h"

#include <cstdint>
#include <string>
#include <unordered_set>
#include <vector>

namespace haven {

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
    /// Wall-clock cost of one GameSession::update(), exponentially averaged.
    /// The frame rate main() prints at exit also includes the GL render.
    float update_ms_ema = 0.0f;
};

/// One frame of the client: predict, send input, ingest snapshots, reconcile,
/// interpolate the remote players and hand the result to the renderer.
class GameSession {
public:
    GameSession(HavenRenderer& renderer, NetClient& net, InputState& input);

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

    /// Draws the interpolated remote players, enemies and projectiles. Remote
    /// players are keyed to SceneNodes; enemies and projectiles go into the
    /// per-frame combat batches.
    void update_entities();

    HavenRenderer& renderer_;
    NetClient& net_;
    InputState& input_;
    ClientPredictor predictor_;
    InterpolationBuffer interpolation_;
    SessionStats stats_;

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
    RenderSnapshot pending_snapshot_;

    float accumulator_ = 0.0f;
    uint64_t session_token_ = 0;
    uint32_t client_tick_ = 0;
};

}  // namespace haven
