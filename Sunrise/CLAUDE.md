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

### 5. An action program's spatial target must be a point set or a path

`combatant_auth::ActionRequest` carries an optional target ClientRef + 3-bit mode + 8-bit marker
(`play_actor_action{target=, target_mode=, target_marker=}`). Every target tried live (scene slot
type 43; squad slot type 1) **stalled the client's main loop within ~2s**, i.e. a hard freeze.
The 2026-09-13 RE explains it: the native resolver `4ffec0` accepts **only slot type 48 (a point
set, the marker being the point index) and type 58 (an authored path)**; any other type leaves
the handle at `0xffffffff`, which the handler then uses as a table index. Encoder and Lua API now
refuse anything else, so a bad target errors instead of freezing. **Validated live**: a type-48
point set as the target is accepted with no hitch at all (the first spatial target that ever
survived), but it does not move the animation — state 9 plays at the same spot with and without
it. Displacement has to come from the path program instead, so `play_actor_path` now takes a
point set too (`marker` = point index). (Runs 10–11 also had an encoder ordering bug — the target
was written before the root fields — fixed.)

### 5c. `play_actor_program`: probing the unnamed kinds, and why it must stay one shape per run

`slot:play_actor_program{generation=, revision=, kind=0..9, word=, float_bits=, ref=, marker=,
bits6=, bit=}` emits any kind with any of the ten decoded body shapes (pieces are written in that
order; an absent piece is not written). It is how kind 8 was identified as `{ClientRef, u8}`. A
body of the **wrong bit length stalls the client** rather than being refused, so shapes are never
batched and `combatant_auth::probe_shape_allowed` rejects the combinations known to be wrong.

### 5b. A program drives an actor only if the program created it

`play_actor_action` on the cell of a squad that is **not placed** creates a docile, immobile
actor that then obeys every action (the Centurion recipe). An actor the squad spawned via
`place` ignores the same sends — passive (`task_group = -1`) or with an active combat group
alike. Reproduced both ways live. Every action needs the **double send**, poses included; and
after `retire_squad`, re-place with explicit counts or the squad stays at zero members while
every send still reports `ok=true`.

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

## Deployed script (not in the repo)

`E:/Sunrise/Game/bin/x64/Sunrise/scripts/mission_towerfall.lua` is the root controller; the beats
live in `scripts/mission_towerfall/` (`require` works with the runtime's `scripts\?.lua` search
path): `common.lua` (squads, doors, cues, directives, music, `cell_action`, scene bind/activate/
keys with live graph resolution from `{ resource = tag }` via `resolve_hash` +0xC0, full-id
aliases for ambiguous scene names, watches with `on_enter`/`on_exit` and automatic release, named
timers), `underwatch.lua` (bubble 9, `region = 72`) and `military.lua` (bubble 4, `region = 32`).
`START_REGION` at the top of the root picks the spawn bubble; only that bubble's module and the
following ones are installed, and `on_start` calls `context:clear_trigger_watches()` because the
64-entry client watch table survives mission restarts in one process. Syntax check: a
`luacheck.exe` built from `vendor/lua` (`luaL_loadfile` on each file), rebuilt in the scratchpad
when needed. Live experiments go through `rt_cmd.txt` without restarting; only new watches need a
mission restart. Dev-mode sandbox relaxations still active.

## Military hangar (bubble 4) so far

- `pt_hangar_early` (registry `0xAA9D42BE`, volume 233): `sq_hangar_overlook_a_a` + `_cent`; both
  Amanda gating doors shut (`d_gating_amanda_start` = first door past the pod,
  `d_gating_amanda_hangar` = second; open by default, position 0 snap shuts them).
- `pt_hangar_spawn` (226): instantiate the pod object `o_cabal_drop_pod_military_hallway`.
- leaving `pt_hangar_spawn_backup` (227): place `sq_military_hallway_destruction` with member
  **count 3** (a one-member squad bound four times as the participant of
  `sc_military_hallway_destruction`, graph `0x80BEB612`, one key) and start the scene: the pod
  opens (~2.8s) and three Legionaries come out. Default counts give one; `replace` mode keeps the
  pod shut; activating the scene before its key finishes it empty and a later key does not
  restart it (only a new generation does). `sq_hangar_a_b` (own spawn rule) does not spawn from the
  hallway but does from `pt_mount_ship` (a pod crashes down with its three Legionaries).
  `place{ spawn_rule = <type-66 sr_* slot>, spawn_lane = 1 }` (wire field .11) **selects that
  spawn rule**: `sq_hangar_fodder_b` placed with `sr_caball_hangar_fodder` arrived in a crashing
  pod. **Never send .12 (`spawn_lane = 2`) live**: a point set (type 48) and a nav point
  (type 47, `slot_0077`) both stalled the client's main loop, like the action-program
  targets -- the reference kind is not the issue; read the native consumer first.
  The Legionary jetpack arrival from behind the command ship at `pt_escape_explosion_a` (three
  `sq_hangar_fodder_a/b/c`) is **parked, unexplained**: none of the 14 actor states is a jump
  (2/4/6/8/10/12/14 resume combat, 3/5/7/13 are poses, 11 is a console interaction, 9 the
  scene entrance); `sc_explosion_a` has no squad participant (point set 140 only); and every
  hangar spawn rule tried live on `sq_hangar_fodder_a` at that spot did nothing —
  `sr_phalanx`, `sr_ceiling_spawn_melee`, `sr_ceiling_spawn_rear`,
  `sr_military_hallway_destruction` (no spawn, existing Cabal unaffected) — except
  `sr_caball_hangar_fodder` (crashing pod). Established since: every hangar squad's
  `spawn_rule_config == spawner_config` (own inline point set) except `sq_hangar_a_b`
  (`0x80B501CC` = `sr_caball_hangar_a_b`) and `sq_military_hallway_destruction`
  (`0x80B501D5`); the generated SDK Lua publishes both tags per squad. The fodder spawner
  config is byte-identical to `sq_hangar_a_a_flank`'s (only the name differs), so the package
  carries no jetpack arrival. The six hangar rules are tags `0x80B501B6` (ceiling_melee),
  `B501BA` (ceiling_rear), `B501C4` (phalanx), `B501CC` (a_b pod), `B501D2` (fodder pod,
  point at (124.7, 83.6, -10.4)), `B501D5` (hallway); each rule has one point row (class
  `0x80809840`, identity u64 first) -- the ceiling/phalanx identities exist in no placement
  table anywhere (dead rules), the two pods resolve in `world.authored_placements`
  (`identifier` is a decimal string). So the shipped jetpack arrival must come from a
  server-sent spawn origin (.12) different from the anchor, with the Legionary AI jump-jetting
  to its points. Actor states re-swept on a fresh actor (creation action = the state, then
  the same state re-sent once): 2/4/6/8/12/14 are poses too, 10 is alert-then-combat -- an
  action on a live actor often needs a second send to take. For now the three fodder are
  placed at their anchors on entering explosion A.
- leaving `pt_hangar_spawn_pod` (228): open the first door, place `sq_hangar_overlook_b_b`.
- `pt_amanda_skip` (219): on entry instantiate the command ship (`cabal_destroyer`, hangar copy
  `slot/80b5036a/000000/0000/0004`) together with the ten `dogfight_*` objects and the escort
  `o_cabal_carrier_r/l` (`slot/80b5036a/00004e|4f`) with their devices `d_cabal_carrier_r/l`
  (`…/000050|51`) at 1.0 (spawning the escort later was visible as a pop), then open the second
  door; on exit Cue 34.
- `pt_hangar_combat` (238): music section 8 on the mission's `m_music_sensor` = slot row 2
  (`set_music_section` is a mask, switch the previous section off first; sections have no names
  anywhere, 8 was found by ear).
