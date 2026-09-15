-- Bubble 4 (Military / hangar): first contact after leaving the Underwatch.
return function(M)
    local B = { region = 32 } -- bubble 4

    -- The hangar's one native combat objective (obj_hangar, type-3 objective_sensor). Every
    -- hangar squad is assigned to it before placement, which is what lets the Cabal move; the
    -- task group is then picked by cost (see M.assign_objective).
    local HANGAR_AI = { objective = "slot/80b5036a/000001/0001/0003" }

    -- Starting the mission here (START_REGION = 32) skips the Underwatch, whose last beat put
    -- "Find Zavala" on the HUD; restore it so the objective matches.
    -- config 0x80B50143, resource 0x80B8251A; graph resolved live from the resource.
    M.register_scene("sc_military_hallway_destruction", { resource = 0x80B8251A })
    -- Escape explosions (resource 0x80B82715 shared by a-d, graph 0x80BEB7B5, one key each, one
    -- explosion point set each). The bare names are shared with two other objects, hence full ids.
    M.register_scene("hangar_explosion_a", 0x80BEB7B5, {
        slot = "slot/80b5036a/00002a/002a/002b",
        scene = "symbol/80b500bc/0004/0000/80b5037e/28/000000/80b5036a/002a/002b" })
    M.register_scene("hangar_explosion_b", 0x80BEB7B5, {
        slot = "slot/80b5036a/00002b/002b/002b",
        scene = "symbol/80b500bc/0004/0000/80b5037e/28/000000/80b5036a/002b/002b" })

    function B.on_spawn(context, state)
        M.set_directive(context, 0x432D2C95, "Find Zavala")
    end

    -- pt_hangar_early: registry 0xAA9D42BE, volume 233 (x 58->76, y 36->69). Armed by its type-60
    -- identity like the other triggers whose type-31 source does not resolve by name.
    B.watches = {
        { id = "pt_hangar_early", raw = { registry_key = 0xAA9D42BE, slot_type = 60, slot_index = 233 },
          on_enter = function(context)
              M.spawn_squad(context, "sq_hangar_overlook_a_a", HANGAR_AI)
              -- Both Amanda gating doors are open by default: d_gating_amanda_start is the first
              -- door (past the pod), d_gating_amanda_hangar the second (identified live).
              M.set_device_position(context, "d_gating_amanda_start", 0.0, true, "shut at pt_hangar_early")
              M.set_device_position(context, "d_gating_amanda_hangar", 0.0, true, "shut at pt_hangar_early")
          end },
        -- The drop pod. o_cabal_drop_pod_military_hallway is the pod object (absent by default, like
        -- the doors); sc_military_hallway_destruction (one key, the shared start gate) opens it and
        -- lets its squad out (sq_military_hallway_destruction gave one Legionary; the shipped pod
        -- holds sq_hangar_a_b).
        -- Two events on one volume, so `once` is off and the watch is released by hand after the
        -- exit: a one-shot watch fires a single handler and is dropped, which would swallow the
        -- second one entirely.
        { id = "pt_hangar_spawn", raw = { registry_key = 0xAA9D42BE, slot_type = 60, slot_index = 226 },
          once = false,
          on_enter = function(context, state, w)
              if w.entered then return end
              w.entered = true
              M.set_door_object(context, "o_cabal_drop_pod_military_hallway", true, "at pt_hangar_spawn")
              -- The pod squad is the scene's participant, sq_military_hallway_destruction (3
              -- Legionaries): with sq_hangar_a_b instead the scene finished at once and the pod
              -- stayed shut. Package default counts only produce one member, hence full counts.
          end,
          on_exit = function(context, state, w)
              if w.exited then return end
              w.exited = true
              -- The Centurion is placed on the way out of this volume rather than at
              -- pt_hangar_early: in the shipped mission it is already on its ledge when the
              -- player arrives, and placing it early let the cost loop walk it across the
              -- overlooks long before that.
              M.spawn_squad(context, "sq_hangar_overlook_a_a_cent", HANGAR_AI)
              M.release_watch(context, w)
          end },
        -- Activated without its key the scene finishes at once (empty) and a later key does not
        -- restart it: bind -> activate -> key must go together here. The ~2.7s to scene_finished
        -- is the pod's own opening animation.
        -- Opened on leaving pt_hangar_spawn_backup (volume 227, y 77->83), one volume before
        -- pt_hangar_spawn_pod (228, y 96+), so the ~2.7s opening lands as the player reaches the pod.
        -- sq_military_hallway_destruction is the scene's participant (its descriptor binds that
        -- one-member squad to four participant roles): without it the pod stays shut, and the
        -- member COUNT is what fills the pod -- count 3 in default mode gave the shipped three
        -- Legionaries (replace mode kept the pod shut). sq_hangar_a_b / sq_hangar_a_a never
        -- spawn from here at all (likely a deeper hangar region not yet streamed).
        { id = "pt_hangar_spawn_backup", raw = { registry_key = 0xAA9D42BE, slot_type = 60, slot_index = 227 },
          on_exit = function(context)
              M.spawn_squad_full(context, "sq_military_hallway_destruction", 3, HANGAR_AI)
              M.start_scene(context, "sc_military_hallway_destruction")
          end },
        -- Past the pod: the first door opens and sq_hangar_overlook_b_b comes through it.
        { id = "pt_hangar_spawn_pod", raw = { registry_key = 0xAA9D42BE, slot_type = 60, slot_index = 228 },
          on_exit = function(context)
              M.set_device_position(context, "d_gating_amanda_start", 1.0, false, "leaving pt_hangar_spawn_pod")
              M.spawn_squad(context, "sq_hangar_overlook_b_b", HANGAR_AI)
          end },
        -- In front of the second door: the Cabal command ship (cabal_destroyer, absent by
        -- default; addressed by full id, the bazaar has a cabal_destroyer_start too) and its
        -- whole escort are put in place in one go (the fleet popping in later was visible), then
        -- the door opens. Leaving the volume: Cue 34.
        { id = "pt_amanda_skip", raw = { registry_key = 0xAA9D42BE, slot_type = 60, slot_index = 219 }, once = false,
          on_enter = function(context, state, w)
              if w.entered then return end
              w.entered = true
              M.set_door_object(context, "slot/80b5036a/000000/0000/0004", true, "cabal_destroyer at pt_amanda_skip")
              for _, name in ipairs({ "dogfight_bank_right_init", "dogfight_bank_left_a", "dogfight_bank_left_b",
                                      "dogfight_bank_left_c", "dogfight_ships_displacement_a",
                                      "dogfight_ships_displacement_b", "dogfight_ships_displacement_c",
                                      "dogfight_bank_right_a", "dogfight_bank_right_b", "dogfight_bank_right_c" }) do
                  M.set_door_object(context, name, true, "fleet at pt_amanda_skip")
              end
              -- The command ship's escort: the two carriers (hangar copies, by full id) and
              -- their movement devices.
              M.set_door_object(context, "slot/80b5036a/00004e/004e/0004", true, "o_cabal_carrier_r")
              M.set_door_object(context, "slot/80b5036a/00004f/004f/0004", true, "o_cabal_carrier_l")
              M.set_device_position(context, "slot/80b5036a/000050/0050/0017", 1.0, false, "d_cabal_carrier_r")
              M.set_device_position(context, "slot/80b5036a/000051/0051/0017", 1.0, false, "d_cabal_carrier_l")
              M.set_device_position(context, "d_gating_amanda_hangar", 1.0, false, "at pt_amanda_skip")
              -- The six Cabal missiles that strike the hangar (o_cabal_missile_1..6, hangar copies
              -- slots 0x30-0x35): each is a type-4 object that flies in and hits on instantiation,
              -- so they are staggered 250ms apart instead of landing together.
              for i = 1, 6 do context:start_timer("hangar_missile_" .. i, 250 * i) end
          end,
          on_exit = function(context, state, w)
              if w.exited then return end
              w.exited = true
              M.release_watch(context, w)
          end },
        -- Past the second door, into the hangar proper: Cue 34 and the combat music (section 8,
        -- found by ear) on the unnamed volume of the hangar-window registry (0xD3847A1F slot 5,
        -- x 93-122, y 109-134), the one between pt_dialogue_hangar_window and pt_goto_plaza.
        { id = "hangar_window_5", raw = { registry_key = 0xD3847A1F, slot_type = 60, slot_index = 5 },
          on_enter = function(context)
              M.play_cue(context, 34)
              M.set_music(context, 8)
          end },
        -- Explosion A with the three fodder Legionaries (standard Legionaries -- same 14 actor
        -- states and a spawner config byte-identical to sq_hangar_a_a_flank's; "fodder" names
        -- their role and the fa_fodder firing area of obj_hangar). Their jetpack arrival in the
        -- shipped mission is not in the package (no spawn point behind the ship, no path, no
        -- scene spawning them); with the objective they hop to their area (task groups 12/13).
        -- Leaving the volume drops the sq_hangar_a_b pod 3s later.
        { id = "pt_escape_explosion_a", raw = { registry_key = 0xAA9D42BE, slot_type = 60, slot_index = 134 }, once = false,
          on_enter = function(context, state, w)
              if w.entered then return end
              w.entered = true
              M.start_scene(context, "hangar_explosion_a")
              for _, name in ipairs({ "sq_hangar_fodder_a", "sq_hangar_fodder_b", "sq_hangar_fodder_c" }) do
                  M.spawn_squad(context, name, HANGAR_AI)
              end
          end,
          on_exit = function(context, state, w)
              if w.exited then return end
              w.exited = true
              -- The sq_hangar_a_b pod crashes down 3s after the player leaves explosion A
              -- (moved here from the pt_mount_ship exit).
              context:start_timer("hangar_a_b_pod", 3000)
              M.release_watch(context, w)
          end },
        -- pt_nux_crouch (volume 223, x 55-83 / y 15-33): Cue 37 on the way in.
        { id = "pt_nux_crouch", raw = { registry_key = 0xAA9D42BE, slot_type = 60, slot_index = 223 },
          on_enter = function(context) M.play_cue(context, 37) end },
        -- pt_holliday: the stairwell between pt_hangar_combat and explosion A (x 111-164,
        -- y 94.7-100.7; pt_hangar_fodder_backup_spawn covers the same spot). The explosion-B
        -- squads other than the pod spawn 4s after entering it, ahead of the player.
        { id = "pt_holliday", raw = { registry_key = 0xAA9D42BE, slot_type = 60, slot_index = 234 },
          on_enter = function(context) context:start_timer("hangar_explosion_b_squads", 4000) end },
        { id = "pt_escape_explosion_b", raw = { registry_key = 0xAA9D42BE, slot_type = 60, slot_index = 135 },
          on_enter = function(context) M.start_scene(context, "hangar_explosion_b") end },
    }

    B.timers = {
        hangar_missile_1 = function(context) M.set_door_object(context, "slot/80b5036a/000030/0030/0004", true, "o_cabal_missile_1") end,
        hangar_missile_2 = function(context) M.set_door_object(context, "slot/80b5036a/000031/0031/0004", true, "o_cabal_missile_2") end,
        hangar_missile_3 = function(context) M.set_door_object(context, "slot/80b5036a/000032/0032/0004", true, "o_cabal_missile_3") end,
        hangar_missile_4 = function(context) M.set_door_object(context, "slot/80b5036a/000033/0033/0004", true, "o_cabal_missile_4") end,
        hangar_missile_5 = function(context) M.set_door_object(context, "slot/80b5036a/000034/0034/0004", true, "o_cabal_missile_5") end,
        hangar_missile_6 = function(context) M.set_door_object(context, "slot/80b5036a/000035/0035/0004", true, "o_cabal_missile_6") end,
        -- sq_hangar_a_b: its own spawn rule sr_caball_hangar_a_b is a pod that crashes down with
        -- three Legionaries (it never showed when placed from the hallway, far from its anchor
        -- at x 112, y 29).
        hangar_explosion_b_squads = function(context)
            for _, name in ipairs({ "sq_hangar_a_a", "sq_hangar_a_b_sniper" }) do M.spawn_squad(context, name, HANGAR_AI) end
        end,
        hangar_a_b_pod = function(context) M.spawn_squad_full(context, "sq_hangar_a_b", 1, HANGAR_AI) end,
    }

    return B
end
