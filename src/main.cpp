#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>
#include <SKSE/Logger.h>   // SKSE::log::log_directory()

#include <Windows.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <string>
#include <thread>

#include "audio.h"
#include "core.h"
#include "director.h"
#include "i18n.h"
#include "menu.h"
#include "platform.h"
#include "volume.h"

namespace fs = std::filesystem;

namespace {

using namespace cms;

Config    g_cfg;
RuleTable g_rules;
Library   g_lib;
AudioSink g_sink;
VolumeMix g_vol;
Director  g_dir;
std::string g_mixerPath;

std::thread       g_engine;
std::atomic<bool> g_run{ false };
std::atomic<bool> g_snapPending{ false };
std::atomic<bool> g_started{ false };

// Seqlock: the game thread publishes a GameContext, the engine thread reads it.
// GameContext is trivially copyable and tiny, so a torn read is cheap to retry.
std::atomic<std::uint64_t> g_ctxSeq{ 0 };
GameContext                g_ctxShared;
GameContext                g_ctxStage;

std::atomic<bool> g_vanillaYield{ false };  // engine thread's decision, applied on the game thread

double Now()
{
	using clock = std::chrono::steady_clock;
	static const auto t0 = clock::now();
	return std::chrono::duration<double>(clock::now() - t0).count();
}

void PublishContext(const GameContext& a_ctx)
{
	g_ctxSeq.fetch_add(1, std::memory_order_acq_rel);  // odd: writing
	g_ctxShared = a_ctx;
	g_ctxSeq.fetch_add(1, std::memory_order_acq_rel);  // even: stable
}

GameContext ReadContext()
{
	for (int attempt = 0; attempt < 50; ++attempt) {
		const auto s1 = g_ctxSeq.load(std::memory_order_acquire);
		if (s1 & 1) continue;
		const GameContext copy = g_ctxShared;
		const auto       s2 = g_ctxSeq.load(std::memory_order_acquire);
		if (s1 == s2) return copy;
	}
	return {};
}

// Runs on the game's main thread.  All RE memory access happens here.
void SnapshotTask()
{
	const auto ctx = platform::Snapshot();
	PublishContext(ctx);

	// Vanilla handover.  The *decision* belongs to the engine thread (it owns the
	// resolution: does this plugin have a track for the event/location?), this
	// thread only performs the write, because a sound category is game state.
	//
	// Nothing is written while the world is loading: the engine rebuilds its
	// audio system there and restores the category levels with it, which is the
	// one write this plugin must not do mid-load.  The next tick after the world
	// is ready re-applies the current decision (SteerVanillaMusic heals the level
	// by itself, so nothing has to be "re-asserted" on a timer).
	const bool busy = ctx.loading || ctx.mainMenu || !ctx.attached;
	// One settle before the first write after a load: the decision published by
	// the engine thread is at most one tick old, and right after a load it can
	// still be the "hand back" from the main menu.  Applying that would let the
	// vanilla layer through for a frame and cut it again immediately - a click at
	// every load.  Half a second is well before the first note (fWorldSettle) and
	// long enough for the engine to have re-decided.
	static double readyAt = -1.0;
	if (busy) {
		readyAt = -1.0;
	} else if (readyAt < 0.0) {
		readyAt = Now();
	} else if (Now() - readyAt > 0.5) {
		// Called even when "keep vanilla" is on: flipping that switch has to give the
		// level back immediately, and Steer(false) only writes when we were the ones
		// holding it.
		platform::SteerVanillaMusic(!g_cfg.keepVanilla && g_vanillaYield.load(std::memory_order_relaxed));
	}
	g_snapPending.store(false, std::memory_order_release);
}

void EngineLoop()
{
	const auto idle = std::chrono::milliseconds(std::max(10, g_cfg.tickMs));

	std::uint64_t lastSeq = g_ctxSeq.load(std::memory_order_relaxed);
	double        lastSeqChange = Now();
	double        dispatchT = 0.0;
	double        lastStallLog = 0.0;
	bool          stalled = false;

	while (g_run.load(std::memory_order_acquire)) {
		// If the task queue ever stops draining, hold the last snapshot instead of
		// reading it from here.  This thread is NOT the game thread: touching
		// RE::* from it (player, cells, ProcessLists, sound categories) is
		// undefined behaviour, and during a save load - exactly when the queue
		// stalls - the engine is busy tearing the old world down, so a cross
		// thread read there is the worst possible timing.  The music simply keeps
		// playing from the last context for those few seconds; the gate closes on
		// its own once the load flag arrives.
		const auto seq = g_ctxSeq.load(std::memory_order_relaxed);
		if (seq != lastSeq) {
			lastSeq = seq;
			lastSeqChange = Now();
			stalled = false;
		} else if (!stalled && Now() - lastSeqChange > 3.0) {
			stalled = true;
			LOG_W("snapshot task stalled for %.0fs -> holding the last context (no game access off-thread)",
				Now() - lastSeqChange);
		}

		if (auto* tasks = SKSE::GetTaskInterface()) {
			if (!g_snapPending.exchange(true, std::memory_order_acq_rel)) {
				dispatchT = Now();
				tasks->AddTask([]() { SnapshotTask(); });
			} else if (Now() - dispatchT > 5.0) {
				// The queued task has not run for 5 s.  Do NOT drop the flag and
				// queue another one: during a long load screen the tasks pile up
				// and then all execute in the same frame (N snapshots, N scans).
				// Holding one pending task is the safe state - SnapshotTask clears
				// the flag itself when it finally runs.
				if (Now() - lastStallLog > 10.0) {
					lastStallLog = Now();
					LOG_W("snapshot task queued for %.0fs (game thread busy) - music holds the last context",
						Now() - dispatchT);
				}
			}
		}

		const GameContext ctx = ReadContext();
		g_dir.Update(Now(), ctx);

		// The takeover is a handover, so the vanilla layer is only held back while
		// this plugin actually has something for the current event/location (or is
		// still fading a track out there).  The moment the resolution comes back
		// empty the vanilla layer gets the context back, which is the fallback for
		// every place the library does not cover.
		g_vanillaYield.store(!g_cfg.keepVanilla && g_dir.Covers(), std::memory_order_relaxed);

		// pick up music dropped into folders that were empty at startup; the
		// page draws from its own copy, so it has to be republished as well
		const double now = Now();
		static double lastScan = 0.0;
		if (now - lastScan > 60.0) {
			lastScan = now;
			if (g_lib.Scan(false)) {
				menu::PublishLibrary(g_lib);
				LOG_D("library: warm rescan done");
			}
		}

		// the page only marks the mixer dirty; the file is written here so a
		// slider drag cannot stall the render thread on disk I/O
		static double lastSave = 0.0;
		if (g_cfg.mixerAutosave && g_vol.Dirty() && now - lastSave > 1.0) {
			lastSave = now;
			if (g_vol.Save(g_mixerPath)) LOG_D("mixer: autosaved %s", g_mixerPath.c_str());
		}

		std::this_thread::sleep_for(idle);
	}
}

// ------------------------------------------------------------ CRT forensics --
// A 0xC0000409 ("fast fail") crash - _invoke_watson from the UCRT's
// invalid-parameter handler, or abort() - is invisible to CrashLoggerSSE: the
// fail-fast path never raises a catchable exception, so no crash log is written
// and the dump has no stack to walk.  Both were seen while a save was loading.
//
// Installing our own invalid-parameter handler does NOT change the outcome (we
// log, then call the previous handler, which still terminates the process) - it
// only writes the expression / function / file / line that tripped the CRT into
// our log, so the *next* occurrence names the offending call instead of leaving
// an empty dossier.
//
// 2.4.3: ucrtbase is shared by every plugin in the process, so "invalid
// parameter" on its own proves nothing about who called it.  Most callers pass
// the "no info" variant, which yields expression='(null)' - useless.  So the
// handler now also records the *caller chain* (module + offset per frame,
// one line).  That turns the log line into an accusation.  The handler stays
// allocation-free apart from the log write, is recursion-guarded, and forwards
// to the previous handler unchanged.
namespace {
using InvalidParamHandler = void (*)(const wchar_t*, const wchar_t*, const wchar_t*, unsigned int, uintptr_t);
using CaptureBackTrace = USHORT (*)(ULONG, ULONG, void**, ULONG*);
InvalidParamHandler g_prevInvalidParam = nullptr;
bool                g_crtHooksInstalled = false;

void ToUtf8(const wchar_t* a_w, char* a_dst, size_t a_size)
{
	if (!a_w || a_size == 0) return;
	const int n = ::WideCharToMultiByte(CP_UTF8, 0, a_w, -1, a_dst, static_cast<int>(a_size) - 1, nullptr, nullptr);
	a_dst[(n > 0) ? n - 1 : 0] = '\0';
}

void OnInvalidParameter(const wchar_t* a_expression, const wchar_t* a_function, const wchar_t* a_file, unsigned int a_line,
    uintptr_t a_reserved)
{
	// a CRT error raised *by* our logging must not re-enter the logger
	static thread_local bool inHandler = false;
	if (inHandler) return;
	inHandler = true;

	// separate buffers: Narrow()-style shared statics would alias inside one
	// printf, and the values are all-null in the common "no info" case
	char expr[160] = "(null)";
	char func[128] = "(null)";
	char file[192] = "(null)";
	ToUtf8(a_expression, expr, sizeof(expr));
	ToUtf8(a_function, func, sizeof(func));
	ToUtf8(a_file, file, sizeof(file));

	LOG_E("CRT invalid parameter: expression='%s' function='%s' %s:%u tid=%lu", expr, func, file, a_line,
		::GetCurrentThreadId());

	void* frames[10] = {};
	USHORT n = 0;
	if (auto* k32 = ::GetModuleHandleW(L"kernel32.dll")) {
		if (auto capture = reinterpret_cast<CaptureBackTrace>(reinterpret_cast<void*>(::GetProcAddress(k32, "RtlCaptureStackBackTrace")))) {
			n = capture(1, 10, frames, nullptr);
		}
	}

	char chain[1200] = {};
	size_t at = 0;
	for (USHORT i = 0; i < n && at + 96 < sizeof(chain); ++i) {
		HMODULE mod = nullptr;
		if (!::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
				reinterpret_cast<LPCWSTR>(frames[i]), &mod) ||
			!mod) {
			continue;
		}
		wchar_t path[MAX_PATH] = {};
		::GetModuleFileNameW(mod, path, MAX_PATH);
		const wchar_t* base = path;
		for (const wchar_t* p = path; *p; ++p) {
			if (*p == L'\\') base = p + 1;
		}
		char name[MAX_PATH] = {};
		ToUtf8(base, name, sizeof(name));
		const auto off = reinterpret_cast<std::uintptr_t>(frames[i]) - reinterpret_cast<std::uintptr_t>(mod);
		at += static_cast<size_t>(std::snprintf(chain + at, sizeof(chain) - at, "%s%s+0x%llx", at ? " <- " : "", name,
			static_cast<unsigned long long>(off)));
	}
	LOG_E("  caller chain: %s", chain[0] ? chain : "(unavailable)");
	LogFlush();

