# RE notes — client reflection, squad Auth fields, actor-program kinds (2026-09-13)

Static work on the Ghidra dump `destiny2_live_dump.bin` (Ghidra base `0x7ff740df0000`; the file
copy `E:\Sunrise\destiny2_live_dump.bin` is the same image at base `0x7ff729760000`, so
`file_offset = va - base`). Upstream RVAs (image base `0x140000000`) are the same build:
`rva + base` works for everything quoted from the removed Ember notes (commit `4593154`).

Tool: `tools/reflect_dump.py` (pure Python, reads the dump file; no Ghidra needed).
`python tools/reflect_dump.py rec <tag>...` prints a class descriptor with its field table,
`... kids <tag>` lists subclasses (records whose parent field is the tag), `... stream` rebuilds
`tools/reflect_stream.pkl` (index of the 20 428 type records). `tools/reflect_refs.py` finds
records referencing a tag.

## Reflection database layout (decoded)

- Registry at `7ff744006b80..7ff744014c00`: 1 769 entries of 32 bytes `{tag u64, descriptor*,
  x*, back*}` sorted by tag descending. `back` points into a `{descriptor*, tag}` index array
  (`7ff742e3fcc0..`, `7ff742e44090..`). `x` is not the parent (points at unrelated classes).
- Type records: one contiguous stream of 8-byte-aligned records sorted by **FNV-1 name hash**
  (`7ff74455e4c0..7ff74483ef00`). Record header: `+0 size u64`, `+8 nameHash`, `+0x10 parent
  tag (0xffffffff = none)`, `+0x14 struct size`, `+0x18 flags (0x0b04/0x0b08/...)`, `+0x48 attr
  list length`, `+0x58.. attribute pairs (a, b)`; then at `+0x48 + len`: `{fieldCount, 0, tag,
  size, size, align, 0, 0}` and the field table (40 bytes each): `{offset, offset, wireIndex,
  0x3f000000, kind, classTag|-1, flag, width, 0, 0}`. Records of size 0x5c–0x64 carry no field
  table (abstract/opaque; their tag is only in the registry).
- Field kinds: `0x01` nested class, `0x02` bool, `0x03` small int (flag 1 = bias one; flag 128
  = bias 128), `0x05` uint(width), `0x07` bits(width), `0x09` u32, `0x0b` float; `+0x100` =
  optional (presence bit, `wireIndex` = the `.N` field number). `0x126` = polymorphic body
  slot. Attribute `8080001d -> X`: X is a family/enum id (307 records share `80809ed8`).
- Names are not stored, only FNV-1 hashes (e.g. `80807f71 -> e71677e8`) — a dictionary attack
  on class names is possible later.

## Squad Auth body `0x80807EC9` (struct 0xC4 bytes, 21 fields)

| wire | offset | type | meaning |
|---|---|---|---|
| .0/.1 | +0x00/+0x08 | ClientRef `80809c42` | objective ref, ? |
| .2 | +0x10 | class `8080991d` | refused by upstream |
| .3/.4 | +0x2c/+0x50 | class `80807ecf` | requested counts |
| .5 | +0x74 | class `80807ed2` | authored profile (4 lanes) |
| .6 | +0x7c | uint31 | spawn generation |
| .7/.8 | +0x80/+0x84 | u32 | unknown |
| .9/.10 | +0x88/+0x90 | ClientRef | unknown |
| .11/.12 | +0x98/+0xa0 | ClientRef | spawn references (.11 = type-66 rule, verified) |
| .13 | +0xa8 | uint31 | objective revision |
| .14 | +0xac | uint31 | unknown |
| .15 | +0xb0 | uint6 bias1 | unknown |
| .16 | +0xb4 | uint5 bias1 | task group |
| .17 | +0xb8 | uint31 | unknown |
| — | +0xbc / +0xbd | int2 / int3 bias1 | active / mode |
| .18 | +0xc0 | u32 | name hash |

