// Off-line checks for the parts that do not need a game: the rule table, the
// family filter, the ride replacement and the pool identity hash.
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "core.h"
#include "i18n.h"
#include "volume.h"
#include "menu_page.h"

using namespace cms;

static int g_fail = 0;
static int g_pass = 0;

#define CHECK(cond)                                                       \
	do {                                                                  \
		if (cond) { ++g_pass; }                                           \
		else { ++g_fail; std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } \
	} while (0)

namespace {

void AddTracks(Library& a_lib, const char* a_folder, int a_count)
{
	for (auto& f : a_lib.folders) {
		if (f.name != a_folder) continue;
		f.tracks.clear();
		for (int i = 1; i <= a_count; ++i) {
			char buf[512];
			std::snprintf(buf, sizeof(buf), "%s/MM%s%02d.xwm", f.name.c_str(), f.prefix.c_str(), i);
			f.tracks.push_back({ buf, 1000 });
		}
		return;
	}
	std::printf("FAIL missing folder %s\n", a_folder);
	++g_fail;
}

bool HasFolder(const Resolution& a_r, const Library& a_lib, const char* a_name)
{
	for (const auto fi : a_r.folders) {
		if (fi >= 0 && fi < static_cast<int>(a_lib.folders.size()) && a_lib.folders[static_cast<std::size_t>(fi)].name == a_name) return true;
	}
	return false;
}

std::size_t FolderCount(const Resolution& a_r) { return a_r.folders.size(); }

}  // namespace

// --- mixer: three levels, mute, persistence, path mapping --------------------
static void VolumeTests()
{
	VolumeMix vol;
	CHECK(vol.MasterEffective() == 1.0f);

	vol.SetMaster(0.5f);
	CHECK(vol.MasterEffective() == 0.5f);
	CHECK(vol.Master().gain == 0.5f);

	// no mute flag exists any more: the level *is* the state, so 0 % and 100 %
	// are the only two ends and nothing can silently stay at zero
	vol.SetMaster(0.0f);
	CHECK(vol.MasterEffective() == 0.0f);
	vol.SetMaster(0.5f);

	// 150 % head room, then clamp
	vol.SetMaster(9.0f);
	CHECK(vol.Master().gain == VolumeMix::kMaxGain);
	CHECK(vol.MasterEffective() == VolumeMix::kMaxGain);

	// round trip of the unified file
	const std::string path = "cms_volume_test.txt";
	CHECK(vol.Save(path));
	VolumeMix loaded;
	CHECK(loaded.Load(path));
	CHECK(loaded.Master().gain == VolumeMix::kMaxGain);
	CHECK(loaded.LegacyRows() == 0);
	CHECK(loaded.MasterEffective() == VolumeMix::kMaxGain);

	// an old three-level mixer file: the master survives, the per-folder and
	// per-track rows are dropped instead of silently quietening one track
	{
		std::ofstream out(path, std::ios::binary);
		out << "m\tmaster\t0.8000\t1\n"  // the old mute column must be ignored
			<< "f\tCombat\t0.4000\t0\n"
			<< "t\tExplore - Day/MMExploreDay01.xwm\t0.1000\t0\n"
			<< "t\tCombat/MMCombat01.xwm\t0.1000\t1\n";
	}
	VolumeMix legacy;
	CHECK(legacy.Load(path));
	CHECK(legacy.Master().gain > 0.79f && legacy.Master().gain < 0.81f);
	CHECK(legacy.MasterEffective() > 0.79f);
	CHECK(legacy.LegacyRows() == 3);

	// saving rewrites the file in the one-level format
	CHECK(legacy.Save(path));
	VolumeMix rewritten;
	CHECK(rewritten.Load(path));
	CHECK(rewritten.LegacyRows() == 0);
	CHECK(rewritten.Master().gain > 0.79f && rewritten.Master().gain < 0.81f);

	std::remove(path.c_str());
}

static void I18nTests()
{
	// The language is a compile-time constant: this binary either carries the
	// Chinese table or the English one, and that is the only thing that decides
	// what the pages show.  Both tables still have to be complete, so a build
	// never falls back to an empty label.
	const bool zh = cms::i18n::kChineseBuild;

	cms::i18n::Init("C:/nonexistent", "english", "chinese");
	CHECK(std::string(cms::i18n::Lang()) == (zh ? "zh" : "en"));

	// menu section names are single-language now
	const std::string title = cms::i18n::T(cms::i18n::S::MenuCategory);
	if (zh) CHECK(title == "个性化音乐系统");
	else CHECK(title == "Custom Music System");
	CHECK(title.find('/') == std::string::npos);

	// every entry has text, and the active table is not the other language's
	const std::string master = cms::i18n::T(cms::i18n::S::MasterTitle);
	if (zh) CHECK(master == "主音量");
	else CHECK(master == "Master volume");

	for (int i = 0; i < static_cast<int>(cms::i18n::S::Count); ++i) {
		CHECK(cms::i18n::T(static_cast<cms::i18n::S>(i))[0] != '\0');
	}

	// the build flag is what the settings page reports
	CHECK(cms::i18n::kChineseBuild == zh);
}

