-- Plaza (the way out onto the Tower plaza: the city, the Traveler and the Cabal fleet above it).
-- First pass: the sky battle is authored as plain objects in the specops_plaza_ship_battle
-- registry 0x80B5103A, absent by default exactly like the hangar's command ship, so the script
-- has to instantiate them. Everything here is to be tuned live.
return function(M)
    -- Region 48 = bubble 6, from the SDK state table (STATE_80B500BC_0006_0000_80B500B6,
    -- slice_set_index 48 / region_index 48). Same value the community module uses. Only matters
    -- for START_REGION.
    local B = { region = 48 }

    local SHIP_BATTLE = "slot/80b5103a/%s/%s/0004"   -- type-4 objects of the sky battle
    local CARRIER_DEVICE = "slot/80b5103a/%s/%s/0017" -- their devices

    -- The fleet, in the order it reads in the slot table. The three near carriers sit over the
    -- city, the nine `_far_*` fill the horizon, the o_do_ship* are the dogfight fighters and
    -- o_cabal_ship is the big one. o_spire is the Traveler-side structure.
    local FLEET = {
        "000002", -- o_spire
        "000005", -- o_cabal_ship
        "000010", "000011", "000012",                     -- o_cabal_carrier_l / _r / _b
        "000016", "000017", "000018",                     -- o_cabal_carrier_far_a / _a_1 / _a_2
        "000019", "00001a", "00001b",                     -- o_cabal_carrier_far_b / _b_1 / _b_2
        "00001c", "00001d", "00001e",                     -- o_cabal_carrier_far_c / _c_1 / _c_2
        "000004", "000009", "00000a", "00000b", "00000c", -- o_do_ship[5] [0] [1] [2] [3]
        "00000d", "00000e", "00000f",                     -- o_do_ship[4] [6] [7]
        "000006", "000007", "000008",                     -- o_do_ship_left[0..2]
    }
    -- Carrier devices, driven to 1.0 like the hangar escort: without it they sit inert.
    local CARRIERS = {
        "000013", "000014", "000015",                     -- d_cabal_carrier_l / _r / _b
        "00001f", "000020", "000021",                     -- d_cabal_carrier_far_a / _a_1 / _a_2
        "000022", "000023", "000024",                     -- d_cabal_carrier_far_b / _b_1 / _b_2
        "000025", "000026", "000027",                     -- d_cabal_carrier_far_c / _c_1 / _c_2
    }
    -- d_spire is deliberately NOT in that list: driving it at spawn set the missile rain off
    -- immediately, and that beat comes later (see the spire watch below).

    local fleet_spawned = false

    local function on_plaza_spawn_init(context)
        if fleet_spawned then return end
        fleet_spawned = true
        for _, index in ipairs(FLEET) do
            M.set_door_object(context, string.format(SHIP_BATTLE, index, index:sub(3)),
                              true, "plaza fleet at pt_plaza_spawn_init")
        end
        for _, index in ipairs(CARRIERS) do
            M.set_device_position(context, string.format(CARRIER_DEVICE, index, index:sub(3)),
                                  1.0, true, "plaza carrier at pt_plaza_spawn_init")
        end
    end

    -- The spire's missile rain. o_spire is already in place and inert; this adds the attack
    -- object and drives the device. Wired to pt_spire_trigger, the innermost of the three
    -- nested plaza volumes -- pt_plaza_spire (same registry, volume 47) is the alternative if
    -- this fires too late.
    local spire_attacked = false
    local function on_spire_attack(context)
        if spire_attacked then return end
        spire_attacked = true
        M.set_door_object(context, string.format(SHIP_BATTLE, "000003", "0003"),
                          true, "o_spire_missile_attack at pt_spire_trigger")
        M.set_device_position(context, string.format(CARRIER_DEVICE, "000028", "0028"),
                              1.0, true, "d_spire at pt_spire_trigger")
    end

    -- Zavala and the plaza waves are data-driven now: Sunrise/sequences/mission_towerfall.json
    -- (edited in Activity Host > World > Sequencer, played by mission_towerfall/sequencer.lua).
    -- The hand-written version lives in mission_towerfall_backup/plaza.lua.pre_sequencer.

    B.watches = {
        -- pt_plaza_spawn_init: registry 0xF8F959CD, volume 38 (x 24->80, y -70->-16).
        { id = "pt_plaza_spawn_init", raw = { registry_key = 0xF8F959CD, slot_type = 60, slot_index = 38 },
          on_enter = function(context) on_plaza_spawn_init(context) end },
        -- pt_spire_trigger: registry 0xC0C5E876, volume 48 (x 41->68, y -64->-32).
        { id = "pt_spire_trigger", raw = { registry_key = 0xC0C5E876, slot_type = 60, slot_index = 48 },
          on_enter = function(context) on_spire_attack(context) end },
    }

    return B
end