Live results on `.12` (this session): a **type-66 rule** in `.12` is accepted (no freeze, sent
five times) but changes nothing visible; point set / nav point in `.12` freeze the client
(earlier sessions). The jetpack hop seen once after a `.12` send was the AI (see below), not
`.12`.

## Combatant (type-2) Auth body `0x80807DA1` (struct 0x958)

`+0 gen u31`, `+4 mode int2`, `+5 marker int3`, `+6 enabled bool`, `.4 +0x08 class 80807da7`,
`.5 +0x4c class 80807dac`, **`.6 +0x100 class 80807f6f` (program, 0x810 bytes)**, `.7 +0x910
class 80807ed9` (manifest = passenger delivery).

Program chain: `80807f6f {rev u31, hdr uint6, 80807f72}` → `80807f72 {hdr uint6, polymorphic
80807f71 (presence + 4-bit kind)}` → `80807f71` (parent `80807f73`, one `0x126` body slot at
+0x40) → body = one of the **10 subclasses of `0x80807F84`**:

| class | wire body | known |
|---|---|---|
| `80807f76` | u32, u32, u32, ClientRef, int3(bias1), int8(bias128) | **action** (kind 9): group, action, +identity, target ref + mode + marker |
| `80807f7a` | ClientRef, uint8, bit | **path** (kind 3): type-58 path, marker, follow flag |
| `80807f77` | ClientRef, uint8 | unknown (ref + marker) |
| `80807f7f` | ClientRef, uint8 | unknown (ref + marker) |
| `80807f78` | u32, bool | unknown |
| `80807f7e` | u32 | unknown |
| `80807f81` | u32, float | unknown |
| `80807f7d` | float | unknown (wait/timer?) |
| `80807f80` | bits6 | unknown |
| `80807f79` | (empty) | unknown (stop/idle?) |

`80807f83 {int4 bias1}` (enum family `80800037`) is the kind enum, `80807f7b {bit}` another enum.
**The kind index ↔ class mapping is not in the data**: it is built at runtime. What *is* in the
code is the kind → behaviour dispatch, now fully read (next section), which confirms there are
ten kinds and which two we already use. Naming the other eight still wants a live read of the
runtime table (read-only, from inside our DLL — never attach a debugger, the anti-tamper closes
the game) or a probe run per kind.

## The kind dispatch, read straight out of the code (2026-09-13, second pass)

Two kind-indexed jump tables settle the dispatch without any runtime read:

- **Handler selection** at `0a9757d`: the caller holds an array of programs at `+0x18`
  (stride `0x40`, index at `+8`, count at `+0x10`); `ab47f0` returns the program's kind, and
  the jump table at **`0a977cc`** (image-base relative, 10 entries — entry 10 onwards is
  `cccccccc`, so there are **exactly 10 kinds, 0..9**, confirming that our 4-bit wire nibble is
  `kind + 1`) picks a case that stores one vtable into the owner at `+0x820`.
- **Body construction** at the same shape, jump table **`0a9921c`**, one in-place constructor
  per kind (this is where each body's defaults come from: kind 3 zeroes a `u32` to `-1`, a
  qword to `-1`, and a float to `0x7f7fffff`).

The `.rdata` block at `1bfe128` is **11 vtables of 11 slots** (`0x58` apart). `vt0`
(`1bfe128`) is the shared second vptr every case also stores; the other ten are the kinds:

| kind | vtable | unique methods | what it resolves |
|---|---|---|---|
| 0 | vt4 `1bfe288` | `a9c140` | entity lookup only |
| 1 | vt7 `1bfe390` | `a98660`, `a979c0` | — |
| 2 | vt2 `1bfe1d8` | `a97a00` | — |
| **3** | vt3 `1bfe230` | `a97bb0`, `a9bf00`, `a9c320`, `a9c4a0`, `a979d0` | **path** (`4ffec0` + `ab0c30`) |
| 4 | vt1 `1bfe180` | none (base behaviour) | — |
| 5 | vt5 `1bfe2e0` | `a9bab0` | — |
| 6 | vt6 `1bfe338` | `a9c060` | — |
| 7 | vt9 `1bfe440` | `a9bfa0` | — |
| 8 | vt10 `1bfe498` | `a9c490`, `a9bd70`, `a9c1e0`, `a97a70` | twin of the path vtable, no ref resolve |
| **9** | vt8 `1bfe3e8` | `a97800`, `a98140` | **action** (`4ffec0` + `ab0c30`) |

