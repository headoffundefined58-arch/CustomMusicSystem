#include "core.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <random>

namespace fs = std::filesystem;

namespace cms {

// ============================================================== logging =====
namespace {
std::mutex  g_logMutex;
std::FILE*  g_logFile = nullptr;
std::string g_logPath;
bool        g_verbose = false;

std::string ToLower(std::string_view s)
{
	std::string r(s);
	for (auto& c : r) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	return r;
}

std::string_view Trim(std::string_view s)
{
	while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r' || s.front() == '\n')) s.remove_prefix(1);
	while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n')) s.remove_suffix(1);
	return s;
}
}  // namespace

void LogOpen(const std::string& a_path)
{
	std::lock_guard lock(g_logMutex);
	if (g_logFile) { std::fclose(g_logFile); g_logFile = nullptr; }
	g_logPath = a_path;
	std::error_code ec;
	fs::create_directories(fs::path(a_path).parent_path(), ec);
	fopen_s(&g_logFile, a_path.c_str(), "wb");
}

void LogClose()
{
	std::lock_guard lock(g_logMutex);
	if (g_logFile) { std::fclose(g_logFile); g_logFile = nullptr; }
}

void LogFlush()
{
	std::lock_guard lock(g_logMutex);
	if (g_logFile) std::fflush(g_logFile);
}

void SetVerbose(bool a_on)
{
	std::lock_guard lock(g_logMutex);
	g_verbose = a_on;
}

// ========================================================== IMR handshake ===
bool ImrLinkActive(const ImrLink& a_link, std::uint64_t a_nowMs, std::uint64_t a_maxAgeMs)
{
	if (a_link.magic != kImrMagic) return false;
	if (a_link.version == 0 || a_link.version > kImrVersion) return false;
	if (a_link.state != 1u) return false;   // 0 = idle, anything else = unknown
	if (a_link.heartbeatMs == 0) return false;

	// Both sides read GetTickCount64(), a monotonic per-boot counter, so this is
	// a straight subtraction.  A heartbeat stamped in the future means the block
	// was written by a different boot (the counter restarted lower) - treat it
	// as stale rather than trusting it forever.
	if (a_link.heartbeatMs > a_nowMs) return false;
	return (a_nowMs - a_link.heartbeatMs) <= a_maxAgeMs;
}

void LogMsg(const char* a_level, const char* a_fmt, ...)
{
	char body[2048];
	va_list ap;
	va_start(ap, a_fmt);
	vsnprintf(body, sizeof(body), a_fmt, ap);
	va_end(ap);

	std::lock_guard lock(g_logMutex);
	if (!g_logFile) return;
	// "debug" lines are trace noise; dropped unless verbose logging is on.
	if (a_level && std::strcmp(a_level, "debug") == 0 && !g_verbose) return;
	std::fprintf(g_logFile, "[%s] %s\n", a_level, body);
	std::fflush(g_logFile);
}

// =============================================================== config =====
namespace {
bool ParseBool(std::string_view v, bool a_default)
{
	const auto s = ToLower(v);
	if (s == "1" || s == "true" || s == "yes" || s == "on") return true;
	if (s == "0" || s == "false" || s == "no" || s == "off") return false;
	return a_default;
}
}  // namespace

void Config::ApplyKey(std::string_view a_key, std::string_view a_value)
{
	const auto k = ToLower(a_key);
	const auto v = std::string(a_value);
	const auto f = static_cast<float>(std::atof(v.c_str()));
	const auto i = std::atoi(v.c_str());

	if (k == "ffadelocation") fadeLocation = std::max(0.0f, f);
	else if (k == "ffadecombatenter") fadeCombatEnter = std::max(0.0f, f);
	else if (k == "ffadecombatexit") fadeCombatExit = std::max(0.0f, f);
	else if (k == "fhandbackfade") fadeHandback = std::max(0.0f, f);
	// 2.3 and older called that fade "fFadeSilence" (it faded to silence); the
	// same key still loads so an existing ini keeps its tuning.
	else if (k == "ffadesilence") fadeHandback = std::max(0.0f, f);
	else if (k == "fcombathold") combatHold = std::max(0.0f, f);
	else if (k == "fworldsettle") worldSettle = std::max(0.0f, f);
	else if (k == "fminplay") minPlay = std::clamp(f, 0.0f, 600.0f);
	else if (k == "fswitchsettle") switchSettle = std::clamp(f, 0.0f, 60.0f);
	else if (k == "fswitchcooldown") switchCooldown = std::clamp(f, 0.0f, 120.0f);
	else if (k == "bsheathendscombat") sheatheEndsCombat = ParseBool(v, sheatheEndsCombat);
	else if (k == "irecentwindow") recentWindow = std::clamp(i, 0, 64);
	else if (k == "imintracks") minTracks = std::clamp(i, 0, 64);
	else if (k == "bsingleformatcheck") singleFormatCheck = ParseBool(v, singleFormatCheck);
	else if (k == "suilanguage") uiLanguage = v;
	else if (k == "bkeepvanilla") keepVanilla = ParseBool(v, keepVanilla);
	// Legacy key from the three-way "vanilla handling" switch.  Only the
	// "leave vanilla alone" setting has a meaning that survives: it becomes
	// keepVanilla = 1.  The old "mute" value (1) and the disabled takeover value
	// (2) both mean "this plugin owns the music", which is now the default
	// takeover-with-fallback behaviour.
	else if (k == "ivanillamode") keepVanilla = (i == 0);
	else if (k == "benabled") enabled = ParseBool(v, enabled);
	else if (k == "ffadeswap") fadeSwap = std::clamp(f, 0.05f, 5.0f);
	else if (k == "fcombatarm") combatArmTime = std::max(0.0f, f);
	else if (k == "bduckindialogue") duckInDialogue = ParseBool(v, duckInDialogue);
	else if (k == "fdialogueduck") dialogueDuck = std::clamp(f, 0.0f, 1.0f);
	else if (k == "fchangesmooth") changeSmooth = std::clamp(f, 0.0f, 5.0f);
	else if (k == "bmixerautosave") mixerAutosave = ParseBool(v, mixerAutosave);
	else if (k == "itickms") tickMs = std::clamp(i, 10, 500);
	else if (k == "bverbose") verbose = ParseBool(v, verbose);
}

bool Config::LoadFromFile(const std::string& a_path)
{
	std::ifstream in(a_path, std::ios::binary);
	if (!in) return false;

	std::string line;
	while (std::getline(in, line)) {
		// strip comments (';' and '#') anywhere
		const auto cut = line.find_first_of(";#");
		if (cut != std::string::npos) line.resize(cut);

		auto sv = Trim(line);
		if (sv.empty()) continue;
		if (sv.front() == '[') continue;  // section header, informational only

		const auto eq = sv.find('=');
		if (eq == std::string_view::npos) continue;

		auto key = Trim(sv.substr(0, eq));
		auto val = Trim(sv.substr(eq + 1));
		// allow `key : value` too (some users copy JSON-ish lines)
		if (key.empty() || val.empty()) continue;
		ApplyKey(key, val);
	}
	iniPath = a_path;
	return true;
}

// The canonical file: every key the plugin understands, commented in both
// languages.  The in-game settings page writes this shape, so an install that
// was configured through the menu and one edited by hand look the same.
bool Config::SaveToFile(const std::string& a_path) const
{
	std::error_code ec;
	const fs::path  p(a_path);
	if (!p.parent_path().empty()) fs::create_directories(p.parent_path(), ec);

	// keep one backup of the hand-edited file
	fs::path bak = p;
	bak += ".bak";
	std::error_code eb;
	if (fs::exists(p, eb)) fs::copy_file(p, bak, fs::copy_options::overwrite_existing, eb);

	std::FILE* f = nullptr;
	if (fopen_s(&f, a_path.c_str(), "wb") != 0 || !f) return false;

	std::fprintf(f, "; CustomMusicSystem - runtime settings / 运行时设置\n");
	std::fprintf(f, "; Written by the in-game page.  The previous file is kept as .bak.\n");
	std::fprintf(f, "; 由游戏内设置页写入；上一份文件保留为 .bak。\n");
	std::fprintf(f, "; Booleans: 1 = on, 0 = off.  Floats: seconds unless noted.\n");
	std::fprintf(f, "; 布尔值 1=开 0=关；浮点数单位除注明外为秒。\n\n");

	std::fprintf(f, "[Audio]\n");
	std::fprintf(f, "; Master switch. 0 = no music at all (the volume page keeps its level).\n");
	std::fprintf(f, "; 音乐总开关：0 = 本插件停止播放、播放权交还原版音乐；主音量仍保留。\n");
	std::fprintf(f, "bEnabled          = %d\n", enabled ? 1 : 0);
	std::fprintf(f, "bMixerAutosave    = %d    ; write volume changes to the mixer file / 音量改动自动写盘\n", mixerAutosave ? 1 : 0);
	std::fprintf(f, "; Combat only attenuates the ambient layer - it never stops it. 1.00 = leave\n"
	                 "; it untouched (both layers audible), 0.00 = the running track loses all\n"
	                 "; its gain, i.e. a fight replaces it.\n"
	                 "; 战斗只降低环境层音量、从不中断它：1.00 = 完全不压低（两首并行），0.00 = 完全让出。\n");
	std::fprintf(f, "fFadeSwap         = %.2f  ; outgoing track fade when an event takes over (exclusive handover) / 事件接管时旧曲淡出（独占接替）\n", fadeSwap);
	std::fprintf(f, "bDuckInDialogue   = %d    ; attenuate the ambient layer under the dialogue menu / 对话期间压低环境层\n", duckInDialogue ? 1 : 0);
	std::fprintf(f, "fDialogueDuck     = %.2f  ; ambient layer gain while talking / 对话期间环境层增益\n", dialogueDuck);
	std::fprintf(f, "fChangeSmooth     = %.2f  ; gain smoothing time / 增益平滑时间\n\n", changeSmooth);

	std::fprintf(f, "[Transition]\n");
	std::fprintf(f, "; equal-power crossfade lengths / 等功率交叉淡化时长\n");
	std::fprintf(f, "fFadeLocation     = %.2f  ; location or time-of-day change / 场景或时段切换\n", fadeLocation);
	std::fprintf(f, "fFadeCombatEnter  = %.2f  ; combat layer fade-in / 战斗层淡入\n", fadeCombatEnter);
	std::fprintf(f, "fFadeCombatExit   = %.2f  ; combat layer fade-out / 战斗层淡出\n", fadeCombatExit);
	std::fprintf(f, "fHandbackFade     = %.2f  ; handing the context back to the vanilla layer / 交还原版时的淡出\n", fadeHandback);
	std::fprintf(f, "fCombatHold       = %.2f  ; combat layer hold after the fight reads as over (AI flag only) / 战斗层保持时长\n", combatHold);
	std::fprintf(f, "fCombatArm        = %.2f  ; min fight duration before the combat track starts (filters momentary scuffles) / 战斗曲最短触发时长（过滤瞬间误触发）\n", combatArmTime);
	std::fprintf(f, "fWorldSettle      = %.2f  ; delay before the first note after a load / 载入后起播延迟\n", worldSettle);
	std::fprintf(f, "; Track switching: minimum play time, settle time of a new context, floor between two switches / 换曲控制\n");
	std::fprintf(f, "fMinPlay          = %.2f  ; minimum play time of a track / 曲目最短播放时长\n", minPlay);
	std::fprintf(f, "fSwitchSettle     = %.2f  ; a new context must persist this long / 换曲判定延迟\n", switchSettle);
	std::fprintf(f, "fSwitchCooldown   = %.2f  ; floor between two switches of one layer / 换曲最小间隔\n\n", switchCooldown);

	std::fprintf(f, "[Behavior]\n");
	std::fprintf(f, "iRecentWindow     = %d    ; track repeat-suppression window / 曲目重复抑制窗口\n", recentWindow);
	std::fprintf(f, "iMinTracks        = %d    ; a smaller pool counts as empty -> vanilla fallback / 有效曲目池下限\n", minTracks);
	std::fprintf(f, "bSheatheEndsCombat= %d    ; sheathing ends the combat event / 收起武器结束战斗事件\n", sheatheEndsCombat ? 1 : 0);
	std::fprintf(f, "; The two keys below are engine diagnostics and are not exposed in the menu.\n");
	std::fprintf(f, "; 以下两项为引擎内部参数，菜单中不提供，仅可在此调整。\n");
	std::fprintf(f, "iTickMs           = %d    ; engine period in ms (restart) / 引擎调度周期（重启生效）\n", tickMs);
	std::fprintf(f, "bSingleFormatCheck= %d    ; log a warning per folder mixing .xwm and .wav / 混用编码格式时写日志\n",
		singleFormatCheck ? 1 : 0);
	std::fprintf(f, "bVerbose          = %d    ; 1 = also log trace/debug lines / 1=同时输出调试级日志\n\n",
		verbose ? 1 : 0);

	std::fprintf(f, "[Vanilla]\n");
	std::fprintf(f, "; 0 = takeover (default).  This plugin replaces the vanilla music system://n");
	std::fprintf(f, ";     the vanilla layer is held back while a personalized track exists for\n");
	std::fprintf(f, ";     the current event/location, and is handed the context back (fallback)\n");
	std::fprintf(f, ";     when none does - a place the library does not cover plays vanilla music.\n");
	std::fprintf(f, "; 1 = keep the vanilla music playing alongside this plugin.\n");
	std::fprintf(f, "; 0=接管（默认；无匹配曲目时回退原版） 1=保留原版（两套并行播放）\n");
	std::fprintf(f, "bKeepVanilla      = %d\n\n", keepVanilla ? 1 : 0);

	std::fprintf(f, "[UI]\n");
	std::fprintf(f, "; auto | english | chinese (applies at once) / 界面语言（改完立即生效）\n");
	std::fprintf(f, "sUILanguage       = %s\n", uiLanguage.c_str());

	if (std::fclose(f) != 0) return false;
	return true;
}

// ============================================================ combat gate ===
void CombatGate::Reset()
{
	offenseAt_ = engagedAt_ = -1.0e9;
	duelSince_ = duelIdleSince_ = -1.0;
	sheathedAt_ = -1.0;
	sustainSince_ = -1.0;
	fleeSince_ = -1.0;
	fleeLock_ = false;
	engaged_ = false;
	raised_ = false;
	reason_ = EngageReason::None;
}

void CombatGate::Release()
{
	engaged_ = false;
	raised_ = false;
	reason_ = EngageReason::None;
	duelSince_ = duelIdleSince_ = -1.0;
	sheathedAt_ = -1.0;
	fleeSince_ = -1.0;
	offenseAt_ = -1.0e9;   // a new fight needs a new offense, not the old one
}

void CombatGate::Engage(double a_now, EngageReason a_reason)
{
	if (!engaged_) {
		engaged_ = true;
		raised_ = true;
		reason_ = a_reason;
	} else if (a_reason < reason_) {
		// a confirmed duel outranks an ambush guess, never the other way round
		reason_ = a_reason;
	}
	engagedAt_ = a_now;
	sheathedAt_ = -1.0;
	sustainSince_ = -1.0;
	fleeSince_ = -1.0;
	fleeLock_ = false;  // something actually happened: we are back in a fight
}

// The player damaged a live actor.  This is the *only* thing that counts as
// "the player started it" - swinging at a wall fires no hit event at all, and
// hitting an ore vein fails the live-actor test before we get here.
void CombatGate::OnPlayerHit(double a_now)
{
	offenseAt_ = a_now;
	fleeSince_ = -1.0;
	fleeLock_ = false;
}

// The enemy answered an offense the player made.  This does NOT open the gate:
// a player-initiated fight only becomes a combat event once it has lasted
// kDuelConfirm (see Update).  An assassination - one hit, the target dead
// before anything really fights back - ends inside that window and never
// becomes a fight, which is exactly the false positive this replaces.
void CombatGate::PendDuel(double a_now)
{
	if (duelSince_ < 0.0) duelSince_ = a_now;
}

// An enemy's own combat flag flipped with the player as the target: it either
// answers our attack (duel, pending confirmation) or started the fight on its
// own (ambush, immediate - the enemy attacking first IS a fight).
void CombatGate::OnEnemyEngage(double a_now)
{
	if (RecentOffense(a_now)) PendDuel(a_now);
	else Engage(a_now, EngageReason::Ambush);
}

void CombatGate::OnEnemyHit(double a_now)
{
	if (RecentOffense(a_now)) PendDuel(a_now);
	else Engage(a_now, EngageReason::Ambush);
}

void CombatGate::OnHostileInCombat(double a_now, bool a_any)
{
	if (!a_any) {
		sustainSince_ = -1.0;
		return;
	}
	if (sustainSince_ < 0.0) sustainSince_ = a_now;
}

CombatGate::Verdict CombatGate::Update(double a_now, bool a_aiInCombat, bool a_weaponDrawn, float a_nearestHostile)
{
	Verdict v;
	bool    released = false;
	bool    bySheathe = false;
	bool    byFlee = false;

	if (engaged_) {
		// Sheathing is the player saying "I am done" - it outranks the AI flag,
		// which happily stays in combat while the player loots or picks a lock.
		// Confirming for kSheatheConfirm filters out the animation hitch of a
		// weapon swap (sheath -> draw in two ticks) and a mid-fight sheathe that
		// the game immediately cancels because something is still hitting us.
		if (!a_weaponDrawn) {
			if (sheathedAt_ < 0.0) sheathedAt_ = a_now;
			if (a_now - sheathedAt_ >= kSheatheConfirm) {
				Release();
				released = true;
				bySheathe = true;
			}
		} else {
			sheathedAt_ = -1.0;
		}

		if (engaged_) {
			if (a_aiInCombat) {
				// the AI still says "in combat": keep the gate open, no release timer
				engagedAt_ = a_now;
			} else if (a_now - engagedAt_ > kHold) {
				Release();
				released = true;
			}
		}

		// Retreat: the player broke away instead of finishing the fight.  This
		// has to be decided on distance, because a fleeing player who is still
		// being "searched for" keeps every AI combat flag lit for a minute or
		// more - which is exactly why the combat track used to play on while the
		// player was already two hills away.  Weapon state says nothing here: a
		// retreat usually happens with the weapon still out.
		if (engaged_) {
			if (a_nearestHostile > kFleeDistance) {
				if (fleeSince_ < 0.0) fleeSince_ = a_now;
				if (a_now - fleeSince_ >= kFleeConfirm) {
					Release();
					released = true;
					byFlee = true;
					fleeLock_ = true;  // and do not let the fallback undo it
				}
			} else {
				fleeSince_ = -1.0;
			}
		}
	}

	if (fleeLock_ && a_nearestHostile >= 0.0f && a_nearestHostile <= kFleeDistance) {
		fleeLock_ = false;  // somebody is close again: a real encounter may re-arm
	}

	// A duel the player started is only a fight once it has lasted: the enemy
	// answered (PendDuel), and the exchange has to keep going kDuelConfirm.
	// Sneak assassinations and one-hit kills end inside that window and are
	// cancelled - they are not combat events.  A pending duel also dies when
	// the player sheathes: putting the weapon away is the player saying "this
	// is not a fight".
	if (!engaged_ && duelSince_ >= 0.0) {
		if (a_weaponDrawn && a_aiInCombat) {
			duelIdleSince_ = -1.0;
			if (a_now - duelSince_ >= kDuelConfirm) {
				duelSince_ = -1.0;
				Engage(a_now, EngageReason::Duel);
			}
		} else {
			// the flag can lag the answer event by a tick or two, so the first
			// quiet snapshot only starts a grace clock instead of cancelling
			if (duelIdleSince_ < 0.0) duelIdleSince_ = a_now;
			else if (a_now - duelIdleSince_ >= kDuelGrace) {
				duelSince_ = duelIdleSince_ = -1.0;
			}
		}
	}

	if (!engaged_ && sustainSince_ >= 0.0 && a_now - sustainSince_ >= kSustain) {
		if (fleeLock_) {
			// A hostile that is still flagged "in combat" while it searches for a
			// player who already ran off must not restart the track on the
			// fallback path - and the timer must be fresh, or walking back into
			// the area would re-open the fight on the very first tick.
			sustainSince_ = a_now;
		} else {
			// nothing confirmed the fight, but a hostile has been in combat with us
			// for a while now - better a late track than none at all
			Engage(a_now, EngageReason::Sustained);
		}
	}

	v.combat = engaged_;
	v.reason = reason_;
	v.raised = raised_;
	v.released = released;
	v.bySheathe = bySheathe;
	v.byFlee = byFlee;
	raised_ = false;
	return v;
}

const char* SwitchBlockReason(double a_now, double a_started, double a_switched, double a_wantSince,
    bool a_active, bool a_fading, float a_duration, float a_playhead, float a_minPlay, float a_settle,
    float a_cooldown, float a_fade)
{
	if (a_wantSince < 0.0 || a_now - a_wantSince < static_cast<double>(a_settle)) return "settling";
	if (!a_active) return nullptr;                 // nothing playing: start at once

	if (a_fading) return "crossfade running";

	const bool nearlyEnd = a_duration > 1.0f && a_playhead > 0.0f &&
		(a_duration - a_playhead) <= std::max(1.0f, a_fade);
	if (!nearlyEnd && a_now - a_started < static_cast<double>(a_minPlay)) return "min play time";
	if (a_now - a_switched < static_cast<double>(a_cooldown)) return "cooldown";
	return nullptr;
}

// ============================================================== library =====
int Library::IndexOf(std::string_view a_name) const
{
	for (std::size_t i = 0; i < folders.size(); ++i) {
		if (folders[i].name == a_name) return static_cast<int>(i);
	}
	return -1;
}

Folder* Library::Find(std::string_view a_name)
{
	const int i = IndexOf(a_name);
	return i < 0 ? nullptr : &folders[static_cast<std::size_t>(i)];
}

std::uint64_t Library::TotalBytes() const
{
	std::uint64_t t = 0;
	for (const auto& f : folders) {
		for (const auto& tr : f.tracks) t += tr.size;
	}
	return t;
}

namespace {
int ReadTrackLimit(const fs::path& a_dir, int a_default)
{
	std::error_code ec;
	for (const auto& e : fs::directory_iterator(a_dir, ec)) {
		if (!e.is_regular_file(ec)) continue;
		auto name = ToLower(e.path().filename().string());
		if (name.rfind("track limit", 0) != 0) continue;
		std::ifstream in(e.path());
		int v = 0;
		if (in >> v && v > 0) return std::min(v, 999);
	}
	return a_default;
}

bool StartsWithNoCase(std::string_view a_str, std::string_view a_prefix)
{
	if (a_str.size() < a_prefix.size()) return false;
	for (std::size_t i = 0; i < a_prefix.size(); ++i) {
		if (std::tolower(static_cast<unsigned char>(a_str[i])) != std::tolower(static_cast<unsigned char>(a_prefix[i]))) return false;
	}
	return true;
}
}  // namespace

bool Library::Scan(bool a_force, bool* a_outChanged)
{
	bool changed = false;
	if (a_outChanged) *a_outChanged = false;

	std::error_code ec;
	for (auto& f : folders) {
		const fs::path dir = fs::path(root) / fs::path(f.name);
		if (!fs::is_directory(dir, ec)) {
			if (a_force || !f.scanned) { f.tracks.clear(); f.scanned = true; changed = true; }
			continue;
		}

		// warm rescan only fills folders that are still empty, so adding tracks
		// mid-session works without ever re-hashing (and restarting) a live pool
		if (!a_force && f.scanned && !f.tracks.empty()) continue;

		// hot-rescan guard: only touch folders whose mtime moved
		const auto stamp = static_cast<std::uint64_t>(fs::last_write_time(dir, ec).time_since_epoch().count());
		if (!a_force && f.scanned && stamp == f.stamp) continue;

		f.stamp = stamp;
		f.scanned = true;

		std::vector<TrackRef> found;
		bool hasXwm = false, hasWav = false;
		const std::string want = "mm" + ToLower(f.prefix);

		for (const auto& e : fs::directory_iterator(dir, ec)) {
			if (!e.is_regular_file(ec)) continue;
			const auto name = e.path().filename().string();
			const auto lower = ToLower(name);
			if (!StartsWithNoCase(lower, want)) continue;

			const auto ext = ToLower(e.path().extension().string());
			if (ext != ".xwm" && ext != ".wav") continue;

			// MM<Prefix><NN> - digits only between prefix and extension
			const auto digits = lower.substr(want.size(), lower.size() - want.size() - ext.size());
			if (digits.empty() || digits.size() > 4) continue;
			if (!std::all_of(digits.begin(), digits.end(), [](char c) { return c >= '0' && c <= '9'; })) continue;

			if (ext == ".xwm") hasXwm = true; else hasWav = true;

			std::error_code szec;
			const auto sz = fs::file_size(e.path(), szec);
			found.push_back({ e.path().string(), szec ? 0ull : static_cast<std::uint64_t>(sz) });
		}

		std::sort(found.begin(), found.end(), [](const TrackRef& a, const TrackRef& b) { return a.path < b.path; });

		f.limit = ReadTrackLimit(dir, f.limit);
		if (static_cast<int>(found.size()) > f.limit) {
			// stable, name-seeded sample so the same install always plays the
			// same subset (a changing subset would make the pool hash flap)
			std::mt19937 rng(static_cast<std::uint32_t>(std::hash<std::string>{}(f.name)));
			std::shuffle(found.begin(), found.end(), rng);
			found.resize(static_cast<std::size_t>(f.limit));
			std::sort(found.begin(), found.end(), [](const TrackRef& a, const TrackRef& b) { return a.path < b.path; });
		}

		f.tracks = std::move(found);
		f.mixedFormat = hasXwm && hasWav;
		changed = true;
	}

	if (a_outChanged) *a_outChanged = changed;
	return true;
}

// ================================================================ rules =====
namespace {
Group GroupOf(std::string_view a_folder)
{
	auto starts = [&](std::string_view p) { return a_folder.rfind(p, 0) == 0; };
	if (starts("Player Homes")) return Group::Home;
	if (starts("Town")) return Group::Town;
	if (starts("Ride")) return Group::Ride;
	if (starts("Combat") || starts("DB Combat")) return Group::Combat;
	if (starts("Dungeon") || starts("DB Dungeon") || starts("Vampire")) return Group::Dungeon;
	if (starts("Explore") || starts("DB Explore") || starts("Soul Cairn") || starts("Falmer Valley")) return Group::Explore;
	return Group::Interior;
}
}  // namespace

#define RA(folder, prefix, prio, limit, additive, replace, mask) \
	rules.push_back({ folder, prefix, static_cast<std::uint8_t>(prio), Layer::Ambient, additive, replace, (mask), -1, static_cast<std::uint8_t>(limit) })
#define RC(folder, prefix, prio, limit, additive, mask) \
	rules.push_back({ folder, prefix, static_cast<std::uint8_t>(prio), Layer::Combat, additive, false, (mask), -1, static_cast<std::uint8_t>(limit) })

void RuleTable::Build()
{
	rules.clear();
	rules.reserve(72);

	// ---- outdoors: biomes anchor, hour / all-area layers are additive ------
	RA("Explore - Forest Fall", "ExploreForestFall", 10, 20, false, false, Exterior | ForestFall);
	RA("Explore - Forest Pine", "ExploreForestPine", 10, 20, false, false, Exterior | ForestPine);
	RA("Explore - Snow", "ExploreSnow", 10, 20, false, false, Exterior | Snow);
	RA("Explore - Tundra", "ExploreTundra", 10, 20, false, false, Exterior | Tundra);
	RA("Explore - Mountain", "ExploreMountain", 10, 20, false, false, Exterior | Mountain);
	RA("Explore - All Areas", "ExploreAllAreas", 5, 20, true, false, B(Exterior));
	RA("Explore - All Hours", "ExploreAllHours", 6, 20, true, false, B(Exterior));
	RA("Explore - Morning", "ExploreMorning", 8, 20, true, false, Exterior | HourMorning);
	RA("Explore - Day", "ExploreDay", 8, 30, true, false, Exterior | HourDay);
	RA("Explore - Evening", "ExploreEvening", 8, 20, true, false, Exterior | HourEvening);
	RA("Explore - Night", "ExploreNight", 8, 30, true, false, Exterior | HourNight);
	RA("Explore - Forest", "ExploreForest", 9, 20, false, false, Exterior | ForestFall);
	RA("Explore - Coastal", "ExploreCoastal", 9, 20, false, false, Exterior | Coastal);
	RA("Explore - Volcanic", "ExploreVolcanic", 9, 20, false, false, Exterior | Volcanic);
	RA("Explore - Marsh", "ExploreMarsh", 9, 20, false, false, Exterior | Marsh);
	RA("Soul Cairn", "SoulCairn", 12, 20, false, false, B(SoulCairn));
	RA("Falmer Valley", "FalmerValley", 12, 30, false, false, B(FalmerValley));
	RA("DB Explore - General", "DBExploreGeneral", 11, 30, false, false, B(DBArea));
	RA("DB Explore - Mountain", "DBExploreMountain", 12, 30, false, false, DBArea | Mountain);
	RA("DB Explore - Snow", "DBExploreSnow", 12, 30, false, false, DBArea | Snow);

	// ---- mounted: replaces the location pool when it has tracks -----------
	RA("Ride - General", "RideGeneral", 16, 20, false, true, B(Riding));

	// ---- dungeons ---------------------------------------------------------
	RA("Dungeon", "Dungeon", 20, 40, false, false, B(Dungeon));
	RA("Dungeon - All", "DungeonAll", 18, 20, true, false, B(Dungeon));
	RA("Dungeon - Cave", "DungeonCave", 21, 40, false, false, B(Cave));
	RA("Dungeon - Fort", "DungeonFort", 21, 30, false, false, B(Fort));
	RA("Dungeon - Ice", "DungeonIce", 21, 30, false, false, B(Ice));
	RA("Dungeon - Crypt", "DungeonCrypt", 21, 30, false, false, B(Crypt));
	RA("Dungeon - Mine", "DungeonMine", 21, 30, false, false, B(Mine));
	RA("Dungeon - Warrens", "DungeonWarrens", 21, 30, false, false, B(Warrens));
	RA("Dungeon - Ruin", "DungeonRuin", 21, 30, false, false, B(Ruin));
	RA("DB Dungeon - Apocrypha", "DBDungeonApocrypha", 22, 30, false, false, B(DBApocrypha));
	RA("Vampire Castle", "VampireCastle", 22, 40, false, false, Castle | Dungeon);

	// ---- combat -----------------------------------------------------------
	RC("Combat", "Combat", 30, 40, false, B(InCombat));
	RC("Combat - All", "CombatAll", 28, 20, true, B(InCombat));
	RC("Combat - Dragon", "CombatBDragon", 34, 20, false, InCombat | EnemyDragon);
	RC("Combat - Civil War", "CombatCivilWar", 34, 20, false, InCombat | CivilWar);
	RC("Combat - Vampire Lord", "CombatVampireLord", 40, 10, false, InCombat | EnemyVampireLord);
	RC("DB Combat - Miraak", "DBCombatMiraak", 40, 20, false, InCombat | EnemyMiraak);
	RC("DB Combat - Karstaag", "DBCombatKarstaag", 40, 10, false, InCombat | EnemyKarstaag);
	RC("Combat - Undead", "CombatUndead", 32, 20, false, InCombat | EnemyUndead);
	RC("Combat - Dragon Priest", "CombatDragonPriest", 36, 10, false, InCombat | EnemyDragonPriest);
	RC("Combat - Werewolf", "CombatWerewolf", 36, 10, false, InCombat | EnemyWerewolf);
	RC("Combat - Daedra", "CombatDaedra", 32, 20, false, InCombat | EnemyDaedra);
	RC("Combat - Bandit", "CombatBandit", 32, 20, false, InCombat | EnemyBandit);
	RC("Combat - Falmer", "CombatFalmer", 32, 20, false, InCombat | EnemyFalmer);
	RC("Combat - Boss", "CombatBoss", 33, 10, false, InCombat | BossGeneric);

	// ---- towns ------------------------------------------------------------
	RA("Town - Day", "TownDay", 22, 30, false, false, Town | TownDay);
	RA("Town - Night", "TownNight", 22, 30, false, false, Town | TownNight);
	RA("Town - Whiterun", "TownWhiterun", 24, 20, false, false, Town | CityWhiterun);
	RA("Town - Markarth", "TownMarkarth", 24, 20, false, false, Town | CityMarkarth);
	RA("Town - Riften", "TownRiften", 24, 20, false, false, Town | CityRiften);
	RA("Town - Solitude", "TownSolitude", 24, 20, false, false, Town | CitySolitude);
	RA("Town - Windhelm", "TownWindhelm", 24, 20, false, false, Town | CityWindhelm);

	// ---- interiors --------------------------------------------------------
	RA("Tavern", "Tavern", 19, 20, false, false, B(Tavern));
	RA("Tavern - B", "TavernB", 18, 20, true, false, B(Tavern));
	RA("Tavern - All", "TavernAll", 18, 20, true, false, B(Tavern));
	RA("Castle", "Castle", 20, 20, false, false, B(Castle));
	RA("College", "College", 20, 20, false, false, B(College));
	RA("Temple", "Temple", 19, 20, false, false, B(Temple));
	RA("Dwemer", "Dwemer", 21, 40, false, false, B(Dwemer));
	RA("Blackreach", "Blackreach", 22, 20, false, false, B(Blackreach));
	RA("Museum", "Museum", 17, 20, false, false, B(Museum));
	RA("Library", "Library", 17, 20, false, false, B(TagLibrary));

	// ---- player homes -----------------------------------------------------
	RA("Player Homes/Breezehome", "Breezehome", 25, 20, false, false, B(HomeBreezehome));
	RA("Player Homes/Vlindrel Hall", "VlindrelHall", 25, 20, false, false, B(HomeVlindrel));
	RA("Player Homes/Hjerim", "Hjerim", 25, 20, false, false, B(HomeHjerim));
	RA("Player Homes/Proudspire Manor", "ProudspireManor", 25, 20, false, false, B(HomeProudspire));
	RA("Player Homes/Honeyside", "Honeyside", 25, 20, false, false, B(HomeHoneyside));
	RA("Player Homes/Player Homes - All", "PlayerHomesAll", 24, 30, true, false, B(HomeAny));

	for (auto& r : rules) r.group = GroupOf(r.folder);
}
#undef RA
#undef RC

void RuleTable::Bind(Library& a_lib)
{
	for (auto& r : rules) {
		int idx = a_lib.IndexOf(r.folder);
		if (idx < 0) {
			Folder f;
			f.name = std::string(r.folder);
			f.prefix = std::string(r.prefix);
			f.limit = r.limit ? r.limit : 20;
			a_lib.folders.push_back(std::move(f));
			idx = static_cast<int>(a_lib.folders.size()) - 1;
		}
		r.folderIndex = idx;
	}
}

void Resolve(const RuleTable& a_rules, const Library& a_lib, std::uint64_t a_bits, Layer a_layer, Resolution& out)
{
	out.anchor = -1;
	out.folders.clear();
	out.hash = 0;
	out.label.clear();

	int best = -1;
	int bestPrio = -1;

	static thread_local std::vector<int> additive;
	additive.clear();

	for (int i = 0; i < static_cast<int>(a_rules.rules.size()); ++i) {
		const Rule& r = a_rules.rules[static_cast<std::size_t>(i)];
		if (r.layer != a_layer) continue;
		if (r.mask == 0) continue;
		if ((a_bits & r.mask) != r.mask) continue;

		// a "replace" rule with an empty folder is meaningless: skip it so the
		// location music keeps playing instead of falling silent
		if (r.replace) {
			const Folder* f = (r.folderIndex >= 0 && r.folderIndex < static_cast<int>(a_lib.folders.size()))
				? &a_lib.folders[static_cast<std::size_t>(r.folderIndex)]
				: nullptr;
			if (!f || f->tracks.empty()) continue;
		}

		if (r.additive) {
			additive.push_back(i);
		} else if (r.priority > bestPrio) {
			bestPrio = r.priority;
			best = i;
		}
	}

	// Degradation: nothing exclusive matched (the usual case outdoors, because a
	// vanilla cell carries no biome keyword).  Promote the strongest additive
	// rule so the layer still plays something instead of falling silent.
	int anchorIdx = best;
	if (anchorIdx < 0 && !additive.empty()) {
		anchorIdx = *std::max_element(additive.begin(), additive.end(), [&](int a, int b) {
			return a_rules.rules[static_cast<std::size_t>(a)].priority < a_rules.rules[static_cast<std::size_t>(b)].priority;
		});
		additive.erase(std::find(additive.begin(), additive.end(), anchorIdx));
	}

	out.anchor = anchorIdx;
	if (anchorIdx < 0) return;

	const Rule& anchorRule = a_rules.rules[static_cast<std::size_t>(anchorIdx)];
	const Group  group = anchorRule.group;
	const Folder* anchorFolder = (anchorRule.folderIndex >= 0 && anchorRule.folderIndex < static_cast<int>(a_lib.folders.size()))
		? &a_lib.folders[static_cast<std::size_t>(anchorRule.folderIndex)]
		: nullptr;

	out.label = std::string(anchorRule.folder);

	// exclusive replacement (Ride): only when the folder really holds tracks,
	// otherwise fall through to the merged pool so we never end up silent
	if (anchorRule.replace && anchorFolder && !anchorFolder->tracks.empty()) {
		out.folders.push_back(anchorRule.folderIndex);
	}
	else {
		if (anchorRule.folderIndex >= 0) out.folders.push_back(anchorRule.folderIndex);

		std::sort(additive.begin(), additive.end(), [&](int a, int b) {
			return a_rules.rules[static_cast<std::size_t>(a)].priority > a_rules.rules[static_cast<std::size_t>(b)].priority;
		});

		for (int ai : additive) {
			const Rule& ar = a_rules.rules[static_cast<std::size_t>(ai)];
			if (ar.replace || ar.group != group) continue;
			if (ar.folderIndex < 0) continue;
			if (std::find(out.folders.begin(), out.folders.end(), ar.folderIndex) != out.folders.end()) continue;
			out.folders.push_back(ar.folderIndex);
			out.label += '|';
			out.label += ar.folder;
		}
	}

	// Pool identity: which folders contribute and how many tracks each one has.
	// Unchanged hash means "same pool" -> the director keeps the current track.
	for (int fi : out.folders) {
		if (fi < 0 || fi >= static_cast<int>(a_lib.folders.size())) continue;
		const auto& f = a_lib.folders[static_cast<std::size_t>(fi)];
		out.hash ^= (static_cast<std::uint64_t>(fi) << 40) ^ static_cast<std::uint64_t>(f.tracks.size());
		out.hash = out.hash * 1099511628211ull + 0x9E3779B97F4A7C15ull;
	}
	if (out.hash == 0) out.hash = 1;
}

}  // namespace cms
