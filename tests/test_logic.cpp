// Phase 1 unit tests for the pure client logic: packet encode/decode, the
// prediction ring buffer and reconciliation, the interpolation buffer, and the
// keyboard mapping. Everything here is Ogre-free so it builds and runs as a
// plain console test.

#include "game_math.h"
#include "input_state.h"
#include "interpolation.h"
#include "prediction.h"
#include "protocol.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

int g_failures = 0;
int g_checks = 0;

void expect(bool condition, const std::string& what)
{
    ++g_checks;
    if (condition) return;
    ++g_failures;
    std::printf("  FAIL: %s\n", what.c_str());
}

void expect_near(float actual, float expected, float tolerance, const std::string& what)
{
    ++g_checks;
    if (std::fabs(actual - expected) <= tolerance) return;
    ++g_failures;
    std::printf("  FAIL: %s (expected %.4f, got %.4f)\n", what.c_str(), expected, actual);
}

void expect_vec(const haven::Vec2& actual, const haven::Vec2& expected, float tolerance, const std::string& what)
{
    expect_near(actual.x, expected.x, tolerance, what + ".x");
    expect_near(actual.y, expected.y, tolerance, what + ".y");
}

void test_vec_math()
{
    std::printf("vec math\n");
    expect_near(haven::length({3.0f, 4.0f}), 5.0f, 1e-5f, "length(3,4) == 5");
    expect_vec(haven::normalized({3.0f, 4.0f}), {0.6f, 0.8f}, 1e-5f, "normalized(3,4)");
    expect_vec(haven::normalized({0.0f, 0.0f}), {0.0f, 0.0f}, 0.0f, "normalized(0,0) is a no-op");
    expect_vec(haven::clamped_to_arena({1500.0f, -1500.0f}),
               {haven::kArenaHalfSize, -haven::kArenaHalfSize}, 0.0f, "arena clamp");
    expect_vec(haven::lerp({0.0f, 0.0f}, {10.0f, 20.0f}, 0.25f), {2.5f, 5.0f}, 1e-5f, "lerp quarter");
}

void test_integrator()
{
    std::printf("kinematic integrator\n");
    // 12.0 units/sec at 60 Hz is 0.2 units per tick.
    expect_vec(haven::integrate({0.0f, 0.0f}, {1.0f, 0.0f}, haven::kTickSeconds), {0.2f, 0.0f}, 1e-5f,
               "one tick of +X");
    // A diagonal input is normalised, so it is not faster than a straight move.
    expect_vec(haven::integrate({0.0f, 0.0f}, {1.0f, 1.0f}, haven::kTickSeconds),
               {0.2f / std::sqrt(2.0f), 0.2f / std::sqrt(2.0f)}, 1e-5f, "diagonal is normalised");
    // Idle input must not move the player.
    expect_vec(haven::integrate({5.0f, 5.0f}, {0.0f, 0.0f}, haven::kTickSeconds), {5.0f, 5.0f}, 0.0f,
               "no input means no movement");
    // Arena walls stop the player rather than wrapping.
    haven::Vec2 pos{0.0f, 0.0f};
    for (int i = 0; i < 100000; ++i) pos = haven::integrate(pos, {1.0f, 0.0f}, haven::kTickSeconds);
    expect_near(pos.x, haven::kArenaHalfSize, 0.01f, "wall stops the player at +1000");
}

void test_position_quantisation()
{
    std::printf("position quantisation\n");
    expect_near(haven::decode_pos(haven::encode_pos(0.0f)), 0.0f, 0.0f, "0.0 round-trips exactly");
    expect_near(haven::decode_pos(haven::encode_pos(1000.0f)), 1000.0f, haven::kPositionQuantStep, "1000.0 round-trips");
    expect_near(haven::decode_pos(haven::encode_pos(-1000.0f)), -1000.0f, haven::kPositionQuantStep, "-1000.0 round-trips");
    expect_near(haven::decode_pos(haven::encode_pos(0.026f)), 0.05f, 0.0f, "0.026 rounds up to one step");
    expect(haven::encode_pos(0.0f) == 0, "encode_pos(0) == 0");
}

