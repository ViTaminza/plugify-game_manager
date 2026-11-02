// game_manager core tests — no engine: the decisions the plugin makes (config parsing with
// defaults, the command-block decision, the kill-feed plan, the corpse alpha curve and the ragdoll
// match, the HUD text filters) are pure functions in game_manager_core.
//
// Run by ./test.sh (build/tests/game_manager_tests).

#include <game_manager_core.hpp>

#include <cstdio>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace {

int g_checks = 0;
int g_failures = 0;

void Check(bool condition, const std::string& what) {
	++g_checks;
	if (!condition) {
		++g_failures;
		std::printf("FAIL: %s\n", what.c_str());
	}
}

// --- s2sdk value guards ------------------------------------------------------------------------

void TestHandleGuards() {
	std::printf("-- s2sdk handle guards (PITFALLS C1)\n");

	// -1 is INVALID_EHANDLE_INDEX, 0 is the world entity; neither is a usable entity reference.
	Check(!gm::IsValidHandle(-1), "-1 is rejected");
	Check(!gm::IsValidHandle(0), "0 is rejected");
	Check(gm::IsValidHandle(1), "a positive handle is valid");
	// The sign is not a validity flag: a serial above 65535 sets bit 31 and valid handles become
	// negative. A `handle > 0` check would silently drop every such entity.
	Check(gm::IsValidHandle(-2), "a negative handle other than -1 is valid");
	Check(gm::IsValidHandle(static_cast<int32_t>(0x80000001u)), "the minimum negative handle is valid");

	Check(gm::IsValidSlot(0) && gm::IsValidSlot(63), "slots 0..63 are valid");
	Check(!gm::IsValidSlot(-1) && !gm::IsValidSlot(64), "slot -1 and slot 64 are rejected");
}

// --- config ------------------------------------------------------------------------------------

void TestConfigDefaults() {
	std::printf("-- config defaults\n");

	const gm::Config defaults = gm::DefaultConfig();
	Check(defaults.HideKillFeed == 0 && defaults.HideCorpses == 0 && defaults.SoundsMuteMVPMusic == 0,
		  "the default modes are all off");
	Check(defaults.BlockedCommands.empty(), "no commands are blocked by default");

	Check(gm::ParseConfig("").HideCorpses == 0, "an empty document keeps the defaults");
	Check(gm::ParseConfig("{ this is not json").HideKillFeed == 0, "broken JSON keeps the defaults");
	Check(gm::ParseConfig("[1, 2, 3]").HideBlood == false, "a non-object document keeps the defaults");
}

void TestConfigProductionLayout() {
	std::printf("-- config: the production layout ({\"GameManager\": {...}})\n");

	// The shape of the live server's configs/plugins/GameManager/config.jsonc (backup 27035).
	const char* text = R"({
	  "GameManager": {
	    "BlockRadio": true,
	    "BlockedCommands": ["gm_block_a", "gm_block_b", 7],
	    "HideKillFeed": 2,
	    "HideCorpses": 2,
	    "HideTeammateHeadtags": 3,
	    "HideTeammateHeadtags_Distance": 250,
	    "SoundsMuteMVPMusic": 1,
	    "IgnoreJoinTeamMessages": true,
	    "DiagGloves": true
	  }
	})";
	const gm::Config cfg = gm::ParseConfig(text);
	Check(cfg.BlockRadio, "BlockRadio is read from the GameManager object");
	Check(cfg.BlockedCommands.size() == 2 && cfg.BlockedCommands[0] == "gm_block_a",
		  "the string entries of BlockedCommands are kept (non-strings are skipped)");
	Check(cfg.HideKillFeed == 2, "HideKillFeed is read");
	Check(cfg.HideCorpses == 2, "HideCorpses is read");
	Check(cfg.HideTeammateHeadtags == 3 && cfg.HideTeammateHeadtagsDistance == 250,
		  "the headtag mode and its distance are read");
	Check(cfg.SoundsMuteMVPMusic == 1, "SoundsMuteMVPMusic is read");
	Check(cfg.IgnoreJoinTeamMessages, "IgnoreJoinTeamMessages is read");
	Check(cfg.IgnoreDisconnectMessages == false, "a missing key keeps its default");
	// The fleet's DiagGloves is not ported: it must never influence anything here.
	Check(!cfg.HideRadar && !cfg.HideBlood, "DiagGloves does not bleed into other options");
}

