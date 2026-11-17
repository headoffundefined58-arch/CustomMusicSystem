# CustomMusicSystem 2.6.6

SKSE plugin that replaces Skyrim's music with your own `Data\Music\<event>` library.
No ESP, no Form edits, no load-order conflicts: it takes over the vanilla music
system and plays your `.xwm` files through its own XAudio2 graph.

**Takeover means replacement, not silence.** While the library has a track for the
current event or location, the vanilla layer stands down; the moment it has
nothing for that place, the vanilla music is handed the context back (fallback),
so a cell the library does not cover still gets the game's own music. One switch,
`bKeepVanilla`, decides whether the vanilla music is kept instead (it then plays
alongside).

2.2 added the in-game **settings page** (every INI key, editable, bilingual), the
**music on/off switch**, **mouse-wheel volume control**, the pre-emptive track
handover and the confirmed-fight combat gate.

2.3 cleaned the UI up: one volume for the whole library (the per-folder /
per-track mixer is gone) and a two-column layout with folded hints.

2.5 replaces the old "vanilla handling" switch (leave alone / mute / takeover)
with one handover rule and one checkbox: the plugin takes the vanilla layer's
place where it has music of its own and gives it back where it has none - there is
no mute mode any more, and no setting that leaves a place without music.

2.4 turned the language into a **build property**: there are two DLLs, one
English and one Chinese, and nothing is parsed or switched at runtime.  It also
adds the **switch guard** (a track is never cut off, a context change has to
settle, nothing starts over a running crossfade), makes a fight end when the
player **sheathes or runs away**, and stops a finished event by ramping the
layer down to silence instead of cutting it.

## What ships where

```
Data\Music\<event folder>\MM<Prefix><NN>.xwm         your library (69 folders)
Data\Music\<event folder>\Track limit N.txt          optional per-folder cap
SKSE\Plugins\CustomMusicSystem.dll / .pdb            the plugin (Chinese UI)
SKSE\Plugins\CustomMusicSystem.ini                   settings
SKSE\Plugins\CustomMusicSystem.mixer.txt             written by the volume page
SKSE\Plugins\Language-EN\CustomMusicSystem_en.dll    the English build, kept aside
SKSE\Plugins\Language-EN\README.txt                  how to swap (1 rename + 1 copy)
```

## The pages

`F1` -> **个性化音乐系统** -> **音量** · **设置**
(English build: `Custom Music System` -> `Volume` · `Settings`)

* **Wheel**: hovering a slider and scrolling changes it by one step - 1 % on
  volume rows, 0.1 s on fade rows. Three notches at once move three steps, so a
  flick does not slam the value to the end. Implemented in `src/fwio.cpp`
  against the framework's own `ImGuiIO`, with a runtime sanity check so a future
  framework update degrades to "no wheel" instead of garbage.
* **Enable music** (top of the volume page) writes `bEnabled`; off fades both
  voices out with `fHandbackFade`, stops resolving and hands the music back to the
  vanilla system. The volume level stays.
* **Settings** edits the live `Config` (changes apply on the next tick), and the
  Save button rewrites the INI in the canonical commented shape, keeping the
  previous file as `CustomMusicSystem.ini.bak`.  Two keys are deliberately **not**
  on the page - `iTickMs` (scheduler period) and `bSingleFormatCheck` (encoding
  warning); both are engine diagnostics and editing them in a menu adds nothing
  but a way to break the scheduler.
* **The language row is read-only.** It reports which build is installed; the
  text is compiled in (see "Localisation"). A switch there would have to rename
  the framework's live section tree, which is exactly what kept failing.

```
Volume page                          Settings page
[x] Enable music                     Crossfades  3.00 / 1.20 / 2.50 / 2.50 s, hold 2.5 s, settle 1.2 s
Running                              Switching   min play 12 s, settle 1.5 s, interval 4 s
--------- Volume ---------            Combat      sheathe ends fight, ambient 0.00, dialogue 0.45
Master volume [=======] 120%         Library     repeat window 3, min pool 1
meter         [====      ] 41 %      Vanilla     keep vanilla music: off (takeover + fallback)
--------- Now playing -----           Interface   Menu language: 中文 (read-only)
Ambient layer: Explore - Day - ...   Autosave volume
--------- Library ---------
26 groups / 48 tracks
[Reset to 100%] [Save now]
```