void test_angle_quantisation()
{
    std::printf("angle quantisation\n");
    for (float radians = -6.28f; radians <= 6.28f; radians += 0.1f)
    {
        const float decoded = haven::decode_angle(haven::encode_angle(radians));
        float diff = std::fabs(decoded - std::fmod(radians + 6.2831853f, 6.2831853f));
        if (diff > 3.14159265f) diff = 6.2831853f - diff;
        expect(diff < 0.03f, "angle round-trips within half a quantisation step");
    }
}

void test_hello_packet()
{
    std::printf("C2S_HELLO\n");
    std::vector<uint8_t> packet = haven::encode_hello(0x1122334455667788ULL, 0, "haven");
    // proto_ver(1) + requested_slot(1) + session_token(8) + name(16) = 26.
    expect(packet.size() == haven::kHeaderSize + 26, "hello is 7 byte header + 26 byte payload");

    haven::PacketHeader header{};
    expect(haven::decode_header(packet.data(), packet.size(), header), "header decodes");
    expect(header.msg_type == 0x01, "msg_type is C2S_HELLO");
    expect(header.payload_len == 26, "payload_len is 26");

    expect(packet[7] == 1, "proto_ver is 1");
    expect(packet[8] == 0, "requested_slot is 0");
    uint64_t token = 0;
    for (int i = 0; i < 8; ++i) token |= static_cast<uint64_t>(packet[9 + i]) << (8 * i);
    expect(token == 0x1122334455667788ULL, "session_token is little-endian");
    expect(packet[17] == 'h' && packet[18] == 'a' && packet[19] == 'v' && packet[20] == 'e' && packet[21] == 'n',
           "name is copied");
    expect(packet[22] == 0 && packet[26] == 0, "name is zero padded to 16 bytes");
}

void test_input_packet()
{
    std::printf("C2S_INPUT\n");
    std::vector<uint8_t> packet = haven::encode_input(42, 7, {0.5f, -0.5f}, 1000, 0);
    // input_seq(4) + client_tick(4) + move_x(1) + move_y(1) + aim_angle(2) + buttons(1) = 13.
    expect(packet.size() == haven::kHeaderSize + 13, "input is 7 byte header + 13 byte payload");

    haven::PacketHeader header{};
    expect(haven::decode_header(packet.data(), packet.size(), header), "header decodes");
    expect(header.msg_type == 0x02, "msg_type is C2S_INPUT");
    expect(header.seq == 42, "seq is the input sequence number");

    expect(packet[7] == 42 && packet[8] == 0 && packet[9] == 0 && packet[10] == 0, "input_seq is little-endian");
    expect(packet[11] == 7, "client_tick follows");

    const auto move_x = static_cast<int8_t>(packet[15]);
    const auto move_y = static_cast<int8_t>(packet[16]);
    expect(move_x == 50, "move_x 0.5 -> 50");
    expect(move_y == -50, "move_y -0.5 -> -50");
    expect(std::abs(static_cast<int>(move_x)) <= 100 && std::abs(static_cast<int>(move_y)) <= 100,
           "move components stay inside [-100, 100]");

    const uint16_t aim = static_cast<uint16_t>(packet[17] | (packet[18] << 8));
    expect(aim == 1000, "aim_angle round-trips");
    expect(packet[19] == 0, "buttons is 0");
}

