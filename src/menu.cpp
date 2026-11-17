#include "menu.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <atomic>
#include <chrono>
#include <string>

#include "core.h"
#include "director.h"
#include "fwio.h"
#include "i18n.h"
#include "menu_page.h"
#include "volume.h"

namespace cms::menu {

namespace {

using RenderFn = void(__stdcall*)();

menupage::Api g_api{};
bool          g_registered = false;

Config*     g_cfg = nullptr;
VolumeMix*  g_mix = nullptr;
Director*   g_dir = nullptr;
std::string g_mixerPath;
std::string g_iniPath;

// ------------------------------------------------------- published library --
// The page no longer lists tracks, it only needs the size, so two atomics are
// enough: the engine thread rescans the library while the game thread draws.
std::atomic<std::size_t> g_folderCount{ 0 };
std::atomic<std::size_t> g_trackCount{ 0 };

double NowSec()
{
	using clock = std::chrono::steady_clock;
	static const auto t0 = clock::now();
	return std::chrono::duration<double>(clock::now() - t0).count();
}

double g_savedAt = -1.0;
double g_reloadedAt = -1.0;

// --------------------------------------------------------------- adapters --
float Wheel() { return fwio::Wheel(); }

void __stdcall RenderVolume()
{
	try {
		// After Unregister (game shutdown) the framework may still call this
		// pointer for a frame; an empty api table would be a null call.
		if (!g_mix || !g_dir || !g_cfg || !g_api.Ready()) return;

		const auto st = g_dir->GetStatus();

		menupage::State state;
		state.gateOpen = st.gateOpen;
		state.combat = st.combat;
		state.combatReason = st.combatReason;
		state.musicEnabled = g_cfg->enabled;
		state.ambientLabel = st.ambientLabel;
		state.ambientTrack = st.ambientTrack;
		state.combatLabel = st.combatLabel;
		state.combatTrack = st.combatTrack;
		state.ambientGain = st.ambientGain;
		state.vanillaTakenOver = st.vanillaTakenOver;

		menupage::LibraryStats stats;
		stats.folders = g_folderCount.load(std::memory_order_relaxed);
		stats.tracks = g_trackCount.load(std::memory_order_relaxed);

		menupage::Build(g_api, *g_cfg, *g_mix, state, stats, g_mixerPath);
	} catch (...) {
		// An exception escaping into the framework's frame would take the menu
		// (and possibly the game) down; the page just draws nothing this frame.
	}
}

void __stdcall RenderSettings()
{
	try {
		if (!g_cfg || !g_api.Ready()) return;

		menupage::SettingsActions act;
		menupage::BuildSettings(g_api, *g_cfg, act);

		if (act.save) {
			const std::string path = g_cfg->iniPath.empty() ? g_iniPath : g_cfg->iniPath;
			if (!path.empty() && g_cfg->SaveToFile(path)) {
				g_savedAt = NowSec();
				LOG_I("%s %s", i18n::T(i18n::S::LogConfigSaved), path.c_str());
			} else {
				LOG_W("%s %s", i18n::T(i18n::S::LogConfigSaveFailed), path.c_str());
			}
		}

		if (act.reload) {
			const std::string path = g_cfg->iniPath.empty() ? g_iniPath : g_cfg->iniPath;
			if (!path.empty() && g_cfg->LoadFromFile(path)) {
				g_reloadedAt = NowSec();
				LOG_D("config reloaded from %s", path.c_str());
			}
		}

		if (act.defaults) {
			// every key back to its built-in default; the paths (and therefore
			// the ini we save to) stay
			Config fresh;
			fresh.dataDir = g_cfg->dataDir;
			fresh.pluginDir = g_cfg->pluginDir;
			fresh.iniPath = g_cfg->iniPath;
			*g_cfg = fresh;
			LOG_D("config reset to defaults (not written yet)");
		}

		const double now = NowSec();
		if (g_savedAt > 0.0 && now - g_savedAt < 3.0) {
			if (g_api.text) g_api.text("%s", i18n::T(i18n::S::HintSaved));
		} else if (g_reloadedAt > 0.0 && now - g_reloadedAt < 3.0) {
			if (g_api.text) g_api.text("%s", g_iniPath.c_str());
		}
	} catch (...) {
	}
}

template <class T>
T Resolve(const char* a_name)
{
	auto* module = ::GetModuleHandleW(L"SKSEMenuFramework");
	if (!module) return nullptr;
	return reinterpret_cast<T>(::GetProcAddress(module, a_name));
}

// Bow the last submitted item is hovered / active - both are plain queries with
// a fixed no-arg shape, so the page can stay ImGui-agnostic.
bool ItemHovered()
{
	using Fn = bool (*)(int);
	static Fn fn = nullptr;
	static bool tried = false;
	if (!tried) {
		tried = true;
		fn = Resolve<Fn>("igIsItemHovered");
	}
	return fn && fn(0);
}

bool ItemActive()
{
	using Fn = bool (*)();
	static Fn fn = nullptr;
	static bool tried = false;
	if (!tried) {
		tried = true;
		fn = Resolve<Fn>("igIsItemActive");
	}
	return fn && fn();
}

}  // namespace

void PublishLibrary(const Library& a_lib)
{
	std::size_t folders = 0;
	std::size_t tracks = 0;
	for (const auto& f : a_lib.folders) {
		if (f.tracks.empty()) continue;
		++folders;
		tracks += f.tracks.size();
	}
	g_folderCount.store(folders, std::memory_order_relaxed);
	g_trackCount.store(tracks, std::memory_order_relaxed);
}

bool Available() { return g_registered; }

bool Register(Config& a_cfg, const Library& a_lib, VolumeMix& a_mix, Director& a_dir, std::string a_mixerPath,
	std::string a_iniPath)
{
	g_cfg = &a_cfg;
	g_mix = &a_mix;
	g_dir = &a_dir;
	g_mixerPath = std::move(a_mixerPath);
	g_iniPath = std::move(a_iniPath);

	PublishLibrary(a_lib);

	if (!::GetModuleHandleW(L"SKSEMenuFramework")) {
		LOG_W("%s", i18n::T(i18n::S::LogNoFramework));
		return false;
	}

	// Everything is resolved by name: the framework exports its whole cimgui
	// surface, and a missing export only costs that one widget.
	g_api.text = Resolve<decltype(menupage::Api::text)>("igText");
	g_api.textRaw = Resolve<decltype(menupage::Api::textRaw)>("igTextUnformatted");
	g_api.sliderFloat = Resolve<decltype(menupage::Api::sliderFloat)>("igSliderFloat");
	g_api.checkbox = Resolve<decltype(menupage::Api::checkbox)>("igCheckbox");
	g_api.button = Resolve<decltype(menupage::Api::button)>("igButton");
	g_api.sameLine = Resolve<decltype(menupage::Api::sameLine)>("igSameLine");
	g_api.separatorText = Resolve<decltype(menupage::Api::separatorText)>("igSeparatorText");
	g_api.separator = Resolve<decltype(menupage::Api::separator)>("igSeparator");
	g_api.spacing = Resolve<decltype(menupage::Api::spacing)>("igSpacing");
	g_api.beginChild = Resolve<decltype(menupage::Api::beginChild)>("igBeginChild_Str");
	g_api.endChild = Resolve<decltype(menupage::Api::endChild)>("igEndChild");
	g_api.pushID = Resolve<decltype(menupage::Api::pushID)>("igPushID_Str");
	g_api.popID = Resolve<decltype(menupage::Api::popID)>("igPopID");
	g_api.setItemWidth = Resolve<decltype(menupage::Api::setItemWidth)>("igSetNextItemWidth");
	g_api.progressBar = Resolve<decltype(menupage::Api::progressBar)>("igProgressBar");
	g_api.pushStyleColor = Resolve<decltype(menupage::Api::pushStyleColor)>("igPushStyleColor_Vec4");
	g_api.popStyleColor = Resolve<decltype(menupage::Api::popStyleColor)>("igPopStyleColor");

	// settings page + wheel
	g_api.sliderInt = Resolve<decltype(menupage::Api::sliderInt)>("igSliderInt");
	g_api.radioButton = Resolve<decltype(menupage::Api::radioButton)>("igRadioButton_IntPtr");
	g_api.inputText = Resolve<decltype(menupage::Api::inputText)>("igInputText");
	g_api.itemHovered = &ItemHovered;
	g_api.itemActive = &ItemActive;
	g_api.wheel = &Wheel;
	g_api.pushTextWrapPos = Resolve<decltype(menupage::Api::pushTextWrapPos)>("igPushTextWrapPos");
	g_api.popTextWrapPos = Resolve<decltype(menupage::Api::popTextWrapPos)>("igPopTextWrapPos");

	if (!g_api.Ready()) {
		LOG_W("%s", i18n::T(i18n::S::LogFrameworkOld));
		return false;
	}

	using AddSectionItem_t = void (*)(const char*, RenderFn);
	using GetVersion_t = float (*)();

	if (auto version = Resolve<GetVersion_t>("GetMenuFrameworkVersion")) {
		LOG_I("SKSE Menu Framework %.2f", version());
	}

	auto addSection = Resolve<AddSectionItem_t>("AddSectionItem");
	if (!addSection) {
		LOG_W("%s", i18n::T(i18n::S::LogFrameworkOld));
		return false;
	}

	// "category/page" - the framework creates the category node for us
	const std::string volume = std::string(i18n::T(i18n::S::MenuCategory)) + "/" + i18n::T(i18n::S::MenuPageVolume);
	const std::string settings = std::string(i18n::T(i18n::S::MenuCategory)) + "/" + i18n::T(i18n::S::MenuPageSettings);
	addSection(volume.c_str(), &RenderVolume);
	addSection(settings.c_str(), &RenderSettings);

	g_registered = true;
	LOG_I("%s (%s, %s) wheel=%s", i18n::T(i18n::S::LogRegistered), volume.c_str(), settings.c_str(),
		fwio::Ready() ? "on" : "off");
	return true;
}

void Unregister()
{
	g_registered = false;
	g_api = menupage::Api{};
	g_cfg = nullptr;
	g_mix = nullptr;
	g_dir = nullptr;
}

}  // namespace cms::menu
