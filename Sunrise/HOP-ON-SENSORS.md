# Type-26 hop-on sensors (`slot:set_hop_on`)

## Correction (2026-09-14, later): hop-ons are the upstream "mission effect", and the reference is the filter

The body this file decodes is not new: `mission_effect_auth.h` (upstream 1AU work, commit
`2fdf2dc`) already encodes the same class `0x8080954B`, with its semantics, and
`slot:set_mission_effect{filter =, enabled =, revision =}` has been on the `validate_auth`
allowlist all along:

- the second boolean is **disabled**;
- the fourth signed value is a **revision**, positive, and a new revision re-attaches the effect;
- the ClientRef is **the hop-on's type-34 object filter**, not the entity the effect lands on --
  the client attaches the authored effect to whatever that filter selects;
- the trailing `0x22` field is an **inline predicate list** of the same type the type-34 filter
  body uses (its maximum, 91 bits, is exactly `kType34PredicateMaximumBitCount`); the upstream
  encoder writes it empty and references the filter slot instead.

So every `set_hop_on` experiment above sent revision 0 and pointed the reference at a squad, a
cell or an object instead of a filter, and the type-1 squad was what stalled the client. The
earlier conclusion that "the payload is the trailing dynamic field" is withdrawn: the payload is
the filter reference plus a revision. `set_hop_on` duplicates `set_mission_effect` with the wrong
field meanings and is to be removed.

**Validated live.** `_object_filter_ho_bubble_shield` armed on players, then
`ho_bubble_shield:set_mission_effect{filter = that filter, revision = 1}` gave the player a
body-hugging shield and a near-invulnerability buff; `set_mission_effect{enabled = false,
revision = 2}` removed both. Attaching `ho_zavala_looping_bunker_anim` through `of_filter_zavala`
armed with `inside_any = {the volume around Zavala}` changed nothing on Zavala: volume predicates
do not appear to select a combatant.

**What selects an actor.** The type-34 filter has thirteen predicate classes
(`0x80809571..0x8080957D`, all children of `0x8080957E`). Five carry a reference, and four of those
share one layout -- a 2-bit mode and a ClientRef -- differing only by class identity: `SlotRefA`,
`SlotRefB`, `SlotRefD` and the unregistered one were never used by the Lua verb, which only emits
`FlagSlotRef` (players, volumes) and `SlotRefC` (a type-4 object). `set_object_filter` now takes
`ref_predicate = "a" | "b" | "c" | "d" | "unregistered"`, `ref_target = <slot>` and `ref_mode` so
the one that matches a squad or a combatant can be found live. A 9 720-name FNV-1/FNV-1a dictionary
did not recover any of their class names.

## First delivered send: the client stalled (2026-09-14)

With the allowlist fixed, the first hop-on that actually reached the client --
`ho_zavala_looping_bunker_anim`, `first = true`, `second = false`, all four values 0,
`target = sq_zavala` (a type-1 squad) -- was transmitted (`outcome=transport_staged`, no refusal)
and **froze the game**. The length cannot be the cause (it matches the slot metadata), so some
field is being read as something it is not. Two candidates, in order:

1. **The reference type.** The actor-program handlers accepted only two slot types for their
   ClientRef and turned any other into a `0xFFFFFFFF` handle used as a table index -- a stall.
   A hop-on applied to a type-1 squad reference is the same shape of mistake.
2. **A value used as an index.** Logical 0 goes out as `0x80000000` under the assumed bias; if one
   of the four integers is a handle or index, that alone is a garbage lookup.

Until the native consumer of `0x8080954B` is read, `set_hop_on` refuses to send unless the call
passes `acknowledge_stall_risk = true`: every send costs a game restart if wrong.

## Correction (2026-09-14): the live sends never left the server

Everything this file said about hop-ons being "accepted without a stall and without an observable
effect" is void. `activity_sdk_devices::detail::validate_auth` admits Auth bodies only from an
allowlist of schemas this tree encodes, and neither the hop-on schema (`0x8080954B`) nor the
toggle schema (`0x8080955A`) was on it. Every `set_hop_on` call was therefore refused on the
server as `slot_auth_refused / invalid_body` before transmission. The script only ever saw its
own queuing succeed (`ok=true`), and the refusal line was not being looked for. It surfaced when
the first `set_toggle` was checked against the log.

Both schemas are now on the allowlist. None of the earlier hop-on experiments (the flag and value
sweeps, the squad and cell targets, the filter variants) tells us anything about how the client
handles a hop-on, and all of them have to be run again. What still stands: the decoded layout,
the width check against the slot metadata, and the fact that the client never reports hop-on
Sense (that observation came from the client's own packets, not from our sends).

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
