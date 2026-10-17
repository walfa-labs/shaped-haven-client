# shaped-haven-client

Ogre3D client for **Shaped Haven**, a minimal bullet-heaven (survivors-like)
game.

The client is a thin **view + input** layer. It does not simulate the game: it
sends input to the headless server and renders the state snapshots it receives.
Everything is drawn from geometric primitives (square player, circle/triangle
enemies) — there are no art assets.

Companion repository: `shaped-haven-server` (Zig simulation core).

## Architecture

- **Transport:** TCP socket driven from its own thread to `127.0.0.1:7777`;
  `C2S_HELLO` up, `S2C_WELCOME` / `S2C_SNAPSHOT_FULL` down.
- **Prediction:** local player movement is simulated immediately at a fixed
  60 Hz from input, in a 128-entry ring buffer, then reconciled against the
  server's `acked_input_seq`. Errors at or above 2.0 units snap; smaller errors
  blend out at 15% per reconcile.
- **Interpolation:** remote players are rendered 100 ms behind the newest
  snapshot (two 20 Hz snapshot intervals) and blended between the two
  bracketing snapshots.
- **Rendering:** `OgreBites::ApplicationContext` (the SDL variant) owns the
  window, the render loop and input dispatch; the client subclasses it rather
  than composing it, because Bites only delivers input events to listeners
  registered against a window it created itself. All meshes are generated from
  code at startup.
- **Combat rendering:** enemies, weapon sweeps and projectiles are drawn from
  the same snapshot stream as the players, interpolated by the same buffer.
  Each draw layer is one `Manual Object` holding one section per colour, so a
  thousand enemies cost a handful of draw calls instead of a thousand entities.
  The colour lives in the material, not the vertex: the RTShaderSystem's
  fixed-function emulation only feeds a vertex colour into the lighting terms,
  and an unlit scene has no lights to tint.

The full design — protocol message list, snapshot contents, interpolation rules,
phase plan — is in [DESIGN.md](DESIGN.md).

## Requirements

- **Ogre3D 14.6.1** SDK. On this machine: `D:\src\ogre3d` (override with
  `-DOGRE_SDK_DIR=<path>`).
- **CMake** 3.20 or newer, and a C++17 compiler (MSVC from Visual Studio 18
  Community is what this machine uses).
- At runtime the Ogre DLLs must be reachable, e.g.
  `PATH=D:\src\ogre3d\bin;%PATH%`.

## Build and run

```sh
cmake -S . -B build -A x64
cmake --build build --config Release
PATH=D:\src\ogre3d\bin:%PATH% ./build/bin/Release/shaped-haven-client.exe
```

The client connects to `127.0.0.1:7777` on startup and retries for about 2.5 s,
so the server can still be starting up. Escape closes the window.

