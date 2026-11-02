# game_manager (Plugify)

Port of the SwiftlyS2 plugin GameManager (criskkky): blocks commands (radio, chat wheel, ping, BlockedCommands) and HUD messages, controls the kill feed (hide / killer only), corpses (hide / fade over spec_freeze_deathanim_time), radar and teammate headtags, mutes sounds and runs the original's server-convar toggles. Config: configs/game_manager/config.jsonc (the production layout, every option under the root GameManager object); gm_reload re-reads it. No exported methods.

Plugify port (fork) of **GameManager** by criskkky.
The code was rewritten for Plugify / s2sdk.

## Requirements
- [Plugify](https://github.com/untrustedmodders/plugify) with [s2sdk](https://github.com/untrustedmodders/plugify-plugin-s2sdk)
- Plugin version: 1.1.0
- Plugify dependencies: s2sdk (>=2.18.2)
- Self-contained s2sdk integration: uses s2sdk directly, without the pisexcompat library.

## Installation
Download `game_manager-1.1.0.zip` from Releases and extract it into `game/csgo/`.

## Building
Requires Docker. `./build.sh` builds in the Steam Runtime 3 (sniper) SDK image; the package
is written to `build/package/game_manager`.

## License
GPL-3.0, see LICENSE.