- `pt_escape_explosion_a/b` (134/135): `sc_explosion_a/b` (hangar copies, resource `0x80B82715`,
  graph `0x80BEB7B5`, one key, one point set each). A: scene + `sq_hangar_fodder_a/b/c` on entry,
  `sq_hangar_a_a` + `sq_hangar_a_b_sniper` on exit (ahead of the player). B: scene on entry.
- leaving `pt_mount_ship` (237): `sq_hangar_a_b` with full counts — its own rule
  `sr_caball_hangar_a_b` is the pod that crashes down with three Legionaries (validated).
- Native added this stretch: `context:clock_ms()` (interactable generations must keep increasing
  across mission restarts — durable variables do not survive them), `context:clear_trigger_watches()`,
  `squad:place{spawn_rule=, spawn_lane=}`.
- Squads/objects whose names are shared by several objects (`sc_explosion_a`, `cabal_destroyer`,
  `o_cabal_carrier_r`) must be addressed by their full SDK slot id / symbol id.

## Session 2026-09-13: combat objectives, task groups, hangar timing, RE of the Auth bodies

Full RE detail is in [RE-ACTOR-PROGRAMS.md](RE-ACTOR-PROGRAMS.md); the tool is
`tools/reflect_dump.py` (decodes the client's reflection registry from the dump file).

- **Cabal move now.** `slot:assign_combat_objective{objective=, revision=, task_group=}` on the
  squad slot, sent **before** `place`, is what gives a squad its AI (without it every Cabal
  stands still — the "no pathfinding" observation). Rules established live: an assignment sent
  to a living squad is ignored unless it changes the group **at the same revision**; the
  revision must be new on every mission run (the client keeps the last one per squad across
  mission restarts in one process) — `common.lua` derives one per run from `clock_ms`.
- Task groups are the objective's combat areas + tactics. `obj_hangar`
  (`slot/80b5036a/000001/0001/0003`) on a fodder: 12/13/16 hold `fa_fodder` (explosion A) with
  jetpack hops, 14 explosion B, 15 the overlook above B, 0/1/2/7/8/9/11 the overlooks via the
  stairs, 3/4/5/17-20 leave the area (despawn). `squad_state.task_costs` (from the client,
  group+1 -> cost, 2040 = unreachable) arrives piecemeal and jumps between events; the script
  waits 1 s, picks the cheapest reachable group once, then keeps it unless it disappears.
  Every hangar squad is placed with `HANGAR_AI = { objective = obj_hangar }`.
- The fodder "jetpack arrival" is that AI: a group-13 Legionary far from the player jets toward
  him. Still not reproduced as shipped (see the RE file: `.12` with a rule is accepted but
  inert; the action target is a type-58 path marker; the 10 actor-program kinds are decoded but
  the kind<->class table is only built at runtime).
- Hangar script changes: Cue 34 + music 8 on entering the unnamed volume `0xD3847A1F/60/5`
  (just past the second door); `o_cabal_missile_1..6` (hangar copies, slots 0x30-0x35, type-4
  objects that fly in and strike on instantiation) staggered by timers at `pt_amanda_skip`;
  `sq_hangar_a_a` + `sq_hangar_a_b_sniper` 4 s after entering `pt_holliday` (the stairwell,
  x 111-164 / y 94.7-100.7); the `sq_hangar_a_b` pod 3 s after leaving explosion A;
  `pt_hangar_combat` no longer watched. `common.lua` was lost once to a bad edit and rebuilt by
  replaying the edit history from the transcripts — a copy lives in
  `scripts/mission_towerfall_backup/common.lua.bak_ai`.
- Squad facts: the three fodder are plain Legionaries (14 states of group `afb11a12`, spawner
  config byte-identical to `sq_hangar_a_a_flank` except name/anchor/`fnv1(name)` at +0x8f8);
  no authored placement exists behind the ship except the `a_b` pod point; no type-58 path in
  the hangar; the hangar fake fight (`sq_red_guard_fake_fight_a/b/c` on the balcony at z 0,
  `sq_frame_cover_a/b/c` + scenes, `sq_frame_throw_a`/`sq_red_guard_throw_a`) and the friendly
  frames (`sq_friendlies_early`, `_upper`) are still unused.
- Community script in `C:/Users/brand/Downloads/mission_towerfall` (AI-written, untested by
  its author): its only useful idea was `assign_combat_objective` + the cost loop; its music
  and cues are placeholders.

## Session 2026-09-14: plaza, Zavala takes cover

- **`sc_zavala` follows the Centurion rule.** A `sq_zavala` placed with a combat objective turns
  and shoots but never moves, and `sc_zavala` ignores him (tried on `obj_plaza_init` and
  `obj_plaza_kill_cabal`, with its cast, and with plaza engagement flags / `tg_plaza_battle`).
  Working, observed live: `retire_squad("sq_zavala")` → 2 s → pose `0x40FC40DA` (group
  `0xAFB11A12`, double send) on his type-2 cell `sq_zavala__banshee` (index 7), plus
  `squad_kill_cabal_5..8` placed without objective → 3 s → `sc_zavala`
  (`slot/80b50746/000002/0002/002b`) bind → activate → its 10 keys (`385838EC C021F76C B8C5C0A5
  1BED1ED3 342380CB B0A69403 2DD14D67 5C569FF8 8751DF51 397A672B`). Zavala vanished, reappeared,
  moved to the wall and took cover. Zavala's actor states are the same generic 14 as the
  Centurion's. Not yet isolated: whether the kill_cabal cast is needed, and whether the bunker
  mission effect (attached to the retired entity beforehand) played any part.
- **Mission effects are selected through filter predicate B.** New read-only client hook
  `src/client/hooks/mission_effect_probe` logs what each type-26 body selects and attaches; see
  [HOP-ON-SENSORS.md](HOP-ON-SENSORS.md). Predicate B on a squad selects its members, D is the
  local player, the authored `of_filter_zavala` selects nothing. Its log cap (400 lines) is reached
  in a few minutes because attached effects are re-checked twice a second.
- **Participants of the Zavala scenes** (decoded from the dumped config blobs
  `hashdump/hash_<config>.bin`: `{class, role_a, role_b, runtime registry 0x28A6B21F, idx<<16|type}`
  entries after the scene's own `80806266` header; names looked up in registry `80b50746` only).
  Zavala has the same role `EB5FA833/C6DDEEAA` in all four:
  - `sc_zavala` (config `0x80B50629`): `sq_zavala`, `squad_kill_cabal_5..8`,
    `ps_zavala_shooting_to_cover_align_point` + point sets 0x40/0x45/0x46/0x47/0x3A;
  - `sc_zavala_combat` (`0x80B5062D`, graph `0x80C3DEB0`, 19 keys): `sq_zavala`,
    `squad_kill_cabal_1/2`, `sq_plaza_reinforce_a_a/a_b`, the align point + 8 point sets;
  - `sc_zavala_bunker_shield_bunker` (`0x80B50633`, graph `0x80BEB7A1`, keys `385838EC C021F76C
    9DD22D40 2CBA081B EED6F6F4 A3BC3509 9D8642A9 E58D98B9 397A672B B057C3B6`): `sq_zavala`,
    `o_missile_spawner` (type-4, index 46), the align point;
  - `sc_zavala_shield_death` (`0x80B50639`): same three as the bunker shield scene.
- With Zavala in cover (held by `sc_zavala`), all observed live, nothing changed him:
  `sc_zavala_bunker_shield_bunker` spawns **its own duplicate Zavala** (Cayde policy) and plays
  shield + missile rain on it (the scene bubble does not protect the player -- the player shield is
  presumably `ho_bubble_shield` on players); `sc_zavala_combat` on generations 1-4, with and
  without its four squads placed passive, only turned him at the very end;
  `assign_combat_objective(obj_plaza_kill_cabal)` on the unplaced squad got no squad report back;
  actor states 2, 3, 4, 5 and 9 on `sq_zavala__banshee` did nothing (probably because the scene
  holds him -- the Centurion sweep was on a free actor); `sc_zavala` re-bound on generation 3 with
  only its first key did nothing; and `retire_squad("sq_zavala")` followed by pose 1 on the cell
  **neither removed the program-created Zavala nor created a second one**.
- Next: a fresh mission run to get a free program-created Zavala (pose 1, no scene), then the 14
  states on him, then `sc_zavala` key by key (cumulative sets on one generation), then find how to
  release or remove a scene-held / program-created actor.

### 2026-09-14 (day): `sc_zavala` replicated on a fresh run, then key by key

- **Replicated on a fresh run without `retire_squad`** (`sq_zavala` is never placed by
  `plaza.lua`, so there is nothing to retire): pose 1 (double send) on `sq_zavala__banshee` ->
  3 s -> `squad_kill_cabal_5..8` via `spawn_squad_full(_, 1)` without objective -> 3 s ->
  `sc_zavala` gen 1 bind -> activate -> 10 keys. Zavala and the Cabal behave as before.
- **Key by key (cumulative sets on generation 1, step N = keys 1..N, each step observed):**
  steps 1 and 2 (`385838EC`, `C021F76C`) nothing visible; **step 3 (`B8C5C0A5` added) plays
  the animation and moves him to cover**; steps 4-10 add nothing visible with him already in
  cover, and no `scene_finished` was logged. **Key 3 alone** (fresh run, bind -> activate ->
  `{B8C5C0A5}`) also triggers the move to cover: keys 1-2 are not prerequisites, the gates are
  independent. Keys 4-10 each sent **alone** on their own fresh generation (2..8), 3 s apart,
  with Zavala already in cover: nothing visible at all. So within `sc_zavala` only key 3 has an
  observable effect on him from the posed/cover state; the other nine are inert or need a state
  we have not reached.
- **`sc_zavala_combat` on a posed Zavala (fresh run, no `sc_zavala` first)**: cast
  `squad_kill_cabal_1/2` + `sq_plaza_reinforce_a_a/a_b` placed passive, then gen 1 bind ->
  activate -> key 2 (`B8C5C0A5`) alone: Zavala leaves his pose, goes to the wall and **shoots at
  the Cabal area**. Then all 18 other keys, each alone on a fresh generation (2..19), 4 s apart:
  **every one produces the same thing** (out of cover, to his position, aims at the Cabal, no
  shot this time), and after the last one he stays aiming, never back in cover. So on this scene
  a new generation restarts the graph's opening (move + aim) regardless of which key is in the
  set: single-key-per-generation does not discriminate the keys here. Yesterday's "only turned
  him at the very end" was a Zavala already held by `sc_zavala`; a scene claims him only while he
  is free.
- **Validated on the next run**: key 1 alone (`385838EC`) as the run's first generation gives the
  same shooting behaviour (out of cover, shoot, back in cover, out again and two more shots, then
  stands exposed and idle); key 2 alone on generation 2 then gives cover in/out/in with no shot.
  So **shooting belongs to the first graph run on a free Zavala, not to any key**; later
  generations only replay the cover cycle.
- **Cumulative keys on ONE generation (1..19, 6 s apart, free posed Zavala + passive cast)**:
  he comes out twice shooting at the Cabal, and once they are all dead he stays in cover and
  does nothing (whereas the generation restarts above pulled him out with no target). Reading:
  `sc_zavala_combat` is his whole battle loop (cover <-> shoot while living targets exist); its
  keys did not produce distinct visible actions at 6 s spacing, so they are presumably progress
  gates (waves). Follow-ups on that same run, all negative: `sq_plaza_reinforce_a_a/a_b`
  re-placed WITH `obj_plaza_kill_cabal` (AI, group 0) -> no reaction; gen 2 with all 19 keys
  -> out of cover and back, no shot; `squad_kill_cabal_1/2` re-placed passive + gen 3 with all
  keys -> no movement at all. Working hypothesis (unconfirmed): the shots are the scene's own
  first-run choreography on its `squad_kill_cabal_*` victims (hence the name), and the reinforce
  squads are real AI; a scene restart does not replay it. Every conclusion about restarts is
  muddied by the actor having been claimed 3 times in one run -- confirm on a fresh run.
- **User observation that settles it:** when the scene makes him shoot, the shots go to fixed
  spots of the scene (its authored point sets), not at the Cabal; the only time he ever aimed at
  actual Cabal was under the combat objective (correct targeting, wrong animation, no movement).
  So `sc_zavala_combat` = choreographed cover/shoot loop (first run only), `sc_zavala` key 3 =
  move to cover. Neither is target-driven.
- **Wiring attempt in `plaza.lua` (spawn: pose -> 3 s -> `sc_zavala` key 3; `pt_zavala_loop`:
  shield; `pt_plaza_zavala_meet`: kill_cabal_1/2 + reinforce AI -> `sc_zavala_combat`)**: the
  volumes are in that order (loop = `0xF8F959CD`/60/39 near the spawn, meet = `0x28A6B21F`/60/124,
  both arm by name) and the shield is right (it fades by itself after ~6 s), but Zavala, once
  held in cover by `sc_zavala`, **ignores `sc_zavala_combat` for good**: all 19 keys, key 1 alone
  on gen 5, and a `sc_zavala` reset (gen 2, empty set, no activate) followed by combat key 1 on
  gen 6 all did nothing. The footage shows him in cover behind the shield, then coming out to
  shoot when the player arrives; the pose spot is not his cover spot. Next candidate: the bunker
  loop hop-on (`ho_zavala_looping_bunker_anim`, predicate B on `sq_zavala`) on a free posed
  Zavala instead of `sc_zavala`, then `sc_zavala_combat` from that state.
- **Bunker loop on a free posed Zavala (fresh run, `sc_zavala` off)**: `of_filter_zavala`
  (`slot/80b50746/000038/0038/0022`) armed with predicate B on `sq_zavala` selects the
  program-created actor too (entity `4CFAA26C`, `selected=1 attached=1`, re-checked twice a
  second with `attach_outside ret=0`), and `ho_zavala_looping_bunker_anim` rev 1 attaches --
  **no visible change**: he stays in pose 1 at the cell spot. `sc_zavala_combat` then (gen 7;
  its gen 1 had fired at `meet` before he existed) gave two cover exits without a shot, i.e. the
  restart behaviour -- **but there was no living Cabal**. Same run, cast re-placed passive
  (`kill_cabal_1/2` + `reinforce_a_a/a_b`) then combat key 1 on gen 8: **three exits, three
  shots, then cover once the last Cabal died.** Rule: `sc_zavala_combat`'s loop fires only while
  Cabal are alive; with none alive a restart gives the empty cover cycle and nothing else. The
  "first run only" reading above is wrong (dead Cabal every time). Whether a `sc_zavala`-held
  Zavala really ignores the combat scene is therefore unproven again (Cabal state unknown in
  those runs): test the scripted flow without killing anything before `meet`.
- **Confirmed with living Cabal: a `sc_zavala`-held Zavala ignores `sc_zavala_combat`** (scripted
  flow, fresh cast at `meet`; and again later with him settled in cover, cast re-placed, gen 2).
- **SOLVED — replace the actor with a new program generation.** `play_actor_action{generation=2,
  revision=1..2, group, action=pose}` on `sq_zavala__banshee` while `sc_zavala` holds the
  generation-1 actor: **the new Zavala replaces the old one** (the held one vanishes) and is
  free; cast placed, then `sc_zavala_combat` gen 3 key 1 -> he takes cover and comes out twice
  to shoot the living Cabal. In the ~7 s between the re-pose and the scene the fresh actor shot
  at the Cabal on its own, so the script now re-poses right after placing the cast and starts
  the scene 1 s later. Generalises the Centurion rule: each scene wants a *fresh, free* actor,
  and the program generation is how the actor is renewed (`retire_squad` never removes a
  program-created actor).
- Scratch commands for this: `zv_cover.lua` (full recipe), `zv_setup.lua` (pose + cast, no
  scene), `zvk_N.lua` (step N) in this session's scratchpad.

## Session 2026-09-14 (afternoon): the plaza opening in `plaza.lua`, what is settled

Reference (user's footage): Zavala is already in cover behind a fake shield when the player
reaches the plaza; the shield fades (~6 s), the wave-1 Cabal land, Zavala leaves cover for a
full salvo, returns, ~3 s later a second salvo, then stays in cover; once the player kills the
rest, a dialogue closes wave 1.

Settled live (each point one or more runs):
- `sc_fake_shield` (`80b50616`/3, resource `0x80B826FB`, 1 key `385838EC`, point set
  `ps_fake_shield`) is the shield; `bubble_shield_1` (the object) leaves an "Immune" collision
  wall in front of him after it fades -- never use it.
- `sc_zavala_combat` bind + **activate alone, no key**, on a free posed Zavala puts him in cover
  and runs the loop: his exits are triggered by **living Cabal appearing** (the shipped two
  salvos), the first exit coming ~3 s after the activate whether or not a key was published
  (a key published later changes nothing; a new generation restarts the loop and, with Cabal
  alive, turns it into shoot-until-dead with no return to cover). With no Cabal at activation
  the first exit is wasted. `squad_kill_cabal_1/2` must stay **passive** on their points (the
  salvos aim at fixed points; with the AI they walk off and the salvos go short/empty).
- A Zavala held by `sc_zavala` never joins `sc_zavala_combat` (all keys, key 1, key 2 alone,
  after a reset generation, with living Cabal) -- confirmed again. There is no scene
  deactivation verb: `scene:activate` is a state-local override
  (`request_state_local_authored_scene_override`) that nothing withdraws.
- A new program generation on `sq_zavala__banshee` replaces the actor (the held one vanishes)
  -- the only known "release"; visible unless hidden (the shield).
- Placing a *participant* squad while the scene runs seemed to re-sync Zavala in some runs; not
  isolated (also correlated with the shield/creation order).
- The plaza volumes are crossed in 3-4 s at a run (on x: `pt_spire_trigger` 69 -> `pt_zavala_loop`
  59 -> spire exit 41 -> `pt_defend` 36; `pt_plaza_zavala_meet` is further north, y 9-45), so
  the beat cannot be timed from volumes alone: one anchor (`pt_zavala_loop`) plus a fixed chain.
  Best run so far ("presque parfait"): pose at `pt_plaza_spawn_init`; at `pt_zavala_loop`:
  `sc_fake_shield`, wave 1 (+2 s), `sc_zavala_combat` activate (+5 s). Only defect: he takes
  cover a little late (the combat scene's own cover, after the player can see him).
- Wave 1 as placed: `squad_kill_cabal_1/2` passive, `sq_plaza_reinforce_start_a/b` +
  `sq_plaza_reinforce_a_a/a_b` with `obj_plaza_kill_cabal` (one task group only, 0).
  Untouched: `squad_kill_cabal_3..8`, `squad_cabal_dropoff_1`, `reinforce_a_c/a_d(+extra)`,
  wave B, `interim_a_*`, the end-of-wave dialogue.
- Activity-level SDK is no help: `matchmaking_config_tag = 0xFFFFFFFF` (not matchmade),
  `mission.tasks = {}` (no authored task graph for Homecoming; the beat logic was Bungie's
  server script), `ap_*` are type-47 nav points.
- Backups of the variants: `mission_towerfall_backup/plaza.lua.chain_variant` (the chain),
  `plaza.lua.swap_variant` (sc_zavala cover at spawn + actor swap behind the shield + combat).

## Session 2026-09-14 (evening): the in-game Sequencer

The plaza beat timing was being tuned by hand-editing timers; it is now data-driven.

- **UI**: Activity Host > World > **Sequencer** page (`src/server/ui/activity_host/
  activity_host_sequencer_view.{h,cpp}`). A sequence = a start + an ordered list of steps, each
  with a delay (ms) relative to the previous step. Starts: `trigger_enter` / `trigger_exit`
  (volume picked from the package trigger-volume catalog, stored as registry/type/index plus the
  type-31 name as fallback), `squads_clear` (every listed squad seen alive then at 0 for 1.5 s),
  `sequence_end`, `spawn` (bootflow 38), `manual`. Steps: squad (count, objective, hold), retire,
  cue, scene (name + slot id, mode full/activate/keys, explicit keys or resource tag for discovery
  at spawn, new generation), pose (cell, group, action, new generation = actor replacement),
  effect (hop-on + filter, players), object, device, directive, sequence, clear, probe. Pickers
  come from the bound SDK view (slots of the scenario by type). Save writes
  `Sunrise/sequences/<file>.json`; "Save + reload script" also restarts the mission script (new
  trigger watches still need a mission restart).
- **Runtime**: `scripts/mission_towerfall/sequencer.lua` (+ `json.lua`), loaded by the root at
  `on_start` (`SEQ.load` + `SEQ.install` before the watch timer; `SEQ.on_spawn` at bootflow 38).
  Timers are `seq:<name>:<step>`; probes are prefixed `seq `. Manual start from `rt_cmd`:
  `require("mission_towerfall.sequencer").start(context, "name", "manual")`.
- **Native**: `context:read_artifact_text{ path = }` (read-only, under `Sunrise/`, 1 MiB cap) in
  `mission_script_lua_probe_api.cpp`. Built and deployed 2026-09-14 18:21.
- `Sunrise/sequences/mission_towerfall.json` reproduces the plaza opening as it stood
  (`zavala_spawn` on spawn, `plaza_loop` on `pt_zavala_loop`, `wave1_clear` on the six wave-1
  squads); `plaza.lua` keeps only the fleet and the spire (the hand-written version is in
  `mission_towerfall_backup/plaza.lua.pre_sequencer`).
- Squad clear tracking (`M.track_clear` in `common.lua`, `squad_state.alive_count`): a zero must
  persist 1.5 s (pods report 0 transiently for up to ~1.4 s after placement), a squad that
  reports alive again is un-cleared, pending zeros are confirmed from the poll tick
  (`M.tick_clears`) because no event may follow the last death. The `objective_progress`
  counter is NOT a kill count (it reached 30 in one run).
- Bash-tool heredocs collapse a double backslash to a single one: write files that contain
  backslashes (C++ string literals, Lua escapes, vcxproj paths) through the Write tool.

## Session 2026-09-15: a scene can be stopped -- the Zavala hand-over is solved

- **The type-43 Auth header's `clear` bit ends the scene's generation and releases its actors.**
  It is what the community fork's `scene:stop{}` sends (same generation, same keys, `clear` = 1);
  our encoder had always written it as 0. Exposed as `set_scene_events{generation=, events=,
  clear=true}` and as scene mode `clear` in the sequencer. Validated live, isolated
  (`test_stop`, 15 s gaps): pose -> `sc_zavala` key 3 (cover) -> clear -> `sc_zavala_combat`
  activate takes him **in place, no vanish, no teleport**; then combat clear -> the bunker scene
  plays on **our** Zavala (no duplicate, the bubble protects the player). The stopped actor keeps
  its pose (the fork saw Shaxx "stranded" the same way), which is exactly what a hand-over needs.
  The actor-swap (new program generation) is no longer used on the plaza.
- The plaza sequences now: `zavala_spawn` (pose + `sc_zavala` key 3), `plaza_loop` (shield,
  +2 s wave 1, `sc_zavala` clear, +2 s combat activate), `wave1_clear` (Cue 52 -> +6 s a_d ->
  +7 s Cue 53 -> +0.5 s combat clear + bunker + player shield effect -> +7 s effect off -> +1 s
  Cue 51 -> a_c / a_d_extra / interim_a_a). `test_stop` kept, disabled.
- `play_actor_action{enabled=false}` (the type-2 root enabled bit) on a fresh generation:
  exposed, used by the hot reload's despawn; reported as **not removing** the actor (to
  re-check with the deployed DLL -- the first attempt ran before the deploy).
- Sequencer runtime fixes: timer names use dots (`seq.<name>.<n>`, the key charset is
  `[A-Za-z0-9_.-/]`, 63 max); disabled sequences arm nothing; a volume crossing is dispatched to
  **every** watch on that volume (two beats may share one) and the client watch is released only
  when none still needs it; generations rebased on the clock at each load (hot reload).
- Hot reload (`Save + hot reload`): script reload in place -> squads placed by sequences retired,
  cells "despawned" (see above), generations rebased, triggers re-armed, the spawn sequence
  replays. Program-created actors persist until replaced.
- The page keeps its loaded document: after an external edit of the JSON, press **Load** before
  **Save** (Save writes the in-memory document).

## Plaza v1 (2026-09-15) -- data-driven, in the repo

The plaza opening through the end of wave 3 plays as authored, entirely from
`sequences/mission_towerfall.json` (deployed copy: `Sunrise/sequences/`). The scripts are now in
the repo too (`scripts/mission_towerfall.lua`, `scripts/mission_towerfall/*.lua`) -- copy them to
`E:/Sunrise/Game/bin/x64/Sunrise/scripts/` to deploy.

Final timeline (delays relative to the sequence start):
- `zavala_spawn` (spawn): pose on `sq_zavala__banshee` + `sc_zavala` key 3 -> cover.
- `plaza_loop` (`pt_zavala_loop`): `sc_fake_shield`; +2 s wave 1 (`kill_cabal_1/2` held,
  `start_a/b`, `a_a/a_b`) + `sc_zavala` **clear**; +4.2 s `sc_zavala_combat` activate.
- `wave1_clear` (wave 1 dead): Cue 52; +4 s `a_d` x2; +11 s combat clear + bunker full; +13.4 s
  Cue 53; +25.4 s Cue 51 + bunker clear + `a_d` x3; +32.4 s `a_d_extra` x3 + `kill_cabal_1/2`
  held; +35.4 s combat activate; +36.4 s `interim_a_a` x2; +37.4 s `a_a_extra` x4.
- `wave2_clear` (wave 2 dead): Cue 55 + `interim_a_b` x2; +10.5 s combat clear + bunker full;
  +13.5 s Cue 57; +25 s Cue 54 + bunker clear; +26.5 s `b_a` x3; +35.5 s `b_b` x3; +41.5 s
  `b_a_extra` x3; +47.5 s `kill_cabal_1/2` held; +50.5 s combat activate; +51.5 s `b_b_extra` x3.
- `wave3_clear` (wave 3 dead): Cue 59; +6 s directive "Leave the Plaza and find the Speaker".

Rules that made it work (all validated live):
- **Hand-over between scenes = the `clear` bit** (`scene` step, mode `clear`): stop the holder,
  then activate the next scene on the same actor. Never a pose swap. The bunker scene without a
  free actor spawns a duplicate whose bubble does not protect.
- **`sc_zavala_combat` shoots only at its own victims**: `squad_kill_cabal_1/2` must be placed
  (held, task group -1) at their points before each activation; otherwise he comes out and goes
  back without firing. Re-placing the dead squads with explicit counts recreates them. Per the
  footage he sorties once per wave: mid-wave 2, late in wave 3 (the victims + activate move with
  it). After a bunker phase he only resumes with a fresh activation.
- **Wave end** = the objective's kill counter (`objective_progress` on `obj_plaza_kill_cabal` is
  one event per member killed) reaching the members placed on that objective by squad steps,
  held squads excluded (Zavala's victims never count), all listed squads placed; fallback:
  every listed squad at `alive_count` 0 for 10 s (20 s after placement if never seen alive).
  Stale wait-list entries (squads no enabled sequence places) are ignored with a warning.
  `sq_plaza_reinforce_a_c` never spawned (it shares `a_b`'s pod rule) and is not used.
- The player shield effect (`ho_bubble_shield`) is not needed: the bunker scene's bubble on our
  Zavala protects.
- Kill counters are cumulative per mission; the sequencer keeps the last count in a durable
  variable (`seq.kills.<idx>`) as an offset for hot reloads.
- Sequencer page: **`Load` before `Save`** whenever the JSON was edited outside the page (Save
  writes the page's in-memory document; this cost three overwritten fixes today).

Not done yet (polish): exact squads/pods per wave vs the footage (`a_c` replacement, extras,
`dropoff_1`, `interim` roles), Cue 58/59 identification, `kill_cabal_3..8` roles, the pod
`alive_count` flicker, `play_actor_action{enabled=false}` never removed an actor.

## Session 2026-09-15 (evening): military polish, full-run fixes, the HUD objective counter

- **Military**: `sc_explosion_c` (config `0x80B5027B`) is the blast that opens the first Amanda
  door: started on leaving `pt_hangar_spawn_pod`, the door driven in the same tick (1200 and
  400 ms were both "too late"). c/d share a's record except tag/name/point set (`slot_008E` /
  `slot_008F`, no squad, no door participant) -- d is wired on the second door at `pt_amanda_skip`
  by symmetry (unverified), the finale (config `0x80B50294`, resource `0x80B82717`, ten point
  sets `ps_explosion_finale_a..f` + 4 unnamed) is registered but not wired. The hallway pod:
  `sr_military_hallway_destruction` on its own squad spawns the pod + 3 Legionaries + ceiling
  break **only if the pod object is instantiated first**, and adds nothing over the static pod
  (no fall animation) -- static kept. The hangar has no Centurion type-2 cell
  (`sq_hangar_overlook_b_a__major` is a Legionary); its states are the generic 14, so the shipped
  Centurion hover is AI, not an actor state.
- **Full-run bugs fixed**: scene keys were discovered once at spawn, so from the Underwatch every
  hangar/plaza scene had none (regions not streamed yet) -- discovery now retries every 3 s from
  the poll and on demand before use (`M.scene_keys`). `zavala_spawn` started on `spawn` (never
  fires on a full run): now `trigger_enter` on `pt_plaza_spawn_init` (`0xF8F959CD`/60/38, fires
  1.3-3 s after a plaza spawn too). "Find Zavala" is set once (Cayde end); the re-display seen at
  Amanda skip was never sent by the script.
- **HUD objective counter ("Assault repelled x / 3")**: the type-68 lane (class `0x80804F6B`,
  0xF8 bytes: +0 nameHash, +4 element, +8 state, +0x10 timed state `0x808099C4`, **+0x48..+0x54
  four int32**, +0x58 state2, +0x5C ClientRef, +0x64 aux, +0x68 markers) is turned into a HUD
  entry by client `0x7ff741df8570`: when the authored element's flag (packed element +0x20) is
  set, the entry takes +0x48/+0x4C as its counter (+0x50/+0x54 always copied). The package's
  directive table (dumped slot blob `hash_80B50913.bin`, class `0x80804F72`, 16 entries of 40
  bytes, elements class `0x80804F76` of 36 bytes = title, description, progress string, a 4th
  string, flag) holds **`0x23716DE6`**: same title as "Defend the Tower", no description,
  progress string "Assault repelled", flag 1 -- the counter variant, dropped by the SDK
  generator (it requires a description). `set_directive{ progress = {cur, max, 0, 0}, raw = true }`
  sends it (raw skips the SDK lookup; the hash must exist in the package table); validated live:
  "Assault repelled 0/3" shows and stays. Sequencer `directive` steps take `progress` + `raw`.
  `plaza_defend` = "Defend the Tower" then +2 s the counter at 0/3; each `waveN_clear` starts by
  re-sending it at N/3. `OVERLOAD_THE_GENERATOR` element 1 is the same pattern (flag 1).
  **Deliberately not done (user's call)**: the SDK-side fix -- accepting elements whose
  description is empty but whose progress string is not (`activity_sdk_native_pack_text.cpp`,
  the title+description requirement), exposing the progress string and the flag in
  `format::DirectiveElement`, which would need a pack regeneration (delete the scenario shard +
  `catalog.bin`). `raw = true` is the supported way for now.

## Session 2026-09-15 (night): the Boulevard / Bazaar, part 1

Bubble 0 (region **0**), runtime registry `0x7BA8F95D`, SDK registry `80b5011f`, module
`scripts/mission_towerfall/boulevard.lua` (hand-written, not sequenced yet). Everything below was
observed live.

- **Entry**: the player spawns facing the closed `d_door_gating`, inside `pt_start_big_ship`
  (volume 54). The door opens 3 s after entering it, and the Ikora victims are placed in the same
  tick.
- **Ikora's scene** (`scene_ikora_boulevard`, config `0x80B500EE`, resource `0x80B8250B`, graph
  `0x80C3DD7D`, **10 keys** `385838EC 6F51AC66 84FFD4F6 5F4638FB 0B78A21A A0229300 2D5973BE
  9C9D3435 4F785DC4 37BA392B`). Participants (from the config blob): `sq_ikora`,
  `squad_cabal_blasted_1..4`, `squad_invisible_shooting_target` and four point sets.
  **The scene spawns Ikora itself** (Cayde policy: bind -> activate -> keys with nothing placed
  played her whole entrance, dialogue included; placing `sq_ikora` only duplicates her).
  **Key 1 alone does nothing visible; key 2 runs the entire sequence** (arrival, both her lines,
  the Cabal ship, her jump onto it, then the Ghost and Zavala lines) -- the dialogue is the
  scene's, nothing to cue by hand. Keys 3-10 not individually identified.
- **Her blast is NOT part of the scene**: `o_nova_bomb_projectile` is a type-4 object that flies
  in and detonates on instantiation (like the hangar missiles) and kills the three Cabal on the
  stairs. Wired 1.5 s after key 2. `o_nova_bomb_projectile_at_ship` is presumably the same for the
  ship. `o_invisible_character_blocker` / `o_invisible_projectile_wall` /
  `o_invisible_blocker_for_ikora` are untested invisible collision (the `o_shaxx_*blocker` family).
- **The victims cannot be made passive.** `squad_cabal_blasted_1..4` engage the player whatever is
  done: plain `place`, `obj_bazaar` at task group -1 with `hold`, placed before or after the
  scene's activation. They have **no type-2 cell** (the whole boulevard registry has none), so the
  Centurion recipe does not apply. The scene still claims and blasts them, so it is cosmetic --
  but the shipped beat has them kneeling/idle and this is unsolved. `ho_no_combat_abilities`
  (`slot/80b505f3/000047/0047/001a`, the bazaar's own hop-on) through predicate B on the squad is
  the untried lead.
- **The pod**: `sq_bazaar_start` is a one-member squad whose own rule `sr_boulevard_start`
  (`0x80B50092`, different from its spawner config) is the drop pod -- `place{ counts = 3,
  spawn_rule = sr_boulevard_start, spawn_lane = 1 }` drops it with three Cabal (the hangar `a_b`
  recipe). Fired **15 s after key 2** (touchdown ~17 s in the footage), and the "Board the command
  ship" directive (`0x5DC9D705`) 2 s later.
- **A pod squad's `alive_count` is useless**: it flickers to 0 right after landing and its living
  members never report again, so `track_clear` opened the bazaar door immediately. The door now
  waits on the **objective kill counter** instead (`objective_progress` on `obj_bazaar`, slot
  index 1, cumulative -- the first report after the drop sets the base, then 3 kills).
- **Bazaar**: the door `d_door_bazaar` opens on `sq_flame` (placed like Shaxx, the only
  participant of `sc_pyro_intro`: config `0x80B500E3`, resource `0x80B82506`, graph `0x80C3DD7A`,
  2 keys), with `sq_bazaar_a_b` (2 Incendiors) at the same moment. Then `sq_bazaar_a_a` (4) on
  entering `pt_bazaar_mid` (volume 84), `sq_bazaar_a_c` (3) + **Cue 75** on entering
  `pt_bazaar_farther` (volume 87), `sq_bazaar_finale` (2) once `a_c` is dead. All on `obj_bazaar`.
- **End of the bazaar**: `o_hawk_1` is Holliday's ship (`_2`/`_3` are the other players'), her line
  is **Cue 76**, then the fade is `ho_fade_out` (`slot/80b5011f/000020/0020/001a`) through
  `_object_filter_fade_out` (`slot/80b5011f/000030/0030/0022`) with `players = true` -- **address
  both by full slot id, the names are ambiguous**. Delays currently 0 / 2 s / 9 s / 12 s.

### The mid cinematic: why it is refused, and the one hook that exists

`mid_cinematic._cinematic` (`slot/80b508fc/000000/0000/0006`) and the command-ship objects live in
**bubble 8, slice set 64**, which carries two states: region **64** (state row 11, the ship object
`80B508F4`) and region **65** (state row 12). `set_cinematic_active` goes through
`current_behavior_occurrence` -> `behavior_scope::select`, which finds an occurrence only when the
**live region is the state that owns it** (or that state is the mission seed). Live results:

| live region | result |
|-------------|--------|
| 0 (boulevard) | `target_unavailable` -- neither live nor seed |
| 64 | `target_unavailable` -- so the cinematic is not state 11 |
| 65 | **accepted, no refusal** -- state 12 is the owner -- but the client hangs on an infinite load |

Region 65 hangs because its roster snapshot carries **no spawn set**: the log line
`ev=activity stage=roster ... region=65 slice=65 spawn=0x811C9DC5` -- and `0x811C9DC5` is
`kAbsentSpawnSetHash`. With no authored spawn point the player is never placed, `awaitClientSync`
(the ws-702 world state 8 gate) never lifts, and the fade never ends. Region 64 does place the
player, but not at its authored "Default" spawn (`0x2EA8FB98` per the in-game list) either:
`context:select_state{}` carries no spawn hash at all, so the client's picker falls back to an
arbitrary point.

**The hook that already exists**: `activity_roster_snapshot.cpp` overrides
`snapshot.spawnSetHash` with `state::activity::membership::checkpoint_spawn_hash(sessionId,
region.index)` -- a **per-region** spawn override, plumbed all the way to the wire. Its only entry
point today is `context:restart_checkpoint{ region =, spawn_set_hash = }`, which
`mission_script_runtime_dispatch.cpp` refuses unless the **whole fireteam is dead and already in
that region** (it is the hard-wipe/respawn path). So the spawn machinery is there; only a
non-wipe entry point is missing.

### Built: `select_state` now carries a spawn set, and can skip the teleport

Two options were added to `context:select_state({ region_index = }, { ... })`:

- **`spawn_set_hash`** -- the authored spawn set the party arrives at. New passive pair
  `MembershipState::spawnRegion` / `spawnRegionHash` (nothing to do with `hardWipe`, so no
  "everyone dead" gate and no client state machine), set by
  `membership::set_region_spawn()` and read by `activity_roster_snapshot.cpp` through
  `region_spawn_hash()` just before the checkpoint override (an armed wipe still wins). The
  dispatch validates the hash against the destination's spawn stem (`find_hash` +
  `pointCount != 0`) exactly like `restart_checkpoint` does, so a bad hash is refused
  (`spawn_set_unavailable`) instead of hanging the client.
  **Validated live**: `select_state({region_index=64},{spawn_set_hash=0x2EA8FB98})` lands the
  player on the ship's authored "Default" point. Without it the client picks an arbitrary point --
  that bug is fixed for every future transition.
- **`no_teleport`** -- publishes the state selection without arming the host teleport
  (`Intent::stateWithoutTeleport`). **It does not do what it was built for**: the seed
  publication alone still drags the client into loading the state, so region 65 hung anyway and
  the session fell back to `no_epoch`. Kept, but it is not the cinematic answer.

### The cinematic recipe, from the Nyxara fork's Ember mission

`github.com/Nyxaraa/Sunrise-Nyxara` @ `c4fbce1` carries **the same three cinematic files we have**
(incident codec, `mission_script_cinematic` ClientRef resolve, and a client hook that *suppresses*
loading cinematics for Ember) and the same `set_cinematic_active` -- no hidden server-side driver.
Its value is `scripts/mission_ember/opening.lua`, which plays one:

```lua
local entry = mission.states.STATE_80B3C09E_0006_0001_80B3C09A  -- bubble 6, ordinal 1
initial_state = entry,
client_state = function(context, state, event)
    local held = event.held_region_index or event.current_region_index
    -- "Only a held-region report starts the movie; a requested destination is insufficient."
    if held == entry.region_index and phase(state) == 0 then
        context:slot(cinematic):set_cinematic_active{ active = true }
    end
end,
terminated = function(...)  -- then:
    context:slot(cinematic):set_cinematic_active{ active = false }
    context:select_state(landing.initial_state)
end
```

Three things follow:

1. **`0006_0001`: ordinal 1.** Independent confirmation that a cinematic lives in the sibling
   ordinal-1 state of its bubble.
2. **A cinematic state IS enterable and the client does load it** -- Ember spawns in one. So
   "region 65 is not a playable state" is too strong: the difference is that Ember enters it as
   `initial_state` through the full launch/arrival flow, while we enter mid-mission through a host
   teleport.
3. **The timing we were missing**: activate only on a `client_state` event whose
   `held_region_index` equals the cinematic's region. Both our attempts fired while the region was
   merely *requested* and never held -- and `on_event_client_state` is not even wired in
   `mission_towerfall.lua`.

**Next steps for the mid cinematic** (`mid_cinematic._cinematic`, `slot/80b508fc/000000/0000/0006`,
region 65): wire `on_event_client_state` in the root; `select_state` to 65 with a spawn set;
activate only on the held-region report; on `on_event_cinematic_terminated` deactivate and
`select_state{ region_index = 64, spawn_set_hash = 0x2EA8FB98 }`. The open question is **why the
client never holds region 65** (endless load) when Ember's client holds its own cinematic state --
i.e. what the launch/arrival flow does that a host teleport does not. `playable = ...` in the
fork's script table has no counterpart in our runtime and may be part of it.

### What the live run actually showed (and where it now stands)

`on_event_client_state_changed`, `on_event_cinematic_started/terminated/skip_requested` are now
wired in `mission_towerfall.lua`, and every client region report is probed
(`client_state held= current= requested= spawn= teleport=`). With that instrumentation:

- **The client DOES hold region 65.** `requested=65 teleport=2` -> `held=65 current=65 teleport=3`
  (3 is `kHostTeleportSpawnState`) -> `teleport=0`, and `bootflow_step` never leaves 38. So the
  transition completes, the client stays in world, and the endless black screen is simply a
  cinematic state with nothing of its own to draw. "Region 65 is unreachable" was wrong.
- **`set_cinematic_active` is never delivered**: no refusal, no result, then
  `outcome=expired outcome_code=2` after the 60 s intent lifetime. The dispatch takes the
  `scene_lease_still_publishing` branch, which only logs at **debug** (raise the `server` channel
  in `Sunrise/settings.json`; backup at `settings.json.bak-precine`). Its detail line is the
  whole answer:

```
result=mission_seed_pending lease=ready configured=1 revision=3 published=2
pending=1 arrival=0 region=65 plan_region=65 state_row=878
```

  The mission-seed lease revision 3 (the move to 65) is **never committed as published**, so
  `publicationPending` sticks and every scene / sequence / cinematic intent behind it pends until
  it expires. Revision 2 (the move to 64) published fine.
- **It is not a transition race.** `common.lua` gained `M.select_region()` /
  `M.note_client_region()`, which serialise state changes exactly like the community script's
  `own_region` (one request in flight, the next deferred until the client reports the region it
  holds; fed from `on_event_client_state_changed`). A fully serialised chain
  64 -> held -> 65 -> held -> activate reproduces `revision=3 published=2` identically. Keep the
  guard -- it is correct -- but it is not the cause.
- **It is not a missing roster push either.** Forcing one from region 65 (any effect) produces
  `ev=activity stage=roster result=encode region=65 force=0x13`, and `0x10` in that mask is
  `kRosterForceMissionSeed`: the seed is staged and the body is encoded and sent. `published`
  still does not advance.

### Root cause: the client never enters the world in region 65

A temporary `ev=activity stage=seed_terms` line in `activity_roster_push.cpp` prints every term of
`missionSeedPending` at push time. Over a full serialised run it reads, on every single push:

```
configured=1 gen_match=1 revision=3 published=2 arrival=0 pending=1 lease_gen=2 session_gen=2
```

So the lease is healthy, the generations match, the arrival window is closed and the seed **is**
pending and forced -- yet nothing publishes. The roster lines say why. `result=encode` is
`RosterOutcome::encodeFailed` (index 5 of `kOutcomeNames`), and diffing a failing body against a
good one:

| field | region 64 (ok) | region 65 (encode) |
|-------|----------------|--------------------|
| sub / subkeys | 2 / 3 | **1 / 2** |
| bytes | 5284 | **0** |
| auth | 15 | **7** |
| held / entered | 64 / 1 | **-1 / 0** |
| force | 0x00 | 0x13 (0x10 = `kRosterForceMissionSeed`) |

**Every push, however long after the client reported `held_region_index=65` to the script, still
carries `held=-1 entered=0`, and not one roster push in the whole run ever saw region 65 as held.**
Two different sources disagree:

- the `client_state` event the script reads -> says 65;
- `membership::instantiated_region()` + `entered` (the ws-702 character write-back, "in world"),
  which the roster snapshot reads -> stay -1 / 0.

The chain is then forced: the client never enters the world in 65 -> the snapshot lacks the target
bubble -> the body is built short (`sub=1`, `auth=7`) and **fails to encode** -> nothing is staged
-> `publishedRevision` never advances -> `publicationPending` sticks -> every scene, sequence and
cinematic intent behind it expires after 60 s.

And it is **circular by construction** for a mid-mission transition: the cinematic would have to
play for the client to be in world, and the client would have to be in world for the cinematic to
be delivered. Ember escapes it because its cinematic state is the `initial_state`: the seed is
published at launch through the join/arrival flow, so no mid-mission publication is ever needed.

Three ways out, to be tried in this order:

- **(a)** enter the cinematic state as an initial state (checkpoint / restart flow) rather than by
  transition -- closest to how the shipped mission behaves;
- **(b)** find why the client does not go in-world in 65 -- it may legitimately be waiting for the
  cinematic to be active already, which would mean the shipped server publishes the seed by
  another route;
- **(c)** (rejected by the user as a bypass rather than a fix) let the seed publish without the
  client's in-world confirmation for a cinematic state.

### SOLVED -- the mid cinematic plays. `initial_state` had never worked.

**`probe_initial_state_region` (`mission_script_vm.cpp`) ran the controller's top level in a Lua
state holding only `_G`, `table` and `package`.** A controller split across `require`d files pulls
in modules that touch `string`/`math` at load (ours through `json.lua`), so the chunk always threw,
the probe returned -1, and the caller logged the indistinguishable
`no_initial_state_declared`. **`initial_state` has therefore never been applied in this project**:
`START_REGION` only chose which beat modules were installed, while the spawn region came from the
launch destination. Fixed: the probe now opens `string` and `math` (the same base libraries the
sandbox grants) and, on failure, logs the actual Lua error as
`result=load_error|run_error detail=<message>` instead of swallowing it.

With that fixed, launching into **bubble 8 / slice 65** (manual launch; the deployed root also
declared `initial_state = { region_index = 65 }`, and both agree):

```
stage=initial_state_probe result=region_applied
bootflow_step transitioned to 38                     <- in world
stage=roster result=ok bytes=921 region=65 held=65 entered=1
```

The client **does** enter a cinematic state and report `entered=1` -- the black screen is simply
the state waiting for its movie. `set_cinematic_active` then came back
`outcome=transport_staged` (delivered, not expired) and the client answered with its own incident:

```
stage=cinematic result=started registry=e8b02346 slot_type=6 slot_index=0 target=5239
```

**The mid cinematic plays.** So option **(a)** is the right model and the earlier deadlock was
never about state 65 being special: a mid-mission `select_state` into it publishes a seed
revision during the arrival window, that body fails to encode (`held=-1 entered=0` -> `sub=1`,
`auth=7`, `bytes=0`), and the lease then never publishes. Entering through the launch/arrival flow
skips that window entirely -- exactly how Ember reaches its own cinematic.

### The real bug: a carried group with no bubble key refuses the whole roster body

Reaching region 65 *at runtime* was then solved by fixing what actually broke, not by adding a
travel verb. Named every silent `return false` in `activity_sensor_auth_encoder.cpp` (18 of them,
including the ones inside `valid_client_sets`, which `valid()` calls) and had the push log the
reason. The answer was immediate and stable:

```
ev=activity stage=roster_encode result=refused reason=group_unreferenced
    groups=4 top=1 sub=1 auth=7 region=65
```

`valid_client_sets()` requires **every non-top-level group in the body to be named by a key of a
bubble sub-block**. In `activity_mission_seed_roster.cpp` the two decisions had drifted apart:

```cpp
if (canonicalPosition == canonicalGroupCount) {
    appendRows[appendCount++] = source;                   // the group is carried
}
if (!canonicalTopLevel && groupActive[source]) {
    activationKeys[activationCount++] = candidate.registryKey;  // its key only when active
}
```

A **new, non-top-level, inactive** group was therefore carried with no key -> unreferenced ->
`valid()` refused the entire body (`bytes=0`) -> nothing staged -> the mission-seed revision riding
it never published -> `publicationPending` stuck -> every scene, sequence and cinematic intent
behind it expired after 60 s. Ten lines above, the file already states the right convention for
the opposite case: *"Removal is a cleared presence bit at the old key ordinal, not omission."*

**Fix**: every carried non-top-level group now gets its bubble key, and inactivity is published as
`retired` instead of an omitted key (a retired group costs no client capacity, so nothing else
moves). This is not Homecoming-specific -- it broke **any** mid-mission state transition whose
transition subset carried an inactive group.

Result, live, with nothing else changed:

```
(no roster_encode refusal)
stage=seed_terms configured=1 gen_match=1 revision=3 published=3 arrival=0 pending=0
stage=cinematic result=started registry=e8b02346 slot_type=6 slot_index=0 target=5239
```

**The mid cinematic now plays from the bazaar**, and the whole beat runs unattended.

### Boulevard / Bazaar v1 -- complete, in `boulevard.lua`

The section plays end to end with no manual command: entry door, Ikora, the pod, the bazaar door
and its Incendior, three waves, the finale, Holliday's Hawk, the fade, the cinematic, and the
landing on the ship. Final wiring:

- **Entry**: 3 s after entering `pt_start_big_ship` the four `squad_cabal_blasted_*` are placed
  (`obj_bazaar`, task group -1, held) and `d_door_gating` opens.
- **Ikora** (`pt_start_ikora`): keys {1,2} of `scene_ikora_boulevard`, guarded by `B.ikora_started`
  -- leaving and re-entering the volume replayed her whole entrance, and the watch's own `fired`
  flag did not stop it. +1.5 s `o_nova_bomb_projectile` (her blast is an object, not part of the
  scene). +15 s the pod, +2 s later the "Board the command ship" directive.
- **Kill counting, not `alive_count`**: `B.wait_kills(label, n, fn)` waits for n further kills on
  `obj_bazaar` (slot index 1, cumulative -- the first report after arming sets the base). Both the
  pod (3) and the finale (2, `B.FINALE_MEMBERS`) use it, because a squad that arrives in a pod
  reports `alive_count` 0 transiently on landing and its living members never report again, which
  fired `M.track_clear` far too early on both.
- **Waves**: `sq_bazaar_a_a` on `pt_bazaar_mid`, `sq_bazaar_a_c` + Cue 75 on `pt_bazaar_farther`,
  `sq_bazaar_finale` once `a_c` is dead.
- **End**: Hawk (`o_hawk_1`; `_2`/`_3` are the other players') -> +2 s Cue 76 -> +7 s the fade
  (`ho_fade_out` through `_object_filter_fade_out`, **both by full slot id**, driven by us: the
  cinematic does not cover the load) -> +1 s `select_region(65)` -> on the held report
  `set_cinematic_active` -> on `cinematic_terminated` (or skip) stop it and `select_region(64)`,
  landing the player on the ship's authored spawn.
- **Straight to 65**: both states share slice set 64, so travelling directly to the cinematic
  state loads the same data -- there is no need to stop at 64 on the way in.
- Names that do not resolve and need their full slot id: `squad_invisible_shooting_target`,
  `ho_fade_out`, `_object_filter_fade_out`, `sq_ikora`.
- Unsolved, cosmetic: `squad_cabal_blasted_*` engage the player instead of holding their authored
  pose (no type-2 cell anywhere in the boulevard registry). `ho_no_combat_abilities`
  (`slot/80b505f3/000047/0047/001a`) through predicate B is the untried lead.

`M.select_region()` / `M.note_client_region()` in `common.lua` serialise every state change (one
request in flight, the next deferred until the client reports the region it holds), fed from
`on_event_client_state_changed`. The root also routes `on_event_cinematic_started`,
`_terminated` and `_skip_requested` to the beats.

Diagnostics left in place: `ev=activity stage=seed_terms` in `activity_roster_push.cpp` (prints
every term of `missionSeedPending`) and the `server` log channel at `debug`
(`settings.json.bak-precine` holds the original). Both should come out once the beat is wired.

## Leads for the next session

0. **Moving a combatant to a point is still unsolved, and the cheap options are exhausted.**
   Live (see [RE-ACTOR-PROGRAMS.md](RE-ACTOR-PROGRAMS.md)): a type-48 point set resolves for the
   path program (kind 3, either value of `follow`), for the new kind-8 program and for the action
   target — every one of them only makes the actor *face* the point. Kind 8's body is
   `{ClientRef, u8}`, proven by one extra bit stalling the client. Next step is therefore the
   **runtime kind -> class table**, read read-only from inside our DLL, before any further live
   send: a wrong body length is not refused, it freezes the game, so guessing costs one restart
   per guess (`probe_shape_allowed` now blocks the shapes we already know are wrong). The other
   open lead is the AI's own traversal — task group 13 already makes a distant fodder jet toward
   the player.
   Test rig that works: an actor **created by the program** on a squad the script never places
   (`sq_hangar_a_a_flank__cell` in the open hangar, `sq_hangar_overlook_b_a__major` before the
   doors), posed twice, re-posed before every new program. The kind → vtable dispatch is now
   fully read from the code (10 kinds, 3 = path, 9 = action); naming the other eight needs a
   runtime read of the kind → class table (read-only from our DLL) or one probe per kind. Then
   wire Underwatch squads to their objectives (`obj_cabal_first_contact`, `obj_centurion_intro`,
   `obj_post_gun`).
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
