-- Bubble 8 / region 64 (sky_battle): the Cabal command ship, entered from the bazaar through
-- mid_cinematic (see boulevard.lua). Runtime registry 0x2D322467, SDK registry 80b508f4.
-- First iteration of the shipped flow, to be corrected live:
--   spawn -> Holliday's Hawk leaves, the drop-pod launcher runs, "Disable the shields"
--   -> ship entry: two Legionaries then a lone one -> Ghost asks for the console
--   -> console interacted: the energy shield drops, "Reach the shield generator"
--   -> corridor: three lone Cabal, then a pair and Pashk
--   -> door: Zavala line, long climb to the ship's back
--   -> platform: psions, a dropship unloading, then Brann.
return function(M)
    local B = { region = 64 }

    local REG = 0x2D322467 -- runtime registry of this bubble's trigger volumes
    local function slot(index, slot_type)
        return string.format("slot/80b508f4/%06x/%04x/%04x", index, index, slot_type)
    end
    local function volume(index) return { registry_key = REG, slot_type = 60, slot_index = index } end
    -- The nav-mode and battleship volumes belong to a second registry of this bubble.
    local REG_NAV = 0x002D225E
    local function nav_volume(index)
        return { registry_key = REG_NAV, slot_type = 60, slot_index = index }
    end

    -- The ship's two combat objectives: obj_damaged covers the entry and the corridor,
    -- obj_deck the platform. Squads are assigned before placement so the Cabal can move.
    local AI_DAMAGED = { objective = slot(0, 3) }  -- obj_damaged
    local AI_DECK = { objective = slot(1, 3) }     -- obj_deck
    local AI_DECK_ULTRA = { objective = slot(2, 3) } -- obj_deck_ultra, the boss's own

    -- sc_explosion_a/b of this bubble. The bare names are shared with the hangar's copies, so
    -- both are addressed by their full ids; the graph is the one every explosion scene uses.
    M.register_scene("ship_explosion_a", { resource = 0x80B82715 }, {
        slot = slot(137, 43),
        scene = "symbol/80b500bc/0008/0000/80b508f8/28/000000/80b508f4/0089/002b" })
    M.register_scene("ship_explosion_b", { resource = 0x80B82715 }, {
        slot = slot(138, 43),
        scene = "symbol/80b500bc/0008/0000/80b508f8/28/000000/80b508f4/008a/002b" })

    local DIRECTIVE_DISABLE_SHIELDS = 0xDA1CA185
    local DIRECTIVE_REACH_GENERATOR = 0x57395492
    -- Element 1 of this hash is the counter variant: same title, but it carries the progress
    -- string ("Exhaust turbines destroyed x of 3"). Element 0 is the plain one.
    local DIRECTIVE_OVERLOAD_GENERATOR = 0xF0D48F30
    local DIRECTIVE_ESCAPE_SHIP = 0xD15B9A42
    local OVERLOAD_ELEMENT = 1
    local TURBINE_COUNT = 3

    -- A cue is fired only once its constant is set, so an unidentified one logs instead of
    -- playing something wrong (found by ear with cue_N.lua through rt_cmd).
    B.CUE_HAWK_LEAVES = 77      -- Holliday as her Hawk pulls away
    B.CUE_GHOST_CONSOLE = 78    -- Ghost: interact with the hologram
    B.CUE_DOORS_OPEN = 79       -- Ghost, as the energy doors open
    B.CUE_HALL_EXIT = 82        -- as the player leaves the damaged hall
    B.CUE_ZAVALA_DOOR = nil     -- Zavala, past the door before the climb
    B.CUE_SHIELD_ROOM = 85      -- entering the shield room
    B.CUE_BATTLESHIP = 86       -- at the battleship, with the Overload directive
    B.CUE_TURBINE = { 88, 90, 91 } -- one per turbine destroyed, in order
    B.CUE_ESCAPE = 92           -- shortly after the escape directive

    local function cue(context, index, label)
        if index == nil then
            context:probe("sky: cue for " .. label .. " not identified yet")
            return
        end
        M.play_cue(context, index)
    end

    ----------------------------------------------------------------------------------------
    -- Spawn: the Hawk leaves, the launcher runs, the first directive
    ----------------------------------------------------------------------------------------

    B.SPAWN_HAWK_MS = 1500       -- the Hawk pulls away shortly after the player lands
    B.SPAWN_DIRECTIVE_MS = 9000  -- the directive lands as the line ends
    -- Time from the interaction receipt to the doors giving way. We do not know yet whether
    -- the receipt marks the start or the end of the hold; 3.5 s covers the start case.
    B.CONSOLE_DOOR_MS = 3500
    B.CONSOLE_PROGRESS_DONE = 0.99  -- the link's own level, 0..1, as the hold completes
    B.HALL_MELEE_MS = 1500       -- the melee pair follows the rear anchor

    function B.on_spawn(context, state)
        -- The Hawk is an object plus its movement device, like the hangar carriers: instantiate
        -- it, then drive the device so it flies off.
        M.set_door_object(context, slot(165, 4), true, "hawk")
        context:start_timer("sky_hawk_leaves", B.SPAWN_HAWK_MS)
        -- The drop-pod launcher across the gap: object + device. It does nothing on its own, so
        -- the device is driven to run it.
        M.set_door_object(context, slot(150, 4), true, "drop_pod_launch")
        M.set_device_position(context, slot(151, 23), 1.0, false, "d_drop_pod_launch")
        context:start_timer("sky_first_directive", B.SPAWN_DIRECTIVE_MS)
    end

    ----------------------------------------------------------------------------------------
    -- Ship entry, the console, and the shield
    ----------------------------------------------------------------------------------------

    -- gl_cabal_console (type 65) is the Ghost link the player interacts with; d_cabal_console is
    -- its device. The link is armed only once the entry Cabal are dead, then the interaction
    -- drops the shield. Generations must keep rising across mission restarts in one process, so
    -- they are derived from the clock like the Underwatch gun door.
    local CONSOLE_LINK, CONSOLE_DEVICE = slot(166, 65), slot(167, 23)
    local console_generation = nil

    local function console_gen(context)
        if console_generation == nil then
            console_generation = math.floor((M.clock_ms(context) or 0) / 1000) % 2000000 + 1
        else
            console_generation = console_generation + 1
        end
        return console_generation
    end

    function B.arm_console(context)
        cue(context, B.CUE_GHOST_CONSOLE, "Ghost asks for the console")
        local ok, err = pcall(function()
            context:slot(CONSOLE_LINK):set_ghost_link{ generation = console_gen(context), enabled = true }
        end)
        context:probe("sky: arm console ok=" .. tostring(ok) .. " err=" .. tostring(err))
    end

    function B.on_console_used(context)
        if B.console_used then return end
        B.console_used = true
        local ok, err = pcall(function()
            context:slot(CONSOLE_LINK):set_ghost_link{ generation = console_gen(context), enabled = false }
        end)
        context:probe("sky: console used, link off ok=" .. tostring(ok) .. " err=" .. tostring(err))
        M.set_device_position(context, CONSOLE_DEVICE, 1.0, false, "d_cabal_console")
        -- The interaction plays out before the doors give way.
        context:start_timer("sky_pod_doors", B.CONSOLE_DOOR_MS)
    end

    -- The two energy doors past the hologram.
    function B.open_pod_doors(context)
        M.set_device_position(context, slot(56, 23), 1.0, false, "d_ship_pod_door_a")
        M.set_device_position(context, slot(57, 23), 1.0, false, "d_ship_pod_door_b")
        M.set_directive(context, DIRECTIVE_REACH_GENERATOR, "Reach the shield generator")
        cue(context, B.CUE_DOORS_OPEN, "Ghost as the interaction completes")
    end

    -- Waits for `wanted` further kills on obj_damaged, then runs `fn`. The counter is
    -- cumulative over the mission, so the first report after arming sets the base.
    function B.wait_kills(context, label, wanted, fn)
        B.kill_label, B.kill_base, B.kill_wanted, B.kill_fn = label, nil, wanted, fn
    end

    function B.on_objective_progress(context, state, event)
        -- Diagnostic: the ship may count the turbines on an objective of its own.
        if B.turbines_armed then
            B.progress_reports = (B.progress_reports or 0) + 1
            if B.progress_reports <= 40 then
                context:probe(string.format("sky: objective_progress slot=%s/%s block=%s task=%s count=%s prev=%s",
                    tostring(event.slot_type), tostring(event.slot_index), tostring(event.objective),
                    tostring(event.task), tostring(event.task_count), tostring(event.previous_task_count)))
            end
        end
        if event.slot_type ~= 3 or event.slot_index ~= 0 or not B.kill_wanted then return end
        local count = event.task_count or 0
        if B.kill_base == nil then B.kill_base = (event.previous_task_count or (count - 1)) end
        local kills = count - B.kill_base
        context:probe(string.format("sky: %s kills %d/%d", B.kill_label, kills, B.kill_wanted))
        if kills >= B.kill_wanted then
            local fn = B.kill_fn
            B.kill_wanted, B.kill_fn = nil, nil
            fn(context)
        end
    end

    -- Interacting with a Ghost link raises on_event_ghost_link_state, not an interaction receipt.
    -- The level rises to 1 as the hold completes; the doors follow that.
    function B.on_ghost_link_state(context, state, event)
        if B.console_used or not console_generation then return end
        local progress = tonumber(event.progress) or 0
        if progress < B.CONSOLE_PROGRESS_DONE then return end
        B.on_console_used(context)
    end

    -- Cue 82 needs both conditions, in either order: the player out of the hall and its three
    -- squads down. Releasing the watch is deferred until it has played, so re-entering the hall
    -- before the last kill still counts as being inside.
    function B.try_hall_cue(context)
        if B.hall_cue_done or B.in_hall or not B.hall_cleared then return end
        B.hall_cue_done = true
        cue(context, B.CUE_HALL_EXIT, "hall cleared and left")
        if B.hall_watch then M.release_watch(context, B.hall_watch) end
    end

    ----------------------------------------------------------------------------------------
    -- Watches
    ----------------------------------------------------------------------------------------

    -- pt_sky_battle has a type-31 sensor slot but no trigger volume of its own, so it can never
    -- be watched: the retry loop spins on it forever. Nothing here uses it.
    B.watches = {
        -- Leaving the spawn area (bubble 8 row 21 of the trigger catalog -- this registry holds
        -- no such slot index, so it is resolved by bubble row at arm time): the three Legionaries
        -- standing by the hologram. Ghost (Cue 78) speaks only once the whole squad is down.
        { id = "sky_spawn_area", bubble = { 8, 21 },
          on_exit = function(context)
              M.spawn_squad(context, slot(4, 1), AI_DAMAGED)  -- sq_pods (3)
              -- alive_count reads 0 between the placement and the squad's first report, which
              -- cleared it instantly: wait on the objective's kill counter instead.
              B.wait_kills(context, "sq_pods", 3, function(ctx) B.arm_console(ctx) end)
          end },
        -- pt_damaged (378): two Cabal and an Incendior, plus the pair behind them.
        { id = "pt_damaged", raw = volume(378),
          on_enter = function(context)
              M.spawn_squad(context, slot(6, 1), AI_DAMAGED)  -- sq_damaged_hall_front (3)
              M.spawn_squad(context, slot(5, 1), AI_DAMAGED)  -- sq_damaged (2)
          end },
        -- pt_damaged_hall (387): the rear pair, then the melee pair. Cue 82 waits on BOTH the
        -- player having left the hall and every squad of the hall being dead, whichever comes
        -- last -- so the line never starts over a fight or while the player is still inside.
        -- Two events on one volume, so the watch is released by hand after the exit.
        { id = "pt_damaged_hall", raw = volume(387), once = false,
          on_enter = function(context, state, w)
              -- The squads are placed once, but `in_hall` tracks every crossing: re-entering
              -- before the last kill must count as being inside again.
              B.in_hall = true
              if w.entered then return end
              w.entered = true
              M.spawn_squad(context, slot(8, 1), AI_DAMAGED)   -- sq_damaged_hall_rear_anchor
              M.spawn_squad(context, slot(7, 1), AI_DAMAGED)   -- sq_damaged_hall_rear
              context:start_timer("sky_hall_melee", B.HALL_MELEE_MS)
          end,
          on_exit = function(context, state, w)
              B.in_hall = false
              B.hall_watch = w
              B.try_hall_cue(context)
          end },
        -- pt_damaged_hall_stairs (388): the stair pair.
        { id = "pt_damaged_hall_stairs", raw = volume(388),
          on_enter = function(context)
              M.spawn_squad(context, slot(10, 1), AI_DAMAGED)  -- sq_damaged_hall_rear_stair
              M.spawn_squad(context, slot(11, 1), AI_DAMAGED)  -- sq_damaged_hall_rear_stair_anchor
          end },
        -- pt_damaged_hall_stairs_door (389): Zavala speaks, then the climb.
        { id = "pt_damaged_hall_stairs_door", raw = volume(389),
          on_enter = function(context) cue(context, B.CUE_ZAVALA_DOOR, "Zavala past the door") end },
        -- pt_deck_start (390): the platform, its first set and the small Cabal ship.
        { id = "pt_deck_start", raw = volume(390),
          on_enter = function(context) B.platform(context) end },
        -- Leaving pt_deck_hardpoint (392): the ship door opens on the whole melee set.
        { id = "pt_deck_hardpoint", raw = volume(392),
          on_exit = function(context)
              M.set_device_position(context, slot(58, 23), 1.0, false, "d_ship_door")
              M.spawn_squad(context, slot(36, 1), AI_DECK)  -- sq_ship_door_melee
              M.spawn_squad(context, slot(37, 1), AI_DECK)  -- sq_ship_door_melee_b
              M.spawn_squad(context, slot(38, 1), AI_DECK)  -- sq_ship_door_melee_c
              M.spawn_squad(context, slot(39, 1), AI_DECK)  -- sq_ship_door_melee_d
              M.spawn_squad(context, slot(40, 1), AI_DECK)  -- sq_ship_door_melee_e
          end },
        -- pt_airlock (395): the ship-entry set.
        { id = "pt_airlock", raw = volume(395),
          on_enter = function(context)
              M.spawn_squad(context, slot(41, 1), AI_DECK)  -- sq_ship_entry_a_a
              M.spawn_squad(context, slot(42, 1), AI_DECK)  -- sq_ship_entry_a_b
              M.spawn_squad(context, slot(43, 1), AI_DECK)  -- sq_ship_entry_a_b_o
              M.spawn_squad(context, slot(44, 1), AI_DECK)  -- sq_ship_entry_a_c
          end },
        -- pt_skybattle_navmode_2b (nav registry, volume 19): the shield room.
        { id = "pt_skybattle_navmode_2b", raw = nav_volume(19),
          on_enter = function(context)
              M.spawn_squad(context, slot(45, 1), AI_DECK)  -- sq_shield_snipes
              M.spawn_squad(context, slot(46, 1), AI_DECK)  -- sq_shield_front
              M.spawn_squad(context, slot(47, 1), AI_DECK)  -- sq_shield_mid
              M.spawn_squad(context, slot(48, 1), AI_DECK)  -- sq_shield_rear
              cue(context, B.CUE_SHIELD_ROOM, "entering the shield room")
          end },
        -- pt_engine_room_lower (399): the melee squad and the inner door.
        { id = "pt_engine_room_lower", raw = volume(399),
          on_enter = function(context)
              M.spawn_squad(context, slot(49, 1), AI_DECK)  -- sq_shield_melee
              M.set_device_position(context, slot(59, 23), 1.0, false, "d_ship_door_enter")
          end },
        -- pt_destroy_battleship (nav registry, volume 41): the generator itself.
        { id = "pt_destroy_battleship", raw = nav_volume(41),
          on_enter = function(context) B.battleship(context) end },
        -- The escape, on the way out: one explosion scene per volume.
        { id = "pt_escape_explosion_a", raw = volume(293),
          on_enter = function(context) M.start_scene(context, "ship_explosion_a") end },
        { id = "pt_escape_explosion_b", raw = volume(294),
          on_enter = function(context) M.start_scene(context, "ship_explosion_b") end },
        -- pt_goto_end (nav registry, volume 49): fade, then the outro cinematic.
        { id = "pt_goto_end", raw = nav_volume(49),
          on_enter = function(context) B.ending(context) end },
    }

    ----------------------------------------------------------------------------------------
    -- The ending
    ----------------------------------------------------------------------------------------

    -- outro_cinematic._cinematic (slot/80b50126) belongs to region 9, bubble 1's ordinal-1 state:
    -- proved live by the client's own started incident, which named registry 42e8f541 -- that
    -- state's hash.
    --
    -- The move is the same one as boulevard -> ship, and like it, it MUST carry a spawn set: a
    -- host teleport places the party from one, and with none the arm never completes (the client
    -- sits at teleport_state=1 and never reports the region). Bubble 1 offers exactly one, the
    -- destination's Default -- spawn sets are declared per MAP bubble, so the set the ship uses
    -- serves this bubble too.
    local OUTRO_CINEMATIC = "slot/80b50126/000000/0000/0006"
    local OUTRO_REGION = 9
    local OUTRO_SPAWN = 0x2EA8FB98
    local FADE_FILTER, FADE_HOPON = slot(208, 34), slot(153, 26)

    B.ENDING_TRAVEL_MS = 1500 -- black before the region changes

    function B.ending(context)
        if B.ending_started then return end
        B.ending_started = true
        local ok, err = pcall(function()
            context:slot(FADE_FILTER):set_object_filter{ players = true }
            context:slot(FADE_HOPON):set_mission_effect{
                filter = context:slot(FADE_FILTER), enabled = true, revision = 1 }
        end)
        context:probe("sky: ending fade ok=" .. tostring(ok) .. " err=" .. tostring(err))
        context:start_timer("sky_ending_travel", B.ENDING_TRAVEL_MS)
    end

    function B.travel_to_outro(context)
        M.region_on_held = function(ctx, held)
            if held ~= OUTRO_REGION then return end
            M.region_on_held = nil
            local ok, err = pcall(function()
                ctx:slot(OUTRO_CINEMATIC):set_cinematic_active{ active = true }
            end)
            ctx:probe("sky: outro cinematic ok=" .. tostring(ok) .. " err=" .. tostring(err))
        end
        M.select_region(context, OUTRO_REGION, { spawn_set_hash = OUTRO_SPAWN })
    end

    -- The movie ends, or the player skips it: stop it. Nothing follows, this is the end of the
    -- mission, so the party is left where the cinematic state put it.
    function B.on_cinematic_terminated(context, state, event)
        if not B.ending_started or B.outro_stopped then return end
        B.outro_stopped = true
        local ok, err = pcall(function()
            context:slot(OUTRO_CINEMATIC):set_cinematic_active{ active = false }
        end)
        context:probe("sky: outro stopped ok=" .. tostring(ok) .. " err=" .. tostring(err))
    end

    -- The generator room. The objects are absent by default, and the ones that spin or carry a
    -- beam stay inert until their device is driven, so each is instantiated and then run.
    -- Which device animates what is not readable from the package: they are labelled in the log
    -- so the ones that do nothing can be dropped.
    local TURBINES = { 142, 144, 146 }             -- shield_generator_a/b/c
    local TURBINE_DEVICES = { 143, 145, 147 }      -- their own devices: rotation and beam
    local GENERATOR_DEVICES = {
        { 63, "d_shield_gen_collar" },             -- the column's rotating collar
        { 64, "d_shield_gen_core" },               -- the column's energy core
        { 61, "d_shield_gen_a" },
        { 62, "d_shield_gen_b" },
        { 159, "d_heat_sink_glows" },
    }
    -- Each turbine lights its own panel as it goes down; the lights are not on at the start.
    local TURBINE_LIGHT = { [142] = 160, [144] = 161, [146] = 162 } -- d_gen_a/b/c_lights
    -- Which turbine each device drives, for reading a destruction off the Sense channel.
    local DEVICE_TURBINE = { [143] = 142, [145] = 144, [147] = 146 }
    -- Position the generator devices are driven to. The beams are not separate objects -- no
    -- type-4 in this registry carries one -- so they must be a state of the turbine and column
    -- objects. If 1.0 leaves them inert, this is the value to sweep (gen_sweep.lua).
    B.GENERATOR_POSITION = 1.0
    -- Silence that separates two bursts of reports on one device.
    B.BURST_GAP_MS = 2000
    B.LAST_CUE_MS = 2000        -- the last turbine's line waits for the blast to land
    B.ESCAPE_DIRECTIVE_MS = 6000 -- after that line: "Escape the command ship" and the crawler
    B.ESCAPE_CUE_MS = 2000      -- Cue 92 follows the directive

    function B.battleship(context)
        for _, index in ipairs(TURBINES) do
            M.set_door_object(context, slot(index, 4), true, "shield_generator")
        end
        M.set_door_object(context, slot(148, 4), true, "o_shield_gen_a")
        M.set_door_object(context, slot(149, 4), true, "o_shield_gen_b")
        for _, index in ipairs(TURBINE_DEVICES) do
            M.set_device_position(context, slot(index, 23), B.GENERATOR_POSITION, false, "turbine device")
        end
        for _, entry in ipairs(GENERATOR_DEVICES) do
            M.set_device_position(context, slot(entry[1], 23), B.GENERATOR_POSITION, false, entry[2])
        end
        B.turbines_down = 0
        B.turbine_seen = {}
        B.turbines_armed = true
        B.device_seen = {}
        M.set_directive(context, DIRECTIVE_OVERLOAD_GENERATOR, "Overload the generator",
                        { 0, TURBINE_COUNT }, nil, OVERLOAD_ELEMENT)
        cue(context, B.CUE_BATTLESHIP, "at the battleship")
    end

    -- Nothing typed reports a turbine's destruction: the client publishes its object level once
    -- at instantiation and never again, and the mission declares no type-20 damage monitor
    -- anywhere. So the raw Sense channel is listened to while the beat is live, bounded, to see
    -- whether anything at all names those slots when one goes down.
    function B.on_sense_update(context, state, event)
        if not B.turbines_armed then return end
        local index = event.slot_index
        -- A destroyed turbine stops spinning, which means its device changes state -- and a
        -- device's state change is what the Sense channel reports. Our own drive to 1.0 makes
        -- the same device report a burst when the beat arms, so bursts are counted rather than
        -- timed: the first burst on a device is ours, the next one is the destruction. A fixed
        -- settle delay lost any turbine destroyed before it elapsed.
        local turbine = DEVICE_TURBINE[index]
        if turbine == nil then return end
        local now = M.clock_ms(context) or 0
        local seen = B.device_seen[index]
        local new_burst = seen == nil or (now - seen.last) >= B.BURST_GAP_MS
        if seen == nil then
            seen = { bursts = 0, last = now }
            B.device_seen[index] = seen
        end
        if new_burst then seen.bursts = seen.bursts + 1 end
        seen.last = now
        B.sense_reports = (B.sense_reports or 0) + 1
        if B.sense_reports <= 200 then
            context:probe(string.format("sky: sense device=%d turbine=%d burst=%d new=%s",
                index, turbine, seen.bursts, tostring(new_burst)))
        end
        -- Burst 1 is our own drive; anything later is the device stopping.
        if seen.bursts < 2 or B.turbine_seen[turbine] then return end
        B.turbine_down(context, turbine)
    end

    -- The client publishes health for anything damageable. A turbine reaching zero is the
    -- destruction signal; nothing else in this mission reports it.
    function B.on_damage_state(context, state, event)
        if not B.turbines_armed then return end
        local index = event.slot_index
        context:probe(string.format("sky: damage type=%s index=%s health=%s shield=%s rev=%s",
            tostring(event.slot_type), tostring(index), tostring(event.health),
            tostring(event.shield), tostring(event.revision)))
        local watched = false
        for _, turbine in ipairs(TURBINES) do watched = watched or index == turbine end
        if not watched or B.turbine_seen[index] then return end
        local health = tonumber(event.health)
        if health == nil or health > 0 then return end
        B.turbine_down(context, index)
    end

    -- A turbine's destruction is read from the object's own level: the client stops reporting it
    -- alive (or present). There is no type-20 damage monitor in this registry to watch instead.
    function B.on_object_state(context, state, event)
        if not B.turbines_armed then return end
        -- Diagnostic while the signal is unknown: print what the client reports for any object
        -- while the generator beat is live, bounded so it cannot flood.
        B.object_reports = (B.object_reports or 0) + 1
        if B.object_reports <= 40 then
            context:probe(string.format("sky: object_state registry=%s type=%s index=%s present=%s alive=%s gen=%s",
                tostring(event.registry_key), tostring(event.slot_type), tostring(event.slot_index),
                tostring(event.present), tostring(event.alive), tostring(event.generation)))
        end
        local index = event.slot_index
        local watched = false
        for _, turbine in ipairs(TURBINES) do watched = watched or index == turbine end
        if not watched or B.turbine_seen[index] then return end
        local gone = event.alive == false or event.present == false
        if not gone then return end
        B.turbine_down(context, index)
    end

    function B.turbine_down(context, index)
        B.turbine_seen[index] = true
        B.turbines_down = B.turbines_down + 1
        context:probe(string.format("sky: turbine %d destroyed (%d/%d)",
            index, B.turbines_down, TURBINE_COUNT))
        M.set_directive(context, DIRECTIVE_OVERLOAD_GENERATOR,
                        string.format("Overload the generator %d/%d", B.turbines_down, TURBINE_COUNT),
                        { B.turbines_down, TURBINE_COUNT }, nil, OVERLOAD_ELEMENT)
        -- That turbine's own panel lights up.
        local light = TURBINE_LIGHT[index]
        if light then
            M.set_device_position(context, slot(light, 23), 1.0, false, "turbine light")
        end
        if B.turbines_down >= TURBINE_COUNT then
            -- The last line waits for the blast to land before it speaks.
            context:start_timer("sky_last_cue", B.LAST_CUE_MS)
        else
            cue(context, B.CUE_TURBINE[B.turbines_down], "turbine " .. B.turbines_down .. " down")
        end
        if B.turbines_down == 2 then
            -- The heat sinks stop glowing once the second turbine is gone.
            M.set_device_position(context, slot(159, 23), 0.0, false, "d_heat_sink_glows off")
        end
        if B.turbines_down >= TURBINE_COUNT then
            B.turbines_armed = false
            -- The way out of the generator room.
            M.set_device_position(context, slot(60, 23), 1.0, false, "d_ship_door_exit")
            M.set_device_position(context, slot(163, 23), 1.0, false, "d_gen_exit_lights")
        end
    end

    ----------------------------------------------------------------------------------------
    -- The platform
    ----------------------------------------------------------------------------------------

    -- Four of the deck squads carry a spawn rule distinct from their spawner config, which is
    -- the hangar drop-pod pattern: placed through that rule they arrive by whatever it authors
    -- instead of appearing at their anchor. The others (a_a, b_a, c_a) have no separate rule and
    -- do pop in place. Whether these rules are a dropship unload, a fall or a phase-in is not
    -- readable from the package -- only a live placement tells.
    local RULE = {         -- squad slot index -> its type-66 rule slot index
        [16] = 229,        -- sq_deck_front_a_b
        [17] = 231,        -- sq_deck_front_a_c
        [22] = 237,        -- sq_deck_front_b_b
        [23] = 239,        -- sq_deck_front_b_c
    }

    -- Places a squad through its authored spawn rule (lane 1, as validated in the hangar).
    local function place_by_rule(context, index, opts, label)
        local rule = RULE[index]
        if rule == nil then
            M.spawn_squad(context, slot(index, 1), opts)
            return
        end
        M.assign_objective(context, slot(index, 1), opts.objective)
        local ok, err = pcall(function()
            context:squad(slot(index, 1)):place{
                retire_on_return = true,
                spawn_rule = context:slot(slot(rule, 66)), spawn_lane = 1 }
        end)
        context:probe(string.format("sky: %s by rule ok=%s err=%s", label, tostring(ok), tostring(err)))
    end

    B.DECK_B_MS = 4000    -- the b_* set follows the dropship's arrival

    -- Entering pt_deck_start: the first set plus the small Cabal ship that comes in to attack.
    function B.platform(context)
        M.spawn_squad(context, slot(12, 1), AI_DECK)          -- sq_deck_front_a_a (3, at its anchor)
        place_by_rule(context, 16, AI_DECK, "sq_deck_front_a_b")
        place_by_rule(context, 17, AI_DECK, "sq_deck_front_a_c")
        M.spawn_squad(context, slot(154, 1), AI_DECK)         -- sq_dropship_a
        context:start_timer("sky_deck_b", B.DECK_B_MS)
    end

    B.timers = {
        sky_hawk_leaves = function(context)
            M.set_device_position(context, slot(164, 23), 1.0, false, "d_hawk (Holliday leaves)")
            cue(context, B.CUE_HAWK_LEAVES, "Holliday leaving")
        end,
        sky_first_directive = function(context)
            M.set_directive(context, DIRECTIVE_DISABLE_SHIELDS, "Disable the shields")
        end,
        sky_pod_doors = function(context) B.open_pod_doors(context) end,
        sky_last_cue = function(context)
            cue(context, B.CUE_TURBINE[TURBINE_COUNT], "the last turbine")
            context:start_timer("sky_escape_directive", B.ESCAPE_DIRECTIVE_MS)
        end,
        sky_escape_directive = function(context)
            M.set_directive(context, DIRECTIVE_ESCAPE_SHIP, "Escape the command ship")
            M.spawn_squad(context, slot(71, 1), AI_DECK)  -- cabal_crawler
            context:start_timer("sky_escape_cue", B.ESCAPE_CUE_MS)
        end,
        sky_escape_cue = function(context) cue(context, B.CUE_ESCAPE, "the escape") end,
        sky_ending_travel = function(context) B.travel_to_outro(context) end,
        sky_hall_melee = function(context)
            M.spawn_squad(context, slot(9, 1), AI_DAMAGED)   -- sq_damaged_hall_melee
            -- Armed once all three are placed, with the default confirmation: a squad seen alive
            -- must sit at zero for 1.5 s, and one that reports alive again is un-cleared. (The
            -- short confirmation used elsewhere is only safe for squads never fought.)
            M.track_clear(context, "sky_hall", { slot(8, 1), slot(7, 1), slot(9, 1) },
                          function(ctx)
                              B.hall_cleared = true
                              B.try_hall_cue(ctx)
                          end)
        end,
        sky_deck_b = function(context)
            -- b_b and b_c arrive through their own rules; b_a has none and lands at its anchor.
            M.spawn_squad(context, slot(18, 1), AI_DECK)      -- sq_deck_front_b_a (3)
            place_by_rule(context, 22, AI_DECK, "sq_deck_front_b_b")
            -- The deck boss arrives with b_b, on its own objective.
            M.spawn_squad(context, slot(33, 1), AI_DECK_ULTRA)  -- sq_deck_ultra
            place_by_rule(context, 23, AI_DECK, "sq_deck_front_b_c")
        end,
    }

    -- Reachable from rt_cmd for live testing: the module's locals are otherwise closed over.
    M.skybattle = B

    return B
end
