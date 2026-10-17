# Protocol v2 — byte layout

Authoritative layout for the wire protocol. DESIGN.md §5 gives the message
list and §2 the snapshot header; where the two disagree this file wins.

Both repos must agree on it. If you change anything here, change it in both.

Version 2 grows the handshake with the player's race and starting weapon,
carries dash, mana and shield state on the player record, telegraph state on
the enemy record, and per-option stat deltas on the level-up offer. The frame
header, the snapshot header, the projectile and gem records, and every other
message are unchanged from v1; `C2S_HELLO` and `C2S_INPUT` change size, and
`PlayerNet`, `EnemyNet` and `S2C_LEVELUP_OFFER` grow.

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

## `C2S_HELLO` (0x01) — 20 byte payload

| Offset | Size | Field |
| --- | --- | --- |
| 0 | u8 | `proto_ver` = 2 |
| 1 | u8 | `race_id` (0..9) |
| 2 | u8 | `starting_weapon_id` (0 = sword, 1 = axe, 2 = bow, 3 = gun) |
| 3 | u8 | `reserved` = 0 |
| 4 | 16 | `name`, zero padded |

The two ids are what the character select produces. The server rejects a
`race_id` it does not know rather than defaulting it to the first row.

## `C2S_INPUT` (0x02) — 11 byte payload

| Offset | Size | Field |
| --- | --- | --- |
| 0 | u4 | `input_seq` (u32) |
| 4 | u4 | `client_tick` (u32) |
| 8 | u1 | `buttons` — bitmask |
| 9 | i1 | `axis_x`, −127..127 |
| 10 | i1 | `axis_y`, −127..127 |

`buttons`: 0x01 up, 0x02 down, 0x04 left, 0x08 right, 0x10 dash. The two axes
are analog, from a gamepad stick; they are 0 when the digital buttons are used
instead.

## `C2S_ACK_SNAPSHOT` (0x04) — 4 byte payload

| Offset | Size | Field |
| --- | --- | --- |
| 0 | u4 | `last_recv_server_tick` |

Sent at most once per snapshot. It is what lets the server pick a baseline:
the client is telling it which tick it holds a complete state for.

## `S2C_WELCOME` (0x81) — 21 byte payload

| Offset | Size | Field |
| --- | --- | --- |
| 0 | u1 | `player_id` |
| 1 | u8 | `session_token` (echoed) |
| 9 | u1 | `tick_hz` = 60 |
| 10 | u1 | `snap_hz` = 20 |
| 11 | u2 | `arena_half_size` = 1000 |
| 13 | u2 | `interest_radius`, world units |
| 15 | u2 | `reserved` = 0 |
| 17 | u4 | `server_tick` |

`interest_radius` is the phase-4 addition: the client needs to know how far its
interest neighbourhood extends. A server that still speaks the 17 byte phase-3
layout omits the two u16s and puts `server_tick` at offset 13; the client
decodes both.

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
| 0 | u4 | `server_tick` |
| 4 | u4 | `baseline_tick` (0 for a full snapshot) |
| 8 | u4 | `run_time_ms` — also the timestamp the client interpolates against |
| 12 | 4 × u4 | `acked_seq[4]`, one per player slot |
| 28 | u1 | `game_state` |
| 29 | 4 × u2 | `counts` = players, enemies, projectiles, gems |
| 37 | … | entity records |

Entity records, in array order. Every record is 4-byte aligned.

`PlayerNet` — 20 bytes:

| Offset | Size | Field |
| --- | --- | --- |
| 0 | u1 | `player_id` |
| 1 | u1 | `state_flags` (1 = ALIVE, 2 = INVULNERABLE, 4 = AWAITING_LEVELUP) |
| 2 | u1 | `race_id` |
| 3 | i2 | `pos_x`, 0.05 unit steps |
| 5 | i2 | `pos_y`, 0.05 unit steps |
| 7 | u1 | `vel_angle`, 0..255 → 0..2π |
| 8 | u1 | `hp_pct`, `hp / max_hp × 100` |
| 9 | u1 | `shield_pct`, `shield / max_shield × 100` (0 when `max_shield` = 0) |
| 10 | u1 | `mana_pct`, `mana / max_mana × 100` |
| 11 | u2 | `dash_cd_remaining`, ms left on the dash cooldown |
| 13 | u2 | `dash_cd_total`, ms, the race's full dash cooldown |
| 15 | u1 | `is_dashing` (1 during dash i-frames) |
| 16 | u2 | `max_hp` |
| 18 | u2 | `max_shield` |

`EnemyNet` — 18 bytes:

| Offset | Size | Field |
| --- | --- | --- |
| 0 | u4 | `id` |
| 4 | u1 | `archetype` |
| 5 | u1 | `elite_mask` |
| 6 | i2 | `pos_x`, 0.05 unit steps |
| 8 | i2 | `pos_y`, 0.05 unit steps |
| 10 | u1 | `vel_angle` |
| 11 | u1 | `hp_pct` |
| 12 | u1 | `telegraph_progress` (0 = no windup, 1..255 = windup progress) |
| 13 | u1 | `telegraph_shape` (0 = None, 1 = CircleAoE, 2 = ConeSlash, 3 = LineCharge) |
| 14 | i2 | `telegraph_angle`, milliradians, −3142..3142 |
| 16 | u2 | `telegraph_radius`, circle/cone radius or line length |

