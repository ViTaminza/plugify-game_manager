// game_manager — Plugify C++ port of the SwiftlyS2 plugin "GameManager" by criskkky (the C#
// original lives in ref/src/gamemanager-upstream/src/; Config.cs has the behaviour, Helper.cs the
// message/command lists).
//
// This is the native s2sdk version (docs/PLUGIN_STANDARD.md §7): no pisexcompat, no guard/wrapper
// library and no hudmenu. Every safety check the old pisexcompat wrappers performed lives in this
// file; the engine-free decisions (config, command block, kill-feed plan, corpse alpha, message
// filters) stay in game_manager_core.
//
// Four s2sdk contracts this port has to respect (each one broke the live server or silently
// disabled a feature before):
//
//   * A Pre *game event* callback must never return Handled/Stop. In s2sdk a Pre result >= Handled
//     frees the event and supersedes the engine's FireEvent, so levels_ranks, quake_sounds, vip,
//     cs2ac, admin_system, round_end_sounds ... would never see player_death & co. again. Events are
//     hidden from the clients with SetEventBroadcast(event, true) only: s2sdk then reads the
//     "dont_broadcast" key and answers Changed, which flips the engine's dontBroadcast argument,
//     and every local listener still gets the event. All handlers here return Continue.
//
//   * The radio commands (coverme, takepoint, ..., radio1-3), playerchatwheel and +radialradio are
//     not ConCommands in CS2 (only player_ping exists as one), so a per-name AddCommandListener
//     fails silently and BlockRadio/BlockChatWheel did nothing. One *global* listener
//     (AddCommandListener("", ...)) sees every dispatched command; args[0] is matched exactly
//     (case-sensitively, like the original's BlockedCommands) for client-issued commands only,
//     which is what SwiftlyS2's HookClientCommand (Config.cs CheckClientCommands / the BlockRadio
//     hook) covers.
//
//   * The map gate (PITFALLS B1/B2): before the first map and between OnMapEnd and the next
//     OnMapStart s2sdk's client list and entity system are torn down; IsClientConnected itself
//     dereferences a dead server. Readiness is latched on the first OnPluginUpdate after OnMapStart
//     (or on the first OnPluginUpdate after OnPluginStart when the server already has a map, i.e. a
//     plugin reloaded mid-map and no OnMapEnd has arrived) and is dropped immediately in OnMapEnd.
//     OnMapEnd only clears state; no client/entity/schema call is made there.
//
//   * User-message names are not resolvable before the first map. Resolution is lazy: the first
//     update with an open map gate (and each later map start) tries the short registry name, then
//     the protobuf type name, then the documented .proto enum id. Never hook id 0 (s2sdk's global
//     user-message hook). Every callback verifies the delivered type before hiding anything.
//
// Everything registered in OnPluginStart/gm_reload is removed in OnPluginEnd with the exact key it
// was registered with: game events (name + mode), user messages (id + mode), the global command
// listener (name + mode), gm_reload (RemoveCommand only when AddConsoleCommand created it, otherwise
// RemoveCommandListener so another owner's command is never removed), the entity-created listener,
// the OnMapStart/OnMapEnd listeners, the corpse fade timers and the deferred next-frame queue.
//
// Deliberate deviations from the original are listed in README.md; the not-ported glove code is
// listed there with its Config.cs lines (Clear-gloves-on-death, the DiagGloves diagnostics and the
// OnTakeDamagePre GM-GLOVE fix).

#include <chrono>
#include <cstdint>
#include <deque>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <plg/plugin.hpp>
#include <plg/string.hpp>
#include <plg/vector.hpp>
#include <plugin_export.h>

#include <s2sdk/s2sdk.hpp>

#include "game_manager_core.hpp"

