// The pages themselves, expressed against an "imgui api" struct instead of the
// framework's exports.  That keeps all layout/pairing logic in a translation
// unit that has no Windows, no game and no ImGui dependency, so an off-line test
// can drive it with a recorder and prove the widget sequence is balanced
// (an unmatched BeginChild/PushID is an ImGui assertion, i.e. a crash in game).
//
// Layout rules the pages follow, because the framework's window is narrower than
// a desktop app and its font is large:
//   * two columns only - label on the left, one control on the right; no third
//     column, nothing is allowed to grow past the window edge;
//   * every explanatory line goes through Wrapped(), so a long sentence folds
//     instead of being clipped at the border;
//   * no per-track lists: one volume slider for the whole library.
#pragma once

#include <cstddef>
#include <string>

namespace cms {

struct Config;
class VolumeMix;

namespace menupage {

struct Vec2 {
	float x;
	float y;
};

struct Vec4 {
	float x;
	float y;
	float z;
	float w;
};

// The subset of cimgui the pages need.  Signatures match the framework's
// exports one to one; everything below Ready() is optional and only costs the
// widget that depends on it.
struct Api {
	void (*text)(const char*, ...) = nullptr;
	void (*textRaw)(const char*, const char*) = nullptr;
	bool (*sliderFloat)(const char*, float*, float, float, const char*, int) = nullptr;
	bool (*checkbox)(const char*, bool*) = nullptr;
	bool (*button)(const char*, Vec2) = nullptr;
	void (*sameLine)(float, float) = nullptr;
	void (*separatorText)(const char*) = nullptr;
	void (*separator)() = nullptr;
	void (*spacing)() = nullptr;
	bool (*beginChild)(const char*, Vec2, int, int) = nullptr;
	void (*endChild)() = nullptr;
	void (*pushID)(const char*) = nullptr;
	void (*popID)() = nullptr;
	void (*setItemWidth)(float) = nullptr;
	void (*progressBar)(float, Vec2, const char*) = nullptr;
	void (*pushStyleColor)(int, Vec4) = nullptr;
	void (*popStyleColor)(int) = nullptr;

	// settings page + mouse wheel + text folding
	bool (*sliderInt)(const char*, int*, int, int, const char*, int) = nullptr;
	bool (*radioButton)(const char*, int*, int) = nullptr;  // igRadioButton_IntPtr
	bool (*inputText)(const char*, char*, std::size_t, int, void*, void*) = nullptr;
	bool (*itemHovered)() = nullptr;  // igIsItemHovered(0) for the last item
	bool (*itemActive)() = nullptr;   // igIsItemActive()
	float (*wheel)() = nullptr;       // ImGuiIO::MouseWheel of this frame
	void (*pushTextWrapPos)(float) = nullptr;  // 0 = fold at the window edge
	void (*popTextWrapPos)() = nullptr;

	[[nodiscard]] bool Ready() const
	{
		return text && sliderFloat && checkbox && button && sameLine && separatorText && beginChild &&
		       endChild && pushID && popID && setItemWidth;
	}
};

// What the director reports, flattened so the page does not include it.
struct State {
	bool        gateOpen = false;
	bool        combat = false;
	bool        musicEnabled = true;
	int         combatReason = 0;  // cms::EngageReason
	std::string ambientLabel;
	std::string ambientTrack;
	std::string combatLabel;
	std::string combatTrack;
	float       ambientGain = 0.0f;
	// true = this plugin owns the event/location (vanilla layer held back);
	// false = the vanilla music is playing again (fallback)
	bool        vanillaTakenOver = false;
};

// Just the counts: the page no longer lists tracks, it only says how much music
// the engine found (and therefore why nothing is playing when it is 0).
struct LibraryStats {
	std::size_t folders = 0;
	std::size_t tracks = 0;
};

// Out-parameters of the settings page: the host performs the file work, the
// page only says what the player clicked.
struct SettingsActions {
	bool        save = false;
	bool        reload = false;
	bool        defaults = false;
};

// Draws the volume page.  Called from inside the framework's own ImGui frame,
// so no Begin/End of its own.
void Build(const Api& a_api, Config& a_cfg, VolumeMix& a_mix, const State& a_state, const LibraryStats& a_stats,
	const std::string& a_mixerPath);

// Draws the settings page (every INI key, editable).
void BuildSettings(const Api& a_api, Config& a_cfg, SettingsActions& a_actions);

}  // namespace menupage
}  // namespace cms