`ProjNet` — 12 bytes: `id` u32, `owner_id` u8, `weapon_id` u8, `shape` u8,
`pos_x` i2, `pos_y` i2, `vel_angle` u1.

`GemNet` — 12 bytes: `id` u32, `pos_x` i2, `pos_y` i2, `xp_value` u2,
`magnet_target_id` u1, `reserved` u1.

Positions are `i16` in 0.05 unit steps, so `[-1000, 1000]` maps to
`[-20000, 20000]` exactly as DESIGN.md §2 specifies. Angles are `u8`, 0..255
mapping 0..2π.

## `game_state` values

Carried in the snapshot header. `0` running, `1` level-up pause, `2` game over.

## Item ids

One id space covers weapons, passives, evolutions and the endless Overclock
upgrades, so an upgrade option, a loadout entry and a projectile's weapon id
all name the same thing the same way.

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

## `S2C_LEVELUP_OFFER` (0x84) — 29 byte payload

| Offset | Size | Field |
| --- | --- | --- |
| 0 | u1 | `player_id` |
| 1 | u2 | `event_id` |
| 3 | u2 | `new_level` |
| 5 | 3 × 8 | `options`: `item_id` u8, `next_level` u8, `is_evo` u8, `stat_type` u8, `current_stat_fp` i2, `upgraded_stat_fp` i2 |

`stat_type` names the stat the two fixed-point fields describe: 0 damage,
1 armor, 2 max HP, 3 move speed, 4 cooldown, 5 area, 6 projectiles,
7 crit chance, 8 max mana, 9 max shield. Both fields are scaled by 100, so
`500` reads as `5.0`; a negative delta is a reduction. The HUD draws the pair
as "current → upgraded (+change)".

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

## `S2C_SNAPSHOT_DELTA` (0x83) — phase 4

A delta carries the same 37-byte header as a full snapshot, with the counts
meaning something different, followed by three variable-length arrays.

| Offset | Size | Field |
| --- | --- | --- |
| 0 | u4 | `server_tick` |
| 4 | u4 | `baseline_tick` — the tick this delta is relative to |
| 8 | u4 | `run_time_ms` |
| 12 | 4 × u4 | `acked_seq[4]` |
| 28 | u1 | `game_state` |
| 29 | 4 × u2 | `counts` = **spawned, updated, destroyed, reserved (0)** |
| 37 | … | the three arrays, in that order |

`kind` bytes name the record type that follows:

| Value | Meaning | Record |
| --- | --- | --- |
| 0 | player | `PlayerNet`, 20 bytes |
| 1 | enemy | `EnemyNet`, 18 bytes |
| 2 | projectile | `ProjNet`, 12 bytes |
| 3 | gem | `GemNet`, 12 bytes |

**Spawned** — `counts[0]` records, each `kind` u8 followed by that kind's full
record. An entity the client has never seen.

**Updated** — `counts[1]` records, each `kind` u8, `id` u32, then that kind's
full record. The full record rather than a dirty mask: every moving entity
changes position every tick, so a mask would cost a byte to save nothing.

**Destroyed** — `counts[2]` records, each `kind` u8, `id` u32.

### Baseline rules

A client holds exactly one complete state: the one it last acknowledged with
`C2S_ACK_SNAPSHOT`. Every delta must be applied on top of that state, and must
carry `baseline_tick` equal to it.

- The server sends `S2C_SNAPSHOT_FULL` when it has no usable model of what the
  client holds, or when that tick has fallen out of its 32-snapshot history
  ring (1.6 s).
- Otherwise it sends a delta against **its model of the client's held tick**,
  not against the raw last acknowledgement. The model advances to a snapshot's
  own tick when that snapshot is sent, because a client that applies a delta
  moves its held state onto it; an acknowledgement corrects the model when it
  disagrees. This exists because an acknowledgement is causally triggered by
  the very broadcast it would have to beat — a client's socket thread flushes
  on its own `select` timeout, which coincides with the snapshot period — so a
  strictly acknowledgement-driven baseline is always one broadcast stale and
  the client refuses roughly half the deltas.
- A client that receives a delta whose `baseline_tick` is not the tick it holds
  **must not apply it, and must not let it advance any run state** — not the
  HUD clock, not the game-state gating, not prediction. It re-acknowledges the
  tick it does hold. A repeated acknowledgement that disagrees with the
  server's model corrects the model, which is what stops a drifted model from
  refusing forever.
- `S2C_SNAPSHOT_FULL` carries `baseline_tick = 0`, which is never a valid delta
  baseline, so the two are never ambiguous. A full snapshot replaces the held
  state outright.
- `C2S_RESTART` forgets the model on both sides: the new run's tick returns to
  zero, and a coinciding tick would otherwise diff the new run against the old
  one.

### Interest management

The snapshot — full or delta — carries only entities within `interest_radius`
of the receiving player, not the whole arena. An entity that leaves that radius
appears in `destroyed`; one that enters it appears in `spawned`. The client
therefore renders a complete world in its own neighbourhood and nothing
outside it.

`interest_radius` is a server constant, sent to the client in `S2C_WELCOME`
as a `u16` in world units. See the `S2C_WELCOME` section above.
