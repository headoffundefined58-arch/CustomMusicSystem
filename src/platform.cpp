#include "platform.h"

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>

// windows.h must come AFTER CommonLib (it errors if seen first).
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <cmath>
#include <functional>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;

namespace cms::platform {

namespace {

// ------------------------------------------------------- combat sensor clock --
// Written by the game thread from inside the event sinks, read by Snapshot().
double NowSec()
{
	using clock = std::chrono::steady_clock;
	static const auto t0 = clock::now();
	return std::chrono::duration<double>(clock::now() - t0).count();
}

// The whole "is this really a fight" decision lives in core.cpp (CombatGate,
// unit tested there).  This file only turns game events into its inputs.
CombatGate g_gate;
bool       g_anyHostileInCombat = false;

// ------------------------------------------------------------ IMR handshake --
// One mapped page, opened lazily on the first read and kept until the process
// ends.  CMS is a *reader only*: it never creates the section, so an install
// without Interactive Music Radio costs nothing and leaves nothing behind.
constexpr wchar_t kImrBlockName[] = L"IMR_CMS_HANDSHAKE";

std::atomic<void*> g_imrView{ nullptr };
std::atomic<bool>  g_imrTried{ false };
bool               g_imrLast = false;   // last logged answer, main thread only

void* ImrView()
{
	if (void* v = g_imrView.load(std::memory_order_acquire)) return v;

	// Only attempt the mapping once per session: if IMR is not installed, every
	// further OpenFileMappingW would just fail again on every tick.
	bool expected = false;
	if (!g_imrTried.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
		return g_imrView.load(std::memory_order_acquire);

	HANDLE h = OpenFileMappingW(FILE_MAP_READ, FALSE, kImrBlockName);
	if (!h) {
		LOG_D("IMR: no handshake block (Interactive Music Radio not running)");
		return nullptr;
	}
	void* view = MapViewOfFile(h, FILE_MAP_READ, 0, 0, sizeof(ImrLink));
	CloseHandle(h);   // the view keeps the section alive
	if (!view) {
		LOG_W("IMR: handshake block found but could not be mapped");
		return nullptr;
	}
	g_imrView.store(view, std::memory_order_release);
	LOG_I("IMR: handshake block attached (radio can take over the music layer)");
	return view;
}

constexpr std::uint32_t kMusicCategoryFormID = 0x00071E64;  // SNCT AudioCategoryMUS

std::string ToLowerCopy(std::string_view a_in)
{
	std::string out(a_in);
	for (auto& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	return out;
}

struct KeywordTag {
	std::string_view needle;
	std::uint64_t    bits;
};

// Substring table on cell / worldspace EditorIDs.  EditorIDs are empty for plug-in
// records at runtime, so this can only ever add *hints*; the hour band plus the
// Always-ish additive rules keep every outdoor cell covered regardless.
constexpr KeywordTag kNameTags[] = {
	{ "whiterun", CityWhiterun | Town },
	{ "markarth", CityMarkarth | Town },
	{ "riften", CityRiften | Town },
	{ "solitude", CitySolitude | Town },
	{ "windhelm", CityWindhelm | Town },
	{ "dragonsreach", Town },
	{ "mistveil", Town },
	{ "bluepalace", Town },
	{ "understone", Town },

	{ "cave", Cave | Dungeon },
	{ "cavern", Cave | Dungeon },
	{ "warren", Warrens | Dungeon },
	{ "nest", Warrens | Dungeon },
	{ "fort", Fort | Dungeon },
	{ "ruin", Ruin | Dungeon },
	{ "barrow", Crypt | Dungeon },
	{ "crypt", Crypt | Dungeon },
	{ "catacomb", Crypt | Dungeon },
	{ "tomb", Crypt | Dungeon },
	{ "mine", Mine | Dungeon },
	{ "icecave", Ice | Dungeon },

	{ "dwemer", Dwemer },
	{ "blackreach", Blackreach },
	{ "castle", Castle },
	{ "volkihar", Castle | Dungeon },
	{ "college", College },
	{ "temple", Temple },
	{ "shrine", Temple },
	{ "tavern", Tavern },
	{ "inn", Tavern },
	{ "museum", Museum },
	{ "library", B(TagLibrary) },
	{ "arcanaeum", B(TagLibrary) },

	{ "breezehome", HomeBreezehome | HomeAny },
	{ "vlindrel", HomeVlindrel | HomeAny },
	{ "hjerim", HomeHjerim | HomeAny },
	{ "proudspire", HomeProudspire | HomeAny },
	{ "honeyside", HomeHoneyside | HomeAny },

	{ "soulcairn", SoulCairn },
	{ "forgottenvale", FalmerValley },
	{ "falmer", FalmerValley },
	{ "apocrypha", DBApocrypha },
	{ "solstheim", DBArea },
	{ "dlc2", DBArea },

	{ "tundra", Tundra },
	{ "snow", Snow },
	{ "frost", Snow },
	{ "pine", ForestPine },
	{ "forest", ForestFall },
	{ "autumn", ForestFall },
	{ "mountain", Mountain },
	{ "cliff", Mountain },
	{ "marsh", Marsh },
	{ "swamp", Marsh },
	{ "coast", Coastal },
	{ "shore", Coastal },
	{ "volcanic", Volcanic },
	{ "ashland", Volcanic },
};

void ApplyNameTags(std::uint64_t& a_bits, std::string_view a_text)
{
	if (a_text.empty()) return;
	const auto low = ToLowerCopy(a_text);
	for (const auto& t : kNameTags) {
		if (low.find(t.needle) != std::string::npos) a_bits |= t.bits;
	}
}

struct KeywordMask {
	std::string_view keyword;
	std::uint64_t    bits;
};

constexpr KeywordMask kActorKeywords[] = {
	{ "ActorTypeDragon", EnemyDragon | BossGeneric },
	{ "ActorTypeDragonPriest", EnemyDragonPriest | BossGeneric },
	{ "ActorTypeUndead", EnemyUndead },
	{ "ActorTypeDaedra", EnemyDaedra },
	{ "ActorTypeBandit", EnemyBandit },
	{ "ActorTypeFalmer", EnemyFalmer },
	{ "ActorTypeWerewolf", EnemyWerewolf },
	{ "ActorTypeVampire", EnemyUndead },
	{ "ActorTypeSoldier", CivilWar },
};

void ClassifyEnemies(std::uint64_t& a_bits)
{
	auto* lists = RE::ProcessLists::GetSingleton();
	auto* player = RE::PlayerCharacter::GetSingleton();
	if (!lists || !player) return;

	lists->ForEachHighActor([&](RE::Actor* a_actor) {
		if (!a_actor || a_actor == player) return RE::BSContainer::ForEachResult::kContinue;
		if (!a_actor->IsInCombat()) return RE::BSContainer::ForEachResult::kContinue;

		for (const auto& m : kActorKeywords) {
			if (a_actor->HasKeywordByEditorID(m.keyword)) a_bits |= m.bits;
		}
		const char* name = a_actor->GetName();
		if (name && *name) {
			const std::string_view n{ name };
			if (n.find("Miraak") != std::string_view::npos) a_bits |= EnemyMiraak | BossGeneric;
			if (n.find("Karstaag") != std::string_view::npos) a_bits |= EnemyKarstaag | BossGeneric;
		}
		return RE::BSContainer::ForEachResult::kContinue;
	});

	if (player->IsInCombat()) {
		const char* race = player->GetRace() ? player->GetRace()->GetName() : nullptr;
		if (race && std::string_view{ race }.find("Vampire Lord") != std::string_view::npos) {
			a_bits |= EnemyVampireLord | BossGeneric;
		}
	}
}

int    g_snapshotTick = 0;
double g_combatEndAt = 0.0;         // last "fight is over" verdict (sheathed or fled)
std::uint8_t g_combatEndReason = 0; // 0 none, 1 sheathed, 2 fled
float  g_nearestHostile = -1.0f;    // distance to the closest hostile fighting us, -1 = none

// Cheap 4 Hz pass: is any living hostile actor actually fighting the player, and
// how far away is the closest one?  The distance is what decides "the player ran
// away" (see CombatGate::kFleeDistance) - the AI combat flag keeps burning for a
// long time while enemies search, so it cannot answer that question on its own.
void ScanHostileCombat(bool& a_any, float& a_nearest)
{
	auto* lists = RE::ProcessLists::GetSingleton();
	auto* player = RE::PlayerCharacter::GetSingleton();
	if (!lists || !player) return;

	const auto here = player->GetPosition();
	float      best = -1.0f;

	lists->ForEachHighActor([&](RE::Actor* a_actor) {
		if (!a_actor || a_actor == player || a_actor->IsDead()) return RE::BSContainer::ForEachResult::kContinue;
		if (!a_actor->IsInCombat()) return RE::BSContainer::ForEachResult::kContinue;
		if (!a_actor->IsHostileToActor(player)) return RE::BSContainer::ForEachResult::kContinue;
		const float d = here.GetDistance(a_actor->GetPosition());
		if (best < 0.0f || d < best) best = d;
		return RE::BSContainer::ForEachResult::kContinue;
	});

	a_any = best >= 0.0f;
	a_nearest = best;
}

}  // namespace

std::string DataDir()
{
	static std::string cached;
	if (!cached.empty()) return cached;

	std::error_code ec;
	const auto cwd = fs::current_path(ec);
	if (fs::exists(cwd / "Data" / "Skyrim.esm", ec)) cached = (cwd / "Data").string();
	else if (fs::exists(cwd / "Skyrim.esm", ec)) cached = cwd.string();
	else cached = (cwd / "Data").string();
	return cached;
}

std::string PluginDir() { return (fs::path(DataDir()) / "SKSE" / "Plugins").string(); }

bool ImrTakeover()
{
	void* view = ImrView();
	if (!view) return false;

	ImrLink link;
	std::memcpy(&link, view, sizeof(link));

	const bool on = ImrLinkActive(link, static_cast<std::uint64_t>(GetTickCount64()));
	if (on != g_imrLast) {
		g_imrLast = on;
		if (on) {
			LOG_I("IMR: radio on the air (pid %u) -> local music layer held down", link.pid);
		} else {
			LOG_I("IMR: radio off the air -> local music layer released");
		}
	} else if (on) {
		LOG_D("IMR: still on the air");   // only with bVerbose=1
	}
	return on;
}

std::string DetectLanguage()
{
	if (auto* settings = RE::GameSettingCollection::GetSingleton()) {
		if (auto* s = settings->GetSetting("sLanguage")) {
			if (s->GetType() == RE::Setting::Type::kString) {
				const char* v = s->GetString();
				if (v && *v) return v;
			}
		}
	}
	return "english";
}

GameContext Snapshot()
{
	GameContext c;

	// Read before any of the early returns below: the radio link does not depend
	// on the game state, and a load screen must not look like "radio gone".
	c.imrTakeover = ImrTakeover();

	auto* ui = RE::UI::GetSingleton();
	if (ui) {
		c.mainMenu = ui->IsMenuOpen("Main Menu");
		c.loading = ui->IsMenuOpen("Loading Menu");
		c.paused = ui->GameIsPaused();
		c.inDialogue = ui->IsMenuOpen("Dialogue Menu");
	}

	// While the loading screen is up the engine is between two worlds: the cell
	// the player still points at may already be freed, the high-actor lists are
	// being rebuilt, and the player object is half-swapped.  Nothing here is
	// needed during that window (the music gate is closed anyway), so the whole
	// read is skipped instead of poking at objects that are mid-teardown.
	// This is the guard that keeps "load a save" from ever touching RE state.
	if (c.loading) {
		if (g_gate.Engaged()) {
			g_gate.Reset();
			LOG_D("combat: gate reset (loading screen)");
		}
		g_anyHostileInCombat = false;
		g_nearestHostile = -1.0f;
		c.inWorld = false;
		c.attached = false;
		c.inCombat = false;
		c.engaged = false;
		c.engageReason = 0;
		return c;
	}

	auto* player = RE::PlayerCharacter::GetSingleton();
	if (!player) return c;

	auto* cell = player->GetParentCell();
	if (!cell) return c;

	c.inWorld = true;
	c.cellFormID = cell->GetFormID();
	c.attached = cell->IsAttached();

	auto* ws = cell->GetRuntimeData().worldSpace;
	c.wsFormID = ws ? ws->GetFormID() : 0;

	const bool interior = cell->IsInteriorCell();
	c.bits |= interior ? B(Interior) : B(Exterior);

	auto* cal = RE::Calendar::GetSingleton();
	c.hour = cal ? std::fmod(cal->GetHoursPassed(), 24.0f) : 12.0f;
	if (c.hour < 0.0f) c.hour += 24.0f;

	if (c.hour >= 5.0f && c.hour < 8.0f) { c.band = 0; c.bits |= B(HourMorning); }
	else if (c.hour < 18.0f) { c.band = 1; c.bits |= B(HourDay); }
	else if (c.hour < 22.0f) { c.band = 2; c.bits |= B(HourEvening); }
	else { c.band = 3; c.bits |= B(HourNight); }

	c.riding = player->IsOnMount();
	if (c.riding) c.bits |= B(Riding);

	// Underwater detection: the engine's own bit (it drives the underwater
	// muffle), read straight off the player's bool flags.  No game setting or
	// water-form maths, nothing to get out of sync with what the player sees.
	c.underwater = player->GetActorRuntimeData().boolFlags.all(RE::Actor::BOOL_FLAGS::kUnderwater);

	// Combat is decided by CombatGate, not by Actor::IsInCombat() alone.  The AI
	// flag lands a beat after the swing, stays on through brawls and scripted
	// fights, and says "in combat" even when the player merely swung at
	// something that never fought back - which is what used to start the combat
	// track for no reason.  The gate needs an attack *and* an answer (or an
	// ambush, or a prolonged fight).
	const double now = NowSec();

	// A load screen means the engine is tearing the old world down and building
	// the new one: high-actor lists are being emptied and refilled, cells are
	// being attached mid-flight, and the player object is briefly between two
	// worlds.  Walking that (ProcessLists + a game call per actor for the
	// distance) right there used to run into half-initialised objects.  There is
	// nothing to decide anyway - the music gate is closed while loading - so the
	// scan is skipped and the gate is dropped with it: a fight from the previous
	// world must not leak into the new one.
	const bool worldBusy = c.loading || c.mainMenu || !c.attached;
	if (c.loading || c.mainMenu) {
		if (g_gate.Engaged()) {
			g_gate.Reset();
			LOG_D("combat: gate reset (loading/menu)");
		}
	}
	if (worldBusy) {
		g_anyHostileInCombat = false;
		g_nearestHostile = -1.0f;
	} else if ((g_snapshotTick & 3) == 0) {
		bool  any = false;
		float nearestDist = -1.0f;
		ScanHostileCombat(any, nearestDist);
		g_anyHostileInCombat = any;
		g_nearestHostile = nearestDist;
	}
	g_gate.OnHostileInCombat(now, g_anyHostileInCombat && !worldBusy);

	const bool aiFlag = !worldBusy && player->IsInCombat();

	// Weapon state is the player's own intent, and it is the only signal that
	// says "this fight is over" without waiting for the AI: the combat flag
	// lingers over looting, dialogue and picking a lock, and fCombatHold then
	// keeps the combat track running on top of that.  IsWeaponDrawn() is also
	// true while sheathing, so a swap animation cannot end the fight early.
	bool weaponDrawn = true;
	if (!worldBusy) {
		if (auto* state = player->AsActorState()) weaponDrawn = state->IsWeaponDrawn();
	}

	// The distance goes in with the same sample rate as the scan: the gate only
	// needs to know "closest hostile still fighting us" to tell a retreat from a
	// lull, and 4 Hz is plenty for that.  An empty scan is reported as "very far
	// away" - nobody fighting us *is* the retreat case - while a negative value
	// would mean "not measured" and disable the rule.
	const float nearest = g_nearestHostile >= 0.0f ? g_nearestHostile : 1.0e9f;
	const CombatGate::Verdict verdict = g_gate.Update(now, aiFlag, weaponDrawn, nearest);

	c.engaged = verdict.combat;
	c.engageReason = static_cast<std::uint8_t>(verdict.reason);
	c.inCombat = verdict.combat;
	// Every release advances the timestamp - sheathe, flee *and* timeout - so
	// the director can drop its per-fight state (the arm-time clock in
	// particular).  Without the timeout row a fight that ends by nobody keeping
	// it alive would leave that clock running, and the next quick kill (an
	// assassination) would find the combat layer already armed and cut in at
	// once - exactly what a short kill must never do.
	if (verdict.released) {
		g_combatEndAt = now;
		g_combatEndReason = verdict.bySheathe ? 1 : (verdict.byFlee ? 2 : 3);
	}
	c.combatEndAt = g_combatEndAt;
	c.combatEndReason = g_combatEndReason;
	if (verdict.released) {
		const char* how = verdict.bySheathe ? "sheathe" : (verdict.byFlee ? "flee" : "timeout");
		LOG_I("combat: OFF  by=%s  aiFlag=%d  weaponDrawn=%d  nearest=%.0f", how, aiFlag ? 1 : 0, weaponDrawn ? 1 : 0,
			g_nearestHostile);
	}
	if (verdict.raised) {
		const char* why = "unknown";
		switch (verdict.reason) {
		case EngageReason::Duel: why = "duel (player struck, target struck back)"; break;
		case EngageReason::Ambush: why = "ambush (enemy engaged first)"; break;
		case EngageReason::Sustained: why = "prolonged fight (hostile stayed engaged)"; break;
		default: break;
		}
		LOG_I("combat: ON  reason=%s  aiFlag=%d", why, aiFlag ? 1 : 0);
	}
	if (c.inCombat) {
		c.bits |= B(InCombat);
		// keyword scans are string heavy: refresh at 4 Hz, not every tick
		if ((g_snapshotTick & 3) == 0) ClassifyEnemies(c.bits);
	}
	++g_snapshotTick;

	ApplyNameTags(c.bits, cell->GetFormEditorID() ? cell->GetFormEditorID() : "");
	if (ws && ws->GetFormEditorID()) ApplyNameTags(c.bits, ws->GetFormEditorID());

	if (c.bits & Town) c.bits |= (c.band == 3) ? B(TownNight) : B(TownDay);

	return c;
}

// ------------------------------------------------------- vanilla handover ---
// The plugin replaces the vanilla music system.  Two things decide how that is
// done safely:
//
//  * The only lever that is safe is the music sound category's menu level.
//    Patching BSMusicManager's vtable (the 2.x "takeover" attempt) swallowed
//    kAdd/kRemove events without forwarding them, which left the manager's
//    track queue holding freed objects - a use-after-free at
//    SkyrimSE+0x1AD5B40, confirmed identical in two minidumps.  The level is
//    therefore steered and the manager is never touched.
//
//  * A held-back level is not a mute: it is a *hand-over*.  While this plugin
//    has a track for the current event/location the vanilla layer waits at 0;
//    the moment it has nothing to play the level the game wants is restored and
//    vanilla music plays again.  The value to restore is learned by watching the
//    category (that is also how a Music-slider change or a load-screen reset
//    gets picked up), so the player's own slider is never overwritten by a
//    hard-coded 1.0.
namespace {

float g_vanillaLevel = -1.0f;   // last level the game wanted for the music category
bool  g_vanillaYielded = false; // true while the vanilla layer is held back

bool IsMusicCategory(RE::BGSSoundCategory* a_cat)
{
	if (!a_cat) return false;
	if (a_cat->GetFormID() == kMusicCategoryFormID) return true;
	const char* edid = a_cat->GetFormEditorID();
	return edid && *edid && std::strncmp(edid, "AudioCategoryMUS", 16) == 0;
}

}  // namespace

int SteerVanillaMusic(bool a_yield)
{
	auto* dh = RE::TESDataHandler::GetSingleton();
	if (!dh) return 0;

	int n = 0;
	for (auto* cat : dh->GetFormArray<RE::BGSSoundCategory>()) {
		if (!IsMusicCategory(cat)) continue;
		++n;

		const float level = cat->GetCategoryVolume();
		if (a_yield) {
			// remember what the game wants (the Music slider), then hold it at 0
			if (level > 0.0f) g_vanillaLevel = level;
			if (level != 0.0f) cat->SetCategoryVolume(0.0f);
		} else if (level > 0.0f) {
			g_vanillaLevel = level;  // vanilla is audible: learn the current level
		} else if (g_vanillaYielded) {
			// we are the reason it is 0 - give the context back
			cat->SetCategoryVolume(g_vanillaLevel > 0.0f ? g_vanillaLevel : 1.0f);
		}
	}

	if (a_yield != g_vanillaYielded) {
		g_vanillaYielded = a_yield;
		if (a_yield) {
			LOG_I("vanilla music: taken over (%d category(ies) held back - this plugin has a track here)", n);
		} else {
			LOG_I("vanilla music: handed back (%d category(ies), level %.2f - no track here, vanilla plays)",
				n, g_vanillaLevel > 0.0f ? g_vanillaLevel : 1.0f);
		}
	}
	return n;
}


// ----------------------------------------------------------- combat sensor --
namespace {
double g_lastOffenseLog = -1000.0;
double g_lastIgnoreLog = -1000.0;

void LogOffense()
{
	const double now = NowSec();
	if (now - g_lastOffenseLog < 3.0) return;
	g_lastOffenseLog = now;
	LOG_D("combat sensor: player hit a live actor - waiting for a reaction");
}

class CombatSink final : public RE::BSTEventSink<RE::TESCombatEvent> {
public:
	RE::BSEventNotifyControl ProcessEvent(const RE::TESCombatEvent* a_event, RE::BSTEventSource<RE::TESCombatEvent>*) override
	{
		if (!a_event || a_event->newState.get() != RE::ACTOR_COMBAT_STATE::kCombat) return RE::BSEventNotifyControl::kContinue;

		auto* player = RE::PlayerCharacter::GetSingleton();
		if (!player) return RE::BSEventNotifyControl::kContinue;

		const auto* actor = a_event->actor.get();
		const auto* target = a_event->targetActor.get();
		if (!actor) return RE::BSEventNotifyControl::kContinue;

		// the player's own flag flipping is *not* a trigger: it also flips when
		// an enemy attacks us, and when we swing at something harmless.  The
		// offensive half of a duel comes from TESHitEvent (an actual hit).
		if (actor == player) return RE::BSEventNotifyControl::kContinue;

		// somebody entered combat with the player: retaliation or ambush, the
		// gate decides which from the offense window
		if (target == player) {
			if (!actor->IsDead()) g_gate.OnEnemyEngage(NowSec());
			return RE::BSEventNotifyControl::kContinue;
		}

		// NPC vs NPC: never our business, this was a false positive before
		return RE::BSEventNotifyControl::kContinue;
	}
};

class HitSink final : public RE::BSTEventSink<RE::TESHitEvent> {
public:
	RE::BSEventNotifyControl ProcessEvent(const RE::TESHitEvent* a_event, RE::BSTEventSource<RE::TESHitEvent>*) override
	{
		if (!a_event) return RE::BSEventNotifyControl::kContinue;

		auto* player = RE::PlayerCharacter::GetSingleton();
		if (!player) return RE::BSEventNotifyControl::kContinue;

		auto* target = a_event->target.get();
		auto* cause = a_event->cause.get();
		if (!target) return RE::BSEventNotifyControl::kContinue;

		if (target == player) {
			// a trap, a fall or script damage has no cause - not a fight with
			// anybody, and it used to start the combat track on its own
			if (!cause) return RE::BSEventNotifyControl::kContinue;
			g_gate.OnEnemyHit(NowSec());
			return RE::BSEventNotifyControl::kContinue;
		}

		// Swinging at a corpse, an ore vein or a wall must not start the combat
		// track: only a living actor on the receiving end counts as an attack,
		// and even then it only *arms* the gate - the answer has to come back.
		if (cause == player) {
			auto* victim = target->As<RE::Actor>();
			if (victim && victim != player && !victim->IsDead()) {
				g_gate.OnPlayerHit(NowSec());
				LogOffense();
			} else {
				const double now = NowSec();
				if (now - g_lastIgnoreLog > 5.0) {
					g_lastIgnoreLog = now;
					LOG_D("combat sensor: hit on a non-actor/dead target ignored (ore vein, dummy, corpse)");
				}
			}
		}
		return RE::BSEventNotifyControl::kContinue;
	}
};

CombatSink g_combatSink;
HitSink    g_hitSink;
bool       g_sensorInstalled = false;
}  // namespace

bool InstallCombatSensor()
{
	if (g_sensorInstalled) return true;

	auto* holder = RE::ScriptEventSourceHolder::GetSingleton();
	if (!holder) return false;

	holder->AddEventSink<RE::TESCombatEvent>(&g_combatSink);
	holder->AddEventSink<RE::TESHitEvent>(&g_hitSink);
	g_sensorInstalled = true;

	LOG_I("combat sensor: TESCombatEvent + TESHitEvent sinks installed");
	return true;
}

void RemoveCombatSensor()
{
	if (!g_sensorInstalled) return;
	if (auto* holder = RE::ScriptEventSourceHolder::GetSingleton()) {
		holder->RemoveEventSink<RE::TESCombatEvent>(&g_combatSink);
		holder->RemoveEventSink<RE::TESHitEvent>(&g_hitSink);
	}
	g_gate.Reset();
	g_sensorInstalled = false;
}

}  // namespace cms::platform