// The checked-in production layout (assets/config.jsonc) is what the fleet runs. The file has no
// "SoundsMuteMVPMusic" key (the original default 0 applies) and carries the unported "DiagGloves"
// diagnostic, which must be parsed without affecting any option.
void TestProductionConfigParity() {
	std::printf("-- config: production asset parity\n");

#ifndef GM_PRODUCTION_CONFIG_PATH
	std::printf("   (GM_PRODUCTION_CONFIG_PATH is not defined - skipped)\n");
	return;
#else
	std::ifstream in(GM_PRODUCTION_CONFIG_PATH, std::ios::binary);
	if (!in) {
		Check(false, "the production config asset could not be opened");
		return;
	}
	std::ostringstream buffer;
	buffer << in.rdbuf();
	const gm::Config cfg = gm::ParseConfig(buffer.str());

	Check(cfg.HideBlood && cfg.HideHeadshotSparks, "the production visuals are enabled");
	Check(cfg.HideCorpses == 2, "the production corpse mode fades");
	Check(cfg.HideKillFeed == 0, "the production kill feed is untouched");
	Check(cfg.HideTeammateHeadtags == 0 && cfg.HideTeammateHeadtagsDistance == 0,
		  "the production headtags are untouched");
	Check(cfg.SoundsMuteMVPMusic == 0, "a missing SoundsMuteMVPMusic key keeps the original default");
	Check(!cfg.BlockRadio && cfg.BlockedCommands.empty(), "the production command blocking is off");
	Check(cfg.IgnoreJoinTeamMessages == false, "the production chat filters are the file's values");
#endif
}

void TestConfigFlatAndWrongTypes() {
	std::printf("-- config: flat layout, wrong types, comments\n");

	const gm::Config flat = gm::ParseConfig(R"({"HideKillFeed": 1, "HideLegs": true})");
	Check(flat.HideKillFeed == 1 && flat.HideLegs, "a flat document is read as well");

	const gm::Config typed = gm::ParseConfig(R"({
		"HideKillFeed": "yes",
		"BlockRadio": 1,
		"BlockedCommands": "gm_x"
	})");
	Check(typed.HideKillFeed == 0, "a string where an int is expected keeps the default");
	Check(!typed.BlockRadio, "a number where a bool is expected keeps the default");
	Check(typed.BlockedCommands.empty(), "a string BlockedCommands keeps the default (empty)");
	const gm::Config oversized = gm::ParseConfig(R"({"HideKillFeed": 4294967297, "HideCorpses": -1,
		"SoundsMuteMVPMusic": 256, "HideTeammateHeadtags_Distance": 512})");
	Check(oversized.HideKillFeed == 0 && oversized.HideCorpses == 0 && oversized.SoundsMuteMVPMusic == 0 &&
		  oversized.HideTeammateHeadtagsDistance == 0, "byte options reject out-of-range values before narrowing");

	// .jsonc: comments are allowed in the file.
	const gm::Config commented = gm::ParseConfig("{\n  // a comment\n  \"HideCorpses\": 1\n}");
	Check(commented.HideCorpses == 1, "comments are ignored");
}

void TestDefaultConfigJson() {
	std::printf("-- config: the written default file\n");

	const std::string text = gm::DefaultConfigJson();
	Check(text.find("\"GameManager\"") != std::string::npos, "the default file uses the production layout");
	Check(text.find("DiagGloves") == std::string::npos, "the unported glove diagnostic is not in the file");

	const gm::Config parsed = gm::ParseConfig(text);
	Check(parsed.HideKillFeed == 0 && parsed.HideCorpses == 0 && !parsed.BlockRadio && parsed.BlockedCommands.empty(),
		  "the written default file parses back into the defaults");
	Check(text.find("\"HideTeammateHeadtags_Distance\"") != std::string::npos,
		  "the original's key spelling is kept");
}

// --- commands ----------------------------------------------------------------------------------