namespace {

const char* kLogTag = "[GameManager]";
constexpr char kFadeConVar[] = "spec_freeze_deathanim_time";
// The fade progress step: the original ticked the corpse alpha in 0.1 s steps. s2sdk clamps a
// repeating timer to at least one engine tick (1/64 s), so this is safely above the zero-delay
// hazard of PITFALLS C9.
constexpr double kCorpseFadeStepSeconds = 0.1;
constexpr s2sdk::TimerId kNoTimer = static_cast<s2sdk::TimerId>(-1);

// --- map gate and plugin state (PITFALLS B1/B2) ------------------------------------------------

bool g_mapReady = false;      // client/entity/schema calls are safe right now
bool g_mapStartSeen = false;  // an OnMapStart arrived since OnPluginStart / the last OnMapEnd
bool g_mapEndSeen = false;    // an OnMapEnd arrived since OnPluginStart / the last OnMapStart
bool g_mapListenersRegistered = false;
bool g_userMessageResolutionPending = false;

gm::Config g_cfg;

void ForgetCorpses(int slot, uint64_t steamid);
void ClearDeferredWork();
void SyncUserMessageHooks();

std::string ToString(const plg::string& value) {
	return std::string(value.data(), value.size());
}

void LogInfo(const std::string& message) {
	if (plg::Log) plg::Log(std::string(kLogTag) + " " + message, plg::Severity::Info, plg::source_location::current());
}
void LogWarn(const std::string& message) {
	if (plg::Log) plg::Log(std::string(kLogTag) + " " + message, plg::Severity::Warning, plg::source_location::current());
}
void LogError(const std::string& message) {
	if (plg::Log) plg::Log(std::string(kLogTag) + " " + message, plg::Severity::Error, plg::source_location::current());
}

// --- s2sdk value guards ------------------------------------------------------------------------

bool ValidEntityHandle(int32_t handle) {
	if (!gm::IsValidHandle(handle) || !__s2sdk_IsValidEntHandle) {
		return false;
	}
	try {
		return s2sdk::IsValidEntHandle(handle);
	} catch (...) {
		return false;
	}
}

bool ValidEntityPointer(void* entity) {
	if (entity == nullptr || !__s2sdk_IsValidEntPointer) {
		return false;
	}
	try {
		return s2sdk::IsValidEntPointer(entity);
	} catch (...) {
		return false;
	}
}

bool IsClientConnectedSafe(int32_t slot) {
	if (!g_mapReady || !gm::IsValidSlot(slot) || !__s2sdk_IsClientConnected) {
		return false;
	}
	try {
		return s2sdk::IsClientConnected(slot);
	} catch (...) {
		return false;
	}
}

// PITFALLS C2: a usable player slot needs a connected client that is in the game, is not the
// SourceTV bot and has a controller entity. Client queries below this gate are safe on the map.
bool HasController(int32_t slot) {
	if (!g_mapReady || !gm::IsValidSlot(slot) || !__s2sdk_IsClientConnected || !__s2sdk_IsClientInGame ||
		!__s2sdk_IsClientSourceTV || !__s2sdk_PlayerSlotToEntPointer || !__s2sdk_IsValidEntPointer) {
		return false;
	}
	try {
		if (!s2sdk::IsClientConnected(slot) || !s2sdk::IsClientInGame(slot) || s2sdk::IsClientSourceTV(slot)) {
			return false;
		}
		// PITFALLS C2: a non-null pointer is not enough; the controller entity must still be alive.
		return ValidEntityPointer(s2sdk::PlayerSlotToEntPointer(slot));
	} catch (...) {
		return false;
	}
}

// The controller handle and its pointer are two views of the same entity; validate both before any
// schema call (PITFALLS C2/C6).
int32_t ControllerHandle(int32_t slot) {
	if (!HasController(slot) || !__s2sdk_PlayerSlotToEntHandle || !__s2sdk_IsValidEntHandle ||
		!__s2sdk_IsValidEntPointer) {
		return -1;
	}
	try {
		const int32_t handle = s2sdk::PlayerSlotToEntHandle(slot);
		if (!ValidEntityHandle(handle)) {
			return -1;
		}
		void* controller = s2sdk::PlayerSlotToEntPointer(slot);
		if (!ValidEntityPointer(controller)) {
			return -1;
		}
		return handle;
	} catch (...) {
		return -1;
	}
}

// The victim/actor pawn handle. GetClientPawn only needs a valid controller, so a client that is
// still connecting answers -1 instead of a console warning from a helper that dereferences a pawn.
int32_t PawnHandle(int32_t slot) {
	if (!HasController(slot) || !__s2sdk_GetClientPawn || !__s2sdk_EntPointerToEntIndex ||
		!__s2sdk_EntIndexToEntHandle || !__s2sdk_PlayerSlotToEntHandle || !__s2sdk_IsValidEntHandle ||
		!__s2sdk_IsValidEntPointer) {
		return -1;
	}
	try {
		// The controller handle and pointer are validated together before GetClientPawn touches it.
		if (!ValidEntityHandle(s2sdk::PlayerSlotToEntHandle(slot)) ||
			!ValidEntityPointer(s2sdk::PlayerSlotToEntPointer(slot))) {
			return -1;
		}
		void* pawn = s2sdk::GetClientPawn(slot);
		if (!ValidEntityPointer(pawn)) {
			return -1;
		}
		const int32_t index = s2sdk::EntPointerToEntIndex(pawn);
		if (index <= 0) {
			return -1;
		}
		const int32_t handle = s2sdk::EntIndexToEntHandle(index);
		return ValidEntityHandle(handle) ? handle : -1;
	} catch (...) {
		return -1;
	}
}

bool IsAliveSafe(int32_t slot) {
	if (!HasController(slot) || !__s2sdk_IsClientAlive) {
		return false;
	}
	try {
		return s2sdk::IsClientAlive(slot);
	} catch (...) {
		return false;
	}
}

bool IsFakeClientSafe(int32_t slot) {
	if (!g_mapReady || !gm::IsValidSlot(slot) || !__s2sdk_IsFakeClient) {
		return false;
	}
	try {
		return s2sdk::IsFakeClient(slot);
	} catch (...) {
		return true; // treat an unavailable client as not receivable rather than as a human
	}
}

// PITFALLS C2/B5: GetClientXuid/SteamID64 must not be read for bots or disconnected slots. A bot
// answers 0, which is also the "unknown victim" marker used by the corpse identity check.
uint64_t ClientSteamId64(int32_t slot) {
	if (!HasController(slot) || !__s2sdk_GetClientSteamID64) {
		return 0;
	}
	if (IsFakeClientSafe(slot)) {
		return 0;
	}
	try {
		return s2sdk::GetClientSteamID64(slot);
	} catch (...) {
		return 0;
	}
}

int32_t MaxPlayers() {
	if (!__s2sdk_GetMaxClients) {
		return 64;
	}
	try {
		const int32_t maxClients = s2sdk::GetMaxClients();
		return maxClients > 0 && maxClients <= 64 ? maxClients : 64;
	} catch (...) {
		return 64;
	}
}

// gpGlobals->curtime through s2sdk (engine.cpp: GetGameTime). Safe to call with the map gate open;
// the caller checks it, and a missing export answers 0 so the fallback duration still works.
double GameTimeNow() {
	if (!g_mapReady || !__s2sdk_GetGameTime) {
		return 0.0;
	}
	try {
		return static_cast<double>(s2sdk::GetGameTime());
	} catch (...) {
		return 0.0;
	}
}

// Hides an event from every client without cancelling it (see the file header).
void HideEvent(void* event) {
	if (event != nullptr && __s2sdk_SetEventBroadcast) {
		try {
			s2sdk::SetEventBroadcast(event, true);
		} catch (...) {
		}
	}
}

void SetEntityAlpha(int32_t handle, uint8_t alpha) {
	if (!g_mapReady || !ValidEntityHandle(handle) || !__s2sdk_GetEntityRenderColor ||
		!__s2sdk_SetEntityRenderColor) {
		return;
	}
	try {
		plg::vec4 color = s2sdk::GetEntityRenderColor(handle);
		if (static_cast<uint8_t>(color.w) == alpha) {
			return;
		}
		color.w = static_cast<float>(alpha);
		s2sdk::SetEntityRenderColor(handle, color);
	} catch (...) {
	}
}

// --- deferred work (the original's Scheduler.NextTick) ----------------------------------------

std::deque<std::function<void()>> g_deferredWork;
// Timer ids whose callback decided to stop: s2sdk runs timer callbacks while it is walking its own
// timer list, so the kill is deferred to OnPluginUpdate instead of re-entering that system from
// inside the callback (pisexcompat did the same with its Poll-based reaper).
std::vector<s2sdk::TimerId> g_pendingTimerKills;
bool g_inFadeTimerCallback = false;

struct FadeTimerScope {
	FadeTimerScope() { g_inFadeTimerCallback = true; }
	~FadeTimerScope() { g_inFadeTimerCallback = false; }
	FadeTimerScope(const FadeTimerScope&) = delete;
	FadeTimerScope& operator=(const FadeTimerScope&) = delete;
};

void FlushTimerKills() {
	if (g_pendingTimerKills.empty() || !__s2sdk_KillsTimer) {
		g_pendingTimerKills.clear();
		return;
	}
	const std::vector<s2sdk::TimerId> kills = std::move(g_pendingTimerKills);
	g_pendingTimerKills.clear();
	for (const s2sdk::TimerId timer : kills) {
		try {
			s2sdk::KillsTimer(timer);
		} catch (...) {
		}
	}
}

void QueueDeferredWork(std::function<void()> work) {
	if (work) {
		g_deferredWork.push_back(std::move(work));
	}
}

// Runs the work queued before this update only: a callback that queues another one must not starve
// the frame (the old pisexcompat NextFrame budget). Callers hold the map gate open.
void DrainDeferredWork() {
	if (!g_mapReady) {
		return;
	}
	const std::size_t budget = g_deferredWork.size();
	for (std::size_t i = 0; i < budget && !g_deferredWork.empty(); ++i) {
		std::function<void()> work = std::move(g_deferredWork.front());
		g_deferredWork.pop_front();
		try {
			work();
		} catch (const std::exception& ex) {
			LogError(std::string("deferred work failed: ") + ex.what());
		} catch (...) {
			LogError("deferred work failed with an unknown exception");
		}
	}
}

void ClearDeferredWork() {
	g_deferredWork.clear();
}

// --- map lifecycle (PITFALLS B1/B2) ------------------------------------------------------------

bool MapAvailable() {
	if (!__s2sdk_GetCurrentMap) {
		return false;
	}
	try {
		return !ToString(s2sdk::GetCurrentMap()).empty();
	} catch (...) {
		return false;
	}
}

// OnMapStart only arms the gate: PITFALLS B1/B2 grants readiness on the first OnPluginUpdate after
// it, never inside the map-start callback itself. The same update path covers a plugin loaded into
// an already running map (no OnMapStart was seen, but the server has a map and OnMapEnd has not
// arrived yet).
void OnMapStartListener() {
	try {
		g_mapStartSeen = true;
		g_mapEndSeen = false;
		g_userMessageResolutionPending = true;
	} catch (...) {
	}
}

void OnMapEndListener() {
	try {
		// From here until the next OnMapStart every client query crashes inside s2sdk (the client
		// list is being torn down): only local state may be changed. No Client* / Entity* call.
		g_mapReady = false;
		g_mapStartSeen = false;
		g_mapEndSeen = true;
		ForgetCorpses(-1, 0);
		ClearDeferredWork();
		FlushTimerKills();
	} catch (...) {
	}
}

void RegisterMapListeners() {
	if (g_mapListenersRegistered) {
		return;
	}
	if (!__s2sdk_OnMapStart_Register || !__s2sdk_OnMapEnd_Register) {
		LogWarn("the s2sdk map listeners are unavailable - the plugin stays inert until a reload");
		return;
	}
	try {
		s2sdk::OnMapStart_Register(&OnMapStartListener);
		s2sdk::OnMapEnd_Register(&OnMapEndListener);
		g_mapListenersRegistered = true;
	} catch (...) {
		LogWarn("registering the s2sdk map listeners failed");
	}
}

void UnregisterMapListeners() {
	if (!g_mapListenersRegistered) {
		return;
	}
	g_mapListenersRegistered = false;
	if (__s2sdk_OnMapStart_Unregister) {
		try {
			s2sdk::OnMapStart_Unregister(&OnMapStartListener);
		} catch (...) {
		}
	}
	if (__s2sdk_OnMapEnd_Unregister) {
		try {
			s2sdk::OnMapEnd_Unregister(&OnMapEndListener);
		} catch (...) {
		}
	}
}

// --- config ------------------------------------------------------------------------------------

// <GetConfigsDir()>/game_manager: the Plugify layout, never a Metamod addons/ path (the manifest
// name and the directory name are the same). "" when the Plugify host is unavailable.
std::string ConfigDirPath() {
	if (!plg::GetConfigsDir) {
		return {};
	}
	std::string dir = ToString(plg::GetConfigsDir());
	if (dir.empty()) {
		return {};
	}
	if (dir.back() != '/') {
		dir.push_back('/');
	}
	dir.append("game_manager");
	return dir;
}

bool ReadTextFile(const std::string& path, std::string& text) {
	std::ifstream in(path, std::ios::binary);
	if (!in) {
		return false;
	}
	std::ostringstream buffer;
	buffer << in.rdbuf();
	text = buffer.str();
	return true;
}

// Parses the text of config.jsonc (pure part in gm::ParseConfig) and logs when it is unreadable.
void ApplyConfigText(const std::string& text) {
	if (text.empty()) {
		g_cfg = gm::DefaultConfig();
		return;
	}
	g_cfg = gm::ParseConfig(text);
}

void LoadConfig() {
	g_cfg = gm::DefaultConfig();
	const std::string dir = ConfigDirPath();
	if (dir.empty()) {
		LogWarn("no Plugify configs directory - running on in-memory defaults");
		return;
	}
	const std::string path = dir + "/config.jsonc";
	std::string text;
	if (ReadTextFile(path, text)) {
		ApplyConfigText(text);
		return;
	}

	// First run: write the defaults so the file can be edited (same layout as the original's).
	LogInfo("no config at " + path + " - writing the defaults");
	try {
		std::error_code ec;
		std::filesystem::create_directories(dir, ec);
		std::ofstream out(path);
		if (out) {
			out << gm::DefaultConfigJson();
		} else {
			LogWarn("cannot write " + path + " - running on in-memory defaults");
		}
	} catch (const std::exception& ex) {
		LogWarn(std::string("cannot write the default config: ") + ex.what());
	}
}

// The original's "native commands": server convars toggled to match the config. ServerCommand does
// not touch a client or an entity, so it is safe outside the map gate (the original ran it from
// OnPluginStart and round_start as well).
void ExecuteNativeCommands() {
	if (!__s2sdk_ServerCommand) {
		return;
	}
	try {
		if (g_cfg.BlockBotRadio) s2sdk::ServerCommand(plg::string("bot_chatter off"));
		if (g_cfg.BlockGrenadesRadio) s2sdk::ServerCommand(plg::string("sv_ignoregrenaderadio 1"));
		if (g_cfg.HideRadar) s2sdk::ServerCommand(plg::string("sv_disable_radar 1"));
		if (g_cfg.HideTeammateHeadtags == 1) s2sdk::ServerCommand(plg::string("sv_teamid_overhead 0"));
		if (g_cfg.HideTeammateHeadtags == 2) {
			s2sdk::ServerCommand(plg::string("sv_teamid_overhead 1; sv_teamid_overhead_always_prohibit 1; sv_teamid_overhead_maxdist 0"));
		}
		if (g_cfg.HideTeammateHeadtags == 3 && g_cfg.HideTeammateHeadtagsDistance > 0) {
			s2sdk::ServerCommand(plg::string("sv_teamid_overhead 1; sv_teamid_overhead_always_prohibit 1; sv_teamid_overhead_maxdist " +
											std::to_string(g_cfg.HideTeammateHeadtagsDistance)));
		}
		if (g_cfg.DisableFallDamage) s2sdk::ServerCommand(plg::string("sv_falldamage_scale 0"));
		if (g_cfg.DisableSVCheats) s2sdk::ServerCommand(plg::string("sv_cheats 0"));
		if (g_cfg.DisableC4) s2sdk::ServerCommand(plg::string("mp_give_player_c4 0"));
		if (g_cfg.DisableCameraSpectator) s2sdk::ServerCommand(plg::string("sv_disable_observer_interpolation true"));
		if (g_cfg.SoundsMuteFootsteps) s2sdk::ServerCommand(plg::string("sv_footsteps 0"));
		if (g_cfg.SoundsMuteJumpLand) s2sdk::ServerCommand(plg::string("sv_min_jump_landing_sound 999999"));
	} catch (...) {
	}
}

// --- corpses (HideCorpses) ---------------------------------------------------------------------

void SyncCorpseListener(); // registers/unregisters the entity-created listener (definition below)

// One death: the pawn that died (faded in mode 2, hidden in mode 1) and - mode 1 only - the
// "prop_ragdoll_attached" props the game spawned near the death (the original keeps those handles to
// hide them again on the deferred pass; in mode 2 the attached props are hidden once at creation and
// only the body fades, Config.cs:145-152).
struct Corpse {
	int slot = -1;
	// The victim's SteamID at death time. The slot alone is not enough: a new player can take the
	// freed slot before the fade ends, and treating that player as "the victim respawned" would
	// cancel the fade (and the player_spawn handler would cancel it even earlier).
	uint64_t steamid = 0;
	int32_t pawnHandle = -1;
	int mode = 0;			   // the HideCorpses mode captured at death time (the original's track.Mode)
	std::vector<int32_t> attachedHandles;
	float x = 0.0f;
	float y = 0.0f;
	float z = 0.0f;
	double deathTime = 0.0;
	double duration = 0.0;
	bool cancelled = false;			 // also invalidates deferred work that still owns this record
	s2sdk::TimerId timer = kNoTimer; // set while the mode-2 fade runs; kNoTimer otherwise
};

std::vector<std::shared_ptr<Corpse>> g_corpses;
// Timer id -> the corpse whose fade it drives. Removed together with KillsTimer, so a late engine
// fire after StopCorpseFade cannot reach a dead record (and s2sdk timer ids start at 0, so 0 is a
// real id - kNoTimer is the -1 sentinel, exactly like CreateTimer's failure value).
std::map<s2sdk::TimerId, std::weak_ptr<Corpse>> g_corpseTimers;

void StopCorpseFade(Corpse& corpse) {
	if (corpse.timer == kNoTimer) {
		return;
	}
	const s2sdk::TimerId timer = corpse.timer;
	corpse.timer = kNoTimer;
	g_corpseTimers.erase(timer);
	if (g_inFadeTimerCallback) {
		// We are inside the timer callback (s2sdk holds its timer mutex and is walking the list);
		// the kill happens on the next OnPluginUpdate.
		g_pendingTimerKills.push_back(timer);
		return;
	}
	if (!__s2sdk_KillsTimer) {
		return;
	}
	try {
		s2sdk::KillsTimer(timer);
	} catch (...) {
	}
}

// Drops the fade (and the death record) - respawn, map end and unload all end the corpse.
// slot < 0 forgets every corpse; otherwise only the corpse of `steamid` in that slot, because the
// slot may already belong to another player when the spawn/events arrive.
void ForgetCorpses(int slot, uint64_t steamid) {
	for (auto it = g_corpses.begin(); it != g_corpses.end();) {
		if (slot < 0 || ((*it)->slot == slot && (*it)->steamid == steamid)) {
			(*it)->cancelled = true;
			StopCorpseFade(**it);
			it = g_corpses.erase(it);
		} else {
			++it;
		}
	}
}

// True when the corpse's own player is alive again in its slot. A different player in the same
// slot is not a respawn: the fade must finish (the new pawn never inherited this corpse's handle).
bool VictimRespawned(const Corpse& corpse) {
	if (!HasController(corpse.slot) || !IsAliveSafe(corpse.slot)) {
		return false;
	}
	if (corpse.steamid == 0) {
		return true; // no SteamID to compare (a bot): keep the slot-based check
	}
	return ClientSteamId64(corpse.slot) == corpse.steamid;
}

// The original drops a corpse track once `now - StartedAt > duration + 2 s` (PruneCorpseFadeTracks).
void PruneCorpses(double now) {
	for (auto it = g_corpses.begin(); it != g_corpses.end();) {
		if ((*it)->timer == kNoTimer && gm::CorpseTrackExpired(now, (*it)->deathTime, (*it)->duration)) {
			it = g_corpses.erase(it);
		} else {
			++it;
		}
	}
}

// spec_freeze_deathanim_time via the original's `convar != null ? convar.Value : 0.8f` - no convar
// handle (missing cvar or missing binding) is the null case.
std::optional<double> CorpseFadeConVar() {
	if (!__s2sdk_FindConVar || !__s2sdk_GetConVarFloat) {
		return std::nullopt;
	}
	try {
		const uint64_t conVar = s2sdk::FindConVar(plg::string(kFadeConVar));
		if (conVar == 0) {
			return std::nullopt;
		}
		return s2sdk::GetConVarFloat(conVar);
	} catch (...) {
		return std::nullopt;
	}
}

bool TickCorpseFade(const std::shared_ptr<Corpse>& corpse);

// One fade step (s2sdk timer, game thread) - the original's FadeStep: only the *body* (the pawn)
// fades; attached props stay hidden at alpha 0. The record is re-validated from scratch on every
// step: the pawn handle may be stale (recycled index), the victim may have respawned and the map
// may have ended while the timer waited.
void OnCorpseFadeTimer(uint32_t id, const plg::vector<plg::any>& /*userData*/) {
	FadeTimerScope inCallback;
	std::shared_ptr<Corpse> corpse;
	try {
		const auto found = g_corpseTimers.find(id);
		if (found == g_corpseTimers.end()) {
			return; // killed on our side; a stale engine fire is not an error
		}
		corpse = found->second.lock();
		if (!corpse || corpse->cancelled || corpse->timer != id) {
			g_corpseTimers.erase(found);
			return;
		}
		if (TickCorpseFade(corpse)) {
			return;
		}
	} catch (const std::exception& ex) {
		LogError(std::string("corpse fade callback: ") + ex.what());
	} catch (...) {
		LogError("corpse fade callback failed with an unknown exception");
	}
	if (corpse) {
		StopCorpseFade(*corpse);
	}
}

bool TickCorpseFade(const std::shared_ptr<Corpse>& corpse) {
	if (!corpse || corpse->cancelled || !g_mapReady || g_cfg.HideCorpses != 2 ||
		!ValidEntityHandle(corpse->pawnHandle)) {
		return false;
	}
	if (VictimRespawned(*corpse)) {
		return false; // respawned: the new pawn must not inherit the corpse alpha
	}
	if (!__s2sdk_GetGameTime) {
		return false; // without the engine clock the fade cannot make progress; hide at once
	}

	const double elapsed = GameTimeNow() - corpse->deathTime;
	const uint8_t alpha = gm::CorpseAlpha(2, elapsed, corpse->duration);
	SetEntityAlpha(corpse->pawnHandle, alpha);
	return alpha != 0; // the original re-schedules FadeStep only while alpha > 0
}

void StartCorpseFade(const std::shared_ptr<Corpse>& corpse) {
	// Creation needs both ends of the timer API: without a way to kill it, a timer would outlive the
	// map or the library. Hiding at once is the closest safe mode-2 behaviour then.
	if (!__s2sdk_CreateTimer || !__s2sdk_KillsTimer || !__s2sdk_GetGameTime) {
		SetEntityAlpha(corpse->pawnHandle, 0);
		return;
	}
	try {
		// Repeat + NoMapChange: RunFrame re-arms a repeating timer, and NoMapChange makes the engine
		// drop it at OnMapEnd even if the map-end listener is never reached (PITFALLS B4/C9).
		const auto flags = static_cast<s2sdk::TimerFlag>(
			static_cast<int32_t>(s2sdk::TimerFlag::Repeat) | static_cast<int32_t>(s2sdk::TimerFlag::NoMapChange));
		const s2sdk::TimerId timer =
			s2sdk::CreateTimer(kCorpseFadeStepSeconds, &OnCorpseFadeTimer, flags, plg::vector<plg::any>{});
		if (timer == kNoTimer) {
			SetEntityAlpha(corpse->pawnHandle, 0);
			return;
		}
		corpse->timer = timer;
		g_corpseTimers[timer] = corpse;
	} catch (...) {
		SetEntityAlpha(corpse->pawnHandle, 0);
	}
}

// The original defers the render work of a death to the next tick (Core.Scheduler.NextTick) so the
// engine's own death processing cannot overwrite it: mode 1 hides the pawn and the attached props
// that are known by then, mode 2 starts the 0.1 s fade chain (whose first step writes ~0.1 s after
// this tick). The deferred work is owned by this plugin, is drained only with the map gate open and
// is cleared on map end/unload - no callback can outlive the library.
void BeginCorpseHide(const std::shared_ptr<Corpse>& corpse) {
	QueueDeferredWork([corpse] {
		if (!corpse || corpse->cancelled || !g_mapReady || !ValidEntityHandle(corpse->pawnHandle)) {
			return;
		}
		if (VictimRespawned(*corpse)) {
			return; // respawned before the deferred pass ran
		}
		if (corpse->mode == 1) {
			SetEntityAlpha(corpse->pawnHandle, 0);
			for (const int32_t handle : corpse->attachedHandles) {
				SetEntityAlpha(handle, 0);
			}
			return;
		}
		if (corpse->mode == 2 && g_cfg.HideCorpses == 2) {
			StartCorpseFade(corpse);
		}
	});
}

// "prop_ragdoll_attached": the corpse prop of a dead player. The original matches the entity's
// designer name case-insensitively in OnEntityCreated, finds the death within 128 units of it and
// hides the prop immediately; in mode 1 the handle is remembered for the deferred re-hide.
void OnEntityCreatedListener(int32_t entityHandle) {
	if (!g_mapReady || !ValidEntityHandle(entityHandle) || (g_cfg.HideCorpses != 1 && g_cfg.HideCorpses != 2) ||
		g_corpses.empty() || !__s2sdk_GetEntityClassname || !__s2sdk_GetEntityAbsOrigin) {
		return; // no death is waiting for its corpse props: skip every entity the server creates
	}
	try {
		if (!gm::IsAttachedRagdollClass(ToString(s2sdk::GetEntityClassname(entityHandle)))) {
			return;
		}

		const plg::vec3 origin = s2sdk::GetEntityAbsOrigin(entityHandle);
		const double now = GameTimeNow();
		PruneCorpses(now);

		std::vector<gm::DeathRecord> deaths;
		deaths.reserve(g_corpses.size());
		for (const auto& corpse : g_corpses) {
			deaths.push_back({corpse->slot, corpse->pawnHandle, corpse->x, corpse->y, corpse->z, corpse->deathTime});
		}
		const int match = gm::FindDeathForRagdoll(deaths, origin.x, origin.y, origin.z);
		if (match < 0) {
			return; // somebody else's prop (a map ragdoll, another plugin)
		}

		const std::shared_ptr<Corpse>& corpse = g_corpses[static_cast<std::size_t>(match)];
		if (corpse->mode == 1) {
			corpse->attachedHandles.push_back(entityHandle);
		}
		// Attached props disappear immediately; only the body uses the configured fade.
		SetEntityAlpha(entityHandle, 0);
	} catch (...) {
	}
}

// --- game events -------------------------------------------------------------------------------

void HandlePlayerDeath(void* event) {
	if (!g_mapReady || !__s2sdk_GetEventPlayerSlot) {
		return; // PITFALLS B2: no client query in the map-change window
	}
	const int victimSlot = s2sdk::GetEventPlayerSlot(event, plg::string("userid"));
	const int attackerSlot = s2sdk::GetEventPlayerSlot(event, plg::string("attacker"));

	// The original's first check after this point (Config.cs:410-411): a death whose attacker is
	// not a valid player (world damage, fall, a suicide by the world) is ignored *entirely* - no
	// corpse handling and no kill feed for it.
	const bool attackerValid = gm::IsValidSlot(attackerSlot) && HasController(attackerSlot);
	if (!attackerValid) {
		return;
	}

	// Hide the corpse (mode 1) / start the fade (mode 2). The original reads the death origin from
	// the victim's pawn and skips the work when there is none.
	if ((g_cfg.HideCorpses == 1 || g_cfg.HideCorpses == 2) && gm::IsValidSlot(victimSlot) &&
		HasController(victimSlot) && __s2sdk_GetEntityAbsOrigin) {
		const int32_t pawn = PawnHandle(victimSlot);
		if (gm::IsValidHandle(pawn) && ValidEntityHandle(pawn)) {
			auto corpse = std::make_shared<Corpse>();
			corpse->slot = victimSlot;
			corpse->steamid = ClientSteamId64(victimSlot);
			corpse->pawnHandle = pawn;
			corpse->mode = g_cfg.HideCorpses;
			corpse->deathTime = GameTimeNow();
			// The original: mode 1 tracks have duration 0, mode 2 reads spec_freeze_deathanim_time.
			corpse->duration = corpse->mode == 2 ? gm::CorpseFadeSeconds(CorpseFadeConVar()) : 0.0;
			const plg::vec3 origin = s2sdk::GetEntityAbsOrigin(pawn);
			corpse->x = origin.x;
			corpse->y = origin.y;
			corpse->z = origin.z;
			g_corpses.push_back(corpse);
			PruneCorpses(corpse->deathTime);
			BeginCorpseHide(corpse);
		}
	}

	// Kill feed. HideKillFeed 1 hides the death for every client; 2 ("show only own kills") hides it
	// too and hands it to the killer directly - the attacker was proven valid above, and a bot has
	// nobody to show it to (the original's !attacker.IsFakeClient around its FireToPlayer).
	if (g_cfg.HideKillFeed > 0) {
		const bool attackerReceivable = !IsFakeClientSafe(attackerSlot);
		const gm::KillFeedPlan plan = gm::PlanKillFeed(g_cfg, attackerReceivable);
		if (plan.hideFromClients) {
			HideEvent(event);
		}
		if (plan.sendToAttacker && __s2sdk_FireEventToClient) {
			// The original fires a fresh copy with eight fields to the killer
			// (Config.cs:465-475); s2sdk's FireEventToClient hands the live event to that one
			// client's listener (event_manager.cpp FireEventToClient) while SetEventBroadcast above
			// keeps it server-side for everyone else. Visible difference: the client sees the
			// full event, not the original's eight-field copy.
			try {
				s2sdk::FireEventToClient(event, attackerSlot);
			} catch (...) {
			}
		}
	}
}

void HandlePlayerSpawn(void* event) {
	if (!g_mapReady || !__s2sdk_GetEventPlayerSlot) {
		return;
	}
	const int slot = s2sdk::GetEventPlayerSlot(event, plg::string("userid"));
	if (!gm::IsValidSlot(slot)) {
		return;
	}
	// The victim is alive again: end the fade (and drop the death record) before the restore below.
	// Only this player's corpse - a different player spawning in the slot must not cancel it.
	ForgetCorpses(slot, ClientSteamId64(slot));

	const int32_t handle = PawnHandle(slot);
	if (gm::IsValidHandle(handle)) {
		SetEntityAlpha(handle, g_cfg.HideLegs ? 254 : 255);
	}
}

// The original's `playerController.MusicKitID = 0; playerController.MusicKitIDUpdated()` (Config.cs:
// 662-663): the MVP's controller field m_iMusicKitID is zeroed with a network state change. The
// field is not on s2sdk's CS2BadList (only the inventory component's m_nMusicID is), so the typed
// setter is the same write the live round_end_sounds plugin makes.
void MuteMvpMusicKit(int slot) {
	if (!__s2sdk_SetEntSchema) {
		return;
	}
	try {
		const int32_t handle = ControllerHandle(slot);
		if (handle == -1) {
			return;
		}
		s2sdk::SetEntSchema(handle, plg::string("CCSPlayerController"), plg::string("m_iMusicKitID"), 0,
							/*changeState=*/true, /*element=*/0);
	} catch (...) {
	}
}

// round_mvp: SoundsMuteMVPMusic 1 = mute the MVP music (the anthem), 2 = stop all music. Exactly
// the original's flow: both modes bail out when the MVP has no valid, non-bot controller/player
// (Config.cs:656-658), mode 1 zeroes the MVP's music kit, mode 2 stops every music for everyone.
void HandleRoundMvp(void* event) {
	if (!g_mapReady || g_cfg.SoundsMuteMVPMusic <= 0 || !__s2sdk_GetEventPlayerSlot) {
		return;
	}
	int slot = -1;
	try {
		slot = s2sdk::GetEventPlayerSlot(event, plg::string("userid"));
	} catch (...) {
		return;
	}
	if (!gm::IsValidSlot(slot) || !HasController(slot) || IsFakeClientSafe(slot)) {
		return;
	}

	if (g_cfg.SoundsMuteMVPMusic == 1) {
		MuteMvpMusicKit(slot);
		return;
	}
	if (g_cfg.SoundsMuteMVPMusic != 2 || !__s2sdk_EmitSoundToClient) {
		return;
	}
	// The original's SoundEvent with SourceEntityIndex = -1; s2sdk's EmitSoundToClient needs a real
	// entity, so the world entity (handle 0) is the source - a stop event does not use its position.
	plg::vector<int32_t> slots;
	const int32_t maxPlayers = MaxPlayers();
	for (int32_t s = 0; s < maxPlayers; ++s) {
		if (IsClientConnectedSafe(s)) {
			slots.push_back(s);
		}
	}
	if (!slots.empty()) {
		try {
			s2sdk::EmitSoundToClient(0, slots, plg::string("StopSoundEvents.StopAllMusic"), 1.0f, 1.0f);
		} catch (...) {
		}
	}
}

// The single entry point of every HookEvent below. It returns Continue on every path: a Pre result
// >= Handled would free the event and supersede FireEvent for every other plugin on the server.
s2sdk::ResultType OnRawGameEvent(const plg::string& name, void* event, bool /*dontBroadcast*/) {
	if (event == nullptr) {
		return s2sdk::ResultType::Continue;
	}
	const std::string eventName = ToString(name);
	try {
		if (eventName == "player_death") {
			HandlePlayerDeath(event);
		} else if (eventName == "player_spawn") {
			HandlePlayerSpawn(event);
		} else if (eventName == "player_team") {
			if (g_cfg.IgnoreJoinTeamMessages) HideEvent(event);
		} else if (eventName == "player_disconnect") {
			if (g_cfg.IgnoreDisconnectMessages) HideEvent(event);
		} else if (eventName == "round_start") {
			ExecuteNativeCommands();
		} else if (eventName == "bomb_planted") {
			if (g_cfg.IgnoreBombPlantedHUDMessages) HideEvent(event);
		} else if (eventName == "round_mvp") {
			HandleRoundMvp(event);
		}
	} catch (const std::exception& ex) {
		LogError(std::string("event ") + eventName + ": " + ex.what());
	} catch (...) {
		LogError("event " + eventName + ": unknown failure");
	}
	return s2sdk::ResultType::Continue;
}

// --- net user messages -------------------------------------------------------------------------

struct UserMessageHookRecord {
	int16_t id = 0;
	s2sdk::UserMessageCallback callback = nullptr;
};

std::vector<UserMessageHookRecord> g_userMessageHooks;
// One warning per name per process (a reload must not repeat the same line).
std::set<std::string> g_warnedMessages;

bool TextMessageFiltersEnabled() {
	return g_cfg.IgnoreAwardsMoneyMessages || g_cfg.IgnorePlayerSavedYouMessages ||
		   g_cfg.IgnoreChickenKilledMessages || g_cfg.IgnorePlantingBombMessages ||
		   g_cfg.IgnoreDefusingBombMessages || g_cfg.IgnoreTeammateAttackMessages;
}

// Hides a user message from every client: the recipients are removed and Stop makes s2sdk write the
// (empty) filter back into the recipient mask and skip the send (user_message_manager.cpp:83-100).
s2sdk::ResultType HideUserMessage(void* message) {
	if (__s2sdk_UserMessageRemoveAllRecipient) {
		s2sdk::UserMessageRemoveAllRecipient(message);
	}
	return s2sdk::ResultType::Stop;
}

bool UserMessageMatches(void* message, std::string_view expected) {
	if (!__s2sdk_UserMessageGetMessageName) {
		return true; // Older bindings cannot validate; the hook ID is still constrained by resolution.
	}
	try {
		const plg::string actualValue = s2sdk::UserMessageGetMessageName(message);
		const std::string actual(actualValue.data(), actualValue.size());
		if (actual.find(expected) != std::string::npos) {
			return true;
		}
		// Keyed by the message type, not by the message object: every delivery is a new object, so
		// a pointer key would log (and grow the set) once per message instead of once per type.
		const std::string key = "type:" + std::string(expected) + ":" + actual;
		if (g_warnedMessages.insert(key).second) {
			LogWarn("user message id hook delivered '" + actual + "', expected " + std::string(expected) +
					" - hook disabled");
		}
	} catch (...) {
	}
	return false;
}

s2sdk::ResultType OnBlood(void* message) {
	try {
		if (!message || !UserMessageMatches(message, "WorldDecal")) return s2sdk::ResultType::Continue;
		if (g_cfg.HideBlood) return HideUserMessage(message);
	} catch (...) {
	}
	return s2sdk::ResultType::Continue;
}

s2sdk::ResultType OnSparks(void* message) {
	try {
		if (!message || !UserMessageMatches(message, "EffectDispatch")) return s2sdk::ResultType::Continue;
		if (g_cfg.HideHeadshotSparks) return HideUserMessage(message);
	} catch (...) {
	}
	return s2sdk::ResultType::Continue;
}

s2sdk::ResultType OnTextMsg(void* message) {
	try {
		if (!message || !UserMessageMatches(message, "TextMsg") || !TextMessageFiltersEnabled() || !__s2sdk_UserMessageGetRepeatedFieldCount || !__s2sdk_PbReadString) {
			return s2sdk::ResultType::Continue;
		}
		// "param" is a repeated field: every element is tested, index 0 is a real element here.
		const int count = s2sdk::UserMessageGetRepeatedFieldCount(message, plg::string("param"));
		for (int i = 0; i < count; ++i) {
			const plg::string value = s2sdk::PbReadString(message, plg::string("param"), i);
			if (gm::ShouldIgnoreTextMessage(g_cfg, std::string_view(value.data(), value.size()))) {
				return HideUserMessage(message);
			}
		}
	} catch (...) {
	}
	return s2sdk::ResultType::Continue;
}

s2sdk::ResultType OnHintText(void* message) {
	try {
		if (!message || !UserMessageMatches(message, "HintText") || !g_cfg.IgnoreTeammateAttackMessages || !__s2sdk_PbReadString) {
			return s2sdk::ResultType::Continue;
		}
		// Non-repeated field: index -1 (a 0 would be "repeated element 0", log on every message).
		const plg::string value = s2sdk::PbReadString(message, plg::string("message"), -1);
		if (gm::ShouldIgnoreHintText(g_cfg, std::string_view(value.data(), value.size()))) {
			return HideUserMessage(message);
		}
	} catch (...) {
	}
	return s2sdk::ResultType::Continue;
}

s2sdk::ResultType OnRadioText(void* message) {
	try {
		if (!message || !UserMessageMatches(message, "RadioText") || !(g_cfg.IgnorePlantingBombMessages || g_cfg.IgnoreDefusingBombMessages) || !__s2sdk_PbReadString) {
			return s2sdk::ResultType::Continue;
		}
		const plg::string value = s2sdk::PbReadString(message, plg::string("msg_name"), -1);
		if (gm::ShouldIgnoreRadioText(g_cfg, std::string_view(value.data(), value.size()))) {
			return HideUserMessage(message);
		}
	} catch (...) {
	}
	return s2sdk::ResultType::Continue;
}

void RegisterUserMessageHook(const char* shortName, const char* protoName, int16_t fallbackId,
							 s2sdk::UserMessageCallback callback) {
	int16_t id = 0;
	const char* selectedName = shortName;
	if (__s2sdk_UserMessageFindMessageIdByName) {
		for (const char* candidate : {shortName, protoName}) {
			try {
				id = s2sdk::UserMessageFindMessageIdByName(plg::string(candidate));
			} catch (...) {
				id = 0;
			}
			if (id > 0) {
				selectedName = candidate;
				break;
			}
		}
	}
	const bool usedFallback = id <= 0;
	if (usedFallback) {
		id = fallbackId;
	}
	// HookUserMessage accepts any positive id; 0 means the global hook, not failure.
	if (id > 0 && __s2sdk_HookUserMessage) {
		try {
			if (s2sdk::HookUserMessage(id, callback, s2sdk::HookMode::Pre)) {
				g_userMessageHooks.push_back({id, callback});
				LogInfo(std::string("user message '") + selectedName + "' id=" + std::to_string(id) +
						(usedFallback ? " (proto enum fallback)" : " (registry)"));
				return;
			}
		} catch (...) {
		}
	}
	if (g_warnedMessages.insert(shortName).second) {
		LogWarn(std::string("user message '") + shortName + "' could not be hooked (including proto id " +
				std::to_string(fallbackId) + ") - that filter is off");
	}
}

void UnregisterUserMessageHooks() {
	if (!__s2sdk_UnhookUserMessage) {
		g_userMessageHooks.clear();
		return;
	}
	for (const UserMessageHookRecord& hook : g_userMessageHooks) {
		try {
			s2sdk::UnhookUserMessage(hook.id, hook.callback, s2sdk::HookMode::Pre);
		} catch (...) {
		}
	}
	g_userMessageHooks.clear();
}

// Fallback ids: ref/src/cs2-lvl_ranks_modules/LR_FakeRanks/protobuf/:
// te.proto:14 WorldDecal=411, :4 EffectDispatch=400; usermessages.proto:22 TextMsg=124;
// cstrike15_usermessages.proto:26 HintText=323, :25 RadioText=322.
// TextMsg uses UM_TextMsg (CUserMessageTextMsg), not CS_UM_TextMsg (307).
void SyncUserMessageHooks() {
	if (!g_mapReady) {
		// User-message names are registered by the game only once a map exists (PLUGIN_STANDARD
		// §7 / PITFALLS B1). Try again on the first update with an open gate or on OnMapStart.
		g_userMessageResolutionPending = true;
		return;
	}
	g_userMessageResolutionPending = false;
	UnregisterUserMessageHooks();
	if (g_cfg.HideBlood) RegisterUserMessageHook("WorldDecal", "CMsgTEWorldDecal", 411, &OnBlood);
	if (g_cfg.HideHeadshotSparks) RegisterUserMessageHook("EffectDispatch", "CMsgTEEffectDispatch", 400, &OnSparks);
	if (TextMessageFiltersEnabled()) RegisterUserMessageHook("TextMsg", "CUserMessageTextMsg", 124, &OnTextMsg);
	if (g_cfg.IgnoreTeammateAttackMessages) RegisterUserMessageHook("HintText", "CCSUsrMsg_HintText", 323, &OnHintText);
	if (g_cfg.IgnorePlantingBombMessages || g_cfg.IgnoreDefusingBombMessages) {
		RegisterUserMessageHook("RadioText", "CCSUsrMsg_RadioText", 322, &OnRadioText);
	}
}

// --- commands ----------------------------------------------------------------------------------

// The global command listener ("", i.e. every dispatched command - a per-name listener only works
// for real ConCommands, and the radio commands are not ones). Only the name is matched (exactly like
// the original's BlockedCommands lookup) and only for client-issued commands: the original's
// HookClientCommand callbacks carry the issuing client and never fire for the server console.
bool g_globalCommandListener = false;

s2sdk::ResultType OnClientCommand(int32_t callerSlot, s2sdk::ConCommandContext /*context*/,
								  const plg::vector<plg::string>& args) {
	try {
		if (callerSlot < 0 || args.empty()) {
			return s2sdk::ResultType::Continue; // the server console / RCON is not a client command
		}
		const std::string command = ToString(args[0]);
		if (gm::ShouldBlockCommand(g_cfg, command)) {
			return s2sdk::ResultType::Stop; // s2sdk supersedes the dispatch: the command runs nowhere
		}
	} catch (...) {
	}
	return s2sdk::ResultType::Continue;
}

bool CommandBlockingEnabled() {
	return g_cfg.BlockRadio || g_cfg.BlockPing || g_cfg.BlockChatWheel || g_cfg.BlockGrenadesRadio ||
		   g_cfg.BlockBotRadio || !g_cfg.BlockedCommands.empty();
}

void SyncCommandListener() {
	if (CommandBlockingEnabled()) {
		if (!g_globalCommandListener && __s2sdk_AddCommandListener) {
			try {
				if (s2sdk::AddCommandListener(plg::string(""), &OnClientCommand, s2sdk::HookMode::Pre)) {
					g_globalCommandListener = true;
				} else {
					LogWarn("the global command listener could not be registered - command blocking is off");
				}
			} catch (...) {
			}
		}
	} else if (g_globalCommandListener && __s2sdk_RemoveCommandListener) {
		try {
			s2sdk::RemoveCommandListener(plg::string(""), &OnClientCommand, s2sdk::HookMode::Pre);
		} catch (...) {
		}
		g_globalCommandListener = false;
	}
}

void RemoveGlobalCommandListener() {
	if (!g_globalCommandListener) {
		return;
	}
	g_globalCommandListener = false;
	if (!__s2sdk_RemoveCommandListener) {
		return;
	}
	try {
		s2sdk::RemoveCommandListener(plg::string(""), &OnClientCommand, s2sdk::HookMode::Pre);
	} catch (...) {
	}
}

// gm_reload: re-read the config and re-sync everything the config controls.
bool g_reloadCommandAttempted = false;
bool g_reloadCommandOwned = false;

s2sdk::ResultType ReloadCommand(int32_t slot, s2sdk::ConCommandContext /*context*/,
								const plg::vector<plg::string>& /*args*/) {
	// s2sdk also dispatches !gm_reload through chat, bypassing ConCommand engine flags.
	if (slot != -1) {
		return s2sdk::ResultType::Continue;
	}
	try {
		LoadConfig();
		ExecuteNativeCommands();
		SyncUserMessageHooks();
		SyncCorpseListener();
		SyncCommandListener();
		LogInfo("config reloaded");
	} catch (const std::exception& ex) {
		LogError(std::string("gm_reload failed: ") + ex.what());
	}
	return s2sdk::ResultType::Continue;
}

// --- registration bookkeeping ------------------------------------------------------------------

// Every hook the plugin registers, so OnPluginEnd can remove exactly the same set (PITFALLS A7 /
// the JEV "no_teardown" finding: a leftover callback would run into the unloaded library).
struct EventHookRecord {
	plg::string name;
	s2sdk::HookMode mode;
};

std::vector<EventHookRecord> g_eventHooks;
bool g_entityCreatedListener = false;

void RegisterEventHook(const char* name, s2sdk::HookMode mode) {
	if (!__s2sdk_HookEvent) {
		return;
	}
	try {
		if (s2sdk::HookEvent(plg::string(name), &OnRawGameEvent, mode) == s2sdk::EventHookError::Okay) {
			g_eventHooks.push_back({plg::string(name), mode});
		} else {
			LogWarn(std::string("hooking event '") + name + "' failed");
		}
	} catch (...) {
	}
}

void UnregisterEventHooks() {
	if (!__s2sdk_UnhookEvent) {
		g_eventHooks.clear();
		return;
	}
	for (const EventHookRecord& hook : g_eventHooks) {
		try {
			s2sdk::UnhookEvent(hook.name, &OnRawGameEvent, hook.mode);
		} catch (...) {
		}
	}
	g_eventHooks.clear();
}

void SyncCorpseListener() {
	if (g_cfg.HideCorpses == 1 || g_cfg.HideCorpses == 2) {
		if (!g_entityCreatedListener && __s2sdk_OnEntityCreated_Register) {
			try {
				s2sdk::OnEntityCreated_Register(&OnEntityCreatedListener);
				g_entityCreatedListener = true;
			} catch (...) {
			}
		}
	} else if (g_entityCreatedListener) {
		ForgetCorpses(-1, 0);
		if (__s2sdk_OnEntityCreated_Unregister) {
			try {
				s2sdk::OnEntityCreated_Unregister(&OnEntityCreatedListener);
			} catch (...) {
			}
		}
		g_entityCreatedListener = false;
	}
}

void RegisterReloadCommand() {
	if (g_reloadCommandAttempted || !__s2sdk_AddConsoleCommand) {
		return;
	}
	g_reloadCommandAttempted = true;
	try {
		// AddConsoleCommand creates the command (owned = true) or only attaches this callback to an
		// already existing command (owned = false). RemoveCommand unregisters the engine command's
		// callbacks as well, so it may only be used for the command this plugin created; a foreign
		// owner's entry is detached with RemoveCommandListener (PITFALLS C11).
		g_reloadCommandOwned = s2sdk::AddConsoleCommand(plg::string("gm_reload"),
														plg::string("Reload the game_manager config"),
														s2sdk::ConVarFlag::None, &ReloadCommand,
														s2sdk::HookMode::Pre);
		if (g_reloadCommandOwned) {
			LogInfo("'gm_reload' registered");
		} else {
			LogInfo("'gm_reload' already exists - only the listener was attached");
		}
	} catch (...) {
		g_reloadCommandAttempted = false;
		g_reloadCommandOwned = false;
	}
}

void UnregisterReloadCommand() {
	if (!g_reloadCommandAttempted) {
		return;
	}
	const bool owned = g_reloadCommandOwned;
	g_reloadCommandAttempted = false;
	g_reloadCommandOwned = false;
	try {
		if (owned) {
			if (__s2sdk_RemoveCommand) {
				s2sdk::RemoveCommand(plg::string("gm_reload"), &ReloadCommand);
			}
		} else if (__s2sdk_RemoveCommandListener) {
			// The command belongs to someone else (or survived a previous load); never RemoveCommand.
			s2sdk::RemoveCommandListener(plg::string("gm_reload"), &ReloadCommand, s2sdk::HookMode::Pre);
		}
	} catch (...) {
	}
}

// --- plugin class ------------------------------------------------------------------------------

class GameManagerPlugin final : public plg::Plugin {
public:
	plg::PluginResult OnPluginStart() override {
		try {
			// A fresh start (including `plg reload`) begins with the map gate shut, even if the
			// static state survived an earlier start in the same process (tests).
			g_mapReady = false;
			g_mapStartSeen = false;
			g_mapEndSeen = false;
			g_mapListenersRegistered = false;
			g_userMessageResolutionPending = false;
			g_entityCreatedListener = false;
			g_globalCommandListener = false;
			g_reloadCommandAttempted = false;
			g_reloadCommandOwned = false;
			// A previous instance that did not get OnPluginEnd (a failed load in tests) must not
			// leave a live fade timer pointing into this library.
			ForgetCorpses(-1, 0);
			FlushTimerKills();
			g_corpseTimers.clear();
			ClearDeferredWork();

			LoadConfig();
			RegisterMapListeners();

			// The original's RegisterNeededHooks: the modes matter for the teardown below.
			RegisterEventHook("player_death", s2sdk::HookMode::Pre);
			RegisterEventHook("player_spawn", s2sdk::HookMode::Post);
			RegisterEventHook("player_team", s2sdk::HookMode::Pre);
			RegisterEventHook("player_disconnect", s2sdk::HookMode::Pre);
			RegisterEventHook("round_start", s2sdk::HookMode::Post);
			RegisterEventHook("bomb_planted", s2sdk::HookMode::Pre);
			RegisterEventHook("round_mvp", s2sdk::HookMode::Pre);

			// No user-message/schema/client call here: before the first map they crash s2sdk. The
			// first update with an open gate resolves and hooks them (SyncUserMessageHooks).
			SyncUserMessageHooks();
			SyncCorpseListener();
			SyncCommandListener();
			RegisterReloadCommand();

			ExecuteNativeCommands();
			LogInfo("loaded; map gate closed until the first update after a map start");
		} catch (const std::exception& ex) {
			LogError(std::string("OnPluginStart: ") + ex.what());
		} catch (...) {
			LogError("OnPluginStart failed");
		}
		return {}; // never Failed: one Failed stops every Plugify plugin on the server
	}

