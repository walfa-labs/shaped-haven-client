// Phase 1 unit tests for the pure client logic: packet encode/decode, the
// prediction ring buffer and reconciliation, the interpolation buffer, and the
// keyboard mapping. Everything here is Ogre-free so it builds and runs as a
// plain console test.

#include "entity_frame.h"
#include "game_math.h"
#include "hud_model.h"
#include "hud_text.h"
#include "input_state.h"
#include "interpolation.h"
#include "item_catalog.h"
#include "prediction.h"
#include "protocol.h"
#include "run_events.h"
#include "snapshot_ingest.h"
#include "snapshot_state.h"

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
    std::vector<uint8_t> packet = haven::encode_hello(0x1122334455667788ULL, 3, 2, "haven");
    // proto_ver(1) + race_id(1) + starting_weapon_id(1) + reserved(1) + name(16) = 20.
    expect(packet.size() == haven::kHeaderSize + 20, "hello is 7 byte header + 20 byte payload");

    haven::PacketHeader header{};
    expect(haven::decode_header(packet.data(), packet.size(), header), "header decodes");
    expect(header.msg_type == 0x01, "msg_type is C2S_HELLO");
    expect(header.payload_len == 20, "payload_len is 20");

    expect(packet[7] == 2, "proto_ver is 2");
    expect(packet[8] == 3, "race_id is 3");
    expect(packet[9] == 2, "starting_weapon_id is 2");
    expect(packet[10] == 0, "reserved is 0");
    expect(packet[11] == 'h' && packet[12] == 'a' && packet[13] == 'v' && packet[14] == 'e' && packet[15] == 'n',
           "name is copied");
    expect(packet[16] == 0 && packet[20] == 0, "name is zero padded to 16 bytes");
}