void TestCommandBlockDecision() {
	std::printf("-- command blocking\n");

	gm::Config cfg;
	Check(!gm::ShouldBlockCommand(cfg, "coverme"), "nothing blocks while every option is off");
	Check(!gm::ShouldBlockCommand(cfg, ""), "an empty command name is never blocked");

	cfg.BlockRadio = true;
	Check(gm::ShouldBlockCommand(cfg, "coverme"), "BlockRadio blocks a radio command");
	Check(gm::ShouldBlockCommand(cfg, "radio3"), "BlockRadio covers the numbered radio commands");
	Check(!gm::ShouldBlockCommand(cfg, "CoverMe"), "the match is case-sensitive (as in the original)");
	Check(!gm::ShouldBlockCommand(cfg, "player_ping"), "BlockRadio does not touch player_ping");

	cfg.BlockPing = true;
	Check(gm::ShouldBlockCommand(cfg, "player_ping"), "BlockPing blocks player_ping");

	cfg.BlockChatWheel = true;
	Check(gm::ShouldBlockCommand(cfg, "playerchatwheel"), "BlockChatWheel blocks playerchatwheel");
	Check(gm::ShouldBlockCommand(cfg, "+radialradio"), "BlockChatWheel blocks +radialradio");
	Check(gm::ShouldBlockCommand(cfg, "+radialradio2"), "the +radialradio prefix match covers its variants");
	Check(!gm::ShouldBlockCommand(cfg, "radialradio"), "a bare radialradio is not the chat wheel command");

	cfg.BlockedCommands = {"gm_custom"};
	Check(gm::ShouldBlockCommand(cfg, "gm_custom"), "BlockedCommands matches exactly");
	Check(!gm::ShouldBlockCommand(cfg, "GM_CUSTOM"), "BlockedCommands is case-sensitive");
	Check(!gm::ShouldBlockCommand(cfg, "gm_custo"), "BlockedCommands is not a prefix match");

	// The radio list itself is the original's.
	const std::vector<std::string>& radio = gm::RadioCommands();
	Check(radio.size() == 33, "the radio command list has the original's 33 entries");
	Check(gm::IsRadioCommand("deathcry") && gm::IsRadioCommand("needrop"), "spot checks of the list");
	Check(!gm::IsRadioCommand("player_ping"), "player_ping is not a radio command");
}

// --- kill feed ---------------------------------------------------------------------------------

void TestKillFeedPlan() {
	std::printf("-- kill feed plan\n");

	gm::Config cfg;
	Check(!gm::PlanKillFeed(cfg, true).hideFromClients, "mode 0 keeps the kill feed untouched");

	cfg.HideKillFeed = 1;
	gm::KillFeedPlan plan = gm::PlanKillFeed(cfg, true);
	Check(plan.hideFromClients && !plan.sendToAttacker, "mode 1 hides the kill from everyone");
	plan = gm::PlanKillFeed(cfg, false);
	Check(plan.hideFromClients && !plan.sendToAttacker, "mode 1 hides it whatever the attacker is");

	cfg.HideKillFeed = 2;
	plan = gm::PlanKillFeed(cfg, true);
	Check(plan.hideFromClients && plan.sendToAttacker, "mode 2 hides it from everyone but hands it to the killer");
	plan = gm::PlanKillFeed(cfg, false);
	Check(plan.hideFromClients && !plan.sendToAttacker, "mode 2 with a bot killer (not receivable) sends to nobody");
	for (int mode : {-1, 3, 255}) {
		cfg.HideKillFeed = mode;
		plan = gm::PlanKillFeed(cfg, true);
		Check(!plan.hideFromClients && !plan.sendToAttacker, "unsupported kill-feed modes leave the event alone");
	}
}

// --- corpses -----------------------------------------------------------------------------------

void TestCorpseAlpha() {
	std::printf("-- corpse alpha\n");

	Check(gm::CorpseAlpha(0, 0.0, 0.5) == 255, "mode 0 never changes the alpha");
	Check(gm::CorpseAlpha(1, 0.0, 0.5) == 0, "mode 1 hides at once");
	Check(gm::CorpseAlpha(1, 10.0, 0.5) == 0, "mode 1 stays hidden");

	Check(gm::CorpseAlpha(2, 0.0, 0.5) == 255, "mode 2 starts fully visible");
	Check(gm::CorpseAlpha(2, 0.25, 0.5) == 128, "mode 2 is half transparent at half the fade");
	Check(gm::CorpseAlpha(2, 0.5, 0.5) == 0, "mode 2 ends fully hidden");
	Check(gm::CorpseAlpha(2, 5.0, 0.5) == 0, "mode 2 stays hidden after the fade");
	Check(gm::CorpseAlpha(2, -1.0, 0.5) == 255, "a negative elapsed time keeps the alpha");
	Check(gm::CorpseAlpha(2, 0.1, 0.5) == 204, "the first 0.1 s step already dims the corpse");

	// Monotonic non-increasing across the steps the timer takes.
	uint8_t previous = 255;
	for (double elapsed = 0.0; elapsed <= 0.6; elapsed += 0.1) {
		const uint8_t alpha = gm::CorpseAlpha(2, elapsed, 0.5);
		Check(alpha <= previous, "alpha never rises during the fade");
		previous = alpha;
	}
}

