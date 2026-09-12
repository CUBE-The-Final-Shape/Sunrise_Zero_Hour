# Sunrise — Session Recap: Homecoming Underwatch complete, Military hangar under way

Community reimplementation of Destiny 2 (Shadowkeep-era) client/server. Goal of this
work-in-progress effort: hand-author the Red War campaign's first mission ("Homecoming",
internal codename `mission_towerfall`, Activity #266 / hash 0x62D85FB3) as a real, scripted
Lua mission, using only data legitimately extracted from the user's own local install — no
copyrighted content is stored in this repo.

**This file supersedes the previous recaps and most of
[SCENE-EVENTS-UPSTREAM-FINDINGS.md](SCENE-EVENTS-UPSTREAM-FINDINGS.md).** That file's wire
protocol section (byte-exact `scene_events::encode()`, zero dependencies, constant generation) and
its "keys are inline at marker+12" finding still hold; its plan and its "which record is the
graph" question are answered below. Everything in this session was established by 13 live runs
on 2026-09-12, each observed by the user in-game and correlated with `script_probe` log lines.

## Where Homecoming stands (both authored scenes now play)

1. `on_start`: arms every entry in `watches`; starts the poll loop.
2. `bootflow_step` reaches 38 → fake-fight squads, dialogue Cue 1, timer chain, "Defend your
   home" directive; also discovers the scene event keys (`find_event_gate_keys`) and lists the
   Centurion's actor states (`squad_state_names`) — both logged as probes.
3. **Wall beat (`pt_sc_underwatch_intro_stand`) — confirmed "perfect" by the user:** retire the
   fake fight → `set_scene_events{generation=1, events={}}` → 1s → `scene:activate{}` + place
   `squad_first_contact_cabal` → 250ms → the 5 event keys + `set_channel(d_underwatch_collapsing_wall)`
   in the same tick. The authored explosion VFX (driven by a scene key) and the wall device open
   together, the Cabal is already behind the wall and attacks. `backup_a` and Cue 5 as before.
4. `underwatch_start_zone` exit → Cue 6.
5. `pt_centurion_intro` → place `sq_frame`, `sq_centurion_intro_backup`, `sq_centurion_intro_rush`
   (reinforcements spawn with the Frame, as in the shipped beat), and **create the Centurion actor
   with a type-2 action program in a static pose** (see below).
6. **Centurion beat (unnamed trigger idx 281 = bubble 9 index 39) — confirmed "exactly the shipped
   behaviour":** `set_scene_events{}` → `activate{}` → 3 keys, all synchronous on entry. The scene
   claims the posed Centurion, teleports it behind the left wall, walks it in and plays the synced
   impale with `sq_frame`, then the Centurion returns to combat AI.
7. `pt_centurion_intro_reinforce` → Cue 10, and the Shaxx door is shut (`o_shaxx_door_enter`
   `set_object_active{active=true}` — the closed door is an object that is absent by default).
8. **Cayde golden-gun beat (`pt_shaxx_enters`, volume 320) — confirmed complete:** bind →
   activate → the 17 keys of graph `0x80C3DEF5`, nothing else. The scene spawns Cayde and the
   three Legionaries itself, opens the door, plays Cues 12/13 and 15/16 itself, and never reports
   `scene_finished`; the "Find Zavala" directive (`0x432D2C95`) goes out 17s after the trigger.
9. **Shaxx hallway — confirmed complete:** at `pt_centurion_intro_reinforce` the script also
   places `sq_shaxx` (Frame-style; his scene does not spawn him), instantiates `o_shaxx_door_exit`
   and `gun_door` (closed doors) and arms `door_interactable` (`set_interactable_object`).
   `scene_shaxx` (graph `0x80BEB7EF`, 5 keys) is progress-gated, one key per trigger: key 1
   (Shaxx stands and talks) on the unnamed zone volume 249 (bubble 9 row 28, resolved at arm
   time via `bubble = {9, 28}`), key 2 (opens the exit door) at `pt_start_shaxx_scene` (275),
   then at `pt_weapon` (registry `0x9027B6A1`, volume 4) keys 4 → 5 → 3 with 4s / 1.5s gaps
   (4 and 5 are his two lines, 3 shuts the door — in index order the close cut the second
   line). The four `sc_civilian_*` scenes (graphs `0x80BEB801/804/806/7F3`) fire at
   `pt_sc_civilians_start` (324); no visible effect, kept. Past Shaxx, `on_event_object_interacted`
   on `door_interactable` (slot 119) drives `d_gun_door` to position 1.0 and disables the prompt.

## The findings, in the order they were established

### 1. The real event-gate graph is at resource-entity `+0xC0`, not `+0x88 → +0x64`