	plg::PluginResult OnPluginUpdate(std::chrono::milliseconds dt) override {
		(void)dt;
		try {
			FlushTimerKills();
			if (!g_mapReady) {
				// PITFALLS B1/B2: readiness is granted by the first update after OnMapStart, or by
				// the first update of a plugin loaded into a running map (no OnMapEnd seen and a
				// map name exists). After OnMapEnd only a new OnMapStart re-opens the gate.
				if (g_mapStartSeen) {
					g_mapReady = true;
				} else if (g_mapListenersRegistered && !g_mapEndSeen && MapAvailable()) {
					g_mapReady = true;
				}
			}
			if (!g_mapReady) {
				return {};
			}
			if (g_userMessageResolutionPending) {
				SyncUserMessageHooks();
			}
			DrainDeferredWork();
		} catch (const std::exception& ex) {
			LogError(std::string("OnPluginUpdate: ") + ex.what());
		} catch (...) {
			LogError("OnPluginUpdate failed with an unknown exception");
		}
		return {};
	}

	plg::PluginResult OnPluginEnd() override {
		try {
			g_mapReady = false;
			g_userMessageResolutionPending = false;

			// Drop our deferred work first: a lambda could still reference a corpse record.
			ClearDeferredWork();
			// Kill the fade timers and drop the records. This path is outside the timer callback,
			// so KillsTimer is called directly (the in-callback kill is deferred to the update).
			ForgetCorpses(-1, 0);
			FlushTimerKills();
			g_corpseTimers.clear();

			// Remove everything OnPluginStart (or gm_reload) registered, each with the exact key it
			// was registered with, before the library is unmapped.
			UnregisterMapListeners();
			UnregisterEventHooks();
			UnregisterUserMessageHooks();
			UnregisterReloadCommand();
			RemoveGlobalCommandListener();
			if (g_entityCreatedListener) {
				g_entityCreatedListener = false;
				if (__s2sdk_OnEntityCreated_Unregister) {
					try {
						s2sdk::OnEntityCreated_Unregister(&OnEntityCreatedListener);
					} catch (...) {
					}
				}
			}
		} catch (...) {
			// nothing left to do at unload
		}
		return {};
	}
};

GameManagerPlugin g_gameManagerPlugin;

} // namespace

PLUGIFY_PLUGIN(PLUGIN_API, &g_gameManagerPlugin)
