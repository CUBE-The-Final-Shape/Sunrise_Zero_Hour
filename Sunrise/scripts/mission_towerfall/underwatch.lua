-- Bubble 9 (Underwatch): spawn, the collapsing wall, the Frame/Centurion impale, Cayde's Golden
-- Gun at the Shaxx door, the civilian hallway, Shaxx's exit door, the hold-to-open door and
-- the red guard at the way out. Every beat here was matched live against the shipped mission.
return function(M)
    local B = { region = 72 } -- bubble 9

    ------------------------------------------------------------------------------------------
    -- Scenes of this bubble: name -> event-gate graph tag (resource entity +0xC0).
    ------------------------------------------------------------------------------------------
    local SCENE_CABAL_FIRST_CONTACT = "scene_cabal_first_contact" -- config 0x80B5098F, resource 0x80B8272C
    local SCENE_CENTURION_INTRO = "sc_centurion_intro"           -- config 0x80B509A1, resource 0x80B82774
    local SCENE_CAYDE_GOLDEN_GUN = "scene_cayde_golden_gun"      -- config 0x80B5099E, resource 0x80B8273C
    local SCENE_SHAXX = "scene_shaxx"                            -- config 0x80B50998, resource 0x80B8277D
    local CIVILIAN_SCENES = { "sc_civilian_ground_1", "sc_civilian_ground_2",
                              "sc_civilian_on_knees_crying", "sc_civilian_kneel" }
    M.register_scene(SCENE_CABAL_FIRST_CONTACT, 0x80C3DEF4)  -- 5 keys
    M.register_scene(SCENE_CENTURION_INTRO, 0x80BEB7EC)      -- 3 keys
    M.register_scene(SCENE_CAYDE_GOLDEN_GUN, 0x80C3DEF5)     -- 17 keys
    M.register_scene(SCENE_SHAXX, 0x80BEB7EF)                -- 5 keys, progress-gated (see below)
    M.register_scene("sc_civilian_ground_1", 0x80BEB801)     -- 3 keys
    M.register_scene("sc_civilian_ground_2", 0x80BEB804)     -- 2 keys
    M.register_scene("sc_civilian_on_knees_crying", 0x80BEB806) -- 2 keys
    M.register_scene("sc_civilian_kneel", 0x80BEB7F3)        -- 4 keys

    ------------------------------------------------------------------------------------------
    -- Combat objectives. Without one a Cabal squad simply stands where it was placed: the
    -- objective carries the combat areas and tactics, and M.dispatch_squad_state then relinks
    -- the squad to the cheapest reachable task group from the client's cost reports. Friendlies
    -- (the Frames, the red guards, Shaxx) are left alone -- these areas are the Cabal's.
    ------------------------------------------------------------------------------------------
    local AI_FIRST_CONTACT = { objective = "slot/80b50ca7/00000b/000b/0003" } -- obj_cabal_first_contact
    local AI_CENTURION = { objective = "slot/80b50ca7/000013/0013/0003" }     -- obj_centurion_intro
    -- The way out, past the hold-to-open door. The four post-gun squads there are friendlies
    -- (sq_frame_post_gun, sq_red_guard_ads, sq_red_guard_post_gun, sq_red_guard_post_gun_jump);
    -- obj_post_gun sits right beside them in the slot table, so it is theirs, not the Cabal's.
    local AI_POST_GUN = { objective = "slot/80b50ca7/000074/0074/0003" }      -- obj_post_gun

    -- Trigger registries: 0x9D8076E4 is the Underwatch object, the others are neighbours.
    local UNDERWATCH = 2642441956
    local DIRECTIVE_DEFEND_YOUR_HOME = 0x4FCECAB6
    local DIRECTIVE_FIND_ZAVALA = 0x432D2C95

    ------------------------------------------------------------------------------------------
    -- Spawn: fake fight squads, Cue 1, "Defend your home" once the line has been heard.
    -- Measured (rt_cmd, stopwatched): ~6s from the call to audible, ~4s of speech.
    ------------------------------------------------------------------------------------------
    function B.on_spawn(context, state)
        M.spawn_squad(context, "sq_frame_fake_fight")
        M.spawn_squad(context, "sq_red_guard_fake_fight")
        M.play_cue(context, 1)
        context:start_timer("uw_cue1_audible", 6000)
    end

    ------------------------------------------------------------------------------------------
    -- Wall beat: scene_cabal_first_contact. The Cabal is placed a beat before the wall device
    -- opens; the authored explosion VFX rides on one of the scene's keys.
    ------------------------------------------------------------------------------------------
    local wall_started = false

    local function on_intro_stand(context, state)
        wall_started = true
        M.retire_squad(context, "sq_frame_fake_fight")
        M.retire_squad(context, "sq_red_guard_fake_fight")
        if M.bind_scene(context, SCENE_CABAL_FIRST_CONTACT) then
            context:start_timer("uw_wall_advance", 1000)
        end
        M.spawn_squad(context, "squad_first_contact_cabal_backup_a", AI_FIRST_CONTACT)
        M.play_cue(context, 5)
    end

    ------------------------------------------------------------------------------------------
    -- Centurion beat: sc_centurion_intro. The Centurion is the one squad here with a type-2
    -- cell; created in a static pose before the scene so the scene can claim it (a squad placed
    -- with place{} fights and ignores the scene). Its class 0x80C1A52D declares 14 states in
    -- group 0xAFB11A12: #1 0x40FC40DA is a static pose, #9 0x8F797125 the impale itself.
    ------------------------------------------------------------------------------------------
    local CENTURION_GROUP = 0xAFB11A12
    local CENTURION_POSE = 0x40FC40DA

    local function on_centurion_intro(context, state)
        M.spawn_squad(context, "sq_frame")
        M.spawn_squad(context, "sq_centurion_intro_backup", AI_CENTURION)
        M.spawn_squad(context, "sq_centurion_intro_rush", AI_CENTURION)
        M.cell_action(context, "sq_centurion_intro__cell_1", CENTURION_GROUP, CENTURION_POSE)
    end

    local function on_centurion_scene(context, state)
        M.start_scene(context, SCENE_CENTURION_INTRO)
    end

    -- Doors past the Centurion are open by default: closed doors are objects that are absent.
    -- gun_door + door_interactable + d_gun_door make the hold-to-open door past Shaxx. The client
    -- keeps the last accepted interactable generation for the whole activity session, so each
    -- arming derives a newer one from the native clock.
    local gun_door_generation = 1

    local function on_centurion_reinforce(context, state)
        M.set_door_object(context, "o_shaxx_door_enter", true, "at pt_centurion_intro_reinforce")
        M.set_door_object(context, "o_shaxx_door_exit", true, "at pt_centurion_intro_reinforce")
        M.spawn_squad(context, "sq_shaxx") -- his scene does not spawn him
        M.set_door_object(context, "gun_door", true, "at pt_centurion_intro_reinforce")
        local clock = M.clock_ms(context)
        if clock then
            gun_door_generation = (clock // 1000) % 0x3FFFFFFF
            if gun_door_generation < 1 then gun_door_generation = 1 end
        else
            gun_door_generation = (state:variable("towerfall.door_generation") or 0) + 1
        end
        context:set_variable("towerfall.door_generation", gun_door_generation)
        local ok, err = pcall(function()
            context:slot("door_interactable"):set_interactable_object{ generation = gun_door_generation }
        end)
        context:probe("set_interactable_object(door_interactable, gen=" .. gun_door_generation
            .. ") ok=" .. tostring(ok) .. " err=" .. tostring(err))
        M.play_cue(context, 10)
    end

    ------------------------------------------------------------------------------------------
    -- Cayde beat: scene_cayde_golden_gun spawns Cayde and the three Legionaries itself, opens the
    -- door and plays Cues 12/13/15/16 itself; it never reports scene_finished, so the directive
    -- goes out on a timer.
    ------------------------------------------------------------------------------------------
    -- The scene opens the Shaxx door itself, and starting it the instant the player crosses the
    -- volume made the door snap open too early: held off by 3s. The Find Zavala directive stays
    -- measured from the scene, not from the trigger, so it moves with it.
    local function on_cayde_door(context, state)
        context:start_timer("uw_cayde_scene", 3000)
    end

    ------------------------------------------------------------------------------------------
    -- Shaxx hallway. scene_shaxx's keys each stand for a player-progress event: 1 = Shaxx stands
    -- and talks, 2 = opens o_shaxx_door_exit, 4 and 5 = his two lines, 3 = shuts the door.
    -- Keys are a cumulative set, so 4 -> 5 -> 3 can be published out of index order.
    ------------------------------------------------------------------------------------------
    local shaxx_started = false

    local function shaxx_wake(context)
        if shaxx_started then return end
        shaxx_started = M.start_scene(context, SCENE_SHAXX, 1)
    end

    local function on_civilians_start(context, state)
        for _, name in ipairs(CIVILIAN_SCENES) do M.start_scene(context, name) end
        shaxx_wake(context)
    end

    local function on_shaxx_door(context, state)
        shaxx_wake(context)
        M.step_scene(context, SCENE_SHAXX, 2)
    end

    local function on_weapon_area(context, state)
        shaxx_wake(context)
        M.publish_scene_keys(context, SCENE_SHAXX, { 1, 2, 4 })
        context:start_timer("uw_shaxx_key5", 4000)
    end

    ------------------------------------------------------------------------------------------
    -- The hold-to-open door: native use receipt on door_interactable (type-4 slot 119).
    ------------------------------------------------------------------------------------------
    function B.on_object_interacted(context, state, event)
        local slot = context:slot("door_interactable")
        if event.registry_key ~= slot.registry_key or event.slot_type ~= slot.slot_type
            or event.slot_index ~= slot.slot_index then
            return false
        end
        M.set_device_position(context, "d_gun_door", 1.0, false, "door_interactable used")
        gun_door_generation = gun_door_generation + 1
        context:set_variable("towerfall.door_generation", gun_door_generation)
        local ok, err = pcall(function()
            context:slot("door_interactable"):set_interactable_object{ generation = gun_door_generation, active = false }
        end)
        context:probe("set_interactable_object(door_interactable, off) ok=" .. tostring(ok) .. " err=" .. tostring(err))
        return true
    end

    ------------------------------------------------------------------------------------------
    -- Timers
    ------------------------------------------------------------------------------------------
    B.timers = {
        uw_cue1_audible = function(context, state) context:start_timer("uw_cue1_end", 4000) end,
        uw_cue1_end = function(context, state)
            M.set_directive(context, DIRECTIVE_DEFEND_YOUR_HOME, "Defend your home")
        end,
        uw_wall_advance = function(context, state)
            M.advance_scene(context, SCENE_CABAL_FIRST_CONTACT)
            M.last_scene_activated = SCENE_CABAL_FIRST_CONTACT
            M.spawn_squad(context, "squad_first_contact_cabal", AI_FIRST_CONTACT)
            context:start_timer("uw_wall_keys", 250)
        end,
        uw_wall_keys = function(context, state)
            M.set_device_position(context, "d_underwatch_collapsing_wall", 1.0, true, "with the scene keys")
            M.step_scene(context, SCENE_CABAL_FIRST_CONTACT)
        end,
        uw_cayde_scene = function(context, state)
            M.start_scene(context, SCENE_CAYDE_GOLDEN_GUN)
            context:start_timer("uw_find_zavala", 16500)
        end,
        uw_find_zavala = function(context, state)
            M.set_directive(context, DIRECTIVE_FIND_ZAVALA, "Find Zavala")
        end,
        uw_shaxx_key5 = function(context, state)
            M.publish_scene_keys(context, SCENE_SHAXX, { 1, 2, 4, 5 })
            context:start_timer("uw_shaxx_key3", 1500)
        end,
        uw_shaxx_key3 = function(context, state)
            M.publish_scene_keys(context, SCENE_SHAXX, { 1, 2, 3, 4, 5 })
        end,
    }

    ------------------------------------------------------------------------------------------
    -- Watches, in the order the player meets them. Unnamed volumes (no resolvable type-31
    -- source) are armed by their type-60 identity or by their debug-panel row in bubble 9.
    ------------------------------------------------------------------------------------------
    B.watches = {
        { id = "pt_start" },
        { id = "pt_sc_underwatch_intro_stand", on_enter = on_intro_stand },
        -- whole starting zone; Cue 6 when the player leaves it
        -- (guarded: the spawn transition can look like an exit before the wall beat has run)
        { id = "underwatch_start_zone", raw = { registry_key = 2418521761, slot_type = 60, slot_index = 23 },
          once = false,
          on_exit = function(context, state, w)
              if wall_started and not w.cue6_done then
                  w.cue6_done = true
                  M.play_cue(context, 6)
                  M.release_watch(context, w)
              end
          end },
        { id = "pt_centurion_intro", on_enter = on_centurion_intro },
        -- unnamed volume between pt_centurion_intro (280) and _reinforce (282): the impale trigger
        { id = "sc_centurion_intro_trigger", raw = { registry_key = UNDERWATCH, slot_type = 60, slot_index = 281 },
          on_enter = on_centurion_scene },
        { id = "pt_centurion_intro_reinforce", on_enter = on_centurion_reinforce },
        -- standing at the closed Shaxx door = inside pt_shaxx_enters (320); no type-31 source resolves
        { id = "pt_shaxx_enters", raw = { registry_key = UNDERWATCH, slot_type = 60, slot_index = 320 },
          on_enter = on_cayde_door },
        -- the hallway zone (debug panel bubble 9 row 28 = volume 249): Shaxx stands up
        { id = "shaxx_wake_zone", bubble = { 9, 28 }, on_enter = function(context) shaxx_wake(context) end },
        { id = "pt_sc_civilians_start", raw = { registry_key = UNDERWATCH, slot_type = 60, slot_index = 324 },
          on_enter = on_civilians_start },
        { id = "pt_start_shaxx_scene", raw = { registry_key = UNDERWATCH, slot_type = 60, slot_index = 275 },
          on_enter = on_shaxx_door },
        -- past the door: Shaxx's two lines, then he shuts it
        { id = "pt_weapon", raw = { registry_key = 0x9027B6A1, slot_type = 60, slot_index = 4 },
          on_enter = on_weapon_area },
        -- the way out of the Underwatch (debug panel bubble 9 row 68)
        { id = "cue30_zone", bubble = { 9, 68 }, on_enter = function(context) M.play_cue(context, 30) end },
        { id = "pt_weapon_complete", raw = { registry_key = 0x026087A0, slot_type = 60, slot_index = 3 },
          on_exit = function(context) M.spawn_squad(context, "sq_red_guard_ads", AI_POST_GUN) end },
    }

    return B
end
