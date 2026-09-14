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
