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
**The kind index ↔ class mapping is not in the data** (no natural order fits both kind 3 =
`f7a` and kind 9 = `f76`); it is built at runtime. Next step: read it from the live process
(read-only, from inside our DLL — never attach a debugger, the anti-tamper closes the game) or
find the polymorphic reader in code (walker kind `0x126`).

Handler vtables (`.rdata` `7ff7429ee128..7ff7429ee510`, ~10 vtables sharing slots `e0dc10`,
`a97cd0`, `a97960`, `b94700`, `10a0ba0`, `11b1db0`): unique per-kind methods `a97bb0` (path;
calls `4ffec0` type-58 resolver, `ab0c30` marker), `a97800` (action; also calls `4ffec0` +
`ab0c30` → **the action target is a type-58 path marker**, which is why every other target
kind froze), `a9c140`, `a9bab0`, `a9c060`, `a98660`, `a9bfa0`, `a9bd70`, `a9c1e0`, `a98170`,
`a9c320`, `a9bf00`. Strings right before the vtables: `"couldn't find firing point"`,
`"reference frame deleted"`, `"discard not making progress"` — one kind goes to a *firing
point* (type-44 firing areas such as `fa_fodder` exist in the hangar). Map each vtable to its
class (constructor xrefs) and each handler to its resolver to name the unknown kinds.

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