void test_welcome_packet()
{
    std::printf("S2C_WELCOME\n");
    std::vector<uint8_t> payload;
    auto append_u8 = [&](uint8_t v) { payload.push_back(v); };
    auto append_u16 = [&](uint16_t v) { payload.push_back(v & 0xFF); payload.push_back((v >> 8) & 0xFF); };
    auto append_u32 = [&](uint32_t v) {
        for (int s = 0; s < 32; s += 8) payload.push_back((v >> s) & 0xFF);
    };
    append_u8(1);
    for (int s = 0; s < 64; s += 8) payload.push_back((0xAABBCCDDEEFF0011ULL >> s) & 0xFF);
    append_u8(60);
    append_u8(20);
    append_u16(1000);
    append_u32(12345);

    haven::Welcome welcome{};
    expect(haven::decode_welcome(payload.data(), payload.size(), welcome), "welcome decodes");
    expect(welcome.player_id == 1, "player_id");
    expect(welcome.session_token == 0xAABBCCDDEEFF0011ULL, "session_token");
    expect(welcome.tick_hz == 60, "tick_hz is 60");
    expect(welcome.snap_hz == 20, "snap_hz is 20");
    expect(welcome.arena_half_size == 1000, "arena_half_size is 1000");
    expect(welcome.server_tick == 12345, "server_tick");

    std::vector<uint8_t> truncated(payload.begin(), payload.end() - 1);
    haven::Welcome broken{};
    expect(!haven::decode_welcome(truncated.data(), truncated.size(), broken), "truncated welcome is rejected");
}

void test_snapshot_packet()
{
    std::printf("S2C_SNAPSHOT_FULL\n");
    std::vector<uint8_t> payload;
    auto append_u8 = [&](uint8_t v) { payload.push_back(v); };
    auto append_u16 = [&](uint16_t v) { payload.push_back(v & 0xFF); payload.push_back((v >> 8) & 0xFF); };
    auto append_i16 = [&](int16_t v) { append_u16(static_cast<uint16_t>(v)); };
    auto append_u32 = [&](uint32_t v) {
        for (int s = 0; s < 32; s += 8) payload.push_back((v >> s) & 0xFF);
    };

    append_u32(600);    // server_tick
    append_u32(0);      // baseline_tick
    append_u32(10000);  // run_time_ms
    append_u32(9);      // acked_seq[0]
    append_u32(0);
    append_u32(0);
    append_u32(0);
    append_u8(0);       // game_state
    append_u16(2);      // players
    append_u16(1);      // enemies
    append_u16(0);      // projs
    append_u16(1);      // gems

    append_u8(0);                 // player id
    append_u8(1);                 // state_flags
    append_i16(haven::encode_pos(12.34f));
    append_i16(haven::encode_pos(-5.6f));
    append_u8(haven::encode_angle(1.5f));
    append_u8(100);

    append_u8(1);
    append_u8(0);
    append_i16(haven::encode_pos(-40.0f));
    append_i16(haven::encode_pos(20.0f));
    append_u8(haven::encode_angle(0.0f));
    append_u8(50);

    append_u32(77);               // enemy id
    append_u8(1);                 // archetype
    append_u8(0);                 // elite_mask
    append_i16(haven::encode_pos(30.0f));
    append_i16(haven::encode_pos(30.0f));
    append_u8(haven::encode_angle(0.5f));
    append_u8(75);

    append_u32(9);                // gem id
    append_i16(haven::encode_pos(1.0f));
    append_i16(haven::encode_pos(2.0f));
    append_u16(2);                // xp_value
    append_u8(0xFF);              // magnet_target_id
    append_u8(0);                 // reserved

    haven::SnapshotFull snapshot{};
    expect(haven::decode_snapshot_full(payload.data(), payload.size(), snapshot), "snapshot decodes");
    expect(snapshot.server_tick == 600, "server_tick");
    expect(snapshot.run_time_ms == 10000, "run_time_ms");
    expect(snapshot.acked_seq[0] == 9, "acked_seq[0]");
    expect(snapshot.player_count == 2 && snapshot.enemy_count == 1 && snapshot.proj_count == 0 &&
               snapshot.gem_count == 1,
           "counts match the arrays");
    expect(snapshot.players.size() == 2, "two players decoded");
    expect_near(snapshot.players[0].pos.x, 12.35f, 0.01f, "player 0 pos.x dequantised");
    expect_near(snapshot.players[0].pos.y, -5.6f, 0.01f, "player 0 pos.y dequantised");
    expect_near(snapshot.players[1].pos.x, -40.0f, 0.01f, "player 1 pos.x dequantised");
    expect(snapshot.enemies.size() == 1 && snapshot.enemies[0].id == 77, "enemy decoded");
    expect(snapshot.gems.size() == 1 && snapshot.gems[0].xp_value == 2, "gem decoded");

    std::vector<uint8_t> truncated(payload.begin(), payload.end() - 1);
    haven::SnapshotFull broken{};
    expect(!haven::decode_snapshot_full(truncated.data(), truncated.size(), broken), "truncated snapshot is rejected");
}

