// game_manager engine test — no game: the plugin TU is compiled in (every handler lives in its
// anonymous namespace) and the __s2sdk_* binding globals are filled with recording fakes.
//
// Covered:
//   * lifecycle: the map gate stays shut before the first map / after OnMapEnd, opens on the first
//     update after OnMapStart (or for a plugin loaded mid-map), and the deferred queue/timers are
//     cleared at map end and unload (PITFALLS B1/B2/B4);
//   * no Pre game-event callback ever returns Handled/Stop; hiding uses SetEventBroadcast only;
//     player-typed event fields go through GetEventPlayerSlot, never GetEventInt;
//   * handle validation: 0 and -1 are rejected, valid negative CEntityHandle values are accepted;
//   * config parity with the checked-in production asset and with the written default file;
//   * commands: the global engine-command listener, gm_reload's source gate and the exact
//     AddConsoleCommand ownership (owned -> RemoveCommand, existing -> RemoveCommandListener);
//   * OnPluginEnd removes exactly what OnPluginStart/gm_reload registered, including map listeners,
//     user messages, the entity-created listener, commands, timers and deferred work.
//
// Run by ./test.sh (build/tests/game_manager_engine_tests).

#include "../src/plugin.cpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

int g_checks = 0;
int g_failures = 0;

bool IsPluginOk(const plg::PluginResult& result);

void remove_all_noexcept(const std::filesystem::path& path);
void Check(bool condition, const std::string& what) {
	++g_checks;
	if (!condition) {
		++g_failures;
		std::printf("FAIL: %s\n", what.c_str());
	}
}

bool IsPluginOk(const plg::PluginResult& result) { return static_cast<bool>(result); }

void remove_all_noexcept(const std::filesystem::path& path) {
	std::error_code ec;
	std::filesystem::remove_all(path, ec);
}

// --- recordings --------------------------------------------------------------------------------

struct HookRecord {
	std::string name;
	int mode = 0;
};

struct CommandRecord {
	std::string name;
	int mode = 0;
	bool owned = false;
};

std::vector<HookRecord> g_hookedEvents;
std::vector<HookRecord> g_unhookedEvents;
std::vector<HookRecord> g_hookedMessages;
std::vector<HookRecord> g_unhookedMessages;
std::vector<HookRecord> g_addedListeners;		  // explicit AddCommandListener calls
std::vector<HookRecord> g_removedListeners;		  // RemoveCommandListener calls (explicit or internal)
std::vector<CommandRecord> g_addedCommands;		  // AddConsoleCommand calls
std::vector<HookRecord> g_internalCommandListeners; // callbacks attached by AddConsoleCommand
std::vector<std::string> g_removedCommands;		  // RemoveCommand calls
std::vector<std::string> g_entityListeners;		  // created:register / created:unregister
std::vector<std::string> g_mapListeners;		  // mapstart:register / mapend:register / ...

std::vector<std::pair<void*, bool>> g_broadcastCalls;
std::vector<std::pair<void*, int>> g_fireToClientCalls;
std::vector<std::string> g_playedSounds;
std::vector<std::string> g_serverCommands;
std::vector<std::pair<int, unsigned char>> g_alphaWrites;
std::vector<std::pair<std::string, std::string>> g_playerSlotReads; // GetEventPlayerSlot(event, key)
std::vector<std::string> g_eventIntReads;							// GetEventInt(event, key)
std::vector<std::string> g_messageLookups;
std::set<int32_t> g_invalidHandles;
std::map<int, unsigned char> g_renderAlpha;
std::map<int32_t, std::string> g_entityClassnames;
std::map<int32_t, plg::vec3> g_entityOrigins;

struct SchemaWrite {
	int32_t handle = 0;
	std::string className;
	std::string member;
	int64_t value = 0;
	bool changeState = false;
};
std::vector<SchemaWrite> g_schemaWrites;

struct EngineTimerRecord {
	s2sdk::TimerId id = 0;
	double delay = 0.0;
	s2sdk::TimerCallback callback = nullptr;
	s2sdk::TimerFlag flags = s2sdk::TimerFlag::Default;
};
std::vector<EngineTimerRecord> g_timers;
std::vector<uint32_t> g_killedTimers;
uint32_t g_nextTimerId = 1;
bool g_failTimerCreation = false;
s2sdk::TimerId g_timerFailureId = 0;

std::vector<std::pair<std::string, plg::Severity>> g_logs;

s2sdk::OnEntityCreatedCallback g_entityCreatedCallback = nullptr;
s2sdk::OnMapStartCallback g_mapStartCallback = nullptr;
s2sdk::OnMapEndCallback g_mapEndCallback = nullptr;

// --- fake world state --------------------------------------------------------------------------

constexpr int kSlots = 64;
constexpr int kControllerIndexBase = 100;
constexpr int kPawnIndexBase = 200;

bool g_connected[kSlots] = {};
bool g_inGame[kSlots] = {};
bool g_sourceTV[kSlots] = {};
bool g_fakeAlive[kSlots] = {};
bool g_fakeBots[kSlots] = {};
bool g_controllerPresent[kSlots] = {};
bool g_pawnPresent[kSlots] = {};
uint64_t g_fakeSteamIds[kSlots] = {};
float g_gameTime = 100.0f;
uint32_t g_handleSerial = 1;
std::string g_currentMap = "de_dust2";

int g_clientQueries = 0;
int g_entityQueries = 0;

// spec_freeze_deathanim_time: the fake convar exists and answers g_fakeSpecFreeze.
float g_fakeSpecFreeze = 0.5f;

bool g_addConsoleCommandCreates = true;
bool g_messageHookSucceeds = true;

std::map<std::string, int16_t> g_messageIds = {
	{"WorldDecal", 411}, {"EffectDispatch", 400}, {"TextMsg", 124}, {"HintText", 323}, {"RadioText", 322}};

std::string ToPlgText(const plg::string& text) { return std::string(text.data(), text.size()); }
plg::string ToPlg(const std::string& text) { return plg::string(text.c_str(), text.size()); }

void* ControllerOf(int slot) { return reinterpret_cast<void*>(static_cast<uintptr_t>(0x10000 + slot * 8)); }
void* PawnOf(int slot) { return reinterpret_cast<void*>(static_cast<uintptr_t>(0x20000 + slot * 8)); }
int32_t EntHandleOfIndex(int32_t index) { return static_cast<int32_t>(static_cast<uint32_t>(index) | (g_handleSerial << 15)); }

// A game event as the s2sdk callbacks see it.
struct FakeEvent {
	uint32_t magic = 0x676D4576; // "gmEv"
	int victim = -1;			 // "userid"
	int attacker = -1;			 // "attacker"
};

FakeEvent* AsEvent(void* event) {
	auto* fake = static_cast<FakeEvent*>(event);
	if (fake == nullptr || fake->magic != 0x676D4576) {
		++g_checks;
		++g_failures;
		std::printf("FAIL: a binding was called with a non-fake event pointer\n");
		return nullptr;
	}
	return fake;
}

// --- fake bindings: events ---------------------------------------------------------------------

s2sdk::EventHookError FakeHookEvent(const plg::string& name, s2sdk::EventCallback, s2sdk::HookMode mode) {
	g_hookedEvents.push_back({ToPlgText(name), static_cast<int>(mode)});
	return s2sdk::EventHookError::Okay;
}
s2sdk::EventHookError FakeUnhookEvent(const plg::string& name, s2sdk::EventCallback, s2sdk::HookMode mode) {
	g_unhookedEvents.push_back({ToPlgText(name), static_cast<int>(mode)});
	return s2sdk::EventHookError::Okay;
}
void FakeSetEventBroadcast(void* event, bool dontBroadcast) { g_broadcastCalls.emplace_back(event, dontBroadcast); }
void FakeFireEventToClient(void* event, int32_t slot) { g_fireToClientCalls.emplace_back(event, slot); }
int32_t FakeGetEventPlayerSlot(void* event, const plg::string& key) {
	if (AsEvent(event) == nullptr) {
		return -1;
	}
	const std::string keyName = ToPlgText(key);
	g_playerSlotReads.emplace_back("player_death", keyName);
	if (keyName == "userid") return static_cast<FakeEvent*>(event)->victim;
	if (keyName == "attacker") return static_cast<FakeEvent*>(event)->attacker;
	return -1;
}
int32_t FakeGetEventInt(void* event, const plg::string& key) {
	AsEvent(event);
	g_eventIntReads.push_back(ToPlgText(key));
	return 0;
}
void FakeSetEventInt(void* event, const plg::string&, int32_t) { AsEvent(event); }

// --- fake bindings: map / entity listeners -----------------------------------------------------

void FakeOnMapStartRegister(s2sdk::OnMapStartCallback callback) {
	g_mapListeners.push_back("mapstart:register");
	g_mapStartCallback = callback;
}
void FakeOnMapStartUnregister(s2sdk::OnMapStartCallback) {
	g_mapListeners.push_back("mapstart:unregister");
	g_mapStartCallback = nullptr;
}
void FakeOnMapEndRegister(s2sdk::OnMapEndCallback callback) {
	g_mapListeners.push_back("mapend:register");
	g_mapEndCallback = callback;
}
void FakeOnMapEndUnregister(s2sdk::OnMapEndCallback) {
	g_mapListeners.push_back("mapend:unregister");
	g_mapEndCallback = nullptr;
}
void FakeOnEntityCreatedRegister(s2sdk::OnEntityCreatedCallback callback) {
	g_entityListeners.push_back("created:register");
	g_entityCreatedCallback = callback;
}
void FakeOnEntityCreatedUnregister(s2sdk::OnEntityCreatedCallback) {
	g_entityListeners.push_back("created:unregister");
	g_entityCreatedCallback = nullptr;
}

// --- fake bindings: clients and entities -------------------------------------------------------