// --- the switch guard: every reason a layer must wait ----------------------
// These are the exact rules that stop "just entered a new cell -> music changed"
// and "two tracks at once".  Driving the pure function keeps them verifiable
// without XAudio2.
static void SwitchGuardTests()
{
	using cms::SwitchBlockReason;

	// baseline: nothing playing, target already settled -> go
	auto reason = [](double now, double started, double switched, double want, bool active, bool fading, float dur,
					  float pos) {
		const char* r = SwitchBlockReason(now, started, switched, want, active, fading, dur, pos,
			12.0f, 1.5f, 4.0f, 3.0f);
		return r ? std::string(r) : std::string{};
	};

	// 1. the new resolution has not been stable long enough
	CHECK(reason(10.0, -100.0, -100.0, 9.0, true, false, 180.0f, 60.0f) == "settling");
	// wantSince < 0 means "no target at all"
	CHECK(reason(10.0, -100.0, -100.0, -1.0, true, false, 180.0f, 60.0f) == "settling");

	// 2. nothing on the slot: start immediately (settle aside)
	CHECK(reason(10.0, -100.0, -100.0, 0.0, false, false, 0.0f, 0.0f).empty());

	// 3. a crossfade is still running
	CHECK(reason(10.0, 2.0, 2.0, 0.0, true, true, 180.0f, 8.0f) == "crossfade running");

	// 4. the current piece has not had its minimum time yet
	CHECK(reason(10.0, 5.0, 5.0, 0.0, true, false, 180.0f, 5.0f) == "min play time");

	// 5. ...but a piece that is nearly over may be replaced (natural handover)
	CHECK(reason(10.0, 5.0, 5.0, 0.0, true, false, 180.0f, 178.5f).empty());

	// 6. cooldown between two changes of the same layer
	CHECK(reason(10.0, -100.0, 8.0, 0.0, true, false, 180.0f, 60.0f) == "cooldown");

	// 7. after minPlay + cooldown the switch goes through
	CHECK(reason(20.0, 1.0, 1.0, 18.0, true, false, 180.0f, 19.0f).empty());

	// 8. a short clip: minPlay must not lock a 6 s piece out of its own
	//    handover, but the cooldown is still the last line of defence
	CHECK(reason(3.0, 0.0, 0.0, 0.0, true, false, 6.0f, 3.5f) == "cooldown");
	CHECK(reason(4.5, 0.0, 0.0, 0.0, true, false, 6.0f, 4.5f).empty());

	// 9. the settle window is short but mandatory (1.5 s here)
	CHECK(reason(10.0, -100.0, -100.0, 9.5, false, false, 0.0f, 0.0f) == "settling");
	CHECK(reason(10.6, -100.0, -100.0, 9.0, false, false, 0.0f, 0.0f).empty());
}

// --- sheathing ends a confirmed fight, without waiting for the AI ----------
static void SheatheTests()
{
	using cms::CombatGate;

	// a duel: player hit, enemy answered - and the exchange has to *last* before
	// it is a fight (kDuelConfirm)
	CombatGate gate;
	gate.OnPlayerHit(0.0);
	gate.OnEnemyEngage(1.0);
	CHECK(!gate.Update(1.0, true, true).combat);          // answered, but only pending
	const auto v1 = gate.Update(3.1, true, true);          // ...2 s of real fighting later
	CHECK(v1.combat);
	CHECK(v1.reason == cms::EngageReason::Duel);
	CHECK(v1.raised);

	// weapon down for a moment: still inside the confirmation window
	CHECK(gate.Update(3.3, true, false).combat);
	// a weapon swap (down then up again) must not end the fight
	CHECK(gate.Update(3.5, true, true).combat);
	CHECK(gate.Update(3.7, true, false).combat);  // new window starts here
	// ...and 0.5 s later the fight is over even though the AI still says combat
	const auto v2 = gate.Update(4.3, true, false);
	CHECK(!v2.combat);
	CHECK(v2.released);
	CHECK(v2.bySheathe);

	// an ambush ends the same way
	CombatGate ambush;
	ambush.OnEnemyEngage(0.0);
	CHECK(ambush.Update(0.0, true, true).reason == cms::EngageReason::Ambush);
	CHECK(ambush.Update(1.0, true, false).combat);
	const auto v3 = ambush.Update(1.6, true, false);
	CHECK(!v3.combat);
	CHECK(v3.bySheathe);

	// the AI flag alone still releases, but by timeout, not by sheathe
	CombatGate timeout;
	timeout.OnEnemyHit(0.0);
	CHECK(timeout.Update(0.0, true, true).combat);
	CHECK(timeout.Update(1.0, false, true).combat);   // inside kHold
	const auto v4 = timeout.Update(3.0, false, true);
	CHECK(!v4.combat);
	CHECK(v4.released);
	CHECK(!v4.bySheathe);
}