void test_interpolation_is_continuous_between_snapshots()
{
    std::printf("interpolation is continuous between snapshots\n");
    // The render clock advances on its own, so remote entities move smoothly
    // between snapshot arrivals instead of holding still and then jumping.
    haven::InterpolationBuffer buffer;
    haven::RenderSnapshot a;
    a.run_time_ms = 0;
    a.positions = {{1, {0.0f, 0.0f}}};
    haven::RenderSnapshot b;
    b.run_time_ms = 50;
    b.positions = {{1, {10.0f, 0.0f}}};
    haven::RenderSnapshot c;
    c.run_time_ms = 100;
    c.positions = {{1, {20.0f, 0.0f}}};

    buffer.push(a);
    buffer.advance(0.100f);
    buffer.push(b);
    buffer.advance(0.100f);
    buffer.push(c);

    // t_render = 100 (newest server time) + 0 elapsed - 100 ms delay = 0.
    haven::Vec2 first{0.0f, 0.0f};
    haven::Vec2 second{0.0f, 0.0f};
    haven::Vec2 third{0.0f, 0.0f};
    expect(buffer.sample(1, first), "sample succeeds");
    expect_near(first.x, 0.0f, 1e-6f, "render time 0 is snapshot a");

    buffer.advance(0.025f);
    expect(buffer.sample(1, second), "sample succeeds 25 ms later");
    buffer.advance(0.025f);
    expect(buffer.sample(1, third), "sample succeeds 50 ms later");

    expect(second.x > first.x, "25 ms of render time moves the entity forward");
    expect(third.x > second.x, "a further 25 ms moves it further");
    expect_near(second.x, 5.0f, 1e-6f, "halfway between a and b");
    expect_near(third.x, 10.0f, 1e-6f, "render time 50 lands on snapshot b");

    // Without clock advance the answer is stable; the clock, not the snapshot
    // set, is what drives motion.
    haven::Vec2 again{0.0f, 0.0f};
    expect(buffer.sample(1, again) && std::fabs(again.x - third.x) < 1e-6f,
           "no clock advance means no movement");

    // A stalled frame must not fling the render time forward.
    const int64_t before_stall = buffer.render_time_ms();
    buffer.advance(10.0f);
    const int64_t after_stall = buffer.render_time_ms();
    expect(after_stall - before_stall == 250, "a 10 s stall advances the clock by the 250 ms clamp");
    expect(!buffer.sample(1, again),
           "running past the newest snapshot reports nothing to draw, so the entity holds still");
}

void test_reconcile_repeats_identical_acks()
{
    std::printf("reconciliation applies repeated identical acks\n");
    // The server may re-send the same acked sequence with a corrected position.
    // Reconciling only when the ack advances would leave the residual error in
    // place forever, so the client must fold every snapshot in.
    haven::ClientPredictor predictor;
    for (int i = 0; i < 10; ++i) predictor.step({1.0f, 0.0f});
    const uint32_t acked = predictor.last_seq();
    const haven::Vec2 server_pos = predictor.position() - haven::Vec2{1.0f, 0.0f};

    haven::Vec2 previous = predictor.position();
    for (int i = 0; i < 40; ++i)
    {
        predictor.reconcile(acked, server_pos);
        expect(predictor.position().x < previous.x, "each identical ack blends further toward the server");
        previous = predictor.position();
    }
    expect(distance(predictor.position(), server_pos) < 0.01f,
           "repeated identical acks converge on the corrected position");

    // And the history is not re-replayed: nothing is left to replay.
    expect(predictor.history().empty(), "history is drained by the first reconcile");
}

