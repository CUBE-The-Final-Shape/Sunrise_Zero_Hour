-- Shared helpers for mission_towerfall. Everything a beat module needs to place squads, drive
-- doors/dialogue/directives, run authored scenes and declare trigger watches. Beat modules
-- return { watches = {...}, timers = { name = fn }, on_spawn = fn, ... } and never touch the
-- runtime callbacks directly; the root script wires them.
local M = {}

-- Log line on the server channel at warn level (visible at the default settings).
function M.probe(context, text) context:probe(text) end

--------------------------------------------------------------------------------------------
-- Squads, doors, dialogue, directives
--------------------------------------------------------------------------------------------

-- Native combat objective (type-3 objective_sensor slot, same registry as the squad). Assigning
-- one is what makes the Cabal AI move at all: a squad placed without it stands where it spawned
-- (no pathing, no cover, no charge). The objective owns up to 24 task groups (combat areas with
-- their tactics; on obj_hangar: 12/13/16 hold the explosion-A ground with jetpack hops, 14/15
-- explosion B, 0/1/2/7/8/9/11 the overlook via the stairs, 3/4/5/17-20 leave the streamed area
-- and despawn). Rules established live on sq_hangar_fodder_a:
--   * the first assignment must precede the placement (sent to a living squad it is ignored);
--   * a later assignment is honoured on a living squad only when its revision is the SAME as the
--     first one (a higher revision is ignored) -- hence one constant revision per run;
--   * the client publishes squad_state.task_costs = {group+1 -> cost} for the groups the squad can
--     reach, so the cheapest reachable group is the one the shipped AI would pick.
-- M.spawn_squad{objective=} assigns group -1 (or opts.task_group) before placing and registers
-- the squad for M.dispatch_squad_state, which relinks it to the cheapest group on every cost
-- report.
-- One revision per mission run: constant within the run (a live group change is honoured only
-- at the same revision) but new on every restart (the client keeps the last revision it saw per
-- squad across mission restarts in one process and ignores a repeat). Fixed at first use.
M.OBJECTIVE_REVISION = nil
M.AI_SETTLE_MS = 1000
M.ai = {}   -- "registry/type/index" -> { name=, objective=, group= }

local function squad_state_key(registry_key, slot_type, slot_index)
    return string.format("%08x/%d/%d", registry_key or 0, slot_type or 0, slot_index or 0)
end

function M.assign_objective(context, name, objective, task_group)
    task_group = task_group or -1
    M.OBJECTIVE_REVISION = M.OBJECTIVE_REVISION or (math.floor((M.clock_ms(context) or 0) / 1000) + 1)
    local ok, err = pcall(function()
        context:slot(name):assign_combat_objective{
            objective = context:slot(objective), revision = M.OBJECTIVE_REVISION,
            task_group = task_group, reserved = false }
    end)
    context:probe(string.format("assign_objective(%s -> %s, group %d) ok=%s err=%s",
        name, objective, task_group, tostring(ok), tostring(err)))
    if ok then
        local squad = context:squad(name)
        local key = squad_state_key(squad.registry_key, squad.slot_type, squad.slot_index)
        -- Update in place: a relink must not wipe the per-squad diagnostic flags.
        local ai = M.ai[key] or {}
        ai.name, ai.objective, ai.group = name, objective, task_group
        M.ai[key] = ai
    end
    return ok
end

-- opts.objective: objective slot id assigned before the placement (see M.assign_objective);
-- opts.task_group: initial task group (default -1, the cost loop picks one);
-- opts.hold: assign the objective but keep the squad out of the cost loop, so it stays at group
-- -1 (immobile) until M.release_hold hands it over. That is how a squad waits in its authored
-- pose and only reacts when the player reaches it -- the shipped behaviour of the hangar
-- Centurion, which holds its ledge and then jet-hops to face the player.
-- Placement times, for the clear tracker: a squad placed long enough ago that only ever
-- reports 0 (no alive report at all, seen on sq_plaza_reinforce_a_c) still counts as down.
M.placed_at = {}
local function note_placement(context, name)
    local ok, squad = pcall(function() return context:squad(name) end)
    if ok and squad then
        M.placed_at[squad_state_key(squad.registry_key, squad.slot_type, squad.slot_index)] = M.clock_ms(context) or 0
    end