uint64_t FakeGetClientSteamID64(int32_t slot) {
	++g_clientQueries;
	return slot >= 0 && slot < kSlots ? g_fakeSteamIds[slot] : 0;
}
void* FakePlayerSlotToEntPointer(int32_t slot) {
	++g_clientQueries;
	if (slot < 0 || slot >= kSlots || !g_connected[slot] || !g_controllerPresent[slot]) {
		return nullptr;
	}
	return ControllerOf(slot);
}
int32_t FakePlayerSlotToEntHandle(int32_t slot) {
	++g_clientQueries;
	if (slot < 0 || slot >= kSlots || !g_connected[slot] || !g_controllerPresent[slot]) {
		return -1;
	}
	return EntHandleOfIndex(kControllerIndexBase + slot);
}
void* FakeGetClientPawn(int32_t slot) {
	++g_clientQueries;
	if (slot < 0 || slot >= kSlots || !g_connected[slot] || !g_controllerPresent[slot] || !g_pawnPresent[slot]) {
		return nullptr;
	}
	return PawnOf(slot);
}
bool FakeIsClientConnected(int32_t slot) {
	++g_clientQueries;
	return slot >= 0 && slot < kSlots && g_connected[slot];
}
bool FakeIsClientInGame(int32_t slot) {
	++g_clientQueries;
	return slot >= 0 && slot < kSlots && g_connected[slot] && g_inGame[slot];
}
bool FakeIsClientSourceTV(int32_t slot) {
	++g_clientQueries;
	return slot >= 0 && slot < kSlots && g_sourceTV[slot];
}
bool FakeIsClientAlive(int32_t slot) {
	++g_clientQueries;
	return slot >= 0 && slot < kSlots && g_fakeAlive[slot];
}
bool FakeIsFakeClient(int32_t slot) {
	++g_clientQueries;
	return slot >= 0 && slot < kSlots && g_fakeBots[slot];
}
int32_t FakeGetMaxClients() { return kSlots; }

bool FakeIsValidEntHandle(int32_t handle) {
	++g_entityQueries;
	return gm::IsValidHandle(handle) && g_invalidHandles.count(handle) == 0;
}
bool FakeIsValidEntPointer(void* entity) {
	++g_entityQueries;
	const uintptr_t value = reinterpret_cast<uintptr_t>(entity);
	return (value >= 0x10000 && value < 0x10000 + static_cast<uintptr_t>(kSlots) * 8) ||
		   (value >= 0x20000 && value < 0x20000 + static_cast<uintptr_t>(kSlots) * 8);
}
int32_t FakeEntIndexToEntHandle(int32_t index) { return EntHandleOfIndex(index); }
int32_t FakeEntHandleToEntIndex(int32_t handle) { return handle & 0x7FFF; }
void* FakeEntIndexToEntPointer(int32_t index) {
	if (index >= kControllerIndexBase && index < kControllerIndexBase + kSlots) {
		return ControllerOf(index - kControllerIndexBase);
	}
	if (index >= kPawnIndexBase && index < kPawnIndexBase + kSlots) {
		return PawnOf(index - kPawnIndexBase);
	}
	return nullptr;
}
int32_t FakeEntPointerToEntIndex(void* pointer) {
	const uintptr_t value = reinterpret_cast<uintptr_t>(pointer);
	if (value >= 0x10000 && value < 0x10000 + static_cast<uintptr_t>(kSlots) * 8) {
		return kControllerIndexBase + static_cast<int32_t>((value - 0x10000) / 8);
	}
	if (value >= 0x20000 && value < 0x20000 + static_cast<uintptr_t>(kSlots) * 8) {
		return kPawnIndexBase + static_cast<int32_t>((value - 0x20000) / 8);
	}
	return -1;
}

plg::vec4 FakeGetEntityRenderColor(int32_t handle) {
	const auto it = g_renderAlpha.find(handle);
	const unsigned char alpha = it == g_renderAlpha.end() ? 255 : it->second;
	return plg::vec4{255.0f, 255.0f, 255.0f, static_cast<float>(alpha)};
}
void FakeSetEntityRenderColor(int32_t handle, const plg::vec4& color) {
	g_renderAlpha[handle] = static_cast<unsigned char>(color.w);
	g_alphaWrites.emplace_back(handle, static_cast<unsigned char>(color.w));
}
plg::string FakeGetEntityClassname(int32_t handle) {
	const auto it = g_entityClassnames.find(handle);
	return it == g_entityClassnames.end() ? plg::string("") : ToPlg(it->second);
}
plg::vec3 FakeGetEntityAbsOrigin(int32_t handle) {
	const auto it = g_entityOrigins.find(handle);
	return it == g_entityOrigins.end() ? plg::vec3{10.0f, 20.0f, 30.0f} : it->second;
}
void SetFakeEntity(int32_t handle, const plg::vec3& origin, const char* classname = "prop_ragdoll_attached") {
	g_entityClassnames[handle] = classname;
	g_entityOrigins[handle] = origin;
}

void FakeSetEntSchema(int32_t handle, const plg::string& className, const plg::string& member, int64_t value,
					  bool changeState, int32_t) {
	++g_entityQueries;
	g_schemaWrites.push_back({handle, ToPlgText(className), ToPlgText(member), value, changeState});
}
void FakeEmitSoundToClient(int32_t, const plg::vector<int32_t>&, const plg::string& sound, float, float) {
	g_playedSounds.push_back(ToPlgText(sound));
}
uint64_t FakeFindConVar(const plg::string& name) { return ToPlgText(name) == "spec_freeze_deathanim_time" ? 1 : 0; }
float FakeGetConVarFloat(uint64_t) { return g_fakeSpecFreeze; }
float FakeGetGameTime() { return g_gameTime; }
plg::string FakeGetCurrentMap() { return ToPlg(g_currentMap); }
s2sdk::TimerId FakeCreateTimer(double delay, s2sdk::TimerCallback callback, s2sdk::TimerFlag flags,
							   const plg::vector<plg::any>&) {
	if (g_failTimerCreation) {
		return g_timerFailureId;
	}
	EngineTimerRecord timer;
	timer.id = g_nextTimerId++;
	timer.delay = delay;
	timer.callback = callback;
	timer.flags = flags;
	g_timers.push_back(timer);
	return timer.id;
}
void FakeKillsTimer(uint32_t id) { g_killedTimers.push_back(id); }
void FakeServerCommand(const plg::string& command) { g_serverCommands.push_back(ToPlgText(command)); }

// --- fake bindings: user messages --------------------------------------------------------------

int16_t FakeFindMessageIdByName(const plg::string& name) {
	const std::string text = ToPlgText(name);
	g_messageLookups.push_back(text);
	const auto it = g_messageIds.find(text);
	return it == g_messageIds.end() ? 0 : it->second;
}
bool FakeHookUserMessage(int16_t id, s2sdk::UserMessageCallback, s2sdk::HookMode mode) {
	if (g_messageHookSucceeds) {
		g_hookedMessages.push_back({std::to_string(id), static_cast<int>(mode)});
	}
	return g_messageHookSucceeds;
}
bool FakeUnhookUserMessage(int16_t id, s2sdk::UserMessageCallback, s2sdk::HookMode mode) {
	g_unhookedMessages.push_back({std::to_string(id), static_cast<int>(mode)});
	return true;
}

struct FakeMessage {
	std::vector<std::string> params;
	std::string text;
	std::string name;
	bool hidden = false;
};
plg::string FakeUserMessageName(void* message) {
	return ToPlg(static_cast<FakeMessage*>(message)->name);
}
int32_t FakeRepeatedCount(void* message, const plg::string& field) {
	Check(ToPlgText(field) == "param", "TextMsg reads the repeated param field");
	return static_cast<int32_t>(static_cast<FakeMessage*>(message)->params.size());
}
plg::string FakePbReadString(void* message, const plg::string& field, int32_t index) {
	auto& msg = *static_cast<FakeMessage*>(message);
	if (ToPlgText(field) == "param") {
		Check(index >= 0 && static_cast<std::size_t>(index) < msg.params.size(), "TextMsg uses a valid repeated index");
		return ToPlg(msg.params.at(static_cast<std::size_t>(index)));
	}
	Check(index == -1 && (ToPlgText(field) == "message" || ToPlgText(field) == "msg_name"),
		  "HintText/RadioText use non-repeated index -1 and the original field name");
	return ToPlg(msg.text);
}
void FakeRemoveAllRecipients(void* message) { static_cast<FakeMessage*>(message)->hidden = true; }

// --- fake bindings: commands and logging -------------------------------------------------------

bool FakeAddCommandListener(const plg::string& name, s2sdk::ConCommandCallback, s2sdk::HookMode mode) {
	g_addedListeners.push_back({ToPlgText(name), static_cast<int>(mode)});
	return true;
}
bool FakeRemoveCommandListener(const plg::string& name, s2sdk::ConCommandCallback, s2sdk::HookMode mode) {
	g_removedListeners.push_back({ToPlgText(name), static_cast<int>(mode)});
	return true;
}
bool FakeAddConsoleCommand(const plg::string& name, const plg::string&, s2sdk::ConVarFlag, s2sdk::ConCommandCallback,
						   s2sdk::HookMode mode) {
	const bool owned = g_addConsoleCommandCreates;
	g_addedCommands.push_back({ToPlgText(name), static_cast<int>(mode), owned});
	// AddConsoleCommand always attaches the callback, even when the command already existed.
	g_internalCommandListeners.push_back({ToPlgText(name), static_cast<int>(mode)});
	return owned;
}
bool FakeRemoveCommand(const plg::string& name, s2sdk::ConCommandCallback) {
	g_removedCommands.push_back(ToPlgText(name));
	// RemoveCommand detaches both listener modes; the plugin only registers Pre.
	g_removedListeners.push_back({ToPlgText(name), static_cast<int>(s2sdk::HookMode::Pre)});
	return true;
}
void FakeLog(std::string_view message, plg::Severity severity, const plg::source_location&) {
	g_logs.emplace_back(std::string(message), severity);
}

std::string g_fakeConfigsRoot("/tmp/game_manager_engine_test_configs");
plg::string FakeGetConfigsDir() { return ToPlg(g_fakeConfigsRoot); }

