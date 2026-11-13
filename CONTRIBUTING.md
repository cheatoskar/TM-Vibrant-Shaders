# Contributing

Bug reports, ideas and pull requests are welcome.

## Reporting a bug

Open an [issue](../../issues) and include:

- the game and its version (and which exe, if you know it)
- the version of TM Vibrant Shaders, and your graphics card and driver for rendering problems
- what you did, what you expected and what happened
- `Documents\TrackMania\TMVS\tmvs.log` from that session (it is rewritten at every game start, so copy it right after the problem). [docs/troubleshooting.md](docs/troubleshooting.md) lists common problems and what to attach.

A screenshot or a short clip helps. Search the existing issues first.

## Feature ideas

Open an issue and describe the problem you want solved. Check the rules below first: some things are out of scope on purpose.

## Pull requests

Pull requests are welcome. Keep them small and focused: one fix or one feature per pull request. For a larger change, open an issue first so the direction is clear before you spend time on it.

In the description, say what changed and how you tested it in the game (game, version, what you did). For a visual change, a before and after picture helps (the offline previewer `tmvs_preview` can render them from an `F12` capture).

## Build and test

Visual Studio 2022 (C++ workload), Windows SDK and CMake 3.21 or newer; the game is 32-bit, so configure for Win32. Details are in [docs/building.md](docs/building.md):

```
cmake -B build -A Win32
cmake --build build --config Release
cmake --build build --config Release --target release_zip
```

The Horizon module tests (`tmvs_moduletest`, `tmvs_modulelogictest`) are described in [docs/horizon-modules.md](docs/horizon-modules.md). Every push to `main` builds on GitHub Actions.

## Code style

The conventions are in [docs/building.md](docs/building.md#conventions): every visual feature is a setting with a field in `fields()` and is off or cheap by default, passes that contribute nothing are skipped, and new engine hooks need an address for both game builds in `kBuilds` (`src/engine.cpp`).

## Rules for in-game mods

These mods run inside the game, also in online play, so a few things are out of scope. A pull request that does any of this will not be merged:

- features that give a competitive advantage online or on leaderboards
- anything that touches physics, inputs or timing of the simulation, or edits runs and replays
- checkpoint or trigger display, ideal or brake line, disabling the camera or MediaTracker behaviour
- anything that gets around the Competition Patch or other anti-cheat and replay validation

The README and [docs/horizon-modules.md](docs/horizon-modules.md#online-and-tmx-rules) state the same rules for the standalone mod and the Horizon modules: no input is sent to the game, the race is only read, the minimap only in the replay editor and the video export. The mod also has to keep working with the Competition Patch (README, compatibility table). The rules come from what the TrackMania Exchange moderators accept: [TMX rules for mods](https://github.com/cheatoskar/trackmania-internals/blob/main/modding/tmx-rules-for-mods.md).

## AI-assisted contributions

Contributions written with AI tools are fine, as long as the contributor has tested them and can explain what the change does. Untested generated code will be sent back.
