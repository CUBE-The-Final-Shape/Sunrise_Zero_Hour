-- Homecoming (mission_towerfall, activity 0x62D85FB3). Root controller: loads the shared helpers
-- and one module per bubble, merges their trigger watches and named timers, and routes the
-- runtime callbacks. Beat logic lives in scripts/mission_towerfall/*.lua.
-- Where the player spawns: 72 = bubble 9 (Underwatch, the mission start), 32 = bubble 4
-- (Military/hangar), 48 = bubble 6 (plaza), 0 = bubble 0 (boulevard/bazaar). Slice-set/region indices from the generated SDK's state table. Starting in a
-- later bubble installs only that bubble's beats and the following ones.
local START_REGION = 48

local M = require("mission_towerfall.common")
local SEQ = require("mission_towerfall.sequencer")
-- Bubbles in mission order (each module declares its `region`). Only the start bubble and the
-- ones after it are installed, so a later START_REGION arms no watches for beats it skips.
local MISSION_ORDER = {
    require("mission_towerfall.underwatch")(M), -- 72, bubble 9
    require("mission_towerfall.military")(M),   -- 32, bubble 4
    require("mission_towerfall.plaza")(M),      -- the Tower plaza, after the hangar
    require("mission_towerfall.boulevard")(M),  -- 0, bubble 0: boulevard + bazaar
    require("mission_towerfall.skybattle")(M),  -- 64, bubble 8: the Cabal command ship
}
local beats = {}
local started = false
for _, beat in ipairs(MISSION_ORDER) do
    if beat.region == START_REGION then started = true end
    if started then beats[#beats + 1] = beat end
end
for _, beat in ipairs(beats) do
    if beat.watches then M.add_watches(beat.watches) end
    if beat.timers then M.add_timers(beat.timers) end
end

local POLL_TIMER = "rt_poll"
local POLL_INTERVAL_MS = 300
local WATCH_TIMER = "watch_retry"
-- The scriptables catalog (real trigger-volume geometry) only builds on request, on a background
-- thread: the first arming attempt kicks it off and this retries until every watch is armed.
local WATCH_RETRY_MS = 500
-- bootflow_step 38 = activity:in_world, the client's own named engine state; used as the spawn
-- signal (measured ~2.8s before player_position_present).
local IN_WORLD_STEP = 38
local last_bootflow_step = nil
local spawn_fired = false
-- Scene keys live in regions that stream as the player advances: after the spawn pass, the
-- scenes still without keys are retried from the poll every KEY_RETRY_MS until none is left.
local KEY_RETRY_MS = 3000
local keys_missing = true
local next_key_retry = 0

local function poll(context, state)
    local stepOk, step = pcall(function() return context:bootflow_step() end)
    if stepOk and step ~= last_bootflow_step then
        context:probe("bootflow_step transitioned to " .. tostring(step))
        if step == IN_WORLD_STEP and not spawn_fired then
            spawn_fired = true
            keys_missing = M.discover_scene_event_keys(context) > 0
            M.discover_verbose = false
            next_key_retry = (M.clock_ms(context) or 0) + KEY_RETRY_MS
            SEQ.on_spawn(context)
            -- Only the bubble the player spawns in runs its spawn beat; the others are entered
            -- on foot (or skipped entirely when starting later in the mission).
            for _, beat in ipairs(beats) do
                if beat.on_spawn and beat.region == START_REGION then beat.on_spawn(context, state) end
            end
        end
        last_bootflow_step = step
    end
    if spawn_fired and keys_missing then
        local now = M.clock_ms(context) or 0
        if now >= next_key_retry then
            next_key_retry = now + KEY_RETRY_MS
            keys_missing = M.discover_scene_event_keys(context) > 0
            if not keys_missing then context:probe("discover_scene_event_keys: every scene resolved") end
        end
    end
    M.tick_clears(context)
    -- Operator commands: a Lua chunk dropped at Sunrise/rt_cmd.txt runs once with (context, state).
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
    initial_state = { region_index = START_REGION },

    on_start = function(context, state)
        context:probe("rt_bridge online")
        -- Drop the watches an earlier run in this process left armed (the 64-entry client table
        -- survives mission restarts; beats skipped by START_REGION never release theirs).
        local okc, errc = pcall(function() context:clear_trigger_watches() end)
        context:probe("clear_trigger_watches ok=" .. tostring(okc) .. " err=" .. tostring(errc))
        -- Data-driven sequences (Sunrise/sequences/*.json, written by the in-game Sequencer).
        -- A reload while already in the world is a hot reload: sequences replay from clean.
        local okb, step = pcall(function() return context:bootflow_step() end)
        local hot = okb and step == IN_WORLD_STEP
        if SEQ.load(context, state) then SEQ.install(context, hot) end
        context:start_timer(WATCH_TIMER, 0)
        poll(context, state)
    end,

    on_event_timer_elapsed = function(context, state, event)
        if event.timer_name == POLL_TIMER then
            poll(context, state)
        elseif event.timer_name == WATCH_TIMER then
            if not M.try_arm_watches(context) then
                context:start_timer(WATCH_TIMER, WATCH_RETRY_MS)
            end
        else
            M.dispatch_timer(context, state, event)
        end
    end,

    on_event_player_trigger = function(context, state, event)
        M.dispatch_player_trigger(context, state, event)
    end,

    -- Native use receipts (hold-to-open doors, pickups): the first beat that claims it wins.
    on_event_object_interacted = function(context, state, event)
        context:probe(string.format("object_interacted registry_key=%s slot_type=%s slot_index=%s generation=%s",
            tostring(event.registry_key), tostring(event.slot_type), tostring(event.slot_index), tostring(event.generation)))
        for _, beat in ipairs(beats) do
            if beat.on_object_interacted and beat.on_object_interacted(context, state, event) then return end
        end
    end,

    -- Real completion/refusal outcome for effects we fired (ok=true on a call only means the call
    -- shape was valid).
    on_event_effect_result = function(context, state, event)
        context:probe(string.format("effect_result request_key=%s effect=%s outcome=%s outcome_code=%s",
            tostring(event.request_key), tostring(event.effect), tostring(event.outcome), tostring(event.outcome_code)))
    end,

    on_event_squad_state = function(context, state, event)
        M.dispatch_squad_state(context, state, event)
    end,

    -- Diagnostic while we work out where the shipped "x of 3" counter comes from: the client
    -- publishes objective/task progress on its own, and nothing in the script drives it yet.
    on_event_objective_progress = function(context, state, event)
        -- `objective` is the block ordinal inside the reporting type-3 slot, not a slot identity:
        -- the slot comes from the event's registry/type/index.
        context:probe(string.format(
            "objective_progress slot=%s/%s/%s block=%s task=%s count=%s previous=%s",
            tostring(event.registry_key), tostring(event.slot_type), tostring(event.slot_index),
            tostring(event.objective), tostring(event.task),
            tostring(event.task_count), tostring(event.previous_task_count)))
        for _, beat in ipairs(beats) do
            if beat.on_objective_progress then beat.on_objective_progress(context, state, event) end
        end
        SEQ.on_objective_progress(context, event)
    end,

    on_event_scene_finished = function(context, state, event)
        context:probe("scene_finished activation_token=" .. tostring(event.activation_token)
            .. " last_activated=" .. tostring(M.last_scene_activated))
    end,

    -- A cinematic may only be activated once the client reports it HOLDS the region its state
    -- owns; a merely requested destination is not enough (the Nyxara fork's Ember opening).
    on_event_client_state_changed = function(context, state, event)
        local held = event.held_region_index
        local current = event.current_region_index
        M.note_client_region(context, held, current)
        if held ~= M.last_held_region or current ~= M.last_current_region then
            M.last_held_region, M.last_current_region = held, current
            context:probe(string.format("client_state held=%s current=%s requested=%s spawn=%s teleport=%s",
                tostring(held), tostring(current), tostring(event.region_index),
                tostring(event.spawn_state), tostring(event.teleport_state)))
        end
        for _, beat in ipairs(beats) do
            if beat.on_client_state then beat.on_client_state(context, state, event) end
        end
    end,

    -- A Ghost link reports its own level (generation, progress 0..1, active); interacting with
    -- one raises this, never an object-interaction receipt.
    -- Two channels nothing routed until now. An authored object's destruction has to reach the
    -- server somehow; if it is neither an object level nor a damage level, it is one of these.
    on_event_entity_died = function(context, state, event)
        context:probe(string.format("entity_died registry=%s type=%s index=%s alive=%s prev=%s",
            tostring(event.registry_key), tostring(event.slot_type), tostring(event.slot_index),
            tostring(event.alive_count), tostring(event.previous_alive_count)))
        for _, beat in ipairs(beats) do
            if beat.on_entity_died then beat.on_entity_died(context, state, event) end
        end
    end,

    on_event_incident_received = function(context, state, event)
        M.incident_reports = (M.incident_reports or 0) + 1
        if M.incident_reports <= 120 then
            context:probe(string.format("incident registry=%s type=%s index=%s target=%s",
                tostring(event.registry_key), tostring(event.slot_type), tostring(event.slot_index),
                tostring(event.incident_target)))
        end
        for _, beat in ipairs(beats) do
            if beat.on_incident then beat.on_incident(context, state, event) end
        end
    end,

    -- Health/shield levels the client publishes for anything damageable. Raised straight from
    -- the Sense channel, so it needs no authored damage monitor -- the mission declares none.
    on_event_damage_state = function(context, state, event)
        for _, beat in ipairs(beats) do
            if beat.on_damage_state then beat.on_damage_state(context, state, event) end
        end
    end,

    -- Raw decoded Sense packets. Nothing routes these normally; a beat may ask for them while
    -- hunting for a signal the typed events do not carry.
    on_event_sensor_sense_updated = function(context, state, event)
        for _, beat in ipairs(beats) do
            if beat.on_sense_update then beat.on_sense_update(context, state, event) end
        end
    end,

    -- The client publishes an object's own level (present, alive, generation). It is how the
    -- destruction of an authored object is observed, there being no damage monitor for one.
    on_event_object_state = function(context, state, event)
        for _, beat in ipairs(beats) do
            if beat.on_object_state then beat.on_object_state(context, state, event) end
        end
    end,

    on_event_ghost_link_state = function(context, state, event)
        -- The client republishes the level on every tick while the player holds the link, so only
        -- the edges are logged: the first report, a change of `active`, and completion.
        local key = tostring(event.registry_key) .. "/" .. tostring(event.slot_index)
        local progress = tonumber(event.progress) or 0
        local previous = M.ghost_seen and M.ghost_seen[key]
        if previous == nil or previous.active ~= event.active
            or (progress >= 1 and not previous.done) then
            M.ghost_seen = M.ghost_seen or {}
            M.ghost_seen[key] = { active = event.active, done = progress >= 1 }
            context:probe(string.format("ghost_link registry=%s index=%s gen=%s progress=%.2f active=%s",
                tostring(event.registry_key), tostring(event.slot_index),
                tostring(event.generation), progress, tostring(event.active)))
        end
        for _, beat in ipairs(beats) do
            if beat.on_ghost_link_state then beat.on_ghost_link_state(context, state, event) end
        end
    end,

    on_event_cinematic_started = function(context, state, event)
        context:probe("cinematic started registry=" .. tostring(event.registry_key)
            .. " slot=" .. tostring(event.slot_index) .. "/" .. tostring(event.slot_type))
    end,

    on_event_cinematic_terminated = function(context, state, event)
        context:probe("cinematic terminated registry=" .. tostring(event.registry_key)
            .. " slot=" .. tostring(event.slot_index) .. "/" .. tostring(event.slot_type))
        for _, beat in ipairs(beats) do
            if beat.on_cinematic_terminated then beat.on_cinematic_terminated(context, state, event) end
        end
    end,

    on_event_cinematic_skip_requested = function(context, state, event)
        context:probe("cinematic skip_requested registry=" .. tostring(event.registry_key))
        for _, beat in ipairs(beats) do
            if beat.on_cinematic_terminated then beat.on_cinematic_terminated(context, state, event) end
        end
    end,
}
