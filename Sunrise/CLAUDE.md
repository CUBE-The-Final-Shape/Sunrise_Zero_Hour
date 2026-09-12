# Sunrise — Session Recap: Reconstructing "Homecoming" (Red War, mission_towerfall)

Community reimplementation of Destiny 2 (Shadowkeep-era) client/server. Goal of this
work-in-progress effort: hand-author the Red War campaign's first mission ("Homecoming",
internal codename `mission_towerfall`, Activity #266 / hash 0x62D85FB3) as a real, scripted
Lua mission, using only data legitimately extracted from the user's own local install — no
copyrighted content is stored in this repo.

## Build & deploy (always Release — Debug triggers the anti-tamper freeze)

```bash
# Compile (MSBuild, Release, x64) then deploy — game must be closed to overwrite the DLL:
cp "H:\Random\Sunrise\Sunrise\build\x64\Release\steam_api64.dll" "E:\Sunrise\Game\bin\x64\steam_api64.dll"
```

The DLL masquerades as `steam_api64.dll` (classic Steam-proxy injection). Game install root:
`E:\Sunrise\Game`. Sunrise's own artifact folder (settings, scripts, logs — same folder as the
DLL): `E:\Sunrise\Game\bin\x64\Sunrise\`.

- Mission script: `E:\Sunrise\Game\bin\x64\Sunrise\scripts\mission_towerfall.lua` — edits take
  effect on the **next attach** (relaunch the activity, or "Reload script" in the debug UI's
  Script tab). No recompile needed for `.lua`-only changes.
- Settings: `...\Sunrise\settings.json` — created automatically on first run if missing. Add
  `"logging": {"file_sink": true}` to get a persistent log file (off by default).
- Log: `...\Sunrise\logs\sunrise.log` (rotates one `.old` copy, never truncated — always tail).
- Remote command file: `...\Sunrise\rt_cmd.txt` (see "rt_bridge" below).

## The rt_bridge: live iteration without relaunching

`mission_towerfall.lua` runs a small poller (`context:poll_command()`, a native addition) every
300ms. Drop a Lua snippet into `rt_cmd.txt`; it's consumed (read + deleted) and executed via
`load()` + `pcall`, receiving `context, state` through `...`. Its return value / error is logged
via `context:probe(text)` as `ev=script_probe ... text=cmd result ok=... value=...`. This is how
almost all of this session's testing happened — write the command file, wait ~1-3s, grep the log.

Convention for every rt_cmd.txt payload:
```lua
local context, state = ...
-- ... do stuff, return a short status string ...
return "some result"
```

**Caveat**: two mission-script instances always attach per session (probably public+private
binding) — expect every probe/log line to appear twice.

## Dev-mode sandbox relaxations (TEMPORARY — revert before treating any script as "final")

- `mission_script_vm.h`: `kInstructionBudget` raised from 100,000 to **50,000,000** (500x).
- `mission_script_lua_sandbox.cpp`: `next`, `pairs`, `load` removed from the disabled-globals
  list; `find`, `match`, `gmatch`, `gsub` un-disabled on the `string` library.

## New native (C++) additions this session, all currently uncommitted on `master`

- `src/client/diagnostics/activity_name_probe.cpp/h` — dumps activity `internalName` matches by
  keyword (superseded by better discovery methods, but harmless to keep).
- `src/server/activity/mission/mission_script_lua_probe_api.cpp` — `context:probe(text)` (free
  text → log) and `context:poll_command()` (the rt_bridge foundation).
- `mission_script_catalog_sdk.h` / `_bridge.cpp` / `lua_catalog_api.cpp` — added
  `directive_elements` and `dialogue_cue_texts` catalog collections (real display **text**, not
  just internal codenames — mirrors what the in-game "SDK World" debug panel shows).
- `mission_script_lua_context_api.cpp` + `mission_script_vm.h` + `mission_script_sdk_bridge.cpp`
  — added `context:region_arrival_pending()`, a native query (via
  `activity_sdk_mission::query(...).regionArrivalPending`) telling a script whether the client
  has finished streaming into the region selected by `initial_state`/`select_state`. Solves "the
  world data is ready instantly server-side but the client is still loading" for **world/HUD**
  effects (confirmed: directives fire correctly right after this clears). Does **not** cover
  audio/dialogue readiness (see open problem below).
- `activity_mission_seed_roster.cpp` — added `scene_seed_diag` diagnostic logging at every
  refusal point in `collect_scene_seeds`/`validate_scene_targets`, **and** merged PR #111's fix
  (a scene seed that fails to materialize now `continue`s instead of refusing the whole roster).
- Merged **PR #111** (github.com/stanuwu/Sunrise, branch `mission-scripting-initial-state-fix`,
  author ngws): adds declarative `initial_state = { region_index = N }` support in a mission
  script's returned table, probed via a disposable Lua VM (`probe_initial_state_region`) before
  full attach, applied via `apply_script_initial_state_override`. **We hardened it further**:
  moved the real logic to `apply_script_initial_state_override_impl` (internal) and added a
  proper lock-acquiring public wrapper in `mission_script_runtime.cpp` (the original PR version
  ran without the mission runtime lock other public entry points always hold — a real, if
  narrow, race the PR's own reviewer flagged and the author had not yet fixed upstream). Also
  added `initial_state_probe` log lines. **Known still-open PR review issues we did NOT fix**:
  activity lookup skips some validation; a seed-change can publish a partial roster; general
  code-style/doxygen nits. The PR remains unmerged/imperfect upstream as of this session.
- `activity_message_framing.cpp` — added temporary `incident_diag` logging of every incoming
  incident's `primaryTarget`/`payloadLength` (used to prove the client never sends the
  player-trigger incident — see below). **Remove or gate this before it's "clean".**

## Homecoming/mission_towerfall: what's confirmed working

- **Correct spawn point**: script declares `initial_state = { region_index = 72 }` (72 = the
  `slice_set_index` for the "underwatch" bubble — found via `catalog.states`, NOT the bubble's
  own ordinal 9 from the Manual-Mode dropdown; using 9 as region_index causes an unrecoverable
  infinite load requiring a full game restart — a mistake made once, don't repeat it). Combined
  with the PR #111 fix, the client now natively streams into the true mission start instead of
  the default "boulevard"/Bazaar-adjacent spawn.
- Variant **#265** (hash 0xF07A75D3) is a dead end: "No direct destination", cannot be launched
  by any method. Ignore it.
- "boulevard" (default spawn) and "underwatch" (true start) are **the same loaded
  bubble/session** — walking between them (even via noclip) never triggers a new attach; the
  squad/slot lists are identical either way. Don't waste time hunting for a second bubble.
- The entrance wall device `d_underwatch_collapsing_wall` (type-23) breaks on:
  `context:slot("d_underwatch_collapsing_wall"):set_channel{channel=context.sdk.device_channels.position, value=context.sdk.unit(1.0), snap=true}`
  — confirmed visually (wall breaks, path opens).
  - `channel`/`value` **must** be constructed via `context.sdk.device_channels.<name>` and
    `context.sdk.unit(N)` — raw numbers error with "value must be a sunrise.sdk.unit_scalar" etc.
- Combat squad `sq_bazaar_finale` (local squad row 1, member_count=2) = a real Legionary +
  Honored Centurion, spawned via `context:squad(1):place{retire_on_return=true}`. **Always** pass
  `retire_on_return=true` — the default (false) causes an infinite respawn loop (confirmed).
- HUD objective "Defend your home" (real Bungie text) shows via:
  `context:slot(1):set_directive{directive={slot_row=17312, name_hash=0x4FCECAB6, element=0}}`
  — local slot row 1 is the activity's main directive slot (object id `slot/80b50913/...`).
  `slot_row` must be the **global** `catalog.slots` row (0-based, matches the in-game SDK World
  debug panel's "slot NNNNN" display) — found by matching `slot.id` between the local
  (`context:slot(N)`) and global (`catalog.slots:at(N)`) representations. It is **not**
  `slot.slot_index` (an unrelated authored field, reads 0 for this slot) and not the local row.
- Dialogue: local slot row 3 (`m_dialog_sensor`, type-53, same object as the directive slot),
  cue 1 = the intro line. `context:slot(3):play_dialogue_cue{cue=1}` reliably reports
  `outcome=transport_staged` (accepted, not refused) via `on_event_effect_result`, but **does
  not reliably play audibly** right when fired automatically — see open problem below. Firing it
  manually via rt_cmd well after the world has settled for a while **does** play audibly, so the
  native path itself works; it's purely a "how soon after arrival is it safe" question.
- Real directive texts + hashes recovered (SDK World debug panel → Directives tab, cross-checked
  via the new `catalog.directive_elements` collection), all on `slot 17312` unless noted:
  - "Defend your home" — `0x4FCECAB6`, element 0 (confirmed working)
  - "Find Zavala" — `0x432D2C95` / `0x432D2C96`
  - "Fight with Zavala" — `0x41222C61`
  - "Reach the shield generator" — `0x57395492`
  - "Board the command ship" — `0x5DC9D705`
  - "Gear up for the fight" — `0xB85090A0`
  - "Defend the Tower" — hash not yet captured
- The 9 squads authored for this shared bubble: `sq_bazaar_finale`(m=2, row1, confirmed = the
  Cabal pair above), `sq_bazaar_a_a`(m=4), `sq_bazaar_a_b`(m=2), `sq_bazaar_a_c`(m=3),
  `sq_bazaar_start`(m=1), `sq_bazaar_tease_a`(m=1), `sq_bazaar_tease_b`(m=1), `sq_flame`(m=1),
  `sq_ikora`(m=1). None yet tested besides row 1.
- One scene-seed resource is permanently missing in our extracted SDK (object global index 449,
  `AuthoredSceneSeedStatus::missingResource`) — almost certainly the actual intro cinematic
  (Voyager/Cabal attack), likely because that content used the separate, since-removed
  "ember_movies" bootflow movie-playback system rather than the generic in-engine scene system.
  Now gracefully skipped (PR #111 fix) instead of blocking the whole roster. Not pursued further
  — rebuilding that stripped system would be a much bigger, separate undertaking.

## Open problem being actively investigated: player-trigger volumes don't fire

Homecoming's design leans heavily on trigger-volume enter/exit (confirmed by user reviewing
period footage), so this blocks a large share of the remaining reconstruction work.

**Confirmed dead ends (do not repeat)**:
- `on_event_player_trigger` (native incident path: client detects a type-31/type-60 volume
  crossing → sends a BAP incident, target=6685, schema 0x8080879F, decoded in
  `player_trigger_incident.h`, resolved server-side in `mission_script_player_trigger.cpp`
  against `state::build_data::scriptables::Snapshot`) — **the client never sends this incident**,
  proven by temporarily logging every incoming incident's `primaryTarget`
  (`activity_message_framing.cpp`, `incident_diag`): only target=1121 ever arrives, never 6685,
  even walking directly through the real trigger `pt_start` (confirmed present in the in-game
  debug UI's Triggers tab, index 239/246, type 31, object_tag 2159351422; its type-60 pair
  `tv_start`, same object tag, resolves fine as an SDK-catalog slot but does **not** appear as a
  live/physical entry in that same Triggers tab — likely just routing metadata, not a separate
  physical volume).
- `on_event_trigger_entered`/`on_event_trigger_exited` (sense-derived, via type-30 "occupancy"
  slots and `slot:set_occupancy_condition{filter=<slot>, value=N}`, native code in
  `mission_script_runtime_edges.cpp::push_trigger_edges`) — configured an existing occupancy slot
  (`pm_vo_nudge`, borrowed only for this test, **not a clean final solution** — it has its own
  real later purpose) to watch first `tv_start` then `pt_start` as `filter`; neither produced any
  event even walking in and out repeatedly. The other 7 existing occupancy slots in this activity
  are named like `pm_zavala_dead_player_monitor`, `pm_reactor_1`, `pm_fireworks` — naming strongly
  suggests type-30 "occupancy" is for **NPC/object state monitoring** (is X dead/active), not
  player spatial presence. Likely the wrong mechanism entirely for this use case.
- `context:restart_checkpoint{region=72, spawn_set_hash=0x4CF73872}` (0x4CF73872 = underwatch's
  `state_hash` from `catalog.states`) — accepts (`ok=true`), zero observable effect whether called
  mid-session or in `on_start`. Probably needs a genuine "spawn_set" hash from a different,
  not-yet-explored catalog, not the state's own `state_hash`. Not pursued further yet.

**Most promising lead, not yet followed to a conclusion**: `activity_sdk_policy_inventory.cpp:24-25`
(client-side, `src/client/content/activity/`) —
```cpp
/** Slot type 31 contributes the currently unimplemented trigger adapter operation. */
constexpr std::uint32_t kTriggerSlotType = 31;
```
This file builds a per-slot-type "capability" inventory as part of Sunrise's own SDK
generation/extraction pipeline (used later for generation-time validation). For type-31, it
only ever builds a `"trigger.pulse"` capability (the manual fire action, confirmed working) —
never anything detection-related, unlike devices which get three capabilities. This strongly
suggests Sunrise's SDK generation pipeline simply never extracts/builds whatever data the
retail client needs in order to *know* it should watch a given trigger volume and report
crossings — i.e. the gap is in **SDK generation/extraction**, not a missing client-side hook and
not something fixable purely from mission-script Lua. Relevant files for the next investigation
pass: `activity_sdk_generation_worker.cpp`, `activity_sdk_native_pack_pipeline.cpp`,
`activity_sdk_policy_inventory.cpp` (+ `_capabilities.cpp`/`_internal.h`),
`activity_sdk_policy_input_adapter.cpp/h`, `activity_sdk_policy_input_validation.cpp`,
`activity_sdk_pack_composer.cpp`/`_rows.cpp` (all under `src/client/content/activity/`). This is
where the next session should resume.

**Explicitly out of scope / do not do**: real reverse-engineering of the retail client binary
(disassembly, memory hooking to force native behavior). The user has a separate, RE-focused
sibling project (`H:\CLion Project\Sunrise`, tooling: `runtime_watch`, SunriseStudio,
`ai_bridge_controller.py`) that is deliberately **not** reused here beyond inspiration — this
project's approach stays at the server/mission-script/SDK-generation level.

## Lua mission-script API reference gathered this session

- `context:probe(text)`, `context:poll_command()`, `context:region_arrival_pending()` — our own
  additions (see above).
- `context.sdk.catalog.<name>` — **global**, unscoped tables spanning the whole game (huge:
  squads ≈275k, texts/slots ≈175-262k rows). Has `.count` and `:at(row)` (**1-based**). Known
  collections: `activities, scenarios, bubbles, states, objects, occurrences, slots, texts,
  capabilities, gates, refusals, actor_classes, rsat_descriptors, rsat_schemas, rsat_fields,
  squads, squad_members, squad_anchors, authored_scene_resources, authored_scene_squad_edges,
  activity_binding_locators, directive_elements, dialogue_cue_texts` (last two added this
  session). `catalog.texts` values are internal script/object codenames (e.g. `sq_zavala`,
  `pt_towerfall_military_500`), **not** player-facing display strings — those live in
  `directive_elements`/`dialogue_cue_texts` (real `.title`/`.description`/`.text` fields) instead.
- `context.sdk.world.<name>` — **activity-scoped** collections (small, real counts, e.g.
  `squad_anchors` = 265 for this activity). Different row-numbering space than `catalog.*`.
  Known: `bubbles, states, objects, slots, descriptors, embedded_placement*, typed_references,
  authored_placements, container_placement*, type23_placement*, static_spatial_*,
  trigger_volume_tables/owners/incoming_references/volumes/vertices/triangles, names, tag_names,
  *_candidates, authored_squad_config/placement/point/edge_contexts, squad_anchors` (via
  `context.sdk.squad_anchors`/`context.sdk.world`, exposed at the activity-table level, not
  nested — see `world_api::push_activity_member`).
- `context:squad(row_or_id)`, `context:slot(row_or_id)`, `context:scene(row_or_id)` — resolve by
  **1-based local row** (this activity's own compact catalog, ~2217 slots for Homecoming) or by
  exact string id/name (e.g. `context:slot("pt_start")` works even when the row isn't found by
  a bulk 1..2217 scan — some named slots sit outside that contiguous range).
  `slot.name`/`squad.name` = readable internal codename; `slot.id`/`squad.id` = an opaque hash
  path (e.g. `slot/80b50913/000002/0002/0035`) — useful for matching the same object across the
  local and global (`catalog.*`) representations.
- `squad:place{counts=?, mode=?, retire_on_return=bool}` — **always** `retire_on_return=true`.
- `slot:set_directive{directive={slot_row=<GLOBAL catalog.slots row, 0-based>, name_hash=N,
  element=N}}` — type-68 slots only. Get the global row by matching `slot.id` between
  `context:slot(N)` (local) and `context.sdk.catalog.slots:at(N)` (global, 1-based — subtract 1
  for the 0-based value this call actually wants).
- `slot:play_dialogue_cue{cue=N}` — type-53 only.
- `slot:set_channel{channel=context.sdk.device_channels.<name>, value=context.sdk.unit(N),
  snap=bool}` / `slot:transition{transition=context.sdk.device_transitions.<name>, snap=bool}` —
  type-23 devices only; channel/value/transition must be constructed through those two
  `context.sdk.*` factories, never raw numbers/strings.
- `slot:fire_trigger()` — type-31, no parameters, "runs the authored pulse" (confirmed to accept
  and transport_stage, but note the whole open problem above concerns the *passive* detection
  side, not this manual fire capability).
- `slot:set_occupancy_condition{filter=<slot>, value=N}` — type-30 only; likely for NPC/object
  state monitoring, not confirmed useful for player-presence detection (see open problem).
- `context:select_state{region_index=N}` — switches which region's **data** is "current" for
  scripting; does **not** teleport the player. Real relocation needs `initial_state` (at attach)
  or possibly `restart_checkpoint` (tried, inconclusive).
- `context:restart_checkpoint{region=N, spawn_set_hash=H}` — "arms a hard wipe"; tried, no
  observable effect with the state's own `state_hash` as the spawn_set_hash.
- `on_event_effect_result` → `event.request_key, event.effect, event.outcome
  ("transport_staged"/"refused"/"expired"/"canceled"), event.outcome_code` — the **only** way to
  learn whether a fire-and-forget `queue_intent`-backed call (squad:place, set_directive,
  play_dialogue_cue, fire_trigger, set_channel, ...) was actually accepted natively; the Lua call
  itself only validates argument shape.
- `on_event_timer_elapsed` → `event.timer_name` (not `event.timer`).
- `initial_state = { region_index = N }` — top-level field in the script's returned table
  (sibling to `on_start`/`on_event_*`), read once via a disposable Lua VM before full attach.
- `load()` is available (dev-mode sandbox relaxation) — this is what makes the rt_bridge work.

## Housekeeping notes for next session

- Working tree on `master` has a merge commit (PR #111) plus a large stack of **uncommitted**
  changes (all the additions listed above). Nothing has been committed fresh by this session
  yet — ask the user before committing/pushing anything.
- There may still be a `git stash` entry left over from resolving the PR #111 merge conflict
  (attempted `git stash drop` was blocked by the auto-mode classifier as a sensitive git op) —
  check `git stash list` and clean up if it's no longer needed.
- A `pr-111` local branch/ref exists (fetched via `refs/pull/111/head`) — fine to leave or delete.
- Before treating any of this as "shippable"/final: revert the dev-mode sandbox relaxations
  (instruction budget, `next`/`pairs`/`load`, `find`/`match`/`gmatch`/`gsub`), and decide what to
  do with the temporary diagnostics (`incident_diag`, `scene_seed_diag`, `activity_name_probe`).
