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

std::vector<uint8_t> encode_hello(uint8_t race_id, uint8_t starting_weapon_id, const std::string& name)
{
    ByteWriter w;
    w.u8(kProtocolVersion);
    w.u8(race_id);
    w.u8(starting_weapon_id);
    w.u8(0);  // reserved
    w.str_fixed(name, kMaxNameLength);

    std::vector<uint8_t> payload(w.data(), w.data() + w.size());
    return encode_packet(static_cast<uint8_t>(MsgType::C2S_HELLO), 0, payload);
}

std::vector<uint8_t> encode_input(uint32_t input_seq, uint32_t client_tick, uint8_t buttons, int8_t axis_x, int8_t axis_y)
{
    ByteWriter w;
    w.u32(input_seq);
    w.u32(client_tick);
    w.u8(buttons);
    w.u8(static_cast<uint8_t>(axis_x));
    w.u8(static_cast<uint8_t>(axis_y));

    std::vector<uint8_t> payload(w.data(), w.data() + w.size());
    return encode_packet(static_cast<uint8_t>(MsgType::C2S_INPUT), input_seq, payload);
}

std::vector<uint8_t> encode_levelup_choice(uint16_t event_id, uint8_t choice_idx)
{
    ByteWriter w;
    w.u16(event_id);
    w.u8(choice_idx);

    std::vector<uint8_t> payload(w.data(), w.data() + w.size());
    return encode_packet(static_cast<uint8_t>(MsgType::C2S_LEVELUP_CHOICE), 0, payload);
}

std::vector<uint8_t> encode_restart()
{
    return encode_packet(static_cast<uint8_t>(MsgType::C2S_RESTART), 0, {});
}

std::vector<uint8_t> encode_ack_snapshot(uint32_t last_recv_server_tick)
{
    ByteWriter w;
    w.u32(last_recv_server_tick);

    std::vector<uint8_t> payload(w.data(), w.data() + w.size());
    return encode_packet(static_cast<uint8_t>(MsgType::C2S_ACK_SNAPSHOT), 0, payload);
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
    // The first 13 bytes are identical in both layouts: player_id u8,
    // session_token u64, tick_hz u8, snap_hz u8, arena_half_size u16.
    if (!r.u8(player_id) || !r.u64(out.session_token) || !r.u8(tick_hz) || !r.u8(snap_hz) ||
        !r.u16(arena_half_size))
        return false;

    out.player_id = player_id;
    out.tick_hz = tick_hz;
    out.snap_hz = snap_hz;
    out.arena_half_size = arena_half_size;
    out.has_interest_radius = false;
    out.interest_radius = 0;

    // What follows is one of two forms, told apart by the payload length the
    // frame header declared: the 17 byte phase-3 layout carries only a u32
    // server_tick here, while the 21 byte phase-4 layout inserts a u16
    // interest_radius and a u16 reserved before it.
    if (r.remaining() == 4)
    {
        return r.u32(out.server_tick);
    }
    if (r.remaining() == 8)
    {
        uint16_t reserved = 0;
        if (!r.u16(out.interest_radius) || !r.u16(reserved) || !r.u32(out.server_tick))
            return false;
        out.has_interest_radius = true;
        return true;
    }
    return false;
}

bool decode_snapshot_header(const uint8_t* payload, size_t len, uint32_t& server_tick, uint32_t& baseline_tick,
                            uint32_t& run_time_ms, std::array<uint32_t, kMaxPlayers>& acked_seq, GameState& game_state,
                            std::array<uint16_t, 4>& counts)
{
    ByteReader r(payload, len);
    uint8_t state = 0;
    if (!r.u32(server_tick) || !r.u32(baseline_tick) || !r.u32(run_time_ms))
        return false;
    for (uint32_t& acked : acked_seq)
        if (!r.u32(acked)) return false;
    if (!r.u8(state)) return false;
    for (uint16_t& count : counts)
        if (!r.u16(count)) return false;
    game_state = static_cast<GameState>(state);
    return true;
}

