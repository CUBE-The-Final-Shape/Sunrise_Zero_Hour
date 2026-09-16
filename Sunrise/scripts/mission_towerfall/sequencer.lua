-- Data-driven sequences. The in-game Sequencer page (Activity Host > World > Sequencer)
-- writes Sunrise/sequences/<mission>.json; this module reads it back at mission start, arms
-- the starts and plays each sequence as a chain of steps separated by delays.
--
-- File shape (version 1):
--   { "version": 1, "sequences": [ {
--       "name": "wave2", "enabled": true, "once": true,
--       "start": { "kind": "trigger_enter" | "trigger_exit" | "squads_clear" | "sequence_end"
--                          | "spawn" | "manual",
--                  "name": "pt_x" | "registry_key": n, "slot_type": 60, "slot_index": n,
--                  "region": n (arm the volume only while the client holds that region),
--                  "squads": [ "sq_a", ... ], "sequence": "other" },
--       "steps": [ { "delay_ms": 1000, "kind": "cue", "cue": 52 }, ... ] } ] }
-- Step kinds and their fields:
--   squad     squad, count (0 = package default), objective, hold, task_group
--   retire    squad
--   cue       cue
--   scene     scene (symbol id or name), slot (slot id), mode = "full" | "activate" | "keys" | "clear",
--             keys = [hash...] (explicit) or resource = tag (keys discovered at spawn),
--             new_generation (default true for "activate"/"full", false for "keys")
--   pose      cell, group, action, new_generation, sends (default 2)
--   despawn   cell (group, action optional): enabled=false on a fresh generation
--   effect    slot, filter, enabled, players (arm the filter on players first)
--   object    slot, active
--   device    slot, position, snap
--   directive hash, label
--   sequence  name (start another sequence)
--   probe     text
-- Delays are relative to the previous step; a step with delay 0 runs in the same tick.
local M = require("mission_towerfall.common")
local json = require("mission_towerfall.json")

local S = {}
S.FILE = "sequences/mission_towerfall.json"
S.sequences = {}      -- name -> definition
S.order = {}          -- names in file order
S.runs = {}           -- name -> { step = i, times = n }
S.scene_generations = {}
S.cell_generations = {}
S.cell_revisions = {}
S.effect_revisions = {}
S.loaded = false
-- Kill accounting per objective slot index (type 3): objective_progress reports one cumulative
-- kill count per objective for the members of squads assigned to it. A "squads_clear" start
-- fires once every squad of its list has been placed and kills >= members placed so far.
S.kills = {}            -- objective index -> kills reported by the client
S.kills_expected = {}   -- objective index -> members placed on it by squad steps
S.placed = {}           -- squad name -> true once a squad step placed it

local function probe(context, text) context:probe("seq " .. text) end

local function try(context, label, fn)
    local ok, err = pcall(fn)
    if not ok then probe(context, label .. " FAILED: " .. tostring(err)) end
    return ok
end

local run_despawn   -- defined with the steps; used by the hot-reload path of S.install

--------------------------------------------------------------------------------------------
-- Loading
--------------------------------------------------------------------------------------------

