# shaped-haven-client

Ogre3D client for **Shaped Haven**, a minimal bullet-heaven (survivors-like)
game.

The client is a thin **view + input** layer. It does not simulate the game: it
sends input to the headless server and renders the state snapshots it receives.
Everything is drawn from geometric primitives (square player, circle/triangle
enemies) — there are no art assets.

Companion repository: `shaped-haven-server` (Zig simulation core).

## Architecture

- **Transport:** async TCP socket thread to `127.0.0.1:7777`; input packets up,
  snapshots down.
- **Prediction:** local player movement is simulated immediately at 60 Hz from
  input, then reconciled against the server's `acked_input_seq`.
- **Interpolation:** remote entities are rendered 100 ms behind the latest
  snapshot (2 snapshot intervals) and blended between bracketing snapshots.
- **Rendering:** procedural meshes built at startup for the primitive shapes.

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
./build/bin/Release/shaped-haven-client.exe
```

## Status

Phase 0 — repository skeleton and design document. The window, camera and
network thread land in phase 1. See `DESIGN.md` §6 for the phase plan.