// --- retreat: running away ends the event --------------------------------
static void FleeTests()
{
	using cms::CombatGate;

	// 1. a duel, then the player legs it: music must stop even though every AI
	//    flag still says "in combat" (that is the whole point of the rule)
	{
		CombatGate gate;
		gate.OnPlayerHit(0.0);
		gate.OnEnemyEngage(1.0);
		CHECK(!gate.Update(1.0, true, true, 150.0f).combat);  // pending...
		CHECK(gate.Update(3.1, true, true, 150.0f).combat);   // ...now a real fight

		// still inside the confirmation window
		CHECK(gate.Update(4.0, true, true, 3500.0f).combat);   // the clock starts here
		CHECK(gate.Update(5.5, true, true, 3500.0f).combat);
		const auto v = gate.Update(7.1, true, true, 3500.0f);
		CHECK(!v.combat);
		CHECK(v.released);
		CHECK(v.byFlee);
		CHECK(!v.bySheathe);
	}

	// 2. one pursuer left behind keeps the track alive
	{
		CombatGate gate;
		gate.OnEnemyEngage(0.0);
		CHECK(gate.Update(0.0, true, true, 400.0f).combat);
		CHECK(gate.Update(1.0, true, true, 3200.0f).combat);  // out of range, timer starts
		CHECK(gate.Update(3.5, true, true, 900.0f).combat);   // caught up: timer resets
		CHECK(gate.Update(8.0, true, true, 900.0f).combat);
	}

	// 3. after a retreat the "prolonged fight" fallback must not restart the
	//    music while the enemy is still searching for the player far away
	{
		CombatGate gate;
		gate.OnEnemyEngage(0.0);
		CHECK(gate.Update(0.0, true, true, 300.0f).combat);
		CHECK(gate.Update(2.0, true, true, 5000.0f).combat);
		CHECK(gate.Update(5.1, true, true, 5000.0f).byFlee);
		CHECK(gate.Fled());

		for (double t = 5.2; t < 60.0; t += 0.5) {
			gate.OnHostileInCombat(t, true);  // they never drop the flag
			CHECK(!gate.Update(t, true, true, 5000.0f).combat);
		}
		CHECK(gate.Fled());
	}

	// 4. walking back into range re-arms the gate, but only through a real
	//    encounter - not through the stale fallback timer
	{
		CombatGate gate;
		gate.OnEnemyEngage(0.0);
		CHECK(gate.Update(0.0, true, true, 300.0f).combat);
		CHECK(gate.Update(2.0, true, true, 6000.0f).combat);  // timer starts
		CHECK(gate.Update(5.1, true, true, 6000.0f).byFlee);
		CHECK(gate.Fled());

		gate.OnHostileInCombat(6.0, true);
		CHECK(!gate.Update(6.0, true, true, 400.0f).combat);  // back close, but no new hit
		CHECK(!gate.Fled());
		CHECK(gate.Update(8.6, true, true, 400.0f).combat);   // fresh 2.5 s fallback ran out
	}

	// 5. nobody fighting us at all counts as retreat as well (the scanner reports
	//    an empty result as a huge distance, not as -1 = "not measured")
	{
		CombatGate gate;
		gate.OnEnemyEngage(0.0);
		CHECK(gate.Update(0.0, true, true, 1.0e9f).combat);
		CHECK(gate.Update(2.5, true, true, 1.0e9f).combat);
		CHECK(gate.Update(3.2, true, true, 1.0e9f).byFlee);
	}

	// 6. a sniper plinking at us resets the timer - being shot at from a hill is
	//    not a retreat, no matter how far away the shooter is
	{
		CombatGate gate;
		gate.OnEnemyEngage(0.0);
		CHECK(gate.Update(0.0, true, true, 250.0f).combat);
		for (double t = 1.0; t < 20.0; t += 2.0) {
			gate.OnEnemyHit(t);  // hit lands from 4000 units away
			CHECK(gate.Update(t, true, true, 4000.0f).combat);
		}
	}

	// 7. sheathing still ends it first when the player stops to put the weapon away
	{
		CombatGate gate;
		gate.OnEnemyEngage(0.0);
		CHECK(gate.Update(0.0, true, true, 200.0f).combat);
		CHECK(gate.Update(1.0, true, false, 200.0f).combat);
		const auto v = gate.Update(1.6, true, false, 200.0f);
		CHECK(v.bySheathe);
		CHECK(!v.byFlee);
	}
}


// --- the volume page, driven by a recorder --------------------------------
// The page is pure logic over an "imgui api"; feeding it a recorder proves the
// widget sequence is balanced (unmatched BeginChild / PushID / PushStyleColor is
// an ImGui assertion, i.e. a crash in game) and that a slider edit really
// reaches the volume model.
namespace mock {
struct Rec {
	int depth = 0;
	int maxDepth = 0;
	int pushes = 0;
	int pops = 0;
	int colors = 0;
	int colorPops = 0;
	int sliders = 0;
	int intSliders = 0;
	int radios = 0;
	int inputs = 0;
	int checks = 0;
	int buttons = 0;
	int beginChild = 0;
	int endChild = 0;
	int texts = 0;
	int separators = 0;
	int wraps = 0;
	int wrapPops = 0;

	std::vector<std::string> ids;
	std::string              fireId;   // row id whose slider reports a change
	float                    fireValue = 0.0f;
	std::string              fireCheck;
	std::string              fireButton;
	int                      fireRadio = -1;  // index of the radio button that reports a click
	int                      fireRadioGroup = -1;  // which RadioGroup it belongs to

