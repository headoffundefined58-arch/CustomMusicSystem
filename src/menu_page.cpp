#include "menu_page.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

#include "core.h"
#include "i18n.h"
#include "volume.h"

namespace cms::menupage {

namespace {

// Two columns and nothing else.  The framework's window is ~930 px wide with a
// large font, so 300 px of label + 220 px of control + a checkbox still leaves
// room for the right edge; anything wider is where the old page started to
// clip.
constexpr float kLabelW = 320.0f;
constexpr float kSliderW = 240.0f;

constexpr Vec4 kPlayingColor{ 1.00f, 0.82f, 0.32f, 1.00f };
constexpr Vec4 kDimColor{ 0.62f, 0.62f, 0.62f, 1.00f };

// ImGuiCol_Text is 0 in every ImGui release, so this survives a framework bump.
constexpr int kColText = 0;

void Text(const Api& a_api, const char* a_str)
{
	if (a_api.text) a_api.text("%s", a_str);
	else if (a_api.textRaw) a_api.textRaw(a_str, nullptr);
}

// A long sentence must fold, not run under the window border.  PushTextWrapPos
// with 0 means "wrap at the window edge", which is exactly what a hint needs;
// without the export the text still draws (just un-wrapped), so a framework
// that dropped the symbol only loses the folding.
void Wrapped(const Api& a_api, const char* a_str)
{
	if (a_api.pushTextWrapPos) a_api.pushTextWrapPos(0.0f);
	Text(a_api, a_str);
	if (a_api.popTextWrapPos) a_api.popTextWrapPos();
}

void Wrapped(const Api& a_api, const std::string& a_str) { Wrapped(a_api, a_str.c_str()); }

void DimWrapped(const Api& a_api, const char* a_str)
{
	if (a_api.pushStyleColor) a_api.pushStyleColor(kColText, kDimColor);
	Wrapped(a_api, a_str);
	if (a_api.popStyleColor) a_api.popStyleColor(1);
}

void Section(const Api& a_api, const char* a_title)
{
	if (a_api.spacing) a_api.spacing();
	if (a_api.separatorText) a_api.separatorText(a_title);
}

// Label in the left column, control on the right.
void RowLabel(const Api& a_api, const char* a_label)
{
	Text(a_api, a_label);
	a_api.sameLine(kLabelW, -1.0f);
}

// --------------------------------------------------------------- wheel ------
// Hovering a slider and rolling the wheel is the gesture the mod is expected to
// support, and not every ImGui build moves a slider with the wheel.  One notch =
// one step, three notches at once = three steps (a flick does not jump to 0 or
// 150 %).
bool WheelStep(const Api& a_api, float* a_value, float a_step, float a_min, float a_max)
{
	if (!a_api.wheel || !a_api.itemHovered) return false;
	if (a_api.itemActive && a_api.itemActive()) return false;  // dragging: leave it alone
	if (!a_api.itemHovered()) return false;

	const float w = a_api.wheel();
	if (w == 0.0f) return false;

	const float notches = std::clamp(std::fabs(w), 1.0f, 3.0f);
	const float next = std::clamp(*a_value + (w > 0.0f ? a_step : -a_step) * notches, a_min, a_max);
	if (next == *a_value) return false;
	*a_value = next;
	return true;
}

// ---------------------------------------------------------- settings rows --
bool FloatRow(const Api& a_api, const std::string& a_label, const char* a_id, float* a_value, float a_min,
	float a_max, const char* a_fmt, float a_step)
{
	RowLabel(a_api, a_label.c_str());
	a_api.setItemWidth(kSliderW);

	bool changed = a_api.sliderFloat(a_id, a_value, a_min, a_max, a_fmt, 0);
	if (WheelStep(a_api, a_value, a_step, a_min, a_max)) changed = true;
	return changed;
}

bool IntRow(const Api& a_api, const std::string& a_label, const char* a_id, int* a_value, int a_min, int a_max)
{
	if (!a_api.sliderInt) return false;

	RowLabel(a_api, a_label.c_str());
	a_api.setItemWidth(kSliderW);

	float f = static_cast<float>(*a_value);
	bool  changed = a_api.sliderInt(a_id, a_value, a_min, a_max, "%d", 0);
	if (WheelStep(a_api, &f, 1.0f, static_cast<float>(a_min), static_cast<float>(a_max))) {
		*a_value = static_cast<int>(std::lround(f));
		changed = true;
	}
	return changed;
}

bool BoolRow(const Api& a_api, const std::string& a_label, const char* a_id, bool* a_value)
{
	RowLabel(a_api, a_label.c_str());
	// the visible label is drawn by this row; "##" hides the duplicate text
	char buf[160];
	std::snprintf(buf, sizeof(buf), "##%s", a_id);
	return a_api.checkbox(buf, a_value);
}

const char* ReasonText(int a_reason)
{
	switch (static_cast<EngageReason>(a_reason)) {
	case EngageReason::Duel: return i18n::T(i18n::S::ReasonDuel);
	case EngageReason::Ambush: return i18n::T(i18n::S::ReasonAmbush);
	case EngageReason::Sustained: return i18n::T(i18n::S::ReasonSustained);
	default: return i18n::T(i18n::S::ReasonNone);
	}
}

}  // namespace

// ============================================================== volume =====
// One page, four blocks: switch, volume, what is playing, and how big the
// library is.  No list, no child window, no per-row slider, so nothing can be
// squeezed or mis-clicked.
void Build(const Api& a_api, Config& a_cfg, VolumeMix& a_mix, const State& a_state, const LibraryStats& a_stats,
	const std::string& a_mixerPath)
{
	const Api& api = a_api;

	// ---- master switch ---------------------------------------------------
	bool enabled = a_cfg.enabled;
	if (BoolRow(api, i18n::T(i18n::S::EnableMusic), "enabled", &enabled)) a_cfg.enabled = enabled;

	if (!a_cfg.enabled) {
		DimWrapped(api, i18n::T(i18n::S::Stopped));
	} else {
		if (api.pushStyleColor) api.pushStyleColor(kColText, kPlayingColor);
		Wrapped(api, a_state.gateOpen ? i18n::T(i18n::S::StatusOk) : i18n::T(i18n::S::StatusGateClosed));
		if (api.popStyleColor) api.popStyleColor(1);
	}

	// ---- the one and only volume -----------------------------------------
	Section(api, i18n::T(i18n::S::MixerTitle));

	const float master = a_mix.Master().gain;
	float       pct = master * 100.0f;

	RowLabel(api, i18n::T(i18n::S::MasterTitle));
	api.setItemWidth(kSliderW);

	bool changed = false;
	if (api.sliderFloat("##master", &pct, 0.0f, VolumeMix::kMaxGain * 100.0f, "%.0f%%", 0)) changed = true;
	if (WheelStep(api, &pct, 1.0f, 0.0f, VolumeMix::kMaxGain * 100.0f)) changed = true;
	if (changed) a_mix.SetMaster(pct / 100.0f);

	// level meter: always mirrors the master volume the player set, never the
	// transient ambient-layer gain.  The old meter read ambientGain, which is 0
	// whenever this plugin has handed the context back to vanilla (our own layer
	// is silent then) - so it drew an empty bar that looked like a dead element.
	if (api.progressBar) {
		char overlay[48];
		if (!a_cfg.enabled) {
			std::snprintf(overlay, sizeof(overlay), "%s", i18n::T(i18n::S::Stopped));
		} else {
			std::snprintf(overlay, sizeof(overlay), "%.0f%%", pct);
		}
		const float frac = std::clamp(pct / (VolumeMix::kMaxGain * 100.0f), 0.0f, 1.0f);
		api.progressBar(frac, Vec2{ 0.0f, 10.0f }, overlay);
	}

	Wrapped(api, i18n::T(i18n::S::MixerHint));
	if (a_mix.LegacyRows() > 0) DimWrapped(api, i18n::T(i18n::S::HintLegacyMixer));

	// ---- now playing -----------------------------------------------------
	Section(api, i18n::T(i18n::S::NowPlayingTitle));

	if (!a_state.ambientTrack.empty()) {
		char line[512];
		std::snprintf(line, sizeof(line), "%s: %s - %s", i18n::T(i18n::S::LayerAmbient), a_state.ambientLabel.c_str(),
			a_state.ambientTrack.c_str());
		Wrapped(api, line);
	}
	if (!a_state.combatTrack.empty()) {
		char line[512];
		std::snprintf(line, sizeof(line), "%s: %s - %s", i18n::T(i18n::S::LayerCombat), a_state.combatLabel.c_str(),
			a_state.combatTrack.c_str());
		Wrapped(api, line);
	}
	if (a_state.ambientTrack.empty() && a_state.combatTrack.empty()) {
		DimWrapped(api, i18n::T(i18n::S::NothingPlaying));
	}
	if (a_state.combat) {
		char line[320];
		std::snprintf(line, sizeof(line), "%s: %s", i18n::T(i18n::S::LayerCombat), ReasonText(a_state.combatReason));
		Wrapped(api, line);
	}

	// Who owns the music right now.  The vanilla layer is only held back where
	// this plugin has something; everywhere else the vanilla music plays, and the
	// player can see that instead of wondering why a place is silent.
	if (a_cfg.keepVanilla) {
		DimWrapped(api, i18n::T(i18n::S::VanillaKept));
	} else if (a_state.vanillaTakenOver) {
		DimWrapped(api, i18n::T(i18n::S::VanillaTakenOver));
	} else {
		DimWrapped(api, i18n::T(i18n::S::VanillaFallback));
	}

	// ---- library size ----------------------------------------------------
	Section(api, i18n::T(i18n::S::LibTitle));

	char stats[160];
	std::snprintf(stats, sizeof(stats), "%zu %s / %zu %s", a_stats.folders, i18n::T(i18n::S::GroupCount),
		a_stats.tracks, i18n::T(i18n::S::TrackCount));
	Wrapped(api, stats);

	if (a_stats.tracks == 0) DimWrapped(api, i18n::T(i18n::S::NoTracks));

	// ---- actions ---------------------------------------------------------
	if (api.spacing) api.spacing();

	if (api.button(i18n::T(i18n::S::BtnAllFull), Vec2{ 0.0f, 0.0f })) {
		a_mix.SetMaster(1.0f);
	}
	api.sameLine(0.0f, 8.0f);
	if (api.button(i18n::T(i18n::S::BtnSave), Vec2{ 0.0f, 0.0f }) && !a_mixerPath.empty()) {
		a_mix.Save(a_mixerPath);
		LOG_D("volume: saved %s", a_mixerPath.c_str());
	}

	DimWrapped(api, i18n::T(i18n::S::EditHint));
}

// ============================================================ settings =====
// A player page, not a diagnostics page.  Everything that is an engine detail -
// scheduler period, library path, encoding warnings - lives in the INI only;
// what is left here is the set of decisions a player can actually hear.
void BuildSettings(const Api& a_api, Config& a_cfg, SettingsActions& a_actions)
{
	const Api& api = a_api;

	Wrapped(api, i18n::T(i18n::S::HintLive));

	// ---- crossfades ------------------------------------------------------
	Section(api, i18n::T(i18n::S::SecTransition));

	FloatRow(api, i18n::T(i18n::S::LblFadeLocation), "##fdl", &a_cfg.fadeLocation, 0.0f, 12.0f, "%.2f s", 0.1f);
	FloatRow(api, i18n::T(i18n::S::LblFadeCombatEnter), "##fdce", &a_cfg.fadeCombatEnter, 0.0f, 12.0f, "%.2f s", 0.1f);
	FloatRow(api, i18n::T(i18n::S::LblFadeCombatExit), "##fdcx", &a_cfg.fadeCombatExit, 0.0f, 12.0f, "%.2f s", 0.1f);
	FloatRow(api, i18n::T(i18n::S::LblHandbackFade), "##fds", &a_cfg.fadeHandback, 0.0f, 12.0f, "%.2f s", 0.1f);
	FloatRow(api, i18n::T(i18n::S::LblCombatHold), "##hold", &a_cfg.combatHold, 0.0f, 30.0f, "%.1f s", 0.5f);
	FloatRow(api, i18n::T(i18n::S::LblWorldSettle), "##settle", &a_cfg.worldSettle, 0.0f, 10.0f, "%.2f s", 0.1f);

	// ---- track switching -------------------------------------------------
	// The three values that stop a doorway or a cell border from retriggering a
	// layer on top of a running crossfade.
	Section(api, i18n::T(i18n::S::SecSwitch));

	FloatRow(api, i18n::T(i18n::S::LblMinPlay), "##minplay", &a_cfg.minPlay, 0.0f, 120.0f, "%.0f s", 1.0f);
	FloatRow(api, i18n::T(i18n::S::LblSwitchSettle), "##settle2", &a_cfg.switchSettle, 0.0f, 10.0f, "%.2f s", 0.1f);
	FloatRow(api, i18n::T(i18n::S::LblSwitchCooldown), "##cooldown", &a_cfg.switchCooldown, 0.0f, 30.0f, "%.2f s", 0.5f);
	DimWrapped(api, i18n::T(i18n::S::HintSwitchGuard));

	// ---- combat ----------------------------------------------------------
	Section(api, i18n::T(i18n::S::SecCombat));

	BoolRow(api, i18n::T(i18n::S::LblSheatheEndsCombat), "sheathe", &a_cfg.sheatheEndsCombat);
	FloatRow(api, i18n::T(i18n::S::LblFadeSwap), "##swap", &a_cfg.fadeSwap, 0.05f, 3.0f, "%.2f", 0.05f);
	DimWrapped(api, i18n::T(i18n::S::HintFadeSwap));
	BoolRow(api, i18n::T(i18n::S::LblDuckInDialogue), "dlgduck", &a_cfg.duckInDialogue);
	FloatRow(api, i18n::T(i18n::S::LblDialogueDuck), "##dlgv", &a_cfg.dialogueDuck, 0.0f, 1.0f, "%.2f", 0.01f);

	// ---- library ---------------------------------------------------------
	Section(api, i18n::T(i18n::S::SecLibrary));

	IntRow(api, i18n::T(i18n::S::LblRecentWindow), "##recent", &a_cfg.recentWindow, 0, 20);
	IntRow(api, i18n::T(i18n::S::LblMinTracks), "##mintrk", &a_cfg.minTracks, 0, 10);

	// ---- vanilla ---------------------------------------------------------
	// One switch, two behaviours - and no third "mute" state, because nothing
	// here is allowed to leave the game without music.
	Section(api, i18n::T(i18n::S::SecVanilla));

	BoolRow(api, i18n::T(i18n::S::LblKeepVanilla), "keepvanilla", &a_cfg.keepVanilla);
	DimWrapped(api, i18n::T(i18n::S::HintVanillaHandover));

	// ---- interface -------------------------------------------------------
	Section(api, i18n::T(i18n::S::SecInterface));

	{
		// No radio buttons: the language is baked into this DLL.  Showing it as
		// read-only text is the honest thing to do - a switch here would have to
		// rebuild the framework's section tree, and that is exactly what kept
		// failing.  Two builds, one language each, zero moving parts.
		char line[192];
		std::snprintf(line, sizeof(line), "%s: %s", i18n::T(i18n::S::LblLanguage),
			i18n::kChineseBuild ? i18n::T(i18n::S::LangChinese) : i18n::T(i18n::S::LangEnglish));
		Wrapped(api, line);
		DimWrapped(api, i18n::T(i18n::S::HintLangBuild));
	}

	// ---- file actions ----------------------------------------------------
	if (api.spacing) api.spacing();
	if (api.separator) api.separator();

	BoolRow(api, i18n::T(i18n::S::LblMixerAutosave), "autosave", &a_cfg.mixerAutosave);

	if (api.button(i18n::T(i18n::S::BtnSaveConfig), Vec2{ 0.0f, 0.0f })) a_actions.save = true;
	api.sameLine(0.0f, 8.0f);
	if (api.button(i18n::T(i18n::S::BtnReloadConfig), Vec2{ 0.0f, 0.0f })) a_actions.reload = true;
	api.sameLine(0.0f, 8.0f);
	if (api.button(i18n::T(i18n::S::BtnDefaults), Vec2{ 0.0f, 0.0f })) a_actions.defaults = true;

	DimWrapped(api, i18n::T(i18n::S::EditHint));
}

}  // namespace cms::menupage
