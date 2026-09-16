-- Bubble 0 (Boulevard / Bazaar, region 0): after the plaza. Runtime registry 0x7BA8F95D, SDK
-- registry 80b5011f. Part 1 (as described from the footage): the entry door, the stairs with
-- three Cabal and a lone one behind, Ikora arriving from the left and blasting the three, her
-- two lines, the Cabal ship she jumps onto, two lines (Ghost, Zavala) on the way, a pod with
-- three Cabal in front of the bazaar door, the door on the right opening on an Incendior, and
-- two more Incendiors inside the bazaar.
return function(M)
    local B = { region = 0 }

    local REG = "slot/80b5011f/%s/%s/%s" -- SDK slot id of the boulevard registry
    local function slot(index, slot_type)
        return string.format(REG, string.format("%06x", index), string.format("%04x", index),
                             string.format("%04x", slot_type))
    end

    -- Names shared with other bubbles are addressed by full id.
    local SQ_IKORA = slot(17, 1)          -- sq_ikora (the Underwatch has one too)
    local MISSILES = { slot(29, 4), slot(30, 4), slot(31, 4) } -- o_cabal_missile_1..3
    local BLASTED = { "squad_cabal_blasted_1", "squad_cabal_blasted_2",
                      "squad_cabal_blasted_3", "squad_cabal_blasted_4" }
    local BAZAAR_AI = { objective = "obj_bazaar" }

    -- Ikora's scene (config 0x80B500EE, resource 0x80B8250B): participants sq_ikora,
    -- squad_cabal_blasted_1..4, squad_invisible_shooting_target and four point sets; 10 keys.
    M.register_scene("scene_ikora_boulevard", 0x80C3DD7D, {
        slot = slot(16, 43),
        scene = "symbol/80b500bc/0000/0000/80b50121/28/000000/80b5011f/0010/002b" })
    -- The Incendior behind the bazaar door (config 0x80B500E3, resource 0x80B82506):
    -- participant sq_flame only; 2 keys.
    M.register_scene("sc_pyro_intro", 0x80C3DD7A, {
        slot = slot(10, 43),
        scene = "symbol/80b500bc/0000/0000/80b50121/28/000000/80b5011f/000a/002b" })

    local DIRECTIVE_LEAVE_PLAZA = 0xD56AE99D
    local DIRECTIVE_BOARD_SHIP = 0x5DC9D705 -- "Board the command ship", ~2 s after the pod drop

    function B.on_spawn(context, state)
        -- Starting here skips the plaza, whose last beat set this directive.
        M.set_directive(context, DIRECTIVE_LEAVE_PLAZA, "Leave the Plaza and find the Speaker")
    end

    -- The stairs: the three Cabal (blasted_2/3/4, around x -88 / y 31-34) and the lone one
    -- further back (blasted_1, x -100 / y 40) are Ikora's scene victims: placed as the entry
    -- door opens, on the bazaar objective at task group -1 and held there (the plaza's
    -- "passive" placement). The scene spawns Ikora itself (test A: bind -> activate -> keys
    -- with nothing placed played her whole entrance) but blasts only victims that exist.
    local setup_done = false
    function B.setup(context)
        if setup_done then return end
        setup_done = true
        -- The scene is activated first (no key), then its victims are placed into it: a
        -- participant placed while the scene runs is claimed by it, whereas a squad placed
        -- before the activation had already engaged the player (never claimed, no blast).
        if M.bind_scene(context, "scene_ikora_boulevard") then
            M.advance_scene(context, "scene_ikora_boulevard")
        end
        -- Scene participant (0 members). The bare name does not resolve, so it goes by full id.
        M.spawn_squad(context, slot(18, 1)) -- squad_invisible_shooting_target
        for _, name in ipairs(BLASTED) do
            M.spawn_squad_full(context, name, 1, { objective = "obj_bazaar", hold = true })
        end
    end

    B.watches = {
        -- Entering the boulevard from the plaza (registry 0xBB7B62E0, volume 7, x -59..-37 /
        -- y -33..-18): the victims are placed before the door if the mission comes from the plaza.
        -- Crossed on the way out of the plaza, before this region is even requested.
        { id = "pt_goto_boulevard", region = false, raw = { registry_key = 0xBB7B62E0, slot_type = 60, slot_index = 7 },
          on_enter = function(context) context:probe("blvd: pt_goto_boulevard entered") end },
        -- pt_start_ikora (0x7BA8F95D volume 55, x -75..-55 / y 26..37): key 2 starts Ikora's
        -- whole entrance (key 1 alone does nothing visible); keys 3-10 still to identify
        -- (ik_3..ik_10 from rt_cmd).
        { id = "pt_start_ikora", raw = { registry_key = 0x7BA8F95D, slot_type = 60, slot_index = 55 },
          on_enter = function(context)
              -- Leaving and re-entering the volume replayed her whole entrance: guard the beat
              -- itself rather than relying on the watch being released.
              if B.ikora_started then return end
              B.ikora_started = true
              M.publish_scene_keys(context, "scene_ikora_boulevard", { 1, 2 })
              -- Her blast is not part of the scene: o_nova_bomb_projectile is a type-4 object
              -- that flies in and detonates on instantiation (validated live: it kills the
              -- three), timed on her arrival.
              context:start_timer("blvd_nova_bomb", B.NOVA_DELAY_MS)
              -- The pod touches down ~17 s into the scene in the footage; ~2 s of fall.
              context:start_timer("blvd_pod", B.POD_DELAY_MS)
          end },
        -- pt_start_big_ship (volume 54, x -75..-54 / y 13..37) is the spawn area, in front of
        -- the closed entry door d_door_gating: the door opens 3 s after entering it.
        { id = "pt_start_big_ship", raw = { registry_key = 0x7BA8F95D, slot_type = 60, slot_index = 54 },
          on_enter = function(context) context:start_timer("blvd_entry_door", B.ENTRY_DOOR_DELAY_MS) end },
        { id = "pt_start_cabal_movement", raw = { registry_key = 0x7BA8F95D, slot_type = 60, slot_index = 56 },
          on_enter = function(context) context:probe("blvd: pt_start_cabal_movement entered") end },
        -- Bazaar waves: sq_bazaar_a_a (4, x -98 / y 111) on entering pt_bazaar_mid (volume 84,
        -- y 69..79); sq_bazaar_a_c (3, x -100 / y 139) + Cue 75 on entering pt_bazaar_farther
        -- (volume 87, y 98..108); sq_bazaar_finale once a_c is dead; Holliday once the finale
        -- is dead.
        { id = "pt_bazaar_mid", raw = { registry_key = 0x7BA8F95D, slot_type = 60, slot_index = 84 },
          on_enter = function(context) M.spawn_squad(context, "sq_bazaar_a_a", BAZAAR_AI) end },
        { id = "pt_bazaar_farther", raw = { registry_key = 0x7BA8F95D, slot_type = 60, slot_index = 87 },
          on_enter = function(context)
              M.spawn_squad(context, "sq_bazaar_a_c", BAZAAR_AI)
              M.play_cue(context, 75)
              -- The finale comes once everything placed so far is dead, read from the
              -- objective's kill counter. alive_count is not a signal: a_c reported 3 then 0
              -- within 0.3 s of its placement and the finale spawned with a_c untouched.
              B.wait_kills(context, "finale spawn", B.KILLS_BEFORE_FINALE,
                           function(ctx) B.finale(ctx) end)
          end },
    }

    -- sq_bazaar_finale (2 members: one 0x80C19B1F + one 0x80C1A52D, x -112 / y 136; its own
    -- rule sr_boulevard_finale differs from its spawner -- plain placement first).
    function B.finale(context)
        M.spawn_squad_full(context, "sq_bazaar_finale", 1, BAZAAR_AI)
        B.wait_kills(context, "finale", B.KILLS_BEFORE_FINALE + B.FINALE_MEMBERS,
                     function(ctx) B.holliday(ctx) end)
    end

    -- End of the bazaar: Holliday's Hawk arrives (o_hawk_1; _2/_3 are the other players'
    -- ships), Cue 76 is her line, then the ride to the command ship.
    --
    -- `sky_battle` lives in bubble 8, whose slice set 64 carries two states: region 64 (the
    -- playable ship) and region 65 (its cinematic sibling, which owns mid_cinematic._cinematic
    -- and has no world of its own). Both name the same slice set, so travelling straight to 65
    -- loads the same data as 64 would -- there is no reason to stop at 64 on the way in.
    -- A cinematic is only deliverable while the client HOLDS the region its state owns (a merely
    -- requested destination is not enough), so the move goes through M.select_region and the
    -- activation waits for the held report. The fade is left to the cinematic, which owns it.
    --
    -- Both legs name the ship's authored "Default" spawn: without a spawn set the client picks an
    -- arbitrary point (see select_state{ spawn_set_hash = }).
    -- ho_fade_out through _object_filter_fade_out, both by full slot id: the bare names are
    -- shared with other objects and do not resolve.
    local FADE_FILTER, FADE_HOPON = slot(48, 34), slot(32, 26)
    local SHIP_REGION, CINEMATIC_REGION = 64, 65
    local SHIP_SPAWN = 0x2EA8FB98
    local MID_CINEMATIC = "slot/80b508fc/000000/0000/0006"

    function B.holliday(context)
        M.set_door_object(context, "o_hawk_1", true, "Holliday's Hawk")
        context:start_timer("blvd_holliday_cue", B.HOLLIDAY_CUE_MS)
        context:start_timer("blvd_holliday_ship", B.HOLLIDAY_SHIP_MS)
    end

    -- The fade is driven here, one FADE_LEAD_MS before the travel, so the screen is already
    -- black when the cinematic state loads.
    function B.fade_out(context)
        local ok, err = pcall(function()
            context:slot(FADE_FILTER):set_object_filter{ players = true }
            context:slot(FADE_HOPON):set_mission_effect{
                filter = context:slot(FADE_FILTER), enabled = true, revision = 1 }
        end)
        context:probe("blvd: fade out ok=" .. tostring(ok) .. " err=" .. tostring(err))
        context:start_timer("blvd_board_travel", B.FADE_LEAD_MS)
    end

    function B.board_ship(context)
        M.region_on_held = function(ctx, held)
            if held ~= CINEMATIC_REGION then return end
            local ok, err = pcall(function()
                ctx:slot(MID_CINEMATIC):set_cinematic_active{ active = true }
            end)
            ctx:probe("blvd: mid cinematic ok=" .. tostring(ok) .. " err=" .. tostring(err))
        end
        M.select_region(context, CINEMATIC_REGION, { spawn_set_hash = SHIP_SPAWN })
    end

    -- The cinematic ends (or the player skips it): stop it and put the player down at the ship's
    -- own start point, ready for the sky_battle beats.
    function B.on_cinematic_terminated(context, state, event)
        if B.ship_landed then return end
        B.ship_landed = true
        local ok, err = pcall(function()
            context:slot(MID_CINEMATIC):set_cinematic_active{ active = false }
        end)
        context:probe("blvd: mid cinematic stopped ok=" .. tostring(ok) .. " err=" .. tostring(err))
        M.select_region(context, SHIP_REGION, { spawn_set_hash = SHIP_SPAWN })
    end

    -- The pod with three Cabal lands in front of the bazaar door (sq_bazaar_start, anchor
    -- x -116 / y 42): a one-member squad whose own rule sr_boulevard_start (0x80B50092, not
    -- its spawner config) is the pod, placed with count 3 and that rule like the hangar's a_b
    -- pod (validated: three Cabal out of the pod). The door on the right opens once they die.
    function B.drop_pod(context)
        M.assign_objective(context, "sq_bazaar_start", "obj_bazaar")
        local ok, err = pcall(function()
            local squad = context:squad("sq_bazaar_start")
            local counts = squad:counts()
            for i = 1, counts.count do counts:set(i, 3) end
            squad:place{ counts = counts, retire_on_return = true,
                         spawn_rule = context:slot("sr_boulevard_start"), spawn_lane = 1 }
        end)
        context:probe("blvd: sq_bazaar_start pod ok=" .. tostring(ok) .. " err=" .. tostring(err))
        -- The door opens on the objective's kill counter (obj_bazaar, slot 1): a pod squad's
        -- alive_count flickers to 0 right after landing and its living members never report
        -- again, so the clear tracker opened the door early.
        B.wait_kills(context, "pod", B.POD_MEMBERS, function(ctx) B.open_bazaar(ctx) end)
    end

    -- Everything in the bazaar is placed on obj_bazaar, so its kill counter, summed over the
    -- whole beat, says how much of what was placed is dead: the door at 3 (the pod), the finale
    -- once everything before it is gone, Holliday once the finale is. This is the plaza's method.
    -- Neither per-squad signal works here: alive_count flickers to zero right after a placement
    -- (a_c 0.3 s in, the finale 5 s in) and reports the same death several times, and a kill on
    -- the objective names no squad -- read per squad, a_a's last death and a_c's first opened
    -- the ending with a_c still standing.
    B.POD_MEMBERS = 3
    B.KILLS_BEFORE_FINALE = 3 + 2 + 4 + 3 -- pod, a_b, a_a, a_c
    B.kills_total = 0

    -- Runs `fn` once the beat's kill total reaches `wanted`.
    function B.wait_kills(context, label, wanted, fn)
        B.kill_label, B.kill_wanted, B.kill_fn = label, wanted, fn
        B.try_kills(context)
    end

    function B.try_kills(context)
        if not B.kill_wanted or B.kills_total < B.kill_wanted then return end
        local fn = B.kill_fn
        B.kill_wanted, B.kill_fn = nil, nil
        fn(context)
    end

    -- objective_progress on obj_bazaar (type 3, index 1) carries one counter PER task group and
    -- per lane (`objective` is the block, `task` the lane): a squad the AI moves to another group
    -- mid-fight has its later kills reported under that group's counter, from 1 again. So a kill
    -- is the delta of the counter it arrived on (`previous_task_count` -> `task_count`), summed
    -- across every counter, never one counter's absolute value: read as one number, the finale's
    -- two deaths came in as 1/2, 1/2, 1/2 on three different counters and the Hawk never came.
    function B.on_objective_progress(context, state, event)
        -- obj_bazaar is index 1 of the boulevard registry; the hangar's objective is index 1 of
        -- its own, so the registry is part of the match.
        if event.registry_key ~= 0x7BA8F95D or event.slot_type ~= 3 or event.slot_index ~= 1 then
            return
        end
        local count = event.task_count or 0
        local previous = event.previous_task_count or (count - 1)
        local delta = count - previous
        if delta <= 0 then return end
        B.kills_total = B.kills_total + delta
        context:probe(string.format("blvd: kills %d (block %s task %s)%s", B.kills_total,
            tostring(event.objective), tostring(event.task),
            B.kill_wanted and string.format(", %s at %d", B.kill_label, B.kill_wanted) or ""))
        B.try_kills(context)
    end

    -- The bazaar door (d_door_bazaar) opens on an Incendior: sq_flame is sc_pyro_intro's only
    -- participant (no type-2 cell, so placed like Shaxx), then the scene; the two Incendiors
    -- inside (sq_bazaar_a_b, x -102 / y 90) come with the bazaar objective.
    function B.open_bazaar(context)
        M.spawn_squad(context, "sq_flame")
        M.set_device_position(context, "d_door_bazaar", 1.0, false, "bazaar door after the pod")
        context:start_timer("blvd_pyro_scene", 500)
        M.spawn_squad(context, "sq_bazaar_a_b", BAZAAR_AI)
    end

    B.HOLLIDAY_CUE_MS = 2000
    B.HOLLIDAY_SHIP_MS = 6000 -- after the finale dies: the fade starts here
    B.FADE_LEAD_MS = 1000     -- black before the cinematic state loads
    B.FINALE_MEMBERS = 2

    B.IKORA_SCENE_DELAY_MS = 500
    B.ENTRY_DOOR_DELAY_MS = 3000
    B.NOVA_DELAY_MS = 1500 -- after key 2, tuned by eye
    B.POD_DELAY_MS = 15000 -- after key 2 (touchdown ~17 s in the footage)

    B.timers = {
        blvd_entry_door = function(context)
            B.setup(context)
            M.set_device_position(context, "d_door_gating", 1.0, false, "entry door, 3 s after pt_start_big_ship")
        end,
        blvd_holliday_cue = function(context) M.play_cue(context, 76) end,
        blvd_holliday_ship = function(context) B.fade_out(context) end,
        blvd_board_travel = function(context) B.board_ship(context) end,
        blvd_pod = function(context)
            B.drop_pod(context)
            context:start_timer("blvd_board_directive", 2000)
        end,
        blvd_board_directive = function(context)
            M.set_directive(context, DIRECTIVE_BOARD_SHIP, "Board the command ship")
        end,
        blvd_nova_bomb = function(context)
            M.set_door_object(context, "o_nova_bomb_projectile", true, "Ikora's blast on the stairs group")
        end,
        blvd_ikora_scene = function(context)
            -- bind -> activate -> key 1 only; the rest by hand until identified.
            M.start_scene(context, "scene_ikora_boulevard", 1)
        end,
        blvd_pyro_scene = function(context) M.start_scene(context, "sc_pyro_intro") end,
        blvd_missile_1 = function(context) M.set_door_object(context, MISSILES[1], true, "o_cabal_missile_1 (boulevard)") end,
        blvd_missile_2 = function(context) M.set_door_object(context, MISSILES[2], true, "o_cabal_missile_2 (boulevard)") end,
        blvd_missile_3 = function(context) M.set_door_object(context, MISSILES[3], true, "o_cabal_missile_3 (boulevard)") end,
    }

    return B
end
