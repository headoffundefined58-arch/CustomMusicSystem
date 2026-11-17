# CustomMusicSystem 2.5 — design notes

## Modules

| file | role |
|---|---|
| `src/core.h/.cpp` | Config parser, logger, `Bit` predicates, `Library` (folder scan), `RuleTable` (69 built-in rules), `Resolve()` |
| `src/audio.h/.cpp` | RIFF/XWMA parser, async clip loader thread, XAudio2 sink with two slots and equal-power fades. **Only TU that includes `<windows.h>`** |
| `src/director.h/.cpp` | State machine: world gate, layer choice, pool selection, gain composition, `Status` for the UI. Game-agnostic |
| `src/volume.h/.cpp` | One master level (2.3 dropped the per-folder / per-track tables) + `CustomMusicSystem.mixer.txt` persistence. Locked; read by the audio thread |
| `src/menu_page.h/.cpp` | The volume + settings pages, written against an imgui function-pointer struct. No Windows, no game, no ImGui → unit-testable |
| `src/i18n.h/.cpp` | One compiled-in table per build (`CMS_UI_LANG_ZH`). No loose files, no `sLanguage` probe, no runtime switch: the language is the DLL you installed |
| `src/menu.h/.cpp` | Bridge: resolves the framework's cimgui exports, publishes a track-list copy, adapter between `Director::Status` and the pages, INI save/reload |
| `src/fwio.h/.cpp` | `ImGuiIO::MouseWheel` (wheel-to-adjust). The only TU that includes the framework's own SDK header, so the field offset is compiler-checked |
| `src/platform.h/.cpp` | RE adapters: snapshot, vanilla handover (`SteerVanillaMusic`), combat event sinks feeding `CombatGate` |
| `src/main.cpp` | SKSE entry, seqlock publication, engine thread, startup wiring, mixer autosave |
| `tests/` | Off-line rule tests, mixer/localisation tests, mock-driven page test, library census, real playback probe |

## Threads

```
game main thread                     engine thread (iTickMs, 20 Hz)
─────────────────                    ─────────────────────────────
SKSE task each tick:                 read GameContext (seqlock)
  Snapshot()  -> bits/cell/hour      Director::Update
  SteerVanillaMusic(yield?)            Resolve ambient / combat
  publish GameContext  ───────────►    Pools, picks, fades
                                       Director::Covers() -> yield?
                                       AudioSink::Play / Poll
                                       mixer autosave (dirty -> file)

menu frame (game thread, F1)           audio thread (XAudio2 callback)
  VolumeMix getters/setters  ◄──────►  VolumeMix::TrackEffective (mutex)
  reads menu::g_groups  ◄───────────   PublishLibrary (after each scan)
                                       (loader thread: file read + RIFF parse)
```

* RE memory is touched **only** on the game thread; the engine thread sees a
  trivially-copyable snapshot. That removes the "read a cell from another
  thread while it is being unloaded" class of crash that a 10 Hz poll invites.
* The loader thread parses files off the tick path, so a 6 MB track never
  stalls scheduling; the next track is prefetched as soon as the current one
  starts, which makes "track end → next" effectively gapless.
* Steady state allocates nothing: the context is a POD, resolution is 69
  integer ANDs, pools are rebuilt only when the pool hash changes.
* The volume page never walks `cms::Library`: the engine thread rescans it while
  the game thread draws, so `menu::PublishLibrary` hands the page a compact copy
  behind a `shared_mutex`. Lock order is always library -> mixer, never the
  reverse, which is why the page can hold the read lock while it mutates gains.
* The page's widget sequence is verified by a recorder in `tests/tests.cpp`:
  unmatched `BeginChild` / `PushID` / `PushStyleColor` is an ImGui assertion,
  i.e. a crash in game, so it must fail in the test binary instead.

## Predicates

Everything the rules can ask about is one bit in a `uint64_t`
(`src/core.h`, `enum Bit`). `GameContext::bits` carries:

* `Interior` / `Exterior`, `Riding`, `InCombat`
* hour band (Morning/Day/Evening/Night — 5/8/18/22 boundaries)
* town + city, dungeon subtype, biome, DLC area, interior flavour, home
* enemy class bits derived from `HasKeywordByEditorID` (refreshed at 4 Hz, not
  every tick) and name checks for Miraak / Karstaag / Vampire Lord