// --- binding installation and teardown keys ----------------------------------------------------

void InstallBindings() {
	__s2sdk_HookEvent = &FakeHookEvent;
	__s2sdk_UnhookEvent = &FakeUnhookEvent;
	__s2sdk_SetEventBroadcast = &FakeSetEventBroadcast;
	__s2sdk_FireEventToClient = &FakeFireEventToClient;
	__s2sdk_GetEventPlayerSlot = &FakeGetEventPlayerSlot;
	__s2sdk_GetEventInt = &FakeGetEventInt;
	__s2sdk_SetEventInt = &FakeSetEventInt;

	__s2sdk_OnMapStart_Register = &FakeOnMapStartRegister;
	__s2sdk_OnMapStart_Unregister = &FakeOnMapStartUnregister;
	__s2sdk_OnMapEnd_Register = &FakeOnMapEndRegister;
	__s2sdk_OnMapEnd_Unregister = &FakeOnMapEndUnregister;
	__s2sdk_OnEntityCreated_Register = &FakeOnEntityCreatedRegister;
	__s2sdk_OnEntityCreated_Unregister = &FakeOnEntityCreatedUnregister;

	__s2sdk_GetClientSteamID64 = &FakeGetClientSteamID64;
	__s2sdk_PlayerSlotToEntPointer = &FakePlayerSlotToEntPointer;
	__s2sdk_PlayerSlotToEntHandle = &FakePlayerSlotToEntHandle;
	__s2sdk_GetClientPawn = &FakeGetClientPawn;
	__s2sdk_IsClientConnected = &FakeIsClientConnected;
	__s2sdk_IsClientInGame = &FakeIsClientInGame;
	__s2sdk_IsClientSourceTV = &FakeIsClientSourceTV;
	__s2sdk_IsClientAlive = &FakeIsClientAlive;
	__s2sdk_IsFakeClient = &FakeIsFakeClient;
	__s2sdk_GetMaxClients = &FakeGetMaxClients;

	__s2sdk_IsValidEntHandle = &FakeIsValidEntHandle;
	__s2sdk_IsValidEntPointer = &FakeIsValidEntPointer;
	__s2sdk_EntIndexToEntHandle = &FakeEntIndexToEntHandle;
	__s2sdk_EntHandleToEntIndex = &FakeEntHandleToEntIndex;
	__s2sdk_EntIndexToEntPointer = &FakeEntIndexToEntPointer;
	__s2sdk_EntPointerToEntIndex = &FakeEntPointerToEntIndex;
	__s2sdk_GetEntityRenderColor = &FakeGetEntityRenderColor;
	__s2sdk_SetEntityRenderColor = &FakeSetEntityRenderColor;
	__s2sdk_GetEntityClassname = &FakeGetEntityClassname;
	__s2sdk_GetEntityAbsOrigin = &FakeGetEntityAbsOrigin;
	__s2sdk_SetEntSchema = &FakeSetEntSchema;
	__s2sdk_EmitSoundToClient = &FakeEmitSoundToClient;
	__s2sdk_FindConVar = &FakeFindConVar;
	__s2sdk_GetConVarFloat = &FakeGetConVarFloat;
	__s2sdk_GetGameTime = &FakeGetGameTime;
	__s2sdk_GetCurrentMap = &FakeGetCurrentMap;
	__s2sdk_CreateTimer = &FakeCreateTimer;
	__s2sdk_KillsTimer = &FakeKillsTimer;
	__s2sdk_ServerCommand = &FakeServerCommand;

	__s2sdk_UserMessageFindMessageIdByName = &FakeFindMessageIdByName;
	__s2sdk_HookUserMessage = &FakeHookUserMessage;
	__s2sdk_UnhookUserMessage = &FakeUnhookUserMessage;
	__s2sdk_UserMessageGetMessageName = &FakeUserMessageName;
	__s2sdk_UserMessageGetRepeatedFieldCount = &FakeRepeatedCount;
	__s2sdk_PbReadString = &FakePbReadString;
	__s2sdk_UserMessageRemoveAllRecipient = &FakeRemoveAllRecipients;

	__s2sdk_AddCommandListener = &FakeAddCommandListener;
	__s2sdk_RemoveCommandListener = &FakeRemoveCommandListener;
	__s2sdk_AddConsoleCommand = &FakeAddConsoleCommand;
	__s2sdk_RemoveCommand = &FakeRemoveCommand;

	plg::Log = &FakeLog;
	plg::GetConfigsDir = &FakeGetConfigsDir;
}

std::vector<std::string> RegistrationKeys() {
	std::vector<std::string> keys;
	for (const HookRecord& hook : g_hookedEvents) keys.push_back("event:" + hook.name + ":" + std::to_string(hook.mode));
	for (const HookRecord& hook : g_hookedMessages) keys.push_back("message:" + hook.name + ":" + std::to_string(hook.mode));
	for (const HookRecord& hook : g_addedListeners) keys.push_back("listener:" + hook.name + ":" + std::to_string(hook.mode));
	for (const HookRecord& hook : g_internalCommandListeners) {
		keys.push_back("listener:" + hook.name + ":" + std::to_string(hook.mode));
	}
	for (const CommandRecord& command : g_addedCommands) {
		if (command.owned) keys.push_back("command:" + command.name);
	}
	for (const std::string& listener : g_entityListeners) {
		if (listener == "created:register") keys.push_back("entitycreated");
	}
	for (const std::string& listener : g_mapListeners) {
		if (listener == "mapstart:register") keys.push_back("mapstart");
		if (listener == "mapend:register") keys.push_back("mapend");
	}
	return keys;
}

std::vector<std::string> UnregistrationKeys() {
	std::vector<std::string> keys;
	for (const HookRecord& hook : g_unhookedEvents) keys.push_back("event:" + hook.name + ":" + std::to_string(hook.mode));
	for (const HookRecord& hook : g_unhookedMessages) keys.push_back("message:" + hook.name + ":" + std::to_string(hook.mode));
	for (const HookRecord& hook : g_removedListeners) keys.push_back("listener:" + hook.name + ":" + std::to_string(hook.mode));
	for (const std::string& command : g_removedCommands) keys.push_back("command:" + command);
	for (const std::string& listener : g_entityListeners) {
		if (listener == "created:unregister") keys.push_back("entitycreated");
	}
	for (const std::string& listener : g_mapListeners) {
		if (listener == "mapstart:unregister") keys.push_back("mapstart");
		if (listener == "mapend:unregister") keys.push_back("mapend");
	}
	return keys;
}

std::string Join(const std::vector<std::string>& values) {
	std::string out;
	for (const std::string& value : values) {
		if (!out.empty()) out += ", ";
		out += value;
	}
	return out;
}

void CheckTeardownIsExact(const std::string& scenario) {
	std::vector<std::string> registered = RegistrationKeys();
	std::vector<std::string> removed = UnregistrationKeys();
	std::sort(registered.begin(), registered.end());
	std::sort(removed.begin(), removed.end());
	Check(registered == removed, scenario + ": OnPluginEnd removed exactly what OnPluginStart registered"
									   "\n  registered: " + Join(registered) + "\n  removed:    " + Join(removed));
	Check(registered.size() >= 7, scenario + ": at least the seven event hooks were registered");
}

// --- record clearing and driving the fake host -------------------------------------------------

void ClearCallRecords() {
	g_broadcastCalls.clear();
	g_fireToClientCalls.clear();
	g_playedSounds.clear();
	g_serverCommands.clear();
	g_alphaWrites.clear();
	g_playerSlotReads.clear();
	g_eventIntReads.clear();
	g_schemaWrites.clear();
	g_timers.clear();
	g_killedTimers.clear();
	g_nextTimerId = 1;
	g_failTimerCreation = false;
	g_timerFailureId = 0;
	g_logs.clear();
	g_messageLookups.clear();
	g_invalidHandles.clear();
	g_renderAlpha.clear();
}

void ClearAllRecords() {
	ClearCallRecords();
	g_hookedEvents.clear();
	g_unhookedEvents.clear();
	g_hookedMessages.clear();
	g_unhookedMessages.clear();
	g_addedListeners.clear();
	g_removedListeners.clear();
	g_addedCommands.clear();
	g_internalCommandListeners.clear();
	g_removedCommands.clear();
	g_entityListeners.clear();
	g_mapListeners.clear();
	g_entityCreatedCallback = nullptr;
	g_mapStartCallback = nullptr;
	g_mapEndCallback = nullptr;
	g_entityClassnames.clear();
	g_entityOrigins.clear();
}

void ResetWorld() {
	for (int slot = 0; slot < kSlots; ++slot) {
		g_connected[slot] = false;
		g_inGame[slot] = false;
		g_sourceTV[slot] = false;
		g_fakeAlive[slot] = false;
		g_fakeBots[slot] = false;
		g_controllerPresent[slot] = false;
		g_pawnPresent[slot] = false;
		g_fakeSteamIds[slot] = 0;
	}
	g_gameTime = 100.0f;
	g_handleSerial = 1;
	g_fakeSpecFreeze = 0.5f;
	g_clientQueries = 0;
	g_entityQueries = 0;
	g_currentMap = "de_dust2";
	g_addConsoleCommandCreates = true;
	g_messageHookSucceeds = true;
	g_entityClassnames.clear();
	g_entityOrigins.clear();
	g_messageIds = {{"WorldDecal", 411}, {"EffectDispatch", 400}, {"TextMsg", 124}, {"HintText", 323}, {"RadioText", 322}};
}

void ConnectPlayers() {
	g_connected[2] = g_connected[3] = true; // victim, killer
	g_inGame[2] = g_inGame[3] = true;
	g_controllerPresent[2] = g_controllerPresent[3] = true;
	g_pawnPresent[2] = g_pawnPresent[3] = true;
	// A player_death arrives when the victim is already dead; the killer is alive.
	g_fakeAlive[2] = false;
	g_fakeAlive[3] = true;
}