	[[nodiscard]] std::string Cur() const { return ids.empty() ? std::string() : ids.back(); }
};

Rec g;

void Text(const char*, ...) { ++g.texts; }
void TextRaw(const char*, const char*) { ++g.texts; }
bool Slider(const char* a_label, float* a_v, float, float, const char*, int)
{
	++g.sliders;
	if (!g.fireId.empty() && (g.Cur() == g.fireId || g.fireId == a_label)) {
		*a_v = g.fireValue;
		g.fireId.clear();
		return true;
	}
	return false;
}
bool Checkbox(const char* a_label, bool* a_v)
{
	++g.checks;
	if (!g.fireCheck.empty() && (g.Cur() == g.fireCheck || g.fireCheck == a_label)) {
		*a_v = !*a_v;
		g.fireCheck.clear();
		return true;
	}
	return false;
}
bool SliderInt(const char*, int*, int, int, const char*, int)
{
	++g.intSliders;
	return false;
}
bool Radio(const char*, int* a_v, int a_button)
{
	++g.radios;
	// every RadioGroup on the page has three buttons, so the running call count
	// also tells which group this is (vanilla mode = 0, language = 1, ...)
	if (g.fireRadio == a_button && g.fireRadioGroup == (g.radios - 1) / 3) {
		*a_v = a_button;
		g.fireRadio = -1;
		return true;
	}
	return false;
}
bool InputText(const char*, char*, std::size_t, int, void*, void*)
{
	++g.inputs;
	return false;
}
bool Button(const char* a_label, cms::menupage::Vec2)
{
	++g.buttons;
	if (!g.fireButton.empty() && a_label == g.fireButton) {
		return true;
	}
	return false;
}
void SameLine(float, float) {}
void SeparatorText(const char*) { ++g.separators; }
void Separator() { ++g.separators; }
void Spacing() {}
bool BeginChild(const char*, cms::menupage::Vec2, int, int)
{
	++g.beginChild;
	++g.depth;
	if (g.depth > g.maxDepth) g.maxDepth = g.depth;
	return true;
}
void EndChild()
{
	++g.endChild;
	--g.depth;
}
void PushID(const char* a_id)
{
	++g.pushes;
	g.ids.push_back(a_id);
}
void PopID()
{
	++g.pops;
	if (!g.ids.empty()) g.ids.pop_back();
}
void SetWidth(float) {}
void Progress(float, cms::menupage::Vec2, const char*) {}
void PushColor(int, cms::menupage::Vec4) { ++g.colors; }
void PopColor(int a_n) { g.colorPops += a_n; }
void PushWrap(float) { ++g.wraps; }
void PopWrap() { ++g.wrapPops; }

cms::menupage::Api Make()
{
	cms::menupage::Api api;
	api.text = &Text;
	api.textRaw = &TextRaw;
	api.sliderFloat = &Slider;
	api.checkbox = &Checkbox;
	api.button = &Button;
	api.sameLine = &SameLine;
	api.separatorText = &SeparatorText;
	api.separator = &Separator;
	api.spacing = &Spacing;
	api.beginChild = &BeginChild;
	api.endChild = &EndChild;
	api.pushID = &PushID;
	api.popID = &PopID;
	api.setItemWidth = &SetWidth;
	api.progressBar = &Progress;
	api.pushStyleColor = &PushColor;
	api.popStyleColor = &PopColor;
	api.sliderInt = &SliderInt;
	api.radioButton = &Radio;
	api.inputText = &InputText;
	api.pushTextWrapPos = &PushWrap;
	api.popTextWrapPos = &PopWrap;
	return api;
}
}  // namespace mock

