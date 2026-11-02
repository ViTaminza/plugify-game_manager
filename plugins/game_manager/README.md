# game_manager

Plugify C++ port of the SwiftlyS2 C# plugin **GameManager** by criskkky
(`ref/src/gamemanager-upstream/src/` — `Config.cs` holds the behaviour, `Helper.cs` the command and
message lists). It blocks commands and HUD messages, controls the kill feed, corpses, radar and
teammate headtags, mutes sounds and runs the original's "native" server-convar commands.

- Config: `configs/game_manager/config.jsonc` — the production layout (every option under the root
  `GameManager` object, the same key names/defaults as `ConfigModel` in Config.cs); a flat object is
  accepted as well. `gm_reload` re-reads it.
- Commands: `gm_reload` (server console / RCON).
- Pure s2sdk (`docs/PLUGIN_STANDARD.md` §7): no `pisexcompat`, no `hudmenu` and no shared guard
  library. The manifest depends only on `s2sdk`.
- The map gate (`PITFALLS` B1/B2) is closed in `OnPluginStart`, opened by the first
  `OnPluginUpdate` after `OnMapStart` (or the first update of a plugin loaded into a running map)
  and closed immediately in `OnMapEnd`; no client/entity/schema call is made in the map-change
  window.
- Everything registered in `OnPluginStart` is removed in `OnPluginEnd` with the exact same key
  (events with their exact mode, user-message hooks, the global command listener, `gm_reload` only
  via `RemoveCommand` when this plugin created it, otherwise via `RemoveCommandListener`, the
  entity-created listener, the map listeners, the corpse fade timers) and the deferred next-frame
  queue is dropped, so `plg reload` is safe.
- User-message lookup uses the short registry name first, then the protobuf type name, then a numeric
  enum fallback. IDs are documented in `ref/src/cs2-lvl_ranks_modules/LR_FakeRanks/protobuf/`:
  `te.proto:4` (`EffectDispatch=400`) and `te.proto:14` (`WorldDecal=411`);
  `usermessages.proto:22` (`TextMsg=124`); and `cstrike15_usermessages.proto:25-26`
  (`RadioText=322`, `HintText=323`). `TextMsg` deliberately uses `UM_TextMsg=124`, not the unrelated
  `CS_UM_TextMsg=307` at `cstrike15_usermessages.proto:11`. Successful registrations log the selected
  name, ID, and source; ID 0 is never hooked because s2sdk reserves it for the global user-message hook.

## How the s2sdk port differs from the C# original

| Area | Original (Config.cs) | This port |
|------|----------------------|-----------|
| Hiding events | `@event.DontBroadcast = true; return HookResult.Handled/Stop` (kills the event for every other plugin in s2sdk terms) | `SetEventBroadcast(event, true)` + `return Continue`: s2sdk answers `Changed` and flips the engine's `dontBroadcast` argument, so the event still reaches every local listener (LR, quake_sounds, VIP, cs2ac, admin, round_end_sounds ...) and only the clients do not see it |
| Radio / ping / chat wheel blocking | `HookClientCommand` (works per name in SwiftlyS2) | one global `AddCommandListener("", …)` filtered by `args[0]` — per-name listeners only work for real ConCommands and the radio commands are not ones |
| `SoundsMuteMVPMusic = 1` | `playerController.MusicKitID = 0; MusicKitIDUpdated()` on the MVP (skipping bots) | the same write through s2sdk's typed schema setter on the controller (`m_iMusicKitID`, not on the CS2BadList) with a state change; the MVP must be a live, non-bot client, exactly like the original |
| `SoundsMuteMVPMusic = 2` | a `StopSoundEvents.StopAllMusic` sound event with `SourceEntityIndex = -1`, `AddAllPlayers` | `EmitSoundToClient(0, slots, …)` (s2sdk needs a real source entity; the world entity is used) to every connected player |
| `HideKillFeed = 2` delivery | a fresh copy of `player_death` with eight fields (`userid`, `attacker`, `weapon`, `headshot`, `assister`, `penetrated`, `dominated`, `revenge`) fired to the killer | `FireEventToClient` hands the **live** event to that one client while `SetEventBroadcast(true)` keeps it server-side for everyone else — same visible result, the client sees the full event instead of the eight-field copy |
| `IgnoreTeammateAttackMessages` hint | `HookClientMessage<CCSUsrMsg_HintText>` — client→server, so that hook can never fire (the hint is server→client) | the HintText user message is hooked server→client, so the option actually drops the teammate hint |
| Config reload | the file watcher re-registers hooks on change | `gm_reload` re-syncs hooks/user messages/listeners with the new config |
| Respawn | the original only rewrites the new pawn's alpha | additionally ends the slot's corpse fade and drops its death record (safety, keeps no timer pointing at a respawned player) |
| `HideCorpses` track | pruned after `duration + 2 s` | same (`gm::CorpseTrackExpired`) |

## Corpses (`HideCorpses`)

Matches the original: a death whose attacker is not a valid player (world, fall, suicide by the
world) is ignored entirely — no corpse work and no kill feed. For a real death the death is recorded
(pawn handle, origin, time, mode) and the render work runs on the next frame (the original's
`Scheduler.NextTick`, so the engine's own death processing cannot overwrite it):

- mode 1: the pawn is hidden at once (`alpha 0`), duration 0.
- mode 2: alpha = `round(255 * (1 - elapsed / duration))`, stepped every 0.1 s, with
  `duration = Math.Max(0.1f, spec_freeze_deathanim_time ?? 0.8f)` (Config.cs:426-427).
- a `prop_ragdoll_attached` entity (designer name, matched case-insensitively) within 128 units of
  a recorded death is hidden at creation in both modes; only in mode 1 the handle is remembered for
  the deferred re-hide, exactly like the original.

## Not ported (per the user's decision, pending)

All three live in `ref/src/gamemanager-upstream/src/Config.cs` and run for **every** death /
lethal damage. `DiagGloves` in the config file is parsed but ignored.

1. **Clear gloves on every death** (Config.cs:382-392) — on every `player_death` (before the
   attacker check!) the victim pawn's `EconGloves.ItemDefinitionIndex` is set to 0,
   `ItemDefinitionIndexUpdated()` and `EconGlovesChanged++` are called, so the corpse never carries
   the wearer's glove model.
2. **`DiagGloves` diagnostics** (Config.cs:394-408 + `DumpNearbyEntities` 251-275 + `GetModelPath`
   212-238, radius 400 at line 158) — after a death it dumps every entity within 400 units to the
   server console ten times, 0.5 s apart.
3. **GM-GLOVE fix** (Config.cs:190-210, registered at 362 on `GameHooks.Entities.TakeDamage.Pre`) —
   on lethal damage to a player pawn it resets the pawn's gloves to the team default
   (5028 T / 5029 CT) and updates the fields, so the death ragdoll shows the default gloves.
