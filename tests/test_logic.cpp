// Phase 1 unit tests for the pure client logic: packet encode/decode, the
// prediction ring buffer and reconciliation, the interpolation buffer, and the
// keyboard mapping. Everything here is Ogre-free so it builds and runs as a
// plain console test.

#include "entity_frame.h"
#include "game_math.h"
#include "input_state.h"
#include "interpolation.h"
#include "prediction.h"
#include "protocol.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

constexpr float kPi = 3.14159265358979323846f;

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

/// The counts in a snapshot header come off the wire, so they are
/// attacker-controlled: the decoder must size-check them against the bytes it
/// actually received before it reserves anything, and it must not accept a
/// payload longer than the records it declares.
void test_snapshot_decoder_validation()
{
    std::printf("snapshot decoder validation\n");

    // A minimal but complete payload: one player and one enemy.
    auto build = []() {
        std::vector<uint8_t> payload;
        auto u8 = [&](uint8_t v) { payload.push_back(v); };
        auto u16 = [&](uint16_t v) { payload.push_back(v & 0xFF); payload.push_back((v >> 8) & 0xFF); };
        auto i16 = [&](int16_t v) { u16(static_cast<uint16_t>(v)); };
        auto u32 = [&](uint32_t v) { for (int s = 0; s < 32; s += 8) payload.push_back((v >> s) & 0xFF); };

        u32(600); u32(0); u32(1000);
        u32(0); u32(0); u32(0); u32(0);
        u8(0);
        u16(1);  // players
        u16(1);  // enemies
        u16(0);  // projectiles
        u16(0);  // gems
        // PlayerNet
        u8(0); u8(1); i16(0); i16(0); u8(0); u8(100);
        // EnemyNet
        u32(7); u8(0); u8(0); i16(0); i16(0); u8(0); u8(100);
        return payload;
    };
    const size_t counts_offset = 29;
    const size_t enemy_count_offset = counts_offset + 2;

    std::vector<uint8_t> payload = build();
    expect(payload.size() == haven::kSnapshotHeaderSize + haven::kPlayerRecordSize + haven::kEnemyRecordSize,
           "the fixture matches the documented record sizes");
    haven::SnapshotFull good{};
    expect(haven::decode_snapshot_full(payload.data(), payload.size(), good), "a complete payload decodes");
    expect(good.players.size() == 1 && good.enemies.size() == 1, "the fixture arrays decode");

    // One extra byte past the last record is a framing error, not padding.
    std::vector<uint8_t> trailing = payload;
    trailing.push_back(0);
    haven::SnapshotFull with_trailing{};
    expect(!haven::decode_snapshot_full(trailing.data(), trailing.size(), with_trailing),
           "trailing garbage is rejected");
    expect(with_trailing.enemies.empty(), "a rejected payload leaves nothing behind");

    // An inflated count must be refused before the vector is resized: the
    // decoder has to see that 65,535 enemy records are not in the payload.
    for (uint16_t inflated : {static_cast<uint16_t>(2), static_cast<uint16_t>(1000),
                              static_cast<uint16_t>(65535)})
    {
        std::vector<uint8_t> lying = payload;
        lying[enemy_count_offset] = static_cast<uint8_t>(inflated & 0xFF);
        lying[enemy_count_offset + 1] = static_cast<uint8_t>((inflated >> 8) & 0xFF);
        haven::SnapshotFull rejected{};
        expect(!haven::decode_snapshot_full(lying.data(), lying.size(), rejected),
               "a count larger than the payload is rejected");
        expect(rejected.enemies.capacity() == 0,
               "the rejected count never reached a resize, so nothing was allocated for it");
    }

    // Every count is checked, not only the enemy one.
    for (size_t field = 0; field < 4; ++field)
    {
        std::vector<uint8_t> lying = payload;
        lying[counts_offset + field * 2] = 0xFF;
        lying[counts_offset + field * 2 + 1] = 0xFF;
        haven::SnapshotFull rejected{};
        expect(!haven::decode_snapshot_full(lying.data(), lying.size(), rejected),
               "an inflated count is rejected for every array");
    }

    // A header that claims fewer records than the payload carries is just as
    // wrong as one that claims more.
    std::vector<uint8_t> short_counts = payload;
    short_counts[enemy_count_offset] = 0;
    short_counts[enemy_count_offset + 1] = 0;
    haven::SnapshotFull understated{};
    expect(!haven::decode_snapshot_full(short_counts.data(), short_counts.size(), understated),
           "an understated count leaves unread bytes and is rejected");

    // The declared bytes must be exactly what the header describes: a payload
    // cut short of its last record is rejected too.
    std::vector<uint8_t> cut(payload.begin(), payload.end() - 1);
    haven::SnapshotFull short_payload{};
    expect(!haven::decode_snapshot_full(cut.data(), cut.size(), short_payload),
           "a payload one byte short of its records is rejected");
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

void test_first_seen_position_hands_over_continuously()
{
    std::printf("an unbracketed entity hands over to interpolation without jumping\n");
    // The window interpolates over the pair of snapshots bracketing a render
    // time that sits 100 ms behind the newest snapshot, so an entity that
    // appeared inside the last few intervals is in neither of that pair and
    // cannot be interpolated. It still has to be drawn, and where it is drawn
    // must match where interpolation starts once the window catches up.
    //
    // Drawing it at its *newest* position is what goes wrong: while the entity
    // is only a snapshot or two old, the newest snapshot can be two intervals
    // ahead of the render time, so the entity is held forward and then snaps
    // backwards when the bracket reaches it. Holding the first-seen position
    // instead hands over continuously, because that is exactly where
    // interpolation starts.
    haven::InterpolationBuffer buffer;
    haven::RenderSnapshot a;
    a.run_time_ms = 0;
    a.positions = {{1, {0.0f, 0.0f}}};
    haven::RenderSnapshot b;
    b.run_time_ms = 50;
    b.positions = {{1, {5.0f, 0.0f}}};
    haven::RenderSnapshot c;
    c.run_time_ms = 100;
    // Entity 2 appears here and keeps moving.
    c.positions = {{1, {10.0f, 0.0f}}, {2, {50.0f, 0.0f}}};
    haven::RenderSnapshot d;
    d.run_time_ms = 150;
    d.positions = {{1, {15.0f, 0.0f}}, {2, {60.0f, 0.0f}}};
    haven::RenderSnapshot e;
    e.run_time_ms = 200;
    e.positions = {{1, {20.0f, 0.0f}}, {2, {70.0f, 0.0f}}};

    // Only entity 2 carries a style, so `collect_draw_items` skips entity 1 and
    // the drawn set is exactly the entity under test.
    haven::EntityStyle projectile;
    projectile.kind = haven::EntityKind::Projectile;
    c.styles.emplace(2, projectile);
    d.styles.emplace(2, projectile);
    e.styles.emplace(2, projectile);

    buffer.push(a);
    buffer.advance(0.100f);
    buffer.push(b);
    buffer.advance(0.100f);
    buffer.push(c);
    buffer.advance(0.100f);
    buffer.push(d);
    buffer.advance(0.100f);
    buffer.push(e);

    // t_render = 200 (newest) + 0 elapsed - 100 = 100, so the window brackets
    // (b, c) and entity 2 is missing from b.
    haven::Vec2 held{0.0f, 0.0f};
    expect(!buffer.sample(2, held), "an unbracketed entity cannot be interpolated");

    const auto newest = buffer.latest_positions().find(2);
    expect(newest != buffer.latest_positions().end(), "the newest snapshot carries it");
    expect_near(newest->second.x, 70.0f, 1e-6f, "its newest position is two intervals ahead");

    expect(buffer.first_seen_position(2, held), "its first-seen position is available");
    expect_near(held.x, 50.0f, 1e-6f, "held at the position it appeared at, not the newest one");

    // What the renderer actually draws is what matters, so check it rather than
    // only the buffer query: `collect_draw_items` must pick the first-seen
    // position, not the newest one.
    std::vector<haven::EntityDrawItem> items;
    haven::collect_draw_items(buffer, items);
    const haven::EntityDrawItem* drawn = nullptr;
    for (const haven::EntityDrawItem& item : items)
        if (item.key == 2) drawn = &item;
    expect(drawn != nullptr, "the unbracketed entity is drawn rather than dropped");
    expect_near(drawn->pos.x, 50.0f, 1e-6f, "drawn where it appeared, not two intervals ahead");

    // The window catches up: one millisecond later the bracket is (c, d), which
    // contains the entity, and interpolation there starts from c — the very
    // position it was held at, so nothing moves backwards.
    buffer.advance(0.001f);
    haven::Vec2 bracketed{0.0f, 0.0f};
    expect(buffer.sample(2, bracketed), "the entity is bracketed once the window reaches it");
    expect_near(bracketed.x, 50.2f, 0.5f, "interpolation starts where the entity was held");
    expect(bracketed.x >= held.x, "the handover never moves the entity backwards");
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

/// Records what a geometry builder emitted so the tests can check vertex and
/// index counts and winding without an Ogre. The colour is not recorded: it is
/// the material's, so the builders do not carry one.
class RecordingSink : public haven::PrimitiveSink {
public:
    uint32_t base_index() const override { return static_cast<uint32_t>(vertices.size() / 3); }
    void vertex(float x, float y, float z) override
    {
        vertices.push_back(x);
        vertices.push_back(y);
        vertices.push_back(z);
    }
    void index(uint32_t index) override { indices.push_back(index); }

    std::vector<float> vertices;
    std::vector<uint32_t> indices;
};

void test_entity_keys()
{
    std::printf("entity keys\n");
    // An enemy and a projectile from separate server pools can share an id;
    // the namespaced keys must not collide, and the raw id survives.
    const uint32_t enemy_key = haven::interpolation_key(5, haven::EntityKind::Enemy);
    const uint32_t proj_key = haven::interpolation_key(5, haven::EntityKind::Projectile);
    expect(enemy_key != proj_key, "enemy and projectile id 5 get different keys");
    expect(haven::key_kind(enemy_key) == haven::EntityKind::Enemy, "enemy key kind survives");
    expect(haven::key_kind(proj_key) == haven::EntityKind::Projectile, "projectile key kind survives");
    expect(haven::key_raw_id(enemy_key) == 5 && haven::key_raw_id(proj_key) == 5, "raw id survives");
    const uint32_t player_key = haven::interpolation_key(2, haven::EntityKind::Player);
    expect(haven::key_kind(player_key) == haven::EntityKind::Player, "player key kind survives");
    expect(haven::key_raw_id(player_key) == 2, "player raw id survives");
    // A large id is not truncated: the pools are far below 2^30 but the mask
    // must not silently wrap one id onto another.
    const uint32_t big = (1u << haven::kKindShift) - 1u;
    expect(haven::key_raw_id(haven::interpolation_key(big, haven::EntityKind::Enemy)) == big,
           "a 30 bit id is not truncated");
}

void test_circle_and_triangle_primitives()
{
    std::printf("primitive builders\n");
    RecordingSink sink;
    haven::append_circle(sink, {10.0f, -4.0f}, 0.5f, 0.03f, 4);
    // A 4 segment circle is a centre vertex, 4 ring vertices and 4 triangles.
    expect(sink.vertices.size() == 5 * 3, "circle emits 5 vertices");
    expect(sink.indices.size() == 4 * 3, "circle emits 4 triangles");
    expect_near(sink.vertices[0], 10.0f, 1e-6f, "circle centre vertex x");
    expect_near(sink.vertices[1], -4.0f, 1e-6f, "circle centre vertex y");
    // First ring vertex sits at angle 0, i.e. one radius to the right.
    expect_near(sink.vertices[3], 10.5f, 1e-6f, "ring vertex 0 is at angle 0");
    expect_near(sink.vertices[4], -4.0f, 1e-6f, "ring vertex 0 keeps the centre y");
    // Counter-clockwise winding: the cross product of the first two edges is
    // positive, which Ogre reads as the front face.
    const float ax = sink.vertices[3] - sink.vertices[0];
    const float ay = sink.vertices[4] - sink.vertices[1];
    const float bx = sink.vertices[6] - sink.vertices[0];
    const float by = sink.vertices[7] - sink.vertices[1];
    expect(ax * by - ay * bx > 0.0f, "circle is wound counter-clockwise");

    RecordingSink triangle;
    haven::append_triangle(triangle, {0.0f, 0.0f}, 1.0f, kPi * 0.5f, 0.0f);
    expect(triangle.vertices.size() == 3 * 3, "triangle emits 3 vertices");
    expect(triangle.indices.size() == 3, "triangle emits one triangle");
    // The first vertex is at +angle: pi/2 points up.
    expect_near(triangle.vertices[0], 0.0f, 1e-5f, "triangle vertex 0 x at angle pi/2");
    expect_near(triangle.vertices[1], 1.0f, 1e-5f, "triangle vertex 0 points up");
    const float e1x = triangle.vertices[3] - triangle.vertices[0];
    const float e1y = triangle.vertices[4] - triangle.vertices[1];
    const float e2x = triangle.vertices[6] - triangle.vertices[0];
    const float e2y = triangle.vertices[7] - triangle.vertices[1];
    expect(e1x * e2y - e1y * e2x > 0.0f, "triangle is wound counter-clockwise");
}

void test_arc_and_hp_bar()
{
    std::printf("arcs and hp bars\n");
    RecordingSink arc;
    // A 90 degree arc facing +X: the ring starts at -45 degrees and ends at +45.
    haven::append_arc(arc, {0.0f, 0.0f}, 3.5f, 0.0f, kPi * 0.5f, 0.0f, 4);
    // A 4 interval arc is a centre vertex plus 5 ring vertices, so the last
    // ring vertex lands exactly on the far edge of the wedge.
    expect(arc.vertices.size() == 6 * 3, "arc emits a centre and 5 ring vertices");
    expect_near(arc.vertices[3], 3.5f * std::cos(-kPi * 0.25f), 1e-5f, "arc starts 45 degrees left of aim");
    expect_near(arc.vertices[4], 3.5f * std::sin(-kPi * 0.25f), 1e-5f, "arc start y");
    const float last_x = arc.vertices[(6 - 1) * 3];
    const float last_y = arc.vertices[(6 - 1) * 3 + 1];
    expect_near(last_x, 3.5f * std::cos(kPi * 0.25f), 1e-5f, "arc ends 45 degrees right of aim");
    expect_near(last_y, 3.5f * std::sin(kPi * 0.25f), 1e-5f, "arc end y");
    // The wedge must be symmetric about the aim direction.
    expect_near(std::fabs(arc.vertices[4]), std::fabs(arc.vertices[(6 - 1) * 3 + 1]), 1e-5f,
               "arc is symmetric about the aim direction");
}

/// Checks the *triangles*, not just the ring they are built from.
///
/// An arc is a fan from the centre into `segments + 1` ring vertices, so the
/// two ring indices of triangle `i` have to be the consecutive pair `i` and
/// `i + 1`. The first implementation wrapped the second index with
/// `% segments` — correct for a closed circle, wrong for an open wedge — which
/// left the final interval unpainted and drew a stray sliver from the last ring
/// vertex back to the first. Checking the first and last ring vertices cannot
/// see either symptom, so this walks the index buffer.
void test_arc_triangulation()
{
    std::printf("arc triangulation\n");
    const uint32_t segments = 4;
    const float radius = 3.5f;
    RecordingSink arc;
    haven::append_arc(arc, {0.0f, 0.0f}, radius, 0.0f, kPi * 0.5f, 0.0f, segments);

    const uint32_t ring_vertices = segments + 1;
    expect(arc.vertices.size() == (1 + ring_vertices) * 3, "arc emits a centre and segments+1 ring vertices");
    expect(arc.indices.size() == segments * 3, "arc emits one triangle per interval");

    // Every triangle is a fan triangle over one consecutive pair of ring
    // vertices, and those pairs must be exactly the intervals 1..segments.
    std::vector<uint32_t> interval_starts;
    for (size_t t = 0; t + 2 < arc.indices.size(); t += 3)
    {
        const uint32_t a = arc.indices[t];
        const uint32_t b = arc.indices[t + 1];
        const uint32_t c = arc.indices[t + 2];
        expect(a == 0, "every fan triangle starts at the centre vertex");
        const uint32_t lo = std::min(b, c);
        const uint32_t hi = std::max(b, c);
        expect(hi <= ring_vertices, "no triangle indexes past the ring");
        expect(hi == lo + 1, "each triangle spans one consecutive pair of ring vertices");
        interval_starts.push_back(lo);
    }
    std::sort(interval_starts.begin(), interval_starts.end());
    interval_starts.erase(std::unique(interval_starts.begin(), interval_starts.end()), interval_starts.end());
    expect(interval_starts.size() == segments, "no interval is covered twice");
    bool covers_every_interval = interval_starts.size() == segments;
    for (uint32_t i = 0; covers_every_interval && i < segments; ++i)
        covers_every_interval = interval_starts[i] == 1 + i;
    expect(covers_every_interval, "the triangles cover ring intervals 1..segments in order");
    expect(!interval_starts.empty() && interval_starts.back() == segments,
           "the final interval is covered, not folded back onto the first ring vertex");

    // The fan's area is the sum of its intervals, and every triangle has to turn
    // the same way: a folded triangle would cancel part of the total out.
    float signed_area = 0.0f;
    for (size_t t = 0; t + 2 < arc.indices.size(); t += 3)
    {
        const auto vx = [&arc](uint32_t i) { return arc.vertices[static_cast<size_t>(i) * 3]; };
        const auto vy = [&arc](uint32_t i) { return arc.vertices[static_cast<size_t>(i) * 3 + 1]; };
        const uint32_t a = arc.indices[t];
        const uint32_t b = arc.indices[t + 1];
        const uint32_t c = arc.indices[t + 2];
        signed_area += (vx(b) - vx(a)) * (vy(c) - vy(a)) - (vy(b) - vy(a)) * (vx(c) - vx(a));
    }
    const float step = (kPi * 0.5f) / static_cast<float>(segments);
    expect_near(signed_area * 0.5f,
                0.5f * radius * radius * static_cast<float>(segments) * std::sin(step), 1e-3f,
                "the fan area is the sum of its intervals");
}

void test_enemy_body_and_hp_bar()
{
    std::printf("enemy body and hp bar\n");
    RecordingSink bodies;
    RecordingSink bars;
    haven::EntityDrawItem item;
    item.kind = haven::EntityKind::Enemy;
    item.style = 3;  // Triangle T1
    item.pos = {2.0f, 3.0f};
    item.angle = kPi * 0.5f;
    item.hp_pct = 40;
    haven::append_enemy_body(bodies, item);
    haven::append_enemy_bar_background(bars, item);
    haven::append_enemy_bar(bars, item);

    expect(bodies.vertices.size() == 3 * 3, "a triangle archetype emits one triangle");
    // The triangle points where the enemy is moving: +Y.
    expect_near(bodies.vertices[1], 3.0f + 0.5f, 1e-5f, "the triangle points along its facing");

    // Two quads: the bar background and the fill.
    expect(bars.vertices.size() == 8 * 3, "the hp bar emits two quads");
    expect(bars.indices.size() == 12, "the hp bar emits two triangles pairs");
    // The bar floats above the body: its lower edge is half a bar height
    // below the centre it was given.
    const float bar_low = bars.vertices[1];
    const float bar_high = bars.vertices[2 * 3 + 1];
    expect_near(bar_high - bar_low, 0.14f, 1e-5f, "the bar height");
    expect_near((bar_low + bar_high) * 0.5f, 3.0f + 0.5f + 0.30f, 1e-5f, "the bar sits above the body radius");
    const float background_width = std::fabs(bars.vertices[3] - bars.vertices[0]);
    expect_near(background_width, haven::enemy_hp_bar_width(3), 1e-5f, "the background spans the bar width");
    // The fill is left anchored and shrinks with hp_pct: 40% of the width.
    const float fill_width = std::fabs(bars.vertices[5 * 3] - bars.vertices[4 * 3]);
    expect(fill_width < background_width, "the fill is narrower than the background at 40% hp");
    expect_near(fill_width / background_width, 0.4f, 0.02f, "the fill width tracks hp_pct");
    // The fill starts at the left edge of the background.
    expect_near(bars.vertices[4 * 3], bars.vertices[0], 1e-4f, "the fill is left anchored");
}

void test_enemy_radius_and_shape_tables()
{
    std::printf("archetype tables\n");
    expect_near(haven::enemy_radius(0), 0.5f, 0.0f, "Circle T1 radius");
    expect_near(haven::enemy_radius(1), 0.9f, 0.0f, "Circle T2 radius");
    expect_near(haven::enemy_radius(3), 0.5f, 0.0f, "Triangle T1 radius");
    expect_near(haven::enemy_radius(5), 1.0f, 0.0f, "Triangle T3 radius");
    expect(haven::archetype_is_circle(0) && !haven::archetype_is_circle(3), "circles and triangles told apart");
    expect(haven::archetype_colour(0).r > haven::archetype_colour(3).r, "tier 1 circle and triangle differ");
    expect(haven::enemy_hp_bar_width(0) > 0.5f, "the hp bar is wider than the body");
    expect(haven::enemy_hp_bar_width(2) < 3.6f, "the hp bar width is bounded for a T3");
}

void test_colour_slots()
{
    std::printf("colour slots\n");
    // Each archetype gets its own slot, and unknown archetypes fall back to the
    // first one rather than running off the table.
    for (uint8_t archetype = 0; archetype < 7; ++archetype)
    {
        haven::EntityDrawItem item;
        item.kind = haven::EntityKind::Enemy;
        item.style = archetype;
        const haven::ColourSlot slot = haven::slot_for_body(item);
        expect(slot.pool == haven::ColourPool::EnemyBody, "the body slot is in the body pool");
        expect(static_cast<uint8_t>(slot.slot) == archetype, "each archetype has its own slot");
    }
    haven::EntityDrawItem unknown;
    unknown.kind = haven::EntityKind::Enemy;
    unknown.style = 99;
    expect(haven::slot_for_body(unknown).slot == 0, "an unknown archetype falls back to slot 0");

    // HP drives the bar slot: empty and full land at opposite ends, and a
    // middle value lands somewhere between.
    haven::EntityDrawItem empty_enemy = unknown;
    empty_enemy.hp_pct = 0;
    haven::EntityDrawItem full_enemy = unknown;
    full_enemy.hp_pct = 100;
    haven::EntityDrawItem half_enemy = unknown;
    half_enemy.hp_pct = 50;
    const haven::ColourSlot empty = haven::slot_for_bar(empty_enemy);
    const haven::ColourSlot full = haven::slot_for_bar(full_enemy);
    const haven::ColourSlot half = haven::slot_for_bar(half_enemy);
    expect(empty.slot == 1, "an empty bar uses the first fill slot");
    expect(full.slot == haven::kEnemyBarSlots - 1, "a full bar uses the last fill slot");
    expect(half.slot > empty.slot && half.slot < full.slot, "a half full bar lands between");
    expect(empty.pool == haven::ColourPool::EnemyBar, "the bar slot is in the bar pool");

    // Every slot has a colour, and the bar background is darker than any fill.
    for (uint8_t pool = 0; pool < 3; ++pool)
    {
        for (uint8_t slot = 0; slot < haven::kMaxSlotsPerPool; ++slot)
        {
            haven::ColourSlot key{static_cast<haven::ColourPool>(pool), slot};
            const haven::Rgba colour = haven::slot_colour(key);
            expect(colour.r >= 0.0f && colour.r <= 1.0f, "a slot colour is in range");
            expect(colour.a == 1.0f, "a slot colour is opaque");
        }
    }
    const haven::Rgba background = haven::slot_colour(haven::ColourSlot{haven::ColourPool::EnemyBar, 0});
    const haven::Rgba full_fill = haven::slot_colour(haven::ColourSlot{haven::ColourPool::EnemyBar, 9});
    expect(full_fill.g > background.g, "a full bar is brighter than its background");
}

void test_projectile_shapes()
{
    std::printf("projectile shapes\n");
    for (uint8_t weapon = 0; weapon < 4; ++weapon)
    {
        RecordingSink sink;
        haven::EntityDrawItem item;
        item.kind = haven::EntityKind::Projectile;
        item.style = weapon;
        item.pos = {0.0f, 0.0f};
        item.angle = 0.0f;
        // The shape byte the server's weapon table sends (shape 0 ARC, 1 CIRCLE,
        // 2 RAY): sword and axe are arcs, the bow is a ray and the gun a
        // travelling circle.
        item.shape = weapon < 2 ? 0 : (weapon == 2 ? 2 : 1);
        haven::append_projectile(sink, item);
        expect(sink.vertices.size() > 0, "every weapon emits geometry");
        expect(sink.indices.size() % 3 == 0, "geometry is made of whole triangles");
    }

    // The sword arc is narrower than the axe arc at the same range.
    RecordingSink sword;
    haven::EntityDrawItem sword_item;
    sword_item.kind = haven::EntityKind::Projectile;
    sword_item.style = 0;
    sword_item.shape = 0;
    sword_item.pos = {0.0f, 0.0f};
    sword_item.angle = 0.0f;
    haven::append_projectile(sword, sword_item);

    RecordingSink axe;
    haven::EntityDrawItem axe_item = sword_item;
    axe_item.style = 1;
    haven::append_projectile(axe, axe_item);

    const auto span = [](const RecordingSink& sink) {
        float low = 1e30f;
        float high = -1e30f;
        for (size_t i = 1; i < sink.vertices.size(); i += 3)
        {
            low = std::min(low, sink.vertices[i]);
            high = std::max(high, sink.vertices[i]);
        }
        return high - low;
    };
    expect(span(axe) > span(sword), "the axe arc spans more than the sword arc");

    // The bow's arrow is a ray: a quad running from the arrow to the end of its
    // reach, `reach` long and twice its radius wide.
    RecordingSink arrow;
    haven::EntityDrawItem arrow_item = sword_item;
    arrow_item.style = 2;
    arrow_item.shape = 2;
    haven::append_projectile(arrow, arrow_item);
    expect_near(arrow.vertices.size(), 4 * 3, 0.0f, "a ray arrow emits one quad");
    const haven::WeaponVisual& bow = haven::weapon_visual(2);
    expect_near(arrow.vertices[0], 0.0f, 1e-4f, "the ray starts at the arrow");
    expect_near(arrow.vertices[2 * 3], bow.reach, 1e-4f, "the ray runs a full reach ahead of it");

    // The gun's bullet is a circle of its projectile radius.
    RecordingSink bullet;
    haven::EntityDrawItem bullet_item = arrow_item;
    bullet_item.style = 3;
    bullet_item.shape = 1;
    haven::append_projectile(bullet, bullet_item);
    expect_near(bullet.vertices.size(), 11 * 3, 0.0f, "a 10 segment bullet emits 11 vertices");

    // A shape byte this client does not know must still draw something: an
    // invisible live projectile reads as a desync. The fallback is the body
    // circle, which the arc weapons need a radius floor for.
    const uint8_t unknown_shapes[] = {3, 7, 200, 255};
    for (uint8_t shape : unknown_shapes)
    {
        RecordingSink fallback;
        haven::EntityDrawItem unknown_item = sword_item;
        unknown_item.shape = shape;
        haven::append_projectile(fallback, unknown_item);
        expect(fallback.vertices.size() == 11 * 3, "an unknown shape draws the fallback circle");
        expect_near(fallback.vertices[0], unknown_item.pos.x, 1e-6f, "the fallback is centred on the projectile");
        expect(fallback.vertices[3] > 0.0f, "the fallback has a visible radius even for a zero radius weapon");

        // A projectile weapon keeps its own radius in the fallback.
        RecordingSink gun_fallback;
        haven::EntityDrawItem gun_item = unknown_item;
        gun_item.style = 3;
        haven::append_projectile(gun_fallback, gun_item);
        expect_near(gun_fallback.vertices[3], haven::weapon_visual(3).radius, 1e-6f,
                    "a projectile weapon keeps its own radius in the fallback");
    }
}

void test_collect_draw_items()
{
    std::printf("collect draw items\n");
    haven::InterpolationBuffer buffer;

    // An enemy and a projectile that share the raw id 5 must both survive.
    haven::RenderSnapshot first;
    first.run_time_ms = 0;
    first.positions = {{haven::interpolation_key(5, haven::EntityKind::Enemy), {0.0f, 0.0f}},
                       {haven::interpolation_key(5, haven::EntityKind::Projectile), {10.0f, 0.0f}}};
    first.styles = {{haven::interpolation_key(5, haven::EntityKind::Enemy),
                     haven::EntityStyle{haven::EntityKind::Enemy, 0, 100, 0, 0.0f}},
                    {haven::interpolation_key(5, haven::EntityKind::Projectile),
                     haven::EntityStyle{haven::EntityKind::Projectile, 2, 0, 1, 1.0f}}};

    // 100 ms of server time between the two snapshots, so the 100 ms
    // interpolation delay puts the render time exactly halfway between them
    // once 50 ms of render clock has passed.
    haven::RenderSnapshot second = first;
    second.run_time_ms = 100;
    second.positions = {{haven::interpolation_key(5, haven::EntityKind::Enemy), {20.0f, 0.0f}},
                        {haven::interpolation_key(5, haven::EntityKind::Projectile), {30.0f, 0.0f}}};

    buffer.push(first);
    buffer.push(second);
    buffer.advance(0.050f);

    const std::vector<haven::EntityDrawItem> items = haven::collect_draw_items(buffer);
    expect(items.size() == 2, "both the enemy and the projectile are drawn");
    bool found_enemy = false;
    bool found_projectile = false;
    for (const haven::EntityDrawItem& item : items)
    {
        expect(haven::key_raw_id(item.key) == 5, "the raw id survives round the buffer");
        if (item.kind == haven::EntityKind::Enemy)
        {
            found_enemy = true;
            expect_near(item.pos.x, 10.0f, 1e-6f, "the enemy is interpolated halfway");
            expect(item.style == 0 && item.hp_pct == 100, "the enemy style is carried");
        }
        else if (item.kind == haven::EntityKind::Projectile)
        {
            found_projectile = true;
            expect_near(item.pos.x, 20.0f, 1e-6f, "the projectile is interpolated halfway");
            expect(item.style == 2 && item.shape == 1, "the projectile style is carried");
        }
    }
    expect(found_enemy, "the enemy is drawn");
    expect(found_projectile, "the projectile is drawn");

    // The per-frame hot path fills a caller-owned vector instead of returning a
    // fresh one, and must clear it rather than append across frames.
    std::vector<haven::EntityDrawItem> reused;
    haven::collect_draw_items(buffer, reused);
    expect(reused.size() == 2, "the in-place collector fills the caller's vector");
    haven::collect_draw_items(buffer, reused);
    expect(reused.size() == 2, "a second in-place collect clears rather than appends");
}

void test_empty_buffer_collects_nothing()
{
    std::printf("empty buffer collects nothing\n");
    haven::InterpolationBuffer buffer;
    expect(haven::collect_draw_items(buffer).empty(), "an empty buffer draws nothing");

    // One snapshot alone cannot be interpolated, so nothing is drawn yet.
    haven::RenderSnapshot only;
    only.run_time_ms = 0;
    only.positions = {{haven::interpolation_key(1, haven::EntityKind::Enemy), {1.0f, 1.0f}}};
    buffer.push(only);
    expect(haven::collect_draw_items(buffer).empty(), "a single snapshot is not interpolated yet");
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
    test_snapshot_decoder_validation();
    test_sequence_numbers();
    test_input_history();
    test_prediction();
    test_reconcile_replays_unacked_inputs();
    test_reconcile_blends_small_errors();
    test_interpolation();
    test_interpolation_is_continuous_between_snapshots();
    test_first_seen_position_hands_over_continuously();
    test_reconcile_repeats_identical_acks();
    test_reconcile_tolerates_a_stale_ack();
    test_input_state();
    test_input_script();
    test_entity_keys();
    test_circle_and_triangle_primitives();
    test_arc_and_hp_bar();
    test_arc_triangulation();
    test_enemy_body_and_hp_bar();
    test_enemy_radius_and_shape_tables();
    test_colour_slots();
    test_projectile_shapes();
    test_collect_draw_items();
    test_empty_buffer_collects_nothing();

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
