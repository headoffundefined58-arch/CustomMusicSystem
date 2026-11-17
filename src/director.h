// The scheduler: world gate, layer resolution, pool selection, fades.
// Talks to the game only through a GameContext snapshot, so it is fully
// testable off-line (see tests/tests.cpp).
#pragma once

#include <atomic>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

#include "core.h"

namespace cms {

class AudioSink;
class VolumeMix;

class Director {
public:
	// Everything the in-game page needs, copied under a lock once per tick so
	// the UI never touches the live slots.
	struct Status {
		bool        gateOpen = false;
		bool        combat = false;
		float       master = 1.0f;
		std::string ambientLabel;
		std::string ambientTrack;
		std::string combatLabel;
		std::string combatTrack;
		float       ambientGain = 0.0f;
		float       combatGain = 0.0f;
		int         combatReason = 0;  // EngageReason (0 none / 1 duel / 2 ambush / 3 sustained)
		// True while this plugin owns the event/location, i.e. while the vanilla
		// layer is held back.  False means the vanilla music is playing again
		// (fallback) - the page shows which of the two is in charge.
		bool        vanillaTakenOver = false;
	};

	void Init(const Config& a_cfg, RuleTable& a_rules, Library& a_lib, AudioSink& a_sink, VolumeMix* a_mix);

	// engine thread, ~20 Hz
	void Update(double a_now, const GameContext& a_ctx);

	// Engine thread, after Update().  True when this plugin has music of its own
	// for the current event/location, which is what makes the vanilla layer stand
	// down; false means "hand the context back to vanilla".
	[[nodiscard]] bool Covers() const;

	[[nodiscard]] bool        GateOpen() const { return gateOpen_; }
	[[nodiscard]] float       MasterGain() const;
	[[nodiscard]] const std::string& AmbientLabel() const { return amb_.label; }
	[[nodiscard]] Status      GetStatus() const;

private:
	struct Slot {
		std::vector<std::string> pool;
		std::uint64_t            hash = 0;
		std::string              label;
		std::string              playing;
		int                      index = -1;
	};

	// Per-layer switch guard.  "The context changed" is not the same thing as
	// "start another track": the resolution has to stay stable, the current
	// track has to have had its say, and nothing may be mid-crossfade.
	struct Arbiter {
		double        started = 0.0;      // start of the clip that is playing
		double        switched = -1.0e9;  // last time this layer changed at all
		double        wantSince = -1.0;   // when the current target first showed up
		double        lastDeferLog = -1.0e9;
		std::uint64_t wantHash = 0;
		bool          wantValid = false;
	};

	void  UpdateGate(double a_now, const GameContext& a_ctx);
	bool  EnsureAmbient(double a_now, const GameContext& a_ctx);  // true = switched this tick
	// a_start = false resolves/caches the combat pool without launching it; the
	// caller uses that to learn whether a fight has music before it stops the
	// track that is currently playing (exclusive handover).
	bool  EnsureCombat(double a_now, bool a_wantCombat, bool a_armed, const GameContext& a_ctx, bool a_start = true);
	void  Advance(int a_slot, double a_now, float a_fade);
	void  StartIndex(int a_slot, int a_index, double a_now, float a_fade);
	void  Clear(Slot& a_slot);
	void  PublishStatus(bool a_combat);
	int   Pick(const Slot& a_slot, int a_avoid) const;

	// Every reason a layer must *not* change yet, in one place.
	bool MaySwitch(Arbiter& a_arb, int a_slot, double a_now, float a_minPlay, const char* a_tag) const;
	void DeferLog(Arbiter& a_arb, double a_now, const char* a_tag, const char* a_why) const;

	// True when the clip on this slot is close enough to its end to start the
	// next one underneath it (a real crossfade instead of "end, gap, attack").
	[[nodiscard]] bool NearEnd(int a_slot, float a_fade) const;

	const Config* cfg_ = nullptr;
	RuleTable*    rules_ = nullptr;
	Library*      lib_ = nullptr;
	AudioSink*    sink_ = nullptr;
	VolumeMix*    mix_ = nullptr;

	Slot amb_;
	Slot cmb_;
	Resolution cmbRes_;  // cached: combat ends -> context loses InCombat, pool must survive
	GameContext combatCtx_{};

	std::deque<std::string> recent_;


	mutable std::mutex statusMtx_;
	Status             status_{};

	bool   gateOpen_ = false;
	bool   gateLatched_ = false;
	double readyT_ = 0.0;       // first "world looks ready" moment
	double inWorldT_ = 0.0;     // first "in world, not loading" moment (force-open fallback)
	double lostT_ = 0.0;
	double lastCombat_ = 0.0;
	double dialogueT0_ = 0.0;
	bool   combatResolved_ = false;
	int    combatReason_ = 0;
	std::uint64_t lastCtxFp_ = 0;

	// vanilla handover (see Director::Covers)
	bool   worldBusy_ = true;   // loading / main menu / cell not attached
	bool   mainMenu_ = false;
	bool   underwater_ = false; // player is below the surface: no music at all (see Covers)
	bool   imrTakeover_ = false; // IMR radio is on the air: local layer stands down
	mutable bool covered_ = false;  // last decision, held while the world is busy

	Arbiter  ambArb_;
	Arbiter  cmbArb_;
	double   combatEndSeen_ = 0.0;    // last GameContext::combatEndAt we acted on
	double   combatArmAt_ = -1.0;     // first tick a confirmed fight was seen (arm-time clock)
	unsigned switchedMask_ = 0;       // bit per slot, set by StartIndex, cleared each tick

	// path we already handed over from, so the near-end check fires once
	mutable std::string ambHandover_;
	mutable std::string cmbHandover_;
};

}  // namespace cms
