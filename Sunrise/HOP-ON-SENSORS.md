# Type-26 hop-on sensors (`slot:set_hop_on`)

## What they are, and what this unlocks

Authored `ho_*` slots are type-26 `hop_on_sensor` rows. The package uses them to impose a state
on something already in the world rather than to spawn or move it:

- `ho_zavala_looping_bunker_anim`, `ho_zavala_looping_angry_emotion` — Homecoming's plaza, where
  Zavala holds a looping bunker animation for the whole battle;
- `ho_bubble_shield`, `ho_infinite_super`, `ho_instant_super` — the shield and super states of
  that same beat;
- `ho_noweapon_the_path`, `ho_no_combat_abilities` — the bazaar's opening walk, where the player
  is disarmed.

Before this, a mission script had no way to reach any of them: an `ho_*` slot could be resolved
but not written, so a placed Zavala fell back to his default idle (he leans on a wall that isn't
there) and a beat built on a looping pose could not be reproduced at all.

## How the body was decoded

The slot metadata gives the schema (`0x8080954B`), the component class (`0x8080953F`) and the
declared Auth width (186..276 bits). The layout itself comes from the client's own reflection
database, read with `tools/reflect_dump.py rec 8080954b`:

| offset | reflected kind | wire |
|---|---|---|
| `+0x00` | `0x02` bool | 1 bit |
| `+0x01` | `0x02` bool | 1 bit |
| `+0x04` | `0x05` uint, flag `0x80000000`, width 32 | signed 32, zero at mid-range |
| `+0x08` | same | signed 32 |
| `+0x0c` | same | signed 32 |
| `+0x10` | same | signed 32 |
| `+0x14` | `0x01` class `0x80809C42` | ClientRef, 55 bits |
| `+0x20` | `0x22` dynamic | 1 bit when empty |

`1 + 1 + 4*32 + 55 + 1 = 186`, exactly the minimum the slot metadata declares, which is what
makes the reading trustworthy before a single live send. The upper bound (276) is the same body
with the trailing dynamic field populated; this encoder always writes it empty.

The convention for the reflected kinds was calibrated against a body this codebase already
encodes byte-for-byte, the type-70 engagement observer (`0x808094F1`, 23 bits): a `0x07` field is
`width` raw bits, a `+0x100` kind is an optional field contributing a presence bit, and a
`flag`-biased integer stores zero at the middle of its unsigned range.

Getting the length right is not a nicety. A body of the wrong bit count is **not refused** by the
client: it misparses and stalls the main loop, which costs a game restart. That is why the layout
is derived from the reflection data and checked against the declared width rather than guessed.

## The API

```lua
context:slot("ho_bubble_shield"):set_hop_on{
    first = true,              -- default true
    second = false,            -- default false
    values = { 0, 0, 0, 0 },   -- four signed 32-bit values, all default 0
    target = context:slot("sq_zavala"),  -- optional; omitted writes the unset reference
}
```

The call refuses any slot that is not an exact type-26 hop-on, and the encoder refuses an
out-of-range reference index.

## What is established, and what is not

Established: the field layout, the bit count, and that the body carries one ClientRef — which is
the only field whose purpose is obvious, since a hop-on has to name what it acts on.

Not established: what the two booleans and the four signed values mean. They are therefore passed
through from the script verbatim instead of being wrapped in invented names, so the meaning can be
swept live without rebuilding the DLL. Once the sweep identifies them, the API should grow named
arguments and this file should say what they are.

## Why this matters more than it looks (Homecoming's plaza, 2026-09-14)

Zavala's actor class (`0x80BFA696`) declares **zero** actor states -- verified live against a
control in the same run, where the Centurion and the Cabal Legionaries each declare fourteen.
So the one recipe that drives every other combatant in this mission, creating the actor with a
pose through `play_actor_action` on its type-2 cell, **cannot apply to him**: there is no state
to send.

That is visible in game. Driving `sc_zavala_bunker_shield_bunker` makes the scene spawn its own
Zavala, who stands in a **T-pose for two to three seconds** before the scene places him -- an
actor created with no state at all. The shipped mission shows no such thing, so its Zavala is
already in a state when the scene reaches him, and `ho_zavala_looping_bunker_anim` is the only
thing in the authored data that can put him there.

The shield has the same shape of answer. `bubble_shield_1` can be instantiated (its type-34
object filter has to be armed first, or the object does not appear at all -- see below), it
lasts the shipped ~6 seconds, but it appears at its own authored spot, not where the scene's
Zavala raises his, and it does not protect the player from the scene's salvo. The protecting
shield belongs to the actor the scene drives, not to a free-standing object.

Side finding, worth its own line: **an authored object whose type-34 filter designates nobody
does not instantiate.** `bubble_shield_1` sent alone did nothing; the same send after
`set_object_filter{players = true}` on `_object_filter_ho_bubble_shield` brought the bubble up.
That likely explains other "accepted but invisible" objects.

## Files

- `src/middleware/bap/activity_message/hop_on_auth.h` — constants, `Request`, `encode`.
- `src/server/activity/mission/mission_script_lua_slot_api.cpp` — `exact_hop_on_slot`,
  `slot_set_hop_on`, and the `set_hop_on` entry in the slot metatable.