void FireMapStart() {
	if (g_mapStartCallback) {
		g_mapStartCallback();
	}
}
void FireMapEnd() {
	if (g_mapEndCallback) {
		g_mapEndCallback();
	}
}
void RunUpdate(int milliseconds = 100) {
	g_gameManagerPlugin.OnPluginUpdate(std::chrono::milliseconds(milliseconds));
}
// Open the map gate the way the host does: OnMapStart followed by the first update.
void BeginMap() {
	FireMapStart();
	RunUpdate();
}

void WriteConfig(const std::string& text) {
	const std::filesystem::path dir = std::filesystem::path(g_fakeConfigsRoot) / "game_manager";
	std::filesystem::create_directories(dir);
	std::ofstream out(dir / "config.jsonc");
	out << text;
}

std::string ReadConfig() {
	std::ifstream in(std::filesystem::path(g_fakeConfigsRoot) / "game_manager" / "config.jsonc", std::ios::binary);
	std::ostringstream buffer;
	buffer << in.rdbuf();
	return buffer.str();
}

bool BroadcastHides(void* event) {
	for (const auto& call : g_broadcastCalls) {
		if (call.first == event) {
			return call.second;
		}
	}
	return false;
}
bool FiredToClient(void* event, int slot) {
	for (const auto& call : g_fireToClientCalls) {
		if (call.first == event && call.second == slot) {
			return true;
		}
	}
	return false;
}

bool TimerHasFlags(const EngineTimerRecord& timer, s2sdk::TimerFlag flag) {
	return (static_cast<int32_t>(timer.flags) & static_cast<int32_t>(flag)) != 0;
}

// --- tests: lifecycle --------------------------------------------------------------------------

void TestMapGateOpening() {
	std::printf("-- map gate (PITFALLS B1/B2)\n");

	ResetWorld();
	g_currentMap.clear();
	ClearAllRecords();
	remove_all_noexcept(std::filesystem::path(g_fakeConfigsRoot) / "game_manager");

	Check(IsPluginOk(g_gameManagerPlugin.OnPluginStart()), "start before the first map succeeds");
	Check(!g_mapReady, "start keeps the gate shut");
	Check(g_clientQueries == 0 && g_entityQueries == 0, "no client/entity call from OnPluginStart");
	Check(g_messageLookups.empty(), "user-message names are not resolved before a map");

	FireMapStart();
	Check(!g_mapReady, "OnMapStart only arms the gate; the first update opens it");
	RunUpdate();
	Check(g_mapReady, "the first update after OnMapStart opens the gate");

	FireMapEnd();
	Check(!g_mapReady, "OnMapEnd closes the gate immediately");
	RunUpdate();
	Check(!g_mapReady, "an update during the changelevel does not re-open the gate");

	FireMapStart();
	Check(!g_mapReady, "the next OnMapStart is still only an arm");
	RunUpdate();
	Check(g_mapReady, "the update after the next map opens the gate again");

	Check(IsPluginOk(g_gameManagerPlugin.OnPluginEnd()), "end succeeds");
	CheckTeardownIsExact("map gate scenario");
}

void TestLatePluginLoadMidMap() {
	std::printf("-- map gate: plugin loaded mid-map\n");

	ResetWorld();
	g_currentMap = "de_dust2";
	ClearAllRecords();
	remove_all_noexcept(std::filesystem::path(g_fakeConfigsRoot) / "game_manager");

	Check(IsPluginOk(g_gameManagerPlugin.OnPluginStart()), "mid-map start succeeds");
	Check(!g_mapReady, "the gate starts shut even though a map exists");
	RunUpdate();
	Check(g_mapReady, "the first update proves the running map and opens the gate");
	FireMapEnd();
	Check(!g_mapReady, "map end closes it");
	RunUpdate();
	Check(!g_mapReady, "no update re-opens it without a new OnMapStart");
	FireMapStart();
	RunUpdate();
	Check(g_mapReady, "a new map start opens it again");

	Check(IsPluginOk(g_gameManagerPlugin.OnPluginEnd()), "end succeeds");
	CheckTeardownIsExact("mid-map load");
}

void TestMapEndDropsDeferredAndTimers() {
	std::printf("-- map end drops deferred work and fade timers\n");

	ResetWorld();
	ConnectPlayers();
	ClearAllRecords();
	WriteConfig(R"({"GameManager":{"HideCorpses":2}})");
	Check(IsPluginOk(g_gameManagerPlugin.OnPluginStart()), "start succeeds");
	BeginMap();

	FakeEvent death;
	death.victim = 2;
	death.attacker = 3;
	OnRawGameEvent(ToPlg("player_death"), &death, false);
	Check(g_timers.empty(), "the deferred pass has not run yet");
	RunUpdate(); // deferred pass -> timer
	Check(g_timers.size() == 1, "the fade timer started after the deferred pass");
	const uint32_t timerId = g_timers.front().id;
	FireMapEnd();
	Check(g_killedTimers.size() == 1 && g_killedTimers.front() == timerId, "map end kills the live fade timer");
	Check(g_corpses.empty(), "map end drops the death records");
	ClearCallRecords();

	// A deferred hide queued on the old map must not run on the next one.
	FireMapStart();
	RunUpdate();
	FakeEvent death2;
	death2.victim = 2;
	death2.attacker = 3;
	OnRawGameEvent(ToPlg("player_death"), &death2, false);
	Check(g_timers.empty(), "the deferred work is still queued");
	FireMapEnd(); // clears the queue before the deferred pass could run
	ClearCallRecords();
	FireMapStart();
	RunUpdate();
	Check(g_alphaWrites.empty() && g_timers.empty(), "deferred work from the old map does not run on the new map");

	Check(IsPluginOk(g_gameManagerPlugin.OnPluginEnd()), "end succeeds");
	CheckTeardownIsExact("map-end cleanup");
}

// --- tests: config -----------------------------------------------------------------------------

void TestDefaultConfigWriteAndParity() {
	std::printf("-- config: written defaults and production asset\n");

	ResetWorld();
	ClearAllRecords();
	remove_all_noexcept(std::filesystem::path(g_fakeConfigsRoot) / "game_manager");
	Check(IsPluginOk(g_gameManagerPlugin.OnPluginStart()), "start (no config file) succeeds");
	const std::string written = ReadConfig();
	Check(written.find("\"GameManager\"") != std::string::npos, "the default file uses the production layout");
	Check(written.find("DiagGloves") == std::string::npos, "the unported glove diagnostic is not written");
	Check(written.find("\"SoundsMuteMVPMusic\"") != std::string::npos, "the default file keeps every parsed option");
	Check(IsPluginOk(g_gameManagerPlugin.OnPluginEnd()), "end succeeds");
	CheckTeardownIsExact("default config write");

	ResetWorld();
	ClearAllRecords();
	WriteConfig(R"({
	  "GameManager": {
	    "BlockRadio": true,
	    "HideKillFeed": 2,
	    "HideCorpses": 1,
	    "IgnoreJoinTeamMessages": true
	  }
	})");
	Check(IsPluginOk(g_gameManagerPlugin.OnPluginStart()), "start (feature config) succeeds");
	Check(g_cfg.BlockRadio && g_cfg.HideKillFeed == 2 && g_cfg.HideCorpses == 1 && g_cfg.IgnoreJoinTeamMessages,
		  "the feature config is loaded");
	Check(!g_cfg.HideBlood && g_cfg.SoundsMuteMVPMusic == 0, "missing keys keep their defaults");
	Check(IsPluginOk(g_gameManagerPlugin.OnPluginEnd()), "end succeeds");
	CheckTeardownIsExact("feature config");

#ifdef GM_PRODUCTION_CONFIG_PATH
	ResetWorld();
	ClearAllRecords();
	{
		std::ifstream in(GM_PRODUCTION_CONFIG_PATH, std::ios::binary);
		std::ostringstream buffer;
		buffer << in.rdbuf();
		WriteConfig(buffer.str());
	}
	Check(IsPluginOk(g_gameManagerPlugin.OnPluginStart()), "start (production asset) succeeds");
	Check(g_cfg.HideBlood && g_cfg.HideHeadshotSparks && g_cfg.HideCorpses == 2 && g_cfg.HideKillFeed == 0,
		  "the production asset parses into the expected options");
	Check(g_cfg.SoundsMuteMVPMusic == 0, "the production asset's missing MVP key keeps the default");
	Check(!g_cfg.BlockRadio && g_cfg.BlockedCommands.empty(), "the production command blocking is off");
	Check(IsPluginOk(g_gameManagerPlugin.OnPluginEnd()), "end succeeds");
	CheckTeardownIsExact("production asset");
#else
	std::printf("   (GM_PRODUCTION_CONFIG_PATH is not defined - production asset skipped)\n");
#endif
}

// --- tests: events -----------------------------------------------------------------------------

