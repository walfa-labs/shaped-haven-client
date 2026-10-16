# Protocol v1 — byte layout

Authoritative layout for the phase 1 wire protocol. DESIGN.md §5 gives the
message list and §2 the snapshot header; where the two disagree this file wins.

Both repos must agree on it. If you change anything here, change it in both.

## Framing

Every packet: a 7 byte header followed by `payload_len` bytes.

| Offset | Size | Field |
| --- | --- | --- |
| 0 | u8 | `msg_type` |
| 1 | u16 | `payload_len` (little-endian) |
| 3 | u32 | `seq` |

All multi-byte fields are little-endian. `TCP_NODELAY` is set by the client.

`seq` is the client's `input_seq` for `C2S_INPUT` and the server tick for
`S2C_SNAPSHOT_*`; it is 0 elsewhere.

## `C2S_HELLO` (0x01) — 26 byte payload

| Offset | Size | Field |
| --- | --- | --- |
| 0 | u8 | `proto_ver` = 1 |
| 1 | u8 | `requested_slot` (0 = any) |
| 2 | u64 | `session_token` |
| 10 | 16 | `name`, zero padded |

## `C2S_INPUT` (0x02) — 13 byte payload

| Offset | Size | Field |
| --- | --- | --- |
| 0 | u4 | `input_seq` (u32) |
| 4 | u4 | `client_tick` (u32) |
| 8 | i1 | `move_x`, −100..100 |
| 9 | i1 | `move_y`, −100..100 |
| 10 | u2 | `aim_angle` |
| 12 | u1 | `buttons` |

## `S2C_WELCOME` (0x81) — 17 byte payload

| Offset | Size | Field |
| --- | --- | --- |
| 0 | u8 | `player_id` |
| 1 | u64 | `session_token` (echoed) |
| 9 | u8 | `tick_hz` = 60 |
| 10 | u8 | `snap_hz` = 20 |
| 11 | u16 | `arena_half_size` = 1000 |
| 13 | u32 | `server_tick` |

## `S2C_SNAPSHOT_FULL` (0x82)

DESIGN.md §2 and §5 disagree here. §2 lists a header carrying `baseline_tick`
and four `u16` counts and calls it 24 bytes; the field list in §2 actually sums
to 37, and §5 omits `baseline_tick` and the counts entirely.

This implementation follows §2 for the fixed header — so `FULL` and `DELTA`
share one header shape — and keeps the four counts, because without them the
client cannot split the trailing arrays without guessing record sizes.

Fixed header, 37 bytes:

| Offset | Size | Field |
| --- | --- | --- |
| 0 | u32 | `server_tick` |
| 4 | u32 | `baseline_tick` (0 for a full snapshot) |
| 8 | u32 | `run_time_ms` — also the timestamp the client interpolates against |
| 12 | 4 × u32 | `acked_seq[4]`, one per player slot |
| 28 | u8 | `game_state` |
| 29 | 4 × u16 | `counts` = players, enemies, projectiles, gems |
| 37 | … | entity records |

Entity records, in array order. Every record is 4-byte aligned.

`PlayerNet` — 8 bytes:

| Offset | Size | Field |
| --- | --- | --- |
| 0 | u8 | `id` |
| 1 | u8 | `state_flags` (1 = ALIVE) |
| 2 | i2 | `pos_x`, 0.05 unit steps |
| 4 | i2 | `pos_y`, 0.05 unit steps |
| 6 | u1 | `vel_angle`, 0..255 → 0..2π |
| 7 | u1 | `hp_pct` |

`EnemyNet` — 12 bytes: `id` u32, `archetype` u8, `elite_mask` u8, `pos_x` i2,
`pos_y` i2, `vel_angle` u1, `hp_pct` u1.

`ProjNet` — 12 bytes: `id` u32, `owner_id` u8, `weapon_id` u8, `shape` u8,
`pos_x` i2, `pos_y` i2, `vel_angle` u1.

`GemNet` — 12 bytes: `id` u32, `pos_x` i2, `pos_y` i2, `xp_value` u2,
`magnet_target_id` u1, `reserved` u1.

Positions are `i16` in 0.05 unit steps, so `[-1000, 1000]` maps to
`[-20000, 20000]` exactly as DESIGN.md §2 specifies. Angles are `u8`, 0..255
mapping 0..2π.

`S2C_SNAPSHOT_DELTA` (0x83), `S2C_LEVELUP_OFFER` (0x84) and `S2C_GAME_OVER`
(0x85) are not implemented in phase 1.