void test_reconcile_tolerates_a_stale_ack()
{
    std::printf("reconciliation tolerates a stale ack\n");
    // A stale ack is only harmful if it is paired with an inconsistent
    // position. With a position that actually corresponds to the acked input
    // count, replaying the remainder must land exactly where the client
    // already is, so a repeated old ack is a no-op rather than a drag
    // backwards.
    haven::ClientPredictor predictor;
    for (int i = 0; i < 10; ++i) predictor.step({1.0f, 0.0f});
    const haven::Vec2 full = predictor.position();

    const uint32_t acked = 3;
    const haven::Vec2 position_at_ack = {static_cast<float>(acked) * 0.2f, 0.0f};
    predictor.reconcile(acked, position_at_ack);
    expect_near(predictor.position().x, full.x, 1e-3f, "a consistent stale ack is a no-op");
    expect(predictor.history().size() == 10 - acked, "only the unacked tail stays in the history");

    // Repeating it changes nothing further.
    predictor.reconcile(acked, position_at_ack);
    expect_near(predictor.position().x, full.x, 1e-3f, "a repeated consistent stale ack is still a no-op");
}

void test_sequence_numbers()
{
    std::printf("sequence numbers\n");
    expect(haven::seq_at_or_before(5, 5), "seq equal counts as at-or-before");
    expect(haven::seq_at_or_before(4, 5), "seq 4 is before 5");
    expect(!haven::seq_at_or_before(6, 5), "seq 6 is after 5");
    expect(haven::seq_at_or_before(0xFFFFFFFFu, 0), "wrap: 0xFFFFFFFF precedes 0");
    expect(!haven::seq_at_or_before(0, 0xFFFFFFFFu), "wrap: 0 follows 0xFFFFFFFF");
}

void test_input_history()
{
    std::printf("input history\n");
    haven::InputHistory history;
    for (uint32_t i = 1; i <= 5; ++i)
        history.push({i, {1.0f, 0.0f}, {static_cast<float>(i) * 0.2f, 0.0f}});

    expect(history.size() == 5, "five samples stored");
    expect(history.find(3) != nullptr && history.find(3)->seq == 3, "find locates seq 3");
    expect(history.find(99) == nullptr, "find returns null for an unknown seq");

    // Ring buffer must keep the newest 128 entries.
    haven::InputHistory overflow;
    for (uint32_t i = 1; i <= 200; ++i) overflow.push({i, {0.0f, 0.0f}, {0.0f, 0.0f}});
    expect(overflow.size() == haven::InputHistory::kCapacity, "ring buffer is capped at 128");
    expect(overflow.find(72) == nullptr, "oldest entries are evicted");
    expect(overflow.find(200) != nullptr, "newest entry survives");
    expect(overflow.at(0).seq == 73, "buffer is ordered oldest first");

    history.discard_at_or_before(3);
    expect(history.size() == 2, "discard_at_or_before(3) keeps 2 samples");
    expect(history.at(0).seq == 4, "remaining samples start at seq 4");
    expect(history.find(3) == nullptr, "acked samples are gone");
}