void TestEventHookModesAndPreContract() {
	std::printf("-- event hooks: exact modes and Pre Continue contract\n");

	ResetWorld();
	ConnectPlayers();
	ClearAllRecords();
	WriteConfig(R"({"GameManager":{"HideKillFeed":1,"HideCorpses":1,"HideLegs":true,
		"IgnoreJoinTeamMessages":true,"IgnoreDisconnectMessages":true,
		"IgnoreBombPlantedHUDMessages":true,"SoundsMuteMVPMusic":1}})");
	Check(IsPluginOk(g_gameManagerPlugin.OnPluginStart()), "start succeeds");

	const std::vector<std::pair<std::string, s2sdk::HookMode>> expected = {
		{"player_death", s2sdk::HookMode::Pre},	 {"player_spawn", s2sdk::HookMode::Post},
		{"player_team", s2sdk::HookMode::Pre},		 {"player_disconnect", s2sdk::HookMode::Pre},
		{"round_start", s2sdk::HookMode::Post},		 {"bomb_planted", s2sdk::HookMode::Pre},
		{"round_mvp", s2sdk::HookMode::Pre}};
	for (const auto& [name, mode] : expected) {
		bool found = false;
		for (const HookRecord& hook : g_hookedEvents) {
			found = found || (hook.name == name && hook.mode == static_cast<int>(mode));
		}
		Check(found, "the " + name + " hook is registered with its original mode");
	}

	BeginMap();
	const char* events[] = {"player_death", "player_spawn", "player_team", "player_disconnect",
							"round_start", "bomb_planted", "round_mvp", "round_end", "weapon_fire"};
	for (const char* name : events) {
		FakeEvent event;
		event.victim = 2;
		event.attacker = 3;
		const s2sdk::ResultType result = OnRawGameEvent(ToPlg(name), &event, false);
		Check(result == s2sdk::ResultType::Continue, std::string("event ") + name + " answers Continue");
		Check(result != s2sdk::ResultType::Handled && result != s2sdk::ResultType::Stop,
			  std::string("event ") + name + " is neither Handled nor Stop");
	}
	Check(OnRawGameEvent(ToPlg("player_death"), nullptr, false) == s2sdk::ResultType::Continue,
		  "a null event answers Continue");
	Check(g_eventIntReads.empty(), "no player-typed event field is read through GetEventInt");
	Check(!g_playerSlotReads.empty(), "player-typed fields are read through GetEventPlayerSlot");

	Check(IsPluginOk(g_gameManagerPlugin.OnPluginEnd()), "end succeeds");
	CheckTeardownIsExact("event modes");
}

void TestDeathAndKillFeedPaths() {
	std::printf("-- player_death: corpses, kill feed and slot identity\n");

	ResetWorld();
	ConnectPlayers();
	ClearAllRecords();
	WriteConfig(R"({"GameManager":{"HideKillFeed":2,"HideCorpses":2}})");
	Check(IsPluginOk(g_gameManagerPlugin.OnPluginStart()), "start succeeds");
	BeginMap();

	FakeEvent death;
	death.victim = 2;
	death.attacker = 3;
	ClearCallRecords();
	Check(OnRawGameEvent(ToPlg("player_death"), &death, false) == s2sdk::ResultType::Continue,
		  "player_death (mode 2) answers Continue");
	Check(BroadcastHides(&death), "the kill is hidden from the clients");
	Check(FiredToClient(&death, 3), "the killer still receives the live event");
	Check(!FiredToClient(&death, 2), "the victim does not receive it");
	Check(g_eventIntReads.empty(), "userid/attacker are not read via GetEventInt");
	Check(g_playerSlotReads.size() >= 2, "userid/attacker come from GetEventPlayerSlot");
	RunUpdate(); // drain the deferred corpse work of this death
	ForgetCorpses(-1, 0);
	ClearCallRecords();

	// A bot killer has no kill feed to look at.
	g_fakeBots[3] = true;
	ClearCallRecords();
	OnRawGameEvent(ToPlg("player_death"), &death, false);
	Check(BroadcastHides(&death) && g_fireToClientCalls.empty(), "a bot killer hides the kill without a delivery");
	g_fakeBots[3] = false;
	RunUpdate();
	ForgetCorpses(-1, 0);
	ClearCallRecords();

	// HideKillFeed 1 hides the death from everyone and hands it to nobody.
	WriteConfig(R"({"GameManager":{"HideKillFeed":1}})");
	ReloadCommand(-1, s2sdk::ConCommandContext::Console, plg::vector<plg::string>{});
	ClearCallRecords();
	OnRawGameEvent(ToPlg("player_death"), &death, false);
	Check(BroadcastHides(&death) && g_fireToClientCalls.empty(), "HideKillFeed 1 hides the kill without a delivery");
	WriteConfig(R"({"GameManager":{"HideKillFeed":2,"HideCorpses":2}})");
	ReloadCommand(-1, s2sdk::ConCommandContext::Console, plg::vector<plg::string>{});
	ClearCallRecords();

	// A world death (no attacker) and a stale attacker slot are ignored entirely.
	FakeEvent worldKill;
	worldKill.victim = 2;
	ClearCallRecords();
	OnRawGameEvent(ToPlg("player_death"), &worldKill, false);
	Check(g_broadcastCalls.empty() && g_fireToClientCalls.empty(), "a world death is not hidden");
	Check(g_timers.empty() && g_alphaWrites.empty(), "a world death touches no corpse");

	FakeEvent staleKill;
	staleKill.victim = 2;
	staleKill.attacker = 9;
	ClearCallRecords();
	OnRawGameEvent(ToPlg("player_death"), &staleKill, false);
	Check(g_broadcastCalls.empty() && g_alphaWrites.empty(), "a death by a slot without a controller is ignored");
	ClearCallRecords();

	// The victim's own death starts the corpse work on the deferred pass.
	OnRawGameEvent(ToPlg("player_death"), &death, false);
	Check(g_timers.empty(), "mode 2 starts no timer inside the event");
	RunUpdate();
	Check(g_timers.size() == 1, "mode 2 starts one fade timer on the next frame");
	Check(TimerHasFlags(g_timers.front(), s2sdk::TimerFlag::Repeat) &&
			  TimerHasFlags(g_timers.front(), s2sdk::TimerFlag::NoMapChange),
		  "the fade timer is Repeat + NoMapChange");
	Check(g_timers.front().delay >= 1.0 / 64.0 && g_timers.front().delay <= 0.2,
		  "the fade timer steps every 0.1 s (never 0 - a 0-delay repeat timer hangs the server)");

	// A different player in the victim's slot must not cancel the fade.
	const uint64_t victimSteamId = 76561198000000001ULL;
	const uint64_t otherSteamId = 76561198000000002ULL;
	RunUpdate();
	ForgetCorpses(-1, 0);
	ClearCallRecords();
	g_fakeSteamIds[2] = victimSteamId;
	WriteConfig(R"({"GameManager":{"HideCorpses":2}})");
	ReloadCommand(-1, s2sdk::ConCommandContext::Console, plg::vector<plg::string>{});
	FakeEvent identityDeath;
	identityDeath.victim = 2;
	identityDeath.attacker = 3;
	OnRawGameEvent(ToPlg("player_death"), &identityDeath, false);
	RunUpdate();
	Check(g_corpses.size() == 1, "the fade track is live");
	const auto corpse = g_corpses.back();

	g_fakeSteamIds[2] = otherSteamId;
	g_fakeAlive[2] = true; // the slot was taken by a living player
	FakeEvent spawn;
	spawn.victim = 2;
	OnRawGameEvent(ToPlg("player_spawn"), &spawn, false);
	Check(g_corpses.size() == 1 && g_killedTimers.empty(), "another player's spawn does not cancel the victim's fade");
	Check(!VictimRespawned(*corpse), "a different SteamID in the slot is not the victim respawning");

	g_gameTime = 100.25f;
	Check(TickCorpseFade(corpse), "the fade keeps running after the slot was taken");
	Check(!g_alphaWrites.empty() && g_alphaWrites.back().second == 128, "and still dims the corpse");

	g_fakeSteamIds[2] = victimSteamId;
	Check(VictimRespawned(*corpse), "the victim's own respawn is recognised");
	ForgetCorpses(-1, 0);
	Check(g_killedTimers.size() == 1, "forgetting the corpse kills its fade timer");

	Check(IsPluginOk(g_gameManagerPlugin.OnPluginEnd()), "end succeeds");
	CheckTeardownIsExact("death paths");
}

void TestHideOnlyEvents() {
	std::printf("-- hide-only events and round_start/round_mvp\n");

	ResetWorld();
	ConnectPlayers();
	ClearAllRecords();
	WriteConfig(R"({"GameManager":{"IgnoreJoinTeamMessages":true,"IgnoreDisconnectMessages":true,
		"IgnoreBombPlantedHUDMessages":true,"SoundsMuteMVPMusic":1}})");
	Check(IsPluginOk(g_gameManagerPlugin.OnPluginStart()), "start succeeds");
	BeginMap();

	FakeEvent event;
	event.victim = 2;
	event.attacker = 3;
	for (const char* name : {"player_team", "player_disconnect", "bomb_planted"}) {
		ClearCallRecords();
		Check(OnRawGameEvent(ToPlg(name), &event, false) == s2sdk::ResultType::Continue,
			  std::string(name) + " answers Continue");
		Check(BroadcastHides(&event), std::string(name) + " is hidden with dont_broadcast");
	}

	g_cfg = gm::Config{};
	ClearCallRecords();
	OnRawGameEvent(ToPlg("player_team"), &event, false);
	OnRawGameEvent(ToPlg("player_disconnect"), &event, false);
	OnRawGameEvent(ToPlg("bomb_planted"), &event, false);
	Check(g_broadcastCalls.empty(), "with the options off no hide-only event is touched");

	// round_start re-applies the original's native convar toggles.
	g_cfg = gm::Config{};
	g_cfg.HideRadar = true;
	g_cfg.DisableSVCheats = true;
	ClearCallRecords();
	OnRawGameEvent(ToPlg("round_start"), &event, false);
	Check(g_serverCommands == std::vector<std::string>{"sv_disable_radar 1", "sv_cheats 0"},
		  "round_start runs ExecuteNativeCommands");

	// round_mvp mode 1 zeroes the MVP controller's music kit.
	g_cfg = gm::Config{};
	g_cfg.SoundsMuteMVPMusic = 1;
	ClearCallRecords();
	OnRawGameEvent(ToPlg("round_mvp"), &event, false);
	Check(g_schemaWrites.size() == 1, "mode 1 writes exactly one schema field");
	Check(!g_schemaWrites.empty() && g_schemaWrites[0].className == "CCSPlayerController" &&
			  g_schemaWrites[0].member == "m_iMusicKitID" && g_schemaWrites[0].value == 0 &&
			  g_schemaWrites[0].changeState,
		  "mode 1 zeroes m_iMusicKitID on the MVP's controller with a state change");
	Check(!g_schemaWrites.empty() && g_schemaWrites[0].handle == EntHandleOfIndex(kControllerIndexBase + 2),
		  "the write goes to the MVP's controller entity");
	Check(g_playedSounds.empty(), "mode 1 stops no other music");

	// Mode 2 stops all music for every connected slot.
	g_cfg.SoundsMuteMVPMusic = 2;
	ClearCallRecords();
	OnRawGameEvent(ToPlg("round_mvp"), &event, false);
	Check(g_playedSounds == std::vector<std::string>{"StopSoundEvents.StopAllMusic"},
		  "mode 2 stops all music for the connected players");
	Check(g_schemaWrites.empty(), "mode 2 touches no schema");

	// Bots are skipped in both modes.
	g_fakeBots[2] = true;
	g_cfg.SoundsMuteMVPMusic = 1;
	ClearCallRecords();
	OnRawGameEvent(ToPlg("round_mvp"), &event, false);
	Check(g_schemaWrites.empty() && g_playedSounds.empty(), "a bot MVP is skipped");
	g_fakeBots[2] = false;

	Check(IsPluginOk(g_gameManagerPlugin.OnPluginEnd()), "end succeeds");
	CheckTeardownIsExact("hide-only events");
}

