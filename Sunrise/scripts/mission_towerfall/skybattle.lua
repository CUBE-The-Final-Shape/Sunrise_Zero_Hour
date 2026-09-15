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

    -- The ship's two combat objectives: obj_damaged covers the entry and the corridor,
    -- obj_deck the platform. Squads are assigned before placement so the Cabal can move.
    local AI_DAMAGED = { objective = slot(0, 3) }  -- obj_damaged
    local AI_DECK = { objective = slot(1, 3) }     -- obj_deck

    local DIRECTIVE_DISABLE_SHIELDS = 0xDA1CA185
    local DIRECTIVE_REACH_GENERATOR = 0x57395492

    -- A cue is fired only once its constant is set, so an unidentified one logs instead of
    -- playing something wrong (found by ear with cue_N.lua through rt_cmd).
    B.CUE_HAWK_LEAVES = 77      -- Holliday as her Hawk pulls away
    B.CUE_GHOST_CONSOLE = 78    -- Ghost: interact with the hologram
    B.CUE_DOORS_OPEN = 79       -- Ghost, as the energy doors open
    B.CUE_HALL_EXIT = 82        -- as the player leaves the damaged hall
    B.CUE_ZAVALA_DOOR = nil     -- Zavala, past the door before the climb

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

    B.watches = {
        -- pt_sky_battle (volume 179): first steps off the spawn.
        { id = "pt_sky_battle", raw = volume(179),
          on_enter = function(context) context:probe("sky: pt_sky_battle entered") end },
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
        -- pt_deck_start (390): the platform. Three psions phase in, a dropship unloads, and
        -- Brann waits further out.
        { id = "pt_deck_start", raw = volume(390),
          on_enter = function(context) B.platform(context) end },
    }

    ----------------------------------------------------------------------------------------
    -- The platform
    ----------------------------------------------------------------------------------------

    B.DROPSHIP_MS = 6000  -- the small Cabal ship arrives after the psions
    B.BRANN_MS = 12000

    function B.platform(context)
        -- The psions phase in: o_psion_phase_in_a/b/c are the effects, the squads are the
        -- deck_front sets closest to the entrance.
        for _, index in ipairs({ 73, 74, 75 }) do
            M.set_door_object(context, slot(index, 4), true, "psion phase-in")
        end
        M.spawn_squad(context, slot(12, 1), AI_DECK)  -- sq_deck_front_a_a (the pair)
        M.spawn_squad(context, slot(16, 1), AI_DECK)  -- sq_deck_front_a_b (the one behind cover)
        context:start_timer("sky_dropship", B.DROPSHIP_MS)
        context:start_timer("sky_brann", B.BRANN_MS)
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
        sky_dropship = function(context)
            -- The dropship that unloads on the platform, with its pilot cell.
            M.spawn_squad(context, slot(154, 1), AI_DECK)    -- sq_dropship_a
            M.spawn_squad(context, slot(22, 1), AI_DECK)     -- sq_deck_front_b_b
            M.spawn_squad(context, slot(17, 1), AI_DECK)     -- sq_deck_front_a_c
        end,
        sky_brann = function(context)
            M.spawn_squad(context, slot(33, 1), AI_DECK)     -- sq_deck_ultra (Brann)
            local ok, err = pcall(function() context:slot(slot(174, 5)):play_sequence{} end)
            context:probe("sky: Brann announce ok=" .. tostring(ok) .. " err=" .. tostring(err))
        end,
    }

    return B
end