void test_input_packet()
{
    std::printf("C2S_INPUT\n");
    std::vector<uint8_t> packet = haven::encode_input(42, 7, 0x1F, 50, -50);
    // input_seq(4) + client_tick(4) + buttons(1) + axis_x(1) + axis_y(1) = 11.
    expect(packet.size() == haven::kHeaderSize + 11, "input is 7 byte header + 11 byte payload");

    haven::PacketHeader header{};
    expect(haven::decode_header(packet.data(), packet.size(), header), "header decodes");
    expect(header.msg_type == 0x02, "msg_type is C2S_INPUT");
    expect(header.seq == 42, "seq is the input sequence number");

    expect(packet[7] == 42 && packet[8] == 0 && packet[9] == 0 && packet[10] == 0, "input_seq is little-endian");
    expect(packet[11] == 7, "client_tick follows");

    expect(packet[15] == 0x1F, "buttons bitmask");
    const auto axis_x = static_cast<int8_t>(packet[16]);
    const auto axis_y = static_cast<int8_t>(packet[17]);
    expect(axis_x == 50, "axis_x 50");
    expect(axis_y == -50, "axis_y -50");
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

/// Phase 4 grows `S2C_WELCOME` from 17 to 21 bytes: `interest_radius` u16 at
/// offset 13 and `reserved` u16 at 15 push `server_tick` from 13 to 17. The
/// first 13 bytes are unchanged, so both layouts decode.
void test_welcome_packet_phase4()
{
    std::printf("S2C_WELCOME phase 4\n");
    std::vector<uint8_t> payload;
    auto append_u8 = [&](uint8_t v) { payload.push_back(v); };
    auto append_u16 = [&](uint16_t v) { payload.push_back(v & 0xFF); payload.push_back((v >> 8) & 0xFF); };
    auto append_u32 = [&](uint32_t v) {
        for (int s = 0; s < 32; s += 8) payload.push_back((v >> s) & 0xFF);
    };
    append_u8(2);  // player_id
    for (int s = 0; s < 64; s += 8) payload.push_back((0x1122334455667788ULL >> s) & 0xFF);
    append_u8(60);
    append_u8(20);
    append_u16(1000);
    append_u16(75);  // interest_radius
    append_u16(0);   // reserved
    append_u32(99999);

    expect(payload.size() == 21, "the phase-4 welcome is 21 bytes");
    haven::Welcome welcome{};
    expect(haven::decode_welcome(payload.data(), payload.size(), welcome), "phase-4 welcome decodes");
    expect(welcome.player_id == 2, "player_id");
    expect(welcome.session_token == 0x1122334455667788ULL, "session_token");
    expect(welcome.arena_half_size == 1000, "arena_half_size");
    expect(welcome.interest_radius == 75, "interest_radius");
    expect(welcome.server_tick == 99999, "server_tick moved to offset 17");
    expect(welcome.has_interest_radius, "the interest-radius field is flagged present");

    // The byte at offset 13 is the low byte of interest_radius, not of
    // server_tick: reading the old layout here would give the wrong tick.
    expect(payload[13] == 75 && payload[14] == 0, "interest_radius sits at offset 13");
    expect(payload[17] == 0x9F && payload[18] == 0x86 && payload[19] == 0x01, "server_tick starts at offset 17");

    // A 17 byte payload is still the phase-3 layout and must keep decoding, and
    // a payload that is neither length is rejected.
    std::vector<uint8_t> old_layout(payload.begin(), payload.begin() + 13);
    for (int s = 0; s < 32; s += 8) old_layout.push_back(static_cast<uint8_t>((12345u >> s) & 0xFF));
    haven::Welcome phase3{};
    expect(haven::decode_welcome(old_layout.data(), old_layout.size(), phase3), "a 17 byte welcome still decodes");
    expect(phase3.server_tick == 12345, "its server_tick is at offset 13");
    expect(!phase3.has_interest_radius, "and carries no interest radius");

    std::vector<uint8_t> wrong(payload.begin(), payload.end() - 1);
    haven::Welcome broken{};
    expect(!haven::decode_welcome(wrong.data(), wrong.size(), broken), "a 20 byte welcome is rejected");
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
    append_u8(3);                 // race_id
    append_i16(haven::encode_pos(12.34f));
    append_i16(haven::encode_pos(-5.6f));
    append_u8(haven::encode_angle(1.5f));
    append_u8(100);               // hp_pct
    append_u8(80);                // shield_pct
    append_u8(60);                // mana_pct
    append_u16(0);                // dash_cd_remaining
    append_u16(3000);             // dash_cd_total
    append_u8(0);                 // is_dashing
    append_u16(100);              // max_hp
    append_u16(50);               // max_shield

    append_u8(1);                 // player id
    append_u8(1);                 // state_flags
    append_u8(0);                 // race_id
    append_i16(haven::encode_pos(-40.0f));
    append_i16(haven::encode_pos(20.0f));
    append_u8(haven::encode_angle(0.0f));
    append_u8(90);                // hp_pct
    append_u8(0);                 // shield_pct
    append_u8(100);               // mana_pct
    append_u16(1500);             // dash_cd_remaining
    append_u16(3000);             // dash_cd_total
    append_u8(1);                 // is_dashing
    append_u16(120);              // max_hp
    append_u16(0);                // max_shield

    append_u32(77);               // enemy id
    append_u8(1);                 // archetype
    append_u8(0);                 // elite_mask
    append_i16(haven::encode_pos(30.0f));
    append_i16(haven::encode_pos(30.0f));
    append_u8(haven::encode_angle(0.5f));
    append_u8(75);                // hp_pct
    append_u8(0);                 // telegraph_progress
    append_u8(0);                 // telegraph_shape
    append_u16(0);                // telegraph_angle
    append_u16(0);                // telegraph_radius

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
        // PlayerNet (20 bytes)
        u8(0); u8(1); u8(0); i16(0); i16(0); u8(0); u8(100);
        u8(0); u8(0); u16(0); u16(0); u8(0); u16(100); u16(0);
        // EnemyNet (18 bytes)
        u32(7); u8(0); u8(0); i16(0); i16(0); u8(0); u8(100);
        u8(0); u8(0); u16(0); u16(0);
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

/// Builds a delta payload byte by byte, the way the tests build a full one, so
/// the decoder is checked against the exact bytes PROTOCOL.md fixes.
std::vector<uint8_t> build_delta_payload()
{
    std::vector<uint8_t> payload;
    auto u8 = [&](uint8_t v) { payload.push_back(v); };
    auto u16 = [&](uint16_t v) { payload.push_back(v & 0xFF); payload.push_back((v >> 8) & 0xFF); };
    auto i16 = [&](int16_t v) { u16(static_cast<uint16_t>(v)); };
    auto u32 = [&](uint32_t v) { for (int s = 0; s < 32; s += 8) payload.push_back(static_cast<uint8_t>((v >> s) & 0xFF)); };

    u32(600);    // server_tick
    u32(540);    // baseline_tick
    u32(10000);  // run_time_ms
    u32(9);      // acked_seq[0]
    u32(0);
    u32(0);
    u32(0);
    u8(0);       // game_state
    u16(2);      // spawned
    u16(1);      // updated
    u16(1);      // destroyed
    u16(0);      // reserved

    // Spawned: one player and one enemy.
    u8(0);                 // kind = player
    u8(0);                 // id
    u8(1);                 // state_flags
    u8(2);                 // race_id
    i16(haven::encode_pos(1.0f));
    i16(haven::encode_pos(-2.0f));
    u8(haven::encode_angle(0.25f));
    u8(100);               // hp_pct
    u8(50);                // shield_pct
    u8(75);                // mana_pct
    u16(0);                // dash_cd_remaining
    u16(2000);             // dash_cd_total
    u8(0);                 // is_dashing
    u16(100);              // max_hp
    u16(50);               // max_shield

    u8(1);                 // kind = enemy
    u32(55);               // id
    u8(1);                 // archetype
    u8(0x03);              // elite_mask: Vampiric | Shielded
    i16(haven::encode_pos(30.0f));
    i16(haven::encode_pos(-30.0f));
    u8(haven::encode_angle(0.5f));
    u8(75);                // hp_pct
    u8(128);               // telegraph_progress
    u8(1);                 // telegraph_shape (CircleAoE)
    u16(1500);             // telegraph_angle
    u16(500);              // telegraph_radius

    // Updated: one enemy, carrying its id then the full record. The full
    // record is the 18 byte EnemyNet, whose own id field follows the separate
    // id — the spec sends the id twice for an updated entity.
    u8(1);                 // kind = enemy
    u32(55);               // id
    u32(55);               // the full EnemyNet record, whose own id field leads it
    u8(1);                 // archetype
    u8(0x03);              // elite_mask
    i16(haven::encode_pos(31.0f));
    i16(haven::encode_pos(-31.0f));
    u8(haven::encode_angle(0.6f));
    u8(70);                // hp_pct
    u8(200);               // telegraph_progress
    u8(2);                 // telegraph_shape (ConeSlash)
    u16(2000);             // telegraph_angle
    u16(800);              // telegraph_radius

    // Destroyed: one gem.
    u8(3);                 // kind = gem
    u32(77);               // id

    return payload;
}

void test_snapshot_delta_packet()
{
    std::printf("S2C_SNAPSHOT_DELTA\n");
    std::vector<uint8_t> payload = build_delta_payload();

    haven::SnapshotDelta delta{};
    expect(haven::decode_snapshot_delta(payload.data(), payload.size(), delta), "delta decodes");
    expect(delta.server_tick == 600, "server_tick");
    expect(delta.baseline_tick == 540, "baseline_tick");
    expect(delta.run_time_ms == 10000, "run_time_ms");
    expect(delta.acked_seq[0] == 9, "acked_seq[0]");
    expect(delta.spawned_count == 2 && delta.updated_count == 1 && delta.destroyed_count == 1, "counts");
    expect(delta.spawned_players.size() == 1, "one spawned player");
    expect(delta.spawned_enemies.size() == 1, "one spawned enemy");
    expect(delta.updated_enemies.size() == 1, "one updated enemy");
    expect(delta.destroyed_gems.size() == 1, "one destroyed gem");

    const haven::EnemyNet& spawned = delta.spawned_enemies[0];
    expect(spawned.id == 55 && spawned.archetype == 1 && spawned.elite_mask == 0x03, "spawned enemy fields");
    expect_near(spawned.pos.x, 30.0f, 0.01f, "spawned enemy pos.x dequantised");
    expect(spawned.hp_pct == 75, "spawned enemy hp_pct");

    const haven::EnemyNet& updated = delta.updated_enemies[0];
    expect(updated.id == 55 && updated.hp_pct == 70, "updated enemy carries its id and full record");
    expect_near(updated.pos.x, 31.0f, 0.01f, "updated enemy pos.x dequantised");

    expect(delta.destroyed_gems[0] == 77, "destroyed gem id");

    // A payload a byte short of its last record is rejected, and one with a
    // trailing byte is a framing error rather than something to ignore.
    std::vector<uint8_t> cut(payload.begin(), payload.end() - 1);
    haven::SnapshotDelta short_delta{};
    expect(!haven::decode_snapshot_delta(cut.data(), cut.size(), short_delta), "truncated delta is rejected");

    std::vector<uint8_t> trailing = payload;
    trailing.push_back(0);
    haven::SnapshotDelta padded{};
    expect(!haven::decode_snapshot_delta(trailing.data(), trailing.size(), padded), "trailing garbage is rejected");

    // A kind byte outside 0..3 names a record type that does not exist. The
    // first spawned record's kind byte sits right after the 37 byte header
    // (which already includes the four counts): offset 37.
    std::vector<uint8_t> bad_kind = payload;
    bad_kind[37] = 4;
    haven::SnapshotDelta rejected{};
    expect(!haven::decode_snapshot_delta(bad_kind.data(), bad_kind.size(), rejected), "an unknown kind byte is rejected");
    expect(rejected.spawned_enemies.empty(), "a rejected delta leaves nothing behind");

    // An inflated count is caught when the reader runs off the end, not by
    // trusting the count: the vectors only ever hold what was actually read.
    const size_t spawned_count_offset = 29;
    std::vector<uint8_t> lying = payload;
    lying[spawned_count_offset] = 0xFF;
    lying[spawned_count_offset + 1] = 0xFF;
    haven::SnapshotDelta lying_delta{};
    expect(!haven::decode_snapshot_delta(lying.data(), lying.size(), lying_delta), "an inflated spawned count is rejected");
    expect(lying_delta.spawned_enemies.size() < 65535, "the inflated count never reached a resize");

    // counts[3] is reserved and PROTOCOL.md fixes it at zero. A non-zero value
    // is a header that disagrees with the frozen layout, not a field to ignore.
    const size_t reserved_offset = spawned_count_offset + 6;
    std::vector<uint8_t> nonzero_reserved = payload;
    nonzero_reserved[reserved_offset] = 1;
    haven::SnapshotDelta reserved_delta{};
    expect(!haven::decode_snapshot_delta(nonzero_reserved.data(), nonzero_reserved.size(), reserved_delta),
           "a non-zero reserved count is rejected");

    // An updated record carries its id twice: the separate u32 that heads it
    // and the record's own id field. They must agree, or the record describes a
    // different entity from the one its key names. The updated array follows the
    // 37 byte header and the two spawned records; its first (and only) record is
    // the enemy, so its external id is 4 bytes past the kind byte.
    const size_t updated_array_offset = haven::kSnapshotHeaderSize + (1 + haven::kPlayerRecordSize) +
                                        (1 + haven::kEnemyRecordSize);
    const size_t external_id_offset = updated_array_offset + 1;
    const size_t record_id_offset = external_id_offset + 4;
    expect(payload[external_id_offset] == 55 && payload[record_id_offset] == 55,
           "the fixture's updated enemy carries the same id twice");

    std::vector<uint8_t> id_mismatch = payload;
    id_mismatch[record_id_offset] = 56;  // the record names a different enemy
    haven::SnapshotDelta mismatch_delta{};
    expect(!haven::decode_snapshot_delta(id_mismatch.data(), id_mismatch.size(), mismatch_delta),
           "an updated record whose id disagrees with its key is rejected");
    expect(mismatch_delta.updated_enemies.empty(), "a rejected updated record leaves nothing behind");

    std::vector<uint8_t> both_new = payload;
    for (size_t i = 0; i < 4; ++i)
    {
        both_new[external_id_offset + i] = 0;
        both_new[record_id_offset + i] = 0;
    }
    both_new[external_id_offset] = 0x2A;
    both_new[record_id_offset] = 0x2A;
    haven::SnapshotDelta agreed{};
    expect(haven::decode_snapshot_delta(both_new.data(), both_new.size(), agreed),
           "an updated record whose ids agree decodes");
    expect(agreed.updated_enemies.size() == 1 && agreed.updated_enemies[0].id == 0x2A,
           "and its id is the shared one");
}

/// The phase-4 core: the client holds exactly one complete state, a delta is
/// applied only against the tick it holds, and a full snapshot replaces it.
void test_snapshot_state_baseline()
{
    std::printf("snapshot state baseline\n");
    haven::SnapshotState state;
    expect(!state.baseline().has_value(), "a fresh state holds no baseline");
    expect(state.size() == 0, "and no entities");

    // A full snapshot becomes the held state and its tick the baseline.
    haven::SnapshotFull full{};
    full.server_tick = 100;
    full.players.push_back(haven::PlayerNet{});
    full.players[0].id = 0;
    full.players[0].pos = {1.0f, 2.0f};
    full.enemies.push_back(haven::EnemyNet{});
    full.enemies[0].id = 7;
    full.enemies[0].archetype = 1;
    full.enemies[0].elite_mask = haven::kEliteVampiric;
    full.enemies[0].pos = {3.0f, 4.0f};
    full.gems.push_back(haven::GemNet{});
    full.gems[0].id = 9;
    full.gems[0].pos = {5.0f, 6.0f};
    full.gems[0].xp_value = 10;

    expect(state.apply_full(full) == 100, "apply_full returns the tick to acknowledge");
    expect(state.baseline().has_value() && *state.baseline() == 100, "the full snapshot's tick is the baseline");
    expect(state.size() == 3, "the held state carries every entity");
    expect(state.contains(0, haven::EntityKind::Player), "the player is held");
    expect(state.contains(7, haven::EntityKind::Enemy), "the enemy is held");
    expect(state.contains(9, haven::EntityKind::Gem), "the gem is held");

    // A delta against a baseline the client does not hold changes nothing.
    haven::SnapshotDelta wrong{};
    wrong.baseline_tick = 999;  // not the tick held
    wrong.server_tick = 150;
    wrong.destroyed_enemies.push_back(7);
    uint32_t applied = 0;
    expect(!state.apply_delta(wrong, applied), "a delta against another baseline is refused");
    expect(state.size() == 3, "and the held state is untouched");
    expect(state.contains(7, haven::EntityKind::Enemy), "the enemy is still there");

    // A delta against the held baseline applies: spawn, update, destroy.
    haven::SnapshotDelta delta{};
    delta.baseline_tick = 100;
    delta.server_tick = 103;
    delta.spawned_enemies.push_back(haven::EnemyNet{});
    delta.spawned_enemies[0].id = 20;
    delta.spawned_enemies[0].archetype = 0;
    delta.spawned_enemies[0].pos = {10.0f, 10.0f};
    delta.updated_enemies.push_back(haven::EnemyNet{});
    delta.updated_enemies[0].id = 7;
    delta.updated_enemies[0].archetype = 1;
    delta.updated_enemies[0].elite_mask = haven::kEliteVampiric | haven::kEliteHasteAura;
    delta.updated_enemies[0].pos = {3.5f, 4.5f};
    delta.updated_enemies[0].hp_pct = 50;
    delta.destroyed_gems.push_back(9);

    expect(state.apply_delta(delta, applied), "a delta against the held baseline applies");
    expect(applied == 103, "and returns the new tick to acknowledge");
    expect(*state.baseline() == 103, "the baseline advances to it");
    expect(state.size() == 3, "one spawned, one updated, one destroyed: still three");
    expect(state.contains(20, haven::EntityKind::Enemy), "the spawned enemy is held");
    expect(!state.contains(9, haven::EntityKind::Gem), "the destroyed gem is gone");
    expect(state.contains(7, haven::EntityKind::Enemy), "the updated enemy is still held");

    // build_render carries the held state, elite mask included, for the
    // interpolation buffer.
    const haven::RenderSnapshot render = state.build_render(1234);
    expect(render.run_time_ms == 1234, "the render carries the run clock");
    const haven::EntityKey enemy_key = haven::interpolation_key(7, haven::EntityKind::Enemy);
    const auto style = render.styles.find(enemy_key);
    expect(style != render.styles.end(), "the updated enemy has a style");
    expect(style->second.elite_mask == (haven::kEliteVampiric | haven::kEliteHasteAura),
           "the style carries the updated elite mask");
    const auto pos = render.positions.find(enemy_key);
    expect(pos != render.positions.end() && std::fabs(pos->second.x - 3.5f) < 0.01f,
           "the render carries the updated position");

    // A full snapshot replaces the held state outright, dropping anything the
    // delta had added that the full does not carry.
    haven::SnapshotFull second{};
    second.server_tick = 200;
    second.players.push_back(full.players[0]);
    state.apply_full(second);
    expect(*state.baseline() == 200, "the new full snapshot's tick is the baseline");
    expect(state.size() == 1, "only what the full carries is held");
    expect(!state.contains(20, haven::EntityKind::Enemy), "the delta-added enemy is gone");

    // reset returns the state to a fresh one, which is what a restart needs.
    state.reset();
    expect(!state.baseline().has_value() && state.size() == 0, "reset clears the baseline and every entity");
}

/// The verdict `handle_delta` consults before it lets a delta touch anything: a
/// delta naming the held tick matches, anything else does not, and a client
/// with no held state matches nothing.
void test_snapshot_state_baseline_match()
{
    std::printf("snapshot state baseline match\n");
    haven::SnapshotState state;
    expect(!state.matches_baseline(0), "with no held state nothing matches, not even tick 0");

    haven::SnapshotFull full{};
    full.server_tick = 100;
    state.apply_full(full);
    expect(state.matches_baseline(100), "the held tick matches");
    expect(!state.matches_baseline(99), "an older tick does not match");
    expect(!state.matches_baseline(101), "a newer tick does not match");
    expect(!state.matches_baseline(0), "and tick 0 — a full snapshot's marker — never matches");
}

/// PROTOCOL.md "Baseline rules": a delta the client refuses must change nothing
/// at all. The run state and the acceptance verdict live in SnapshotIngest, and
/// a refused delta must leave every field of it untouched — the tick, the run
/// clock and the game state — and must not disarm the restart boundary.
void test_snapshot_ingest_refuses_without_advancing()
{
    std::printf("snapshot ingest refuses a delta without advancing run state\n");
    haven::SnapshotIngest ingest;

    // A full snapshot always matches what the client holds, so it sets the run
    // state.
    expect(ingest.offer(600, 10000, haven::GameState::Running, /*baseline_ok=*/true, /*is_full=*/true),
           "a full snapshot is accepted");
    expect(ingest.server_tick() == 600, "the accepted tick is the run tick");
    expect(ingest.run_time_ms() == 10000, "the accepted run clock is the run clock");
    expect(ingest.game_state() == haven::GameState::Running, "the accepted state is the run state");

    // A delta whose baseline is not the tick the client holds is refused, and it
    // must not advance anything.
    expect(!ingest.offer(660, 20000, haven::GameState::GameOver, /*baseline_ok=*/false, /*is_full=*/false),
           "a refused delta is not accepted");
    expect(ingest.server_tick() == 600, "a refused delta does not move the run tick");
    expect(ingest.run_time_ms() == 10000, "a refused delta does not move the run clock");
    expect(ingest.game_state() == haven::GameState::Running, "a refused delta does not move the game state");
    expect(ingest.stale_snapshots() == 0, "and it is not counted as a stale snapshot");

    // An applicable delta advances all three.
    expect(ingest.offer(660, 12000, haven::GameState::LevelUpPause, /*baseline_ok=*/true, /*is_full=*/false),
           "an applicable delta is accepted");
    expect(ingest.server_tick() == 660 && ingest.run_time_ms() == 12000 &&
               ingest.game_state() == haven::GameState::LevelUpPause,
           "an accepted delta advances the run state");

    // A refused delta must not disarm the restart boundary either: one offered
    // while the boundary is armed must leave it armed, and only an accepted
    // snapshot may disarm it.
    ingest.begin_run(ingest.run_time_ms());
    expect(ingest.server_tick() == 0 && ingest.run_time_ms() == 0, "begin_run clears the tick and the clock");
    expect(!ingest.offer(10, 12000, haven::GameState::GameOver, /*baseline_ok=*/false, /*is_full=*/false),
           "a refused delta during a restart is refused");
    expect(!ingest.offer(11, 13000, haven::GameState::GameOver, /*baseline_ok=*/true, /*is_full=*/true),
           "the boundary is still armed, so the dead run's tail is refused");
    expect(ingest.stale_snapshots() == 1, "and the boundary rejection is counted as stale");
    expect(ingest.offer(12, 0, haven::GameState::Running, /*baseline_ok=*/true, /*is_full=*/true),
           "the new run's first snapshot is accepted and disarms the boundary");
    expect(ingest.game_state() == haven::GameState::Running, "and its game state is the run's");
}

/// A full snapshot the client did not ask for, at a tick below the one it
/// holds, is the server having restarted its run on its own. The client must
/// accept it as a new run rather than wedge on the ordering check. Over TCP
/// such a snapshot cannot be a delayed leftover of the run the client is on —
/// the byte stream is in send order, so anything that arrived after the snapshot
/// that set the held tick was sent after it, and a tick below it can only mean
/// the server's counter went backwards, i.e. a restart. A delta at a lower tick
/// is still refused: only a full snapshot restarts a run.
void test_snapshot_ingest_unsolicited_restart()
{
    std::printf("snapshot ingest accepts an unsolicited restart\n");
    haven::SnapshotIngest ingest;

    // The client is mid-run and holds tick 5000 of the dead run, whose clock has
    // frozen at 1650 ms. The dead run's tail keeps arriving in order.
    expect(ingest.offer(5000, 1650, haven::GameState::GameOver, /*baseline_ok=*/true, /*is_full=*/true),
           "the dead run's last snapshot is accepted");
    expect(ingest.offer(5001, 1650, haven::GameState::GameOver, /*baseline_ok=*/true, /*is_full=*/true),
           "the dead run's in-flight tail is accepted in order");
    expect(ingest.server_tick() == 5001, "the client holds the dead run's newest tick");

    // The server restarts without the client asking; its first broadcast is a
    // full snapshot at a tick below the one the client holds. Over TCP this
    // cannot be a delayed snapshot of the run the client is on — the byte
    // stream is in send order, so anything that arrived after the snapshot that
    // set the held tick was sent after it, and a tick below it can only mean
    // the server's counter went backwards, i.e. a restart. It is a new run.
    expect(ingest.offer(3, 50, haven::GameState::Running, /*baseline_ok=*/true, /*is_full=*/true),
           "the new run's full snapshot is accepted as a new run");
    expect(ingest.server_tick() == 3 && ingest.run_time_ms() == 50 &&
               ingest.game_state() == haven::GameState::Running,
           "the run state moves onto the new run");

    // The new run continues normally.
    expect(ingest.offer(4, 100, haven::GameState::Running, /*baseline_ok=*/true, /*is_full=*/true),
           "the new run's next snapshot is accepted");
    expect(ingest.server_tick() == 4, "and advances the new run's tick");

    // A delta is never a new-run signal: one at a tick below the held one is
    // still refused, and it resets nothing.
    expect(!ingest.offer(2, 150, haven::GameState::Running, /*baseline_ok=*/true, /*is_full=*/false),
           "a lower-tick delta is refused");
    expect(ingest.server_tick() == 4 && ingest.run_time_ms() == 100,
           "and it leaves the run state untouched");
    expect(ingest.stale_snapshots() == 1, "the refused delta is counted stale");
}

/// A refused acknowledgement send must not be forgotten: the tick stays owed
/// until a send for it succeeds, and the newest request supersedes an older one.
void test_ack_retry()
{
    std::printf("ack retry\n");
    haven::AckRetry ack;
    expect(!ack.owes(), "nothing is owed before a request");
    expect(!ack.pending().has_value(), "and there is nothing to send");

    ack.request(100);
    expect(ack.owes() && ack.pending().has_value() && *ack.pending() == 100, "a request is owed");

    // The send was refused (sent() was not called), so the tick is still owed
    // and the caller can retry it.
    expect(ack.owes() && *ack.pending() == 100, "a refused send leaves the tick owed");

    // A newer snapshot supersedes it: the tick to acknowledge is the newest
    // state the client holds.
    ack.request(105);
    expect(*ack.pending() == 105, "a newer request supersedes an older one");

    ack.sent();
    expect(!ack.owes(), "a successful send clears the debt");
    expect(!ack.pending().has_value(), "and there is nothing left to send");
}

/// A new run begins with nothing owed for the dead run's tick. The restart path
/// runs `AckRetry::clear()`, so a tick whose send the bounded queue refused in
/// the old run cannot be emitted in the new one: the server forgets its baseline
/// model on a restart, and an old-run acknowledgement could only drag the new
/// run's model onto a tick of a run that no longer exists.
void test_ack_retry_cleared_on_restart()
{
    std::printf("ack retry cleared on restart\n");
    haven::AckRetry ack;
    // The old run's last snapshot arrived and its acknowledgement was refused,
    // so the tick stayed owed and a later frame would retry it.
    ack.request(100);
    expect(ack.owes() && *ack.pending() == 100, "the old run's tick is still owed");

    // The run restarts, which clears the debt; nothing is emitted for it.
    ack.clear();
    expect(!ack.owes(), "a restart leaves nothing owed");
    expect(!ack.pending().has_value(), "so flush_ack() would emit no old-run ack");
}

/// A full snapshot is the server's recovery path, and it is the one place the
/// run clock can go backwards. A backward clock must not be left in the buffer,
/// where sample() assumes the timestamps only rise; a phase-3 server's
/// full-only stream has a rising clock and must keep interpolating.
void test_full_snapshot_recovery_and_clock_reset()
{
    std::printf("full snapshot recovery and clock reset\n");
    haven::InterpolationBuffer buffer;

    haven::RenderSnapshot a;
    a.run_time_ms = 5000;
    a.positions = {{1, {0.0f, 0.0f}}};
    haven::RenderSnapshot b;
    b.run_time_ms = 5100;
    b.positions = {{1, {10.0f, 0.0f}}};
    buffer.push(a);
    buffer.advance(0.100f);
    buffer.push(b);
    expect(buffer.size() == 2, "two snapshots are buffered");

    // A recovery full snapshot with the clock still rising is just the newest
    // state: the history stays, so interpolation keeps working.
    haven::RenderSnapshot c;
    c.run_time_ms = 5200;
    c.positions = {{1, {20.0f, 0.0f}}};
    buffer.push_replacing(c);
    expect(buffer.size() == 3, "a rising recovery snapshot keeps the history");
    expect(buffer.latest_time_ms() == 5200, "and becomes the newest");

    // A full snapshot whose clock went backwards means the server restarted its
    // clock: the history is dropped so the buffer stays monotonic.
    haven::RenderSnapshot reset;
    reset.run_time_ms = 0;
    reset.positions = {{1, {0.0f, 0.0f}}};
    buffer.push_replacing(reset);
    expect(buffer.size() == 1, "a backward clock drops the history");
    expect(buffer.latest_time_ms() == 0, "and the new clock is the only one left");
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
    const haven::EntityKey enemy_key = haven::interpolation_key(5, haven::EntityKind::Enemy);
    const haven::EntityKey proj_key = haven::interpolation_key(5, haven::EntityKind::Projectile);
    expect(enemy_key != proj_key, "enemy and projectile id 5 get different keys");
    expect(haven::key_kind(enemy_key) == haven::EntityKind::Enemy, "enemy key kind survives");
    expect(haven::key_kind(proj_key) == haven::EntityKind::Projectile, "projectile key kind survives");
    expect(haven::key_raw_id(enemy_key) == 5 && haven::key_raw_id(proj_key) == 5, "raw id survives");
    const haven::EntityKey player_key = haven::interpolation_key(2, haven::EntityKind::Player);
    expect(haven::key_kind(player_key) == haven::EntityKind::Player, "player key kind survives");
    expect(haven::key_raw_id(player_key) == 2, "player raw id survives");

    // The key is wide enough for the whole 32 bit id space, so no id is ever
    // truncated onto another. A key that took its kind bits out of the id would
    // fold these two together, and an endless run is where that would show up.
    const uint32_t high = 1u << 30;
    const haven::EntityKey high_key = haven::interpolation_key(high, haven::EntityKind::Enemy);
    const haven::EntityKey low_key = haven::interpolation_key(0, haven::EntityKind::Enemy);
    expect(high_key != low_key, "an id above 2^30 does not fold onto a smaller one");
    expect(haven::key_raw_id(high_key) == high, "an id above 2^30 survives the key");
    const uint32_t max_id = 0xFFFFFFFFu;
    expect(haven::key_raw_id(haven::interpolation_key(max_id, haven::EntityKind::Gem)) == max_id,
           "the largest id survives the key");
    expect(haven::key_kind(haven::interpolation_key(max_id, haven::EntityKind::Gem)) == haven::EntityKind::Gem,
           "and its kind is still recovered");
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
    for (uint8_t pool = 0; pool < 4; ++pool)
    {
        for (uint8_t slot = 0; slot < haven::kMaxSlotsPerPool; ++slot)
        {
            haven::ColourSlot key{static_cast<haven::ColourPool>(pool), slot};
            const haven::Rgba colour = haven::slot_colour(key);
            expect(colour.r >= 0.0f && colour.r <= 1.0f, "a slot colour is in range");
            expect(colour.a == 1.0f, "a slot colour is opaque");
        }
    }
    // The gem pool has one colour per XP tier, and they are told apart.
    const haven::Rgba small = haven::slot_colour(haven::ColourSlot{haven::ColourPool::Gem, 0});
    const haven::Rgba cache = haven::slot_colour(haven::ColourSlot{haven::ColourPool::Gem, 3});
    expect(small.r != cache.r || small.g != cache.g || small.b != cache.b,
           "small gems and boss caches are different colours");
    const haven::Rgba background = haven::slot_colour(haven::ColourSlot{haven::ColourPool::EnemyBar, 0});
    const haven::Rgba full_fill = haven::slot_colour(haven::ColourSlot{haven::ColourPool::EnemyBar, 9});
    expect(full_fill.g > background.g, "a full bar is brighter than its background");
}

void test_elite_outline_and_telegraph_slots()
{
    std::printf("elite outline and boss telegraph slots\n");
    // Each modifier bit gets its own slot in the outline pool, and the slots
    // are told apart by colour.
    for (uint8_t bit = 0; bit < haven::kEliteOutlineSlots; ++bit)
    {
        const haven::ColourSlot slot = haven::slot_for_elite_outline(bit);
        expect(slot.pool == haven::ColourPool::EliteOutline, "the ring slot is in the outline pool");
        expect(static_cast<uint8_t>(slot.slot) == bit, "each modifier bit has its own slot");
    }
    haven::Rgba previous = haven::slot_colour(haven::slot_for_elite_outline(0));
    for (uint8_t bit = 1; bit < haven::kEliteOutlineSlots; ++bit)
    {
        const haven::Rgba colour = haven::slot_colour(haven::slot_for_elite_outline(bit));
        expect(colour.r != previous.r || colour.g != previous.g || colour.b != previous.b,
               "each modifier ring colour is distinct");
        previous = colour;
    }

    // The boss telegraph has its own pool and a single red slot.
    const haven::ColourSlot telegraph = haven::slot_for_boss_telegraph();
    expect(telegraph.pool == haven::ColourPool::BossTelegraph, "the telegraph slot is in its pool");
    expect(telegraph.slot == 0, "and is the pool's only slot");
    const haven::Rgba red = haven::slot_colour(telegraph);
    expect(red.r > 0.9f && red.g < 0.3f && red.b < 0.3f, "the telegraph is warning red");

    // The modifier bits and names line up with DESIGN.md section 4's order.
    expect(std::string(haven::elite_name(0)) == "Vampiric", "bit 0 is Vampiric");
    expect(std::string(haven::elite_name(1)) == "Splitter", "bit 1 is Splitter");
    expect(std::string(haven::elite_name(2)) == "Shielded", "bit 2 is Shielded");
    expect(std::string(haven::elite_name(3)) == "Haste Aura", "bit 3 is Haste Aura");
    expect(haven::kEliteMaskAny == 0x0F, "the four modifiers fill the low nibble");

    // The telegraph window is the 1.0 s before the dash in a 6.0 s loop.
    expect(!haven::boss_telegraph_active(0), "the loop start is not the warning");
    expect(!haven::boss_telegraph_active(1999), "just before the window is not the warning");
    expect(haven::boss_telegraph_active(2000), "the window opens at 2.0 s");
    expect(haven::boss_telegraph_active(2500), "and spans the full second");
    expect(!haven::boss_telegraph_active(3000), "and closes at 3.0 s");
    expect(haven::boss_telegraph_active(8500), "and repeats every 6.0 s (8.5 s is 2.5 s into the loop)");
    expect(!haven::boss_telegraph_active(5999), "just before the loop wraps is not the warning");
}

void test_wireframe_rect()
{
    std::printf("wireframe rect\n");
    // The boss telegraph box is four thin quads: 16 vertices, 24 indices, wound
    // counter-clockwise like every other builder.
    RecordingSink sink;
    haven::append_wireframe_rect(sink, {10.0f, -4.0f}, {11.0f, 3.6f}, 0.0f, 0.045f, 0.18f);
    expect(sink.vertices.size() == 16 * 3, "four quads is 16 vertices");
    expect(sink.indices.size() == 24, "four quads is 24 indices");

    // At angle 0 the box spans centre +/- half_extents on both axes, plus the
    // stroke's own half thickness: each edge is a quad centred on the nominal
    // edge, so it overhangs by thickness / 2.
    const float overhang = 0.18f * 0.5f;
    float min_x = 1e30f, max_x = -1e30f, min_y = 1e30f, max_y = -1e30f;
    for (size_t i = 0; i < sink.vertices.size(); i += 3)
    {
        min_x = std::min(min_x, sink.vertices[i]);
        max_x = std::max(max_x, sink.vertices[i]);
        min_y = std::min(min_y, sink.vertices[i + 1]);
        max_y = std::max(max_y, sink.vertices[i + 1]);
    }
    expect_near(min_x, 10.0f - 11.0f - overhang, 1e-4f, "the box reaches its left edge");
    expect_near(max_x, 10.0f + 11.0f + overhang, 1e-4f, "the box reaches its right edge");
    expect_near(min_y, -4.0f - 3.6f - overhang, 1e-4f, "the box reaches its bottom edge");
    expect_near(max_y, -4.0f + 3.6f + overhang, 1e-4f, "the box reaches its top edge");

    // Every triangle is wound counter-clockwise, which Ogre reads as the front
    // face.
    for (size_t t = 0; t + 2 < sink.indices.size(); t += 3)
    {
        const auto vx = [&sink](uint32_t i) { return sink.vertices[static_cast<size_t>(i) * 3]; };
        const auto vy = [&sink](uint32_t i) { return sink.vertices[static_cast<size_t>(i) * 3 + 1]; };
        const uint32_t a = sink.indices[t];
        const uint32_t b = sink.indices[t + 1];
        const uint32_t c = sink.indices[t + 2];
        expect((vx(b) - vx(a)) * (vy(c) - vy(a)) - (vy(b) - vy(a)) * (vx(c) - vx(a)) > 0.0f,
               "wireframe rect triangles are wound counter-clockwise");
    }

    // Rotated 90 degrees, a box half-extents (11, 3.6) spans 3.6 horizontally
    // and 11 vertically: the rotation is applied, not ignored.
    RecordingSink rotated;
    haven::append_wireframe_rect(rotated, {0.0f, 0.0f}, {11.0f, 3.6f}, kPi * 0.5f, 0.045f, 0.18f);
    float rmin_x = 1e30f, rmax_x = -1e30f, rmin_y = 1e30f, rmax_y = -1e30f;
    for (size_t i = 0; i < rotated.vertices.size(); i += 3)
    {
        rmin_x = std::min(rmin_x, rotated.vertices[i]);
        rmax_x = std::max(rmax_x, rotated.vertices[i]);
        rmin_y = std::min(rmin_y, rotated.vertices[i + 1]);
        rmax_y = std::max(rmax_y, rotated.vertices[i + 1]);
    }
    expect_near(rmax_x - rmin_x, 2.0f * (3.6f + overhang), 1e-3f, "rotated 90 degrees the box is 2 * (3.6 + stroke) wide");
    expect_near(rmax_y - rmin_y, 2.0f * (11.0f + overhang), 1e-3f, "and 2 * (11 + stroke) tall");
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

    // An enemy shot arrives as a circle carrying `weapon_id` 0 — the sword,
    // whose radius is zero — so it must not collapse to a zero-radius circle
    // and vanish from the screen.
    RecordingSink hostile;
    haven::EntityDrawItem hostile_item = sword_item;
    hostile_item.style = 0;
    hostile_item.shape = 1;
    haven::append_projectile(hostile, hostile_item);
    expect(hostile.vertices.size() > 0, "a hostile shot emits geometry");
    expect(span(hostile) > 0.1f, "a hostile shot is drawn with a real radius");

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

void test_gem_geometry()
{
    std::printf("gem geometry\n");
    // A gem is one diamond quad: four vertices, two counter-clockwise
    // triangles, sized by its XP bucket.
    for (uint8_t bucket = 0; bucket < haven::kGemSlots; ++bucket)
    {
        RecordingSink sink;
        haven::EntityDrawItem item;
        item.kind = haven::EntityKind::Gem;
        item.style = bucket;
        item.pos = {3.0f, -4.0f};
        haven::append_gem(sink, item);
        expect_near(static_cast<float>(sink.vertices.size()), 4 * 3, 0.0f, "a gem emits one quad");
        expect_near(static_cast<float>(sink.indices.size()), 6, 0.0f, "a gem emits two triangles");

        // The diamond's vertices sit one circumradius out along the diagonals.
        const float radius = haven::gem_radius(bucket);
        float max_distance = 0.0f;
        for (size_t i = 0; i < sink.vertices.size(); i += 3)
            max_distance = std::max(max_distance,
                                    std::sqrt((sink.vertices[i] - item.pos.x) * (sink.vertices[i] - item.pos.x) +
                                              (sink.vertices[i + 1] - item.pos.y) * (sink.vertices[i + 1] - item.pos.y)));
        expect_near(max_distance, radius, 1e-4f, "the diamond reaches its circumradius");
    }

    // DESIGN.md section 1 gem values: Small = 2, Medium = 10, Large = 50,
    // Boss Cache = 250. Elite drops are 5x, so the value between two tiers
    // lands in the lower bucket.
    expect(haven::gem_bucket(2) == 0, "2 xp is a small gem");
    expect(haven::gem_bucket(10) == 1, "10 xp is a medium gem");
    expect(haven::gem_bucket(50) == 2, "50 xp is a large gem");
    expect(haven::gem_bucket(250) == 3, "250 xp is a boss cache");
    expect(haven::gem_bucket(5) == 1, "an elite small (5 xp) lands in the medium bucket");
    expect(haven::gem_bucket(0) == 0, "zero xp is a small gem");
    expect(haven::gem_bucket(65535) == 3, "a huge value lands in the cache bucket");

    // Bigger tiers draw bigger gems, and every bucket has a colour.
    for (uint8_t bucket = 1; bucket < haven::kGemSlots; ++bucket)
        expect(haven::gem_radius(bucket) > haven::gem_radius(bucket - 1), "a bigger tier draws a bigger gem");
    for (uint8_t bucket = 0; bucket < haven::kGemSlots; ++bucket)
        expect(haven::gem_colour(bucket).a == 1.0f, "a gem bucket colour is opaque");

    // Each gem bucket gets its own slot in the gem pool.
    for (uint8_t bucket = 0; bucket < haven::kGemSlots; ++bucket)
    {
        haven::EntityDrawItem item;
        item.kind = haven::EntityKind::Gem;
        item.style = bucket;
        const haven::ColourSlot slot = haven::slot_for_gem(item);
        expect(slot.pool == haven::ColourPool::Gem, "a gem draws in the gem pool");
        expect(static_cast<uint8_t>(slot.slot) == bucket, "each gem bucket has its own slot");
    }
}

void test_levelup_packets()
{
    std::printf("C2S_LEVELUP_CHOICE / C2S_RESTART\n");
    // event_id u16 + choice_idx u8 = 3 byte payload.
    std::vector<uint8_t> packet = haven::encode_levelup_choice(0x1234, 2);
    expect(packet.size() == haven::kHeaderSize + 3, "a level-up choice is 7 byte header + 3 byte payload");
    haven::PacketHeader header{};
    expect(haven::decode_header(packet.data(), packet.size(), header), "header decodes");
    expect(header.msg_type == 0x03, "msg_type is C2S_LEVELUP_CHOICE");
    expect(header.payload_len == 3, "payload_len is 3");
    expect(packet[7] == 0x34 && packet[8] == 0x12, "event_id is little-endian");
    expect(packet[9] == 2, "choice_idx is the third payload byte");

    // C2S_RESTART has an empty payload.
    std::vector<uint8_t> restart = haven::encode_restart();
    expect(restart.size() == haven::kHeaderSize, "a restart is a bare 7 byte header");
    haven::PacketHeader restart_header{};
    expect(haven::decode_header(restart.data(), restart.size(), restart_header), "restart header decodes");
    expect(restart_header.msg_type == 0x05, "msg_type is C2S_RESTART");
    expect(restart_header.payload_len == 0, "payload_len is 0");
}

void test_ack_packet()
{
    std::printf("C2S_ACK_SNAPSHOT\n");
    // last_recv_server_tick u32 = 4 byte payload.
    std::vector<uint8_t> packet = haven::encode_ack_snapshot(0xDEADBEEF);
    expect(packet.size() == haven::kHeaderSize + 4, "an ack is a 7 byte header + 4 byte payload");
    haven::PacketHeader header{};
    expect(haven::decode_header(packet.data(), packet.size(), header), "header decodes");
    expect(header.msg_type == 0x04, "msg_type is C2S_ACK_SNAPSHOT");
    expect(header.payload_len == 4, "payload_len is 4");
    expect(packet[7] == 0xEF && packet[8] == 0xBE && packet[9] == 0xAD && packet[10] == 0xDE,
           "last_recv_server_tick is little-endian");
}

void test_levelup_offer_packet()
{
    std::printf("S2C_LEVELUP_OFFER\n");
    std::vector<uint8_t> payload;
    auto append_u8 = [&](uint8_t v) { payload.push_back(v); };
    auto append_u16 = [&](uint16_t v) { payload.push_back(v & 0xFF); payload.push_back((v >> 8) & 0xFF); };
    auto append_i16 = [&](int16_t v) { append_u16(static_cast<uint16_t>(v)); };
    append_u8(0);        // player_id
    append_u16(0x1020);  // event_id
    append_u16(7);       // new_level
    for (int i = 0; i < 3; ++i)
    {
        append_u8(i);        // item_id
        append_u8(i + 1);    // next_level
        append_u8(i == 2);   // is_evo
        append_u8(0);        // stat_type (Damage)
        append_i16(500);     // current_stat_fp (5.00)
        append_i16(600);     // upgraded_stat_fp (6.00)
    }

    haven::LevelUpOffer offer{};
    expect(haven::decode_levelup_offer(payload.data(), payload.size(), offer), "offer decodes");
    expect(offer.player_id == 0, "player_id");
    expect(offer.event_id == 0x1020, "event_id");
    expect(offer.new_level == 7, "new_level");
    expect(offer.options[0].item_id == 0 && offer.options[0].next_level == 1 && offer.options[0].is_evo == 0,
           "option 0");
    expect(offer.options[0].stat_type == 0 && offer.options[0].current_stat_fp == 500 &&
               offer.options[0].upgraded_stat_fp == 600,
           "option 0 stat fields");
    expect(offer.options[2].item_id == 2 && offer.options[2].next_level == 3 && offer.options[2].is_evo == 1,
           "option 2 is the evolution");

    expect(!haven::decode_levelup_offer(payload.data(), payload.size() - 1, offer), "truncated offer is rejected");
    expect(!haven::decode_levelup_offer(payload.data(), payload.size() + 1, offer), "a padded offer is rejected");
}

void test_game_over_packet()
{
    std::printf("S2C_GAME_OVER\n");
    std::vector<uint8_t> payload;
    auto append_u32 = [&](uint32_t v) {
        for (int s = 0; s < 32; s += 8) payload.push_back(static_cast<uint8_t>((v >> s) & 0xFF));
    };
    append_u32(72000);   // final_tick
    append_u32(1234);    // seconds_survived
    append_u32(98765);   // score
    append_u32(4321);    // kills

    haven::GameOverStats stats{};
    expect(haven::decode_game_over(payload.data(), payload.size(), stats), "game over decodes");
    expect(stats.final_tick == 72000, "final_tick");
    expect(stats.seconds_survived == 1234, "seconds_survived");
    expect(stats.score == 98765, "score");
    expect(stats.kills == 4321, "kills");
    expect(!haven::decode_game_over(payload.data(), payload.size() - 1, stats), "truncated game over is rejected");
    expect(!haven::decode_game_over(payload.data(), payload.size() + 1, stats), "a padded game over is rejected");
}

void test_loadout_packet()
{
    std::printf("S2C_LOADOUT\n");
    std::vector<uint8_t> payload;
    auto append_u8 = [&](uint8_t v) { payload.push_back(v); };
    auto append_u16 = [&](uint16_t v) { payload.push_back(v & 0xFF); payload.push_back((v >> 8) & 0xFF); };
    auto append_u32 = [&](uint32_t v) {
        for (int s = 0; s < 32; s += 8) payload.push_back(static_cast<uint8_t>((v >> s) & 0xFF));
    };
    append_u8(0);        // player_id
    append_u16(12);      // level
    append_u32(685);     // xp
    append_u32(690);     // xp_next
    append_u8(2);        // weapon_count
    append_u8(0); append_u8(3); append_u8(0);  // sword level 3
    append_u8(32); append_u8(1); append_u8(1);  // Vorpal Blender, evolved
    append_u8(0xFF); append_u8(0); append_u8(0);  // empty slot
    append_u8(0xFF); append_u8(0); append_u8(0);  // empty slot
    append_u8(2);        // passive_count
    append_u8(21); append_u8(3);  // Prism Splitter level 3
    append_u8(16); append_u8(5);  // Spinach Core level 5
    for (int i = 0; i < 8; ++i) { append_u8(0xFF); append_u8(0); }  // empty passives

    expect(payload.size() == 45, "a loadout is always 45 bytes");
    haven::Loadout loadout{};
    expect(haven::decode_loadout(payload.data(), payload.size(), loadout), "loadout decodes");
    expect(loadout.player_id == 0, "player_id");
    expect(loadout.level == 12, "level");
    expect(loadout.xp == 685 && loadout.xp_next == 690, "xp and xp_next");
    expect(loadout.weapon_count == 2, "weapon_count");
    expect(loadout.weapons[0].item_id == 0 && loadout.weapons[0].level == 3 && loadout.weapons[0].evolved == 0,
           "sword slot");
    expect(loadout.weapons[1].item_id == 32 && loadout.weapons[1].level == 1 && loadout.weapons[1].evolved == 1,
           "evolved weapon slot");
    expect(loadout.weapons[2].item_id == 0xFF && loadout.weapons[3].item_id == 0xFF, "unused weapon slots are 0xFF");
    expect(loadout.passive_count == 2, "passive_count");
    expect(loadout.passives[0].item_id == 21 && loadout.passives[0].level == 3, "prism slot");
    expect(loadout.passives[1].item_id == 16 && loadout.passives[1].level == 5, "spinach slot");
    expect(loadout.passives[2].item_id == 0xFF && loadout.passives[9].item_id == 0xFF, "unused passive slots are 0xFF");

    expect(!haven::decode_loadout(payload.data(), payload.size() - 1, loadout), "truncated loadout is rejected");
    expect(!haven::decode_loadout(payload.data(), payload.size() + 1, loadout), "a padded loadout is rejected");

    // The payload always carries all four weapon slots and all ten passive
    // slots, so a count past the array it indexes is a payload the sender got
    // wrong. Accepting one would have the HUD walk past the slots it was given
    // and build a tray that does not match the game.
    std::vector<uint8_t> bad = payload;
    bad[11] = 5;  // weapon_count, one past the four slots that exist
    expect(!haven::decode_loadout(bad.data(), bad.size(), loadout), "weapon_count past four is rejected");
    bad[11] = 4;
    expect(haven::decode_loadout(bad.data(), bad.size(), loadout), "weapon_count of exactly four is accepted");

    bad = payload;
    bad[24] = 11;  // passive_count, one past the ten slots that exist
    expect(!haven::decode_loadout(bad.data(), bad.size(), loadout), "passive_count past ten is rejected");
    bad[24] = 10;
    expect(haven::decode_loadout(bad.data(), bad.size(), loadout), "passive_count of exactly ten is accepted");

    bad = payload;
    bad[11] = 0xFF;
    expect(!haven::decode_loadout(bad.data(), bad.size(), loadout), "a wrapped weapon_count is rejected");
}

void test_item_catalog()
{
    std::printf("item catalog\n");
    // The PROTOCOL.md id space: weapons 0-3, passives 16-25, evolutions 32-39,
    // Overclock 48-50, 0xFF none.
    expect(haven::item_kind(0) == haven::ItemKind::Weapon, "0 is a weapon");
    expect(haven::item_kind(3) == haven::ItemKind::Weapon, "3 is a weapon");
    expect(haven::item_kind(16) == haven::ItemKind::Passive, "16 is passive P0");
    expect(haven::item_kind(25) == haven::ItemKind::Passive, "25 is passive P9");
    expect(haven::item_kind(32) == haven::ItemKind::Evolution, "32 is an evolution");
    expect(haven::item_kind(39) == haven::ItemKind::Evolution, "39 is an evolution");
    expect(haven::item_kind(48) == haven::ItemKind::Overclock, "48 is Overclock");
    expect(haven::item_kind(50) == haven::ItemKind::Overclock, "50 is Overclock");
    expect(haven::item_kind(0xFF) == haven::ItemKind::None, "0xFF is no item");
    expect(haven::item_kind(7) == haven::ItemKind::None, "7 is not in the id space");
    expect(haven::item_kind(30) == haven::ItemKind::None, "30 is not in the id space");

    // DESIGN.md section 3 names and max levels.
    expect(std::string(haven::item_name(0)) == "SWORD", "sword name");
    expect(std::string(haven::item_name(18)) == "CLOCKWORK GEAR", "P2 name");
    expect(std::string(haven::item_name(34)) == "GRAVITY CLEAVER", "evolution name");
    expect(std::string(haven::item_short_name(35)) == "JUGGER", "halberd short name");
    expect(haven::item_max_level(0) == 8, "weapons cap at 8");
    expect(haven::item_max_level(16) == 5, "passives cap at 5");
    expect(haven::item_max_level(21) == 3, "Prism Splitter caps at 3");
    expect(haven::item_max_level(48) == 0, "Overclock is uncapped");

    // Every item in the space has a name, a short name, and a colour.
    for (uint8_t id : {0, 1, 2, 3, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 32, 33, 34, 35, 36, 37, 38, 39, 48, 49, 50})
    {
        expect(haven::item_name(id)[0] != '\0', "the item has a name");
        expect(haven::item_short_name(id)[0] != '\0', "the item has a short name");
        expect(haven::item_colour(id).a == 1.0f, "the item has a colour");
    }

    // The eight evolutions pair one weapon with one named passive.
    const uint8_t expected[8][2] = {{0, 18}, {0, 23}, {1, 17}, {1, 24}, {2, 19}, {2, 21}, {3, 16}, {3, 20}};
    for (uint8_t evo = 0; evo < 8; ++evo)
    {
        uint8_t weapon = 0xFF;
        uint8_t passive = 0xFF;
        expect(haven::evo_components(32 + evo, weapon, passive), "evolution has components");
        expect(weapon == expected[evo][0] && passive == expected[evo][1], "the pairing matches DESIGN.md");
    }
    uint8_t weapon = 0;
    uint8_t passive = 0;
    expect(!haven::evo_components(0, weapon, passive), "a weapon has no evolution components");
    expect(!haven::evo_components(0xFF, weapon, passive), "0xFF has no evolution components");

    // Blurbs: an evolution names its components; a plain upgrade names a level.
    expect(haven::item_blurb(34, 1) == "AXE + LENS", "a cleaver blurb names its components");
    expect(haven::item_blurb(18, 4) == "LEVEL 4", "a passive blurb names a level");
}

void test_hud_text()
{
    std::printf("bitmap font\n");
    // 5x7 glyphs on a 6 pixel advance: n characters span 6n - 1 pixels.
    expect(haven::text_pixel_width("") == 0, "an empty string has no width");
    expect(haven::text_pixel_width("A") == haven::kGlyphWidth, "one character is a glyph wide");
    expect(haven::text_pixel_width("AB") == 2 * haven::kGlyphAdvance - 1, "two characters gain one step");
    expect(haven::text_pixel_width("LEVEL UP!") == 9 * haven::kGlyphAdvance - 1, "a card title");

    expect(haven::font_has_glyph('A'), "A has a glyph");
    expect(haven::font_has_glyph('7'), "7 has a glyph");
    expect(haven::font_has_glyph('!'), "! has a glyph");
    // Space is what everything unknown falls back to, so it is the blank rather
    // than a glyph of its own: `find_glyph` cannot tell the two apart, and the
    // header lists the charset as A-Z, 0-9 and the HUD's punctuation.
    expect(!haven::font_has_glyph(' '), "space is the blank fallback, not a glyph of its own");
    expect(!haven::font_has_glyph('@'), "@ has no glyph");

    // Each lit pixel is one quad: 4 vertices and 6 indices.
    for (const std::string& text : {std::string("SWORD"), std::string("LEVEL 7"), std::string("01:23")})
    {
        RecordingSink sink;
        haven::append_text(sink, text, {0.0f, 0.0f}, 0.25f);
        expect_near(static_cast<float>(sink.vertices.size()), haven::text_lit_pixels(text) * 4 * 3, 0.0f,
                    "one quad per lit pixel");
        expect_near(static_cast<float>(sink.indices.size()), haven::text_lit_pixels(text) * 6, 0.0f,
                    "two triangles per lit pixel");
    }

    // The glyph box's top-left corner is the point the line is given. 'E' lights
    // its first column, so the first quad's left edge is exactly top_left.x; a
    // quad's first vertex is its bottom-left corner, so the top edge is one
    // pixel up from it.
    RecordingSink sink;
    const float pixel = 0.5f;
    haven::append_text(sink, "E", {10.0f, -5.0f}, pixel);
    expect_near(sink.vertices[0], 10.0f, 1e-6f, "the first pixel's left edge is top_left.x");
    expect_near(sink.vertices[1] + pixel, -5.0f, 1e-6f, "the first pixel's top edge is top_left.y");

    // The digit 0 has a hole in the middle: its lit pixel count is below the
    // full glyph area, and its first quad is the top bar.
    expect(haven::text_lit_pixels("0") < 5 * 7, "the 0 glyph is not a full block");

    // Auto-fit. The size passed in is the one the layout was designed around,
    // so the fit only ever shrinks: a long name is narrowed to exactly the
    // width it is given, and a short name is never blown up to fill the box.
    const float long_fit = haven::fit_text_pixel("JUGGERNAUT HALBERD", 10.0f, 0.0f, 0.34f);
    expect_near(long_fit, 10.0f / static_cast<float>(haven::text_pixel_width("JUGGERNAUT HALBERD")), 1e-6f,
                "a long name shrinks to exactly the width it is given");
    expect_near(static_cast<float>(haven::text_pixel_width("JUGGERNAUT HALBERD")) * long_fit, 10.0f, 1e-4f,
                "the shrunk name fits its box");
    const float short_fit = haven::fit_text_pixel("AXE", 20.0f, 0.0f, 0.34f);
    expect_near(short_fit, 0.34f, 1e-6f, "a short name keeps the size it was given");
    // The old behaviour grew a short name to fill the box: "AXE" is 17 font
    // pixels wide, so a 20 unit box asked for 20/17 ~= 1.18, more than three
    // times the card's title size. The upper bound is the point of the test.
    expect(short_fit <= 0.34f, "a short name is never enlarged past its given size");
    expect_near(haven::fit_text_pixel("", 20.0f, 0.0f, 0.34f), 0.34f, 1e-6f, "an empty string keeps the given size");
}

void test_restart_boundary()
{
    std::printf("restart boundary\n");
    haven::RestartBoundary boundary;
    expect(!boundary.armed(), "the boundary starts disarmed");
    expect(boundary.accepts(1234, haven::GameState::Running), "a disarmed boundary accepts anything");
    expect(boundary.accepts(1234, haven::GameState::GameOver), "whatever state it carries");

    // The dead run's snapshots still in flight carry its final run clock and
    // its game_over state, so anything at or above that clock is the run the
    // client just left.
    boundary.arm(30000);
    expect(boundary.armed(), "arming the boundary closes it");
    expect(!boundary.accepts(30000, haven::GameState::GameOver), "the dead run's final snapshot is rejected");
    expect(!boundary.accepts(30400, haven::GameState::GameOver), "a later dead-run snapshot is rejected too");
    expect(boundary.armed(), "a rejected snapshot leaves the boundary closed");

    // The new run restarts its clock at zero and reports the run running.
    expect(boundary.accepts(0, haven::GameState::Running), "the new run's first snapshot is accepted");
    expect(!boundary.armed(), "the first new snapshot disarms the boundary");
    expect(boundary.accepts(30000, haven::GameState::Running), "the ordinary tick ordering takes over afterwards");

    // Over TCP everything still in the socket was sent after the last snapshot
    // the client processed, so its clock is at or above that snapshot's: a
    // clock below the boundary is the new run's, not an older dead-run one.
    boundary.arm(30000);
    expect(boundary.accepts(29950, haven::GameState::GameOver),
           "a clock below the boundary is accepted as the new run's");
    expect(!boundary.armed(), "and it disarms the boundary");

    boundary.arm(5000);
    boundary.clear();
    expect(!boundary.armed(), "clear disarms the boundary");
    expect(boundary.accepts(9000, haven::GameState::GameOver), "and everything passes again");
}

/// The boundary is a filter, not the proof: what proves the run changed is the
/// snapshot's own state. This walks the packets the client would actually see,
/// in the order the server sends them, across a restart.
void test_restart_boundary_proves_run_identity()
{
    std::printf("restart boundary proves run identity\n");

    // The server holds `game_over` and stops advancing the run clock until it
    // has processed C2S_RESTART, so what is still in flight when the client
    // arms the boundary is the tail of frozen snapshots; over TCP, nothing the
    // server sent before them is left either.
    struct Packet {
        uint32_t run_time_ms;
        haven::GameState state;
        bool new_run;
    };
    const Packet tail[] = {
        {1650, haven::GameState::GameOver, false},  // already in the socket when the request went out
        {1650, haven::GameState::GameOver, false},  // the frozen tick re-broadcast while the client decides
        {1650, haven::GameState::GameOver, false},
        {50, haven::GameState::Running, true},  // first broadcast after the restart: tick 3, clock 50
        {100, haven::GameState::Running, true},
    };

    haven::RestartBoundary boundary;
    boundary.arm(1650);
    for (const Packet& packet : tail)
    {
        const bool accepted = boundary.accepts(packet.run_time_ms, packet.state);
        expect(accepted == packet.new_run, "a snapshot is accepted exactly when it belongs to the new run");
    }
    expect(!boundary.armed(), "and the first new-run snapshot disarms the boundary");

    // The clock alone cannot do this. The client only ever sees the run clock at
    // the 20 Hz snapshot boundary, so the new run's first snapshot carries the
    // clock of the first broadcast after the restart, not zero. A dead run that
    // ended before its own second snapshot leaves the boundary armed at that
    // same value, and every snapshot of the new run is then at or above it: a
    // boundary that could only disarm on a strictly lower clock would reject the
    // whole new run, for the rest of the session. The state is what disarms it.
    haven::RestartBoundary short_run;
    short_run.arm(50);
    expect(short_run.accepts(50, haven::GameState::Running),
           "the new run's first snapshot is accepted even at the dead run's own clock");
    expect(!short_run.armed(), "so a short dead run cannot latch the boundary shut");

    // The running state is the proof, so it is accepted whatever clock it
    // carries; the clock is only the narrower filter. The server cannot report a
    // running run until it has processed the restart, which is what makes this
    // safe.
    haven::RestartBoundary running_wins;
    running_wins.arm(1650);
    expect(running_wins.accepts(1650, haven::GameState::Running),
           "a running state is the new run whatever clock it carries");
}

/// A run that ends before its clock advances: the client has seen no snapshot of
/// it at all, so the clock has nothing to compare — but the packets that arrive
/// next are still the dead run's until the server has processed the restart, so
/// the boundary must not simply give up.
void test_restart_boundary_zero_clock()
{
    std::printf("restart boundary with a zero run clock\n");
    haven::RestartBoundary boundary;
    boundary.arm(0);
    expect(boundary.armed(), "a zero run clock still arms the boundary");
    expect(!boundary.accepts(0, haven::GameState::GameOver),
           "the dead run's game-over snapshot is rejected even at clock zero");
    expect(boundary.armed(), "a rejected snapshot leaves the boundary closed");
    expect(boundary.accepts(0, haven::GameState::Running), "the new run's first snapshot is accepted");
    expect(!boundary.armed(), "and disarms the boundary");

    // Arming at zero cannot latch: the running state is what disarms it, and
    // that does not depend on the clock at all.
    boundary.arm(0);
    expect(boundary.accepts(5000, haven::GameState::Running), "a running snapshot at any clock disarms it");
    expect(!boundary.armed(), "and the ordinary ordering takes over");
}

void test_levelup_gate()
{
    std::printf("level-up gate\n");
    haven::LevelUpGate gate;
    expect(gate.accept(1), "the first offer opens");
    expect(!gate.accept(1), "the same offer arriving again is suppressed");

    // One offer at a time. The server pauses the run while an offer is
    // unresolved and cannot raise another one, so an offer arriving now is a
    // duplicate or a corrupt packet. Replacing the open offer with it would
    // change the modal under the player's hand and leave the event it displaced
    // unanswered with the server still paused on it.
    expect(!gate.accept(2), "a newer offer while one is unanswered is ignored");
    expect(!gate.accept(3), "and so is any later one");
    expect(!gate.accept(1), "and the open offer itself is not reopened");

    // Answering the open offer lets the next event open.
    gate.resolve();
    expect(gate.accept(2), "once the open offer is answered, the next event opens");
    expect(!gate.accept(1), "an event older than the answered one is still suppressed");
    expect(!gate.accept(2), "and the answered one is suppressed when it repeats");
    gate.resolve();
    expect(gate.accept(3), "offers keep arriving");

    // Resolving an offer that is not open must not disturb the next one.
    gate.resolve();
    gate.resolve();
    expect(gate.accept(4), "a repeated resolve is harmless");

    // The server renumbers from 1 with the run, so a restart starts the gate
    // over: without it the new run's first offer would look like an old one.
    gate.reset();
    expect(gate.accept(1), "a restarted run's first offer is new again");
    expect(!gate.accept(2), "and the gate is open again, so a second offer is still ignored");
    gate.resolve();
    expect(gate.accept(2), "and its second one opens once the first is answered");

    // The event id is a wrapping u16 counter; the comparison has to survive it.
    haven::LevelUpGate wrapping;
    expect(wrapping.accept(65535), "the last id before the wrap is accepted");
    wrapping.resolve();
    expect(wrapping.accept(0), "the id after the wrap is newer, not older");
    wrapping.resolve();
    expect(wrapping.accept(1), "and so is the one after that");
    wrapping.resolve();
    expect(!wrapping.accept(65535), "while the pre-wrap id is now old");
}

/// Two consecutive `restart` markers in a choice script are two requests for the
/// same game over: the client cannot see the new run until the server has
/// answered, so only the first may become a C2S_RESTART.
void test_restart_gate()
{
    std::printf("restart gate\n");
    const std::vector<int8_t> script = {-1, -1};
    haven::InputState input;
    input.set_choice_script(script);

    haven::RestartGate gate;
    bool mismatch = false;
    int queued = 0;
    for (int frame = 0; frame < 200; ++frame)
        if (gate.take(input, mismatch)) ++queued;

    expect(queued == 1, "two restart markers queue exactly one C2S_RESTART");
    expect(!mismatch, "a restart marker is not reported as a level-up mismatch");
    expect(input.peek_scripted_line() == -1,
           "the second marker is left where it is, for the game over after this one");

    // The new run arrives, so the next game over gets its own restart.
    gate.clear();
    for (int frame = 0; frame < 200; ++frame)
        if (gate.take(input, mismatch)) ++queued;
    expect(queued == 2, "the next game over queues its own restart");
    expect(input.peek_scripted_line() == -3, "and the script is then exhausted");

    // The keyboard is the same one-shot: R held down through a restart must not
    // send a second request either.
    haven::InputState keys;
    haven::RestartGate key_gate;
    int key_queued = 0;
    keys.key_down(haven::InputState::kKeyR);
    for (int frame = 0; frame < 200; ++frame)
        if (key_gate.take(keys, mismatch)) ++key_queued;
    expect(key_queued == 1, "a restart key that stays down queues exactly one request");
    expect(!keys.restart_requested(), "and the flag is consumed even while the gate is pending");

    key_gate.clear();
    keys.key_down(haven::InputState::kKeyR);
    expect(key_gate.take(keys, mismatch), "a keypress after the new run queues another");
    expect(key_gate.pending(), "and leaves the gate pending");
    expect(!key_gate.take(keys, mismatch), "while a request is in flight nothing more is queued");

    // A choice line is not a restart, and a game over must leave it alone.
    // `set_choice_script` keeps a pointer to the vector, so it has to outlive
    // the InputState.
    const std::vector<int8_t> pick_script = {0, -1};
    haven::InputState picks;
    picks.set_choice_script(pick_script);
    haven::RestartGate pick_gate;
    expect(!pick_gate.take(picks, mismatch), "a choice line is not a restart");
    expect(mismatch, "and the stall is reported as a level-up mismatch");
    expect(picks.peek_scripted_line() == 0, "the pick is left for the level-up it names");
    expect(picks.take_scripted_choice() == 0, "which the level-up then consumes");
    expect(pick_gate.take(picks, mismatch), "and the restart marker after it is spent here");
}

void test_xp_bar_fill()
{
    std::printf("xp bar fill\n");
    // The HUD asks for this on every frame, including the ones before the first
    // S2C_LOADOUT and after a restart clears the HUD. Reading through an absent
    // loadout is undefined behaviour, which is what the first frame used to do.
    const haven::XpBarFill none = haven::xp_bar_fill(std::nullopt);
    expect(!none.visible, "no loadout means no XP bar");
    expect_near(none.fraction, 0.0f, 0.0f, "and no fill");

    haven::Loadout loadout{};
    loadout.level = 1;
    loadout.xp = 0;
    loadout.xp_next = 0;
    expect(!haven::xp_bar_fill(loadout).visible, "an unknown xp_next hides the bar");

    loadout.xp = 5;
    loadout.xp_next = 10;
    const haven::XpBarFill half = haven::xp_bar_fill(loadout);
    expect(half.visible, "a loadout with an xp_next shows the bar");
    expect_near(half.fraction, 0.5f, 1e-6f, "half the xp fills half the bar");

    loadout.xp = 10;
    expect_near(haven::xp_bar_fill(loadout).fraction, 1.0f, 1e-6f, "a full bar is capped at one");

    // A malformed payload could carry xp above xp_next; the bar must not run
    // past the end of its track.
    loadout.xp = 40;
    expect_near(haven::xp_bar_fill(loadout).fraction, 1.0f, 1e-6f, "an overflowing xp is clamped");

    loadout.xp = 0;
    loadout.xp_next = 100;
    expect_near(haven::xp_bar_fill(loadout).fraction, 0.0f, 1e-6f, "an empty bar is zero");
}

void test_evolution_watch()
{
    std::printf("evolution watch\n");
    haven::Loadout loadout{};
    loadout.weapon_count = 2;
    loadout.weapons[0].item_id = 0;
    loadout.weapons[1].item_id = 1;

    haven::EvolutionWatch watch;
    expect(watch.observe(loadout) == -1, "no evolution at the start");

    loadout.weapons[1].evolved = 1;
    expect(watch.observe(loadout) == 1, "a slot whose evolved flag rose reports its index");
    expect(watch.observe(loadout) == -1, "the same loadout again reports nothing: the flag did not rise");

    // The flag stays set across the run, so the edge is only visible if the
    // watch's memory is cleared with the run. Without that reset a new run
    // would never announce the evolution of a slot that was already evolved
    // when the previous run ended.
    watch.reset();
    expect(watch.observe(loadout) == 1, "after a reset the same evolved slot reports its index again");

    loadout.weapon_count = 4;
    loadout.weapons[3].evolved = 1;
    watch.reset();
    expect(watch.observe(loadout) == 3, "the last slot to rise wins");
    expect(watch.observe(loadout) == -1, "and does not rise again");

    // A slot past weapon_count is not part of the loadout, so it is not watched.
    loadout.weapons[2].evolved = 1;
    loadout.weapon_count = 2;
    watch.reset();
    expect(watch.observe(loadout) == 1, "a slot past weapon_count is ignored");
}

void test_choice_input()
{
    std::printf("choice input\n");
    haven::InputState input;
    expect(input.pending_choice() == -1, "no choice is pending at rest");
    expect(!input.restart_requested(), "no restart is pending at rest");

    input.key_down(haven::InputState::kKey2);
    expect(input.pending_choice() == 1, "key 2 offers choice 1");
    input.consume_choice();
    expect(input.pending_choice() == -1, "consume clears the pending choice");

    // The card keys must not disturb the movement keys.
    input.key_down(haven::InputState::kKey1);
    input.key_down(haven::InputState::kKeyD);
    expect(input.pending_choice() == 0, "key 1 offers choice 0");
    expect_vec(input.move_vector(), {1.0f, 0.0f}, 0.0f, "D still moves right with a choice pending");
    input.consume_choice();

    input.key_down(haven::InputState::kKeyR);
    expect(input.restart_requested(), "R requests a restart");
    input.consume_restart();
    expect(!input.restart_requested(), "consume clears the pending restart");

    // The choice script: one line per run event, chronological.
    std::vector<int8_t> script;
    expect(haven::load_choice_script("tests/data/choice_script.txt", script), "choice script loads");
    expect(script.size() == 5, "1+2+1+1 lines expand in order");
    expect(script[0] == 0 && script[1] == 2 && script[3] == -1, "picks and the restart marker decode");

    haven::InputState scripted;
    scripted.set_choice_script(script);
    expect(scripted.has_choice_script(), "the script is armed");
    expect(scripted.peek_scripted_line() == 0, "peeking does not consume the line");
    expect(scripted.peek_scripted_line() == 0, "peeking twice still does not");
    expect(scripted.take_scripted_choice() == 0, "a choice line answers a level-up");
    expect(scripted.take_scripted_choice() == 2, "the next choice line follows");
    expect(scripted.take_scripted_choice() == 1, "the third choice line follows");

    // The fourth line is a restart marker. It names a game over, so a level-up
    // must leave it alone: spending it as choice 0 would answer an offer with a
    // pick nobody made and leave the run with no restart when it ends.
    expect(scripted.peek_scripted_line() == -1, "the fourth line is a restart marker");
    expect(scripted.take_scripted_choice() == -1, "a restart marker is not a choice");
    expect(scripted.peek_scripted_line() == -1, "and the level-up left it in place");
    expect(scripted.take_scripted_restart(), "the restart marker is consumed by the game over it names");

    // The fifth line is a choice, which the game over must leave alone.
    expect(scripted.peek_scripted_line() == 1, "the fifth line is a choice");
    expect(!scripted.take_scripted_restart(), "a choice line is not a restart");
    expect(scripted.peek_scripted_line() == 1, "and the game over left it in place");
    expect(scripted.take_scripted_choice() == 1, "the level-up then consumes it");

    expect(scripted.peek_scripted_line() == -3, "an exhausted script reports empty");
    expect(scripted.take_scripted_choice() == -3, "an exhausted script offers no choice");
    expect(!scripted.take_scripted_restart(), "an exhausted script offers no restart");

    haven::InputState unarmed;
    expect(unarmed.peek_scripted_line() == -3, "no script peeks empty");
    expect(unarmed.take_scripted_choice() == -3, "no script offers no choice");
    expect(!unarmed.take_scripted_restart(), "no script offers no restart");

    expect(!haven::load_choice_script("tests/data/does_not_exist.txt", script), "missing script is rejected");
    expect(!haven::load_choice_script("tests/data/choice_script_bad.txt", script), "malformed script is rejected");
}

}  // namespace

int main()
{
    // The client keeps both streams unbuffered so a crash cannot lose the
    // verdict; the test harness has the same reason to.
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);

    test_vec_math();
    test_integrator();
    test_position_quantisation();
    test_angle_quantisation();
    test_hello_packet();
    test_input_packet();
    test_welcome_packet();
    test_welcome_packet_phase4();
    test_snapshot_packet();
    test_snapshot_decoder_validation();
    test_snapshot_delta_packet();
    test_snapshot_state_baseline();
    test_snapshot_state_baseline_match();
    test_snapshot_ingest_refuses_without_advancing();
    test_snapshot_ingest_unsolicited_restart();
    test_ack_retry();
    test_ack_retry_cleared_on_restart();
    test_full_snapshot_recovery_and_clock_reset();
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
    test_choice_input();
    test_restart_boundary();
    test_restart_boundary_proves_run_identity();
    test_restart_boundary_zero_clock();
    test_levelup_gate();
    test_restart_gate();
    test_entity_keys();
    test_circle_and_triangle_primitives();
    test_arc_and_hp_bar();
    test_arc_triangulation();
    test_enemy_body_and_hp_bar();
    test_enemy_radius_and_shape_tables();
    test_colour_slots();
    test_elite_outline_and_telegraph_slots();
    test_wireframe_rect();
    test_gem_geometry();
    test_levelup_packets();
    test_ack_packet();
    test_levelup_offer_packet();
    test_game_over_packet();
    test_loadout_packet();
    test_item_catalog();
    test_hud_text();
    test_xp_bar_fill();
    test_evolution_watch();
    test_projectile_shapes();
    test_collect_draw_items();
    test_empty_buffer_collects_nothing();

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
