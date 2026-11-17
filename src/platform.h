// Game-side adapters.  Everything in here runs on the game's main thread
// (dispatched through SKSE tasks) - the engine thread never touches RE memory.
#pragma once

#include <string>

#include "core.h"

namespace cms::platform {

std::string DataDir();
std::string PluginDir();  // <Data>\SKSE\Plugins

// "chinese" / "english" / ... straight from the game settings (sLanguage).
std::string DetectLanguage();

GameContext Snapshot();  // main thread only

// --- vanilla music: takeover with fallback --------------------------------
// The plugin replaces the vanilla music system instead of silencing it.  One
// lever is used and it is the safe one: the `AudioCategoryMUS` menu level.
//
//   SteerVanillaMusic(true)   the vanilla layer yields - our track owns the
//                             event/location, vanilla keeps selecting and
//                             decoding in the background but is held at 0
//   SteerVanillaMusic(false)  the vanilla layer plays again: the level the game
//                             (or the player's Music slider) wants is restored,
//                             which is the fallback for a place this library
//                             does not cover
//
// Called every tick with the *current* decision, so a level the game rewrites
// (options menu, load screen) is corrected on the next tick and the value to
// restore is learned from it.  Nothing here touches `BSMusicManager`: patching
// its vtable is what used to corrupt the track queue (use-after-free, two
// minidumps), so the level is steered and the queue is left alone.
// Returns the number of music categories found (0 = audio system not ready).
int SteerVanillaMusic(bool a_yield);

// Combat detection beyond Actor::IsInCombat().  Snapshot() no longer ORs the AI
// flag in directly; instead these sinks feed cms::CombatGate (core.h), which
// only opens on a *confirmed* fight:
//   TESCombatEvent - an enemy's state flipped to kCombat with the player as the
//                    target (retaliation or ambush); the player's own flip and
//                    NPC-vs-NPC flips are ignored
//   TESHitEvent    - the player damaged a live actor (arms the gate) or an enemy
//                    damaged the player (answers it); trap/fall damage has no
//                    cause and is ignored
bool InstallCombatSensor();
void RemoveCombatSensor();

// --- Interactive Music Radio link -----------------------------------------
// Reads the IMR handshake block ("IMR_CMS_HANDSHAKE", 64 bytes, see ImrLink in
// core.h) and reports whether the radio is currently on the air.  The mapping
// is opened once and kept for the session; a missing block means IMR is not
// installed or not running, which is a normal state, not an error.
//
// Cheap enough to call every tick: it is one memcpy out of a mapped page plus
// the arithmetic in ImrLinkActive().  The heartbeat makes the link self-healing
// - if IMR dies without a clean shutdown, the takeover expires on its own.
bool ImrTakeover();

}  // namespace cms::platform