So kind 3 = path and kind 9 = action are now confirmed from the code, not inferred, and the
`80807f76` layout is confirmed by `a97800`'s reads: `+0` group, `+4` action, `+8` identity,
`+0xc` ClientRef, `+0x14` mode (int3), `+0x15` marker (int8).

The class ↔ kind table for the eight remaining kinds is still unmapped: the descriptors are
referenced from nowhere but the registry, no tag constant appears in code, and the base
`80807f84` record is abstract with no subclass list — so that binding really is built at
runtime and needs a live read (or behavioural probing: each kind's body has a distinct bit
length, so a wrong layout is refused by the decoder).

## The action target is a point set or a path — and that is why the others froze

`4ffec0` (called by both `a97bb0` and `a97800` on the body's ClientRef) branches on the
reference's **slot type byte** and accepts exactly two values:

- `0x30` = **48, a point set** → `c4a140(ref, 1)` writes the handle directly;
- `0x3a` = **58, an authored path** → `c4a140(ref, 0)` plus the path's own point indirection.

Anything else falls straight through and leaves the out `u32` at the `0xffffffff` it was
initialised with. `a97800` then does `sar 13` / `and 0x1fff` on that `-1` and indexes the
entity table with the result: a garbage pointer, dereferenced on the main loop. **That is the
stall** we blamed on "unknown reference kind" — the type-43 scene slot and type-1 squad slot
were simply not of the two accepted types. With a valid reference, `ab0c30(object, marker)`
resolves the marker (the body's `+0x15`, i.e. `target_marker`) and `aaf990` returns the
position the action plays at; when the reference is absent the handler writes a default and
returns success, which is our shipped no-target form.

Encoder and script API now refuse any other type (`combatant_auth::spatial_reference_slot_type`),
so a bad target is a Lua error instead of a freeze.

### Validated live (13 sends, 2026-09-13)

An action with `target = ps_explosion_a` (type 48) and `target_marker = 0` or `1` was sent
repeatedly on two different actors: **accepted every time, and not one hitch or stall** — the
first spatial target ever to survive, and the direct confirmation of the resolver reading.
It does **not** relocate the animation, though: actor state 9 plays at exactly the same spot
with and without the target, and with either marker. The handler resolves the reference to a
position and returns it to its caller; consuming it is up to the animation, and this one does
not. So a *displacement* has to come from the path program (kind 3), whose handler exists for
that — hence `play_actor_path` now accepting a point set (below).

### Kind 8 decoded live: `{ClientRef, u8}`, and a wrong body length stalls the client

With the generic probe API (`slot:play_actor_program{kind =, ref =, marker =, word =,
float_bits =, bits6 =, bit =}`, which can emit any of the ten kinds with any of the decoded body
shapes):

- **kind 8 + reference + marker**: accepted, and the actor **turns toward the point** — same
  signature as the path program, no movement. So kind 8 is one of the two `{ClientRef, uint8}`
  classes (`80807f77` / `80807f7f`), established without recovering its name.
- **kind 8 + reference + marker + one extra bit**: **stalls the main loop.** The stall probe's
  idle counter dates the freeze to the moment of that send. A body of the wrong bit length is not
  refused, it misparses — so the bit length is the discriminator *and* a wrong guess costs a game
  restart. The three kinds queued behind it in the same sweep (0, 1, 2) were sent while the client
  was already frozen: not tested, not implicated.

Consequence for method: **never batch shape guesses**, and never send a shape to an unknown kind
casually. `combatant_auth::probe_shape_allowed` now enforces the shapes of the kinds we know
(3 needs the trailing bit, 8 must not have it, 9 has its own encoder) and an unknown kind may be
probed one shape per run. Naming kinds 0-2 and 4-7 is better done by reading the runtime
kind -> class table from inside our DLL (read-only) than by paying a restart per guess.

### Three programs take a reference; none of them walks

`play_actor_path` now accepts a point set (type 48, `marker` = point index) as well as a type-58
path, and `follow` exposes the body's trailing bit. Live, on an actor created by the program:

| program | result |
|---|---|
| kind 3, point set, `follow = true` | turns toward the point, hands control back, no step |
| kind 3, point set, `follow = false` | nothing at all, not even the turn |
| kind 8, point set | turns toward the point, no step |
| kind 9 (action), point-set target | animation plays, unchanged, at the actor's own spot |

So a point set resolves to a position that every one of these programs *uses for facing*, and
none of them uses for locomotion. `follow = true` reading as "traverse the authored curve" fits:
a point set has no curve to traverse. Moving a combatant to an arbitrary point therefore still
has no known mechanism, and the shipped jetpack arrival stays unexplained — the remaining leads
are the six unnamed kinds and the AI's own traversal (task group 13 already makes a distant
fodder jet toward the player).

Also: `ps_explosion_a` holds a **single point** — marker 0 resolves, marker 1 silently falls back
to the default, which retro-explains every "marker 1 did nothing" observation, action included.

### The rule that actually governs actor programs

Half a session was lost to this, so it is the headline: **a program drives an actor only if the
program created it.** `play_actor_action` on the cell of a squad that is *not placed* creates a
docile, immobile actor which then plays every action sent to it (the Centurion recipe). An actor
that the squad spawned — `squad:place`, with or without a combat objective, passive `task_group`
-1 or an active group — ignores the same sends. Reproduced both ways on `sq_hangar_fodder_a`
(spawned: silent) and `sq_hangar_overlook_b_a__major` (created by the program: plays).

Corollaries established the same way: the double send is needed for **every** action including a
pose (a single send does not take on a live actor); actor state 9 is a **climb-and-drop
traversal** (up like a ladder, down with a sideways jump), not the impale the notes claimed —
the impale in the shipped beat is the scene's doing; and after `retire_squad` a squad must be
re-placed with **explicit counts**, otherwise it stays at zero members and every later send
reports `ok=true` with no actor behind it. States 1, 3 and 4 were re-swept with the double send
and are plain poses, confirming the earlier sweep.

Handler vtable context: strings right before the block are `"couldn't find firing point"`,
`"reference frame deleted"`, `"discard not making progress"` — one of the unnamed kinds goes to
a *firing point* (type-44 firing areas such as `fa_fodder` exist in the hangar).


## Objective / task groups (validated live, the big win of the session)

- `slot:assign_combat_objective{objective=, revision=, task_group=}` must be sent **before**
  `place` (ignored on a living squad), with one revision per mission run (the client remembers
  the last revision per squad across mission restarts in one process; a repeat is ignored →
  immobile Cabal). A later assignment on a living squad is honoured only at the **same
  revision** (group change); a higher revision is ignored.
- `obj_hangar` task groups on `sq_hangar_fodder_a`: -1 immobile; 0/1/2/7/8/9/11 walk to the
  overlook areas via the stairs; 3/4/5/17–20 leave the streamed area (despawn after one shot);
  12/13/16 hold the explosion-A area (`fa_fodder`) with jetpack hops; 14 explosion B; 15 the
  overlook above explosion B. The client publishes `squad_state.task_costs` (group+1 → cost,
  quantised 8.1/24.3/40.5/…, 2040 = unreachable) piecemeal and noisily; the script now waits
  1 s after the first costs, picks the cheapest reachable group once and keeps it unless it
  becomes unreachable (`common.lua`: `M.assign_objective`, `M.spawn_squad{objective=}`,
  `M.dispatch_squad_state`).
- The "jetpack arrival" of the fodder is the group-13 AI jetting toward a distant player; a
  live group change 13 → 14 did not move them (to investigate), so the "hide behind the ship
  then return" idea is not yet doable.
