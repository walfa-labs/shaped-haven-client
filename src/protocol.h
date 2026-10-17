#pragma once

#include "game_math.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

// Wire protocol v2, per scratch/protocol-v2-spec.md.
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
    C2S_RESTART = 0x05,
    S2C_WELCOME = 0x81,
    S2C_SNAPSHOT_FULL = 0x82,
    S2C_SNAPSHOT_DELTA = 0x83,
    S2C_LEVELUP_OFFER = 0x84,
    S2C_GAME_OVER = 0x85,
    S2C_LOADOUT = 0x86,
};

constexpr uint8_t kProtocolVersion = 2;
constexpr size_t kHeaderSize = 7;
constexpr size_t kMaxNameLength = 16;
constexpr size_t kMaxPlayers = 4;

/// PROTOCOL.md fixed sizes: the snapshot's fixed header, and one record of each
/// trailing array. The decoder uses these to size-check the counts before it
/// trusts them enough to allocate.
constexpr size_t kSnapshotHeaderSize = 37;  // 3 x u32 + 4 x u32 + u8 + 4 x u16
constexpr size_t kPlayerRecordSize = 20;
constexpr size_t kEnemyRecordSize = 18;
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

std::vector<uint8_t> encode_hello(uint64_t session_token, uint8_t race_id, uint8_t starting_weapon_id, const std::string& name);
std::vector<uint8_t> encode_input(uint32_t input_seq, uint32_t client_tick, uint8_t buttons, int8_t axis_x, int8_t axis_y);

/// C2S_LEVELUP_CHOICE (0x03): event_id u16, choice_idx u8 — 3 byte payload.
std::vector<uint8_t> encode_levelup_choice(uint16_t event_id, uint8_t choice_idx);

/// C2S_RESTART (0x05): empty payload. Only meaningful while game_state is
/// game over; the server ignores it otherwise.
std::vector<uint8_t> encode_restart();

/// C2S_ACK_SNAPSHOT (0x04) — 4 byte payload: last_recv_server_tick u32. This is
/// what tells the server which tick the client holds a complete state for, so
/// it can pick a baseline to diff against. Sent at most once per snapshot.
std::vector<uint8_t> encode_ack_snapshot(uint32_t last_recv_server_tick);

bool decode_header(const uint8_t* data, size_t len, PacketHeader& out);

struct Welcome {
    uint8_t player_id = 0;
    uint64_t session_token = 0;
    uint8_t tick_hz = 0;
    uint8_t snap_hz = 0;
    uint16_t arena_half_size = 0;
    uint32_t server_tick = 0;
    /// Phase 4: the server's interest radius in world units, from the 21 byte
    /// `S2C_WELCOME`. Zero when the server still speaks the 17 byte phase-3
    /// layout, which carries no such field.
    uint16_t interest_radius = 0;
    /// True when the payload carried the phase-4 interest_radius field, i.e.
    /// when it was the 21 byte layout rather than the 17 byte one.
    bool has_interest_radius = false;
};

/// Decodes either `S2C_WELCOME` layout. The first 13 bytes are identical across
/// both; the phase-4 form appends `interest_radius` u16 and `reserved` u16
/// before moving `server_tick` from offset 13 to 17, growing the payload from
/// 17 to 21 bytes. The payload length in the frame header is what tells them
/// apart, so a phase-3 server and a phase-4 server both decode.
bool decode_welcome(const uint8_t* payload, size_t len, Welcome& out);

struct PlayerNet {
    uint8_t id = 0;
    uint8_t state_flags = 0;
    uint8_t race_id = 0;
    Vec2 pos;
    float vel_angle = 0.0f;
    uint8_t hp_pct = 0;
    uint8_t shield_pct = 0;
    uint8_t mana_pct = 0;
    uint16_t dash_cd_remaining = 0;
    uint16_t dash_cd_total = 0;
    uint8_t is_dashing = 0;
    uint16_t max_hp = 0;
    uint16_t max_shield = 0;
};

struct EnemyNet {
    uint32_t id = 0;
    uint8_t archetype = 0;
    uint8_t elite_mask = 0;
    Vec2 pos;
    float vel_angle = 0.0f;
    uint8_t hp_pct = 0;
    uint8_t telegraph_progress = 0;
    uint8_t telegraph_shape = 0;
    uint16_t telegraph_angle = 0;
    uint16_t telegraph_radius = 0;
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

/// The `kind` byte that heads every spawned and updated record in a delta.
/// It names the record type that follows, so the decoder can split the
/// trailing arrays without guessing record sizes.
enum class DeltaKind : uint8_t {
    Player = 0,
    Enemy = 1,
    Projectile = 2,
    Gem = 3,
};

/// S2C_SNAPSHOT_DELTA (0x83) — phase 4. The fixed header is byte-for-byte the
/// same shape as a full snapshot's; only the four counts mean something
/// different (spawned, updated, destroyed, reserved). The three trailing
/// arrays carry interest-management changes relative to `baseline_tick`: an
/// entity the client has never seen, an entity whose full record changed, and
/// an entity that left the interest radius. See PROTOCOL.md.
struct SnapshotDelta {
    uint32_t server_tick = 0;
    uint32_t baseline_tick = 0;
    uint32_t run_time_ms = 0;
    GameState game_state = GameState::Running;
    std::array<uint32_t, kMaxPlayers> acked_seq{};
    uint16_t spawned_count = 0;
    uint16_t updated_count = 0;
    uint16_t destroyed_count = 0;

