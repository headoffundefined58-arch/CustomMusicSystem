#include "i18n.h"

#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "core.h"

namespace fs = std::filesystem;

namespace cms::i18n {

namespace {

constexpr int kCount = static_cast<int>(S::Count);

// Keys used in the loose translation files (and mirrored in the .ini comments).
constexpr const char* kKeys[kCount] = {
	"$CMS_MENU_CATEGORY",
	"$CMS_MENU_PAGE_VOLUME",
	"$CMS_MENU_PAGE_SETTINGS",

	"$CMS_MASTER_TITLE",
	"$CMS_MASTER_HINT",
	"$CMS_STOPPED",

	"$CMS_NOW_PLAYING",
	"$CMS_LAYER_AMBIENT",
	"$CMS_LAYER_COMBAT",
	"$CMS_NOTHING",
	"$CMS_GATE_CLOSED",
	"$CMS_STATUS_OK",

	"$CMS_MIXER_TITLE",
	"$CMS_MIXER_HINT",
	"$CMS_GROUP_COUNT",
	"$CMS_TRACK_COUNT",
	"$CMS_NO_TRACKS",
	"$CMS_BTN_ALL_FULL",
	"$CMS_BTN_SAVE",
	"$CMS_EDIT_HINT",
	"$CMS_LIB_TITLE",
	"$CMS_HINT_LEGACY_MIXER",

	"$CMS_ENABLE_MUSIC",
	"$CMS_ENABLE_HINT",

	"$CMS_SEC_TRANSITION",
	"$CMS_SEC_SWITCH",
	"$CMS_SEC_COMBAT",
	"$CMS_SEC_LIBRARY",
	"$CMS_SEC_VANILLA",
	"$CMS_SEC_INTERFACE",

	"$CMS_LBL_MIXER_AUTOSAVE",
	"$CMS_LBL_FADE_LOCATION",
	"$CMS_LBL_FADE_COMBAT_IN",
	"$CMS_LBL_FADE_COMBAT_OUT",
	"$CMS_LBL_HANDBACK_FADE",
	"$CMS_LBL_COMBAT_HOLD",
	"$CMS_LBL_WORLD_SETTLE",
	"$CMS_LBL_MIN_PLAY",
	"$CMS_LBL_SWITCH_SETTLE",
	"$CMS_LBL_SWITCH_COOLDOWN",
	"$CMS_LBL_SHEATHE_ENDS_COMBAT",
	"$CMS_LBL_COMBAT_DUCK",
	"$CMS_LBL_DUCK_DIALOGUE",
	"$CMS_LBL_DIALOGUE_DUCK",
	"$CMS_LBL_RECENT",
	"$CMS_LBL_MIN_TRACKS",
	"$CMS_LBL_KEEP_VANILLA",
	"$CMS_LBL_LANGUAGE",

	"$CMS_VANILLA_KEPT",
	"$CMS_VANILLA_TAKEN",
	"$CMS_VANILLA_FALLBACK",
	"$CMS_LANG_EN",
	"$CMS_LANG_ZH",

	"$CMS_BTN_SAVE_CFG",
	"$CMS_BTN_RELOAD_CFG",
	"$CMS_BTN_DEFAULTS",
	"$CMS_HINT_LIVE",
	"$CMS_HINT_WHEEL",
	"$CMS_HINT_SAVED",
	"$CMS_HINT_SAVE_FAILED",
	"$CMS_HINT_SWITCH_GUARD",
	"$CMS_HINT_COMBAT_DUCK",
	"$CMS_HINT_VANILLA_HANDOVER",
	"$CMS_HINT_LANG_BUILD",

	"$CMS_REASON_DUEL",
	"$CMS_REASON_AMBUSH",
	"$CMS_REASON_SUSTAINED",
	"$CMS_REASON_NONE",

	"$CMS_LOG_REGISTERED",
	"$CMS_LOG_NO_FRAMEWORK",
	"$CMS_LOG_FRAMEWORK_OLD",
	"$CMS_LOG_MIXER_LOADED",
	"$CMS_LOG_MIXER_MISSING",
	"$CMS_LOG_CFG_SAVED",
	"$CMS_LOG_CFG_FAILED",
};

constexpr const char* kEn[kCount] = {
	"Custom Music System",
	"Volume",
	"Settings",

	"Master volume",
	"One volume for the whole library. Drag the slider, or hover it and roll the wheel for 1 % steps.",
	"Stopped",

	"Now playing",
	"Ambient layer",
	"Combat layer",
	"(nothing)",
	"Waiting for the world (output gate closed)",
	"Running",

	"Volume",
	"Every track plays at this volume; the value is stored in the mixer file.",
	"groups",
	"tracks",
	"The library is empty. Place .xwm files in Data\\Music\\<event folder>\\ and restart the game.",
	"Reset to 100 %",
	"Save now",
	"Changes are written to disk about one second after the last edit.",
	"Library",
	"Per-folder and per-track volumes from an older mixer file were discarded; one master volume applies now.",

	"Enable music",
	"Off = this plugin stops and the vanilla music takes the context back. The master volume is kept.",

	"Crossfades",
	"Track switching",
	"Combat",
	"Library",
	"Vanilla music",
	"Interface",

	"Autosave volume",
	"Location crossfade",
	"Combat layer fade-in",
	"Combat layer fade-out",
	"Vanilla hand-back fade",
	"Combat layer hold",
	"Start delay after load",
	"Minimum play time",
	"Switch settle time",
	"Switch interval floor",
	"Sheathing ends combat",
	"Combat takeover fade",
	"Attenuate during dialogue",
	"Ambient volume in dialogue",
	"Repeat-suppression window",
	"Minimum pool size",
	"Keep vanilla music",
	"Menu language",

	"Vanilla: kept (plays alongside this plugin)",
	"Vanilla: taken over (this plugin is playing)",
	"Vanilla: hand-back (no track for this event or location)",
	"English",
	"Chinese",

	"Save to INI",
	"Reload from INI",
	"Defaults",
	"Changes apply immediately; Save writes them to the INI.",
	"Hover a slider and roll the wheel to fine-tune it.",
	"Saved",
	"Save failed - see the log",
	"A new context must hold for the settle time, the current track gets its minimum play time, and nothing new starts over a running crossfade.",
	"Seconds the current track takes to fade out when a fight replaces it. The new track starts only once the old one is silent, so two pieces never play at once.",
	"Off (default) = takeover: where the library has a matching track the vanilla layer is held at 0 %, where it has none the vanilla music is handed the context back. On = the vanilla music plays alongside.",
	"Decided by the installed build: the English and the Chinese version are two separate DLLs.",

	"Confirmed duel (you struck, they struck back)",
	"Ambush (they started it)",
	"Prolonged fight (hostile stayed engaged)",
	"No fight",

	"volume page registered in the SKSE menu framework",
	"SKSE Menu Framework not found - the pages are skipped (the mixer file still applies)",
	"SKSE Menu Framework found but its imgui exports are incomplete - pages skipped",
	"mixer loaded:",
	"no mixer file yet, starting from defaults:",
	"settings written to",
	"could not write settings to",
};

constexpr const char* kZh[kCount] = {
	"个性化音乐系统",
	"音量",
	"设置",

	"主音量",
	"整座曲库共用一个音量：拖动滑块，或把指针停在滑块上滚动滚轮微调（1% 步进）。",
	"已停止",

	"正在播放",
	"环境层",
	"战斗层",
	"（无）",
	"等待世界就绪（输出闸门关闭）",
	"运行中",

	"音量",
	"所有曲目均按此音量播放；数值保存在混音文件中。",
	"组",
	"首",
	"曲库为空：把 .xwm 文件放入 Data\\Music\\<事件目录>\\ 后重新启动游戏。",
	"重置为 100%",
	"立即保存",
	"最后一次操作约 1 秒后写入磁盘。",
	"曲库",
	"旧版混音文件中的分组、单曲音量已作废；现在只保留一个主音量。",

	"启用音乐",
	"关闭后本插件停止播放，原版音乐取回播放权；主音量仍保留。",

	"交叉淡化",
	"换曲控制",
	"战斗",
	"曲库",
	"原版音乐",
	"界面",

	"音量设置自动保存",
	"场景切换交叉淡化",
	"战斗层淡入",
	"战斗层淡出",
	"原版交还淡出",
	"战斗层保持时长",
	"载入后起播延迟",
	"曲目最短播放时长",
	"换曲判定延迟",
	"换曲最小间隔",
	"收起武器结束战斗",
	"战斗接替淡出",
	"对话期间压低环境层",
	"对话期间环境层增益",
	"曲目重复抑制窗口",
	"有效曲目池下限",
	"保留原版音乐",
	"界面语言",

	"原版音乐：保留（与本插件并行播放）",
	"原版音乐：已接管（本插件正在播放）",
	"原版音乐：已交还（当前事件或地点无匹配曲目）",
	"英文",
	"中文",

	"保存到 INI",
	"从 INI 重读",
	"恢复默认",
	"改动立即生效；点击保存后写入 INI。",
	"指针停在滑块上滚动滚轮可微调。",
	"已保存",
	"保存失败，详见日志",
	"新场景须持续经过判定延迟才换曲；当前曲目须播满最短播放时长；交叉淡化进行中不叠加新曲。",
	"战斗取代当前音乐时，旧曲淡出的秒数。旧曲完全静音后才开始播放战斗曲，两首不会同时响起。",
	"关（默认）＝接管：曲库中有匹配曲目时原版层音量归零，无匹配曲目时将播放权交还原版。开＝保留原版，两套音乐并行播放。",
	"由所安装的构建决定：英文版与中文版是两个独立的 dll。",

	"已确认交战（你先出手，对方还击）",
	"遭遇伏击（对方先出手）",
	"持续交战（敌对目标保持交战状态）",
	"未交战",

	"音量页已注册到 SKSE 菜单框架",
	"未检测到 SKSE 菜单框架：页面跳过（混音文件照常生效）",
	"检测到 SKSE 菜单框架，但 imgui 导出不完整：页面跳过",
	"已读取混音文件:",
	"尚无混音文件，使用默认值:",
	"设置已写入",
	"设置写入失败：",
};

// ------------------------------------------------------------ fixed table ---
// Exactly one of the two tables is referenced by the compiled output, so the
// English DLL does not carry Chinese text and vice versa.  (Both arrays exist in
// the source; the linker drops the unused one.)
constexpr const char* const* kTable = kChineseBuild ? kZh : kEn;
constexpr const char*        kLangTag = kChineseBuild ? "zh" : "en";

}  // namespace

void Init(const std::string& a_dataDir, const std::string& a_gameLanguage, const std::string& a_override)
{
	// Nothing is loaded: the table is a compile-time constant.  Keeping the
	// parameters (and logging them) makes a bug report tell us which build the
	// player is running, which is the only thing that decides the language now.
	LOG_I("ui language: %s (built-in %s table; game says '%s', ini says '%s')", kLangTag,
		kChineseBuild ? "chinese" : "english", a_gameLanguage.empty() ? "?" : a_gameLanguage.c_str(),
		a_override.empty() ? "auto" : a_override.c_str());
	(void)a_dataDir;
}

const char* T(S a_id)
{
	const auto i = static_cast<std::size_t>(a_id);
	if (i >= static_cast<std::size_t>(kCount)) return "";
	return kTable[i];
}

const char* Lang() { return kLangTag; }

const char* GameLanguage() { return kLangTag; }  // one build, one language

const char* TranslationPath() { return ""; }

}  // namespace cms::i18n