void test_prediction()
{
    std::printf("local prediction\n");
    haven::ClientPredictor predictor;
    const haven::Vec2 first = predictor.step({1.0f, 0.0f});
    expect_near(first.x, 0.2f, 1e-5f, "first step moves 0.2 units");
    expect(predictor.next_seq() == 2, "next_seq advances");
    expect(predictor.last_seq() == 1, "last_seq is the input just applied");

    haven::Vec2 pos = first;
    for (int i = 0; i < 59; ++i) pos = predictor.step({1.0f, 0.0f});
    expect_near(pos.x, 12.0f, 1e-3f, "60 ticks of +X at 12 units/sec is 12 units");
    expect(predictor.history().size() == 60, "history holds every input");

    // The server acknowledges everything: the prediction is already exact, so
    // reconciliation must not move it.
    predictor.reconcile(predictor.last_seq(), pos);
    expect_near(predictor.position().x, 12.0f, 1e-3f, "exact prediction is unchanged by reconciliation");
    expect(predictor.history().empty(), "history is drained once the server has caught up");

    // Server disagrees by a large amount: the client snaps.
    haven::ClientPredictor snap;
    for (int i = 0; i < 10; ++i) snap.step({1.0f, 0.0f});
    const haven::Vec2 before = snap.position();
    snap.reconcile(snap.last_seq(), {100.0f, 0.0f});
    expect_near(snap.position().x, 100.0f, 1e-3f, "large error snaps to the server position");
    expect(distance(before, snap.position()) > haven::kReconcileSnapThreshold, "the snap really was large");

    // Server disagrees by a small amount: the client blends part of the way.
    haven::ClientPredictor blend;
    for (int i = 0; i < 10; ++i) blend.step({1.0f, 0.0f});
    const haven::Vec2 blended_from = blend.position();
    blend.reconcile(blend.last_seq(), blended_from + haven::Vec2{0.05f, 0.0f});
    const float moved = std::fabs(blend.position().x - blended_from.x);
    expect(moved > 0.0f && moved < 0.05f, "small error blends rather than snaps");
    expect_near(moved, 0.05f * haven::kReconcileBlendRate, 1e-4f, "blend is 15% of the error");
}

void test_reconcile_replays_unacked_inputs()
{
    std::printf("reconciliation replays unacknowledged inputs\n");
    haven::ClientPredictor predictor;
    for (int i = 0; i < 10; ++i) predictor.step({1.0f, 0.0f});
    const uint32_t acked = predictor.last_seq() - 4;

    // Server says the player is at x = 100.0 having applied `acked` inputs; the
    // client must replay the remaining four on top of that. The error is far
    // above the snap threshold, so the corrected value is taken verbatim and
    // the replay can be checked exactly.
    predictor.reconcile(acked, {100.0f, 0.0f});
    expect_near(predictor.position().x, 100.0f + 4.0f * 0.2f, 1e-3f, "unacked inputs are replayed");
    expect(predictor.history().size() == 4, "only unacked inputs remain in the history");
}

void test_reconcile_blends_small_errors()
{
    std::printf("reconciliation blends small errors\n");
    haven::ClientPredictor predictor;
    for (int i = 0; i < 10; ++i) predictor.step({1.0f, 0.0f});
    const haven::Vec2 before = predictor.position();
    const haven::Vec2 authoritative = before - haven::Vec2{0.05f, 0.0f};

    predictor.reconcile(predictor.last_seq(), authoritative);
    const float moved = std::fabs(predictor.position().x - before.x);
    expect_near(moved, 0.05f * haven::kReconcileBlendRate, 1e-4f,
                "a 0.05 unit error is blended 15% rather than snapped");
    expect_near(predictor.position().x, before.x - 0.05f * haven::kReconcileBlendRate, 1e-4f,
                "the blend moves 15% of the way to the server position");
}

