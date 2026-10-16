#include "game_session.h"

#include "protocol.h"

#include <chrono>
#include <cstdio>
#include <thread>
#include <unordered_map>
#include <vector>

namespace haven {

namespace {

constexpr int kMaxStepsPerFrame = 8;
constexpr int kConnectAttempts = 10;

}  // namespace

GameSession::GameSession(HavenRenderer& renderer, NetClient& net, InputState& input)
    : renderer_(renderer), net_(net), input_(input)
{
    // Stable per-process token; phase 1 never reconnects with it.
    session_token_ = 0x53484156454E31ULL;
}

bool GameSession::connect(const std::string& host, uint16_t port, const std::string& name)
{
    for (int attempt = 1; attempt <= kConnectAttempts; ++attempt)
    {
        if (net_.connect(host, port))
        {
            net_.start();
            if (!net_.send(encode_hello(session_token_, 0, name)))
                std::fprintf(stderr, "haven: could not queue C2S_HELLO\n");
            std::printf("haven: connected to %s:%u\n", host.c_str(), static_cast<unsigned>(port));
            return true;
        }
        std::printf("haven: connect attempt %d/%d failed\n", attempt, kConnectAttempts);
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    return false;
}

void GameSession::update(float dt_seconds)
{
    // Fixed 60 Hz simulation steps, decoupled from the render rate so the
    // prediction matches the server's integrator exactly.
    accumulator_ += dt_seconds < 0.0f ? 0.0f : (dt_seconds > 0.25f ? 0.25f : dt_seconds);
    int steps = 0;
    while (accumulator_ >= kTickSeconds && steps < kMaxStepsPerFrame)
    {
        step_fixed();
        accumulator_ -= kTickSeconds;
        ++steps;
    }
    if (steps == kMaxStepsPerFrame) accumulator_ = 0.0f;

    stats_.client_tick = client_tick_;
    stats_.predicted = predictor_.position();

    drain_packets();

    // The interpolation clock tracks wall time so remote motion stays
    // continuous between snapshot arrivals.
    interpolation_.advance(dt_seconds);

    renderer_.set_local_player(predictor_.position());
    update_remote_players();
    renderer_.follow_camera(predictor_.position());
}

void GameSession::step_fixed()
{
    // Nothing to predict against until the server has told us who we are.
    if (!stats_.welcomed)
    {
        ++stats_.pre_welcome_steps;
        return;
    }

    // Without a live link the integrator would run away from any state
    // reconciliation could correct, so hold position instead.
    if (!net_.connected())
    {
        ++stats_.disconnected_steps;
        return;
    }

    const Vec2 move = input_.move_vector();
    stats_.last_move = move;

    // Reserve the sequence number the predictor is about to use, so the packet
    // describes exactly the step that is about to be taken.
    const uint32_t seq = predictor_.next_seq();
    if (!net_.send(encode_input(seq, client_tick_, move, 0, 0)))
    {
        // The link went away between the connected() check above and the send.
        // The move is lost, so do not advance the prediction either: a step the
        // server never saw would show up as a permanent offset.
        ++stats_.send_failures;
        return;
    }

    predictor_.step(move);
    renderer_.set_local_player(predictor_.position());
    ++stats_.inputs_sent;
    ++client_tick_;
}

void GameSession::drain_packets()
{
    std::vector<std::vector<uint8_t>> packets;
    net_.poll(packets);
    for (const std::vector<uint8_t>& packet : packets)
    {
        PacketHeader header{};
        if (!decode_header(packet.data(), packet.size(), header)) continue;
        const uint8_t* payload = packet.data() + kHeaderSize;
        const size_t payload_len = packet.size() - kHeaderSize;
        if (static_cast<uint8_t>(MsgType::S2C_WELCOME) == header.msg_type)
            handle_welcome(payload, payload_len);
        else if (static_cast<uint8_t>(MsgType::S2C_SNAPSHOT_FULL) == header.msg_type)
            handle_snapshot(payload, payload_len);
    }
}

void GameSession::handle_welcome(const uint8_t* payload, size_t len)
{
    Welcome welcome{};
    if (!decode_welcome(payload, len, welcome))
    {
        std::fprintf(stderr, "haven: malformed S2C_WELCOME\n");
        return;
    }
    stats_.welcomed = true;
    stats_.player_id = welcome.player_id;
    std::printf("haven: welcome player_id=%u tick_hz=%u snap_hz=%u arena_half=%u server_tick=%u\n",
                static_cast<unsigned>(welcome.player_id), static_cast<unsigned>(welcome.tick_hz),
                static_cast<unsigned>(welcome.snap_hz), static_cast<unsigned>(welcome.arena_half_size),
                welcome.server_tick);
}

void GameSession::handle_snapshot(const uint8_t* payload, size_t len)
{
    SnapshotFull snapshot{};
    if (!decode_snapshot_full(payload, len, snapshot))
    {
        std::fprintf(stderr, "haven: malformed S2C_SNAPSHOT_FULL\n");
        return;
    }

    ++stats_.snapshots_received;

    // Snapshots must arrive in order. A stale one carries an older
    // authoritative position, and applying it would drag the prediction
    // backwards.
    if (stats_.server_tick != 0 && snapshot.server_tick < stats_.server_tick)
    {
        ++stats_.stale_snapshots;
        return;
    }
    stats_.server_tick = snapshot.server_tick;

    RenderSnapshot render{};
    render.run_time_ms = snapshot.run_time_ms;
    for (const PlayerNet& player : snapshot.players)
        render.positions.emplace(player.id, player.pos);

    // Reconcile on every snapshot, not only when the ack advances: the server
    // can re-send the same acked sequence with a corrected position, and the
    // blend needs those corrections to converge instead of leaving residual
    // error behind on repeated identical acks.
    if (stats_.welcomed && stats_.player_id < snapshot.acked_seq.size())
    {
        const uint32_t acked = snapshot.acked_seq[stats_.player_id];
        const PlayerNet* self = nullptr;
        for (const PlayerNet& player : snapshot.players)
            if (player.id == stats_.player_id) self = &player;

        if (self != nullptr)
        {
            stats_.last_acked_seq = acked;
            stats_.authoritative = self->pos;
            const Vec2 before = predictor_.position();
            predictor_.reconcile(acked, self->pos);
            stats_.last_prediction_error = distance(before, predictor_.position());
            ++stats_.reconcile_count;
        }
    }

    interpolation_.push(render);
    stats_.interpolation_depth = interpolation_.size();
}

void GameSession::update_remote_players()
{
    if (!stats_.welcomed) return;

    std::unordered_map<uint32_t, Vec2> rendered;
    for (const auto& entry : interpolation_.latest_positions())
    {
        if (entry.first == stats_.player_id) continue;
        Vec2 pos;
        if (interpolation_.sample(entry.first, pos)) rendered.emplace(entry.first, pos);
    }

    for (const auto& entry : rendered)
        renderer_.set_remote_player(static_cast<uint8_t>(entry.first), entry.second);

    stats_.remote_player_count = rendered.size();
}

void GameSession::shutdown()
{
    net_.stop();
}

}  // namespace haven