## Rules

`Rule{folder, prefix, priority, layer, additive, replace, mask, group}`.

* **Larger priority wins.** Ordering (ambient): biome 9-12 < ride 16 < interiors
  17-25 < dungeon 20-22 < town 22-24. Combat layer: 28-40.
* **`group`** (Explore / Ride / Dungeon / Combat / Town / Interior / Home)
  scopes `additive` rules: "Explore - Day" only merges into an exploration
  anchor, so a town cell never drags exploration tracks in just because it is
  also an exterior cell. 1.x lacked this and double-listed folders.
* **Degradation**: if no exclusive rule matches (normal outdoors — vanilla cells
  carry no biome keyword) the strongest additive rule becomes the anchor, so the
  layer plays the hour track instead of falling silent.
* **`replace`** (Ride): the anchor plays alone *when its folder has tracks*;
  an empty ride folder is skipped entirely, so riding just keeps the location
  music.

## Gates and hysteresis

* **World gate**: `inWorld && cell.IsAttached() && !loading && !mainMenu` must
  hold for `fWorldSettle`. Latched: short dips (doors, fast travel) do not close
  it; only main menu or 3 s without a player/cell does. Force-open after
  `fWorldSettle + 3 s` in case `IsAttached()` never reports true.
* **Combat hold**: `fCombatHold` keeps the combat layer alive after
  `IsInCombat()` flaps (dialogue, surrender, sheathing). The combat resolution
  is cached because the context has already lost the `InCombat` bit by then.
* **Dialogue freeze**: while the dialogue menu is open the ambient layer is not
  re-resolved (cap 90 s), which is what stopped "combat → ambient → combat"
  ping-pong.

## Volume model

```
voice volume = fadeEnvelope × slotBase × master
master       = VolumeMix::MasterEffective()   (the page's one slider, 0..150 %)
slotBase(ambient) = dialogue ? fDialogueDuck : 1   (combat stops the ambient layer, it is not ducked)
```

`AudioCategoryMUS` (FormID `0x00071E64`) is the vanilla layer's level, and it is
steered rather than muted: 0 while a pool of ours covers the event/location, the
level the game wants otherwise (see "Vanilla handover").  `fMusicVolume` is never
read and never written - it only drives the vanilla layer, which is exactly why
multiplying our master by it (the old `bFollowMusicSlider`) double-counted the
handover.  Parameter changes are smoothed over ~0.12 s so dragging the slider
does not zipper.

## Exclusive handover (2.6.3)

Only one track is audible at any moment.  When a fight takes over, the ambient
track is stopped **first** (over `fFadeSwap`, 0.35 s) and the combat track only
starts once that voice has gone completely silent.  The same rule runs in
reverse when the fight ends: the combat voice fades out over `fFadeCombatExit`
and the ambient layer resumes only after it is silent.  Two pieces therefore
never sound together.

This replaces the 2.6.2 "additional layer" model, where combat ducked the
ambient layer to `fCombatDuck` (0.35) instead of stopping it.  That kept the
running track audible but is exactly the "two songs at once" report: two
`IXAudio2SourceVoice`s alive at the same time.  The cost of strict exclusivity is
a very short dip while the outgoing track fades; `fFadeSwap` trades that against
how abrupt the takeover feels.  The crossfade is still used *within* a layer
(track -> next track of the same pool), where it is a normal playlist transition
rather than a new event.

## XWM decode

Skyrim's `.xwm` is `RIFF … XWMA` with an 18-byte `WAVEFORMATEX` (tag `0x0161`)
plus a `dpds` chunk. XAudio2 still decodes xWMA, but only when the buffer is
submitted with `XAUDIO2_BUFFER_WMA` (decoded-bytes-per-packet table) — without
it `SubmitSourceBuffer` returns `0x88960001`. Verified by
`CustomMusicSystemPlayTest` (real track, no game needed).

## Sizing

621 KB vs 1.30 MB in 1.x: spdlog is gone (plain `FILE*` logger), no JSON rule
parser, `/GL /LTCG /OPT:REF /OPT:ICF`.

## Combat gate (2.2)

