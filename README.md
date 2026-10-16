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
| `--verbose` | Print one status line per 60 frames. |

`--input-script` exists so the input path can be exercised without a focused
window, which matters for automated runs. Each non-comment line is
`<move_x> <move_y> <ticks>`, one move per 60 Hz tick; see
`tests/data/input_script.txt`.

## Tests

```sh
cmake --build build --config Release
./build/Release/shaped-haven-client-tests.exe   # or: ctest --test-dir build
```

The tests cover the packet encode/decode and its payload validation, the
position and angle quantisation, the prediction ring buffer and both
reconciliation paths, the interpolation buffer, the keyboard mapping, and the
geometry builders (primitive counts, winding, HP bar layout, and the arc fan's
interval coverage). They link no Ogre libraries.

## Status

Phase 2 — combat rendering. The window, orthographic camera, procedural mesh
builders, async TCP thread, 60 Hz prediction with reconciliation and the 100 ms
interpolation buffer are in place, and on top of them the client now parses and
draws the `EnemyNet` and `ProjNet` arrays the snapshot already carries:

- Enemies are circles or triangles by archetype, each with an HP bar whose
  width tracks `hp_pct` and whose colour drains from green to red.
- Weapon sweeps are drawn as arc wedges, the bow's arrow as a ray and the gun's
  bullet as a travelling circle, at the level-1 reach and radius from
  DESIGN.md §3. `ProjNet`'s `shape` byte picks the primitive, `weapon_id` the
  size and colour; a shape byte the client does not know falls back to the
  bullet circle rather than drawing nothing.
- Everything is interpolated through the same 100 ms buffer the remote
  players use, so there is no second interpolation path.
- A snapshot whose counts do not match the bytes that arrived is rejected
  before any vector is resized, and the frame header's `payload_len` is checked
  against the bytes actually delivered.

Measured on this machine (Release build, 1280x720), against the real
`shaped-haven-server` phase-2 branch built from `phase-2-combat` and started
with `--stress-enemies 1000 --stress-projectiles 500`:

- The logic tests report **544 checks, 0 failures**.
- A 600-frame run renders at **136.0 fps** (4.41 s), with 3.5–3.8 ms of
  client-side work per frame, `malformed=0`, `stale=0` and `qdropped=0`
  throughout.
- The client draws exactly what the server reports: **1,000 enemies and 500
  projectiles** per frame, matching the stress flag. A raw probe of the same
  server confirms every snapshot on the wire carries exactly that. An entity
  the interpolation window does not bracket yet is held at its first-seen
  position, so nothing is dropped and the handover to interpolation is
  continuous.

The arc wedge's triangulation is pinned by a test that walks the index buffer
and requires every interval of the ring to be covered exactly once; the
rendered result was also checked in pixels: a probe that feeds the client one
static axe projectile renders a 160° wedge of 5.0 u radius whose covered area
is 8764 px against the analytic fan area of 8756 px, with no gap in the final
interval.