static void MenuPageTests()
{
	cms::i18n::Init("C:/nonexistent", "english", "english");

	cms::VolumeMix mix;
	cms::Config    cfg;
	auto           api = mock::Make();
	CHECK(api.Ready());

	cms::menupage::LibraryStats stats;
	stats.folders = 26;
	stats.tracks = 48;

	cms::menupage::State st;
	st.gateOpen = true;
	st.ambientLabel = "Explore - Day";
	st.ambientTrack = "MMExploreDay01.xwm";
	st.ambientGain = 0.8f;

	// --- volume page: one slider, no list, no child window -----------------
	mock::g = mock::Rec{};
	cms::menupage::Build(api, cfg, mix, st, stats, "C:/tmp/volume.txt");

	CHECK(mock::g.depth == 0);
	CHECK(mock::g.beginChild == 0);   // the per-track list is gone
	CHECK(mock::g.endChild == 0);
	CHECK(mock::g.pushes == mock::g.pops);
	CHECK(mock::g.colors == mock::g.colorPops);
	CHECK(mock::g.sliders == 1);       // exactly one: the master volume
	CHECK(mock::g.checks == 1);        // enable music - there is no mute switch any more
	CHECK(mock::g.buttons == 2);       // reset to 100% + save now
	CHECK(mock::g.ids.empty());
	CHECK(mock::g.wraps == mock::g.wrapPops);  // every hint folded and closed

	// the single slider writes the master level
	mock::g = mock::Rec{};
	mock::g.fireId = "##master";
	mock::g.fireValue = 40.0f;
	cms::menupage::Build(api, cfg, mix, st, stats, "");
	CHECK(mix.Master().gain > 0.39f && mix.Master().gain < 0.41f);
	CHECK(mock::g.depth == 0);

	// 0 % is the only way to silence the plugin - and it is a level, not a
	// hidden flag, so the slider still shows it afterwards
	CHECK(mix.MasterEffective() > 0.39f && mix.MasterEffective() < 0.41f);
	CHECK(mock::g.checks == 1);

	// reset button puts it back to 100 %
	mock::g = mock::Rec{};
	mock::g.fireButton = cms::i18n::T(cms::i18n::S::BtnAllFull);
	cms::menupage::Build(api, cfg, mix, st, stats, "");
	CHECK(mix.Master().gain == 1.0f);
	CHECK(mix.MasterEffective() == 1.0f);

	// an empty library still draws (and says so) without a child window
	mock::g = mock::Rec{};
	{
		cms::menupage::LibraryStats none;
		cms::menupage::Build(api, cfg, mix, st, none, "");
	}
	CHECK(mock::g.beginChild == 0);
	CHECK(mock::g.depth == 0);
	CHECK(mock::g.sliders == 1);


	// --- settings page: every INI key is reachable, buttons only report ------
	mock::g = mock::Rec{};
	cms::menupage::SettingsActions act;
	cms::menupage::BuildSettings(api, cfg, act);
	CHECK(mock::g.depth == 0);
	CHECK(mock::g.pushes == mock::g.pops);
	CHECK(mock::g.colors == mock::g.colorPops);
	CHECK(mock::g.wraps == mock::g.wrapPops);
	// 11 float rows: 6 crossfade/hold + 3 switch-guard + 2 combat levels
	CHECK(mock::g.sliders == 11);
	CHECK(mock::g.intSliders == 2);   // repeat window / minimum pool size
	CHECK(mock::g.radios == 0);       // no radio group: vanilla is one switch, the language is a build
	// the library folder is a constant now (Data\Music), so the page has no
	// text field at all - it cannot be pointed somewhere else by accident
	CHECK(mock::g.inputs == 0);
	CHECK(mock::g.checks == 4);       // sheathe / dialogue duck / keep vanilla / autosave
	CHECK(mock::g.buttons == 3);      // save / reload / defaults
	CHECK(!act.save && !act.reload && !act.defaults);

	// "keep vanilla music" is the one vanilla switch and it is a plain checkbox,
	// so there is no third state that could silence the game's own music
	mock::g = mock::Rec{};
	mock::g.fireCheck = "##keepvanilla";
	CHECK(!cfg.keepVanilla);          // takeover is the default
	{
		cms::menupage::SettingsActions act2;
		cms::menupage::BuildSettings(api, cfg, act2);
	}
	CHECK(cfg.keepVanilla);
	mock::g.fireCheck = "##keepvanilla";
	{
		cms::menupage::SettingsActions act3;
		cms::menupage::BuildSettings(api, cfg, act3);
	}
	CHECK(!cfg.keepVanilla);

	// the autosave switch is still the one file option on the page
	mock::g = mock::Rec{};
	mock::g.fireCheck = "##autosave";
	CHECK(cfg.mixerAutosave);
	{
		cms::menupage::SettingsActions act5;
		cms::menupage::BuildSettings(api, cfg, act5);
	}
	CHECK(!cfg.mixerAutosave);

	// the fade that used to be called "silence fade" is now the hand-back fade,
	// and it is still a settable row (same widget id)
	mock::g = mock::Rec{};
	mock::g.fireId = "##fds";
	mock::g.fireValue = 1.5f;
	{
		cms::menupage::SettingsActions act4;
		cms::menupage::BuildSettings(api, cfg, act4);
	}
	CHECK(cfg.fadeHandback > 1.49f && cfg.fadeHandback < 1.51f);

	// the volume page says who owns the music right now: takeover vs fallback
	{
		cms::menupage::State fb;
		fb.vanillaTakenOver = false;
		mock::g = mock::Rec{};
		cms::menupage::Build(api, cfg, mix, fb, stats, "");
		CHECK(mock::g.texts > 0);
		CHECK(mock::g.wraps == mock::g.wrapPops);
	}
}

