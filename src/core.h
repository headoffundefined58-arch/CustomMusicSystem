// CustomMusicSystem 2.0 - shared types (no Win32 / no RE headers here).
#pragma once

#include <cstdint>
#include <cstdarg>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

namespace cms {

// ---------------------------------------------------------------- logging ---
// Deliberately dependency free (no spdlog): the log is a plain FILE* with a
// small mutex; every line is flushed so a crash never eats the last lines.
void LogOpen(const std::string& path);
void LogClose();
void LogMsg(const char* level, const char* fmt, ...);
void LogFlush();
void SetVerbose(bool a_on);   // when false (default), "debug" level lines are dropped

#define CMS_LOG(level, ...) cms::LogMsg(level, __VA_ARGS__)
#define LOG_I(...) CMS_LOG("info", __VA_ARGS__)
#define LOG_W(...) CMS_LOG("warn", __VA_ARGS__)
#define LOG_E(...) CMS_LOG("error", __VA_ARGS__)
// LOG_D: trace / per-frame noise. Suppressed unless bVerbose=1 in the ini.
#define LOG_D(...) CMS_LOG("debug", __VA_ARGS__)

// ------------------------------------------------------------- game context -
// Every predicate the rule table can ask for is one bit, so resolving a frame
// is 69 integer ANDs and zero allocations.
enum Bit : std::uint64_t {
	Interior = 1ull << 1,
	Exterior = 1ull << 2,
	Riding = 1ull << 3,
	InCombat = 1ull << 4,

	HourMorning = 1ull << 5,
	HourDay = 1ull << 6,
	HourEvening = 1ull << 7,
	HourNight = 1ull << 8,

	Town = 1ull << 9,
	TownDay = 1ull << 10,
	TownNight = 1ull << 11,

	CityWhiterun = 1ull << 12,
	CityMarkarth = 1ull << 13,
	CityRiften = 1ull << 14,
	CitySolitude = 1ull << 15,
	CityWindhelm = 1ull << 16,

	Dungeon = 1ull << 17,
	Cave = 1ull << 18,
	Fort = 1ull << 19,
	Ice = 1ull << 20,
	Crypt = 1ull << 21,
	Mine = 1ull << 22,
	Warrens = 1ull << 23,
	Ruin = 1ull << 24,

	Snow = 1ull << 25,
	ForestFall = 1ull << 26,
	ForestPine = 1ull << 27,
	Tundra = 1ull << 28,
	Mountain = 1ull << 29,
	Marsh = 1ull << 30,
	Coastal = 1ull << 31,
	Volcanic = 1ull << 32,

	SoulCairn = 1ull << 33,
	FalmerValley = 1ull << 34,
	DBArea = 1ull << 35,

	Castle = 1ull << 36,
	College = 1ull << 37,
	Dwemer = 1ull << 38,
	Blackreach = 1ull << 39,
	Temple = 1ull << 40,
	Tavern = 1ull << 41,
	Museum = 1ull << 42,
	TagLibrary = 1ull << 43,

	HomeBreezehome = 1ull << 44,
	HomeVlindrel = 1ull << 45,
	HomeHjerim = 1ull << 46,
	HomeProudspire = 1ull << 47,
	HomeHoneyside = 1ull << 48,

	EnemyDragon = 1ull << 49,
	EnemyVampireLord = 1ull << 50,
	EnemyMiraak = 1ull << 51,
	EnemyKarstaag = 1ull << 52,
	EnemyDragonPriest = 1ull << 53,
	EnemyUndead = 1ull << 54,
	EnemyDaedra = 1ull << 55,
	EnemyBandit = 1ull << 56,
	EnemyFalmer = 1ull << 57,
	EnemyWerewolf = 1ull << 58,
	CivilWar = 1ull << 59,
	BossGeneric = 1ull << 60,

