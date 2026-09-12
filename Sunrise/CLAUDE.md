# Sunrise — Session Recap: Reconstructing "Homecoming" (Red War, mission_towerfall)

Community reimplementation of Destiny 2 (Shadowkeep-era) client/server. Goal of this
work-in-progress effort: hand-author the Red War campaign's first mission ("Homecoming",
internal codename `mission_towerfall`, Activity #266 / hash 0x62D85FB3) as a real, scripted
Lua mission, using only data legitimately extracted from the user's own local install — no
copyrighted content is stored in this repo.

**This file supersedes the previous session recap.** The previous session solved the spawn
location and got the intro directive/dialogue working from a fixed timer. This session built a
whole native trigger-detection system from scratch (the big lasting deliverable), then used it to
extend the mission, and separately found and used a much better native spawn signal
(`bootflow_step`).

## Build & deploy (always Release — Debug triggers the anti-tamper freeze)

```bash
# Compile (MSBuild, Release, x64) then deploy — game must be closed to overwrite the DLL:
"C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" \
  "H:\Random\Sunrise\Sunrise\Sunrise.vcxproj" /p:Configuration=Release /p:Platform=x64 /m:2
cp "H:\Random\Sunrise\build\x64\Release\steam_api64.dll" "E:\Sunrise\Game\bin\x64\steam_api64.dll"
```

Note the build output lands in `H:\Random\Sunrise\build\...` — **one level above** the `Sunrise`
project folder (the git repo root is `H:\Random\Sunrise`, and `Sunrise` is a subdirectory of it;
easy to get wrong, cost a failed `cp` once this session). `/m:2` (limited parallelism) avoids an
MSVC "out of heap space" (C1060) error seen with full `/m` parallelism in this environment.

The DLL masquerades as `steam_api64.dll`. Game install root: `E:\Sunrise\Game`. Sunrise's own
artifact folder (settings, scripts, logs — same folder as the DLL):
`E:\Sunrise\Game\bin\x64\Sunrise\`.

- Mission script: `...\Sunrise\scripts\mission_towerfall.lua` — edits take effect on the **next
  attach** (relaunch the activity). A "Reload script" action exists in the debug UI but was
  observed to be unreliable this session (a script reload did not pick up new event handlers
  correctly) — **relaunch the activity fully** rather than trusting Reload, at least for changes
  that add new `on_event_*` handlers.
- Log: `...\Sunrise\logs\sunrise.log` (rotates one `.old` copy, never truncated — always tail).
- Remote command file: `...\Sunrise\rt_cmd.txt` — see **RT_BRIDGE.md** (now a committed doc, not
  just this file) for the full write-up of the convention.

## Documentation now committed to the repo (not just this file)

- **[TRIGGER_WATCH.md](TRIGGER_WATCH.md)** — the `slot:watch_trigger()` / player-trigger-volume
  detection system: what problem it solves, how to use it, why the first call(s) can return
  `false`. Written for the community, not just this session.
- **[RT_BRIDGE.md](RT_BRIDGE.md)** — the `rt_cmd.txt` / `context:probe()` live-iteration
  convention, with a worked example.
- Both are at the **repo root** (`H:\Random\Sunrise\Sunrise\...`), not under `docs/` — that
  folder is `.gitignore`d in this repo (reserved for something else, presumably local generated
  docs), discovered the hard way when a first attempt to commit them there silently produced
  nothing to commit.

## Git / GitHub state

- Working fork: **https://github.com/Fozkais/Sunrise** (remote name `myfork`; `origin` remains
  `https://github.com/stanuwu/Sunrise.git`, the upstream).