// --- the combat gate: the branch table the game code feeds -------------------
static void CombatGateTests()
{
	using cms::CombatGate;
	using cms::EngageReason;

	// 1. a swing nobody answers is not a fight (this was the false positive)
	{
		CombatGate gate;
		gate.OnPlayerHit(10.0);
		CHECK(!gate.Update(10.1, false).combat);
		CHECK(!gate.Update(12.0, false).combat);
		CHECK(!gate.Update(20.0, false).combat);  // offense window long gone
	}

	// 2. hit, enemy engages -> a duel, but only after kDuelConfirm of fighting
	{
		CombatGate gate;
		gate.OnPlayerHit(10.0);
		gate.OnEnemyEngage(11.5);
		CHECK(!gate.Update(11.5, true).combat);  // answered: pending, not a fight yet
		const auto v = gate.Update(13.6, true);
		CHECK(v.combat);
		CHECK(v.reason == EngageReason::Duel);
		CHECK(v.raised);
	}

	// 3. answered, but the AI flag never comes on (the swing was answered by
	//    nothing that really fights): the pending duel is cancelled, never
	//    confirmed - a fight nobody actually fights is not a combat event
	{
		CombatGate gate;
		gate.OnPlayerHit(10.0);
		gate.OnEnemyHit(10.8);
		CHECK(!gate.Update(10.8, false).combat);  // pending, grace clock starts
		CHECK(!gate.Update(11.3, false).combat);  // inside kDuelGrace
		CHECK(!gate.Update(11.8, false).combat);  // grace over: cancelled
		CHECK(!gate.Update(30.0, false).combat);
	}

	// 4. enemy engages out of nowhere -> ambush
	{
		CombatGate gate;
		gate.OnEnemyEngage(5.0);
		const auto v = gate.Update(5.0, true);
		CHECK(v.combat);
		CHECK(v.reason == EngageReason::Ambush);
	}

	// 5. a stale offense does not turn a later ambush into a duel
	{
		CombatGate gate;
		gate.OnPlayerHit(1.0);
		gate.OnEnemyHit(9.0);
		const auto v = gate.Update(9.0, true);
		CHECK(v.combat);
		CHECK(v.reason == EngageReason::Ambush);
	}

	// 6. hold: survives the AI flag flapping, then releases
	{
		CombatGate gate;
		gate.OnEnemyEngage(0.0);
		CHECK(gate.Update(0.0, true).combat);
		CHECK(gate.Update(2.0, false).combat);   // inside kHold
		CHECK(!gate.Update(4.0, false).combat);  // released
	}

	// 7. the AI flag alone keeps a confirmed fight alive indefinitely
	{
		CombatGate gate;
		gate.OnEnemyEngage(0.0);
		CHECK(gate.Update(0.0, true).combat);
		CHECK(gate.Update(600.0, true).combat);
		CHECK(gate.Update(600.0, true).reason == EngageReason::Ambush);
	}

	// 8. a hostile that stays in combat without any event -> sustained fallback
	{
		CombatGate gate;
		gate.OnHostileInCombat(0.0, true);
		CHECK(!gate.Update(1.0, false).combat);
		gate.OnHostileInCombat(2.6, true);
		const auto v = gate.Update(2.6, false);
		CHECK(v.combat);
		CHECK(v.reason == EngageReason::Sustained);
		gate.OnHostileInCombat(3.0, false);
		CHECK(!gate.Update(7.0, false).combat);
	}

	// 9. "raised" is reported exactly once per fight
	{
		CombatGate gate;
		gate.OnEnemyEngage(0.0);
		CHECK(gate.Update(0.0, true).raised);
		CHECK(!gate.Update(0.05, true).raised);
	}

	// 10. a duel stays a duel for the whole fight, even if a second enemy joins
	{
		CombatGate gate;
		gate.OnPlayerHit(0.0);
		gate.OnEnemyEngage(1.0);
		CHECK(!gate.Update(1.0, true).combat);          // pending...
		CHECK(gate.Update(3.1, true).combat);           // ...confirmed
		gate.OnEnemyEngage(30.0);                       // a second enemy joins
		CHECK(gate.Update(30.0, true).reason == EngageReason::Duel);
	}

	// 11. assassination: one sneak hit, the target answers and dies right away -
	//     the pending duel is cancelled by the AI flag dropping, never confirmed
	{
		CombatGate gate;
		gate.OnPlayerHit(10.0);
		gate.OnEnemyEngage(10.5);
		CHECK(!gate.Update(10.5, true).combat);   // answered, pending
		CHECK(!gate.Update(11.0, false).combat);  // target dead: flag off, grace starts
		CHECK(!gate.Update(11.5, false).combat);  // still inside the grace
		CHECK(!gate.Update(12.0, false).combat);  // grace over: cancelled for good
		CHECK(!gate.Update(60.0, false).combat);
	}

	// 12. sheathing during a pending duel cancels it: putting the weapon away is
	//     the player saying "this is not a fight"
	{
		CombatGate gate;
		gate.OnPlayerHit(10.0);
		gate.OnEnemyEngage(10.5);
		CHECK(!gate.Update(10.6, true, false).combat);  // sheathed while pending
		CHECK(!gate.Update(11.6, true, false).combat);  // past the grace: cancelled
		// and a later enemy attack still counts as an ambush on its own
		gate.OnEnemyEngage(19.5);
		const auto v = gate.Update(20.0, true, true);
		CHECK(v.combat);
		CHECK(v.reason == EngageReason::Ambush);
	}

	// 13. the AI flag lagging the answer event by a tick must not kill a real duel
	{
		CombatGate gate;
		gate.OnPlayerHit(10.0);
		gate.OnEnemyEngage(10.5);
		CHECK(!gate.Update(10.6, false).combat);  // flag late: grace clock, not cancel
		CHECK(!gate.Update(11.4, true).combat);   // flag on again, 0.9 s since the answer
		const auto v = gate.Update(12.6, true);   // kDuelConfirm reached
		CHECK(v.combat);
		CHECK(v.reason == EngageReason::Duel);
	}
}