	DBApocrypha = 1ull << 61,
	HomeAny = 1ull << 63
};

constexpr std::uint64_t B(Bit a_bit) { return static_cast<std::uint64_t>(a_bit); }
constexpr std::uint64_t operator|(Bit a, Bit b) { return B(a) | B(b); }
constexpr std::uint64_t operator|(std::uint64_t a, Bit b) { return a | B(b); }

enum class HourBand : std::uint8_t { Morning = 0, Day = 1, Evening = 2, Night = 3 };

// Plain-old-data snapshot of everything the rules can see.  Copied across the
// game/engine thread boundary with a seqlock, so it must stay trivially copyable.
struct GameContext {
	std::uint64_t bits = 0;
	std::uint32_t cellFormID = 0;
	std::uint32_t wsFormID = 0;
	float         hour = 12.0f;
	std::uint8_t  band = 1;
	bool          inWorld = false;
	bool          attached = false;
	bool          loading = false;
	bool          mainMenu = false;
	bool          paused = false;
	bool          inCombat = false;
	bool          engaged = false;  // combat sensor fired (hit / engage event)
	std::uint8_t  engageReason = 0;  // cms::EngageReason, 0 = none
	// Time of the last "the fight is over" verdict - the player sheathed or
	// broke away.  A timestamp, not a pulse: the engine thread samples at 20 Hz
	// while the game thread ticks at 60+, so a one-frame flag would simply be
	// missed.  The director drops its "hold the combat layer" timer when this
	// advances, which is what makes either ending fade out at once instead of
	// waiting out fCombatHold.
	double        combatEndAt = 0.0;
	std::uint8_t  combatEndReason = 0;  // 0 none, 1 sheathed, 2 fled, 3 timeout
	bool          inDialogue = false;
	bool          riding = false;
	// The player's head is below the water surface (engine BOOL_FLAGS::kUnderwater,
	// the same bit that drives the muffle effect).  No music at all down there -
	// ours or the vanilla layer's - so underwater ambience mods own that space.
	bool          underwater = false;
	// Interactive Music Radio is live and streaming an external source, so the
	// local music layer is held down and the radio is what the player hears.
	// Comes from the IMR handshake block (see ImrLink below), never from the UI.
	bool          imrTakeover = false;

	[[nodiscard]] std::uint64_t Fingerprint() const
	{
		std::uint64_t h = bits;
		h ^= static_cast<std::uint64_t>(cellFormID) * 0x9E3779B97F4A7C15ull;
		h ^= static_cast<std::uint64_t>(wsFormID) << 21;
		h ^= static_cast<std::uint64_t>(band) << 3;
		h += static_cast<std::uint64_t>(hour * 20.0f);
		return h;
	}
};

// ------------------------------------------------------- IMR handshake link -
// Interactive Music Radio is a separate SKSE plugin in its own DLL.  The two
// are never linked and never share a symbol: they meet in one named shared
// section ("IMR_CMS_HANDSHAKE"), 64 bytes wide, laid out exactly like the
// structure below.  IMR writes, CMS only reads.
//
// The block carries a heartbeat on purpose.  A state word alone would stay at
// "takeover" forever if IMR crashed or was killed mid-session, and CMS would go
// permanently silent with no way back short of a restart.  With a timestamp the
// link simply expires: if IMR has not touched the block for kImrHeartbeatMaxMs,
// CMS takes the context back on its own.
//
// Layout is fixed-width and versioned so a future IMR can add fields without
// moving the ones CMS already reads.
constexpr std::uint32_t kImrMagic = 0x48524D49u;   // 'IMRH' little-endian
constexpr std::uint32_t kImrVersion = 1u;
constexpr std::uint64_t kImrHeartbeatMaxMs = 3000u;   // 3 s of silence = gone

struct ImrLink {
	std::uint32_t magic = 0;
	std::uint32_t version = 0;
	std::uint32_t state = 0;   // 0 idle, 1 takeover (radio is on the air)
	std::uint32_t pid = 0;     // writer process, for the log only
	std::uint64_t heartbeatMs = 0;   // writer's GetTickCount64() at last write
	std::uint32_t flags = 0;   // reserved; bit 0 = an external source is audible
	std::uint32_t pad[9] = {};      // pad to 64 bytes
};

static_assert(sizeof(ImrLink) == 64, "ImrLink must match the shared block size");

// True only when the block is well-formed, someone is holding the link, and the
// heartbeat is fresh.  Pure arithmetic, so it is unit-tested off-machine.
[[nodiscard]] bool ImrLinkActive(const ImrLink& a_link, std::uint64_t a_nowMs,
	std::uint64_t a_maxAgeMs = kImrHeartbeatMaxMs);

// ------------------------------------------------------------------ config --
// The library is always <Data>\Music.  It is a constant, not a setting: the
// folder is read-only by design, so nothing in the menu or the ini can point the
// scanner somewhere else.
constexpr const char* kLibraryFolder = "Music";

struct Config {