function S.load(context, state)
    S.sequences, S.order, S.loaded = {}, {}, false
    S.kills, S.kills_expected, S.placed, S.kills_offset = {}, {}, {}, {}
    local text, why = context:read_artifact_text{ path = S.FILE }
    if not text then
        probe(context, "no sequence file (" .. tostring(why) .. ")")
        return false
    end
    local ok, doc = pcall(json.decode, text)
    if not ok then
        probe(context, "sequence file rejected: " .. tostring(doc))
        return false
    end
    for _, def in ipairs(doc.sequences or {}) do
        if type(def.name) == "string" and def.name ~= "" then
            def.steps = def.steps or {}
            if def.enabled == nil then def.enabled = true end
            if def.once == nil then def.once = true end
            S.sequences[def.name] = def
            S.order[#S.order + 1] = def.name
        end
    end
    S.loaded = true
    -- The client's kill counters are cumulative for the mission: after a hot reload the count
    -- last seen (kept in a durable mission variable) becomes the offset, so the new run counts
    -- from zero again.
    for _, name in ipairs(S.order) do
        for _, step in ipairs(S.sequences[name].steps) do
            if step.kind == "squad" and step.objective and step.objective ~= json.null then
                local idx = S.objective_index(step.objective)
                if idx and state and not S.kills_offset[idx] then
                    local ok, last = pcall(function() return state:variable("seq.kills." .. idx) end)
                    S.kills_offset[idx] = (ok and tonumber(last)) or 0
                end
            end
        end
    end
    -- Generations must keep increasing across script reloads in one mission (the client ignores
    -- a generation it has already seen): base them on the clock, then count from there.
    -- 31-bit actor-program generations: seconds * 1000 leaves 24 days of process uptime.
    local clock = M.clock_ms(context) or 0
    S.base = (math.floor(clock / 1000) % 2000000) * 1000
    S.scene_generations, S.cell_generations, S.cell_revisions, S.effect_revisions = {}, {}, {}, {}
    probe(context, string.format("loaded %d sequence(s) from %s (generation base %d)", #S.order, S.FILE, S.base))
    -- Scenes with a resource tag get their gate keys discovered at spawn (common.lua).
    for _, name in ipairs(S.order) do
        for _, step in ipairs(S.sequences[name].steps) do
            if step.kind == "scene" and step.resource and step.resource ~= json.null then
                M.register_scene(step.scene, { resource = step.resource },
                    step.slot and { slot = step.slot, scene = step.scene } or nil)
            end
        end
    end
    return true
end

--------------------------------------------------------------------------------------------
-- Starts
--------------------------------------------------------------------------------------------

local function can_start(def)
    local run = S.runs[def.name]
    if not def.enabled then return false end
    if def.once and run and run.times > 0 then return false end
    return true
end

-- `hot` = the script was reloaded inside a running mission (bootflow already in world): every
-- squad any sequence places is retired so the sequences can be replayed from a clean plaza.
-- Program-created actors (poses) cannot be removed; the next pose replaces them.
function S.install(context, hot)
    if hot then
        local retired = {}
        for _, name in ipairs(S.order) do
            for _, step in ipairs(S.sequences[name].steps) do
                if step.kind == "squad" and step.squad and not retired[step.squad] then
                    retired[step.squad] = true
                    M.retire_squad(context, step.squad)
                end
            end
        end
        -- Program-created actors: clear the enabled bit on every posed cell (fresh generation).
        local cells = {}
        for _, name in ipairs(S.order) do
            for _, step in ipairs(S.sequences[name].steps) do
                if step.kind == "pose" and step.cell and not cells[step.cell] then
                    cells[step.cell] = true
                    run_despawn(context, step.cell, step.group, step.action)
                end
            end
        end
        probe(context, "hot reload: squads retired, cells disabled, generations rebased")
    end
    local watches = {}
    for _, name in ipairs(S.order) do
        local def = S.sequences[name]
        local start = def.start or { kind = "manual" }
        if not def.enabled then
            -- disabled: no watch, no clear tracker
        elseif start.kind == "trigger_enter" or start.kind == "trigger_exit" then
            local w = { id = "seq:" .. name, once = def.once }
            if start.region and start.region ~= json.null then w.region = start.region end
            if start.registry_key and start.registry_key ~= json.null then
                w.raw = { registry_key = start.registry_key, slot_type = start.slot_type or 60,
                          slot_index = start.slot_index }
            else
                w.id = start.name
            end
            local handler = function(ctx) S.start(ctx, name, start.kind) end
            if start.kind == "trigger_enter" then w.on_enter = handler else w.on_exit = handler end
            watches[#watches + 1] = w
        elseif start.kind == "squads_clear" then
            -- Primary: the objective's kill counter (S.check_clears). Fallback, for members the
            -- counter may not report (killed by Zavala?): every listed squad at 0 for 10 s.
            local list = {}
            for _, squad_name in ipairs(start.squads or {}) do
                if S.placeable(squad_name) then list[#list + 1] = squad_name
                elseif not S.unplaceable_warned[squad_name] then
                    S.unplaceable_warned[squad_name] = true
                    probe(context, string.format("%s waits on %s, which no enabled sequence places: ignored", name, squad_name))
                end
            end
            M.track_clear(context, "seq:" .. name, list,
                function(ctx) S.start(ctx, name, "squads_clear/alive") end,
                { confirm_ms = 10000, unseen_placed_ms = 20000, unseen_zero_ms = 10000 })
        end
    end
    if #watches > 0 then M.add_watches(watches) end
    probe(context, string.format("installed: %d trigger start(s)", #watches))
end

function S.on_spawn(context)
    for _, name in ipairs(S.order) do
        local def = S.sequences[name]
        if def.start and def.start.kind == "spawn" then S.start(context, name, "spawn") end
    end
end

-- "slot/<registry>/<index>/<index>/<type>" -> the slot index (the event carries the runtime
-- registry key, not the package one, so the objective is matched on type 3 + index).
function S.objective_index(slot_id)
    local hex = tostring(slot_id):match("^slot/%x+/(%x+)/")
    return hex and tonumber(hex, 16) or nil
end

-- A listed squad no enabled sequence ever places cannot be waited on (a stale wait list):
-- it is ignored, once, with a warning.
S.unplaceable_warned = {}
function S.placeable(squad_name)
    for _, seq_name in ipairs(S.order) do
        local def = S.sequences[seq_name]
        if def.enabled then
            for _, step in ipairs(def.steps) do
                if step.kind == "squad" and step.squad == squad_name then return true end
            end
        end
    end
    return false
end

function S.on_objective_progress(context, event)
    if event.slot_type ~= 3 then return end
    local idx, count = event.slot_index, event.task_count or 0
    pcall(function() context:set_variable("seq.kills." .. idx, count) end)
    S.kills[idx] = count - (S.kills_offset[idx] or 0)
    S.check_clears(context)
end

-- Every "squads_clear" sequence whose squads are all placed and whose objective has no
-- living member left (kills >= members placed) starts.
function S.check_clears(context)
    for _, name in ipairs(S.order) do
        local def = S.sequences[name]
        local start = def.start
        if def.enabled and start and start.kind == "squads_clear" and can_start(def) then
            local all_placed, idx = true, nil
            for _, squad_name in ipairs(start.squads or {}) do
                if not S.placed[squad_name] and S.placeable(squad_name) then all_placed = false end
            end
            -- the objective: from the first squad step placing one of the listed squads
            for _, seq_name in ipairs(S.order) do
                for _, step in ipairs(S.sequences[seq_name].steps) do
                    if idx == nil and step.kind == "squad" and step.objective and step.objective ~= json.null then
                        for _, squad_name in ipairs(start.squads or {}) do
                            if step.squad == squad_name then idx = S.objective_index(step.objective) end
                        end
                    end
                end
            end
            if all_placed and idx and (S.kills[idx] or 0) >= (S.kills_expected[idx] or 0)
                and (S.kills_expected[idx] or 0) > 0 then
                probe(context, string.format("%s: objective %d kills %d >= expected %d", name, idx,
                    S.kills[idx] or 0, S.kills_expected[idx] or 0))
                S.start(context, name, "squads_clear")
            end
        end
    end
end

local function on_sequence_end(context, ended)
    for _, name in ipairs(S.order) do
        local def = S.sequences[name]
        if def.start and def.start.kind == "sequence_end" and def.start.sequence == ended then
            S.start(context, name, "sequence_end:" .. ended)
        end
    end
end

--------------------------------------------------------------------------------------------
-- Steps
--------------------------------------------------------------------------------------------

local function scene_ids(step)
    local symbol = step.scene
    local slot = (step.slot and step.slot ~= json.null) and step.slot or symbol
    return symbol, slot
end

local function run_scene(context, step)
    local symbol, slot = scene_ids(step)
    local mode = step.mode or "full"
    local fresh = step.new_generation
    if fresh == nil then fresh = (mode ~= "keys" and mode ~= "clear") end
    if fresh or not S.scene_generations[symbol] then
        S.scene_generations[symbol] = (S.scene_generations[symbol] or S.base) + 1
    end
    local gen = S.scene_generations[symbol]
    if mode == "clear" then
        -- The header's clear bit on the scene's current generation (keys kept, so the body
        -- differs only by that bit): the release probe.
        local keys = step.keys
        if (not keys or keys == json.null or #keys == 0) then keys = {} end
        try(context, "scene clear " .. symbol, function()
            context:slot(slot):set_scene_events{ generation = gen, events = keys, clear = true } end)
        probe(context, string.format("scene %s CLEAR gen=%d", symbol, gen))
        return
    end
    if mode == "full" or mode == "activate" then
        try(context, "scene bind " .. symbol, function()
            context:slot(slot):set_scene_events{ generation = gen, events = {} } end)
        try(context, "scene activate " .. symbol, function() context:scene(symbol):activate{} end)
    end
    if mode == "full" or mode == "keys" then
        local keys = step.keys
        if (not keys or keys == json.null or #keys == 0) then keys = M.scene_keys(context, symbol) end
        if keys and #keys > 0 then
            try(context, "scene keys " .. symbol, function()
                context:slot(slot):set_scene_events{ generation = gen, events = keys } end)
        else
            probe(context, "scene " .. symbol .. ": no keys known (give keys or resource)")
        end
    end
    probe(context, string.format("scene %s mode=%s gen=%d", symbol, mode, gen))
end

local function run_pose(context, step)
    local cell = step.cell
    local fresh = step.new_generation
    if fresh or not S.cell_generations[cell] then
        S.cell_generations[cell] = (S.cell_generations[cell] or S.base) + 1
        S.cell_revisions[cell] = 0
    end
    local gen = S.cell_generations[cell]
    for _ = 1, (step.sends or 2) do
        S.cell_revisions[cell] = (S.cell_revisions[cell] or 0) + 1
        local rev = S.cell_revisions[cell]
        try(context, "pose " .. cell, function()
            context:slot(cell):play_actor_action{
                generation = gen, revision = rev, group = step.group, action = step.action } end)
    end
    probe(context, string.format("pose %s gen=%d action=%08X", cell, gen, step.action or 0))
end

-- enabled = false on a fresh generation of the cell: the actor-removal probe (the type-2 Auth
-- root carries an enabled bit the shipped encoders always set). group/action default to the
-- generic pose so the body stays the shipped shape.
run_despawn = function(context, cell, group, action)
    S.cell_generations[cell] = (S.cell_generations[cell] or S.base) + 1
    S.cell_revisions[cell] = 1
    local gen = S.cell_generations[cell]
    try(context, "despawn " .. cell, function()
        context:slot(cell):play_actor_action{
            generation = gen, revision = 1, group = group or 0xAFB11A12, action = action or 0x40FC40DA,
            enabled = false } end)
    probe(context, string.format("despawn %s gen=%d (enabled=false)", cell, gen))
end

local function run_effect(context, step)
    S.effect_revisions[step.slot] = (S.effect_revisions[step.slot] or S.base) + 1
    local rev = S.effect_revisions[step.slot]
    try(context, "effect " .. step.slot, function()
        if step.players then context:slot(step.filter):set_object_filter{ players = true } end
        context:slot(step.slot):set_mission_effect{
            filter = context:slot(step.filter), enabled = step.enabled ~= false, revision = rev }
    end)
    probe(context, string.format("effect %s enabled=%s rev=%d", step.slot, tostring(step.enabled ~= false), rev))
end

local function run_step(context, name, step)
    local kind = step.kind
    if kind == "squad" then
        local opts = {}
        if step.objective and step.objective ~= json.null and step.objective ~= "" then
            opts.objective = step.objective
            opts.hold = step.hold == true
            if step.task_group and step.task_group ~= json.null then opts.task_group = step.task_group end
        end
        local count = tonumber(step.count) or 1
        if count <= 0 then M.spawn_squad(context, step.squad, opts)
        else M.spawn_squad_full(context, step.squad, count, opts) end
        S.placed[step.squad] = true
        -- Held squads (task group -1: the scene's victims) are not counted by the objective.
        if opts.objective and not opts.hold then
            local idx = S.objective_index(opts.objective)
            if idx then
                S.kills_expected[idx] = (S.kills_expected[idx] or 0) + math.max(count, 1)
                probe(context, string.format("kills expected on objective %d: %d (reported %d)",
                    idx, S.kills_expected[idx], S.kills[idx] or 0))
            end
        end
        S.check_clears(context)
    elseif kind == "retire" then M.retire_squad(context, step.squad)
    elseif kind == "cue" then M.play_cue(context, tonumber(step.cue))
    elseif kind == "scene" then run_scene(context, step)
    elseif kind == "pose" then run_pose(context, step)
    elseif kind == "despawn" then run_despawn(context, step.cell, step.group, step.action)
    elseif kind == "effect" then run_effect(context, step)
    elseif kind == "object" then M.set_door_object(context, step.slot, step.active ~= false, "seq " .. name)
    elseif kind == "device" then
        M.set_device_position(context, step.slot, tonumber(step.position) or 1.0, step.snap == true, "seq " .. name)
    elseif kind == "directive" then
        local progress = step.progress
        if progress == json.null then progress = nil end
        M.set_directive(context, step.hash, step.label or "", progress, step.raw == true)
    elseif kind == "sequence" then S.start(context, step.name, "sequence:" .. name)
    elseif kind == "clear" then
        M.track_clear(context, step.name or ("seq:" .. name), step.squads or {},
            function(ctx) on_sequence_end(ctx, step.name or name) end)
    elseif kind == "probe" then context:probe(tostring(step.text))
    else probe(context, name .. ": unknown step kind " .. tostring(kind)) end
end

local function schedule(context, name, index)
    local def = S.sequences[name]
    local step = def.steps[index]
    if not step then
        probe(context, name .. " finished")
        on_sequence_end(context, name)
        return
    end
    -- Timer names accept only [A-Za-z0-9_.-/]; sequence names are sanitised the same way.
    local timer = string.format("seq.%s.%d", (name:gsub("[^%w_%-%./]", "_")), index)
    M.add_timers({ [timer] = function(ctx)
        local run = S.runs[name]
        if not run or run.step ~= index then return end
        run.step = index + 1
        probe(ctx, string.format("%s step %d/%d %s", name, index, #def.steps, tostring(step.kind)))
        run_step(ctx, name, step)
        schedule(ctx, name, index + 1)
    end })
    local delay = tonumber(step.delay_ms) or 0
    if delay <= 0 then
        M.timers[timer](context)
    else
        local ok, err = pcall(function() context:start_timer(timer, delay) end)
        if not ok then probe(context, string.format("%s step %d: timer refused (%s)", name, index, tostring(err))) end
    end
end

function S.start(context, name, why)
    local def = S.sequences[name]
    if not def then probe(context, "unknown sequence " .. tostring(name)); return end
    if not can_start(def) then probe(context, name .. " not started (" .. tostring(why) .. ")"); return end
    local run = S.runs[name] or { times = 0 }
    run.times = run.times + 1
    run.step = 1
    S.runs[name] = run
    probe(context, string.format("start %s (%s), %d step(s)", name, tostring(why), #def.steps))
    schedule(context, name, 1)
end

function S.stop(context, name)
    S.runs[name] = nil
    probe(context, "stopped " .. tostring(name))
end

return S
