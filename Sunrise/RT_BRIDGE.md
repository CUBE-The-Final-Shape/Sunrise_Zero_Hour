# The rt_bridge: live mission-script iteration without relaunching

A mission script normally only reloads when you relaunch the activity or use the debug UI's
"Reload script" action. The rt_bridge is a small, entirely script-side convention that lets you
push arbitrary Lua into an **already-attached** mission instance and see the result in the log,
so you can iterate on native calls (firing a trigger, setting a device channel, arming a
`watch_trigger`, reading a catalog row, ...) without recompiling or relaunching anything.

It is not a native feature — there is no dedicated "rt_bridge" API. It is just two existing
context calls used together in a small poll loop:

- `context:poll_command()` — reads `Sunrise/rt_cmd.txt` (next to `settings.json`), returns its
  contents as a string, and deletes the file so the same command is not replayed. Returns `nil`
  when the file is absent.
- `context:probe(text)` — writes an arbitrary line of text to `sunrise.log` (channel `server`,
  level `warn`). This is the only way a script can report something back to you; there is no
  return channel other than the log.

## Wiring it into a script

```lua
local POLL_TIMER = "rt_poll"
local POLL_INTERVAL_MS = 300

local function poll(context, state)
    local command = context:poll_command()
    if command then
        local fn, loadErr = load(command)
        if not fn then
            context:probe("cmd load error: " .. tostring(loadErr))
        else
            local ok, result = pcall(fn, context, state)
            context:probe("cmd result ok=" .. tostring(ok) .. " value=" .. tostring(result))
        end
    end
    context:start_timer(POLL_TIMER, POLL_INTERVAL_MS)
end

return {
    on_start = function(context, state)
        poll(context, state)
    end,
    on_event_timer_elapsed = function(context, state, event)
        if event.timer_name == POLL_TIMER then
            poll(context, state)
        end
    end,
}
```

`load()` and `pairs`/`next` need to be reachable from the sandbox for this to work (see the
dev-mode sandbox relaxations in `mission_script_lua_sandbox.cpp` — they are off by default and
meant to be reverted before treating a script as finished/shippable).

## Using it

Drop a snippet into `Sunrise/rt_cmd.txt` (same folder as `settings.json` and `logs/`). It runs
with `context, state` available through `...`, exactly like every other mission callback:

```lua
local context, state = ...
context:slot("pt_start"):watch_trigger()
return "armed"
```

Within one poll interval the file is consumed and a line like this appears in `sunrise.log`:

```
ev=script_probe activity=... text=cmd result ok=true value=true
```

Any `context`/`state` call is fair game: reading catalog rows, firing a device, checking
`region_arrival_pending()`, arming a `watch_trigger()`, etc. This is how effects like device
channels, dialogue cues, and trigger watches were tested interactively during Homecoming's
reconstruction, instead of relaunching the whole activity for every small change.

**Caveat:** a mission usually attaches two script VM instances per session (public and private
binding), so a dropped command runs — and its `probe` output appears — twice.
