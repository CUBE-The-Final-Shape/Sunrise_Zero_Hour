# Scene / Authored Scene Findings from the Upstream `stanuwu/Sunrise` Repo

Source: `https://github.com/stanuwu/Sunrise` at commit `c4fbce1a564f8a962fbd3ce3c464553e519eed79`,
analyzed by fetching the full commit tree and reading the relevant files in full. Same project
(`sunrise::` namespace, identical directory layout under `src/client/content/activity/`,
`src/server/activity/mission/`, `src/middleware/bap/activity_message/`) but a materially more
advanced state of the exact problem this repo's [CLAUDE.md](CLAUDE.md) was blocked on: where a
type-43 authored scene's external event-gate keys actually live, and the correct wire shape for
sending them. This file records what that repo answered, and what was changed here as a result.
No game assets were reproduced — everything below is clean-room reverse-engineering code/docs
already public in that repo.

## 1. The event-gate keys are inline in the graph body, not behind the header's array pointer

[CLAUDE.md](CLAUDE.md) (previous session) mapped the authored-scene graph's header as a list of
`{class, countA, countB, 0}` entries followed by `{offset, 0, typeHash, 0}` entries, and found that
for class `0x8080637D` (the external event-gate node class, 192 bytes/node) the array's declared
`offset`/`countB` fields were **zero even though the header said 5 such nodes exist** — the
previous session's conclusion was that the keys must live somewhere else entirely (the activity's
behavior graph, or a type-31 trigger slot's own descriptor).

The upstream repo's `tests/verify_ember_explosion_content.py` proves that conclusion wrong. It
reads Ember's own explosion scene graph (`80BEB1CC.bin`, the graph parallel to this project's
`scene_cabal_first_contact`'s `0x80BEB7EA`) and asserts, for each of Ember's four documented
events:

```python
for suffix, offset, key in [('a', 0x5BC0, 0x329EB106), ('b', 0x5C20, 0x633B82E9),
                            ('c', 0x5C80, 0x15A78938), ('d', 0x5CE0, 0xF9D55A83)]:
    assert struct.unpack_from('<I', scene, offset - 12)[0] == 0x8080637D
    assert struct.unpack_from('<I', scene, offset)[0] == key
```

**The FNV-1 event key sits exactly 12 bytes after its node's `0x8080637D` class-marker dword.**
The four nodes are `0x60` (96) bytes apart. `docs/mission-ember-final-corrections.md` (around
line 139–156) documents the same table with an explicit "Graph key offset" column.

This is consistent with a fact this repo had already confirmed independently:
`activity_sdk_actor_engine_semantics.inc` gives `0x8080637D` a `fieldCount` of 0 — the generic
schema decoder never traverses into it, which is exactly why no schema-driven inventory tool ever
surfaced these keys and why they had to be found by direct byte scanning instead.

**Practical consequence:** to find a scene's real event keys, don't trust the graph header's
array-offset/count fields for class `0x8080637D` — scan the raw graph bytes for the literal dword
`0x8080637D` and read the `uint32` sitting 12 bytes after each match.

## 2. The wire protocol: zero dependencies and a constant generation, confirmed by a working implementation

Upstream's `src/middleware/bap/activity_message/scene_events_auth.h` is functionally the same
message this repo already had a draft of, but correct: signed generation (biased `+0x80000000`),
`stop=false` (1 bit), **dependency count = 0** (4 bits, hardcoded, not a parameter), **scalar = 0**
(31 bits), then a 6-bit event count and up to 32 32-bit event keys (`74 + 32*n` bits — matches the
bit width this repo had already deduced from the Ember doc). There is no dependency-reference
concept anywhere in that file — it was never part of the real message, at all.

`tests/scene_events_test.cpp` includes a byte-exact fixture: Ember's four known keys encoded at
generation 1 produce the wire bytes
`8000000100000000010ca7ac4198cee0ba4569e24e3e7556a0c0` (26 bytes / 202 bits). This is a ready-made
oracle for verifying any local encoder against.

Upstream's `scripts/mission_ember/apex.lua` shows the intended calling convention: each trigger
fire flips a durable per-index variable, then the **entire cumulative event list** is recomputed
from that durable state and resent at **one constant generation**; the generation is bumped only
on an explicit reset/wipe (`scene::encode(2, {}, ...)` — "new generation, cleared event history"),
never on an ordinary event addition. Bumping generation on every publish (as this repo's deployed
mission script was doing) restarts the graph and discards every previously committed key.

## 3. The correct call is a slot-level Auth patch on the type-43 slot itself — not a separate scene/dependency API

Upstream's Lua entry point lives on the **slot**, not a distinct scene handle:
`src/server/activity/mission/mission_script_lua_slot_api.cpp`'s `slot_set_scene_events()`
validates `slotType==43`, `componentClass==0x80806382`, `authSchema==0x8080626B`
(`scene_events::kSchema`), encodes the body, and sends it through `queue_slot_auth()` — **the same
generic mechanism every other authored slot type uses** (type-2 combatant, type-30 occupancy,
ghost link, music section, mission effect, etc.), not a bespoke scene/bind/dependency call.