- **The fork was deleted and recreated once this session.** Cause: commit messages that
  mentioned "PR #111" made GitHub permanently record a "referenced this pull request" timeline
  entry on the *upstream* PR #111 page, visible to anyone, which the user explicitly did not
  want (private experimentation only). GitHub does **not** retroactively remove that timeline
  entry even after the offending commits are rewritten/force-pushed away — the only fix found was
  deleting the fork and re-pushing a history that never contained the "#111" text in the first
  place. **Lesson: never write a literal `#<number>` referencing another repo's issue/PR in a
  commit message pushed to a fork of that repo**, even in prose ("PR #111") — GitHub's
  auto-linking does not care about phrasing, only the `#123` pattern appearing anywhere in the
  message.
- Current clean history on the fork (3 commits ahead of upstream `master`, at `66888a8`):
  1. `ef0ad59` "Merge fix for initial_state mission script propagation" — the merged (and
     locally hardened: proper mission-runtime-lock acquisition before probing) external fix that
     lets a mission script declare `initial_state = { region_index = N }` and have the client
     natively stream into that region at attach instead of the activity's default spawn.
  2. `41c906e` "Add SDK dialogue/directive text catalogs and region-arrival query" — real
     player-facing dialogue/directive text catalogs, `context:region_arrival_pending()`, the
     scene-seed roster `continue`-on-failure fix, dev-mode sandbox relaxations.
  3. `7e3abdb` "Add native player-trigger-volume detection (slot:watch_trigger)" — the trigger
     system, documented in TRIGGER_WATCH.md/RT_BRIDGE.md.
- **Currently uncommitted** on top of `7e3abdb` (this session's newest work, not yet committed —
  ask the user before committing/pushing): `bootflow_step`/`in_world` Lua exposure,
  `find_trigger_by_bubble` diagnostic tool, `watch_trigger_identity`, the `resolve_direct_target`
  extension to `player_trigger::resolve()`, and enter/exit direction reporting in
  `player_trigger_watch`. See the "New native additions this session" section below for the full
  list of touched files.
- A pre-existing, unrelated `git stash` entry (`WIP: probe tooling, catalog additions, seed
  diagnostics`) has been sitting in the stash list since before this session even started; still
  unresolved housekeeping, non-blocking, left alone again this session.
- A local `backup-before-reword` branch exists (safety net from the history-rewrite operation,
  not pushed anywhere) — fine to delete once confident the fork is in good shape.

## The trigger-detection system (this session's main deliverable)

**The problem**: the retail client never reports a local player crossing an authored type-31
trigger volume (the schema-0x8080879F / target-6685 incident `mission_script_player_trigger.cpp`
already knows how to resolve). Confirmed by temporarily logging every incoming incident's target:
only ever `1121`, never `6685`, even walking directly through a known trigger box.