The previous session's chain `resource +0x88 → schema record → +0x64 → graph` lands on a small
record (`0x80BEB7EA` for the Cabal scene, `0x80BEB7ED` for the Centurion) that only *declares* the
gate count in its header (5 and 3) and holds no nodes. The resource entity references a second,
large graph at `+0xC0` (repeated at `+0x1A0/+0x240/+0x290/+0x2B8`) which holds exactly that many
`0x8080637D` nodes, `0x60` apart, FNV-1 key at marker+12:

| scene                      | gate graph   | keys |
|----------------------------|--------------|------|
| `scene_cabal_first_contact`| `0x80C3DEF4` | `385838EC, AE7CC69C, 50B1B667, E404F85C, 9645E0E8` |
| `sc_centurion_intro`       | `0x80BEB7EC` | `385838EC, 6F51AC66, CFB8CB39` |

`0x385838EC` opens every scene's list in the whole activity (a shared "start" gate). Some 20
sibling graphs `0x80BEB7xx`/`0x80C3DFxx` each carry their own run — the offset-scan technique is
general. `find_event_gate_keys()` is now bounded to the record (`uint32` at offset 0) and skips
`0`, `0xFFFFFFFF` and the `0x8080xxxx` class band (the header declaration entry reads as key 0).

### 2. Protocol confirmed live: empty deposit → activate → all keys at once

`scene_finished` fires reliably ~7s after the keys (≈3.75s after the last key when they are
staggered 3s apart). An empty event list with a valid body does nothing: the keys are required.
Sending them staggered made the scene start visibly late; one publish of the full list is right.

### 3. Scenes neither spawn nor deliver their participants

With no manual placement nothing appears (run 2); squad mode `reserve` produces nothing either
(run 6). Participants come from the descriptor (`0x9D8076E4` + packed `slotIndex<<16|slotType`):
Cabal scene = squad 6 (`squad_first_contact_cabal`), squad 7 (`sq_frame_die`), aiPointSet 274;
Centurion scene = squad 20 (`sq_centurion_intro`), squad 24 (`sq_frame`). Participant squads all
share the scene's own origin as their authored anchor (SDK: `sq_centurion_intro` and `sq_frame`
at the identical point), so ordinary placement drops them on each other.

`sq_frame_die` is deliberately not placed: it only slid half-sunk through the floor and the user
is confident it is not part of the beat as shipped.

### 4. The rule: a scene claims a combatant only if it already exists and is not fighting

Across runs 1–5 the Frame (no type-2 cell, client-simulated) always played its part while the
Centurion/Cabal (combat AI) ignored the scene whether placed before or after the bind, and the
SET_FACTION actor command (`squad:actor_command{command=45, value=-1/-2/-3}`; policy accepts only
NONE/REMOVED/HOSTILE_TO_ALL) did not stop the engagement.

