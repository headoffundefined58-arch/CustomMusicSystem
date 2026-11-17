// Menu strings.
//
// The page is a *player* page: it shows what the player can decide and hides the
// engine internals (scheduler period, library path, encoding diagnostics).  Every
// label that stays is named after the audio concept it controls - crossfade,
// gain, pool, hand-back - because "level 0.45" means nothing without a noun.
//
// The language is a compile-time constant (see kChineseBuild below); there is no
// loose translation file and no runtime switch.
#pragma once

#include <string>

namespace cms::i18n {

enum class S {
	// menu tree
	MenuCategory = 0,
	MenuPageVolume,
	MenuPageSettings,

	// volume page - master
	MasterTitle,
	MasterHint,
	Stopped,

	// volume page - now playing
	NowPlayingTitle,
	LayerAmbient,
	LayerCombat,
	NothingPlaying,
	StatusGateClosed,
	StatusOk,

	// volume page - level block + library size
	MixerTitle,
	MixerHint,
	GroupCount,
	TrackCount,
	NoTracks,
	BtnAllFull,
	BtnSave,
	EditHint,
	LibTitle,
	HintLegacyMixer,

	// settings - master switch
	EnableMusic,
	EnableMusicHint,

	// settings - sections
	SecTransition,
	SecSwitch,
	SecCombat,
	SecLibrary,
	SecVanilla,
	SecInterface,

	// settings - rows
	LblMixerAutosave,
	LblFadeLocation,
	LblFadeCombatEnter,
	LblFadeCombatExit,
	LblHandbackFade,
	LblCombatHold,
	LblWorldSettle,
	LblMinPlay,
	LblSwitchSettle,
	LblSwitchCooldown,
	LblSheatheEndsCombat,
	LblFadeSwap,
	LblDuckInDialogue,
	LblDialogueDuck,
	LblRecentWindow,
	LblMinTracks,
	LblKeepVanilla,
	LblLanguage,

	// vanilla handover state shown on the volume page
	VanillaKept,
	VanillaTakenOver,
	VanillaFallback,
	LangEnglish,
	LangChinese,

	// settings buttons / hints
	BtnSaveConfig,
	BtnReloadConfig,
	BtnDefaults,
	HintLive,
	HintWheel,
	HintSaved,
	HintSaveFailed,
	HintSwitchGuard,
	HintFadeSwap,
	HintVanillaHandover,
	HintLangBuild,

	// combat reason shown on the volume page
	ReasonDuel,
	ReasonAmbush,
	ReasonSustained,
	ReasonNone,

	// logs
	LogRegistered,
	LogNoFramework,
	LogFrameworkOld,
	LogMixerLoaded,
	LogMixerMissing,
	LogConfigSaved,
	LogConfigSaveFailed,

	Count
};

//   english build : -DCMS_UI_LANG_ZH=0
//   chinese build : -DCMS_UI_LANG_ZH=1
#ifndef CMS_UI_LANG_ZH
#define CMS_UI_LANG_ZH 1
#endif

constexpr bool kChineseBuild = CMS_UI_LANG_ZH != 0;

// Kept so callers/tests do not have to know about the build flag; both values
// are ignored, the table is fixed.
void Init(const std::string& a_dataDir, const std::string& a_gameLanguage, const std::string& a_override);

[[nodiscard]] const char* T(S a_id);
[[nodiscard]] const char* Lang();             // "zh" or "en" - decided at build time
[[nodiscard]] const char* GameLanguage();     // raw sLanguage of this session (log only)
[[nodiscard]] const char* TranslationPath();  // always "" - no loose files are read

}  // namespace cms::i18n