	// transitions (seconds)
	float fadeLocation = 3.0f;
	float fadeCombatEnter = 1.2f;
	float fadeCombatExit = 2.5f;
	// Fade used when this plugin has nothing for the current event/location and
	// hands the context back to the vanilla layer (and when the master switch
	// turns everything off).  It is a hand-back, not a mute: the vanilla music
	// takes over as our voice ramps out.
	float fadeHandback = 2.5f;
	float combatHold = 2.5f;
	float worldSettle = 1.2f;

	// switch guard (seconds): how a "the context changed" request is turned into
	// an actual track change.  Without it a doorway, a cell border or a firefight
	// that starts during a crossfade retriggers the layer immediately, which is
	// what used to put two tracks on top of each other.
	//   fMinPlay        - a track plays at least this long before being replaced
	//                     (short clips and the "nearly over" case are exempt)
	//   fSwitchSettle   - the *new* resolution must stay stable this long first
	//   fSwitchCooldown - hard floor between two changes of the same layer
	float minPlay = 12.0f;
	float switchSettle = 1.5f;
	float switchCooldown = 4.0f;

	// behaviour
	int  recentWindow = 3;
	int  minTracks = 1;
	// A fight the player started (duel) or one that ambushed the player both end
	// when the player sheathes: that is the one signal that is both deliberate
	// and immediate, unlike the AI flag which lingers over loot and dialogue.
	bool sheatheEndsCombat = true;
	// Vanilla music.  The plugin *replaces* the vanilla music system by
	// default: for as long as it has a track for the current event/location the
	// vanilla layer is held back, and the moment it has nothing to play the
	// vanilla layer is handed the context back (fallback) - a place the library
	// does not cover still gets the vanilla music for that place instead of
	// silence.  There is no "mute" mode any more: nothing ever leaves the game
	// without music because of this setting.
	//   false = takeover (default)   true = keep vanilla playing alongside
	bool keepVanilla = false;

	// ui
	std::string uiLanguage = "auto";  // auto | en | zh (menu strings)

	// audio
	bool  enabled = true;            // master switch (menu "Enable music"); false = all voices stop
	// The volume page owns the master level (persisted in the .mixer file).  There
	// is no seed value and no dependency on the game's Music slider: the level a
	// fresh install starts at is 100 %, and afterwards only the page changes it.
	// Exclusive handover.  Exactly one track is audible at a time: when a new
	// event (a fight) takes over, the track that is playing is stopped *first*
	// and the new one only starts once that voice has gone silent, so two
	// pieces never sound together.  This is the fade used for the outgoing
	// track while it stops (kept short so a takeover still feels immediate).
	float fadeSwap = 0.35f;
	// Minimum fight duration before the combat track is allowed to start.  The
	// gate already debounces the *trigger*; this debounces the *insertion* so a
	// momentary scuffle the gate still flags (a single hit, a brawl that breaks
	// off in a second, an enemy that immediately disengages) never pops a combat
	// track over the music that is already playing.  A real fight is longer than
	// this and starts seamlessly once it has proved it is real.  0 = start at once
	// (the old, trigger-happy behaviour).
	float combatArmTime = 2.0f;
	bool  duckInDialogue = true;
	float dialogueDuck = 0.45f;
	float changeSmooth = 0.30f;      // 滑块/增益变化的平滑时间
	bool  mixerAutosave = true;      // 菜单里改完自动写盘

	// engine (ini only - not shown in the menu)
	int  tickMs = 50;             // engine tick (20 Hz)
	bool singleFormatCheck = true; // log a warning per folder that mixes .xwm/.wav

	// logging
	bool verbose = false;         // bVerbose: 1 = also emit the trace/debug lines

	std::string iniPath;   // where it was read from (diagnostics)
	std::string dataDir;   // <game>\Data
	std::string pluginDir; // SKSE\Plugins