    // Spawned records: kind implied by which vector they land in.
    std::vector<PlayerNet> spawned_players;
    std::vector<EnemyNet> spawned_enemies;
    std::vector<ProjNet> spawned_projs;
    std::vector<GemNet> spawned_gems;
    // Updated records: same, carrying the full record rather than a mask.
    std::vector<PlayerNet> updated_players;
    std::vector<EnemyNet> updated_enemies;
    std::vector<ProjNet> updated_projs;
    std::vector<GemNet> updated_gems;
    // Destroyed records: just the id, per kind.
    std::vector<uint32_t> destroyed_players;
    std::vector<uint32_t> destroyed_enemies;
    std::vector<uint32_t> destroyed_projs;
    std::vector<uint32_t> destroyed_gems;
};

/// Decodes the shared 37 byte snapshot header both message types start with:
/// the three u32s, the four acked sequences, the game state byte, and the four
/// u16 counts. For a full snapshot the counts are the entity counts; for a
/// delta they are spawned / updated / destroyed / reserved. Returns false if
/// the payload is too short to hold the header.
bool decode_snapshot_header(const uint8_t* payload, size_t len, uint32_t& server_tick, uint32_t& baseline_tick,
                            uint32_t& run_time_ms, std::array<uint32_t, kMaxPlayers>& acked_seq, GameState& game_state,
                            std::array<uint16_t, 4>& counts);

bool decode_snapshot_full(const uint8_t* payload, size_t len, SnapshotFull& out);

/// Decodes a delta. The counts come off the wire, so they are validated as the
/// records are read: every record is pulled through a ByteReader that checks
/// the remaining length first, and the payload must be exactly consumed. A
/// `kind` byte outside 0..3, a payload that does not hold every record the
/// counts declare, a non-zero reserved count, or an updated record whose own id
/// disagrees with the `id` that heads it, is rejected.
bool decode_snapshot_delta(const uint8_t* payload, size_t len, SnapshotDelta& out);

/// S2C_LEVELUP_OFFER (0x84) — 29 byte payload: player_id u8, event_id u16,
/// new_level u16, then three 8-byte UpgradeOptionNet. The run pauses until the
/// client answers with C2S_LEVELUP_CHOICE carrying the same event_id, or the
/// server's 15 s timeout auto-selects option 0.
struct UpgradeOptionNet {
    uint8_t item_id = 0;
    uint8_t next_level = 0;
    uint8_t is_evo = 0;
    uint8_t stat_type = 0;
    int16_t current_stat_fp = 0;
    int16_t upgraded_stat_fp = 0;
};

/// `UpgradeOptionNet.stat_type` values, in the server's wire order. The two
/// fixed-point fields are the stat's value times 100 (500 reads as 5.0); the
/// HUD formats them as the level-up card's comparison string.
enum class StatType : uint8_t {
    Damage = 0,
    Armor = 1,
    MaxHP = 2,
    MoveSpeed = 3,
    Cooldown = 4,
    Area = 5,
    Projectiles = 6,
    CritChance = 7,
    MaxMana = 8,
    MaxShield = 9,
};

struct LevelUpOffer {
    uint8_t player_id = 0;
    uint16_t event_id = 0;
    uint16_t new_level = 0;
    std::array<UpgradeOptionNet, 3> options{};
};

/// S2C_GAME_OVER (0x85) — 16 byte payload. PROTOCOL.md fixes the layout at
/// these four scalars; DESIGN.md's summaries array is not on the wire.
struct GameOverStats {
    uint32_t final_tick = 0;
    uint32_t seconds_survived = 0;
    uint32_t score = 0;
    uint32_t kills = 0;
};

/// S2C_LOADOUT (0x86) — 45 byte payload, always the full 4 + 10 slots with
/// 0xFF filling the unused ones. Sent whenever level, XP or the item set
/// changes, so the HUD never polls.
struct LoadoutSlot {
    uint8_t item_id = 0;
    uint8_t level = 0;
    uint8_t evolved = 0;  // weapons only; passives read level then stop
};

struct Loadout {
    uint8_t player_id = 0;
    uint16_t level = 0;
    uint32_t xp = 0;
    uint32_t xp_next = 0;
    uint8_t weapon_count = 0;
    std::array<LoadoutSlot, 4> weapons{};
    uint8_t passive_count = 0;
    std::array<LoadoutSlot, 10> passives{};
};

bool decode_levelup_offer(const uint8_t* payload, size_t len, LevelUpOffer& out);
bool decode_game_over(const uint8_t* payload, size_t len, GameOverStats& out);
bool decode_loadout(const uint8_t* payload, size_t len, Loadout& out);

bool decode_snapshot_full(const uint8_t* payload, size_t len, SnapshotFull& out);

/// True when `a` is at or before `b` in the u32 sequence space, tolerating the
/// wrap that happens after ~50 days of continuous play.
bool seq_at_or_before(uint32_t a, uint32_t b);

}  // namespace haven
