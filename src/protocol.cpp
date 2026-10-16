#include "protocol.h"

namespace haven {

void ByteWriter::u16(uint16_t v)
{
    buf_.push_back(static_cast<uint8_t>(v & 0xFF));
    buf_.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
}

void ByteWriter::u32(uint32_t v)
{
    buf_.push_back(static_cast<uint8_t>(v & 0xFF));
    buf_.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
    buf_.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
    buf_.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
}

void ByteWriter::u64(uint64_t v)
{
    for (int shift = 0; shift < 64; shift += 8)
        buf_.push_back(static_cast<uint8_t>((v >> shift) & 0xFF));
}

void ByteWriter::raw(const void* data, size_t len)
{
    const uint8_t* bytes = static_cast<const uint8_t*>(data);
    buf_.insert(buf_.end(), bytes, bytes + len);
}

void ByteWriter::str_fixed(const std::string& text, size_t field_len)
{
    const size_t n = text.size() < field_len ? text.size() : field_len;
    raw(text.data(), n);
    for (size_t i = n; i < field_len; ++i)
        buf_.push_back(0);
}

bool ByteReader::u8(uint8_t& out)
{
    if (remaining() < 1) return false;
    out = data_[pos_++];
    return true;
}

bool ByteReader::u16(uint16_t& out)
{
    if (remaining() < 2) return false;
    out = static_cast<uint16_t>(data_[pos_] | (static_cast<uint16_t>(data_[pos_ + 1]) << 8));
    pos_ += 2;
    return true;
}

bool ByteReader::u32(uint32_t& out)
{
    if (remaining() < 4) return false;
    out = static_cast<uint32_t>(data_[pos_]) | (static_cast<uint32_t>(data_[pos_ + 1]) << 8) |
          (static_cast<uint32_t>(data_[pos_ + 2]) << 16) | (static_cast<uint32_t>(data_[pos_ + 3]) << 24);
    pos_ += 4;
    return true;
}

bool ByteReader::u64(uint64_t& out)
{
    if (remaining() < 8) return false;
    uint64_t v = 0;
    for (int shift = 0; shift < 64; shift += 8)
        v |= static_cast<uint64_t>(data_[pos_++]) << shift;
    out = v;
    return true;
}

bool ByteReader::vec2_raw(int16_t& x, int16_t& y)
{
    return i16(x) && i16(y);
}

bool ByteReader::vec2(Vec2& out)
{
    int16_t x = 0;
    int16_t y = 0;
    if (!vec2_raw(x, y)) return false;
    out = Vec2{decode_pos(x), decode_pos(y)};
    return true;
}

bool ByteReader::skip(size_t len)
{
    if (remaining() < len) return false;
    pos_ += len;
    return true;
}

std::vector<uint8_t> encode_packet(uint8_t msg_type, uint32_t seq, const std::vector<uint8_t>& payload)
{
    std::vector<uint8_t> packet;
    packet.reserve(kHeaderSize + payload.size());
    packet.push_back(msg_type);
    packet.push_back(static_cast<uint8_t>(payload.size() & 0xFF));
    packet.push_back(static_cast<uint8_t>((payload.size() >> 8) & 0xFF));
    for (int shift = 0; shift < 32; shift += 8)
        packet.push_back(static_cast<uint8_t>((seq >> shift) & 0xFF));
    packet.insert(packet.end(), payload.begin(), payload.end());
    return packet;
}

std::vector<uint8_t> encode_hello(uint64_t session_token, uint8_t requested_slot, const std::string& name)
{
    ByteWriter w;
    w.u8(kProtocolVersion);
    w.u8(requested_slot);
    w.u64(session_token);
    w.str_fixed(name, kMaxNameLength);

    std::vector<uint8_t> payload(w.data(), w.data() + w.size());
    return encode_packet(static_cast<uint8_t>(MsgType::C2S_HELLO), 0, payload);
}

std::vector<uint8_t> encode_input(uint32_t input_seq, uint32_t client_tick, Vec2 move, uint16_t aim_angle, uint8_t buttons)
{
    // move_x / move_y are i8 in [-100, 100]; diagonals are normalised first so
    // the quantized magnitude never exceeds the field.
    const Vec2 clamped{std::max(-1.0f, std::min(1.0f, move.x)),
                        std::max(-1.0f, std::min(1.0f, move.y))};
    const auto to_i8 = [](float v) {
        return static_cast<int8_t>(std::lround(std::max(-100, std::min(100, static_cast<int>(std::lround(v * 100.0f))))));
    };

    ByteWriter w;
    w.u32(input_seq);
    w.u32(client_tick);
    w.u8(static_cast<uint8_t>(to_i8(clamped.x)));
    w.u8(static_cast<uint8_t>(to_i8(clamped.y)));
    w.u16(aim_angle);
    w.u8(buttons);

    std::vector<uint8_t> payload(w.data(), w.data() + w.size());
    return encode_packet(static_cast<uint8_t>(MsgType::C2S_INPUT), input_seq, payload);
}

bool decode_header(const uint8_t* data, size_t len, PacketHeader& out)
{
    ByteReader r(data, len);
    if (!r.u8(out.msg_type)) return false;
    if (!r.u16(out.payload_len)) return false;
    if (!r.u32(out.seq)) return false;
    return true;
}

bool decode_welcome(const uint8_t* payload, size_t len, Welcome& out)
{
    ByteReader r(payload, len);
    uint8_t player_id = 0;
    uint8_t tick_hz = 0;
    uint8_t snap_hz = 0;
    uint16_t arena_half_size = 0;
    if (!r.u8(player_id) || !r.u64(out.session_token) || !r.u8(tick_hz) || !r.u8(snap_hz) ||
        !r.u16(arena_half_size) || !r.u32(out.server_tick))
        return false;

    out.player_id = player_id;
    out.tick_hz = tick_hz;
    out.snap_hz = snap_hz;
    out.arena_half_size = arena_half_size;
    return true;
}

bool decode_snapshot_full(const uint8_t* payload, size_t len, SnapshotFull& out)
{
    ByteReader r(payload, len);
    uint8_t game_state = 0;
    std::array<uint16_t, 4> counts{};
    if (!r.u32(out.server_tick) || !r.u32(out.baseline_tick) || !r.u32(out.run_time_ms))
        return false;
    for (uint32_t& acked : out.acked_seq)
        if (!r.u32(acked)) return false;
    if (!r.u8(game_state)) return false;
    for (uint16_t& count : counts)
        if (!r.u16(count)) return false;

    out.game_state = static_cast<GameState>(game_state);
    out.player_count = counts[0];
    out.enemy_count = counts[1];
    out.proj_count = counts[2];
    out.gem_count = counts[3];

    // The counts come off the wire, so they are attacker-controlled: a header
    // claiming 65,535 of each record would otherwise make the decoder allocate
    // megabytes before it discovers the bytes are not there. The records are
    // fixed size, so a well-formed payload carries exactly this many bytes after
    // the fixed header — anything else is a header/record mismatch, and nothing
    // is reserved until that is settled. The arithmetic is in size_t and the
    // counts are u16, so it cannot overflow.
    const size_t needed = static_cast<size_t>(out.player_count) * kPlayerRecordSize +
                          static_cast<size_t>(out.enemy_count) * kEnemyRecordSize +
                          static_cast<size_t>(out.proj_count) * kProjRecordSize +
                          static_cast<size_t>(out.gem_count) * kGemRecordSize;
    if (r.remaining() != needed) return false;

    out.players.resize(out.player_count);
    for (PlayerNet& p : out.players)
    {
        uint8_t vel_angle = 0;
        if (!r.u8(p.id) || !r.u8(p.state_flags) || !r.vec2(p.pos) || !r.u8(vel_angle)) return false;
        p.vel_angle = decode_angle(vel_angle);
        if (!r.u8(p.hp_pct)) return false;
    }

    out.enemies.resize(out.enemy_count);
    for (EnemyNet& e : out.enemies)
    {
        if (!r.u32(e.id) || !r.u8(e.archetype) || !r.u8(e.elite_mask) || !r.vec2(e.pos)) return false;
        uint8_t vel_angle = 0;
        if (!r.u8(vel_angle)) return false;
        e.vel_angle = decode_angle(vel_angle);
        if (!r.u8(e.hp_pct)) return false;
    }

    out.projs.resize(out.proj_count);
    for (ProjNet& p : out.projs)
    {
        if (!r.u32(p.id) || !r.u8(p.owner_id) || !r.u8(p.weapon_id) || !r.u8(p.shape) || !r.vec2(p.pos)) return false;
        uint8_t vel_angle = 0;
        if (!r.u8(vel_angle)) return false;
        p.vel_angle = decode_angle(vel_angle);
    }

    out.gems.resize(out.gem_count);
    for (GemNet& g : out.gems)
    {
        if (!r.u32(g.id) || !r.vec2(g.pos) || !r.u16(g.xp_value) || !r.u8(g.magnet_target_id)) return false;
        if (!r.skip(1)) return false;  // reserved, keeps GemNet 12 bytes like the rest
    }

    // A payload longer than the records it declares is a framing error, not
    // something to silently ignore: accepting it would hide a header/record
    // mismatch and leave the reader out of step on the next packet.
    return r.remaining() == 0;
}

bool seq_at_or_before(uint32_t a, uint32_t b)
{
    return static_cast<int32_t>(a - b) <= 0;
}

}  // namespace haven
