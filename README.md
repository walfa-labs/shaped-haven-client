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

### Command line

| Flag | Meaning |
| --- | --- |
| `--host <ip>` / `--port <n>` | Server address (default `127.0.0.1:7777`). |
| `--name <name>` | Player name sent in `C2S_HELLO`. |
| `--frames <n>` | Quit after `n` rendered frames. |
| `--screenshot <path>` | Write the framebuffer to `path` on exit (implies `--frames 120`). |
| `--input-script <path>` | Feed move vectors from a file instead of the keyboard. |
| `--choice-script <path>` | Answer level-ups and restarts from a file. |
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
counts, trailing bytes), the `SnapshotState` baseline machine (a full snapshot
replaces the held state, a delta applies only against the tick it holds and is
refused otherwise, and `build_render` carries the elite mask), the elite
outline and boss telegraph slots and colours, and the wireframe-rect builder.

The pure parts of the run logic live in their own translation units so they can
be tested without Ogre: `run_events` holds the restart boundary, the level-up
offer gate and the restart gate, `hud_model` holds what the HUD should show
for a `S2C_LOADOUT`, and `snapshot_state` holds the baseline machine the delta
protocol rests on.

## Status

Phase 4 — deltas, elites and the boss. The client speaks the phase-4 protocol
from `PROTOCOL.md` (the authoritative byte layout for both repos) while still
decoding the phase-3 `S2C_WELCOME` and `S2C_SNAPSHOT_FULL`, so it runs against
either server:

- **Baseline state machine.** The client holds exactly one complete world
  state — the tick it last acknowledged with `C2S_ACK_SNAPSHOT` (0x04) — in a new
  Ogre-free `snapshot_state` module. A `S2C_SNAPSHOT_FULL` replaces it outright
  and its tick becomes the baseline; a `S2C_SNAPSHOT_DELTA` (0x83) is applied on
  top of it only when its `baseline_tick` is the tick the client holds. A delta
  that names any other tick is **not** applied: the client keeps acknowledging
  the state it does hold and the server answers with a full snapshot. The 21
  byte `S2C_WELCOME` carries the server's `interest_radius`; the 17 byte
  phase-3 layout still decodes, with the field defaulting to 0.
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

Measured on this machine (Release build, 1280x720):

- The logic tests report **999 checks, 0 failures** (Release and Debug).
- Against a mock delta server (21 byte welcome, full baseline, then deltas):
  `interest_radius=75`, **27 deltas applied, 0 rejected**, 28 `C2S_ACK_SNAPSHOT`
  sent, entities spawning and leaving the radius, `stale=0 malformed=0
  disconnected=0`, ~119 fps.
- Against the real `shaped-haven-server` `phase-4-delta` branch under
  `--stress-enemies 40`: the 21 byte welcome decodes (`interest_radius=50`),
  deltas are applied and acknowledged, level-ups and game over + restart all
  work, `stale=0 malformed=0`. The server sends some deltas against a baseline
  one broadcast stale (its reader thread processes the ACK a 50 ms broadcast
  late), so the client rejects roughly half and re-acknowledges — the behaviour
  the spec requires, and the server answers with a full snapshot once the
  baseline leaves its 32-snapshot ring. The run is playable start to finish.
- Against the phase-3 server (`main`, 17 byte welcome, full snapshots only):
  `interest_radius=0`, full snapshots flow, level-ups and restart work,
  `stale=0 malformed=0`, ~134 fps.

An evolution was not reached in these runs: the scripted picks are the ones the
offer happened to list first, and reaching a level-8 weapon needs both a longer
run and the matching pick.