void TestCorpseFadeSeconds() {
	std::printf("-- corpse fade duration (Math.Max(0.1f, value ?? 0.8f))\n");

	Check(gm::CorpseFadeSeconds(std::nullopt) == 0.8, "a missing spec_freeze_deathanim_time uses 0.8 s");
	Check(gm::CorpseFadeSeconds(1.5) == 1.5, "a positive spec_freeze_deathanim_time is the fade length");
	Check(gm::CorpseFadeSeconds(0.0) == gm::kMinCorpseFadeSeconds, "0 is raised to the 0.1 s minimum");
	Check(gm::CorpseFadeSeconds(-1.0) == gm::kMinCorpseFadeSeconds, "a negative value is raised to 0.1 s");
	Check(gm::CorpseFadeSeconds(0.05) == gm::kMinCorpseFadeSeconds, "a sub-minimum value is raised to 0.1 s");
	Check(gm::CorpseFadeSeconds(gm::kDefaultCorpseFadeSeconds) == gm::kDefaultCorpseFadeSeconds,
		  "the fallback itself is stable");
}

void TestAttachedRagdollClass() {
	std::printf("-- prop_ragdoll_attached class match\n");

	Check(gm::IsAttachedRagdollClass("prop_ragdoll_attached"), "the exact class name matches");
	Check(gm::IsAttachedRagdollClass("PROP_RAGDOLL_ATTACHED"), "the match is case-insensitive (OrdinalIgnoreCase)");
	Check(gm::IsAttachedRagdollClass("Prop_Ragdoll_Attached"), "mixed case matches as well");
	Check(!gm::IsAttachedRagdollClass("prop_ragdoll"), "a plain prop_ragdoll is not the corpse prop");
	Check(!gm::IsAttachedRagdollClass("prop_ragdoll_attached2"), "a longer name does not match");
	Check(!gm::IsAttachedRagdollClass(""), "an empty class name does not match");
}

void TestCorpseTrackExpiry() {
	std::printf("-- corpse track lifetime (duration + 2 s)\n");

	Check(!gm::CorpseTrackExpired(100.0, 100.0, 0.0), "an instant track (mode 1) is alive at 0 s");
	Check(!gm::CorpseTrackExpired(102.0, 100.0, 0.0), "an instant track is alive at the 2 s boundary");
	Check(gm::CorpseTrackExpired(102.1, 100.0, 0.0), "an instant track is dropped past 2 s");
	Check(!gm::CorpseTrackExpired(102.5, 100.0, 0.5), "a 0.5 s fade track lives 2.5 s");
	Check(gm::CorpseTrackExpired(102.6, 100.0, 0.5), "and is dropped after that");
}

void TestRagdollMatch() {
	std::printf("-- prop_ragdoll_attached match\n");

	const std::vector<gm::DeathRecord> deaths = {
		{2, 100, 0.0f, 0.0f, 0.0f, 100.0},
		{3, 200, 100.0f, 0.0f, 0.0f, 109.0},  // 100 units away from the first
		{4, 300, 1000.0f, 0.0f, 0.0f, 109.9}, // far away
	};

	Check(gm::FindDeathForRagdoll(deaths, 90.0f, 0.0f, 0.0f) == 1, "the nearest death matches");
	Check(gm::FindDeathForRagdoll(deaths, 0.0f, 0.0f, 0.0f) == 0, "the exact position matches");
	Check(gm::FindDeathForRagdoll(deaths, 500.0f, 0.0f, 0.0f) == -1, "nothing within 128 units: no match");
	Check(gm::FindDeathForRagdoll({}, 0.0f, 0.0f, 0.0f) == -1, "no deaths: no match");

	// The distance is inclusive at the radius (the original's 128 units) and ties keep the older
	// record (the original's stable OrderBy().FirstOrDefault()).
	const std::vector<gm::DeathRecord> edge = {{0, 1, 128.0f, 0.0f, 0.0f, 0.0}, {1, 2, -128.0f, 0.0f, 0.0f, 1.0}};
	Check(gm::FindDeathForRagdoll(edge, 0.0f, 0.0f, 0.0f) == 0,
		  "exactly 128 units matches and the first of two equal distances wins");
}

// --- HUD text filters --------------------------------------------------------------------------

