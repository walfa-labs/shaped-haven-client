#pragma once

#include "game_math.h"
#include "input_state.h"
#include "interpolation.h"
#include "net_client.h"
#include "prediction.h"
#include "renderer.h"

#include <cstdint>
#include <string>

namespace haven {

struct SessionStats {
    bool welcomed = false;
    uint8_t player_id = 0;
    uint32_t client_tick = 0;
    uint32_t server_tick = 0;
    uint32_t last_acked_seq = 0;
    uint32_t inputs_sent = 0;
    uint32_t snapshots_received = 0;
    uint32_t reconcile_count = 0;
    float last_prediction_error = 0.0f;
    Vec2 predicted;
    Vec2 last_move;
    Vec2 authoritative;
    size_t interpolation_depth = 0;
    size_t remote_player_count = 0;
};

/// One frame of the client: predict, send input, ingest snapshots, reconcile,
/// interpolate the remote players and hand the result to the renderer.
class GameSession {
public:
    GameSession(HavenRenderer& renderer, NetClient& net, InputState& input);

    /// Connects, sends C2S_HELLO and starts the reader thread. Retries a few
    /// times so a server that is still starting up does not kill the client.
    bool connect(const std::string& host, uint16_t port);

    void update(float dt_seconds);
    void shutdown();

    const SessionStats& stats() const { return stats_; }

private:
    void step_fixed();
    void drain_packets();
    void handle_welcome(const uint8_t* payload, size_t len);
    void handle_snapshot(const uint8_t* payload, size_t len);
    void update_remote_players();

    HavenRenderer& renderer_;
    NetClient& net_;
    InputState& input_;
    ClientPredictor predictor_;
    InterpolationBuffer interpolation_;
    SessionStats stats_;

    float accumulator_ = 0.0f;
    uint64_t session_token_ = 0;
    uint32_t client_tick_ = 0;
};

}  // namespace haven