From the main menu, Enter opens the **character select**: a name field (seeded
by `--name`, 16 characters, the wire's `C2S_HELLO` field), the ten races with
their base stats, the four starting weapons, and START. Up/Down move between
the four fields, Left/Right cycle the race or the weapon, printable keys edit
the name and backspace deletes, Enter confirms the name or starts the run, and
Escape goes back to the menu. The two ids it produces are the `race_id` and
`starting_weapon_id` the server's `C2S_HELLO` reads, and the table in
`src/character_catalog.h` mirrors the server's `src/races.zig`.

### Command line

| Flag | Meaning |
| --- | --- |
| `--host <ip>` / `--port <n>` | Server address (default `127.0.0.1:7777`). |
| `--name <name>` | Player name sent in `C2S_HELLO`; seeds the character select's name field. |
| `--frames <n>` | Quit after `n` rendered frames of a run. |
| `--screenshot <path>` | Write the framebuffer to `path` on exit (implies `--frames 120`). |
| `--input-script <path>` | Feed move vectors from a file instead of the keyboard. |
| `--choice-script <path>` | Answer level-ups and restarts from a file. |
| `--key-script <path>` | Press keys for the menu and the character select from a file. |
| `--auto-single-player` | Skip the menu and connect single player at once. |
| `--server-path <path>` | The server executable Single Player spawns. |
| `--verbose` | Print one status line per 60 frames. |

`--input-script` exists so the input path can be exercised without a focused
window, which matters for automated runs. Each non-comment line is
`<move_x> <move_y> <ticks>`, one move per 60 Hz tick; see
`tests/data/input_script.txt`.

`--choice-script` drives the upgrade draft without a human. Each non-comment
line is one run event in chronological order: a choice index `0..2` for a
level-up offer, or the literal `restart` at a game over; see
`tests/data/choice_script.txt`. A line is consumed only by the event it names,
so the script has to match the run's event sequence: a `restart` marker waits
for a game over instead of being spent as a pick, and a pick waits for a
level-up instead of being thrown away at the scoreboard. A line that names the
other event stalls the run at that event — reported once on stderr — rather
than being spent on the wrong one. The keyboard always works and wins over the
script: `1`/`2`/`3` pick a card, `R` restarts. With no input at all the
server's 15 s timeout auto-selects option 0.

`--key-script` drives the screens a run has not reached yet, for the same
reason: synthetic keystrokes do not reach the window in a headless session.
Each non-comment line is one press, held for one frame and released on the
next — a key name (`up`, `down`, `left`, `right`, `enter`, `escape`,
`backspace`, `space`) or a single character, which is what the name field is
typed with; see `tests/data/charselect_keys.txt`. When the script runs out on
the main menu, the character select or a failed connection the client stops
there, so `--screenshot` captures the screen the script left up; `--frames`
ends a run as before.

## Tests

```sh
cmake --build build --config Release
./build/Release/shaped-haven-client-tests.exe   # or: ctest --test-dir build
```

The tests cover the packet encode/decode and its payload validation (including
the phase-3 messages and the loadout slot counts), the position and angle
quantisation, the prediction ring buffer and both reconciliation paths, the
interpolation buffer, the keyboard mapping, the choice script's event matching,
the restart boundary, the level-up offer gate and the one-shot restart gate,
the XP bar fill and the evolution rising edge, the item catalog, the bitmap font
and its auto-fit, and the geometry builders (primitive counts, winding, HP bar
layout, and the arc fan's interval coverage). They link no Ogre libraries.

Phase 4 added coverage for the delta protocol: the 21 byte `S2C_WELCOME` (and
the 17 byte phase-3 layout it must still decode), `C2S_ACK_SNAPSHOT`, the
`S2C_SNAPSHOT_DELTA` decoder and its validation (unknown `kind` bytes, inflated
counts, trailing bytes, a non-zero reserved count, and an updated record whose
own id disagrees with the `id` that heads it), the `SnapshotState` baseline
machine (a full snapshot replaces the held state, a delta applies only against
the tick it holds and is refused otherwise, and `build_render` carries the elite
mask), the elite outline and boss telegraph slots and colours, and the
wireframe-rect builder.

The review fixes are covered too: `SnapshotIngest` proves a refused delta
advances none of the run state and does not disarm the restart boundary,
`AckRetry` proves a refused acknowledgement send stays owed until one succeeds
and that a restart leaves nothing owed for the dead run's tick, the 64 bit
entity key is shown to survive the whole 32 bit id space, and
`InterpolationBuffer::push_replacing` is shown to drop the history only when a
full snapshot's run clock went backwards. The last review round is covered as
well: a full snapshot the client did not ask for, at a tick below the one it
holds, is accepted as an unsolicited new run (a lower-tick delta is still
refused), because over TCP a snapshot that arrives after the one that set the
held tick was sent after it, so a tick below the held one cannot be a delayed
snapshot of the run the client is on — the server's counter only goes backwards
on a restart.

The pure parts of the run logic live in their own translation units so they can
be tested without Ogre: `run_events` holds the restart boundary, the level-up
offer gate and the restart gate, `hud_model` holds what the HUD should show —
the XP bar fill and evolution rising edge for a `S2C_LOADOUT`, and the local
player's vitals with the health ghost bar and mana-flash timers — and
`snapshot_state` holds the baseline machine the delta protocol rests on.

The character select is covered the same way: `character_catalog` pins the ten
race rows and their stats and the four weapons, `character_select` holds the
name, race, weapon and highlighted field with their key rules (the field order
and its wraparound, the 16 byte name limit, Enter on START being the only Enter
that connects), and the key script is shown to press each entry for one frame
and release it, so a repeated key is two presses and not one long hold.

## Status

Character select — a name, one of ten races and one of four starting weapons,
chosen before the connection. `C2S_HELLO` carries the two ids, so the run starts
with the race's stat row and the one weapon picked; the client's
`src/character_catalog.h` mirrors the server's `src/races.zig` and is tested
against the same ten rows. The screen is drawn from the same overlay system as
the menu: a field box per field, the chosen race's row and weapon box
highlighted, the chosen race's HP, shield, mana, speed and damage shown, and
the name typed in place with a caret.

Phase 4 — deltas, elites and the boss. The client speaks the phase-4 protocol
from `PROTOCOL.md` (the authoritative byte layout for both repos) while still
decoding the phase-3 `S2C_WELCOME` and `S2C_SNAPSHOT_FULL`, so it runs against
either server:

- **Baseline state machine.** The client holds exactly one complete world
  state — the tick it last acknowledged with `C2S_ACK_SNAPSHOT` (0x04) — in a new
  Ogre-free `snapshot_state` module. A `S2C_SNAPSHOT_FULL` replaces it outright
  and its tick becomes the baseline; a `S2C_SNAPSHOT_DELTA` (0x83) is applied on
  top of it only when its `baseline_tick` is the tick the client holds. A delta
  that names any other tick is **refused, and changes nothing at all** — not the
  HUD clock, not the game-state gating, not prediction: the baseline verdict is
  reached before the header is ingested or any record reconciled, and the client
  re-acknowledges the tick it does hold so the server answers with a full
  snapshot. The run state and that ordering rule live in an Ogre-free
  `snapshot_ingest` module, which is what makes the ordering testable. The 21
  byte `S2C_WELCOME` carries the server's `interest_radius`; the 17 byte
  phase-3 layout still decodes, with the field defaulting to 0.
- **Acknowledgment retry.** The socket's outgoing queue is bounded, so a
  `C2S_ACK_SNAPSHOT` can be refused. `AckRetry` keeps the newest tick owed until
  a send for it actually succeeds, instead of forgetting it — a forgotten
  acknowledgement would leave the server's model of this client stuck on a tick
  it has moved past.
- **Interest management.** The held state is the complete world within the
  interest radius, so it is what goes into the interpolation buffer each
  snapshot. An entity the server destroys is simply absent from the newest
  snapshot, and the draw-item collection — which iterates the newest snapshot's
  positions — then does not draw it. Nothing stale survives a destroy, and the
  HUD holds no per-entity state to clean up.
- **Elite modifiers.** `EnemyNet.elite_mask` is carried through the
  interpolation buffer to the renderer, which draws one concentric outline ring
  per set bit (Vampiric red, Splitter orange, Shielded blue, Haste Aura yellow)
  under the archetype's own body colour — a filled circle at a slightly larger
  radius and a lower z, so the body covers the middle and only the annulus
  shows. Two new colour pools (elite outline, boss telegraph) join the four
  existing ones in the same batched renderer.
- **Boss telegraph.** The boss runs a 6.0 s attack loop; the client derives the
  phase from the snapshot's run clock (boss spawns land on 180 s multiples, which
  are multiples of the loop, so the phase is the same whether the loop is
  anchored to the run start or the boss's spawn). During the 1.0 s window before
  the dash it draws a red wireframe rectangle spanning the 22.0 u dash path
  ahead of the boss along its facing.
- **Run timer.** The HUD run clock (`TIME mm:ss`) is driven by the snapshot's
  `run_time_ms`, so it tracks the run even while deltas keep the entity state
  in sync.
- **Persona-styled HUD.** The status cluster — health, shield and mana — is drawn
  as sheared parallelograms with a black outer border and a white inner one, the
  whole cluster turned -8 degrees about its own centre. The health bar carries a
  white ghost bar behind its fill: a hit holds the ghost at the old value for
  450 ms and then snaps it down, while healing snaps it up at once. The shield
  bar sits above health, is absent for a race with no shield, and carries a
  hexagonal chevron divider every 25 points of its 100-point scale. The mana bar
  sits beneath health as a cobalt-to-violet gradient, drawn as 24 flat slices
  because the HUD's materials carry one colour each, and flashes white for
  200 ms when a dash is attempted without the mana to pay for it. The same frame
  and the crimson accent (`#E60012`) are carried by the XP bar, the item tray
  slots, the level-up cards and the modal panels. Every layer that has to sit
  over another is given its own depth offset, because two coplanar quads have no
  defined order between them.

Measured on this machine (Release build, 1280x720):

- The logic tests report **1304 checks, 0 failures** (Release and Debug). 158 of
  those are the character select's: the ten race rows against the server's
  table, the four weapons, the field and value rules, and the key script.
- The character select, driven by `--key-script` and captured with
  `--screenshot`, shows the ten races, the chosen race's stats and the four
  weapons, with the highlighted row and weapon box where the model put them
  (checked by reading the framebuffer's colours at the layout's coordinates);
  START then connects with `C2S_HELLO name=havenz race=3 weapon=2` — the name
  typed on the screen, Aerie Seraph and the Bow — and the server accepts it and
  runs. Escape from the screen returns to the menu instead of closing the
  window. The scripts are `tests/data/charselect_keys.txt` (stop on the screen
  with a race and a weapon chosen), `charselect_default_keys.txt` (the same,
  before either is changed) and `menu_roundtrip_keys.txt` (into the screen and
  back out).
- Against a mock delta server (21 byte welcome, full baseline, then deltas):
  `interest_radius=75`, **27 deltas applied, 0 rejected**, 28 `C2S_ACK_SNAPSHOT`
  sent, entities spawning and leaving the radius, `stale=0 malformed=0
  disconnected=0`, ~119 fps.
- Against the real `shaped-haven-server` `phase-4-delta` branch (commit
  `f5edbcb`) under `--stress-enemies 40`: the 21 byte welcome decodes
  (`interest_radius=40`), and deltas flow. In a 2400 frame run the client applied
  **333 deltas and refused none**; in another it applied **735 of 737** (99.7%),
  the two refusals being the baseline-one-broadcast-stale case the protocol
  expects — the client re-acknowledged the tick it held and carried on. Every
  run reported `stale=0 malformed=0 ackretry=0 sendfail=0 qdropped=0`, and the
  frame rate was **~142 fps** (1800 frames in 12.7 s). Level-ups, the level-up
  pause (`game_state=1`) and game over (`game_state=2`) are all observed; a game
  over answered with a scripted restart put the client back into a fresh run
  (`restart requested`, new run at `srv_tick=6`, `restarts=1`, `stale=0`
  throughout), so the restart boundary still disarms on the new run's first
  snapshot. The run is playable start to finish.
- Against the current `phase-4-delta` server (which carries the corrected
  first-disagreement acknowledgement predicate), a scripted run of 7000 frames
  (48.8 s) at **143.3 fps** survived to a game over at 159 s, answered it with a
  scripted restart and came back into a fresh run: `restarts=1`, the server's
  tick reset to a low value and the client accepted that recovery full snapshot
  rather than refusing it as stale, with `stale=0` throughout. Level-ups, the
  level-up pause and game over are all observed. Across two runs the client
  refused **2 and 5** deltas out of 662 and 653 (under 1%), every refusal the
  baseline-one-broadcast-stale case the protocol expects, and reported
  `stale=0 malformed=0 ackretry=0 sendfail=0 qdropped=0` in both — it never
  wedged. The restart also cleared the old run's still-pending acknowledgement
  (a refused send), which the unit test proves cannot then be emitted.
- The retry path and the recovery-full-snapshot path did not come up in these
  runs — on loopback the outgoing queue never fills (`qdropped=0 ackretry=0`)
  and the server's baseline model held, so no recovery full snapshot was needed.
  Both are covered by unit tests instead (see above).
- Against the phase-3 server (`main`, 17 byte welcome, full snapshots only):
  `interest_radius=0`, full snapshots flow, level-ups and restart work,
  `stale=0 malformed=0`, ~134 fps.

An evolution was not reached in these runs: the scripted picks are the ones the
offer happened to list first, and reaching a level-8 weapon needs both a longer
run and the matching pick.