This repo already had exactly that shape (`slot_set_scene_events` in
[mission_script_lua_slot_api.cpp](src/server/activity/mission/mission_script_lua_slot_api.cpp)
routing through `queue_slot_auth`) — the architecture was already right. The bug was narrower:
this repo's version additionally accepted and encoded a `dependencies` argument (a `Dependency`
list encoded as wire `ClientRef`s), which the upstream implementation shows is simply not part of
the message. The generic `scene:activate{}` call
([mission_script_lua_scene_api.cpp](src/server/activity/mission/mission_script_lua_scene_api.cpp))
still exists and is unrelated: it only publishes a generation with an empty event list and does
not connect any trigger to the scene by itself. Event delivery and generic activation are two
separate calls that both need to happen (this repo's mission script already does both, as
`bind_scene()` then `advance_scene()`).

## 4. There is no generic trigger→event-key binding table — it's authored by hand

The upstream repo's type-31→type-60 trigger resolution
(`mission_script_player_trigger.h/.cpp`) is a distinct, pre-existing mechanism (its own incident
delivery path) unrelated to scene event keys. `scripts/mission_ember/apex.lua` maps each trigger
firing to a specific authored key via a **hardcoded Lua table** keyed by index, not a generic
runtime lookup. This means the local project's lead #1 ("find a generic binding table in the
activity's behavior graph") was a dead end: Bungie's original trigger→event-name mapping had to be
recovered once (by the offset-scan technique above, against the game's own data) and then hand
transcribed into script code — there is no shortcut around discovering the real keys for
`scene_cabal_first_contact` and `sc_centurion_intro` specifically.

## What changed in this repo as a result

- **[scene_events_auth.h](src/middleware/bap/activity_message/scene_events_auth.h)**: removed
  `Dependency`, `encode_with_dependencies()`, `kMaximumDependencies`,
  `kMaximumBytesWithDependencies` — all confirmed dead/wrong. The remaining `encode()` always
  writes a dependency count of 0 (previously done anyway when called with no dependencies; now
  it's the only path).
- **[mission_script_lua_slot_api.cpp](src/server/activity/mission/mission_script_lua_slot_api.cpp)**:
  `slot_set_scene_events` no longer accepts a `dependencies` argument; it only takes `generation`
  and `events` and always calls the corrected `encode()`.
- **[content_resolver.h](src/client/hooks/content_resolver/content_resolver.h) /
  [.cpp](src/client/hooks/content_resolver/content_resolver.cpp)**: added
  `find_event_gate_keys(hash, outKeys)`, which resolves a blob (an authored scene's graph tag) and
  returns the key sitting 12 bytes after every `kEventGateNodeClass` marker it finds — the native
  implementation of the scan technique from finding #1 above.
- **[mission_script_vm.h](src/server/activity/mission/mission_script_vm.h)**: added the
  `FindEventGateKeys` function-pointer typedef and `DefinitionApi::findEventGateKeys` field.
- **[mission_script_sdk_bridge.cpp](src/server/activity/mission/mission_script_sdk_bridge.cpp)**:
  added the `find_event_gate_keys` bridge function and wired it into `definition_api()`.
- **[mission_script_lua_internal.h](src/server/activity/mission/mission_script_lua_internal.h) /
  [mission_script_lua_context_api.cpp](src/server/activity/mission/mission_script_lua_context_api.cpp)**:
  added `context:find_event_gate_keys{hash = ...}`, returning a Lua array of the keys found (up to
  32).
- **`scripts/mission_towerfall.lua`** (deployed script, not tracked in this repo — lives at
  `E:\Sunrise\Game\bin\x64\Sunrise\scripts\mission_towerfall.lua`):
  - `bind_scene()` no longer bumps a per-scene generation counter or sends a dependency reference;
    it publishes a fixed `SCENE_GENERATION = 1` and the scene's cumulative discovered event keys.
  - Added `discover_scene_event_keys()`, called once at the `bootflow_step == 38` (in-world) spawn
    signal, which calls the new `context:find_event_gate_keys{hash = <graph tag>}` for every known
    scene graph tag (currently only `scene_cabal_first_contact`'s `0x80BEB7EA` — `sc_centurion_intro`'s
    graph tag is not yet known, only its config/resource/schema chain) and probe-logs whatever keys
    it finds.

## What is still unresolved

- **`sc_centurion_intro`'s graph tag is unknown.** Its config (`0x80B509A1`), resource
  (`0x80B82774`) and schema (`0x80C3DF17`) are known from the prior session, but the graph tag
  (the schema record's own reference, at resource `+0x88`) still needs a `context:dump_hash{}` walk
  from `0x80C3DF17` to identify, the same way `0x80BEB7EA` was found for the Cabal scene.
- **The actual event keys for both Homecoming scenes are still unverified against a live
  process.** `find_event_gate_keys()` has not yet been run in-game — it needs the player to
  actually be in the activity (content is not resident at attach) and a rebuild+redeploy of the
  native hook. The next live session should watch the `discover_scene_event_keys` probe lines.
- **Whether nonempty event keys are actually required for `scene_cabal_first_contact` and
  `sc_centurion_intro` to visibly play is still unconfirmed.** It's possible these two scenes'
  gates are optional dressing (e.g. secondary VFX triggers) rather than required for the scene's
  core animation, in which case fixing the generation/dependency bug alone (already done) may be
  enough to make scenes play even with an empty event list — this is now cheap to test live, since
  the wire body is finally correct either way.

## Verification pass — offline, against the dumps already on disk

Everything in this section was checked *after* the changes above were made, without running the
game and without modifying any code. It confirms one of the changes outright, finds one blocking
bug in another, and narrows the remaining unknown.

### The encoder is now byte-exact against upstream's fixture — confirmed

The corrected `encode()` was replayed bit for bit (MSB-first, matching
`middleware/encoding/bit_writer.h`) with generation 1 and Ember's four documented keys:

```
produced        : 8000000100000000010ca7ac4198cee0ba4569e24e3e7556a0c0   (202 bits, 26 bytes)
upstream fixture: 8000000100000000010ca7ac4198cee0ba4569e24e3e7556a0c0
```

Exact match. Field order, widths, the `+0x80000000` signed bias, MSB-first bit order, the
hardcoded zero dependency count and zero scalar are all correct. **The wire side of this problem
is now settled** — the first time that can be stated with evidence rather than inference.

### `find_event_gate_keys()` has a blocking bug: the scan is not bounded to the record

`find_event_gate_keys()` calls `read_blob()`, which reads up to `kMaximumBlobBytes` (256KB) from
the resolver's pointer. That is the whole committed region, **not the record**. A record's real
length is the `uint32` at its own offset 0 (verified repeatedly: the scene graph, the resource
entity and the placed-object config all carry it). Scene graph `0x80BEB7EA` is only `0x10F8`
bytes, so the current implementation walks ~62x past the end of it and into unrelated neighbouring
records.

Measured on the dumped blob, the function as written would return **96 "keys"**, including values
such as `0x8080638A` (a package class id), `0x00000000` and `0xFFFFFFFF`. The last two are fatal:
`scene_events::encode()` explicitly rejects zero and `kInvalidEventKey`, so `set_scene_events`
would return false and send nothing at all. **Fix before the next live test:** clamp the scan to
`min(record size at offset 0, blob size)` and skip keys that are `0`, `0xFFFFFFFF`, or in the
`0x8080xxxx` package-class band.

### Bounded correctly, our graph tag yields zero keys — the graph tag itself is the open question

Within `0x80BEB7EA`'s actual `0x10F8`-byte record there is exactly **one** `0x8080637D` dword, at
offset `0xB0`, and the `uint32` twelve bytes after it is `0`. That offset is the header's
*declaration* entry (`{class, countA=5, countB=0, 0}`) described in [CLAUDE.md](CLAUDE.md), not a
node. Ember's graph is large enough to hold its nodes inline at ~`0x5BC0`; ours is 4344 bytes and
holds none.

So finding #1 is not contradicted, but it does not yet apply here. Either `0x80BEB7EA` is not this
scene's real graph — it was inferred from an entry in schema record `0x80C3DF16`, never confirmed
the way Ember's was — or the gate nodes live in a sibling record.

### The technique itself is valid on this game's data — real key runs exist nearby

Scanning all ~530 dumped blobs for chains of `0x8080637D` markers spaced exactly `0x60` apart
whose marker+12 value is FNV-shaped (not zero, not `0xFFFFFFFF`, not `0x80xxxxxx`) finds genuine
runs:

```
385838EC, B687F07B, DA1603C7, A31FE6A6     (run of 4)
385838EC, 6F51AC66, CFB8CB39               (run of 3, shares its first key)
```

They recur at many addresses only because the 256KB dump windows overlap the same memory. This is
exactly the shape upstream's test fixture asserts, so the offset-scan technique does work against
this install — **only the mapping from a named scene to the record holding its nodes is missing.**

### Ordered plan for the next live session

1. Bound the scan in `find_event_gate_keys()` and filter invalid keys (above). Without this the
   first call poisons the event list and `set_scene_events` fails outright.
2. Test the cheapest hypothesis first: publish a **constant generation with an empty event list**
   and zero dependencies, then `scene:activate{}`. The body is finally valid; if the gates are
   optional dressing this alone may make the scene play.
3. Identify the record holding the `385838EC / B687F07B / DA1603C7 / A31FE6A6` run and work out
   which tag owns it (walk `dump_tree` from `0x80C3DF16` and `0x80C3DF17` and compare record
   start addresses). If that record belongs to one of our scenes, its keys are the answer.
4. Only then resolve `sc_centurion_intro`'s graph tag the same way.

