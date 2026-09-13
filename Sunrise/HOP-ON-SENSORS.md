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

## Files

- `src/middleware/bap/activity_message/hop_on_auth.h` — constants, `Request`, `encode`.
- `src/server/activity/mission/mission_script_lua_slot_api.cpp` — `exact_hop_on_slot`,
  `slot_set_hop_on`, and the `set_hop_on` entry in the slot metatable.