`sq_centurion_intro` is the one squad of the beat with a **type-2 cell** (`sq_centurion_intro__cell_1`).
`slot:play_actor_action{generation=1, revision=N, group=, action=}` on that cell **creates the
actor** (exactly like Ember's Harvester) and runs the named actor state. The Centurion class
(`0x80C1A52D`) declares 14 states, all in group `0xAFB11A12` (listed by the new
`context:squad_state_names{squad=row}`; hashes only, no names recovered):

```
1:40FC40DA 2:8E530259 3:92B2820F 4:4294D6A1 5:56A95B3A 6:41651ADD 7:675F9DA3
8:69F9A1BC 9:8F797125 10:211C59FE 11:F64F014C 12:C654F1BB 13:9BC29DEE 14:F7C6C8F4
```

States 1–8 are static poses; **#9 `0x8F797125` is the entrance/impale animation itself** (sent
directly it plays from the actor's current spot — walks into the left wall). The working recipe
(run 13): create the actor with pose #1 at `pt_centurion_intro`, then start the scene at the
trigger; the scene claims the existing, non-fighting actor, places it at its authored entrance
and plays everything itself. This is presumably the general pattern for every scene with a
combatant participant (`scene_cayde_golden_gun`, `sc_hero_moment_underwatch`, ...).

### 5. Do NOT send a spatial target with an action program

`combatant_auth::ActionRequest` now carries an optional target ClientRef + 3-bit mode + 8-bit
marker (`play_actor_action{target=, target_mode=, target_marker=}`), added to try positioning
the actor. Every present target tried (scene slot type 43; squad slot type 1; corrected encoder)
**stalled the client's main loop within ~2s** (`hitch detected: mainloop world controller ...
stalled`), i.e. a hard freeze. The fields stay in the API but unset; the accepted reference kind
is unknown and not needed now that the scene positions the actor. (Runs 10–11 also had an
encoder ordering bug — the target was written before the root fields — fixed; run 12 with the
fixed encoder still froze.)

### 6. Participant policy is per scene; watches must be released; keys can be progress-gated

Shaxx's scene shows a third participant policy (place `sq_shaxx` normally, like the Frame) and
that a scene's keys can each stand for a player-progress event: sent all at once the exit door
opened and shut within 2s. Keys are a cumulative *set*, so they can be published out of index
order (`publish_scene_keys{...}` in the script). A watch can now be declared as
`bubble = {index, row}` and is resolved through `find_trigger_by_bubble` at arm time.


`scene_cayde_golden_gun` (config `0x80B5099E`, resource `0x80B8273C`, 33 participants incl.
`o_shaxx_door_enter/exit`) spawns its own Cayde and Legionaries — placing them (or posing the
Legionaries) only produced duplicates. So: Centurion = pre-create; Cayde = supply nothing. Check
each new scene live before assuming either. `d_gun_door`/`gun_door` do nothing to that door;
`o_shaxx_*blocker` are invisible collision only. `pt_shaxx_enters` does not arm by name — arm the
type-60 identity `2642441956/60/320`, and only once its region is streamed (the retry loop covers
it).

The client trigger-watch table holds **64 entries and survives a mission restart in the same
process**; a live mass-arm of 47 volumes (done once for discovery) filled it and every later arm
failed until the game was restarted. New API `context:unwatch_trigger_identity(reg, type, idx)` /
`slot:unwatch_trigger()` (native `player_trigger_watch::unregister_watch`); the script releases
every one-shot watch right after it fires. Never mass-arm live again.

## Native additions (all uncommitted — ask before committing)

- `src/client/hooks/content_resolver/content_resolver.{h,cpp}`: `find_event_gate_keys()` bounded
  to the record and filtered; `dump_tree` logs `window=`/`record=` and counts gates within the
  record. Plus the previous session's `resolve()`/`dump_tree()`.
- `mission_script_vm.h` / `mission_script_sdk_bridge.cpp` / `mission_script_lua_context_api.cpp`
  / `mission_script_lua_internal.h`: `context:squad_state_names{squad=<row>}` → array of
  `{group=, name=, ordinal=}` from the SDK actor-state-name table for the squad's member classes.
- `combatant_auth.h` + `mission_script_lua_slot_api.cpp`: optional `target`/`target_mode`/
  `target_marker` on `play_actor_action` (see finding 5 — leave unset).
- `player_trigger_watch.{h,cpp}` `unregister_watch()`; `mission_script_vm.h` `UnregisterTriggerWatch`;
  bridge `unregister_trigger_watch`; Lua `context:unwatch_trigger_identity()` and
  `slot:unwatch_trigger()`.
- Still present from earlier sessions: `context:find_event_gate_keys{hash=}`,
  `context:dump_hash{}`, `context:resolve_hash()`, scene `config_tag`/`descriptor_offset`/
  `resource_tag`, squad `registry_key`/`slot_type`/`slot_index`.

Builds clean; deployed to `E:\Sunrise\Game\bin\x64\steam_api64.dll` (copy fails while the game
runs — close it first).

## Deployed script

`E:\Sunrise\Game\bin\x64\Sunrise\scripts\mission_towerfall.lua` (not in the repo) is cleaned of
the experiment scaffolding; `mission_towerfall.lua.bak_run*` backups beside it record each run's
state. Remaining diagnostics: `scene_fallback`/`wall_fallback` probes (no-ops now), the
`squad_slot(...)`/`squad_state_names` probes at bootflow 38, and the cinematic/`scene_finished`
probes. Dev-mode sandbox relaxations still active.

## Leads for the next session

1. Next beats after "Find Zavala": `scene_shaxx` (slot 14, `pt_start_shaxx_scene` /
   `pt_player_near_shaxx`, doors `o_shaxx_door_*`, `seq_vig_shaxx_brawl_lp`), the civilian scenes
   (`sc_civilian_*`), `sc_hero_moment_underwatch`. Same method: gate graph via `resource+0xC0`,
   participants from the descriptor, then test live whether the scene spawns its own participants
   (Cayde-style) or needs them pre-created (Centurion-style). Live commands go through
   `E:/Sunrise/Game/bin/x64/Sunrise/rt_cmd.txt` (read on the poll tick; `cmd result` in the log).
2. `E:\Sunrise\Game\bin\x64\Sunrise\hashdump\` (~1100 blobs) is disposable.
3. If a spatial target ever becomes necessary, read the native kind-9 consumer first; the
   upstream `A97800 → AB4030 → A054C0 → A889E0` chain only led to the (group, action) → index
   lookup (`A889E0`) and an animation/pose path, not to the target decoder.