`Actor::IsInCombat()` is the AI's own flag, and it is a bad trigger on its own: it
lands a beat after the swing, it stays on through brawls and scripted fights, and
it flips on when the player swings at something that never fights back.  2.1 ORed
the flag with "any event fired", which is why a swing at a passive NPC or a hit on
an ore vein could start the combat track.

2.2 moves the decision into `cms::CombatGate` (`src/core.h`): pure time arithmetic,
no game API, so `tests/tests.cpp` covers every branch.  `src/platform.cpp` only
translates events into three inputs:

| event | condition | fed to the gate as |
|---|---|---|
| `TESCombatEvent` (newState = kCombat) | `actor != player && targetActor == player && !actor->IsDead()` | `OnEnemyEngage` |
| `TESHitEvent` | `target == player && cause != nullptr` | `OnEnemyHit` (no cause = trap / fall → ignored) |
| `TESHitEvent` | `cause == player && target->As<Actor>() alive` | `OnPlayerHit` (arms, never opens) |
| 4 Hz scan | a living hostile high actor is in combat | `OnHostileInCombat` + distance to the nearest one |

The player's own `TESCombatEvent` flip and NPC-vs-NPC flips are **not** triggers.
`Update(now, aiFlag)` opens on `Duel` (offense window 6 s, answered, **and still
going kDuelConfirm = 2 s after the answer**), `Ambush` (enemy engaged with no
offense) or `Sustained` (hostile in combat 2.5 s).  It holds for 2 s without the
AI flag, and indefinitely while the flag is on; then `fCombatHold` keeps the
layer up.  `GameContext::engageReason` carries the verdict to the UI/log, and the
reason is only upgraded Duel > Ambush, never downgraded.

### What counts as a fight (2.6.4)

Striking first is not a combat event in itself.  The answer opens only a
*pending* duel (`duelSince_`); the gate confirms it once the exchange has kept
going `kDuelConfirm` (2 s) with the AI flag on, and cancels it when the player
sheathes or the flag has been off for `kDuelGrace` (0.75 s - a grace, because the
flag can lag the answer event by a tick).  Three consequences:

* a sneak attack the target dies in never becomes a fight - the answer arrives,
  the flag drops inside the window, the pending duel is cancelled;
* swinging at something that never really fights back never becomes a fight;
* an enemy attacking first is still a fight at once (`Ambush`).

The same timestamp that ends a fight (`combatEndAt`, now also advanced by a
timeout release, reason 3) resets the director's arm-time clock, so the *next*
quick kill always starts from zero instead of finding the layer still armed from
the previous one.

### Ending a fight: sheathe, retreat, timeout (2.4 / 2.4.1)

Three ways to close the gate, all of them the player's decision:

| verdict | input | rule |
|---|---|---|
| sheathe | `ActorState::IsWeaponDrawn()` | weapon down for `kSheatheConfirm` (0.5 s) |
| retreat | nearest hostile distance | beyond `kFleeDistance` (3000 u) for `kFleeConfirm` (3 s) |
| timeout | `aiFlag` | flag off for `kHold` (2 s) |

The retreat rule exists because the AI flag is *wrong* in exactly this case: a
fleeing player leaves enemies behind who keep `IsInCombat()` for a minute while
they search, so the combat track played on after the player had already left.
Distance is the only honest signal, and it is measured against the **nearest**
hostile in combat with the player - one pursuer left behind must keep the track
alive.  Any real contact (`TESHitEvent` either way, a combat event engaging us)
clears the timer, so a sniper shooting from a ridge is not a retreat.

An empty scan is passed as "very far away" (`1e9`) rather than `-1`: nobody
fighting us *is* a retreat.  `-1` means "not measured" and switches the rule off,
which is the behaviour the pre-2.4.1 unit tests exercise.

## Underwater silence (2.6.5)

