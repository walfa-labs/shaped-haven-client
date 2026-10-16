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
the restart boundary and the level-up offer gate, the XP bar fill and the
evolution rising edge, the item catalog, the bitmap font and its auto-fit, and
the geometry builders (primitive counts, winding, HP bar layout, and the arc
fan's interval coverage). They link no Ogre libraries.

The pure parts of the run logic live in their own translation units so they can
be tested without Ogre: `run_events` holds the restart boundary and the level-up
gate, and `hud_model` holds what the HUD should show for a `S2C_LOADOUT`.

## Status

Phase 3 — level-up and HUD. The client now speaks the phase-3 protocol from
`PROTOCOL.md` (the authoritative byte layout for both repos):

- **XP gems.** The `GemNet` array the snapshot already carries is drawn as
  diamonds through the same batched primitive renderer and the same 100 ms
  interpolation buffer as players, enemies and projectiles — one more
  `EntityKind`, one more colour pool, no second render path. Size and colour
  follow the DESIGN.md §1 gem tiers: small cyan, medium green, large violet,
  boss cache gold.
- **Level-up draft.** `S2C_LEVELUP_OFFER` (0x84) raises a three-card modal over
  a dimmed, input-frozen game; the pick goes out as `C2S_LEVELUP_CHOICE` (0x03)
  with the offer's `event_id`, from the `1`/`2`/`3` keys or `--choice-script`.
  An evolution card names its two components; the server's 15 s timeout
  auto-selects option 0 when no input arrives.
- **HUD.** `S2C_LOADOUT` (0x86) drives an XP bar with the level label and run
  clock, and an item tray: up to four weapon boxes (level pips, or an `EVOLVED`
  tag) and up to ten passive boxes. Text is a baked 5x7 bitmap font drawn as
  quads through the same immediate-mode sink — the Ogre SDK ships no font and
  the design allows no art assets.
- **Evolution banner.** A weapon slot whose `evolved` byte just became 1 raises
  an `EVOLUTION!` banner naming the new weapon for 2.5 s. The rising-edge memory
  is cleared with the run, so an evolution in a new run is announced even when
  the slot index already held one when the previous run ended.
- **Game over and restart.** `S2C_GAME_OVER` (0x85) freezes input and shows a
  scoreboard (time survived, score, kills); `R` or a `restart` line in the
  choice script sends `C2S_RESTART` (0x05), and the next snapshot starts a fresh
  run. The dead run's snapshots are still in flight when the request goes out,
  so the boundary is the run clock rather than a count of snapshots: a new run's
  `run_time_ms` restarts at zero while the dead run's snapshots carry its final
  value, and everything at or above that value is dropped. The server restarts
  its tick counter with the run, so the interpolation buffer, the HUD, the
  level-up offer gate, the evolution memory and both run-transition tick guards
  are reset with it — without that the scoreboard stayed up and input stayed
  frozen for the whole of the new run.

A duplicate or older `S2C_LEVELUP_OFFER` — the same `event_id` arriving again
after the client has answered it — is ignored rather than reopening the modal,
so a resolved event cannot be answered twice.

Measured on this machine (Release build, 1280x720) against the real
`shaped-haven-server` `phase-3-upgrades` branch:

- The logic tests report **855 checks, 0 failures** (Release and Debug).
- A 6,000-frame run renders at **143.5 fps** with 0.3 ms of client-side update
  work per frame, `stale=0`, `staleoffers=0`, `malformed=0` and `qdropped=0`
  throughout, while the server offers level-ups and the script answers them
  (`choices` counts them; the item tray and XP bar track `S2C_LOADOUT`).
- The restart path was driven to a real game over against a server whose run had
  been live for ~160 s: `S2C_GAME_OVER` at 178 s, `C2S_RESTART`, and the next
  snapshot arrived with the new run's clock (`srv_tick` 10,713 → 3) and was
  accepted — the client was back in `Running`, sending input and predicting
  again, on the following frame, with `stale=0`. The pre-fix client, run against
  the same server with the same scripts, stayed on the scoreboard with its input
  frozen from frame 1,980 to frame 8,340 while the new run's clock advanced from
  24 to 1,749, and sent two `C2S_RESTART`s in consecutive frames as it spent the
  choice script one line per frame.

An evolution was not reached in these runs: the scripted picks are the ones the
offer happened to list first, and reaching a level-8 weapon needs both a longer
run and the matching pick.