void test_interpolation()
{
    std::printf("interpolation buffer\n");
    expect_near(haven::blend_alpha(50, 0, 50), 1.0f, 1e-6f, "alpha at t_b is 1");
    expect_near(haven::blend_alpha(0, 0, 50), 0.0f, 1e-6f, "alpha at t_a is 0");
    expect_near(haven::blend_alpha(25, 0, 50), 0.5f, 1e-6f, "alpha halfway is 0.5");
    expect_near(haven::blend_alpha(10, 0, 50), 0.2f, 1e-6f, "alpha clamps below 1 before t_b");
    expect_near(haven::blend_alpha(0, 100, 100), 1.0f, 1e-6f, "degenerate interval yields 1");

    haven::InterpolationBuffer buffer;
    haven::RenderSnapshot a;
    a.run_time_ms = 0;
    a.positions = {{1, {0.0f, 0.0f}}};
    haven::RenderSnapshot b;
    b.run_time_ms = 50;
    b.positions = {{1, {10.0f, 0.0f}}};

    haven::Vec2 out{0.0f, 0.0f};
    expect(!buffer.sample(1, out), "a single snapshot cannot be interpolated");

    buffer.push(a);
    buffer.push(b);
    expect(buffer.size() == 2, "two snapshots buffered");
    expect(buffer.sample(1, out), "interpolation succeeds with two snapshots");
    // t_render = latest (50) - 100 ms delay = -50, clamped to t_a, so the
    // entity is drawn at its older position until the buffer has 3 snapshots.
    expect_near(out.x, 0.0f, 1e-6f, "render time before t_a clamps to t_a");

    haven::RenderSnapshot c;
    c.run_time_ms = 100;
    c.positions = {{1, {20.0f, 0.0f}}};
    buffer.push(c);
    expect(buffer.sample(1, out), "interpolation succeeds with three snapshots");
    // t_render = 100 - 100 = 0, which is exactly snapshot a.
    expect_near(out.x, 0.0f, 1e-6f, "render time exactly on t_a");

    haven::RenderSnapshot d;
    d.run_time_ms = 150;
    d.positions = {{1, {30.0f, 0.0f}}};
    buffer.push(d);
    expect(buffer.sample(1, out), "interpolation succeeds with four snapshots");
    // t_render = 150 - 100 = 50, which is exactly snapshot b.
    expect_near(out.x, 10.0f, 1e-6f, "render time exactly on t_b");

    haven::RenderSnapshot e;
    e.run_time_ms = 200;
    e.positions = {{1, {40.0f, 0.0f}}};
    buffer.push(e);
    expect(buffer.sample(1, out), "interpolation succeeds with five snapshots");
    // t_render = 200 - 100 = 100, exactly snapshot c.
    expect_near(out.x, 20.0f, 1e-6f, "render time exactly on t_c");

    haven::RenderSnapshot f;
    f.run_time_ms = 250;
    f.positions = {{1, {50.0f, 0.0f}}};
    buffer.push(f);
    expect(buffer.sample(1, out), "interpolation succeeds with six snapshots");
    // t_render = 250 - 100 = 150, exactly snapshot d.
    expect_near(out.x, 30.0f, 1e-6f, "render time exactly on t_d");

    // Between snapshots the position is a straight blend.
    haven::RenderSnapshot g;
    g.run_time_ms = 275;
    g.positions = {{1, {60.0f, 0.0f}}};
    buffer.push(g);
    expect(buffer.sample(1, out), "interpolation succeeds with seven snapshots");
    // t_render = 275 - 100 = 175, halfway between d (150) and e (200).
    expect_near(out.x, 35.0f, 1e-6f, "midpoint between d and e");

    haven::Vec2 missing{0.0f, 0.0f};
    expect(!buffer.sample(999, missing), "unknown entity is not interpolated");
}