`Director::Update` has one hard override above everything else: when the
snapshot reports the player underwater (`Actor::BOOL_FLAGS::kUnderwater`, the
engine's own muffle bit - no water-form maths to drift out of sync), both slots
fade out over `fFadeHandback`, the pools are dropped, and `Covers()` reports
*covered* - which holds the vanilla category at 0 instead of handing the context
back.  A plain hand-back would start vanilla music underwater, and the whole
point is that the underwater soundscape belongs to ambience mods.  The combat
gate keeps running through a dive, so a fight that resumes at the surface picks
its track up after the normal arm time.

Both endings travel to the director as the `combatEndAt` timestamp plus
`combatEndReason` (1 sheathe / 2 flee), so the combat layer ramps down on the
same tick instead of waiting out `fCombatHold`.  After a retreat the gate stays
`Fled()` until somebody is close again; that flag blocks the `Sustained`
fallback, otherwise the same searching enemy would restart the track one tick
later and the music would stutter for as long as it kept looking.

## The IMR handshake reader (2.6.6)

Interactive Music Radio is a second SKSE plugin with its own DLL, and the only
thing the two share is a 64-byte page.  That is a deliberate ceiling on the
coupling: no exported symbols, no patching, no load-order requirement.  Either
plugin runs alone.

`cms::ImrLink` (core.h) fixes the layout, `ImrLinkActive()` (core.cpp) is the
whole decision - magic, version, `state == 1`, and a heartbeat no older than
`kImrHeartbeatMaxMs` - and it is a pure function, so it is unit-tested without
Windows or the game (`ImrLinkTests`).

Two design points worth keeping:

* **Heartbeat, not a flag.** A bare state word would survive a crashed writer
  and leave the player in permanent silence.  CMS reads `GetTickCount64()` on
  every snapshot, so a dead IMR ages out in 3 s.  A stamp *ahead* of the current
  tick means the section survived a reboot (the counter restarted lower), and is
  treated as stale rather than as an eternal "fresh" link.
* **Held, not handed back.** While the radio is on the air, `Covers()` returns
  true.  Returning false would hand the event to the vanilla layer, which then
  plays Skyrim's selection underneath the stream.  This is the same reasoning as
  the underwater rule above: hold the category at 0 whenever something else owns
  the soundscape.

Reader side is one `OpenFileMappingW` attempt per session, cached; a missing
block is a normal state (IMR not installed), logged at debug level only.

## Thread ownership and the load window (2.4.2)
One invariant, enforced everywhere: **RE state is only read or written on the
game's main thread.**  The engine loop (20 Hz) is a plain worker; it reads the
seqlock-published `GameContext` and never calls into the game.

This used to be violated by a stall fallback: if the SKSE task queue did not
drain for 3 s, the engine loop called `platform::Snapshot()` itself, and on that
path it also re-applied the vanilla level every 50 ms - i.e.
`BGSSoundCategory::SetCategoryVolume(0)` for every music category, from a
worker thread, 20 times a second.  A save load is precisely when the task queue
goes quiet for seconds, so the write landed while the engine was rebuilding the
audio system.  The symptom was a `0xC0000409` fast fail inside `ucrtbase` -
`_invoke_watson` (invalid parameter) or `abort` - which no crash logger can hook,
on a *different* thread than the one that caused it.

Rules that replaced it:

| rule | why |
|---|---|
| a stalled queue means "hold the last context", never "read it yourself" | off-thread RE access is UB; during a load it is UB on a world that is being torn down |
| one pending snapshot task, never re-queued on timeout | a load screen used to leave ~20 queued tasks that all ran in a single frame |
| no `SetCategoryVolume` while `Loading Menu` / `Main Menu` is open | that window is the audio rebuild; the level is re-applied on the first tick after it |
| `Snapshot()` returns a bare `loading` context without touching player/cell/ProcessLists | those objects are between two values mid-load, and the gate is closed anyway |
| the combat gate is reset when a load starts | a fight from the previous world must not leak into the new save |

Because a fast fail cannot be intercepted, the plugin installs a pass-through
`_invalid_parameter_handler` and `std::terminate` handler that log the offending
expression/function/file/line and then hand over to the original handler, so the
crash behaviour is unchanged but the next occurrence is identifiable from
`CustomMusicSystem.log` alone.

## Vanilla handover (2.5)

2.0-2.4 offered three ways to deal with the vanilla music system: leave it alone,
**mute** it (`AudioCategoryMUS` = 0), or "take over" by patching
`BSMusicManager`'s vtable - which crashed.  The mute was the shipped option, and
it had two consequences nobody wants: the game had no music at all where the
library had nothing, and the main menu was silent.

2.5 states the intent instead: **this plugin replaces the vanilla music system, and
gives the context back when it has nothing to play.**  There is no mute mode.

| piece | file | rule |
|---|---|---|
| decision | `Director::Covers()` | a layer owns a pool for the current context, or one of our voices is still sounding |
| transport | `atomic<bool> g_vanillaYield` | engine thread writes it after `Update`, game thread reads it in `SnapshotTask` |
| write | `platform::SteerVanillaMusic(bool)` | level 0 to hold vanilla back, restore the learned level to hand it back |
| switch | `Config::keepVanilla` | 1 = never touch the level, both systems play |

`Covers()` holds its previous answer while the world is busy (`loading`,
`mainMenu`, `!attached`) or the gate has not opened yet, and drops to `false` at
the main menu.  Deriving it from the empty mid-load world would release the
vanilla layer at every load screen and take it back a moment later - an audible
flam at every door.

There is no re-assert timer.  Every call reads the category's live level first:
a non-zero level while holding is what the game wants (Music slider, load-screen
reset), so it is learned and then zeroed again; a zero level while we are not
holding is ours to give back.  Nothing is written while a menu or a load screen is
up, so the mid-load audio rebuild never sees a write from this plugin.

## Crossfade and handover (2.2)

* `AudioSink::Voice` owns its `IXAudio2SourceVoice` (RAII).  Before, replacing an
  already fading voice just dropped the pointer, so the voice kept playing — the
  "two tracks at once" bug that the manual switch made easy to hit.
* `PlayheadSec()` reads `SamplesPlayed` from XAudio2, so `Director` cues the next
  track `fFadeLocation` seconds before the end instead of after `ConsumeFinished`.
  The fade therefore always has a partner voice; `ConsumeFinished` stays as the
  fallback for "the handover could not be scheduled".
* Slot base gain ramps over an explicit duration (`SetBaseGain(slot, gain, sec)`),
  so the combat duck follows `fFadeCombatEnter` / `fFadeCombatExit` instead of
  dropping out inside one 80 ms tick.
* A voice never starts above zero: `max(0.05f, fade)` is always applied.

## Volume model: one level (2.3)

`VolumeMix` holds **one** `Entry{gain}`: the master.  2.1-2.2 also kept a
per-event-folder and a per-track table and a page that listed all of them; with 69
folders that was 574 rows in a 420 px scroll box, clipped labels, and single
tracks that could be quietened for good by one mis-drag.  The tables, their
mutex-guarded lookups and the audio thread's per-voice lookup are gone.

`AudioSink::Poll` now computes `env x slotBase x master` per voice and eases it
with an 80 ms one-pole filter, so dragging the slider never steps the waveform.
The mixer file keeps its `m` row; old `f` / `t` rows are counted
(`VolumeMix::LegacyRows()`) and dropped on the next save, so an upgrade never
re-applies a level the UI cannot show.  The vanilla `AudioCategoryMUS` is 0
wherever this plugin has music of its own (2.5: and restored where it does not),
which is why the in-game Music slider cannot drive this plugin and the page has to
own the master.

The `m` row keeps its fourth column (a per-install mute flag) only so the file
format stays readable by older builds; `Load` ignores it and `Save` writes 0.
Muting was removed in 2.5 - a level of 0 % is silence, and a flag that can
disagree with the slider is a way to start the game silent with nothing on the
page to undo it.

## Pages (2.2)
Two sections are registered under one category: `Volume` and `Settings`.  The
pages never call ImGui — they take a `menupage::Api` of function pointers, which
is what lets the test binary drive them with a recorder and assert the widget
sequence is balanced.  The wheel is the one thing that needs a *field* rather
than a function, so it lives in `fwio.cpp` (SDK header → compiler-checked offset,
plus a DisplaySize/DeltaTime sanity check so a framework update degrades to "no
wheel").  Settings editing the live `Config` is safe because the engine thread
only reads scalars from it after startup.  The page shows only fields a player can
hear; `iTickMs` and `bSingleFormatCheck` stay INI-only, and the library root is not
a field at all - it is `kLibraryFolder` in `core.h`, so no edit can point the
scanner at another folder.