// The IMR handshake is the only place two DLLs agree on anything, so the reader
// side is pinned down here: a stale or half-written block must never leave the
// game silent, and a live one must always be honoured.
static void ImrLinkTests()
{
	CHECK(sizeof(ImrLink) == 64);
	CHECK(kImrMagic == 0x48524D49u);

	auto fresh = [](std::uint64_t a_now) {
		ImrLink l;
		l.magic = kImrMagic;
		l.version = kImrVersion;
		l.state = 1;
		l.pid = 4242;
		l.heartbeatMs = a_now;
		return l;
	};

	// 1. an all-zero block (nobody ever wrote it) is not a link
	{
		const ImrLink zero{};
		CHECK(!ImrLinkActive(zero, 1000));
	}
	// 2. a live block is honoured
	{
		CHECK(ImrLinkActive(fresh(1000), 1000));
		CHECK(ImrLinkActive(fresh(1000), 2500));   // 1.5 s old, still fresh
	}
	// 3. state 0 means IMR released on purpose
	{
		auto l = fresh(1000);
		l.state = 0;
		CHECK(!ImrLinkActive(l, 1000));
	}
	// 4. an unknown state is not trusted
	{
		auto l = fresh(1000);
		l.state = 7;
		CHECK(!ImrLinkActive(l, 1000));
	}
	// 5. the heartbeat expires: a crashed IMR must hand the context back
	{
		CHECK(!ImrLinkActive(fresh(1000), 1000 + kImrHeartbeatMaxMs + 1));
		CHECK(ImrLinkActive(fresh(1000), 1000 + kImrHeartbeatMaxMs));
	}
	// 6. a heartbeat stamped after "now" (previous boot, tick counter restarted)
	//    is stale, not trusted forever
	{
		CHECK(!ImrLinkActive(fresh(5000), 1000));
	}
	// 7. magic / version mismatch: written by something else, or by a future
	//    IMR that this build cannot understand
	{
		auto l = fresh(1000);
		l.magic = 0xDEADBEEF;
		CHECK(!ImrLinkActive(l, 1000));
		l = fresh(1000);
		l.version = 0;
		CHECK(!ImrLinkActive(l, 1000));
		l.version = kImrVersion + 1;
		CHECK(!ImrLinkActive(l, 1000));
	}
	// 8. an unset heartbeat (0) cannot be "fresh since boot"
	{
		auto l = fresh(1000);
		l.heartbeatMs = 0;
		CHECK(!ImrLinkActive(l, 1000));
	}
}

static int ScanReport(const char* a_root)
{
	RuleTable rules;
	rules.Build();
	Library lib;
	rules.Bind(lib);
	lib.root = a_root;
	lib.Scan(true);

	std::size_t total = 0, withTracks = 0;
	for (const auto& f : lib.folders) {
		total += f.tracks.size();
		if (!f.tracks.empty()) ++withTracks;
		std::printf("%-38s limit=%-3d tracks=%-3zu %s%s\n", f.name.c_str(), f.limit, f.tracks.size(),
			f.tracks.empty() ? "" : f.tracks.front().path.c_str(), f.mixedFormat ? "  [MIXED]" : "");
	}
	std::printf("\ntotal: %zu folders with tracks / %zu, %zu tracks, %.1f MB\n", withTracks, lib.folders.size(), total,
		static_cast<double>(lib.TotalBytes()) / (1024.0 * 1024.0));
	return total > 0 ? 0 : 1;
}

