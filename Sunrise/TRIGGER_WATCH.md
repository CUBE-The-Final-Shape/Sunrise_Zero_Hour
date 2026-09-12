# `slot:watch_trigger()`: client-side player-trigger-volume detection

## The problem

Destiny 2's activity SDK has a native path for reporting a local player crossing an authored
type-31 trigger volume: the client is expected to send a schema-`0x8080879F` (target `6685`)
incident, which `mission_script_player_trigger.cpp` already resolves into the type-31 → type-60
authored mapping and turns into `on_event_player_trigger`. In practice the retail client never
sends this incident for the local player — confirmed by logging every incident's target as it
arrives (only ever target `1121`, never `6685`) — so that native path is permanently silent for
the one thing missions need it for most.

## The fix

Sunrise already has everything needed to detect the crossing itself, without touching the
retail client's own internal logic:

- the local player's live world position is already tracked (`client::player::position`, used by
  the teleport feature);
- the exact world-space extruded-prism geometry of every trigger volume is already extracted
  (`state::build_data::scriptables`, the same data the debug UI's trigger-volume wireframe
  overlay draws).

`client::activity::player_trigger_watch` combines the two: it runs a point-in-prism containment
test against the live player position once per frame, and on a containment transition it
synthesizes the *same* schema-`0x8080879F` payload the retail client would have sent, and submits
it through the *same* ingestion path a real one takes (`host::submit_incident`). Nothing
downstream — `mission_script_player_trigger.cpp`, `on_event_player_trigger`, the event's
`volume_registry_key`/`volume_slot_type`/`volume_slot_index` fields — has to know the crossing was
detected locally rather than reported by the game.

## Using it from a mission script

```lua
context:slot("pt_start"):watch_trigger()
```

Call it on any exact type-31 trigger slot. It returns `true` once the watch is armed, `false`
otherwise — including while the underlying data just is not ready yet (see below), so a script
should retry on a short timer rather than treating one `false` as final:

```lua
local WATCH_TIMER = "watch_retry"

local ok, armed = pcall(function() return context:slot("pt_start"):watch_trigger() end)
if not (ok and armed) then
    context:start_timer(WATCH_TIMER, 500)
end
```

Once armed, walking into the volume fires `on_event_player_trigger` exactly like a real incident
would:

```lua
on_event_player_trigger = function(context, state, event)
    -- event.volume_registry_key / volume_slot_type / volume_slot_index name the resolved
    -- type-60 volume, same as for a genuine client-reported incident.
end
```

`watch_trigger()` is a plain synchronous context call — no queued intent, no `on_event_effect_result`
round trip — so it can also be armed at runtime through the [rt_bridge](RT_BRIDGE.md) on an
already-running mission, without relaunching:

```lua
local context, state = ...
return context:slot("pt_start"):watch_trigger()
```

## Why the first call(s) can return `false`

The trigger volume's authored geometry lives in a separate catalog
(`state::build_data::scriptables`) that is normally only ever built on request — the debug
"Scriptable Browser" panel is the only thing that used to ask for it. `watch_trigger()` requests
a build for the current scenario itself (idempotent, so subsequent calls are cheap), but the
build runs on a background worker thread, so the first call or two will return `false` while it
catches up. Retrying every few hundred milliseconds until `true` is the expected pattern.

## Scope and cleanup

A watch is scoped to the mission's own session binding and is dropped automatically when that
instance really closes (not on a mere ActivityClient generation rebind) — see
`clear_watches` in `mission_script_runtime.cpp`. There is currently no `unwatch_trigger()`; add
one the same way if a script needs to stop watching before its instance closes.