	// Parse "key = value" lines; sections are ignored, ';' starts a comment.
	bool LoadFromFile(const std::string& path);
	// Rewrites the whole file in the canonical (commented) shape.  The previous
	// file is kept as <path>.bak the first time, so a bad edit is recoverable.
	bool SaveToFile(const std::string& path) const;
	void ApplyKey(std::string_view key, std::string_view value);
};

// ------------------------------------------------------------ combat gate --
// Why this exists: the AI flag (Actor::IsInCombat) is not a good trigger on its
// own.  It lands late, it stays on for brawls and scripted fights, and it says
// "the player is in combat" even when the player merely swung at something that
// never fought back.  The gate below only opens when the fight is *confirmed*:
//
//   Duel     - the player damaged a live actor AND that side answered AND the
//              exchange kept going for kDuelConfirm.  Striking first is *not* a
//              combat event by itself: a sneak attack the target dies in, or one
//              against something that never really fights back, must not start
//              the combat track.  Only a fight that is still on kDuelConfirm
//              after the answer counts.
//   Ambush   - an enemy engaged or hit the player with no player offense first
//              (a hidden archer, a dragon landing, a scripted ambush).  The
//              enemy attacking first IS a fight, at once.
//   Sustained- fallback: a hostile actor stays in combat with the player for
//              longer than kSustain.  Covers fights whose events got eaten.
//
// Closing the gate - three ways, all of them the *player's* decision:
//   Sheathe  - weapon stays down for kSheatheConfirm.  The AI flag lingers over
//              looting, dialogue and lockpicking, so it cannot be the trigger.
//   Retreat  - the nearest hostile that still targets the player is farther than
//              kFleeDistance for kFleeConfirm.  Running away is a way of ending
//              the fight; Skyrim however keeps the AI flag (and therefore the
//              combat music) alive while enemies "search" for a player who has
//              long left, so the flag cannot be the trigger here either.
//   Timeout  - nobody confirmed the fight for kHold after the AI flag dropped.
//
// Pure time arithmetic, no game API, so tests/tests.cpp can drive every branch.
enum class EngageReason : std::uint8_t { None = 0, Duel = 1, Ambush = 2, Sustained = 3 };

class CombatGate {
public:
	struct Verdict {
		bool         combat = false;
		EngageReason reason = EngageReason::None;
		bool         raised = false;    // true only on the tick the gate opened
		bool         released = false;  // true only on the tick the gate closed
		bool         bySheathe = false; // ...and it closed because the player sheathed
		bool         byFlee = false;    // ...or because the player ran away
	};

	static constexpr double kOffenseWindow = 6.0;  // "the player started it" stays true this long
	// A duel the player started is only a fight once it has lasted: after the
	// enemy answered, the exchange has to keep going this long before the gate
	// opens.  An assassination - one hit, the target dead before anything can
	// really fight back - ends inside this window and never becomes a combat
	// event, which is the whole point.
	static constexpr double kDuelConfirm = 2.0;
	// The AI flag can lag the answer event by a tick or two; the pending duel is
	// only cancelled once the flag has been off this long (or the player
	// sheathed), not on the first quiet snapshot.
	static constexpr double kDuelGrace = 0.75;
	static constexpr double kHold = 2.0;           // a hit keeps combat alive without the AI flag
	static constexpr double kSustain = 2.5;        // AI-only combat must persist this long
	static constexpr double kSheatheConfirm = 0.5; // weapon stays down this long -> fight over
	// Retreat: distance to the nearest hostile that still has the player as a
	// combat target.  One pursuer left behind keeps the track alive, so this is
	// measured against the *nearest* one, not an average.  3000 units is about
	// 43 m - well past melee, bow and most dragon distances, but still inside
	// the range where a fight can be picked back up by simply turning around.
	static constexpr float  kFleeDistance = 3000.0f;
	static constexpr double kFleeConfirm = 3.0;    // seconds spent beyond that range

	void OnPlayerHit(double a_now);                     // player damaged a live actor
	void OnEnemyEngage(double a_now);                   // an enemy's combat state flipped onto the player
	void OnEnemyHit(double a_now);                      // an enemy damaged the player
	void OnHostileInCombat(double a_now, bool a_any);   // per-tick scan of hostile high actors
	// a_weaponDrawn is the player's own weapon state; false + engaged_ for
	// kSheatheConfirm closes the fight regardless of what the AI still thinks.
	// a_nearestHostile is the distance to the nearest hostile in combat with the
	// player, or a negative value when that is not known (no distance input =
	// no retreat rule, which is what the callers that do not scan pass).  A
	// scanner that finds nobody must pass a *large* number, not -1: "nobody is
	// fighting us" is exactly the retreat case, "we did not look" is not.
	Verdict Update(double a_now, bool a_aiInCombat, bool a_weaponDrawn = true, float a_nearestHostile = -1.0f);
	void    Reset();