	inHandler = false;
	if (g_prevInvalidParam) g_prevInvalidParam(a_expression, a_function, a_file, a_line, a_reserved);
}

void InstallCrtDiagnostics()
{
	if (g_crtHooksInstalled) return;
	g_crtHooksInstalled = true;
	g_prevInvalidParam = _set_invalid_parameter_handler(&OnInvalidParameter);
	std::set_terminate([]() {
		LOG_E("std::terminate reached (unhandled C++ exception or noexcept violation); abort follows");
		LogFlush();
		std::abort();
	});
}

}  // namespace

void StartAll()
{
	if (g_started.exchange(true)) return;

	g_cfg.dataDir = platform::DataDir();
	g_cfg.pluginDir = platform::PluginDir();

	// Put the log next to SKSE's own logs (My Games\Skyrim Special Edition\SKSE),
	// never in the game root.  The directory is resolved per-machine, so no
	// absolute path is hard-coded here.
	fs::path logDir = g_cfg.dataDir.empty() ? fs::path() : fs::path(g_cfg.dataDir).parent_path();
	if (auto skseLog = SKSE::log::log_directory()) logDir = *skseLog;
	const std::string logPath = (logDir / "CustomMusicSystem.log").string();
	LogOpen(logPath);

	LOG_I("CustomMusicSystem 2.6.6  (dynamic music director)");
	LOG_I("data dir:   %s", g_cfg.dataDir.c_str());
	LOG_I("plugin dir: %s", g_cfg.pluginDir.c_str());
	LOG_I("log file:   %s", logPath.c_str());

	InstallCrtDiagnostics();

	const std::string ini = (fs::path(g_cfg.pluginDir) / "CustomMusicSystem.ini").string();
	if (g_cfg.LoadFromFile(ini)) {
		LOG_I("config loaded: %s", ini.c_str());
	} else {
		const std::string alt = (fs::path(g_cfg.dataDir) / "CustomMusicSystem.ini").string();
		if (g_cfg.LoadFromFile(alt)) LOG_I("config loaded: %s", alt.c_str());
		else LOG_W("config not found (%s), using built-in defaults", ini.c_str());
	}

	SetVerbose(g_cfg.verbose);
	if (g_cfg.verbose) LOG_I("verbose logging enabled (bVerbose = 1)");

	g_lib.root = (fs::path(g_cfg.dataDir) / kLibraryFolder).string();
	g_rules.Build();
	g_rules.Bind(g_lib);

	bool changed = false;
	g_lib.Scan(true, &changed);

	// --- language ---------------------------------------------------------
	// Fixed at build time: this DLL is either the English or the Chinese build,
	// so there is nothing to detect, load or switch.  The log line still reports
	// what the game thinks its language is, which is the first thing to check if
	// a player complains that the menu is "the wrong language".
	const std::string gameLang = platform::DetectLanguage();
	i18n::Init(g_cfg.dataDir, gameLang, g_cfg.uiLanguage);

	// --- volume -----------------------------------------------------------
	g_mixerPath = (fs::path(g_cfg.pluginDir) / "CustomMusicSystem.mixer.txt").string();
	if (g_vol.Load(g_mixerPath)) {
		LOG_I("%s %s", i18n::T(i18n::S::LogMixerLoaded), g_mixerPath.c_str());
		if (g_vol.LegacyRows() > 0) {
			// The per-folder / per-track levels have no UI any more; leaving them
			// in the file would keep quietening tracks the player cannot see.
			LOG_D("volume: dropping %zu old per-folder/per-track rows (one master level now)", g_vol.LegacyRows());
			if (g_vol.Save(g_mixerPath)) LOG_D("volume: rewrote %s in the one-level format", g_mixerPath.c_str());
		}
	} else {
		LOG_I("%s %s (master level starts at 100 %%)", i18n::T(i18n::S::LogMixerMissing), g_mixerPath.c_str());
	}

	std::size_t foldersWithTracks = 0;
	std::size_t totalTracks = 0;
	std::size_t mixed = 0;
	for (const auto& f : g_lib.folders) {
		if (!f.tracks.empty()) ++foldersWithTracks;
		totalTracks += f.tracks.size();
		if (f.mixedFormat) ++mixed;
	}
	LOG_I("music root: %s", g_lib.root.c_str());
	LOG_I("rules: built-in, %zu entries; library: %zu folders (%zu with tracks), %zu tracks, %.1f MB",
		g_rules.rules.size(), g_lib.folders.size(), foldersWithTracks, totalTracks,
		static_cast<double>(g_lib.TotalBytes()) / (1024.0 * 1024.0));

	if (g_cfg.singleFormatCheck && mixed > 0) {
		for (const auto& f : g_lib.folders) {
			if (f.mixedFormat) LOG_D("folder mixes .xwm and .wav: %s", f.name.c_str());
		}
	}
	if (totalTracks == 0) LOG_W("library is empty - drop .xwm files into %s\\<event folder> to get music", g_lib.root.c_str());

	std::string err;
	if (!g_sink.Init(&err)) LOG_E("audio: %s", err.c_str());

	g_dir.Init(g_cfg, g_rules, g_lib, g_sink, &g_vol);

	// combat events: engages + hits, on top of Actor::IsInCombat()
	if (!platform::InstallCombatSensor()) LOG_W("combat sensor: event sources not ready (IsInCombat only)");

	// in-game pages (F1 -> category -> Volume / Settings); missing framework is fine
	menu::Register(g_cfg, g_lib, g_vol, g_dir, g_mixerPath, g_cfg.iniPath);

	if (g_cfg.keepVanilla) {
		LOG_I("vanilla music: kept - it plays alongside this plugin (bKeepVanilla = 1)");
	} else {
		LOG_I("vanilla music: takeover mode (bKeepVanilla = 0) - held back where this plugin has a track, handed back where it has none");
	}

	// publish one snapshot from the main thread so the gate has data immediately
	SnapshotTask();
	g_snapPending.store(false, std::memory_order_release);

	g_run.store(true, std::memory_order_release);
	g_engine = std::thread(EngineLoop);
	LOG_I("engine started (%d ms tick)", std::max(10, g_cfg.tickMs));
}