void TestEventsBeforeMapAndAfterMapEndAreInert() {
	std::printf("-- events outside the map gate\n");

	ResetWorld();
	ConnectPlayers();
	ClearAllRecords();
	WriteConfig(R"({"GameManager":{"HideKillFeed":1}})");
	Check(IsPluginOk(g_gameManagerPlugin.OnPluginStart()), "start succeeds");
	FireMapStart(); // armed, not ready
	FakeEvent death;
	death.victim = 2;
	death.attacker = 3;
	ClearCallRecords();
	g_clientQueries = 0;
	OnRawGameEvent(ToPlg("player_death"), &death, false);
	Check(g_broadcastCalls.empty() && g_fireToClientCalls.empty(), "events before the first update do not hide");
	Check(g_clientQueries == 0, "events before the first update do not query a client");

	RunUpdate();
	ClearCallRecords();
	OnRawGameEvent(ToPlg("player_death"), &death, false);
	Check(BroadcastHides(&death), "the event path works once the gate is open");

	FireMapEnd();
	ClearCallRecords();
	g_clientQueries = 0;
	OnRawGameEvent(ToPlg("player_death"), &death, false);
	OnRawGameEvent(ToPlg("player_spawn"), &death, false);
	OnRawGameEvent(ToPlg("round_mvp"), &death, false);
	Check(g_clientQueries == 0 && g_broadcastCalls.empty(), "no client query and no hide in the map-end window");

	Check(IsPluginOk(g_gameManagerPlugin.OnPluginEnd()), "end succeeds");
	CheckTeardownIsExact("events outside the map gate");
}

// --- tests: commands ---------------------------------------------------------------------------

void TestCommandBlockingThroughTheGlobalListener() {
	std::printf("-- command blocking through the global listener\n");

	ResetWorld();
	ConnectPlayers();
	ClearAllRecords();
	WriteConfig(R"({"GameManager":{"BlockRadio":true,"BlockedCommands":["gm_custom"]}})");
	Check(IsPluginOk(g_gameManagerPlugin.OnPluginStart()), "start succeeds");
	BeginMap();
	Check(g_addedListeners.size() == 1 && g_addedListeners[0].name.empty(),
		  "one global command listener is registered (the radio commands are not ConCommands)");
	Check(g_addedListeners[0].mode == static_cast<int>(s2sdk::HookMode::Pre), "the listener is Pre");

	plg::vector<plg::string> args;
	args.push_back(ToPlg("coverme"));
	Check(OnClientCommand(3, s2sdk::ConCommandContext::Chat, args) == s2sdk::ResultType::Stop,
		  "a blocked radio command answers Stop");
	args[0] = ToPlg("CoverMe");
	Check(OnClientCommand(3, s2sdk::ConCommandContext::Chat, args) == s2sdk::ResultType::Continue,
		  "the match stays case-sensitive");
	args[0] = ToPlg("gm_custom");
	Check(OnClientCommand(3, s2sdk::ConCommandContext::Console, args) == s2sdk::ResultType::Stop,
		  "a BlockedCommands entry is blocked too");
	Check(OnClientCommand(-1, s2sdk::ConCommandContext::Console, args) == s2sdk::ResultType::Continue,
		  "the server console is never blocked");
	args[0] = ToPlg("say");
	Check(OnClientCommand(3, s2sdk::ConCommandContext::Console, args) == s2sdk::ResultType::Continue,
		  "unrelated commands pass through");

	g_cfg.BlockPing = true;
	g_cfg.BlockChatWheel = true;
	args[0] = ToPlg("player_ping");
	Check(OnClientCommand(3, s2sdk::ConCommandContext::Console, args) == s2sdk::ResultType::Stop,
		  "BlockPing blocks player_ping");
	args[0] = ToPlg("+radialradio2");
	Check(OnClientCommand(3, s2sdk::ConCommandContext::Console, args) == s2sdk::ResultType::Stop,
		  "BlockChatWheel blocks the +radialradio prefix");
	args[0] = ToPlg("playerchatwheel");
	Check(OnClientCommand(3, s2sdk::ConCommandContext::Console, args) == s2sdk::ResultType::Stop,
		  "BlockChatWheel blocks playerchatwheel");
	args[0] = ToPlg("radialradio");
	Check(OnClientCommand(3, s2sdk::ConCommandContext::Console, args) == s2sdk::ResultType::Continue,
		  "a bare radialradio is not blocked");
	Check(OnClientCommand(3, s2sdk::ConCommandContext::Console, plg::vector<plg::string>{}) ==
			  s2sdk::ResultType::Continue,
		  "an empty argument list answers Continue");

	// Disabling command blocking through a reload removes the global listener.
	WriteConfig(R"({"GameManager":{"BlockRadio":false}})");
	ClearCallRecords();
	ReloadCommand(-1, s2sdk::ConCommandContext::Console, plg::vector<plg::string>{});
	Check(g_removedListeners.size() == 1 && g_removedListeners[0].name.empty(), "the listener is removed when blocking is off");

	Check(IsPluginOk(g_gameManagerPlugin.OnPluginEnd()), "end succeeds");
	CheckTeardownIsExact("command listener");
}

void TestReloadCommandOwnershipAndSource() {
	std::printf("-- gm_reload: source gate and AddConsoleCommand ownership\n");

	// Attempted + owned: the plugin created the command, so teardown uses RemoveCommand.
	ResetWorld();
	ClearAllRecords();
	remove_all_noexcept(std::filesystem::path(g_fakeConfigsRoot) / "game_manager");
	g_addConsoleCommandCreates = true;
	Check(IsPluginOk(g_gameManagerPlugin.OnPluginStart()), "start (owned gm_reload) succeeds");
	Check(g_addedCommands.size() == 1 && g_addedCommands[0].name == "gm_reload" && g_addedCommands[0].owned,
		  "gm_reload was created by the plugin");
	Check(IsPluginOk(g_gameManagerPlugin.OnPluginEnd()), "end succeeds");
	Check(g_removedCommands == std::vector<std::string>{"gm_reload"}, "an owned command is removed with RemoveCommand");
	CheckTeardownIsExact("owned gm_reload");

	// Attempted but not owned: only the listener may be detached; the command belongs to someone else.
	ResetWorld();
	ClearAllRecords();
	g_addConsoleCommandCreates = false;
	Check(IsPluginOk(g_gameManagerPlugin.OnPluginStart()), "start (existing gm_reload) succeeds");
	Check(g_addedCommands.size() == 1 && !g_addedCommands[0].owned, "gm_reload already existed");
	Check(IsPluginOk(g_gameManagerPlugin.OnPluginEnd()), "end succeeds");
	Check(g_removedCommands.empty(), "RemoveCommand is never used on a command the plugin did not create");
	bool detached = false;
	for (const HookRecord& hook : g_removedListeners) {
		detached = detached || (hook.name == "gm_reload" && hook.mode == static_cast<int>(s2sdk::HookMode::Pre));
	}
	Check(detached, "the existing command is detached with RemoveCommandListener(Pre)");
	CheckTeardownIsExact("existing gm_reload");
	g_addConsoleCommandCreates = true;

	// A client's !gm_reload is refused; the server console reloads and re-syncs the message hooks.
	ResetWorld();
	ConnectPlayers();
	ClearAllRecords();
	WriteConfig(R"({"GameManager":{"HideBlood":true}})");
	Check(IsPluginOk(g_gameManagerPlugin.OnPluginStart()), "start (reload test) succeeds");
	BeginMap();
	const std::size_t hookedBefore = g_hookedMessages.size();
	const std::size_t unhookedBefore = g_unhookedMessages.size();
	Check(ReloadCommand(3, s2sdk::ConCommandContext::Chat, plg::vector<plg::string>{}) ==
			  s2sdk::ResultType::Continue,
		  "a client's !gm_reload answers Continue");
	Check(g_hookedMessages.size() == hookedBefore && g_unhookedMessages.size() == unhookedBefore,
		  "a client's !gm_reload cannot reload the config or churn hooks");

	WriteConfig(R"({"GameManager":{"HideBlood":false,"IgnoreJoinTeamMessages":true}})");
	const std::size_t hookedBeforeReload = g_hookedMessages.size();
	ClearCallRecords();
	Check(ReloadCommand(-1, s2sdk::ConCommandContext::Console, plg::vector<plg::string>{}) ==
			  s2sdk::ResultType::Continue,
		  "the server console reload answers Continue");
	Check(g_unhookedMessages.size() == 1 && g_hookedMessages.size() == hookedBeforeReload,
		  "the reload unhooks the old WorldDecal filter and registers no replacement");
	Check(g_cfg.IgnoreJoinTeamMessages && !g_cfg.HideBlood, "the reload re-read the config");

	Check(IsPluginOk(g_gameManagerPlugin.OnPluginEnd()), "end succeeds");
	CheckTeardownIsExact("gm_reload source");
}