end

function M.spawn_squad(context, name, opts)
    note_placement(context, name)
    if opts and opts.objective then M.assign_objective(context, name, opts.objective, opts.task_group) end
    if opts and opts.hold then M.hold_objective(context, name) end
    local ok, err = pcall(function() context:squad(name):place{ retire_on_return = true } end)
    context:probe("spawn_squad(" .. name .. ") ok=" .. tostring(ok) .. " err=" .. tostring(err))
end

-- Takes a squad out of the cost loop: it keeps the objective it was given but nothing relinks
-- it, so it stays where it was placed.
function M.hold_objective(context, name)
    local squad = context:squad(name)
    local key = squad_state_key(squad.registry_key, squad.slot_type, squad.slot_index)
    local ai = M.ai[key]
    if ai then ai.held = true end
    context:probe("hold_objective(" .. name .. ")")
end

-- Hands a held squad back to the cost loop, which relinks it on the next cost report.
function M.release_hold(context, name)
    local squad = context:squad(name)
    local key = squad_state_key(squad.registry_key, squad.slot_type, squad.slot_index)
    local ai = M.ai[key]
    if ai then ai.held = nil end
    context:probe("release_hold(" .. name .. ") tracked=" .. tostring(ai ~= nil))
end

-- squad_state events: relink every objective-driven squad to its cheapest reachable task group.
-- 2040 is the native saturated "unreachable" cost.
-- Policy (ported from the reference implementation after the locked-group version left every
-- Cabal parked): re-pick on every cost report and re-assign whenever the choice changes, but
-- keep the current group when its cost ties with the best, which is what stops the ping-pong
-- between zones that the quantised costs otherwise cause.
-- Clear tracking: a squad counts as cleared once it has been seen alive (alive_count > 0)
-- and then reports 0. M.track_clear(context, name, list, fn) calls fn(context) once, when
-- every squad of the list is cleared. Squads are keyed like M.ai.
M.clears = {}
-- opts (optional): confirm_ms (zero must persist, default 1500), unseen_placed_ms (default
-- 20000), unseen_zero_ms (default 5000).
function M.track_clear(context, name, list, fn, opts)
    local squads = {}
    for _, squad_name in ipairs(list) do
        local squad = context:squad(squad_name)
        local key = squad_state_key(squad.registry_key, squad.slot_type, squad.slot_index)
        squads[key] = { name = squad_name, key = key, seen = false, cleared = false }
    end
    M.clears[name] = { squads = squads, fn = fn, done = false, opts = opts or {} }
    context:probe("track_clear(" .. name .. ") squads=" .. #list)
end

-- A zero must persist CLEAR_CONFIRM_MS before it counts: alive_count dips to 0 transiently
-- right after placement (pods in flight), which wiped a whole wave 6 s after its spawn. A squad
-- that reports alive again is un-cleared, and pending zeros are also confirmed from the poll
-- tick (M.tick_clears): after the last death no squad_state event may ever come.
local CLEAR_CONFIRM_MS = 1500
-- A squad never reported alive still counts once it has been placed this long and sits at 0.
local CLEAR_UNSEEN_PLACED_MS = 20000
-- ...and its zero must have lasted this long (pods report 0 while in flight, for many seconds).
local CLEAR_UNSEEN_ZERO_MS = 5000
local function evaluate_clears(context, now)
    for clear_name, clear in pairs(M.clears) do
        if not clear.done then
            local all = true
            for _, e in pairs(clear.squads) do
                local placed = M.placed_at[e.key]
                local unseen_placed = clear.opts.unseen_placed_ms or CLEAR_UNSEEN_PLACED_MS
                local eligible = e.seen or (placed and now - placed >= unseen_placed)
                -- No report at all since placement: treat as sitting at 0 since then.
                if eligible and not e.seen and e.last_alive == nil and not e.zero_since then e.zero_since = placed end
                local confirm = e.seen and (clear.opts.confirm_ms or CLEAR_CONFIRM_MS)
                                or (clear.opts.unseen_zero_ms or CLEAR_UNSEEN_ZERO_MS)
                if not e.cleared and eligible and e.zero_since and now - e.zero_since >= confirm then
                    e.cleared = true
                    context:probe(string.format("clear %s: %s down", clear_name, e.name))
                end
                if not e.cleared then all = false end
            end
            if all then
                clear.done = true
                context:probe("clear " .. clear_name .. ": all squads down")
                clear.fn(context)
            end
        end
    end
end

local function update_clears(context, key, alive)
    local now = M.clock_ms(context)
    if not now then return end
    for clear_name, clear in pairs(M.clears) do
        if not clear.done then
            local entry = clear.squads[key]
            if entry then
                if entry.last_alive ~= alive then
                    entry.last_alive = alive
                    context:probe(string.format("clear %s: %s alive=%d t=%d", clear_name, entry.name, alive, now))
                end
                if alive > 0 then
                    entry.seen = true
                    entry.zero_since = nil
                    if entry.cleared then
                        entry.cleared = false
                        context:probe(string.format("clear %s: %s back alive", clear_name, entry.name))
                    end
                elseif not entry.cleared and not entry.zero_since then
                    entry.zero_since = now
                end
            end
        end
    end
    evaluate_clears(context, now)
end

function M.tick_clears(context)
    local now = M.clock_ms(context)
    if now then evaluate_clears(context, now) end
end

function M.dispatch_squad_state(context, state, event)
    local key = squad_state_key(event.registry_key, event.slot_type, event.slot_index)
    if event.alive_count ~= nil then update_clears(context, key, event.alive_count) end
    local ai = M.ai[key]
    if not ai or ai.held or not event.task_costs then return end
    -- Diagnostic, not a filter: blocking on a mismatch would silently stop every relink if the
    -- client reports something other than the revision we sent. Reported once per squad.
    if event.objective_revision and M.OBJECTIVE_REVISION
        and event.objective_revision ~= M.OBJECTIVE_REVISION and not ai.revision_warned then
        ai.revision_warned = true
        context:probe(string.format("ai %s revision mismatch: event=%s ours=%s",
            ai.name, tostring(event.objective_revision), tostring(M.OBJECTIVE_REVISION)))
    end
    local best, cost, listed = -1, 2040, {}
    for group = 0, 23 do
        local candidate = event.task_costs[group + 1]
        if candidate then
            listed[#listed + 1] = group .. ":" .. string.format("%.1f", candidate)
            if candidate >= 0 and candidate < cost then best, cost = group, candidate end
        end
    end
    if best < 0 then return end
    -- Equal quantised costs: stay where we are rather than relink for nothing.
    if ai.group >= 0 and event.task_costs[ai.group + 1] == cost then best = ai.group end
    if best == ai.group then return end
    context:probe(string.format("ai %s group %d -> %d costs={%s}", ai.name, ai.group, best, table.concat(listed, " ")))
    M.assign_objective(context, ai.name, ai.objective, best)
end

-- Places a squad with every member count set to `per_member` (default 1) instead of the
-- package defaults, which can leave members at zero.
function M.spawn_squad_full(context, name, per_member, opts)
    note_placement(context, name)
    if opts and opts.objective then M.assign_objective(context, name, opts.objective, opts.task_group) end
    if opts and opts.hold then M.hold_objective(context, name) end
    local ok, err = pcall(function()
        local squad = context:squad(name)
        local counts = squad:counts()
        for i = 1, counts.count do counts:set(i, per_member or 1) end
        squad:place{ counts = counts, retire_on_return = true }
    end)
    context:probe("spawn_squad_full(" .. name .. ") ok=" .. tostring(ok) .. " err=" .. tostring(err))
end

-- Sets every member count to zero and replaces the squad's placement, i.e. despawns it now.
function M.retire_squad(context, name)
    local ok, err = pcall(function()
        local squad = context:squad(name)
        local counts = squad:counts()
        for i = 1, counts.count do counts:set(i, 0) end
        squad:place{ counts = counts, mode = context.sdk.squad_modes.replace }
    end)
    context:probe("retire_squad(" .. name .. ") ok=" .. tostring(ok) .. " err=" .. tostring(err))
end

-- Closed doors in this mission are type-4 objects that are absent by default: instantiating the
-- object shows the closed door, and the scene that owns it animates it open.
function M.set_door_object(context, door, closed, why)
    local ok, err = pcall(function() context:slot(door):set_object_active{ active = closed } end)
    context:probe(string.format("set_object_active(%s, %s) %s ok=%s err=%s",
        door, tostring(closed), why, tostring(ok), tostring(err)))
end

-- Drives a type-23 device channel (position 1.0 = open for the doors met so far).
function M.set_device_position(context, device, value, snap, why)
    local ok, err = pcall(function()
        context:slot(device):set_channel{
            channel = context.sdk.device_channels.position, value = context.sdk.unit(value), snap = snap }
    end)
    context:probe(string.format("set_channel(%s, position=%s) %s ok=%s err=%s",
        device, tostring(value), why, tostring(ok), tostring(err)))
end

-- Dialogue cues live on the authored dialogue list at slot row 3.
function M.play_cue(context, cue)
    local ok, keyOrErr = pcall(function() return context:slot(3):play_dialogue_cue{ cue = cue } end)
    context:probe(string.format("play_dialogue_cue(row3, cue=%d) ok=%s request_key=%s",
        cue, tostring(ok), tostring(keyOrErr)))
end

-- HUD directives live on slot row 1; name hashes come from the generated SDK's directive table.
function M.set_directive(context, name_hash, label)
    local ok, err = pcall(function()
        context:slot(1):set_directive{ directive = { slot_row = 17312, name_hash = name_hash, element = 0 } }
    end)
    context:probe("set_directive(" .. label .. ") ok=" .. tostring(ok) .. " err=" .. tostring(err))
end

-- Music: the mission's m_music_sensor is slot row 2 (directives are row 1, dialogue row 3).
-- Sections form a selection mask, so the previous one is switched off first. Section numbers
-- were found by ear (8 = the command-ship reveal).
local music_section = nil
-- Authoring switch: music off while iterating on long runs. Set back to true for a real pass.
M.MUSIC_ENABLED = false

-- Turns the active section off and forgets it, so a later set_music starts clean.
function M.mute_music(context)
    if not music_section then context:probe("mute_music: nothing playing") return end
    local ok, err = pcall(function()
        context:slot(2):set_music_section{ section = music_section, enabled = false }
    end)
    context:probe(string.format("mute_music(section %d) ok=%s err=%s",
        music_section, tostring(ok), tostring(err)))
    music_section = nil
end

function M.set_music(context, section)
    if not M.MUSIC_ENABLED then
        context:probe("set_music(" .. section .. ") skipped: M.MUSIC_ENABLED is false")
        return
    end
    if music_section == section then return end
    if music_section then
        pcall(function() context:slot(2):set_music_section{ section = music_section, enabled = false } end)
    end
    local ok, err = pcall(function() context:slot(2):set_music_section{ section = section, enabled = true } end)
    context:probe(string.format("set_music_section(%s -> %d) ok=%s err=%s",
        tostring(music_section), section, tostring(ok), tostring(err)))
    music_section = section
end

-- Milliseconds since boot, or nil on a DLL without context:clock_ms().
function M.clock_ms(context)
    local ok, clock = pcall(function() return context:clock_ms() end)
    if ok then return clock end
    return nil
end

--------------------------------------------------------------------------------------------
-- Type-2 actor programs
--------------------------------------------------------------------------------------------

-- play_actor_action on a squad's type-2 cell CREATES the actor (like Ember's Harvester) and
-- runs the named actor state. A scene claims a combatant participant only if the actor already
-- exists and is not running combat AI, so a static pose sent before the scene is the way to
-- hand a combatant to a scene.
-- A spatial target is optional and takes only a type-48 point set (marker = point index) or a
-- type-58 path (marker 0 its start, 1 its destination): the native resolver accepts no other
-- slot type and the client stalls on one, so the encoder refuses it outright now.
-- One revision counter per cell; generation 1 is the fresh-actor value.
local cell_revisions = {}

function M.cell_action(context, cell, group, action, target, marker)
    cell_revisions[cell] = (cell_revisions[cell] or 0) + 1
    local ok, err = pcall(function()
        local request = {
            generation = 1, revision = cell_revisions[cell], group = group, action = action }
        if target then
            request.target = context:slot(target)
            request.target_marker = marker or 0
        end
        context:slot(cell):play_actor_action(request)
    end)
    context:probe(string.format("cell_action(%s) rev=%d action=%08X target=%s/%s ok=%s err=%s",
        cell, cell_revisions[cell], action, tostring(target), tostring(marker),
        tostring(ok), tostring(err)))
end

--------------------------------------------------------------------------------------------
-- Authored scenes (type-43)
--------------------------------------------------------------------------------------------

-- Event-gate keys are FNV-1 hashes stored inline in the scene's gate graph (the tag at its
-- resource entity +0xC0), read natively by context:find_event_gate_keys{}. The wire body is an
-- empty deposit at a constant generation, then scene:activate{}, then the keys as one cumulative
-- set at the same generation (bumping the generation restarts the graph).
M.SCENE_GENERATION = 1
M.scene_graph_tags = {}   -- scene name -> graph tag, filled by beat modules
M.scene_event_keys = {}   -- scene name -> discovered keys
local scene_committed = {}