int main(int argc, char** argv)
{
	setvbuf(stdout, nullptr, _IONBF, 0);  // survive a crash: no buffered output

	if (argc >= 3 && std::string(argv[1]) == "--scan") return ScanReport(argv[2]);

	// --i18n <Data dir> [language]: proves the loose translation file is found,
	// decoded and matched to the right keys before shipping it.
	if (argc >= 3 && std::string(argv[1]) == "--i18n") {
		const std::string lang = argc >= 4 ? argv[3] : "chinese";
		std::printf("build language=%s (chinese=%d)  file=%s\n", cms::i18n::Lang(),
			cms::i18n::kChineseBuild ? 1 : 0, "(none - compiled in)");
		for (int i = 0; i < static_cast<int>(cms::i18n::S::Count); ++i) {
			std::printf("  %2d  %s\n", i, cms::i18n::T(static_cast<cms::i18n::S>(i)));
		}
		return 0;
	}

	RuleTable rules;
	rules.Build();
	CHECK(rules.rules.size() == 69);

	Library lib;
	rules.Bind(lib);
	CHECK(lib.folders.size() == 69);

	// give every folder a couple of tracks
	for (auto& f : lib.folders) AddTracks(lib, f.name.c_str(), 2);

	Resolution r;

	// --- outdoors, no biome keyword: degrade to the hour rule ---------------
	Resolve(rules, lib, Exterior | HourDay, Layer::Ambient, r);
	CHECK(r.anchor >= 0);
	CHECK(rules.rules[static_cast<std::size_t>(r.anchor)].folder == "Explore - Day");
	CHECK(r.label.find("Explore - Day") != std::string::npos);
	CHECK(r.label.find("Explore - All Areas") != std::string::npos);
	CHECK(FolderCount(r) == 3);  // day + all areas + all hours

	// night flips the anchor, pool hash must change
	Resolution rNight;
	Resolve(rules, lib, Exterior | HourNight, Layer::Ambient, rNight);
	CHECK(rules.rules[static_cast<std::size_t>(rNight.anchor)].folder == "Explore - Night");
	CHECK(rNight.hash != r.hash);

	// --- town: no exploration leakage --------------------------------------
	Resolve(rules, lib, Exterior | HourDay | Town | TownDay | CityWhiterun, Layer::Ambient, r);
	CHECK(rules.rules[static_cast<std::size_t>(r.anchor)].folder == "Town - Whiterun");
	CHECK(!HasFolder(r, lib, "Explore - Day"));
	CHECK(!HasFolder(r, lib, "Explore - All Areas"));

	Resolution rTownGeneric;
	Resolve(rules, lib, Exterior | HourDay | Town | TownDay, Layer::Ambient, rTownGeneric);
	CHECK(rules.rules[static_cast<std::size_t>(rTownGeneric.anchor)].folder == "Town - Day");
	CHECK(!HasFolder(rTownGeneric, lib, "Explore - All Hours"));

	// town at night
	Resolution rTownNight;
	Resolve(rules, lib, Exterior | HourNight | Town | TownNight, Layer::Ambient, rTownNight);
	CHECK(rules.rules[static_cast<std::size_t>(rTownNight.anchor)].folder == "Town - Night");

	// --- dungeon subtypes + Dungeon - All additive --------------------------
	Resolve(rules, lib, Interior | Cave | Dungeon, Layer::Ambient, r);
	CHECK(rules.rules[static_cast<std::size_t>(r.anchor)].folder == "Dungeon - Cave");
	CHECK(HasFolder(r, lib, "Dungeon - All"));
	CHECK(FolderCount(r) == 2);

	// plain dungeon
	Resolution rDungeon;
	Resolve(rules, lib, Interior | Dungeon, Layer::Ambient, rDungeon);
	CHECK(rules.rules[static_cast<std::size_t>(rDungeon.anchor)].folder == "Dungeon");
	CHECK(HasFolder(rDungeon, lib, "Dungeon - All"));

	// unknown interior -> silence
	Resolution rNone;
	Resolve(rules, lib, Interior, Layer::Ambient, rNone);
	CHECK(rNone.empty());

	// --- combat layer ------------------------------------------------------
	Resolve(rules, lib, InCombat, Layer::Combat, r);
	CHECK(rules.rules[static_cast<std::size_t>(r.anchor)].folder == "Combat");
	CHECK(HasFolder(r, lib, "Combat - All"));

	Resolution rDragon;
	Resolve(rules, lib, InCombat | EnemyDragon | BossGeneric, Layer::Combat, rDragon);
	CHECK(rules.rules[static_cast<std::size_t>(rDragon.anchor)].folder == "Combat - Dragon");

	// ambient resolution must ignore combat-only bits
	Resolution rAmb;
	Resolve(rules, lib, Exterior | HourDay | InCombat | EnemyDragon, Layer::Ambient, rAmb);
	CHECK(rules.rules[static_cast<std::size_t>(rAmb.anchor)].folder == "Explore - Day");

	// --- ride: replaces only when the folder has tracks --------------------
	Resolve(rules, lib, Exterior | HourDay | Riding, Layer::Ambient, r);
	CHECK(rules.rules[static_cast<std::size_t>(r.anchor)].folder == "Ride - General");
	CHECK(FolderCount(r) == 1);

	AddTracks(lib, "Ride - General", 0);
	Resolution rRideEmpty;
	Resolve(rules, lib, Exterior | HourDay | Riding, Layer::Ambient, rRideEmpty);
	CHECK(FolderCount(rRideEmpty) > 1);
	CHECK(HasFolder(rRideEmpty, lib, "Explore - Day"));

	// --- player homes ------------------------------------------------------
	Resolve(rules, lib, Interior | HomeBreezehome | HomeAny, Layer::Ambient, r);
	CHECK(rules.rules[static_cast<std::size_t>(r.anchor)].folder == "Player Homes/Breezehome");
	CHECK(HasFolder(r, lib, "Player Homes/Player Homes - All"));

	// --- solstheim ---------------------------------------------------------
	Resolution rDb;
	Resolve(rules, lib, Exterior | HourDay | DBArea, Layer::Ambient, rDb);
	CHECK(rules.rules[static_cast<std::size_t>(rDb.anchor)].folder == "DB Explore - General");

	VolumeTests();
	I18nTests();
	SwitchGuardTests();
	SheatheTests();
	FleeTests();
	MenuPageTests();
	CombatGateTests();
	ImrLinkTests();

	std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
	return g_fail == 0 ? 0 : 1;
}