void TestNativeCommands() {
	std::printf("-- all native command toggles\n");

	g_cfg = gm::Config{};
	ClearCallRecords();
	ExecuteNativeCommands();
	Check(g_serverCommands.empty(), "defaults issue no convar commands");

	g_cfg.BlockBotRadio = g_cfg.BlockGrenadesRadio = g_cfg.HideRadar = true;
	g_cfg.DisableFallDamage = g_cfg.DisableSVCheats = g_cfg.DisableC4 = g_cfg.DisableCameraSpectator = true;
	g_cfg.SoundsMuteFootsteps = g_cfg.SoundsMuteJumpLand = true;
	g_cfg.HideTeammateHeadtags = 3;
	g_cfg.HideTeammateHeadtagsDistance = 150;
	ClearCallRecords();
	ExecuteNativeCommands();
	const std::vector<std::string> expected = {
		"bot_chatter off", "sv_ignoregrenaderadio 1", "sv_disable_radar 1",
		"sv_teamid_overhead 1; sv_teamid_overhead_always_prohibit 1; sv_teamid_overhead_maxdist 150",
		"sv_falldamage_scale 0", "sv_cheats 0", "mp_give_player_c4 0",
		"sv_disable_observer_interpolation true", "sv_footsteps 0", "sv_min_jump_landing_sound 999999"};
	Check(g_serverCommands == expected, "every enabled native toggle matches Config.cs exactly");

	g_cfg = gm::Config{};
	g_cfg.HideTeammateHeadtags = 1;
	ClearCallRecords();
	ExecuteNativeCommands();
	Check(g_serverCommands == std::vector<std::string>{"sv_teamid_overhead 0"}, "headtag mode 1 command");

	g_cfg.HideTeammateHeadtags = 2;
	ClearCallRecords();
	ExecuteNativeCommands();
	Check(g_serverCommands ==
			  std::vector<std::string>{"sv_teamid_overhead 1; sv_teamid_overhead_always_prohibit 1; sv_teamid_overhead_maxdist 0"},
		  "headtag mode 2 command");

	g_cfg.HideTeammateHeadtags = 3;
	ClearCallRecords();
	ExecuteNativeCommands();
	Check(g_serverCommands.empty(), "headtag mode 3 distance 0 issues no command");
}

// --- tests: corpses ----------------------------------------------------------------------------

void TestCorpseModesAndHandles() {
	std::printf("-- corpses: modes, handles, attached props\n");

	ResetWorld();
	ConnectPlayers();
	ClearAllRecords();
	WriteConfig(R"({"GameManager":{"HideCorpses":1}})");
	Check(IsPluginOk(g_gameManagerPlugin.OnPluginStart()), "start (mode 1) succeeds");
	BeginMap();

	FakeEvent death;
	death.victim = 2;
	death.attacker = 3;
	ClearCallRecords();
	OnRawGameEvent(ToPlg("player_death"), &death, false);
	Check(g_alphaWrites.empty(), "mode 1 writes nothing inside the death event (deferred to the next frame)");
	RunUpdate();
	Check(!g_alphaWrites.empty() && g_alphaWrites.back().second == 0, "mode 1 hides the pawn on the next frame");
	Check(g_timers.empty(), "mode 1 does not fade (no timer)");

	// The attached ragdoll is hidden at creation; mode 1 remembers the handle for the deferred pass.
	Check(g_entityCreatedCallback != nullptr, "the entity-created listener is captured");
	const int32_t farProp = EntHandleOfIndex(kPawnIndexBase + 20);
	SetFakeEntity(farProp, {500.0f, 0.0f, 0.0f});
	ClearCallRecords();
	g_entityCreatedCallback(farProp);
	Check(g_alphaWrites.empty(), "a corpse prop without a matching death is untouched");

	const int32_t nearProp = EntHandleOfIndex(kPawnIndexBase + 21);
	SetFakeEntity(nearProp, {10.0f, 20.0f, 30.0f});
	g_entityCreatedCallback(nearProp);
	Check(!g_alphaWrites.empty() && g_alphaWrites.back().first == nearProp && g_alphaWrites.back().second == 0,
		  "a corpse prop near the death is hidden at creation");

	// A plain prop_ragdoll is not a corpse; the match is case-insensitive.
	SetFakeEntity(nearProp, {10.0f, 20.0f, 30.0f}, "prop_ragdoll");
	ClearCallRecords();
	g_entityCreatedCallback(nearProp);
	Check(g_alphaWrites.empty(), "a plain prop_ragdoll is not treated as a corpse");
	SetFakeEntity(nearProp, {10.0f, 20.0f, 30.0f}, "PROP_RAGDOLL_ATTACHED");
	ClearCallRecords();
	g_renderAlpha.clear();
	g_entityCreatedCallback(nearProp);
	Check(!g_alphaWrites.empty(), "the class match ignores the case");

	// Invalid 0 / -1 handles never reach an entity call.
	ClearCallRecords();
	g_clientQueries = 0;
	g_entityQueries = 0;
	OnEntityCreatedListener(0);
	OnEntityCreatedListener(-1);
	Check(g_entityQueries == 0 && g_alphaWrites.empty(), "entity handles 0 and -1 are rejected before any entity call");

	// A valid negative handle (serial bit 31 set) is accepted.
	g_handleSerial = 0x10000;
	const int32_t negativePawn = EntHandleOfIndex(kPawnIndexBase + 2);
	Check(negativePawn < -1, "the fixture exercises a valid negative handle");
	g_cfg = gm::Config{};
	g_cfg.HideCorpses = 1;
	ClearCallRecords();
	g_renderAlpha.clear();
	OnRawGameEvent(ToPlg("player_death"), &death, false);
	RunUpdate();
	Check(!g_alphaWrites.empty() && g_alphaWrites.back().first == negativePawn && g_alphaWrites.back().second == 0,
		  "a negative valid pawn handle is hidden normally");

	g_cfg.HideLegs = true;
	FakeEvent spawn;
	spawn.victim = 2;
	OnRawGameEvent(ToPlg("player_spawn"), &spawn, false);
	Check(!g_alphaWrites.empty() && g_alphaWrites.back().second == 254,
		  "HideLegs accepts a negative valid pawn handle");

	// 0 / -1 pawn handles never reach the render setters either.
	ClearCallRecords();
	SetEntityAlpha(0, 0);
	SetEntityAlpha(-1, 0);
	Check(g_alphaWrites.empty(), "SetEntityAlpha rejects 0 and -1");
	g_handleSerial = 1;

	Check(IsPluginOk(g_gameManagerPlugin.OnPluginEnd()), "end succeeds");
	CheckTeardownIsExact("corpse modes and handles");
}

void TestCorpseFadeTimerCreationFailure() {
	std::printf("-- corpse fade timer creation failure\n");
	ResetWorld();
	ConnectPlayers();
	ClearAllRecords();
	WriteConfig(R"({"GameManager":{"HideCorpses":2}})");
	Check(IsPluginOk(g_gameManagerPlugin.OnPluginStart()), "start for timer failure succeeds");
	BeginMap();

	FakeEvent death;
	death.victim = 2;
	death.attacker = 3;
	for (const s2sdk::TimerId failedId : {0U, static_cast<s2sdk::TimerId>(-1)}) {
		ClearCallRecords();
		g_failTimerCreation = true;
		g_timerFailureId = failedId;
		OnRawGameEvent(ToPlg("player_death"), &death, false);
		RunUpdate();
		Check(g_timers.empty(), "failed timer creation leaves no engine timer");
		Check(g_corpseTimers.empty(), "failed timer creation leaves no timer lookup entry");
		Check(g_corpses.size() == 1 && g_corpses.front()->timer == kNoTimer,
			  "failed timer creation leaves the corpse timer unassigned");
		Check(!g_alphaWrites.empty() && g_alphaWrites.back().second == 0,
			  "failed timer creation immediately hides the corpse");
		ForgetCorpses(-1, 0);
		Check(g_killedTimers.empty(), "cleanup never kills a failed timer id");
	}
	g_failTimerCreation = false;
	Check(IsPluginOk(g_gameManagerPlugin.OnPluginEnd()), "end after timer failure succeeds");
	CheckTeardownIsExact("corpse fade timer creation failure");
}

void TestCorpseFadeTimerLifecycle() {
	std::printf("-- corpse fade timer lifecycle\n");

	ResetWorld();
	ConnectPlayers();
	ClearAllRecords();
	WriteConfig(R"({"GameManager":{"HideCorpses":2}})");
	Check(IsPluginOk(g_gameManagerPlugin.OnPluginStart()), "start (mode 2) succeeds");
	BeginMap();

	FakeEvent death;
	death.victim = 2;
	death.attacker = 3;
	ClearCallRecords();
	OnRawGameEvent(ToPlg("player_death"), &death, false);
	RunUpdate();
	Check(g_timers.size() == 1, "mode 2 starts one fade timer on the deferred pass");
	const EngineTimerRecord timer = g_timers.front();

	g_gameTime = 100.25f; // half of the 0.5 s convar fade
	Check(timer.callback != nullptr, "the timer has a callback");
	timer.callback(timer.id, plg::vector<plg::any>{});
	Check(!g_alphaWrites.empty() && g_alphaWrites.back().second == 128, "half the fade leaves the corpse half transparent");

	g_gameTime = 101.0f; // past the whole fade
	ClearCallRecords();
	timer.callback(timer.id, plg::vector<plg::any>{});
	Check(!g_alphaWrites.empty() && g_alphaWrites.back().second == 0, "the end of the fade writes alpha 0");
	RunUpdate(); // the timer kill is deferred out of the engine callback
	Check(g_killedTimers == std::vector<uint32_t>{timer.id}, "the timer is killed once the fade reaches 0");
	g_gameTime = 100.0f;

	// A respawn cancels the live fade and restores the pawn; the timer is killed.
	g_timers.clear();
	g_killedTimers.clear();
	ClearCallRecords();
	OnRawGameEvent(ToPlg("player_death"), &death, false);
	RunUpdate();
	Check(g_timers.size() == 1, "a second death starts a new fade timer");
	const uint32_t secondId = g_timers.front().id;
	ClearCallRecords();
	g_renderAlpha[EntHandleOfIndex(kPawnIndexBase + 2)] = 128; // a mid-fade corpse
	FakeEvent spawn;
	spawn.victim = 2;
	OnRawGameEvent(ToPlg("player_spawn"), &spawn, false);
	Check(!g_alphaWrites.empty() && g_alphaWrites.back().second == 255, "player_spawn restores the pawn alpha");
	Check(g_killedTimers == std::vector<uint32_t>{secondId}, "player_spawn kills the corpse fade timer");

	// A removed pawn stops the fade instead of polling a stale handle.
	g_timers.clear();
	g_killedTimers.clear();
	ClearCallRecords();
	OnRawGameEvent(ToPlg("player_death"), &death, false);
	RunUpdate();
	Check(g_corpses.size() == 1, "a live corpse record exists");
	const auto fading = g_corpses.back();
	ClearCallRecords();
	g_invalidHandles.insert(fading->pawnHandle);
	Check(!TickCorpseFade(fading), "an invalidated pawn ends the fade");
	Check(g_alphaWrites.empty(), "no render call is made for a stale handle");
	g_invalidHandles.clear();

	// Unload kills a live timer (the plg-reload contract).
	ClearCallRecords();
	ForgetCorpses(-1, 0);
	ClearCallRecords();
	OnRawGameEvent(ToPlg("player_death"), &death, false);
	RunUpdate();
	Check(g_timers.size() == 1, "the leaving death starts its fade");
	const uint32_t leavingId = g_timers.front().id;
	Check(IsPluginOk(g_gameManagerPlugin.OnPluginEnd()), "end succeeds");
	Check(g_killedTimers == std::vector<uint32_t>{leavingId}, "the live corpse fade timer is killed at unload");
	CheckTeardownIsExact("fade timer lifecycle");
}