-- `graph` is the gate-graph tag, or { resource = tag }: the graph tag is then read live from the
-- scene's resource entity at +0xC0 (context:resolve_hash returns the first 256 bytes), so a new
-- scene needs no offline dump.
-- `ids` (optional) = { slot = <full slot id>, scene = <full symbol id> } for scene names shared
-- by several objects (sc_explosion_a exists on three), where the bare name is ambiguous.
M.scene_ids = {}
function M.register_scene(name, graph, ids)
    M.scene_graph_tags[name] = graph
    if ids then M.scene_ids[name] = ids end
end
local function scene_slot_id(name) return (M.scene_ids[name] and M.scene_ids[name].slot) or name end
local function scene_symbol_id(name) return (M.scene_ids[name] and M.scene_ids[name].scene) or name end

local function u32le(bytes, offset) -- 0-based offset into a Lua byte string
    local a, b, c, d = string.byte(bytes, offset + 1, offset + 4)
    if not d then return nil end
    return a + b * 0x100 + c * 0x10000 + d * 0x1000000
end

function M.discover_scene_event_keys(context)
    for name, graph in pairs(M.scene_graph_tags) do
        local graph_tag = graph
        if type(graph) == "table" then
            local ok, resolved, bytes = pcall(function() return context:resolve_hash(graph.resource) end)
            graph_tag = ok and resolved and bytes and u32le(bytes, 0xC0) or nil
            context:probe(string.format("resolve_scene_graph(%s) resource=0x%08X graph=%s",
                name, graph.resource, graph_tag and string.format("0x%08X", graph_tag) or "nil"))
            if graph_tag then M.scene_graph_tags[name] = graph_tag end
        end
        if not graph_tag then goto continue end
        local ok, keys = pcall(function() return context:find_event_gate_keys{ hash = graph_tag } end)
        if ok and keys then
            M.scene_event_keys[name] = keys
            local parts = {}
            for _, key in ipairs(keys) do parts[#parts + 1] = string.format("0x%08X", key) end
            context:probe("discover_scene_event_keys(" .. name .. ") found=" .. #keys
                .. " keys={" .. table.concat(parts, ",") .. "}")
        else
            context:probe("discover_scene_event_keys(" .. name .. ") ok=" .. tostring(ok)
                .. " err=" .. tostring(keys))
        end
        ::continue::
    end
end

function M.bind_scene(context, name)
    local ok, err = pcall(function()
        context:slot(scene_slot_id(name)):set_scene_events{ generation = M.SCENE_GENERATION, events = {} }
    end)
    context:probe("bind_scene(" .. name .. ", gen=" .. M.SCENE_GENERATION .. ") ok=" .. tostring(ok)
        .. " err=" .. tostring(err))
    return ok
end

function M.advance_scene(context, name)
    local ok, err = pcall(function() context:scene(scene_symbol_id(name)):activate{} end)
    context:probe("advance_scene(" .. name .. ") ok=" .. tostring(ok) .. " err=" .. tostring(err))
end

-- Publishes keys 1..upto (default: all) as one cumulative set.
function M.step_scene(context, name, upto)
    local keys = M.scene_event_keys[name] or {}
    local count = upto or #keys
    if count > #keys then count = #keys end
    if (scene_committed[name] or 0) >= count or count == 0 then
        context:probe("step_scene(" .. name .. ") nothing new to commit (" .. #keys .. " keys)")
        return
    end
    local events = {}
    for i = 1, count do events[i] = keys[i] end
    local ok, err = pcall(function()
        context:slot(scene_slot_id(name)):set_scene_events{ generation = M.SCENE_GENERATION, events = events }
    end)
    scene_committed[name] = count
    context:probe(string.format("step_scene(%s) committed=%d/%d ok=%s err=%s",
        name, count, #keys, tostring(ok), tostring(err)))
end

-- Publishes an explicit subset of keys (cumulative set), for scenes whose gates must not fire in
-- index order (scene_shaxx: the door-close key sits before the second dialogue line).
function M.publish_scene_keys(context, name, indices)
    local keys = M.scene_event_keys[name] or {}
    local events = {}
    for _, i in ipairs(indices) do if keys[i] then events[#events + 1] = keys[i] end end
    local ok, err = pcall(function()
        context:slot(scene_slot_id(name)):set_scene_events{ generation = M.SCENE_GENERATION, events = events }
    end)
    context:probe(string.format("publish_scene_keys(%s) {%s} ok=%s err=%s",
        name, table.concat(indices, ","), tostring(ok), tostring(err)))
end

-- bind -> activate, then keys 1..upto (default: all).
function M.start_scene(context, name, upto)
    if M.bind_scene(context, name) then
        M.advance_scene(context, name)
        M.last_scene_activated = name
        M.step_scene(context, name, upto)
        return true
    end
    return false
end

--------------------------------------------------------------------------------------------
-- Trigger watches
--------------------------------------------------------------------------------------------

-- A watch entry: { id =, on_enter = fn(context, state, w), on_exit = fn(...), once = true,
--   raw = { registry_key, slot_type, slot_index }   -- arm by type-60 identity
--   bubble = { bubble_index, row }                   -- resolve the debug panel row at arm time
--   (neither) -> context:slot(id):watch_trigger() by the type-31 slot name }
-- The client's watch table holds 64 entries and survives mission restarts, so every one-shot
-- watch (`once`, the default) is released right after it fires.
M.watches = {}

function M.add_watches(list)
    for _, w in ipairs(list) do
        w.armed = false
        w.volume = nil
        if w.once == nil then w.once = true end
        M.watches[#M.watches + 1] = w
    end
end

function M.try_arm_watches(context)
    local all_armed = true
    for _, w in ipairs(M.watches) do
        if not w.armed then
            local ok, armed, vreg, vtype, vidx
            if w.raw then
                ok, armed, vreg, vtype, vidx = pcall(function()
                    return context:watch_trigger_identity(w.raw.registry_key, w.raw.slot_type, w.raw.slot_index)
                end)
            elseif w.bubble then
                -- Before the scriptables catalog is built the row resolves to 0/0/0, so only
                -- remember the identity once a real one came back and the watch armed.
                ok, armed, vreg, vtype, vidx = pcall(function()
                    local fok, _, _, _, _, _, treg, ttype, tidx =
                        context:find_trigger_by_bubble(w.bubble[1], w.bubble[2])
                    if not treg or treg == 0 then return false end
                    local a, r, t, i = context:watch_trigger_identity(treg, ttype, tidx)
                    if a then w.raw = { registry_key = treg, slot_type = ttype, slot_index = tidx } end
                    return a, r, t, i
                end)
            else
                ok, armed, vreg, vtype, vidx = pcall(function() return context:slot(w.id):watch_trigger() end)
            end
            if ok and armed then
                w.armed = true
                w.volume = { registry_key = vreg, slot_type = vtype, slot_index = vidx }
                context:probe(string.format(
                    "watch_trigger(%s) armed volume_registry_key=%s volume_slot_type=%s volume_slot_index=%s",
                    w.id, tostring(vreg), tostring(vtype), tostring(vidx)))
            elseif w.optional then
                w.armed = true
                context:probe("watch_trigger(" .. w.id .. ") optional, not armed (ok=" .. tostring(ok) .. ")")
            else
                context:probe("watch_trigger(" .. w.id .. ") ok=" .. tostring(ok) .. " armed=" .. tostring(armed))
                all_armed = false
            end
        end
    end
    return all_armed
end

function M.release_watch(context, w)
    if not w.armed or w.released then return end
    w.released = true
    local ok, released = pcall(function()
        if w.raw then
            return context:unwatch_trigger_identity(w.raw.registry_key, w.raw.slot_type, w.raw.slot_index)
        end
        return context:slot(w.id):unwatch_trigger()
    end)
    context:probe("release_watch(" .. w.id .. ") ok=" .. tostring(ok) .. " released=" .. tostring(released))
end

-- Routes one crossing to the watch it belongs to. resolved_object_id doubles as the enter/exit
-- flag for our own synthesized crossings (0 = entered, 1 = exited).
function M.dispatch_player_trigger(context, state, event)
    local entering = event.resolved_object_id == 0
    local matched = false
    -- Every watch on that volume gets the crossing (several beats can share one volume); the
    -- client watch is only released once no armed watch needs it any more.
    for _, w in ipairs(M.watches) do
        if w.volume and w.volume.registry_key == event.volume_registry_key
            and w.volume.slot_type == event.volume_slot_type
            and w.volume.slot_index == event.volume_slot_index then
            matched = true
            context:probe("player_trigger matched " .. w.id .. " entering=" .. tostring(entering))
            local handler
            if entering then handler = w.on_enter else handler = w.on_exit end
            if handler and not w.fired then
                if w.once then w.fired = true end
                handler(context, state, w)
            end
        end
    end
    if matched then
        local still_needed = false
        for _, w in ipairs(M.watches) do
            if w.volume and w.volume.registry_key == event.volume_registry_key
                and w.volume.slot_type == event.volume_slot_type
                and w.volume.slot_index == event.volume_slot_index
                and not (w.once and w.fired) then still_needed = true end
        end
        if not still_needed then
            for _, w in ipairs(M.watches) do
                if w.volume and w.once and w.fired and w.volume.registry_key == event.volume_registry_key
                    and w.volume.slot_type == event.volume_slot_type
                    and w.volume.slot_index == event.volume_slot_index then
                    M.release_watch(context, w)   -- guards against a double release itself
                end
            end
        end
        return
    end
    context:probe(string.format(
        "player_trigger unmatched volume_registry_key=%s volume_slot_type=%s volume_slot_index=%s",
        tostring(event.volume_registry_key), tostring(event.volume_slot_type),
        tostring(event.volume_slot_index)))
end

--------------------------------------------------------------------------------------------
-- Named timers
--------------------------------------------------------------------------------------------

M.timers = {}

function M.add_timers(map)
    for name, fn in pairs(map) do M.timers[name] = fn end
end

function M.dispatch_timer(context, state, event)
    local fn = M.timers[event.timer_name]
    if fn then fn(context, state) end
end

return M