bool decode_snapshot_full(const uint8_t* payload, size_t len, SnapshotFull& out)
{
    std::array<uint32_t, kMaxPlayers> acked{};
    std::array<uint16_t, 4> counts{};
    if (!decode_snapshot_header(payload, len, out.server_tick, out.baseline_tick, out.run_time_ms, acked,
                                out.game_state, counts))
        return false;
    out.acked_seq = acked;
    out.player_count = counts[0];
    out.enemy_count = counts[1];
    out.proj_count = counts[2];
    out.gem_count = counts[3];

    ByteReader r(payload + kSnapshotHeaderSize, len - kSnapshotHeaderSize);

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
        if (!r.u8(p.id) || !r.u8(p.state_flags) || !r.u8(p.race_id) || !r.vec2(p.pos) || !r.u8(vel_angle))
            return false;
        p.vel_angle = decode_angle(vel_angle);
        if (!r.u8(p.hp_pct) || !r.u8(p.shield_pct) || !r.u8(p.mana_pct)) return false;
        if (!r.u16(p.dash_cd_remaining) || !r.u16(p.dash_cd_total)) return false;
        if (!r.u8(p.is_dashing)) return false;
        if (!r.u16(p.max_hp) || !r.u16(p.max_shield)) return false;
    }

    out.enemies.resize(out.enemy_count);
    for (EnemyNet& e : out.enemies)
    {
        if (!r.u32(e.id) || !r.u8(e.archetype) || !r.u8(e.elite_mask) || !r.vec2(e.pos)) return false;
        uint8_t vel_angle = 0;
        if (!r.u8(vel_angle)) return false;
        e.vel_angle = decode_angle(vel_angle);
        if (!r.u8(e.hp_pct)) return false;
        if (!r.u8(e.telegraph_progress) || !r.u8(e.telegraph_shape)) return false;
        if (!r.i16(e.telegraph_angle) || !r.u16(e.telegraph_radius)) return false;
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

namespace {

// Reads one record of the kind `kind_byte` names from `r`. Every field goes
// through the ByteReader, which checks the remaining length first, so a count
// larger than the payload is caught the moment a record runs off the end
// rather than by trusting the count up front. Returns false on a short read or
// a kind byte outside 0..3.
//
// For an updated record the caller has already read the separate `id` u32 that
// heads it, and passes it as `external_id`; the record's own id field must
// agree with it. PROTOCOL.md sends the id twice for an updated entity, and the
// two disagreeing means the record and its key describe different entities —
// applying it would write one entity's state under another's key.
bool read_delta_record(ByteReader& r, uint8_t kind_byte, SnapshotDelta& out, bool spawned, uint32_t external_id)
{
    const DeltaKind kind = static_cast<DeltaKind>(kind_byte);
    switch (kind)
    {
        case DeltaKind::Player:
        {
            PlayerNet p;
            uint8_t vel_angle = 0;
            if (!r.u8(p.id) || !r.u8(p.state_flags) || !r.u8(p.race_id) || !r.vec2(p.pos) || !r.u8(vel_angle))
                return false;
            if (!r.u8(p.hp_pct) || !r.u8(p.shield_pct) || !r.u8(p.mana_pct)) return false;
            if (!r.u16(p.dash_cd_remaining) || !r.u16(p.dash_cd_total)) return false;
            if (!r.u8(p.is_dashing)) return false;
            if (!r.u16(p.max_hp) || !r.u16(p.max_shield)) return false;
            if (!spawned && static_cast<uint32_t>(p.id) != external_id) return false;
            p.vel_angle = decode_angle(vel_angle);
            (spawned ? out.spawned_players : out.updated_players).push_back(p);
            return true;
        }
        case DeltaKind::Enemy:
        {
            EnemyNet e;
            uint8_t vel_angle = 0;
            if (!r.u32(e.id) || !r.u8(e.archetype) || !r.u8(e.elite_mask) || !r.vec2(e.pos) || !r.u8(vel_angle))
                return false;
            if (!r.u8(e.hp_pct)) return false;
            if (!r.u8(e.telegraph_progress) || !r.u8(e.telegraph_shape)) return false;
            if (!r.i16(e.telegraph_angle) || !r.u16(e.telegraph_radius)) return false;
            if (!spawned && e.id != external_id) return false;
            e.vel_angle = decode_angle(vel_angle);
            (spawned ? out.spawned_enemies : out.updated_enemies).push_back(e);
            return true;
        }
        case DeltaKind::Projectile:
        {
            ProjNet p;
            uint8_t vel_angle = 0;
            if (!r.u32(p.id) || !r.u8(p.owner_id) || !r.u8(p.weapon_id) || !r.u8(p.shape) || !r.vec2(p.pos) ||
                !r.u8(vel_angle))
                return false;
            if (!spawned && p.id != external_id) return false;
            p.vel_angle = decode_angle(vel_angle);
            (spawned ? out.spawned_projs : out.updated_projs).push_back(p);
            return true;
        }
        case DeltaKind::Gem:
        {
            GemNet g;
            if (!r.u32(g.id) || !r.vec2(g.pos) || !r.u16(g.xp_value) || !r.u8(g.magnet_target_id) || !r.skip(1))
                return false;
            if (!spawned && g.id != external_id) return false;
            (spawned ? out.spawned_gems : out.updated_gems).push_back(g);
            return true;
        }
    }
    return false;
}

// Reads `count` (kind, id) pairs into the matching per-kind id vector.
bool read_destroyed(ByteReader& r, uint16_t count, SnapshotDelta& out)
{
    for (uint16_t i = 0; i < count; ++i)
    {
        uint8_t kind_byte = 0;
        uint32_t id = 0;
        if (!r.u8(kind_byte) || !r.u32(id)) return false;
        switch (static_cast<DeltaKind>(kind_byte))
        {
            case DeltaKind::Player: out.destroyed_players.push_back(id); break;
            case DeltaKind::Enemy: out.destroyed_enemies.push_back(id); break;
            case DeltaKind::Projectile: out.destroyed_projs.push_back(id); break;
            case DeltaKind::Gem: out.destroyed_gems.push_back(id); break;
            default: return false;
        }
    }
    return true;
}

}  // namespace

bool decode_snapshot_delta(const uint8_t* payload, size_t len, SnapshotDelta& out)
{
    std::array<uint32_t, kMaxPlayers> acked{};
    std::array<uint16_t, 4> counts{};
    if (!decode_snapshot_header(payload, len, out.server_tick, out.baseline_tick, out.run_time_ms, acked,
                                out.game_state, counts))
        return false;
    out.acked_seq = acked;
    out.spawned_count = counts[0];
    out.updated_count = counts[1];
    out.destroyed_count = counts[2];
    // counts[3] is reserved, and PROTOCOL.md fixes it at zero. A non-zero value
    // is not a field a future revision may reuse silently: the frozen layout
    // says the header is 37 bytes with a zero there, so anything else is a
    // header that disagrees with the format this client implements.
    if (counts[3] != 0) return false;

    ByteReader r(payload + kSnapshotHeaderSize, len - kSnapshotHeaderSize);

    for (uint16_t i = 0; i < out.spawned_count; ++i)
    {
        uint8_t kind_byte = 0;
        if (!r.u8(kind_byte)) return false;
        if (!read_delta_record(r, kind_byte, out, /*spawned=*/true, 0)) return false;
    }
    for (uint16_t i = 0; i < out.updated_count; ++i)
    {
        uint8_t kind_byte = 0;
        uint32_t id = 0;
        if (!r.u8(kind_byte) || !r.u32(id)) return false;
        if (!read_delta_record(r, kind_byte, out, /*spawned=*/false, id)) return false;
    }
    if (!read_destroyed(r, out.destroyed_count, out)) return false;

    // The payload must be exactly the three arrays and nothing more: trailing
    // bytes would mean the header and the records disagree, which is a framing
    // error rather than something to silently ignore.
    return r.remaining() == 0;
}

bool decode_levelup_offer(const uint8_t* payload, size_t len, LevelUpOffer& out)
{
    // player_id(1) + event_id(2) + new_level(2) + 3 x option(8) = 29, exactly
    // as the protocol v2 spec fixes it; the arrays are fixed length, so the
    // length is part of the format.
    if (len != 29) return false;

    ByteReader r(payload, len);
    if (!r.u8(out.player_id) || !r.u16(out.event_id) || !r.u16(out.new_level)) return false;
    for (UpgradeOptionNet& option : out.options)
    {
        if (!r.u8(option.item_id) || !r.u8(option.next_level) || !r.u8(option.is_evo)) return false;
        if (!r.u8(option.stat_type)) return false;
        if (!r.i16(option.current_stat_fp) || !r.i16(option.upgraded_stat_fp)) return false;
    }
    return true;
}

bool decode_game_over(const uint8_t* payload, size_t len, GameOverStats& out)
{
    // 4 x u32 = 16, exactly as PROTOCOL.md fixes it.
    if (len != 16) return false;

    ByteReader r(payload, len);
    return r.u32(out.final_tick) && r.u32(out.seconds_survived) && r.u32(out.score) &&
           r.u32(out.kills);
}

bool decode_loadout(const uint8_t* payload, size_t len, Loadout& out)
{
    // player_id(1) + level(2) + xp(4) + xp_next(4) + weapon_count(1) +
    // 4 x weapon(3) + passive_count(1) + 10 x passive(2) = 45, always the full
    // slots with 0xFF filling the unused ones.
    if (len != 45) return false;

    ByteReader r(payload, len);
    if (!r.u8(out.player_id) || !r.u16(out.level) || !r.u32(out.xp) || !r.u32(out.xp_next) ||
        !r.u8(out.weapon_count))
        return false;

    // The payload always carries all four weapon slots and all ten passive
    // slots, so a count past the array it indexes is a payload the sender got
    // wrong. Accepting it would have the HUD walk past the slots it was given
    // and build a tray that does not match the game.
    if (static_cast<size_t>(out.weapon_count) > out.weapons.size()) return false;

    for (LoadoutSlot& slot : out.weapons)
    {
        if (!r.u8(slot.item_id) || !r.u8(slot.level) || !r.u8(slot.evolved)) return false;
    }

    if (!r.u8(out.passive_count)) return false;
    if (static_cast<size_t>(out.passive_count) > out.passives.size()) return false;

    for (LoadoutSlot& slot : out.passives)
    {
        if (!r.u8(slot.item_id) || !r.u8(slot.level)) return false;
    }
    return true;
}

bool seq_at_or_before(uint32_t a, uint32_t b)
{
    return static_cast<int32_t>(a - b) <= 0;
}

}  // namespace haven