// --- tests: user messages ----------------------------------------------------------------------

void TestUserMessageResolutionAndFilters() {
	std::printf("-- user messages: lazy resolution, filters, reload\n");

	ResetWorld();
	ConnectPlayers();
	ClearAllRecords();
	WriteConfig(R"({"GameManager":{"HideBlood":true,"HideHeadshotSparks":true,
		"IgnoreAwardsMoneyMessages":true,"IgnoreTeammateAttackMessages":true,
		"IgnorePlantingBombMessages":true}})");
	Check(IsPluginOk(g_gameManagerPlugin.OnPluginStart()), "start succeeds");
	Check(g_messageLookups.empty() && g_hookedMessages.empty(), "no user-message name is resolved before a map");
	BeginMap();
	Check(g_hookedMessages.size() == 5, "all five enabled filters are hooked once the map is up");
	Check(g_messageLookups.size() == 5, "short registry names resolve first");

	// TextMsg: every repeated param is tested.
	FakeMessage msg;
	msg.name = "TextMsg";
	msg.params = {"unrelated", "#Player_Cash_Award_Killed_Enemy"};
	Check(OnTextMsg(&msg) == s2sdk::ResultType::Stop && msg.hidden, "a money TextMsg is hidden");
	msg.hidden = false;
	msg.params = {"unrelated"};
	Check(OnTextMsg(&msg) == s2sdk::ResultType::Continue && !msg.hidden, "an unmatched TextMsg stays visible");

	// HintText/RadioText: non-repeated index -1.
	g_cfg.IgnoreTeammateAttackMessages = true;
	msg.name = "HintText";
	msg.text = "#Cstrike_TitlesTXT_Killed_Teammate";
	Check(OnHintText(&msg) == s2sdk::ResultType::Stop && msg.hidden, "the teammate hint is hidden");
	msg.hidden = false;
	msg.name = "RadioText";
	msg.text = "prefix #Cstrike_TitlesTXT_Planting_Bomb suffix";
	Check(OnRadioText(&msg) == s2sdk::ResultType::Stop && msg.hidden, "radio text uses substring matching");

	// A hook id delivering another type warns once per type and is ignored.
	g_warnedMessages.clear();
	g_logs.clear();
	std::vector<FakeMessage> mismatched(3);
	for (FakeMessage& message : mismatched) {
		message.name = "CMsgTEEffectDispatch";
		Check(!UserMessageMatches(&message, "WorldDecal"), "a message of another type is not processed");
	}
	Check(g_logs.size() == 1 && g_logs[0].second == plg::Severity::Warning,
		  "the type-mismatch warning is logged once per type, not per message object");

	// A map start re-syncs the hooks: the old set is unhooked first.
	const std::size_t unhookedBefore = g_unhookedMessages.size();
	FireMapEnd();
	FireMapStart();
	RunUpdate();
	Check(g_unhookedMessages.size() == unhookedBefore + 5, "a map start unhooks the previous message hooks");
	Check(g_hookedMessages.size() == 10, "and registers the same set again");

	Check(IsPluginOk(g_gameManagerPlugin.OnPluginEnd()), "end succeeds");
	CheckTeardownIsExact("user messages");
}

void TestMessageResolutionOrder() {
	std::printf("-- user-message registry names and enum fallback\n");

	ResetWorld();
	ConnectPlayers();
	ClearAllRecords();
	WriteConfig(R"({"GameManager":{"HideBlood":true,"HideHeadshotSparks":true,
		"IgnoreTeammateAttackMessages":true,"IgnorePlantingBombMessages":true,
		"IgnoreAwardsMoneyMessages":true}})");
	Check(IsPluginOk(g_gameManagerPlugin.OnPluginStart()), "start succeeds");
	BeginMap();

	const auto shortIds = g_messageIds;
	const std::vector<std::string> shorts = {"WorldDecal", "EffectDispatch", "TextMsg", "HintText", "RadioText"};
	const std::vector<std::string> protos = {"CMsgTEWorldDecal", "CMsgTEEffectDispatch", "CUserMessageTextMsg",
		"CCSUsrMsg_HintText", "CCSUsrMsg_RadioText"};
	const std::vector<int16_t> ids = {411, 400, 124, 323, 322};
	for (int scenario = 0; scenario < 3; ++scenario) {
		g_messageIds.clear();
		std::vector<std::string> expectedLookups;
		for (std::size_t i = 0; i < shorts.size(); ++i) {
			expectedLookups.push_back(shorts[i]);
			if (scenario != 0) expectedLookups.push_back(protos[i]);
			if (scenario == 0) g_messageIds[shorts[i]] = ids[i] + 1000;
			if (scenario == 1) g_messageIds[protos[i]] = ids[i] + 2000;
		}
		g_messageLookups.clear();
		g_logs.clear();
		SyncUserMessageHooks();
		Check(g_messageLookups == expectedLookups, "short name first, protobuf second only on miss, then numeric fallback");
		Check(g_userMessageHooks.size() == 5, "all five enabled hooks register even when every name lookup fails");
		for (std::size_t i = 0; i < g_userMessageHooks.size(); ++i) {
			const int expected = ids[i] + (scenario == 0 ? 1000 : scenario == 1 ? 2000 : 0);
			Check(g_userMessageHooks[i].id == expected, "the hook uses the registry result when available");
		}
		UnregisterUserMessageHooks();
	}

	// Missing lookup export still permits enum fallback; id 0 must never become a global hook.
	__s2sdk_UserMessageFindMessageIdByName = nullptr;
	RegisterUserMessageHook("TextMsg", "CUserMessageTextMsg", 124, &OnTextMsg);
	Check(g_userMessageHooks.size() == 1 && g_userMessageHooks[0].id == 124, "missing lookup binding uses numeric fallback");
	UnregisterUserMessageHooks();
	const std::size_t before = g_hookedMessages.size();
	RegisterUserMessageHook("Invalid", "InvalidProto", 0, &OnTextMsg);
	Check(g_hookedMessages.size() == before, "unusable fallback id never registers the global id-0 hook");

	g_logs.clear();
	g_messageHookSucceeds = false;
	RegisterUserMessageHook("TestFailure", "TestFailureProto", 124, &OnTextMsg);
	RegisterUserMessageHook("TestFailure", "TestFailureProto", 124, &OnTextMsg);
	Check(g_userMessageHooks.empty() && g_logs.size() == 1 && g_logs[0].second == plg::Severity::Warning,
		  "an unusable fallback hook warns once and is not recorded as registered");
	g_messageHookSucceeds = true;
	__s2sdk_UserMessageFindMessageIdByName = &FakeFindMessageIdByName;
	g_messageIds = shortIds;

	// Restore the feature configuration for the teardown comparison.
	SyncUserMessageHooks();

	Check(IsPluginOk(g_gameManagerPlugin.OnPluginEnd()), "end succeeds");
	CheckTeardownIsExact("message resolution");
}

// --- main --------------------------------------------------------------------------------------

} // namespace

int main() {
	std::printf("game_manager engine tests (fake s2sdk bindings, no engine)\n");

	g_fakeConfigsRoot = "/tmp/game_manager_engine_test_configs";
	remove_all_noexcept(g_fakeConfigsRoot);
	InstallBindings();
	ResetWorld();

	TestMapGateOpening();
	TestLatePluginLoadMidMap();
	TestMapEndDropsDeferredAndTimers();
	TestDefaultConfigWriteAndParity();
	TestEventHookModesAndPreContract();
	TestDeathAndKillFeedPaths();
	TestHideOnlyEvents();
	TestEventsBeforeMapAndAfterMapEndAreInert();
	TestCommandBlockingThroughTheGlobalListener();
	TestReloadCommandOwnershipAndSource();
	TestNativeCommands();
	TestCorpseModesAndHandles();
	TestCorpseFadeTimerCreationFailure();
	TestCorpseFadeTimerLifecycle();
	TestUserMessageResolutionAndFilters();
	TestMessageResolutionOrder();

	remove_all_noexcept(g_fakeConfigsRoot);
	std::printf("%d checks, %d failures\n", g_checks, g_failures);
	return g_failures == 0 ? 0 : 1;
}