void test_input_state()
{
    std::printf("input state\n");
    haven::InputState input;
    expect_vec(input.move_vector(), {0.0f, 0.0f}, 0.0f, "no keys held means no movement");

    auto key = [&input](int code, bool down) {
        if (down)
            input.key_down(code);
        else
            input.key_up(code);
    };

    key(haven::InputState::kKeyD, true);
    expect_vec(input.move_vector(), {1.0f, 0.0f}, 0.0f, "D moves right");
    key(haven::InputState::kKeyA, true);
    expect_vec(input.move_vector(), {0.0f, 0.0f}, 0.0f, "A and D cancel out");
    key(haven::InputState::kKeyD, false);
    expect_vec(input.move_vector(), {-1.0f, 0.0f}, 0.0f, "A moves left");
    key(haven::InputState::kKeyA, false);

    key(haven::InputState::kKeyW, true);
    expect_vec(input.move_vector(), {0.0f, 1.0f}, 0.0f, "W moves up");
    key(haven::InputState::kKeyS, true);
    expect_vec(input.move_vector(), {0.0f, 0.0f}, 0.0f, "W and S cancel out");
    key(haven::InputState::kKeyW, false);
    expect_vec(input.move_vector(), {0.0f, -1.0f}, 0.0f, "S moves down");
    key(haven::InputState::kKeyS, false);

    // Arrow keys must be independent of the letters: their keycodes are outside
    // the ASCII range and share no bits with a-z.
    key(haven::InputState::kKeyRight, true);
    expect_vec(input.move_vector(), {1.0f, 0.0f}, 0.0f, "arrow right moves right");
    key(haven::InputState::kKeyRight, false);
    key(haven::InputState::kKeyLeft, true);
    expect_vec(input.move_vector(), {-1.0f, 0.0f}, 0.0f, "arrow left moves left");
    key(haven::InputState::kKeyLeft, false);
    key(haven::InputState::kKeyUp, true);
    expect_vec(input.move_vector(), {0.0f, 1.0f}, 0.0f, "arrow up moves up");
    key(haven::InputState::kKeyUp, false);
    key(haven::InputState::kKeyDown, true);
    expect_vec(input.move_vector(), {0.0f, -1.0f}, 0.0f, "arrow down moves down");
    key(haven::InputState::kKeyDown, false);

    // Pressing a letter must not be confused with an arrow key.
    key('p', true);
    expect_vec(input.move_vector(), {0.0f, 0.0f}, 0.0f, "P is not an arrow key");
    key('p', false);

    expect(!input.quit_requested(), "escape has not been pressed");
    key(haven::InputState::kKeyEscape, true);
    expect(input.quit_requested(), "escape requests quit");
    input.consume_quit();
    expect(!input.quit_requested(), "consume_quit clears the flag");
}

void test_input_script()
{
    std::printf("input script\n");
    std::vector<haven::Vec2> moves;
    expect(haven::load_input_script("tests/data/input_script.txt", moves), "script loads");
    expect(moves.size() == 5, "script expands 1+2+2 ticks");
    expect_vec(moves[0], {1.0f, 0.0f}, 0.0f, "first tick is +X");
    expect_vec(moves[1], {0.0f, 1.0f}, 0.0f, "next two ticks are +Y");
    expect_vec(moves[2], {0.0f, 1.0f}, 0.0f, "second +Y tick");
    expect_vec(moves[3], {0.0f, 0.0f}, 0.0f, "idle ticks follow");
    expect_vec(moves[4], {0.0f, 0.0f}, 0.0f, "last idle tick");

    haven::InputState scripted;
    scripted.set_script(moves);
    expect_vec(scripted.move_vector(), {1.0f, 0.0f}, 0.0f, "script drives the first tick");
    expect_vec(scripted.move_vector(), {0.0f, 1.0f}, 0.0f, "script drives the second tick");
    scripted.set_script(moves);
    expect_vec(scripted.move_vector(), {1.0f, 0.0f}, 0.0f, "set_script rewinds");

    expect(!haven::load_input_script("tests/data/does_not_exist.txt", moves), "missing script is rejected");
    expect(!haven::load_input_script("tests/data/input_script_bad.txt", moves), "malformed script is rejected");
}

}  // namespace

int main()
{
    test_vec_math();
    test_integrator();
    test_position_quantisation();
    test_angle_quantisation();
    test_hello_packet();
    test_input_packet();
    test_welcome_packet();
    test_snapshot_packet();
    test_sequence_numbers();
    test_input_history();
    test_prediction();
    test_reconcile_replays_unacked_inputs();
    test_reconcile_blends_small_errors();
    test_interpolation();
    test_interpolation_is_continuous_between_snapshots();
    test_reconcile_repeats_identical_acks();
    test_reconcile_tolerates_a_stale_ack();
    test_input_state();
    test_input_script();

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
