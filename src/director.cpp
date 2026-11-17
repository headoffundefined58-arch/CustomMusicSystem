#include "director.h"

#include <algorithm>
#include <cmath>
#include <random>

#include "audio.h"
#include "volume.h"

namespace cms {

void Director::Init(const Config& a_cfg, RuleTable& a_rules, Library& a_lib, AudioSink& a_sink, VolumeMix* a_mix)
{
	cfg_ = &a_cfg;
	rules_ = &a_rules;
	lib_ = &a_lib;
	sink_ = &a_sink;
	mix_ = a_mix;
	amb_.pool.reserve(256);
	cmb_.pool.reserve(128);
}

float Director::MasterGain() const
{
	// One level, owned by the volume page.  The game's own Music slider is not
	// part of this: the vanilla layer is steered separately (see
	// platform::SteerVanillaMusic), so mixing the two here would scale our music
	// twice and make the page's percentage a lie.
	const float m = mix_ ? mix_->MasterEffective() : 1.0f;
	return std::clamp(m, 0.0f, 4.0f);
}

Director::Status Director::GetStatus() const
{
	std::lock_guard lock(statusMtx_);
	return status_;
}

void Director::PublishStatus(bool a_combat)
{
	Status st;
	st.gateOpen = gateOpen_;
	st.combat = a_combat;
	st.master = MasterGain();
	st.ambientLabel = amb_.label;
	st.ambientTrack = amb_.playing;
	st.combatLabel = cmb_.label;
	st.combatTrack = cmb_.playing;
	st.ambientGain = sink_ ? sink_->CurrentGain(0) : 0.0f;
	st.combatGain = sink_ ? sink_->CurrentGain(1) : 0.0f;
	st.combatReason = combatReason_;
	st.vanillaTakenOver = covered_;

	// only the file name matters in the UI; the full path eats the whole row
	auto leaf = [](std::string& a_path) {
		const auto cut = a_path.find_last_of("/\\");
		if (cut != std::string::npos) a_path.erase(0, cut + 1);
	};
	leaf(st.ambientTrack);
	leaf(st.combatTrack);

	std::lock_guard lock(statusMtx_);
	status_ = std::move(st);
}

bool Director::Covers() const
{
	// The main menu is not an event and not a location: nothing of ours can cover
	// it, so the vanilla layer owns the menu music.
	if (mainMenu_) {
		covered_ = false;
		return false;
	}

	// A load screen, a door to a world that is not attached yet, the main menu -
	// none of those have a resolution to look at.  Hold the last answer instead
	// of re-deriving one from an empty world: releasing the vanilla layer every
	// time a load screen comes up, and taking it back a second later when the new
	// pool binds, would be an audible flam at every load and every door.
	if (worldBusy_ || !gateOpen_) return covered_;

	// A bound pool means "we have music for this event/location".  A voice of
	// ours that is still sounding counts too, so the hand-back waits for our last
	// fade instead of dropping vanilla on top of it.  Underwater counts as well:
	// no music at all down there means the vanilla layer is held back too, not
	// handed the context back (see Update).  The IMR takeover counts as well, for
	// the same reason: the radio is playing, so the vanilla layer must stay at 0
	// underneath it instead of being handed the event it would otherwise fill.
	const bool now = underwater_ || imrTakeover_ || !amb_.pool.empty() || !cmb_.pool.empty() ||
		(sink_ && (sink_->IsActive(0) || sink_->IsActive(1)));
	covered_ = now;
	return now;
}

void Director::Clear(Slot& a_slot)
{
	a_slot.pool.clear();
	a_slot.hash = 0;
	a_slot.label.clear();
	a_slot.playing.clear();
	a_slot.index = -1;
}

int Director::Pick(const Slot& a_slot, int a_avoid) const
{
	const int n = static_cast<int>(a_slot.pool.size());
	if (n <= 0) return -1;
	if (n == 1) return 0;

	std::vector<int> cand;
	cand.reserve(static_cast<std::size_t>(n));
	for (int i = 0; i < n; ++i) {
		if (i == a_avoid) continue;
		const auto& p = a_slot.pool[static_cast<std::size_t>(i)];
		if (std::find(recent_.begin(), recent_.end(), p) != recent_.end()) continue;
		cand.push_back(i);
	}
	if (cand.empty()) {
		for (int i = 0; i < n; ++i) {
			if (i != a_avoid) cand.push_back(i);
		}
	}
	if (cand.empty()) return a_avoid >= 0 ? a_avoid : 0;

	static thread_local std::mt19937 rng{ 0xC0FFEEu ^ 0x9E3779B9u };
	std::uniform_int_distribution<int> dist(0, static_cast<int>(cand.size()) - 1);
	return cand[static_cast<std::size_t>(dist(rng))];
}

void Director::StartIndex(int a_slot, int a_index, double a_now, float a_fade)
{
	Slot& s = (a_slot == 0) ? amb_ : cmb_;
	if (a_index < 0 || a_index >= static_cast<int>(s.pool.size())) return;

	const std::string path = s.pool[static_cast<std::size_t>(a_index)];
	s.index = a_index;
	s.playing = path;

	// Book the change *before* asking for it: MaySwitch() reads these, and a
	// track we just launched must never pass its own min-play test.
	Arbiter& arb = (a_slot == 0) ? ambArb_ : cmbArb_;
	(a_slot == 0 ? ambHandover_ : cmbHandover_).clear();
	arb.started = a_now;
	arb.switched = a_now;
	arb.wantSince = -1.0;
	arb.wantValid = false;
	switchedMask_ |= 1u << a_slot;

	sink_->Play(a_slot, path, a_fade);

	recent_.push_back(path);
	const std::size_t keep = static_cast<std::size_t>(std::max(1, cfg_->recentWindow));
	while (recent_.size() > keep) recent_.pop_front();

	LOG_D("play  [layer=%s key='%s' pool=%zu fade=%.1f]  %s", a_slot == 0 ? "ambient" : "combat",
		s.label.c_str(), s.pool.size(), a_fade, path.c_str());
	(void)a_now;

	const int nx = Pick(s, a_index);
	if (nx >= 0 && nx != a_index) sink_->Prefetch(s.pool[static_cast<std::size_t>(nx)]);
}

void Director::Advance(int a_slot, double a_now, float a_fade)
{
	Slot& s = (a_slot == 0) ? amb_ : cmb_;
	if (s.pool.empty()) {
		sink_->Stop(a_slot, a_fade);
		return;
	}
	const int nx = Pick(s, s.index);
	if (nx < 0) return;
	LOG_D("next  %s", s.pool[static_cast<std::size_t>(nx)].c_str());
	StartIndex(a_slot, nx, a_now, a_fade);
}

// The rule set itself lives in core.cpp (SwitchBlockReason) so the tests can
// drive it without an audio device; this is only the wiring.
bool Director::MaySwitch(Arbiter& a_arb, int a_slot, double a_now, float a_minPlay, const char* a_tag) const
{
	const char* why = SwitchBlockReason(a_now, a_arb.started, a_arb.switched, a_arb.wantSince,
		sink_->IsActive(a_slot), sink_->IsFading(a_slot), sink_->DurationSec(a_slot), sink_->PlayheadSec(a_slot),
		a_minPlay, cfg_->switchSettle, cfg_->switchCooldown, cfg_->fadeLocation);
	if (!why) return true;
	DeferLog(a_arb, a_now, a_tag, why);
	return false;
}

void Director::DeferLog(Arbiter& a_arb, double a_now, const char* a_tag, const char* a_why) const
{
	// Once every two seconds per layer is enough to diagnose a stuck switch
	// without turning the log into a wall of "still waiting".
	if (a_now - a_arb.lastDeferLog < 2.0) return;
	a_arb.lastDeferLog = a_now;
	LOG_D("switch %s: deferred (%s)", a_tag, a_why);
}

void Director::UpdateGate(double a_now, const GameContext& c)
{
	if (c.mainMenu) {
		gateOpen_ = false;
		gateLatched_ = false;
		readyT_ = inWorldT_ = lostT_ = 0.0;
		return;
	}

	const bool ready = c.inWorld && c.attached && !c.loading && !c.mainMenu;
	if (ready) {
		lostT_ = 0.0;
		if (readyT_ == 0.0) readyT_ = a_now;
		if (!gateOpen_ && a_now - readyT_ >= cfg_->worldSettle) {
			gateOpen_ = true;
			gateLatched_ = true;
			LOG_D("gate: world ready (cell attached, settle %.1fs) -> music on", cfg_->worldSettle);
		}
	} else {
		readyT_ = 0.0;
	}

	// fallback: never stay silent for the whole session if IsAttached() misbehaves
	if (c.inWorld && !c.loading) {
		if (inWorldT_ == 0.0) inWorldT_ = a_now;
		if (!gateOpen_ && a_now - inWorldT_ >= cfg_->worldSettle + 3.0) {
			gateOpen_ = true;
			gateLatched_ = true;
			LOG_D("gate: forced open after %.1fs (cell never reported attached)", a_now - inWorldT_);
		}
	} else {
		inWorldT_ = 0.0;
	}

	// latch: short dips (opening a door, fast travel) must not restart the music
	if (gateOpen_ && !ready && !c.mainMenu) {
		if (lostT_ == 0.0) lostT_ = a_now;
		if (a_now - lostT_ > 3.0) {
			gateOpen_ = false;
			gateLatched_ = false;
			lostT_ = 0.0;
			LOG_D("gate: closed (no player/cell for 3s)");
		}
	}
}

bool Director::EnsureAmbient(double a_now, const GameContext& ctx)
{
	Resolution r;
	Resolve(*rules_, *lib_, ctx.bits, Layer::Ambient, r);

	// Same resolution as the pool that is on the air: nothing to do, and any
	// pending change away from it is cancelled.  This is the "cell/hour binds to
	// one pool" rule - as long as the hash matches, the track keeps playing even
	// if a rule label changed underneath.
	if (r.hash == amb_.hash && !amb_.pool.empty()) {
		ambArb_.wantSince = -1.0;
		ambArb_.wantHash = r.hash;
		ambArb_.wantValid = true;
		if (amb_.label != r.label) {
			LOG_D("keep  [pool unchanged] '%s' -> '%s'", amb_.label.c_str(), r.label.c_str());
			amb_.label = r.label;
		}
		// Restart only when nothing is left on the slot.  IsActive() also counts
		// a voice that is still fading out, so this cannot launch a new track on
		// top of a running crossfade.
		if (!sink_->IsActive(0)) StartIndex(0, Pick(amb_, -1), a_now, cfg_->fadeLocation);
		return false;
	}

	std::vector<std::string> pool;
	pool.reserve(256);
	for (const auto fi : r.folders) {
		if (fi < 0 || fi >= static_cast<int>(lib_->folders.size())) continue;
		for (const auto& t : lib_->folders[static_cast<std::size_t>(fi)].tracks) {
			if (std::find(pool.begin(), pool.end(), t.path) == pool.end()) pool.push_back(t.path);
		}
	}
	const bool tooSmall = static_cast<int>(pool.size()) < cfg_->minTracks;

	// New target: restart the "has it settled?" clock only when the target
	// actually changed, not on every tick.
	if (!ambArb_.wantValid || ambArb_.wantHash != r.hash) {
		ambArb_.wantHash = r.hash;
		ambArb_.wantValid = true;
		ambArb_.wantSince = a_now;
		LOG_D("want ambient  key='%s'  %zu tracks", r.label.c_str(), pool.size());
	}

	if (!MaySwitch(ambArb_, 0, a_now, cfg_->minPlay, "ambient")) return false;

	if (tooSmall) {
		// Nothing of our own to play here: ramp out over fHandbackFade and let the
		// vanilla layer have the context back.  The fade is what makes the
		// hand-over sound like a piece ending instead of a cut, and it is why the
		// two systems never overlap for more than one fade.
		LOG_D("hand back: pool too small (%zu < %d) key='%s' -> vanilla music", pool.size(), cfg_->minTracks, r.label.c_str());
		sink_->Stop(0, cfg_->fadeHandback);
		Clear(amb_);
		amb_.hash = r.hash;
		amb_.label = r.label;
		ambArb_.switched = a_now;
		ambArb_.wantSince = -1.0;
		return true;
	}

	const bool had = !amb_.pool.empty();
	if (had) {
		LOG_D("switch ambient  fade=%.1f  played %.1fs  '%s' -> '%s'", cfg_->fadeLocation, a_now - ambArb_.started,
			amb_.label.c_str(), r.label.c_str());
	}

	amb_.pool = std::move(pool);
	amb_.hash = r.hash;
	amb_.label = r.label;
	amb_.index = -1;
	StartIndex(0, Pick(amb_, -1), a_now, cfg_->fadeLocation);
	return true;
}

bool Director::EnsureCombat(double a_now, bool a_wantCombat, bool a_armed, const GameContext& a_ctx, bool a_start)
{
	if (!a_wantCombat) {
		// No fight: the combat layer is always released here, even when the last
		// fight had no music at all - otherwise a stale "no combat tracks" cache
		// would keep every later fight silent too.
		if (!cmb_.pool.empty() || sink_->IsPlaying(1) || sink_->IsPending(1)) {
			// The event is over: the combat layer ramps down over
			// fFadeCombatExit and is destroyed when it reaches silence, so the
			// piece ends by itself instead of being cut.
			LOG_D("combat: over -> fading out over %.1fs", cfg_->fadeCombatExit);
			sink_->Stop(1, cfg_->fadeCombatExit);
		}
		Clear(cmb_);
		combatResolved_ = false;
		cmbArb_.wantSince = -1.0;
		cmbArb_.wantValid = false;
		return false;
	}

	if (combatResolved_ && cmb_.pool.empty()) return false;  // already known: no combat tracks
	if (combatResolved_ && !cmb_.pool.empty()) {
		// A fight is on and the pool is known: resume only if nothing is left and
		// the arm time has elapsed, never during the tail of the previous fight's
		// fade or before the fight has proved it is real.
		if (a_armed && a_start && !sink_->IsActive(1)) StartIndex(1, Pick(cmb_, -1), a_now, cfg_->fadeCombatEnter);
		return false;
	}

	const GameContext& c = a_ctx.inCombat ? a_ctx : combatCtx_;
	Resolution r;
	Resolve(*rules_, *lib_, c.bits, Layer::Combat, r);

	if (r.empty()) {
		LOG_D("combat: no combat tracks (bits 0x%016llx)", static_cast<unsigned long long>(c.bits));
		if (sink_->IsActive(1)) sink_->Stop(1, cfg_->fadeCombatExit);
		Clear(cmb_);
		combatResolved_ = true;
		return false;
	}

	std::vector<std::string> pool;
	pool.reserve(128);
	for (const auto fi : r.folders) {
		if (fi < 0 || fi >= static_cast<int>(lib_->folders.size())) continue;
		for (const auto& t : lib_->folders[static_cast<std::size_t>(fi)].tracks) {
			if (std::find(pool.begin(), pool.end(), t.path) == pool.end()) pool.push_back(t.path);
		}
	}

	if (static_cast<int>(pool.size()) < cfg_->minTracks) {
		LOG_D("combat: pool too small (%zu) key='%s'", pool.size(), r.label.c_str());
		if (sink_->IsActive(1)) sink_->Stop(1, cfg_->fadeCombatExit);
		Clear(cmb_);
		cmb_.hash = r.hash;
		cmb_.label = r.label;
		combatResolved_ = true;
		return false;
	}

	// A fight that has proved real still has to be answered at once - waiting
	// fMinPlay here would leave the player in silence during the opening seconds.
	// The one thing we refuse is launching on top of the previous fight's fade-out
	// (the "two combat tracks at once" case).  Before the arm time has elapsed we
	// resolve and cache the pool but do not start: a momentary scuffle that ends
	// within fCombatArm therefore never inserts a track over the running music.
	if (sink_->IsFading(1)) {
		DeferLog(cmbArb_, a_now, "combat", "crossfade running");
		return false;
	}

	cmb_.pool = std::move(pool);
	cmb_.hash = r.hash;
	cmb_.label = r.label;
	cmb_.index = -1;
	combatResolved_ = true;
	if (a_armed && a_start) {
		LOG_I("combat: start  fade=%.1f  '%s'", cfg_->fadeCombatEnter, r.label.c_str());
		StartIndex(1, Pick(cmb_, -1), a_now, cfg_->fadeCombatEnter);
	} else if (a_armed) {
		// Armed, but the ambient layer still owns the air: cache the pool and
		// start on a later tick, once that voice has gone silent.
	} else {
		LOG_D("combat: armed, waiting %.1fs before play  '%s'", cfg_->combatArmTime, r.label.c_str());
	}
	return true;
}

bool Director::NearEnd(int a_slot, float a_fade) const
{
	if (!sink_ || !sink_->IsPlaying(a_slot) || sink_->IsPending(a_slot)) return false;
	// A handover during a running crossfade would be a third voice: skip it and
	// let the clip end, the normal "finished" path picks it up one tick later.
	if (sink_->IsFading(a_slot)) return false;

	const std::string cur = sink_->CurrentPath(a_slot);
	const std::string& mark = (a_slot == 0) ? ambHandover_ : cmbHandover_;
	if (cur.empty() || cur == mark) return false;  // already handed over from this clip

	const float dur = sink_->DurationSec(a_slot);
	const float pos = sink_->PlayheadSec(a_slot);
	if (dur <= 1.0f || pos <= 0.0f) return false;

	// start the follow-up while the current one is still playing, so the fade
	// has something to fade *into* - no silence, no re-attack
	const float lead = std::max(0.8f, a_fade * 0.9f);
	return (dur - pos) <= lead;
}

void Director::Update(double a_now, const GameContext& a_ctx)
{
	sink_->Poll(a_now);

	// Where the vanilla handover looks: while the world is mid-swap there is no
	// event or location to speak of, so Covers() keeps its last answer instead of
	// re-deriving one from an empty world (see Covers()).
	worldBusy_ = a_ctx.loading || a_ctx.mainMenu || !a_ctx.inWorld || !a_ctx.attached;
	mainMenu_ = a_ctx.mainMenu;

	const std::uint64_t fp = a_ctx.Fingerprint();
	if (fp != lastCtxFp_) {
		lastCtxFp_ = fp;
		LOG_D("context: cell=%08X ws=%08X hour=%.2f bits=0x%016llX  w=%d a=%d L=%d M=%d combat=%d sensor=%d dlg=%d ride=%d",
			a_ctx.cellFormID, a_ctx.wsFormID, a_ctx.hour, static_cast<unsigned long long>(a_ctx.bits),
			a_ctx.inWorld ? 1 : 0, a_ctx.attached ? 1 : 0, a_ctx.loading ? 1 : 0, a_ctx.mainMenu ? 1 : 0,
			a_ctx.inCombat ? 1 : 0, a_ctx.engaged ? 1 : 0, a_ctx.inDialogue ? 1 : 0, a_ctx.riding ? 1 : 0);
	}
	sink_->SetMasterGain(MasterGain());

	// master switch from the page: fade everything out once, then stay quiet.
	// The mixer keeps its levels, so flipping it back is instant.
	if (!cfg_->enabled) {
		if (sink_->IsActive(0) || sink_->IsActive(1) || !amb_.pool.empty() || !cmb_.pool.empty()) {
			LOG_I("music disabled -> fading out");
			sink_->Stop(0, cfg_->fadeHandback);
			sink_->Stop(1, cfg_->fadeHandback);
			Clear(amb_);
			Clear(cmb_);
			combatResolved_ = false;
			lastCombat_ = 0.0;
			combatReason_ = 0;
		}
		PublishStatus(false);
		return;
	}

	UpdateGate(a_now, a_ctx);

	// Interactive Music Radio took the context.  The player linked an external
	// source (a player, a browser, a rhythm game) from the in-game radio, and it
	// is audible right now, so every local layer stands down: our two voices fade
	// out, our pools are dropped, and Covers() keeps the vanilla category at 0
	// for the whole time the link is held - otherwise Skyrim's own selection
	// would start playing underneath the stream the moment our pools were empty.
	//
	// Nothing else is frozen.  The world gate, CombatGate, the sensors and the
	// resolution table keep running, so when the radio signs off (or the player
	// closes it) the current cell / hour / fight is simply resolved again on the
	// next tick.  The handshake has a heartbeat, so a crashed IMR releases here
	// automatically instead of leaving the game silent for good.
	if (a_ctx.imrTakeover) {
		if (!imrTakeover_) {
			imrTakeover_ = true;
			LOG_I("IMR: radio on the air -> local music off, vanilla held back");
		}
		if (sink_->IsActive(0) || sink_->IsActive(1) || !amb_.pool.empty() || !cmb_.pool.empty()) {
			sink_->Stop(0, cfg_->fadeHandback);
			sink_->Stop(1, cfg_->fadeHandback);
			Clear(amb_);
			Clear(cmb_);
			combatResolved_ = false;
			lastCombat_ = 0.0;
			combatReason_ = 0;
			combatArmAt_ = -1.0;
		}
		PublishStatus(false);
		return;
	}
	if (imrTakeover_) {
		imrTakeover_ = false;
		LOG_I("IMR: radio off the air -> local music takes the context back");
	}

	// Underwater: no music at all.  The player's head is below the surface -
	// the engine flag that drives the muffle effect - and that space belongs to
	// the underwater ambience mods; a music track on top of it is exactly what
	// the player does not want.  Both layers fade out (the combat gate keeps
	// running, so a fight that continues at the surface is picked back up), and
	// Covers() keeps the vanilla category held at 0 for as long as it lasts, so
	// neither system plays down there.
	if (a_ctx.underwater) {
		if (!underwater_) {
			underwater_ = true;
			LOG_I("underwater -> no music (both layers fade out, vanilla held back)");
		}
		if (sink_->IsActive(0) || sink_->IsActive(1) || !amb_.pool.empty() || !cmb_.pool.empty()) {
			sink_->Stop(0, cfg_->fadeHandback);
			sink_->Stop(1, cfg_->fadeHandback);
			Clear(amb_);
			Clear(cmb_);
			combatResolved_ = false;
			lastCombat_ = 0.0;
			combatReason_ = 0;
			combatArmAt_ = -1.0;
		}
		PublishStatus(false);
		return;
	}
	underwater_ = false;

	if (!gateOpen_) {
		if (sink_->IsActive(0) || sink_->IsActive(1) || !amb_.pool.empty() || !cmb_.pool.empty()) {
			LOG_I("hand back [gate closed] -> vanilla music");
			sink_->Stop(0, cfg_->fadeHandback);
			sink_->Stop(1, cfg_->fadeHandback);
			Clear(amb_);
			Clear(cmb_);
			combatResolved_ = false;
			lastCombat_ = 0.0;
		}
		PublishStatus(false);
		return;
	}

	// The player ended the fight - by sheathing, by running away, or by nobody
	// keeping it alive: drop the hysteresis timer AND the arm-time clock so the
	// combat layer starts ramping down on this very tick, and the *next* fight
	// has to prove itself from zero again.  Without the arm reset, a string of
	// quick kills (assassinations) would find the clock still running from the
	// previous fight and cut a combat track in at once - which is exactly what
	// a short kill must never do.  A timestamp (not a flag) because the engine
	// thread samples at 20 Hz.
	if (a_ctx.combatEndAt > combatEndSeen_) {
		combatEndSeen_ = a_ctx.combatEndAt;
		if (lastCombat_ > 0.0) {
			const char* how = a_ctx.combatEndReason == 2 ? "player fled the fight"
				: (a_ctx.combatEndReason == 3 ? "nobody kept it alive" : "weapon sheathed");
			LOG_I("combat: %s -> event over, combat layer fades out now", how);
			lastCombat_ = 0.0;
			combatReason_ = 0;
		}
		combatArmAt_ = -1.0;
	}

	// combat with hysteresis: dialogue, surrenders and short disengages must not
	// flip the layer back and forth
	if (a_ctx.inCombat) {
		combatReason_ = a_ctx.engageReason;
		lastCombat_ = a_now;
		combatCtx_ = a_ctx;
	}
	const bool wantCombat = a_ctx.inCombat || (lastCombat_ > 0.0 && a_now - lastCombat_ < static_cast<double>(cfg_->combatHold));
	if (!wantCombat) combatReason_ = 0;

	// Arm time: the gate has already confirmed this is a real fight, but a momentary
	// scuffle (a single hit, a brawl that breaks off in a second) still trips it.
	// Hold the actual combat track back until the fight has been continuously open
	// for fCombatArm seconds.  Until then nothing is inserted and the ambient layer
	// is not ducked, so the running music is never interrupted by a false alarm.
	if (wantCombat) {
		if (combatArmAt_ < 0.0) combatArmAt_ = a_now;
	} else {
		combatArmAt_ = -1.0;
	}
	const bool armed = combatArmAt_ >= 0.0 && (a_now - combatArmAt_) >= static_cast<double>(cfg_->combatArmTime);
	const bool fighting = wantCombat && armed;

	// dialogue freeze: do not re-resolve the ambient layer while talking
	if (a_ctx.inDialogue) {
		if (dialogueT0_ == 0.0) dialogueT0_ = a_now;
	} else {
		dialogueT0_ = 0.0;
	}
	const bool frozen = a_ctx.inDialogue && (a_now - dialogueT0_ < 90.0);

	// A pending context switch outranks "next track of the old pool": if the
	// target is already waiting, handing the old pool's next track over would
	// restart its fMinPlay clock and lock the player into a pool the context has
	// already left.  Let this clip run out; EnsureAmbient switches on the next
	// tick (IsActive is false by then, so the guard lets it through).
	const bool ambPending = ambArb_.wantSince >= 0.0;

	// Every StartIndex sets its bit, so the handover / end-of-track paths below
	// skip a layer that was already handled this tick.  Without this, a pool
	// switch would be followed immediately by a handover to the new pool's next
	// track - two Play() calls on the same slot in one tick.
	switchedMask_ = 0;

	// ---- exclusive layer ownership --------------------------------------
	// Exactly one track is audible at any moment.  When a new event (a fight)
	// takes over, the track that is playing is stopped *first* and the new one
	// only starts once that voice has gone silent, so two pieces are never
	// heard together.  The crossfade is kept only *within* a layer, i.e.
	// between two tracks of the same pool.
	sink_->SetBaseGain(1, 1.0f, 0.2f);  // the combat layer is never ducked
	{
		float ambBase = 1.0f;
		if (!fighting && a_ctx.inDialogue && cfg_->duckInDialogue) ambBase *= cfg_->dialogueDuck;
		sink_->SetBaseGain(0, ambBase, 0.2f);
	}
	const float swap = std::max(0.05f, cfg_->fadeSwap);

	if (fighting) {
		// Resolve the combat pool *without* launching it: we must know whether
		// the fight even has music before stopping the track the player hears.
		EnsureCombat(a_now, true, armed, a_ctx, /*a_start=*/false);
		const bool hasCombatMusic = !cmb_.pool.empty();

		// IsActive(1) also covers a combat voice that is still fading out, so
		// this branch is taken until slot 1 is completely silent.
		if (hasCombatMusic || sink_->IsActive(1) || sink_->IsPending(1)) {
			if (sink_->IsActive(0)) {
				// Ambient owns the air: fade it out now, start combat next tick.
				if (!sink_->IsFading(0)) {
					LOG_I("combat: taking over -> stopping the current track (fade %.2fs)", swap);
					sink_->Stop(0, swap);
				}
				PublishStatus(true);
				return;
			}
			// Air is free: launch (or resume) the combat track and advance it.
			EnsureCombat(a_now, true, armed, a_ctx, /*a_start=*/true);
			if (!(switchedMask_ & 2u) && NearEnd(1, cfg_->fadeCombatExit)) {
				if (const auto cur = sink_->CurrentPath(1); !cur.empty()) {
					cmbHandover_ = cur;
					Advance(1, a_now, cfg_->fadeCombatExit);
				}
			}
			if (!(switchedMask_ & 2u) && sink_->ConsumeFinished(1)) {
				Advance(1, a_now, cfg_->fadeCombatExit);
			}
			PublishStatus(true);
			return;
		}
		// The fight has no music of its own: keep the ambient track rather than
		// fading it out for nothing.  Fall through to the ambient layer.
	}

	// Ambient owns (or the fight has nothing to play).  Release the combat
	// layer once the fight is really over (this also drops a stale "no combat
	// tracks" cache), or when something of it is still sounding; while a
	// musicless fight is ongoing the cached "no tracks" answer is kept, so we
	// do not re-resolve the combat rules every tick.
	if (!wantCombat || sink_->IsActive(1) || sink_->IsPending(1)) {
		EnsureCombat(a_now, false, false, a_ctx);
	}
	if (sink_->IsActive(1)) {
		PublishStatus(false);
		return;
	}

	if (!frozen) EnsureAmbient(a_now, a_ctx);

	// hand the next track over *before* this one ends: the crossfade then has a
	// partner for its whole length and the join is inaudible
	if (!(switchedMask_ & 1u) && !ambPending && NearEnd(0, cfg_->fadeLocation)) {
		if (const auto cur = sink_->CurrentPath(0); !cur.empty()) {
			ambHandover_ = cur;
			LOG_D("handover [ambient] %.1fs before end", sink_->DurationSec(0) - sink_->PlayheadSec(0));
			Advance(0, a_now, cfg_->fadeLocation);
		}
	}
	if (!(switchedMask_ & 1u) && sink_->ConsumeFinished(0)) {
		if (ambPending && !frozen) {
			LOG_D("track end [ambient] -> pending context switch takes over");
		} else {
			LOG_D("track end [ambient]");
			Advance(0, a_now, cfg_->fadeLocation);
		}
	}

	PublishStatus(false);
}

}  // namespace cms
