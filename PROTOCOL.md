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

`S2C_SNAPSHOT_DELTA` (0x83) is not implemented yet — phase 4. Everything below
is the phase 3 addition.

## `game_state` values

Carried in the snapshot header. `0` running, `1` level-up pause, `2` game over.

## Item ids

One id space covers weapons, passives, evolutions and the endless Overclock
upgrades, so an upgrade option, a loadout entry and a weapon id all name the
same thing the same way.

| Range | Meaning |
| --- | --- |
| 0–3 | weapons, matching `ProjNet.weapon_id`: 0 sword, 1 axe, 2 bow, 3 gun |
| 16–25 | passives P0–P9, in the DESIGN.md §3 table order |
| 32–39 | evolutions, in DESIGN.md §3 order: 32 Vorpal Blender, 33 Phase Katana, 34 Gravity Cleaver, 35 Juggernaut Halberd, 36 Railgun Longbow, 37 Storm Volley, 38 Vulcan Minigun, 39 Cryo Flechette |
| 48–50 | Overclock, the uncapped endgame: 48 global damage, 49 attack speed, 50 max HP |

`0xFF` means "no item" — an empty weapon or passive slot.

## `C2S_LEVELUP_CHOICE` (0x03) — 3 byte payload

| Offset | Size | Field |
| --- | --- | --- |
| 0 | u2 | `event_id` |
| 2 | u1 | `choice_idx`, 0..2 |

## `C2S_RESTART` (0x05) — empty payload

Asks the server for a fresh run on this connection. Only meaningful while
`game_state` is `game_over`; ignored otherwise.

## `S2C_LEVELUP_OFFER` (0x84) — 14 byte payload

| Offset | Size | Field |
| --- | --- | --- |
| 0 | u1 | `player_id` |
| 1 | u2 | `event_id` |
| 3 | u2 | `new_level` |
| 5 | 3 × 3 | `options`: `item_id` u8, `next_level` u8, `is_evo` u8 |

The run pauses (`game_state = 1`) and each living player gets one offer. The
client answers with `C2S_LEVELUP_CHOICE` carrying the same `event_id`; a 15 s
server timeout auto-selects option 0.

## `S2C_GAME_OVER` (0x85) — 16 byte payload

| Offset | Size | Field |
| --- | --- | --- |
| 0 | u4 | `final_tick` |
| 4 | u4 | `seconds_survived` |
| 8 | u4 | `score` |
| 12 | u4 | `kills` |

## `S2C_LOADOUT` (0x86) — 45 byte payload

Sent whenever the player's level, XP or item set changes, so the HUD can draw
an XP bar and an item tray without polling. Positions stay in the snapshot;
this message is only about progression.

| Offset | Size | Field |
| --- | --- | --- |
| 0 | u1 | `player_id` |
| 1 | u2 | `level` |
| 3 | u4 | `xp` — XP collected into the current level |
| 7 | u4 | `xp_next` — XP needed to reach the next level |
| 11 | u1 | `weapon_count`, 0..4 |
| 12 | 4 × 3 | `weapons`: `item_id` u8, `level` u8, `evolved` u8 |
| 24 | u1 | `passive_count`, 0..10 |
| 25 | 10 × 2 | `passives`: `item_id` u8, `level` u8 |

Slots beyond the count are filled with `0xFF` rather than being omitted, so the
payload is always 45 bytes.