	[[nodiscard]] bool         Engaged() const { return engaged_; }
	[[nodiscard]] EngageReason Reason() const { return reason_; }
	// true while a retreat must not be undone by the "prolonged fight" fallback
	[[nodiscard]] bool Fled() const { return fleeLock_; }

private:
	bool RecentOffense(double a_now) const { return a_now - offenseAt_ < kOffenseWindow; }
	void PendDuel(double a_now);  // the enemy answered an offense: the duel is pending, not confirmed
	void Engage(double a_now, EngageReason a_reason);
	void Release();

	double       offenseAt_ = -1.0e9;
	double       duelSince_ = -1.0;     // >= 0: the enemy answered an offense; the duel is pending confirmation
	double       duelIdleSince_ = -1.0; // first quiet tick of a pending duel (grace before cancelling)
	double       engagedAt_ = -1.0e9;
	double       sheathedAt_ = -1.0;  // < 0 = weapon drawn / unknown
	double       sustainSince_ = -1.0;  // < 0 = no hostile in combat
	double       fleeSince_ = -1.0;     // < 0 = somebody is still close
	bool         fleeLock_ = false;     // released by retreat, not yet re-engaged
	bool         engaged_ = false;
	bool         raised_ = false;
	EngageReason reason_ = EngageReason::None;
};

// ------------------------------------------------------------ switch guard --
// Decides whether a layer may change track *now*.  Split out of Director so the
// whole rule set is testable without an audio device: every argument is a
// number or a bool, and the return value is the reason to wait (nullptr = go).
//
//   settle    the new resolution must persist - a doorway or a cell border
//             must not swap the pool twice in one second
//   active    nothing on the slot means nothing to interrupt
//   fading    never launch a track on top of a running crossfade
//   minPlay   the current piece keeps playing; nearlyEnd is its exception,
//             because changing there *is* the natural handover
//   cooldown  hard floor between two changes of the same layer
[[nodiscard]] const char* SwitchBlockReason(double a_now, double a_started, double a_switched, double a_wantSince,
    bool a_active, bool a_fading, float a_duration, float a_playhead, float a_minPlay, float a_settle,
    float a_cooldown, float a_fade);

// ----------------------------------------------------------------- library --
struct TrackRef {
	std::string path;
	std::uint64_t size = 0;
};

struct Folder {
	std::string           name;    // "Explore - Day"
	std::string           prefix;  // "ExploreDay"
	int                   limit = 20;
	std::vector<TrackRef> tracks;
	bool                  scanned = false;
	bool                  mixedFormat = false;  // both .xwm and .wav present
	std::uint64_t         stamp = 0;            // mtime of the folder, for hot rescan
};

struct Library {
	std::string         root;     // <Data>\Music
	std::vector<Folder> folders;

	int  IndexOf(std::string_view name) const;
	Folder* Find(std::string_view name);
	bool Scan(bool force, bool* outChanged = nullptr);
	std::uint64_t TotalBytes() const;
};

// ------------------------------------------------------------------- rules --
enum class Layer : std::uint8_t { Ambient = 0, Combat = 1 };

// Rule family.  Additive rules ("Explore - Day", "Dungeon - All", "Combat - All")
// only merge into an anchor from the *same* family - otherwise a town cell would
// pull in exploration tracks simply because it is also an exterior cell.
enum class Group : std::uint8_t { Explore = 0, Ride, Dungeon, Combat, Town, Interior, Home };

struct Rule {
	std::string_view folder;
	std::string_view prefix;
	std::uint8_t     priority = 0;   // larger = stronger
	Layer            layer = Layer::Ambient;
	bool             additive = false;
	bool             replace = false;
	std::uint64_t    mask = 0;
	int              folderIndex = -1;  // resolved against the library at start
	std::uint8_t     limit = 20;        // default "Track limit N"
	Group            group = Group::Explore;
};

struct RuleTable {
	std::vector<Rule> rules;
	void Build();  // fills the 69 built-in entries
	void Bind(Library& a_lib);
};

struct Resolution {
	int                      anchor = -1;  // index into RuleTable::rules
	std::vector<std::int32_t> folders;     // library folder indices, deduped
	std::uint64_t            hash = 0;
	std::string              label;        // "<anchor>/<additive>/..."
	bool                     empty() const { return folders.empty(); }
};

// Resolve one layer for a context snapshot (no allocation beyond `out`).
void Resolve(const RuleTable& a_rules, const Library& a_lib, std::uint64_t a_bits, Layer a_layer, Resolution& out);

}  // namespace cms
