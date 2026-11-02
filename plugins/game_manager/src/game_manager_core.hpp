// game_manager — the engine-free half of the port (all decisions, no s2sdk).
//
// Everything here is a pure function over the plugin's config / the data a hook got, so the test
// suite can cover the tricky parts (the command-block decision, the kill-feed plan, the corpse
// alpha curve, the ragdoll match, the HUD text filters) without the engine.
//
// The option set is the SwiftlyS2 GameManager's config.jsonc (the production file ships under
// {"GameManager": {...}}; a flat object is accepted as well). Behaviour references come from the
// ported C# original's config semantics - the plan/alpha/filter functions keep exactly the
// decisions the original made in its hooks.

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace gm {

// --- s2sdk value guards ----------------------------------------------------------------------

// Entity HANDLES are the raw int32 view of the engine's CEntityHandle (index | serial << 15):
// -1 is INVALID_EHANDLE_INDEX and 0 is the world entity, so neither is a usable entity here.
// The sign of the value means nothing: once the engine's serial counter passes 65535 bit 31 is
// set and a perfectly valid handle is negative. A `handle > 0` test would silently drop those
// entities - PITFALLS C1. s2sdk::IsValidEntHandle additionally checks that index + serial still
// exist in the entity system; callers combine both checks.
inline bool IsValidHandle(std::int32_t handle) {
	return handle != -1 && handle != 0;
}

// CS2's player slots are 0..63 (MAXPLAYERS); every slot-taking s2sdk export assumes the range.
inline bool IsValidSlot(std::int32_t slot) {
	return slot >= 0 && slot < 64;
}

// --- config ----------------------------------------------------------------------------------

// The options of the original's config.jsonc, with the original's defaults.
struct Config {
	// Commands
	bool BlockRadio = false;
	bool BlockBotRadio = false;
	bool BlockGrenadesRadio = false;
	bool BlockChatWheel = false;
	bool BlockPing = false;
	std::vector<std::string> BlockedCommands;

	// HUD
	bool HideRadar = false;
	int HideKillFeed = 0; // 0 off, 1 hide, 2 only the killer still gets the kill
	bool HideBlood = false;
	bool HideHeadshotSparks = false;
	int HideTeammateHeadtags = 0;
	int HideTeammateHeadtagsDistance = 0;
	int HideCorpses = 0; // 0 off, 1 hide instantly, 2 fade over spec_freeze_deathanim_time
	bool HideLegs = false;

	// Gameplay
	bool DisableFallDamage = false;
	bool DisableSVCheats = false;
	bool DisableC4 = false;
	bool DisableCameraSpectator = false;

	// Sounds
	bool SoundsMuteFootsteps = false;
	bool SoundsMuteJumpLand = false;
	int SoundsMuteMVPMusic = 0; // 0 off, 1 mute the MVP music only, 2 stop all music at the MVP

	// HUD messages
	bool IgnoreBombPlantedHUDMessages = false;
	bool IgnoreTeammateAttackMessages = false;
	bool IgnoreAwardsMoneyMessages = false;
	bool IgnorePlayerSavedYouMessages = false;
	bool IgnoreChickenKilledMessages = false;
	bool IgnoreJoinTeamMessages = false;
	bool IgnorePlantingBombMessages = false;
	bool IgnoreDefusingBombMessages = false;
	bool IgnoreDisconnectMessages = false;
};

Config DefaultConfig();

// Parses the config document. Accepts the production layout (every option inside a root
// "GameManager" object) and a flat object. Numeric options keep ConfigModel's byte range (0..255).
// Missing keys and values of the wrong type keep the
// default; malformed JSON returns the defaults (the caller logs that).
Config ParseConfig(std::string_view jsonText);

// The document written when the config file does not exist yet: the production layout and every
// default, 2-space indented like the original's file.
std::string DefaultConfigJson();

// --- commands --------------------------------------------------------------------------------

// The original's radio command list (BlockRadio).
const std::vector<std::string>& RadioCommands();
bool IsRadioCommand(std::string_view command);

// BlockRadio + BlockChatWheel + BlockPing + BlockedCommands for the executed command name
// (args[0]). Case-sensitive, like the original's comparisons.
bool ShouldBlockCommand(const Config& cfg, std::string_view commandName);

// --- kill feed -------------------------------------------------------------------------------

// The player_death kill-feed decision.
struct KillFeedPlan {
	bool hideFromClients = false; // HideKillFeed 1/2: the engine must not broadcast the event
	bool sendToAttacker = false;  // HideKillFeed == 2: the killer still gets it (FireEventToClient)
};

// HideKillFeed == 1 hides the kill for everyone; == 2 ("show only own kills") hides it from every
// client but hands it to the killer directly. attackerReceivable is false when the killer has
// nobody to show it to (a bot) - only consulted for mode 2. (Deaths whose attacker is not a valid
// player at all never get here: the plugin ignores them like the original does.)
KillFeedPlan PlanKillFeed(const Config& cfg, bool attackerReceivable);

// --- corpses ---------------------------------------------------------------------------------

// The alpha written `elapsed` seconds into a corpse fade:
//   * mode 1 hides instantly (0);
//   * mode 2 fades 255 -> 0 linearly over the whole duration (rounded, clamped);
//   * any other mode or a death that just happened keeps the render color (255).
uint8_t CorpseAlpha(int mode, double elapsed, double durationSeconds);

// The fade length, exactly the original's `Math.Max(0.1f, spec_freeze_deathanim_time ?? 0.8f)`:
// the convar when the game has one, 0.8 s otherwise, never below 0.1 s.
constexpr double kDefaultCorpseFadeSeconds = 0.8;
constexpr double kMinCorpseFadeSeconds = 0.1;
double CorpseFadeSeconds(std::optional<double> specFreezeDeathanimTime);

// The entity class ("DesignerName") the original matches in its OnEntityCreated hook to catch a
// player's corpse: `string.Equals(DesignerName, "prop_ragdoll_attached", OrdinalIgnoreCase)`.
constexpr std::string_view kAttachedRagdollClassname = "prop_ragdoll_attached";
bool IsAttachedRagdollClass(std::string_view classname);

// A death whose attached corpse props ("prop_ragdoll_attached") are expected to appear shortly.
struct DeathRecord {
	int slot = -1;       // victim player slot
	int pawnHandle = -1; // s2sdk entity handle of the pawn at the time it died
	float x = 0.0f;
	float y = 0.0f;
	float z = 0.0f;
	double time = 0.0; // game time of the death
};

// The original matched a newly created prop_ragdoll_attached against a death within this radius.
constexpr float kRagdollMatchRadius = 128.0f;

// How long a death stays in the list: the original drops a track once
// `now - StartedAt > duration + 2 s` (so mode 1 tracks, duration 0, live 2 s).
constexpr double kCorpseTrackGraceSeconds = 2.0;
bool CorpseTrackExpired(double now, double deathTime, double durationSeconds);

// Index of the nearest death within kRagdollMatchRadius (the oldest record wins a tie, like the
// original's stable OrderBy().FirstOrDefault()), or -1 when the entity belongs to nobody.
int FindDeathForRagdoll(const std::vector<DeathRecord>& deaths, float x, float y, float z);

// --- HUD message filters ---------------------------------------------------------------------

// The "#..." tokens the original matched against (the config's Ignore*Messages options).
const std::vector<std::string>& MoneyMessageTokens();
const std::vector<std::string>& SavedMessageTokens();
const std::vector<std::string>& TeamWarningTokens();
constexpr std::string_view kChickenKilledToken = "#Pet_Killed";
constexpr std::string_view kPlantingToken = "#Cstrike_TitlesTXT_Planting_Bomb";
constexpr std::string_view kDefusingToken = "#Cstrike_TitlesTXT_Defusing_Bomb";

// CUserMessageTextMsg: the message is tested against every repeated "param" value; true = drop it.
bool ShouldIgnoreTextMessage(const Config& cfg, std::string_view param);
// CCSUsrMsg_HintText: the "message" field (only IgnoreTeammateAttackMessages uses it).
bool ShouldIgnoreHintText(const Config& cfg, std::string_view message);
// CCSUsrMsg_RadioText: the "msg_name" field (planting/defusing).
bool ShouldIgnoreRadioText(const Config& cfg, std::string_view msgName);

} // namespace gm
