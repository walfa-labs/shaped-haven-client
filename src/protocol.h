#pragma once

#include "game_math.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

// Wire protocol v1, DESIGN.md section 5.
//
// Every packet starts with a 7 byte header: msg_type u8, payload_len u16,
// seq u32. Framing is little-endian, TCP_NODELAY is enabled by the client.
//
// DESIGN.md section 2 and section 5 disagree on the snapshot header: section 2
// lists a 24 byte header carrying baseline_tick and four u16 counts, while the
// section 5 message table lists the arrays without them. This implementation
// follows section 2 for the fixed header (so FULL and DELTA share one header
// shape) and keeps the four counts, which is what lets the client split the
// trailing arrays without guessing record sizes. PROTOCOL.md in this repo is the
// authoritative byte layout both repositories must agree on.

namespace haven {

enum class MsgType : uint8_t {
    C2S_HELLO = 0x01,
    C2S_INPUT = 0x02,
    C2S_LEVELUP_CHOICE = 0x03,
    C2S_ACK_SNAPSHOT = 0x04,
    S2C_WELCOME = 0x81,
    S2C_SNAPSHOT_FULL = 0x82,
    S2C_SNAPSHOT_DELTA = 0x83,
    S2C_LEVELUP_OFFER = 0x84,
    S2C_GAME_OVER = 0x85,
};

constexpr uint8_t kProtocolVersion = 1;
constexpr size_t kHeaderSize = 7;
constexpr size_t kMaxNameLength = 16;
constexpr size_t kMaxPlayers = 4;

/// PROTOCOL.md fixed sizes: the snapshot's fixed header, and one record of each
/// trailing array. The decoder uses these to size-check the counts before it
/// trusts them enough to allocate.
constexpr size_t kSnapshotHeaderSize = 37;  // 3 x u32 + 4 x u32 + u8 + 4 x u16
constexpr size_t kPlayerRecordSize = 8;
constexpr size_t kEnemyRecordSize = 12;
constexpr size_t kProjRecordSize = 12;
constexpr size_t kGemRecordSize = 12;

enum class GameState : uint8_t {
    Running = 0,
    LevelUpPause = 1,
    GameOver = 2,
};

/// Header shared by every packet, including the ones this phase never sends.
struct PacketHeader {
    uint8_t msg_type = 0;
    uint16_t payload_len = 0;
    uint32_t seq = 0;
};

class ByteWriter {
public:
    void u8(uint8_t v) { buf_.push_back(v); }
    void boolean(bool v) { u8(v ? 1 : 0); }
    void u16(uint16_t v);
    void u32(uint32_t v);
    void i16(int16_t v) { u16(static_cast<uint16_t>(v)); }
    void u64(uint64_t v);
    void raw(const void* data, size_t len);
    void str_fixed(const std::string& text, size_t field_len);

    const uint8_t* data() const { return buf_.data(); }
    size_t size() const { return buf_.size(); }
    void clear() { buf_.clear(); }

private:
    std::vector<uint8_t> buf_;
};

class ByteReader {
public:
    ByteReader() = default;
    explicit ByteReader(const uint8_t* data, size_t len) : data_(data), len_(len) {}

    bool u8(uint8_t& out);
    bool boolean(bool& out) { uint8_t v = 0; if (!u8(v)) return false; out = v != 0; return true; }
    bool u16(uint16_t& out);
    bool u32(uint32_t& out);
    bool i16(int16_t& out) { uint16_t v = 0; if (!u16(v)) return false; out = static_cast<int16_t>(v); return true; }
    bool u64(uint64_t& out);
    bool vec2_raw(int16_t& x, int16_t& y);
    bool vec2(Vec2& out);
    bool skip(size_t len);
    size_t remaining() const { return len_ - pos_; }

private:
    const uint8_t* data_ = nullptr;
    size_t len_ = 0;
    size_t pos_ = 0;
};

std::vector<uint8_t> encode_packet(uint8_t msg_type, uint32_t seq, const std::vector<uint8_t>& payload);

std::vector<uint8_t> encode_hello(uint64_t session_token, uint8_t requested_slot, const std::string& name);
std::vector<uint8_t> encode_input(uint32_t input_seq, uint32_t client_tick, Vec2 move, uint16_t aim_angle, uint8_t buttons);

bool decode_header(const uint8_t* data, size_t len, PacketHeader& out);

struct Welcome {
    uint8_t player_id = 0;
    uint64_t session_token = 0;
    uint8_t tick_hz = 0;
    uint8_t snap_hz = 0;
    uint16_t arena_half_size = 0;
    uint32_t server_tick = 0;
};

bool decode_welcome(const uint8_t* payload, size_t len, Welcome& out);

struct PlayerNet {
    uint8_t id = 0;
    uint8_t state_flags = 0;
    Vec2 pos;
    float vel_angle = 0.0f;
    uint8_t hp_pct = 0;
};

struct EnemyNet {
    uint32_t id = 0;
    uint8_t archetype = 0;
    uint8_t elite_mask = 0;
    Vec2 pos;
    float vel_angle = 0.0f;
    uint8_t hp_pct = 0;
};

struct ProjNet {
    uint32_t id = 0;
    uint8_t owner_id = 0;
    uint8_t weapon_id = 0;
    uint8_t shape = 0;
    Vec2 pos;
    float vel_angle = 0.0f;
};

struct GemNet {
    uint32_t id = 0;
    Vec2 pos;
    uint16_t xp_value = 0;
    uint8_t magnet_target_id = 0;
};

struct SnapshotFull {
    uint32_t server_tick = 0;
    uint32_t baseline_tick = 0;
    uint32_t run_time_ms = 0;
    GameState game_state = GameState::Running;
    std::array<uint32_t, kMaxPlayers> acked_seq{};
    uint16_t player_count = 0;
    uint16_t enemy_count = 0;
    uint16_t proj_count = 0;
    uint16_t gem_count = 0;
    std::vector<PlayerNet> players;
    std::vector<EnemyNet> enemies;
    std::vector<ProjNet> projs;
    std::vector<GemNet> gems;
};

bool decode_snapshot_full(const uint8_t* payload, size_t len, SnapshotFull& out);

/// True when `a` is at or before `b` in the u32 sequence space, tolerating the
/// wrap that happens after ~50 days of continuous play.
bool seq_at_or_before(uint32_t a, uint32_t b);

}  // namespace haven
