// game_manager_core — pure logic (see the header for what each function is for).

#include "game_manager_core.hpp"

#include <algorithm>
#include <cmath>

#include <nlohmann/json.hpp>

namespace gm {

namespace {

// Reads a boolean from an object; a wrong type keeps the default (the original's loader skipped
// values it could not parse instead of failing the plugin).
bool ReadBool(const nlohmann::json& obj, const char* key, bool fallback) {
	const auto it = obj.find(key);
	if (it == obj.end() || !it->is_boolean()) {
		return fallback;
	}
	return it->get<bool>();
}

int ReadInt(const nlohmann::json& obj, const char* key, int fallback) {
	const auto it = obj.find(key);
	if (it == obj.end() || !it->is_number_integer()) {
		return fallback;
	}
	// Every numeric option in ConfigModel is a byte, including the headtag distance.
	// Check before narrowing: nlohmann otherwise silently wraps oversized integers.
	if (*it < 0 || *it > 255) {
		return fallback;
	}
	return it->get<int>();
}

// The radio commands the original blocked (BlockRadio). Names as typed by the client; the list is
// compared case-sensitively, exactly like the original's BlockedCommands lookup.
const std::vector<std::string> kRadioCommands = {
	"coverme", "takepoint", "holdpos",	 "regroup",	   "followme",	"takingfire", "go",		  "fallback",
	"sticktog", "getinpos", "stormfront", "report",	   "roger",		"enemyspot",  "needbackup", "sectorclear",
	"inposition", "reportingin", "getout", "negative", "enemydown", "sorry",	  "cheer",	  "compliment",
	"thanks",	"go_a",		"go_b",		 "needrop",	   "deathcry",	"radio",	  "radio1",	  "radio2",
	"radio3"};

const std::vector<std::string> kMoneyMessages = {
	"#Player_Cash_Award_Kill_Teammate",
	"#Player_Cash_Award_Killed_VIP",
	"#Player_Cash_Award_Killed_Enemy_Generic",
	"#Player_Cash_Award_Killed_Enemy",
	"#Player_Cash_Award_Bomb_Planted",
	"#Player_Cash_Award_Bomb_Defused",
	"#Player_Cash_Award_Rescued_Hostage",
	"#Player_Cash_Award_Interact_Hostage",
	"#Player_Cash_Award_Respawn",
	"#Player_Cash_Award_Get_Killed",
	"#Player_Cash_Award_Damage_Hostage",
	"#Player_Cash_Award_Kill_Hostage",
	"#Player_Point_Award_Killed_Enemy",
	"#Player_Point_Award_Killed_Enemy_Plural",
	"#Player_Point_Award_Killed_Enemy_NoWeapon",
	"#Player_Point_Award_Killed_Enemy_NoWeapon_Plural",
	"#Player_Point_Award_Assist_Enemy",
	"#Player_Point_Award_Assist_Enemy_Plural",
	"#Player_Point_Award_Picked_Up_Dogtag",
	"#Player_Point_Award_Picked_Up_Dogtag_Plural",
	"#Player_Team_Award_Killed_Enemy",
	"#Player_Team_Award_Killed_Enemy_Plural",
	"#Player_Team_Award_Bonus_Weapon",
	"#Player_Team_Award_Bonus_Weapon_Plural",
	"#Player_Team_Award_Picked_Up_Dogtag",
	"#Player_Team_Award_Picked_Up_Dogtag_Plural",
	"#Player_Team_Award_Picked_Up_Dogtag_Friendly",
	"#Player_Cash_Award_ExplainSuicide_YouGotCash",
	"#Player_Cash_Award_ExplainSuicide_TeammateGotCash",
	"#Player_Cash_Award_ExplainSuicide_EnemyGotCash",
	"#Player_Cash_Award_ExplainSuicide_Spectators",
	"#Team_Cash_Award_T_Win_Bomb",
	"#Team_Cash_Award_Elim_Hostage",
	"#Team_Cash_Award_Elim_Bomb",
	"#Team_Cash_Award_Win_Time",
	"#Team_Cash_Award_Win_Defuse_Bomb",
	"#Team_Cash_Award_Win_Hostages_Rescue",
	"#Team_Cash_Award_Win_Hostage_Rescue",
	"#Team_Cash_Award_Loser_Bonus",
	"#Team_Cash_Award_Bonus_Shorthanded",
	"#Notice_Bonus_Enemy_Team",
	"#Notice_Bonus_Shorthanded_Eligibility",
	"#Notice_Bonus_Shorthanded_Eligibility_Single",
	"#Team_Cash_Award_Loser_Bonus_Neg",
	"#Team_Cash_Award_Loser_Zero",
	"#Team_Cash_Award_Rescued_Hostage",
	"#Team_Cash_Award_Hostage_Interaction",
	"#Team_Cash_Award_Hostage_Alive",
	"#Team_Cash_Award_Planted_Bomb_But_Defused",
	"#Team_Cash_Award_Survive_GuardianMode_Wave",
	"#Team_Cash_Award_CT_VIP_Escaped",
	"#Team_Cash_Award_T_VIP_Killed",
	"#Team_Cash_Award_no_income",
	"#Team_Cash_Award_no_income_suicide",
	"#Team_Cash_Award_Generic",
	"#Team_Cash_Award_Custom"};

const std::vector<std::string> kSavedMessages = {
	"#Chat_SavePlayer_Savior", "#Chat_SavePlayer_Spectator", "#Chat_SavePlayer_Saved"};

const std::vector<std::string> kTeamWarnings = {
	"#Cstrike_TitlesTXT_Game_teammate_attack",
	"#Cstrike_TitlesTXT_Game_teammate_kills",
	"#Cstrike_TitlesTXT_Hint_careful_around_teammates",
	"#Cstrike_TitlesTXT_Hint_try_not_to_injure_teammates",
	"#Cstrike_TitlesTXT_Killed_Teammate",
	"#SFUI_Notice_Game_teammate_kills",
	"#SFUI_Notice_Hint_careful_around_teammates",
	"#SFUI_Notice_Killed_Teammate"};

bool InList(const std::vector<std::string>& list, std::string_view value) {
	return std::find(list.begin(), list.end(), value) != list.end();
}

} // namespace

Config DefaultConfig() {
	return Config{};
}

Config ParseConfig(std::string_view jsonText) {
	Config cfg; // defaults
	if (jsonText.empty()) {
		return cfg;
	}

	nlohmann::json doc = nlohmann::json::parse(jsonText, nullptr, /*allow_exceptions=*/false,
											   /*ignore_comments=*/true);
	if (!doc.is_object()) {
		return cfg;
	}
	// The production file nests everything under "GameManager"; a flat object (e.g. a hand-edited
	// file or a future layout) is read as-is.
	const nlohmann::json* root = &doc;
	const auto wrapper = doc.find("GameManager");
	if (wrapper != doc.end() && wrapper->is_object()) {
		root = &*wrapper;
	}

	cfg.BlockRadio = ReadBool(*root, "BlockRadio", cfg.BlockRadio);
	cfg.BlockBotRadio = ReadBool(*root, "BlockBotRadio", cfg.BlockBotRadio);
	cfg.BlockGrenadesRadio = ReadBool(*root, "BlockGrenadesRadio", cfg.BlockGrenadesRadio);
	cfg.BlockChatWheel = ReadBool(*root, "BlockChatWheel", cfg.BlockChatWheel);
	cfg.BlockPing = ReadBool(*root, "BlockPing", cfg.BlockPing);
	if (const auto it = root->find("BlockedCommands"); it != root->end() && it->is_array()) {
		for (const auto& entry : *it) {
			if (entry.is_string()) {
				cfg.BlockedCommands.push_back(entry.get<std::string>());
			}
		}
	}

	cfg.HideRadar = ReadBool(*root, "HideRadar", cfg.HideRadar);
	cfg.HideKillFeed = ReadInt(*root, "HideKillFeed", cfg.HideKillFeed);
	cfg.HideBlood = ReadBool(*root, "HideBlood", cfg.HideBlood);
	cfg.HideHeadshotSparks = ReadBool(*root, "HideHeadshotSparks", cfg.HideHeadshotSparks);
	cfg.HideTeammateHeadtags = ReadInt(*root, "HideTeammateHeadtags", cfg.HideTeammateHeadtags);
	cfg.HideTeammateHeadtagsDistance =
		ReadInt(*root, "HideTeammateHeadtags_Distance", cfg.HideTeammateHeadtagsDistance);
	cfg.HideCorpses = ReadInt(*root, "HideCorpses", cfg.HideCorpses);
	cfg.HideLegs = ReadBool(*root, "HideLegs", cfg.HideLegs);

	cfg.DisableFallDamage = ReadBool(*root, "DisableFallDamage", cfg.DisableFallDamage);
	cfg.DisableSVCheats = ReadBool(*root, "DisableSVCheats", cfg.DisableSVCheats);
	cfg.DisableC4 = ReadBool(*root, "DisableC4", cfg.DisableC4);
	cfg.DisableCameraSpectator = ReadBool(*root, "DisableCameraSpectator", cfg.DisableCameraSpectator);

	cfg.SoundsMuteFootsteps = ReadBool(*root, "SoundsMuteFootsteps", cfg.SoundsMuteFootsteps);
	cfg.SoundsMuteJumpLand = ReadBool(*root, "SoundsMuteJumpLand", cfg.SoundsMuteJumpLand);
	cfg.SoundsMuteMVPMusic = ReadInt(*root, "SoundsMuteMVPMusic", cfg.SoundsMuteMVPMusic);

	cfg.IgnoreBombPlantedHUDMessages =
		ReadBool(*root, "IgnoreBombPlantedHUDMessages", cfg.IgnoreBombPlantedHUDMessages);
	cfg.IgnoreTeammateAttackMessages =
		ReadBool(*root, "IgnoreTeammateAttackMessages", cfg.IgnoreTeammateAttackMessages);
	cfg.IgnoreAwardsMoneyMessages =
		ReadBool(*root, "IgnoreAwardsMoneyMessages", cfg.IgnoreAwardsMoneyMessages);
	cfg.IgnorePlayerSavedYouMessages =
		ReadBool(*root, "IgnorePlayerSavedYouMessages", cfg.IgnorePlayerSavedYouMessages);
	cfg.IgnoreChickenKilledMessages =
		ReadBool(*root, "IgnoreChickenKilledMessages", cfg.IgnoreChickenKilledMessages);
	cfg.IgnoreJoinTeamMessages = ReadBool(*root, "IgnoreJoinTeamMessages", cfg.IgnoreJoinTeamMessages);
	cfg.IgnorePlantingBombMessages =
		ReadBool(*root, "IgnorePlantingBombMessages", cfg.IgnorePlantingBombMessages);
	cfg.IgnoreDefusingBombMessages =
		ReadBool(*root, "IgnoreDefusingBombMessages", cfg.IgnoreDefusingBombMessages);
	cfg.IgnoreDisconnectMessages = ReadBool(*root, "IgnoreDisconnectMessages", cfg.IgnoreDisconnectMessages);

	return cfg;
}

std::string DefaultConfigJson() {
	// Same layout and key order as the production file (backup-27035.../configs/plugins/
	// GameManager/config.jsonc), so the fleet config can be dropped in unchanged. The original's
	// "DiagGloves" is deliberately absent - that diagnostic is not ported (see the README).
	nlohmann::ordered_json def = {
		{"GameManager",
		 {{"BlockRadio", false},
		  {"BlockBotRadio", false},
		  {"BlockGrenadesRadio", false},
		  {"BlockChatWheel", false},
		  {"BlockPing", false},
		  {"BlockedCommands", nlohmann::ordered_json::array()},
		  {"HideRadar", false},
		  {"HideKillFeed", 0},
		  {"HideBlood", false},
		  {"HideHeadshotSparks", false},
		  {"HideTeammateHeadtags", 0},
		  {"HideTeammateHeadtags_Distance", 0},
		  {"HideCorpses", 0},
		  {"HideLegs", false},
		  {"DisableFallDamage", false},
		  {"DisableSVCheats", false},
		  {"DisableC4", false},
		  {"DisableCameraSpectator", false},
		  {"SoundsMuteFootsteps", false},
		  {"SoundsMuteJumpLand", false},
		  {"SoundsMuteMVPMusic", 0},
		  {"IgnoreBombPlantedHUDMessages", false},
		  {"IgnoreTeammateAttackMessages", false},
		  {"IgnoreAwardsMoneyMessages", false},
		  {"IgnorePlayerSavedYouMessages", false},
		  {"IgnoreChickenKilledMessages", false},
		  {"IgnoreJoinTeamMessages", false},
		  {"IgnorePlantingBombMessages", false},
		  {"IgnoreDefusingBombMessages", false},
		  {"IgnoreDisconnectMessages", false}}}};
	return def.dump(2) + "\n";
}

const std::vector<std::string>& RadioCommands() {
	return kRadioCommands;
}

bool IsRadioCommand(std::string_view command) {
	return InList(kRadioCommands, command);
}

bool ShouldBlockCommand(const Config& cfg, std::string_view commandName) {
	if (commandName.empty()) {
		return false;
	}
	if (cfg.BlockPing && commandName == "player_ping") {
		return true;
	}
	if (cfg.BlockChatWheel && (commandName == "playerchatwheel" || commandName.rfind("+radialradio", 0) == 0)) {
		return true;
	}
	if (cfg.BlockRadio && IsRadioCommand(commandName)) {
		return true;
	}
	return InList(cfg.BlockedCommands, commandName);
}

KillFeedPlan PlanKillFeed(const Config& cfg, bool attackerReceivable) {
	KillFeedPlan plan;
	if (cfg.HideKillFeed != 1 && cfg.HideKillFeed != 2) {
		return plan;
	}
	plan.hideFromClients = true;
	// Mode 2 ("show only own kills"): the kill is handed to the killer directly. A bot has nobody
	// to show it to (the original's !attacker.IsFakeClient around its FireToPlayer).
	if (cfg.HideKillFeed == 2 && attackerReceivable) {
		plan.sendToAttacker = true;
	}
	return plan;
}

uint8_t CorpseAlpha(int mode, double elapsed, double durationSeconds) {
	if (mode <= 0) {
		return 255;
	}
	if (mode == 1) {
		return 0;
	}
	// mode 2: linear fade; the first step already dims (round, so the last values are 1 then 0).
	const double duration = durationSeconds > 0.0 ? durationSeconds : kDefaultCorpseFadeSeconds;
	if (elapsed <= 0.0) {
		return 255;
	}
	if (elapsed >= duration) {
		return 0;
	}
	const double alpha = std::round(255.0 * (1.0 - elapsed / duration));
	if (alpha <= 0.0) {
		return 0;
	}
	if (alpha >= 255.0) {
		return 255;
	}
	return static_cast<uint8_t>(alpha);
}

double CorpseFadeSeconds(std::optional<double> specFreezeDeathanimTime) {
	// The original: Math.Max(0.1f, spec_freeze_deathanim_time ?? 0.8f).
	const double value = specFreezeDeathanimTime.value_or(kDefaultCorpseFadeSeconds);
	return value > kMinCorpseFadeSeconds ? value : kMinCorpseFadeSeconds;
}

bool IsAttachedRagdollClass(std::string_view classname) {
	if (classname.size() != kAttachedRagdollClassname.size()) {
		return false;
	}
	for (std::size_t i = 0; i < classname.size(); ++i) {
		const char left = classname[i];
		char right = kAttachedRagdollClassname[i];
		// ASCII fold, like StringComparison.OrdinalIgnoreCase for this (pure ASCII) class name.
		const char leftFold = (left >= 'A' && left <= 'Z') ? static_cast<char>(left - 'A' + 'a') : left;
		const char rightFold = (right >= 'A' && right <= 'Z') ? static_cast<char>(right - 'A' + 'a') : right;
		if (leftFold != rightFold) {
			return false;
		}
	}
	return true;
}

bool CorpseTrackExpired(double now, double deathTime, double durationSeconds) {
	return now - deathTime > durationSeconds + kCorpseTrackGraceSeconds;
}

int FindDeathForRagdoll(const std::vector<DeathRecord>& deaths, float x, float y, float z) {
	const float radiusSq = kRagdollMatchRadius * kRagdollMatchRadius;
	int best = -1;
	float bestDistSq = 0.0f;
	for (std::size_t i = 0; i < deaths.size(); ++i) {
		const DeathRecord& death = deaths[i];
		const float dx = death.x - x;
		const float dy = death.y - y;
		const float dz = death.z - z;
		const float distSq = dx * dx + dy * dy + dz * dz;
		if (distSq > radiusSq) {
			continue; // the radius is inclusive, like the original's DistanceSquared <= r * r
		}
		if (best < 0 || distSq < bestDistSq) {
			best = static_cast<int>(i);
			bestDistSq = distSq;
		}
	}
	return best;
}

const std::vector<std::string>& MoneyMessageTokens() {
	return kMoneyMessages;
}

const std::vector<std::string>& SavedMessageTokens() {
	return kSavedMessages;
}

const std::vector<std::string>& TeamWarningTokens() {
	return kTeamWarnings;
}

bool ShouldIgnoreTextMessage(const Config& cfg, std::string_view param) {
	if (cfg.IgnoreAwardsMoneyMessages && InList(kMoneyMessages, param)) {
		return true;
	}
	if (cfg.IgnorePlayerSavedYouMessages && InList(kSavedMessages, param)) {
		return true;
	}
	if (cfg.IgnoreChickenKilledMessages && param.find(kChickenKilledToken) != std::string_view::npos) {
		return true;
	}
	if (cfg.IgnorePlantingBombMessages && param == kPlantingToken) {
		return true;
	}
	if (cfg.IgnoreDefusingBombMessages && param == kDefusingToken) {
		return true;
	}
	if (cfg.IgnoreTeammateAttackMessages && InList(kTeamWarnings, param)) {
		return true;
	}
	return false;
}

bool ShouldIgnoreHintText(const Config& cfg, std::string_view message) {
	return cfg.IgnoreTeammateAttackMessages && InList(kTeamWarnings, message);
}

bool ShouldIgnoreRadioText(const Config& cfg, std::string_view msgName) {
	if (cfg.IgnorePlantingBombMessages && msgName.find(kPlantingToken) != std::string_view::npos) {
		return true;
	}
	return cfg.IgnoreDefusingBombMessages && msgName.find(kDefusingToken) != std::string_view::npos;
}

} // namespace gm
