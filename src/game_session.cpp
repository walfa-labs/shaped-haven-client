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

bool GameSession::connect(const std::string& host, uint16_t port)
{
    for (int attempt = 1; attempt <= kConnectAttempts; ++attempt)
    {
        if (net_.connect(host, port))
        {
            net_.start();
            net_.send(encode_hello(session_token_, 0, "haven"));
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
    renderer_.set_local_player(predictor_.position());
    update_remote_players();
}

void GameSession::step_fixed()
{
    const Vec2 move = input_.move_vector();
    stats_.last_move = move;
    const Vec2 pos = predictor_.step(move);
    renderer_.set_local_player(pos);

    const uint32_t seq = predictor_.last_seq();
    net_.send(encode_input(seq, client_tick_, move, 0, 0));
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
    stats_.server_tick = snapshot.server_tick;

    RenderSnapshot render{};
    render.run_time_ms = snapshot.run_time_ms;
    for (const PlayerNet& player : snapshot.players)
        render.positions.emplace(player.id, player.pos);

    // Reconcile our own prediction against the server's acknowledged input.
    if (stats_.welcomed && stats_.player_id < snapshot.acked_seq.size())
    {
        const uint32_t acked = snapshot.acked_seq[stats_.player_id];
        const PlayerNet* self = nullptr;
        for (const PlayerNet& player : snapshot.players)
            if (player.id == stats_.player_id) self = &player;

        if (self != nullptr && acked != stats_.last_acked_seq)
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