**The fix, fully working**: `client::activity::player_trigger_watch` (new module) combines two
things Sunrise already had lying around unused for this purpose — the local player's live tracked
position (`client::player::position`, built for the teleport feature) and every trigger volume's
already-extracted exact world-space geometry (`state::build_data::scriptables`, the same data the
debug UI's wireframe overlay draws) — to do the containment test itself, once a frame. On a
transition it encodes and submits the *same* incident payload the retail client would have sent,
through the *same* `host::submit_incident` ingestion path a real one takes, so nothing downstream
has to know the difference. Full write-up: **TRIGGER_WATCH.md**.

Two real bugs had to be fixed to make this actually work end to end (both fixed, both left as
permanent code, not workarounds):
1. The trigger-volume geometry catalog (`state::build_data::scriptables`) is **only ever built on
   request** — normally only the debug "Scriptable Browser" panel asks for it. Nothing else in
   the mission runtime did, so it silently stayed empty. `register_trigger_watch` (server side)
   now requests it itself (idempotent) and returns `false` while it's still building; the Lua
   side must retry on a short timer rather than treat one `false` as final.
2. A synthesized incident with `sourceGeneration` left at its default `0` was **silently dropped**
   by the mission feed's own eligibility gate (`event.sourceGeneration !=
   instance.view.activityClientGeneration` in `mission_script_runtime_feed.cpp`). Fixed by
   stamping the real `activityClientGeneration` (captured at watch-registration time) onto the
   synthesized incident.

### `slot:watch_trigger()` vs `context:watch_trigger_identity()`

- `slot:watch_trigger()` — for a **named** type-31 slot (`context:slot("pt_start")
  :watch_trigger()`). Resolves its type-31→type-60 mapping via the existing incoming-reference
  join in `player_trigger::resolve()`.
- `context:watch_trigger_identity(registry_key, slot_type, slot_index)` — new this session, for
  arming by **raw wire identity** directly, needed for two cases:
  - a trigger `context:slot(...)` cannot name (an "Unnamed trigger" in the debug browser with no
    alias/tag Sunrise could recover a name for) — find its local Lua row anyway by iterating
    `context:slot(1..N)` and matching `.registry_key/.slot_type/.slot_index` (works fine, just
    slow-ish; ~1900 iterations took a moment but completed within the raised instruction budget);
  - a type-60 target volume that has **no type-31 source at all** in the extracted data (see
    below) — pass `slot_type = 60` directly instead of `31`, naming the target table itself.
    `player_trigger::resolve()` was extended with a `resolve_direct_target` branch: when the
    payload's `slotType == 60`, it looks up the `TriggerVolumeTable` directly by
    `(registryKey, slotIndex)` instead of requiring an incoming type-31 reference. This only ever
    runs for our own client-synthesized payloads (a real wire incident from the retail client
    always names a genuine type-31 source), so it changes nothing for the real path.

### `context:find_trigger_by_bubble(bubble_index, visible_index)` — new diagnostic tool

Resolves the type-31 source (or, always, the type-60 table identity) of the Nth trigger row the
debug "Scriptable Browser" trigger-volume panel would list for one bubble filter value, by
replicating that panel's own row-enumeration code
(`activity_host_trigger_volumes.cpp::materialize`) natively, since `state::build_data::scriptables`
is not reachable from mission-script Lua directly. Assumes no text/scope filter is active in the
panel (only the bubble filter). Returns `ok, registry_key, slot_type, slot_index, match_count,
total_rows, table_registry_key, table_slot_type, table_slot_index` — `total_rows` tells you
whether the requested index even exists for that bubble; the trailing three `table_*` values are
the row's own type-60 identity, always populated once the row is found, independent of whether it
has exactly one (or any) type-31 source.

**Bubble index 9 = underwatch** (the bubble's own ordinal from the debug UI's Manual-Mode
dropdown — *not* the `region_index`/slice-set value `72` used for `initial_state`; these are two
completely different, easily-confused number spaces, a mistake already made and fixed once last
session).

Used this session to identify a real, drawable "Unnamed trigger" (row 65 of bubble 9) that turned
out to have **zero** incoming type-31 references anywhere in the extracted data (confirmed by
sweeping every plausible type-31 slot index, 0–300, on its owning object via
`watch_trigger_identity` and finding only the two already-known ones). Type-30 "occupancy" was
also tried against it (borrowing an existing occupancy slot, `pm_vo_nudge`, redirected via
`slot:set_occupancy_condition`) and **also produced no event** — consistent with this session's
earlier finding for `pt_start` that type-30 occupancy just does not reflect player spatial
presence at all, for any trigger, and should be considered a dead end for this purpose generally.
The `resolve_direct_target` extension above is what actually made this trigger usable.

### Enter/exit direction

The real wire schema has no enter/exit bit — the retail client, on the rare occasions its own
native mechanism might fire, apparently only ever reports one direction. Since our own system
fully controls both ends of the synthetic round trip, `resolvedObjectId` (decoded but never read
by `resolve()`) now carries it: `0` = entered, `1` = exited. Exposed to Lua for free, since
`event.resolved_object_id` was already a field on `on_event_player_trigger`. `player_trigger_watch`
now reports **both** directions (previously entry-only).

## Spawn-signal investigation: what actually happens between attach and control

The original ask was "what client state changes happen between activity launch and the player
really spawning" (with half a hope one of them lines up with the loading-screen fade-out). Three
signals were tried and measured, in order of increasing accuracy, all confirmed via real timed
log data from actual test runs (not guessed):

1. **`context:region_arrival_pending()`** (from last session) — clears in **~500ms**. Reflects
   only "the server-side region/slice-set selection resolved", not anything about the client
   having actually streamed in or the audio subsystem being ready. Firing dialogue on this alone
   is why last session's naive attempt looked "unreliable" — it wasn't flaky, it was just too
   early combined with too short a buffer afterward.
2. **`context:player_position_present()`** (new this session) — exposes
   `client::player::position::snapshot().present` (whether the physics hook has found a real
   local-player body to read). Cleared at **~18-19s** after attach in test runs — a real, working
   spawn signal (used successfully to gate the Cue-1/squad-spawn logic for a while), but not the
   earliest available one, and not derived from a named client-engine state.
3. **`context:bootflow_step()` reaching 38 (`activity:in_world`)** (new this session, currently
   used) — exposes the client's own already-hooked boot-flow step accessor
   (`client::hooks::bootflow::raw_step()`, previously used only by a debug HUD overlay, never
   exposed to mission-script Lua). Measured **~2.8s earlier** than `player_position_present` in a
   back-to-back comparison on the same run, and unlike the other two, it is the client engine's
   own named state (`activity:in_world`), not something Sunrise derives. **This is what the
   mission script fires Cue 1 / the two "fake fight" squads on now.**

Real measured boot-flow sequence from one test run (`bootflow_step`, each value held for several
seconds — looks like a genuine authored loading/spawn choreography, not noise):
`33 → 34 → (35, never observed at 300ms poll granularity) → 36 → 37 → 38`. Only `38` is named
(`activity:in_world`, `kInWorld` constant in `world_step.cpp`); `33/34/36/37` are real, distinct,
reproducible values but their meaning is **not yet identified** — a good next lead if a still
earlier/more precise signal is ever needed (a step transition might line up with the black-screen
fade-out specifically).

Also checked and ruled out as *automatic* spawn signals (both real, both fire, neither useful for
this):
- **`on_event_phase_entered`** — purely reactive to the script's own `context:set_phase()` calls.
  Cannot ever tell you about something you don't already know, by construction.
- **`on_event_cinematic_terminated` / `on_event_cinematic_started`** — real native incident path
  (schema 0x808087BF), needs no arming, but never fired in any test this session. Consistent with
  the already-known permanently-missing intro-cinematic scene-seed resource (object 449 — see
  below); most likely this is exactly the cinematic that never materializes.
- **`on_event_player_trigger`** — reliable (it's our own system), but structurally **cannot** be
  an earlier signal than `player_position_present`, since our own containment test itself depends
  on a live player position existing to test in the first place. At best equal, realistically
  later (walking to a box takes time).

### Dialogue timing (measured, not guessed)

Firing `slot(3):play_dialogue_cue{cue=1}` (row 3, `m_dialog_sensor`) has a real, **reproducible**
gap before the line is actually audible, confirmed by stopwatching a fresh rt_cmd-fired cue
several times: **~5-6 seconds of latency**, then **~4 seconds** of actual speech. This is not
random/load-dependent (it reproduced identically whether fired right after a fresh relaunch or
long after the world had settled), so the working theory is either a fixed engine
cue-activation latency or (more likely) real authored lead-in silence baked into the clip itself,
originally meant to be absorbed by the walk from spawn to the trigger box in the real game. The
mission script now models this explicitly as two chained timers
(`DIALOGUE_START_LATENCY_MS = 6000`, `DIALOGUE_SPEECH_MS = 4000`) before firing the
"Defend your home" objective, rather than one guessed buffer.

## Homecoming/mission_towerfall: current script behavior (fully working, tested)

Sequence, in the order it actually happens:
1. `on_start`: arms every entry in `watches` (retries every 500ms until the scriptables catalog
   is ready and all are armed — see above); starts the poll loop.
2. `bootflow_step` reaches 38 → spawns `sq_frame_fake_fight` and `sq_red_guard_fake_fight`
   (`retire_on_return=true`), fires dialogue Cue 1, starts the 6s+4s timer chain.
3. Timer chain elapses → fires the "Defend your home" HUD directive
   (`slot_row=17312, name_hash=0x4FCECAB6, element=0`, the real recovered Bungie text).
4. Player enters `pt_sc_underwatch_intro_stand` (named type-31 trigger, armed via
   `slot:watch_trigger()`) → **retires** `sq_frame_fake_fight`/`sq_red_guard_fake_fight` (see
   "retiring a squad" below), **spawns** `squad_first_contact_cabal` and
   `squad_first_contact_cabal_backup_a`, fires the `d_underwatch_collapsing_wall` device
   (unchanged from last session), fires dialogue Cue 5.
5. Player **exits** `underwatch_start_zone` (the unnamed, no-type-31-source trigger, armed via
   `watch_trigger_identity(2418521761, 60, 23)`) → fires dialogue Cue 6.

`pt_start` (the very first trigger discovered last session) is still armed and logged but no
longer drives anything — superseded by the `bootflow_step`-based spawn signal for Cue 1.

### Retiring a squad (no dedicated "despawn" call exists)

There is no native "remove this squad now" entry point. The working pattern found this session:
get the squad's own count vector (`squad:counts()`), set every member's count to `0`
(`counts:set(i, 0)`), then `squad:place{counts = counts, mode = context.sdk.squad_modes.replace}`.
`retire_on_return` (used when spawning) is a completely different, authored "despawn when they
return home" condition — not something a script can trigger directly.

### Real dialogue cues recovered/used this session (slot row 3, `m_dialog_sensor`, same as Cue 1)

Cue 1 (intro line), Cue 5 (first-contact/Cabal reveal), Cue 6 (after leaving the start zone) are
all confirmed to accept (`transport_staged`) on slot row 3. Not yet verified whether every cue
in the mission lives on this one dialogue sensor or whether later beats need a different slot —
worth checking before assuming.

## New native additions this session (all under `sunrise::`)

Committed (in `7e3abdb`, see TRIGGER_WATCH.md/RT_BRIDGE.md for the user-facing side):
- `src/client/activity/player_trigger_watch.h`/`.cpp` — the trigger-detection module.
- `src/middleware/bap/activity_message/player_trigger_incident.h`/
  `activity_player_trigger_incident_codec.cpp` — added `encode()` (mirrors the existing
  `decode()`).
- Lua wiring: `mission_script_lua_slot_api.cpp` (`slot_watch_trigger`), `mission_script_vm.h`
  (`RegisterTriggerWatch` in `DefinitionApi`), `mission_script_sdk_bridge.cpp`
  (`register_trigger_watch`), `mission_script_runtime.cpp` (`clear_watches` on real instance
  close), `mission_script_runtime_attach.cpp` (finishes last session's PR-based `initial_state`
  hardening — the lock-acquiring public wrapper).

**Not yet committed** (ask before committing — this is fresh, untested-beyond-this-session's-manual-runs):
- `src/client/hooks/bootflow/bootflow_hook_lifecycle.h`/`world_step.cpp` — added `raw_step()`
  (the boot-flow step was already tracked for the HUD overlay; this just exposes the raw value
  publicly instead of only the derived `in_world()` boolean).
- `mission_script_vm.h` / `mission_script_sdk_bridge.cpp` / `mission_script_lua_context_api.cpp` /
  `mission_script_lua_internal.h`:
  - `context:player_position_present()` — exposes `client::player::position::snapshot().present`.
  - `context:bootflow_step()` — exposes the raw boot-flow step integer.
  - `context:find_trigger_by_bubble(bubble_index, visible_index)` — the debug-browser-row
    lookup tool described above.
  - `context:watch_trigger_identity(registry_key, slot_type, slot_index)` — arms a watch by raw
    identity instead of a resolved Lua slot handle.
- `mission_script_player_trigger.cpp` — added `resolve_direct_target` and the `slotType == 60`
  branch in `resolve()`.
- `mission_script_lua_slot_api.cpp` — `slot_watch_trigger` now returns 4 values (armed +
  resolved volume identity) instead of just a boolean, so a script can tell watched triggers
  apart without hardcoding values read off one test run.
- `player_trigger_watch.h`/`.cpp` — enter/exit direction reporting (`resolvedObjectId` reused as
  the flag).

## Housekeeping / known temporary state

- Dev-mode sandbox relaxations still active (from last session, unchanged): instruction budget
  50,000,000 (raised from 100,000 — needed this session too, e.g. the ~1900-iteration local-slot
  sweep to find an unnamed trigger's Lua row); `next`/`pairs`/`load` and the `string` pattern
  functions restored. Revert before treating any of this as final/shippable.
- Temporary diagnostics still in the mission script: `on_event_cinematic_started/terminated`
  probes (harmless, currently never fire; remove once the cinematic question is settled for
  good), the `bootflow_step`/`player_position_present` transition tracing in `poll()` (useful,
  arguably worth keeping longer-term rather than stripping).
- The `pm_vo_nudge` occupancy slot was redirected via rt_cmd during the type-30 investigation
  (`filter = context:slot(1897), value = 1`) and never reset — harmless (nothing reads it), but
  worth remembering it's not in its original authored state if that slot matters later.
- Before treating any of this as "shippable": revert the sandbox relaxations, decide what to do
  with the temporary probes above, and double check whether `context:find_trigger_by_bubble`/
  `watch_trigger_identity` should stay as permanent community-facing API (they're currently
  documented in code as diagnostic tools, not polished ones) or get a cleaner public wrapper.

## Lua mission-script API reference (cumulative, updated this session)

New/changed this session (see previous recap, preserved in git history, for the fuller
pre-existing reference — `context.sdk.catalog.*`/`context.sdk.world.*`, `context:squad/slot/scene`,
`squad:place`, `slot:set_directive`, `slot:play_dialogue_cue`, `slot:set_channel`,
`on_event_effect_result`, `on_event_timer_elapsed`, etc.):

- `context:player_position_present()` → bool. See spawn-signal section above.
- `context:bootflow_step()` → integer (`-1` if unavailable). Only `38` is named
  (`activity:in_world`).
- `context:find_trigger_by_bubble(bubble_index, visible_index)` → `ok, registry_key, slot_type,
  slot_index, match_count, total_rows, table_registry_key, table_slot_type, table_slot_index`.
  Diagnostic tool, mirrors the debug "Scriptable Browser" trigger panel's own row order.
- `context:watch_trigger_identity(registry_key, slot_type, slot_index)` → `armed,
  volume_registry_key, volume_slot_type, volume_slot_index` (last three only when armed). Same
  underlying mechanism as `slot:watch_trigger()`, addressed by raw identity.
- `slot:watch_trigger()` → now returns `armed, volume_registry_key, volume_slot_type,
  volume_slot_index` (was boolean-only last session).
- `squad:counts()` → a mutable count-vector handle (`.count`, `.at(member)`, `.set(member,
  value)`) seeded with the squad's default counts; `squad:place{counts=..., mode=...}` is how you
  actually change placement. `context.sdk.squad_modes.{reinforce,replace,reserve}`.
- `on_event_player_trigger`'s `event.resolved_object_id` is `0`/`1` (entered/exited) for our own
  synthesized crossings (see "Enter/exit direction" above) — was previously unused/always `0`.