void TestTextMessageFilters() {
	std::printf("-- TextMsg filters\n");

	gm::Config cfg;
	Check(!gm::ShouldIgnoreTextMessage(cfg, "#Player_Cash_Award_Killed_Enemy"), "nothing is filtered while off");

	cfg.IgnoreAwardsMoneyMessages = true;
	Check(gm::ShouldIgnoreTextMessage(cfg, "#Player_Cash_Award_Killed_Enemy"), "a money award is dropped");
	Check(gm::ShouldIgnoreTextMessage(cfg, "#Team_Cash_Award_Custom"), "the list's last entry is covered");
	Check(!gm::ShouldIgnoreTextMessage(cfg, "#Chat_SavePlayer_Savior"), "an unrelated column is kept");

	cfg.IgnorePlayerSavedYouMessages = true;
	Check(gm::ShouldIgnoreTextMessage(cfg, "#Chat_SavePlayer_Savior"), "the saved-you message is dropped");
	Check(!gm::ShouldIgnoreTextMessage(cfg, "#Chat_SavePlayer_Savior2"), "the saved-you match is exact");

	cfg.IgnoreChickenKilledMessages = true;
	Check(gm::ShouldIgnoreTextMessage(cfg, "#Pet_Killed_Something"), "the chicken kill is a substring match");
	Check(!gm::ShouldIgnoreTextMessage(cfg, "#Pet_Saved"), "an unrelated pet message is kept");

	cfg.IgnorePlantingBombMessages = true;
	cfg.IgnoreDefusingBombMessages = true;
	Check(gm::ShouldIgnoreTextMessage(cfg, "#Cstrike_TitlesTXT_Planting_Bomb"), "the planting message is dropped");
	Check(gm::ShouldIgnoreTextMessage(cfg, "#Cstrike_TitlesTXT_Defusing_Bomb"), "the defusing message is dropped");

	cfg.IgnoreTeammateAttackMessages = true;
	Check(gm::ShouldIgnoreTextMessage(cfg, "#Cstrike_TitlesTXT_Killed_Teammate"), "a team warning is dropped");

	// A filter that is off never matches, whatever the token.
	gm::Config onlySaved;
	onlySaved.IgnorePlayerSavedYouMessages = true;
	Check(!gm::ShouldIgnoreTextMessage(onlySaved, "#Cstrike_TitlesTXT_Planting_Bomb"),
		  "a disabled filter stays out of the way");
}

void TestHintAndRadioTextFilters() {
	std::printf("-- HintText / RadioText filters\n");

	gm::Config cfg;
	Check(!gm::ShouldIgnoreHintText(cfg, "#Cstrike_TitlesTXT_Killed_Teammate"), "hint: off by default");

	cfg.IgnoreTeammateAttackMessages = true;
	Check(gm::ShouldIgnoreHintText(cfg, "#Cstrike_TitlesTXT_Killed_Teammate"), "the teammate hint is dropped");
	Check(!gm::ShouldIgnoreHintText(cfg, "#Cstrike_TitlesTXT_Planting_Bomb"), "an unrelated hint is kept");

	Check(!gm::ShouldIgnoreRadioText(cfg, "#Cstrike_TitlesTXT_Planting_Bomb"), "radio: off by default");

	cfg.IgnorePlantingBombMessages = true;
	Check(gm::ShouldIgnoreRadioText(cfg, "#Cstrike_TitlesTXT_Planting_Bomb"), "the radio planting text is dropped");
	Check(!gm::ShouldIgnoreRadioText(cfg, "#Cstrike_TitlesTXT_Defusing_Bomb"), "defusing is off here");

	cfg.IgnoreDefusingBombMessages = true;
	Check(gm::ShouldIgnoreRadioText(cfg, "#Cstrike_TitlesTXT_Defusing_Bomb"), "the radio defusing text is dropped");
}

void TestFilterLists() {
	std::printf("-- the token lists\n");

	Check(gm::MoneyMessageTokens().size() == 56, "the money message list matches the original");
	Check(gm::SavedMessageTokens().size() == 3, "three saved-player messages");
	Check(gm::TeamWarningTokens().size() == 8, "eight teammate-warning messages");
	Check(gm::MoneyMessageTokens().front() == "#Player_Cash_Award_Kill_Teammate", "the money list keeps the order");
}

} // namespace

int main() {
	std::printf("game_manager core tests\n");
	TestConfigDefaults();
	TestConfigProductionLayout();
	TestProductionConfigParity();
	TestHandleGuards();
	TestConfigFlatAndWrongTypes();
	TestDefaultConfigJson();
	TestCommandBlockDecision();
	TestKillFeedPlan();
	TestCorpseAlpha();
	TestCorpseFadeSeconds();
	TestAttachedRagdollClass();
	TestCorpseTrackExpiry();
	TestRagdollMatch();
	TestTextMessageFilters();
	TestHintAndRadioTextFilters();
	TestFilterLists();
	std::printf("%d checks, %d failures\n", g_checks, g_failures);
	return g_failures == 0 ? 0 : 1;
}