void StopAll()
{
	if (!g_started.exchange(false)) return;

	g_run.store(false, std::memory_order_release);
	if (g_engine.joinable()) g_engine.join();

	menu::Unregister();
	platform::RemoveCombatSensor();
	// Give the vanilla layer its level back on the way out, so a disabled plugin
	// never leaves the game without music.
	platform::SteerVanillaMusic(false);
	g_sink.Shutdown();
	LOG_I("engine stopped");
	LogClose();
}

}  // namespace

SKSEPluginLoad(const SKSE::LoadInterface* a_skse)
{
	SKSE::Init(a_skse);

	if (auto* messaging = SKSE::GetMessagingInterface()) {
		messaging->RegisterListener([](SKSE::MessagingInterface::Message* a_msg) {
			if (!a_msg) return;
			if (a_msg->type == SKSE::MessagingInterface::kDataLoaded) StartAll();
		});
	}
	return true;
}

SKSEPluginInfo(
	.Version = { 2, 6, 6, 0 },
	.Name = "CustomMusicSystem",
	.Author = "Icarus",
	.SupportEmail = "",
	.StructCompatibility = SKSE::StructCompatibility::Independent,
	.RuntimeCompatibility = SKSE::PluginDeclaration::RuntimeCompatibility(SKSE::VersionIndependence::AddressLibrary));