## What the page deliberately does not show (2.5)

The menu used to be a control panel for the implementation: a text field with the
library path, the scheduler period, a seed value for the master level, a switch
that multiplied everything by the game's Music slider, an encoding warning.  None
of that is a decision a player makes, and three of them were actively harmful:

* **The library folder is now a constant.**  `Data\Music\<event folder>\` is what
  every Personalized Music layout already uses, and a typo in that field was the
  fastest way to end up with "no music anywhere".  `kLibraryFolder` in `core.h` is
  the only definition; there is no INI key and no text field.
* **The master seed is gone.**  It only ever applied on a fresh install, before
  the page had written the mixer file - a value nobody could observe.  A new
  install starts at 100 %.
* **The game's Music slider is no longer an input.**  Scaling by `fMusicVolume`
  while the plugin also steers `AudioCategoryMUS` for the vanilla layer meant two
  multipliers pointing in different directions; the level the page shows is now
  the level that is applied.

Everything that stayed is named after what it does to the audio - crossfade,
hold, minimum play time, settle, interval, pool - instead of an internal field
name.

## The volume page

```
[x] Enable music                                  <- master switch
Running                                           <- gate state
--------- Volume ---------
Master volume   [========|-----] 110%             <- 0..150 %, wheel = +/-1 %
[========                                          ] 41 %   <- what is applied now
Every track plays at this level; the value is stored in the mixer file.
--------- Now playing ---------
Ambient: Explore - Day - MMExploreDay01.xwm
Combat: Confirmed duel (they struck back, and it lasted)
--------- Library ---------
26 groups / 48 tracks
[Reset to 100%] [Save now]
Changes are written to disk about a second after you stop.
```

**One level, not three.** 2.2 shipped a Windows-mixer clone: one slider per event
folder and one per `.xwm`. With 69 folders that was a 420-px scroll box of 574
rows, labels cut at the column edge, and a single mis-drag could quieten one
track for good - with no way to see which. 2.3 keeps only the value a player
actually reaches for:

| level | scope | stored as |
|---|---|---|
| master | everything | `m` row in the mixer file |

A voice's gain is `envelope x slot base x master`, eased over ~80 ms so dragging
the slider never clicks.  The level is the only state: 0 % *is* silence, so there
is no separate mute flag that could disagree with what the slider shows.

The mixer file keeps its name and its `m` row, so an existing install upgrades in
place. Old `f` / `t` rows are **read, counted and dropped** (and the file is
rewritten in the one-level shape on the next start) instead of quietly
re-applying levels the page can no longer show.

Edits are marked dirty by the page and written by the engine thread about a
second later (`bMixerAutosave`), so dragging never stalls on disk I/O. Deleting
`CustomMusicSystem.mixer.txt` resets the volume to 100 %.

## Settings (all optional, all editable in game)

Every row below has a widget on the Settings page; the table is the file/format
reference. `bEnabled` lives on the volume page (the switch at the top).

| key | default | meaning |
|---|---|---|
| `bEnabled` | 1 | master switch; 0 = this plugin stops and the vanilla layer gets the music back |
| `sUILanguage` | `auto` | **obsolete in 2.4** - the language is the build you install. Parsed, logged, otherwise ignored |
| `bMixerAutosave` | 1 | persist volume edits ~1 s after the last change |
| `fFadeSwap` | 0.35 | seconds the current track takes to fade out when a fight replaces it. The combat track starts only once the old one is silent, so two pieces never play at once |
| `bDuckInDialogue` / `fDialogueDuck` | 1 / 0.45 | duck under the dialogue menu |
| `bKeepVanilla` | 0 | 0 = **takeover**: the vanilla layer stands down where this plugin has a track and is handed back (fallback) where it has none · 1 = keep the vanilla music playing alongside |
| `fFadeLocation` / `fFadeCombatEnter` / `fFadeCombatExit` / `fHandbackFade` | 3.0 / 1.2 / 2.5 / 2.5 | crossfade lengths (s); the last one is also the fade used when the context goes back to vanilla |
| `fCombatHold` | 2.5 | keep combat music this long after the **AI** flag clears. Sheathing bypasses it |
| `fMinPlay` | 12.0 | a track plays at least this long before a context change may replace it (a nearly-finished clip is exempt) |
| `fSwitchSettle` | 1.5 | the new resolution must stay stable this long (a doorway / cell border does not count) |
| `fSwitchCooldown` | 4.0 | hard floor between two changes of the same layer |
| `bSheatheEndsCombat` | 1 | sheathing ends the fight event (duel and ambush alike) |
| `fWorldSettle` | 1.2 | how long the world must look ready before the first note |
| `iRecentWindow` / `iMinTracks` | 3 / 1 | no-repeat window, minimum pool size (a pool below the minimum counts as empty, i.e. vanilla takes the place) |
| `iTickMs` | 50 | scheduler period - **INI only**, not on the settings page |
| `bSingleFormatCheck` | 1 | log a folder that mixes `.xwm` and `.wav` - **INI only** |

Removed in 2.2: `bManualTrackSwitch`, `fFadeSkip`. The `+ / -` hotkeys are gone
**and so is the code path behind them** - see below.

## Why the switch is gone (and what replaced it)

Cueing a second track while the first one was still audible meant two
`IXAudio2SourceVoice` objects for one slot. The slot only ever kept one
"fading-out" pointer, so a third change - or any quick combat enter/exit - dropped
the pointer of a voice that was still running, and nothing owned it any more. It
kept playing at whatever volume it last had: the "two songs at once" report.
Three things fix that class of bug rather than just hiding the button:

1. `AudioSink::Voice` owns its source voice (RAII) **and every retiring voice is
   kept in a list until its own envelope reaches zero** (2.4). There used to be a
   single `prev` pointer, so a switch during a switch destroyed a fading voice
   mid-fade - audible as a click, and it left nothing to fade back into.
2. The next track is started **before** the current one ends: `PlayheadSec()` is
   read from XAudio2 and the follow-up is cued `fadeLocation` seconds early, so
   the crossfade has a partner for its whole length - no gap, no re-attack.
3. A new event is a **strict handover, not a layer** (2.6.3): when a fight
   starts, the ambient track is stopped first (`fFadeSwap`) and the combat track
   only starts once that voice is silent; when the fight ends the combat voice
   fades out and the ambient layer resumes only after it is silent. Two tracks
   therefore never sound together. The crossfade remains only *within* a layer
   (track -> next track of the same pool). Every voice start still gets at least
   a 50 ms ramp even when the configured fade is 0.

### The switch guard (2.4)

Four rules decide whether "the context changed" is allowed to become "another
track". They are one pure function, `cms::SwitchBlockReason()` in `core.cpp`, so
all of them are covered by `SwitchGuardTests()` without an audio device.

| rule | value | what it prevents |
|---|---|---|
| settle | `fSwitchSettle` 1.5 s | walking through a door or over a cell border swapping the pool twice in a second |
| crossfade | busy? wait | launching a third track on top of a running crossfade - the literal "two songs at once" |
| minPlay | `fMinPlay` 12 s, **exempt when the clip is nearly over** | hearing 1.5 s of one piece and 1.5 s of the next; the exemption turns the end of a clip into the natural handover |
| cooldown | `fSwitchCooldown` 4 s | any two changes of one layer inside the same fade window |

Two more details keep them honest:

* A **pending** context change outranks "next track of the old pool". Handing the
  old pool's follow-up over would restart the min-play clock and lock the player
  into a pool the context has already left, so the clip is allowed to run out and
  the switch happens on the next tick.
* One `StartIndex` per tick per layer (`switchedMask_`). Otherwise a pool switch
  is immediately followed by a handover into the *new* pool - two `Play()` calls
  in one tick, which is how overlaps used to start.

### How an event ends

The combat layer is not cut: `EnsureCombat(false)` hands the voice to `Stop()`
with `fFadeCombatExit`, its envelope ramps to zero and only then is the voice
destroyed. Same for an ambient pool that resolves to nothing (`fHandbackFade`) -
and there the fade is also the hand-over: as our voice ramps out, the vanilla
layer gets the place back, so a cell the library does not cover continues with the
game's own music instead of ending in silence.

## Vanilla music: takeover with fallback (2.5)

The plugin **replaces** the vanilla music system; it does not silence it.  Two
rules, one switch:

| situation | vanilla layer | log line |
|---|---|---|
| the library has a track for this event/location | held back (level 0) while our track plays | `vanilla music: taken over (...)` |
| the library has nothing here (empty folder, below `iMinTracks`, unknown cell, main menu) | gets the context back, vanilla music plays | `vanilla music: handed back (...)` |
| `bKeepVanilla = 1` | never touched; plays alongside | `vanilla music: kept ...` |

* **The decision is the resolution's**, not "is a voice audible".  A bound pool
  means the place is covered, so vanilla stands down just before our first note
  instead of after it.  A voice that is still fading out also counts, so the
  hand-back waits for our last fade instead of dropping vanilla on top of it.
* **The hand-back is the fallback**, and it is why no place is ever silent: the
  pool resolves empty, the ambient voice ramps out over `fHandbackFade`, and the
  level the game (or the player's Music slider) wants is restored on the same
  tick.  The two systems never overlap by more than that one fade.
* **A load screen does not flip the decision.**  `Director::Covers()` holds its
  last answer while the world is between two cells (`loading`, `mainMenu`,
  `!attached`, gate not open yet).  Re-deriving it there would release vanilla
  every time a loading screen appears and take it back a second later - an
  audible flam at every door and every load.
* **The level is steered, never the queue.**  Patching `BSMusicManager`'s vtable
  (the 2.x "takeover" attempt) swallowed its `kAdd`/`kRemove` events, left the
  track queue pointing at freed objects and crashed with a use-after-free
  (`SkyrimSE+0x1AD5B40`, identical in two minidumps).  The plugin reads
  `AudioCategoryMUS`'s live level and writes 0 / restores it - which is also why a
  Music-slider change and the load-screen reset are picked up by themselves.
* **There is no mute mode.**  Nothing in this feature can leave the game without
  music: the off state of `bEnabled` and the "no track here" case both hand the
  place back to vanilla.

## Underwater: no music at all (2.6.5)

While the player's head is below the water surface, **no music plays** - neither
this plugin's tracks nor the vanilla layer's:

* The detection is the engine's own `Actor::BOOL_FLAGS::kUnderwater` bit (the one
  that drives the underwater muffle), read off the player each snapshot - nothing
  to configure, nothing that can drift out of sync with what is on screen.
* Both layers fade out over `fFadeHandback` (the combat gate keeps running, so a
  fight that continues at the surface picks its track back up after the usual
  arm time).
* In takeover mode the vanilla category is **held at 0, not handed back** for as
  long as the player is down there: handing back would start vanilla music
  underwater, which is precisely what this rule exists to prevent. Underwater
  ambience mods own that space. (`bKeepVanilla = 1` still never steers vanilla;
  underwater it silences only this plugin's own layer.)
* Surfacing resumes normally: pools re-resolve, the ambient track starts over
  `fFadeLocation`, the log carries one `underwater -> no music` line per dip.

## Interactive Music Radio: the handshake reader (2.6.6)

[Interactive Music Radio](https://www.prismaui.dev/) (IMR) is a separate SKSE
plugin that links an external media source — a player, a browser, a rhythm game
— and lets that audio be what the player hears. This build makes CMS read the
link, so the two actually take turns instead of talking past each other.

* **The meeting point is one shared page**, `IMR_CMS_HANDSHAKE`, 64 bytes,
  declared as `cms::ImrLink` in `core.h`. IMR writes, CMS only reads. No symbol
  is shared, no CMS code is patched, and either plugin runs fine without the
  other.
* **The heartbeat is the safety net.** A state word alone would stay at
  "takeover" forever if IMR crashed, and the game would be silent until a
  restart. CMS ignores a block whose `heartbeatMs` is older than
  `kImrHeartbeatMaxMs` (3 s), so an abnormal exit releases the music by itself.
  A heartbeat stamped *after* the current tick (a previous boot, counter reset)
  is treated as stale rather than trusted forever.
* **While the radio is on the air** both of this plugin's layers fade out over
  `fFadeHandback`, its pools are dropped, and `Covers()` keeps the vanilla music
  category at 0 — handing the context back would start Skyrim's own selection
  underneath the stream.
* **Nothing else freezes**: the world gate, `CombatGate`, the sensors and the
  rule table keep running, so signing off re-resolves the current cell / hour /
  fight on the next tick.
* One log line per transition (`IMR: radio on the air …` / `radio off the air
  …`), debug-only repeats with `bVerbose = 1`. If IMR is not installed the block
  is never opened and nothing changes.

## Combat detection

`Actor::IsInCombat()` alone is not a good trigger: it lands late, it stays on
during brawls and scripted fights, and it flips on even when the player swings at
something that never fought back. So the plugin decides with `cms::CombatGate`
(`src/core.h`, pure logic, unit tested) fed by two game events:

| event | fires when | feeds the gate as |
|---|---|---|
| `TESCombatEvent` | an actor's combat state flips | enemy and target is the player -> *someone engaged us*; the player's own flip and NPC-vs-NPC flips are ignored |
| `TESHitEvent` | a hit lands | player is the target and a cause exists -> *we were hit* (traps/falls have no cause and are ignored); cause is the player and the target is a living actor -> *the player attacked* (ore veins, dummies, corpses are not) |

The gate opens on exactly three verdicts:

| verdict | condition |
|---|---|
| **Duel** | the player damaged a living actor, that side answered within 6 s, **and the exchange kept going for 2 s after the answer** |
| **Ambush** | an enemy engaged or hit the player with no player attack in the last 6 s |
| **Sustained** | fallback: a hostile stays in combat with the player for 2.5 s with no usable event |

A swing that nobody answers never opens the gate - that was the false positive
this rebuild removes. **A fight the player started is not a combat event until it
has lasted**: a sneak attack the target dies in, or one against something that
never really fights back, ends inside the 2 s confirmation window and never
opens the gate (2.6.4). The reason is shown on the volume page and logged once
per fight:

```
combat: ON  reason=duel (player struck, target struck back)  aiFlag=1
combat: ON  reason=ambush (enemy engaged first)  aiFlag=0
combat sensor: hit on a non-actor/dead target ignored (ore vein, dummy, corpse)
```

### How a fight ends (2.4): sheathe or retreat

**Putting the weapon away ends the event**, whether the player started it or was
ambushed. It is the only signal that is both deliberate and immediate: the AI
flag lingers through looting, dialogue and lockpicking, and `fCombatHold` then
kept the combat track running on top of that.

* `ActorState::IsWeaponDrawn()` is sampled each tick. It is still `true` while
  sheathing, so a swap animation cannot end a fight early.
* The weapon has to stay down for `CombatGate::kSheatheConfirm` (0.5 s). That
  filters a weapon-swap flicker and a sheathe the game cancels because something
  is still hitting us.
* The release travels as a **timestamp** (`GameContext::combatEndAt`), not a
  one-frame flag: the engine thread samples at 20 Hz while the game ticks at 60+,
  so a pulse would simply be missed. The director drops its `fCombatHold` timer
  when that timestamp advances, and the combat layer starts its
  `fFadeCombatExit` ramp on the same tick. 2.6.4: **every** release advances it -
  sheathe, retreat and the plain timeout (reason 3) alike - and the director also
  resets its arm-time clock, so the next fight has to prove itself from zero
  again (a string of quick kills never accumulates "armed" state).
* Without either ending (a fight that ends by enemies dying) the AI flag
  clears and `fCombatHold` (2.5 s) still covers it, so nothing flaps during a
  dialogue or a short disengage.

`bSheatheEndsCombat = 0` restores the old "wait for the AI" behaviour.

### Running away ends it too (2.4.1)

Leaving the fight is a decision as well, and the AI flag cannot see it: after
the player legs it, every enemy stays `IsInCombat()` while it *searches* for a
player who is already two hills away, so the combat track used to play on for
another minute.

* The 4 Hz hostile scan now also reports the **distance to the nearest hostile
  that still has the player as a combat target** (`ScanHostileCombat`).
* `CombatGate::Update` takes that distance and closes the gate (`byFlee`) when
  it stays beyond `kFleeDistance` (3000 units ≈ 43 m) for `kFleeConfirm` (3 s).
  *nearest*, not average: one pursuer left behind keeps the track alive, so a
  dragon circling overhead or a straggler still swinging keeps the fight going.
* Any real contact resets the clock - `TESHitEvent` in either direction and a
  `TESCombatEvent` engaging us - so being sniped at from a ridge is not a
  retreat, no matter how far the shooter is.
* Weapon state is ignored on purpose: people flee with the weapon still out.
* After a retreat the gate stays `Fled()` until somebody is close again or a hit
  lands, which blocks the *prolonged fight* fallback from restarting the music
  while the same enemy is still flagged and still searching. Walking back into
  range re-arms it with a fresh 2.5 s timer.
* Log line: `combat: OFF  by=flee  aiFlag=1  weaponDrawn=1  nearest=5120` and
  `combat: player fled the fight -> event over, combat layer fades out now`.

An empty scan (nobody is fighting us at all) is passed to the gate as "very far
away", so it counts as a retreat as well; a *negative* distance means "not
measured" and disables the rule, which is what the unit tests use for the
pre-2.4.1 call pattern.


## Load safety (2.4.2)

Loading a save used to kill the process with `0xC0000409` in `ucrtbase.dll` -
a *fast fail*, which is why CrashLoggerSSE wrote nothing at all.  Two rules now
hold, and they are the reason the load path is quiet:

1. **This plugin only ever touches the game from the game thread.**  The engine
   loop used to fall back to `platform::Snapshot()` from its own thread after the
   SKSE task queue went quiet for 3 s - and a load screen is exactly when that
   queue goes quiet.  That fallback is gone: if no snapshot arrives, the music
   keeps playing from the last context (and the log says so, once per stall).
   The queued task is also no longer re-queued on a timeout, which used to leave
   a pile of tasks that all ran in one frame.
2. **Nothing is written to the game during a load.**  `SetCategoryVolume` (the
   level that holds the vanilla layer back) was the one write this plugin did
   mid-load, 20 times a second, from a worker thread while the engine was
   rebuilding its audio system.  It is skipped while `Loading Menu` / `Main Menu`
   is up.  The engine resets category levels while loading, so the level is
   re-applied on the first tick after the world is ready - `SteerVanillaMusic`
   reads the live value before writing, which is what makes that recovery
   automatic instead of a timer.

`platform::Snapshot()` also returns a bare `loading` context without reading the
player, the cell or `ProcessLists`.  A half-loaded world is exactly the state
where `IsAttached()`, high-actor lists and combat flags are between two values;
there is nothing to decide there anyway because the music gate is closed, and the
combat gate is reset so a fight from the previous world cannot leak into the new
save.

### If it ever fast-fails again

A fast fail cannot be caught, so the plugin installs a pass-through CRT
`_invalid_parameter_handler` plus a `std::terminate` handler: they write one line
and then call the original handler, which still terminates the process exactly as
before.

2.4.3 adds the **caller chain** to that line.  `ucrtbase` is shared by every
plugin in the process, so "invalid parameter" on its own proves nothing about who
called it - and the common "no info" variant passes `expression = nullptr`, which
is why the first version printed `'(null)'` and named nobody.  Each frame is now
resolved to `module+offset`, so the line points at the guilty plugin:

```
[error] CRT invalid parameter: expression='(null)' function='(null)' (null):0 tid=16756
[error]   caller chain: ucrtbase.dll+0x36af5 <- MSVCP140.dll+0x1285b <- <culprit>.dll+0x... <- ...
```

The handler is recursion-guarded (a CRT error raised by our own logging cannot
re-enter it), allocation-free apart from the log write, and forwards to the
previous handler unchanged, so this changes no crash behaviour.

## Localisation

**Two builds, one language each.  No files, no switch.**

```
build\Release\CustomMusicSystem.dll       <- Chinese UI (default)
build\Release\CustomMusicSystem_en.dll    <- English UI
```

The language is a compile-time constant (`CMS_UI_LANG_ZH` in `src/i18n.h`), so
`i18n::T()` returns a pointer straight out of one of the two built-in tables.
There is no `sLanguage` probe, no `Interface\Translations\*.txt`, no radio
button and no "applies on the next launch" note - the four things that kept the
menu in the wrong language.

| what went wrong before | why it cannot happen now |
|---|---|
| the radio only changed the config, nothing re-resolved the strings | there is nothing to resolve; the table is compiled in |
| a failed file load kept the previous override vector | no vectors, no files |
| a restart with `chinese` still showed English when only `_english.txt` was installed | there is no file to be missing |
| the framework builds its section tree once, so a renamed page kept the old label | every label, including `Volume` / `音量`, is one language in this build |

Switching versions: rename `CustomMusicSystem.dll` to `.bak`, copy
`Language-EN\CustomMusicSystem_en.dll` in as `CustomMusicSystem.dll`.  Both DLLs
must never sit in `SKSE\Plugins\` at the same time - SKSE would load both and
each would play its own music.  That is why the alternative lives in a
subdirectory (`SKSE\Plugins\Language-EN\`), which SKSE does not scan.

`CustomMusicSystemTests.exe` and `CustomMusicSystemTests_en.exe` run the same
387 assertions against both cores; the language assertions branch on
`i18n::kChineseBuild`, so a table that silently fell back to the other language
fails the build.

## Build

```
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release --parallel
# -> build\Release\CustomMusicSystem.dll      (Chinese UI)
# -> build\Release\CustomMusicSystem_en.dll   (English UI)
```

One `add_library` pair per language (see `CMakeLists.txt`), sharing every source
file except the language constant. Building both from one configure keeps the
CommonLibSSE-NG setup, the include paths and the tests identical.

`CommonLibSSE-NG` is consumed from `D:/dev/CommonLibSSE-NG` (edit the
`add_subdirectory` line if it moves). No vcpkg, no vcvars: the VS generator
resolves the toolchain itself. `D:/dev/deps` provides header-only spdlog that
CommonLibSSE needs at configure time.

## Verify without the game

```
build/Release/CustomMusicSystemTests.exe                          # 387 assertions, Chinese core
build/Release/CustomMusicSystemTests_en.exe                       # 387 assertions, English core
build/Release/CustomMusicSystemTests.exe --scan "<...>\Music"      # per-folder track census
build/Release/CustomMusicSystemTests.exe --i18n "<...>"            # dump the compiled-in table
build/Release/CustomMusicSystemPlayTest.exe <file.xwm> 4           # decodes + plays for real
```

The test binary drives both pages with a mock imgui api, so an unbalanced
`BeginChild` / `PushID` / `PushStyleColor` (an ImGui assertion = a crash in game)
fails here instead of in Skyrim. It also drives `CombatGate` through every
verdict (unanswered swing, duel, ambush, stale offense, hold, sustained fallback,
**sheathe release**, **retreat release**) and `SwitchBlockReason` through every wait reason
(settling, crossfade, min play, cooldown, nearly-over exemption).

## Log

`<game root>\CustomMusicSystem.log`, rewritten every launch. The lines that
matter:

```
ui language: zh (requested 'CHINESE', strings '...\CustomMusicSystem_chinese.txt')
mixer loaded: ...\CustomMusicSystem.mixer.txt
combat sensor: TESCombatEvent + TESHitEvent sinks installed
volume page registered in the SKSE menu framework (个性化音乐系统/音量, 个性化音乐系统/设置) wheel=on
gate: world ready ...   play  [layer=ambient key='Explore - Day|...']
context: ... combat=0 sensor=0 dlg=0 ride=0
combat: ON  reason=duel (player struck, target struck back)  aiFlag=1
handover [ambient] 2.7s before end
settings written to ...\CustomMusicSystem.ini
```
