#include <Windows.h>

#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>

#include "../../../state/activity/membership/definition.h"
#include "mission_script_lua_internal.h"
#include "mission_script_lua_names.h"
#include "mission_script_lua_peer_internal.h"
#include "mission_script_lua_resolve.h"
#include "mission_script_lua_types.h"
#include "mission_script_vm_internal.h"

namespace sunrise::server::activity::mission::lua_vm::detail {
namespace {

[[nodiscard]] int context_squad(lua_State* state) {
    static_cast<void>(luaL_checkudata(state, 1, kContextMetatable));
    SquadDefinition definition{};
    if (!resolve_squad(state, 2, definition)) {
        return luaL_error(state, "unknown or ambiguous activity squad");
    }
    push_handle(state, kSquadMetatable, SquadHandle{definition.localRow});
    return 1;
}

/**
 * Arms a native hard wipe at an authored spawn set, or releases one with its request key.
 * The Lua caller passes `release_request` as the decimal string of the original key.
 */
[[nodiscard]] int context_restart_checkpoint(lua_State* state) {
    static_cast<void>(luaL_checkudata(state, 1, kContextMetatable));
    static constexpr std::array<std::string_view, 3> kDeclared{
        "region", "spawn_set_hash", "release_request"};
    refuse_unknown_arguments(state, kDeclared);
    const lua_Integer region = optional_integer_argument(state, "region", -1);
    const lua_Integer hash = optional_integer_argument(state, "spawn_set_hash", 0);
    if (region < 0 || region > ::sunrise::state::activity::membership::kMaximumSliceSetIndex
        || hash <= 0 || hash >= (std::numeric_limits<std::uint32_t>::max)()) {
        return luaL_error(state, "checkpoint requires an authored region and spawn-set hash");
    }
    std::uint64_t release = 0;
    lua_getfield(state, 2, "release_request");
    if (!lua_isnil(state, -1)) {
        std::size_t length = 0;
        const char* const value = luaL_checklstring(state, -1, &length);
        const auto parsed = std::from_chars(value, value + length, release);
        if (parsed.ec != std::errc{} || parsed.ptr != value + length || release == 0) {
            return luaL_error(state, "checkpoint release requires the original RequestKey.value");
        }
    }
    lua_pop(state, 1);
    Intent intent{};
    intent.kind = IntentKind::restartCheckpoint;
    intent.checkpointReleaseRequest = release;
    intent.effectiveRegion = static_cast<std::int32_t>(region);
    intent.checkpointSpawnHash = static_cast<std::uint32_t>(hash);
    return queue_intent(state, active_frame(state), intent);
}

[[nodiscard]] int context_scene(lua_State* state) {
    static_cast<void>(luaL_checkudata(state, 1, kContextMetatable));
    SceneDefinition definition{};
    if (!resolve_scene(state, 2, definition)) {
        return luaL_error(state, "unknown or ambiguous authored scene");
    }
    push_handle(state, kSceneMetatable, SceneHandle{definition.localRow});
    return 1;
}

[[nodiscard]] int context_slot(lua_State* state) {
    static_cast<void>(luaL_checkudata(state, 1, kContextMetatable));
    SlotDefinition definition{};
    if (!resolve_slot(state, 2, definition)) {
        return luaL_error(state, "unknown or ambiguous activity slot");
    }
    push_handle(state, kSlotMetatable, SlotHandle{definition.localRow});
    return 1;
}

} // namespace

/**
 * True until the client has physically streamed into the region this mission selected (via
 * initial_state or select_state). A script that fires an effect immediately in on_start risks
 * the client not having arrived yet; polling this on a short timer is the robust alternative to
 * guessing a fixed delay.
 */
[[nodiscard]] int context_region_arrival_pending(lua_State* state) {
    static_cast<void>(luaL_checkudata(state, 1, kContextMetatable));
    Impl* const impl = impl_from_state(state);
    const bool pending = impl == nullptr || impl->definitions.regionArrivalPending == nullptr
                          || impl->definitions.regionArrivalPending(impl->definitions.context);
    lua_pushboolean(state, pending ? 1 : 0);
    return 1;
}

/**
 * Exploratory: true once the client-side physics hook has a live local-player body. Not reset on
 * activity attach, so a single true reading does not prove a fresh spawn; a script investigating
 * this as a spawn signal should watch for its own transition instead.
 */
[[nodiscard]] int context_player_position_present(lua_State* state) {
    static_cast<void>(luaL_checkudata(state, 1, kContextMetatable));
    Impl* const impl = impl_from_state(state);
    const bool present = impl != nullptr && impl->definitions.playerPositionPresent != nullptr
                         && impl->definitions.playerPositionPresent(impl->definitions.context);
    lua_pushboolean(state, present ? 1 : 0);
    return 1;
}

/** `context:player_position()` -> `{x=, y=, z=}` of the local player, or nil when unseen. */
[[nodiscard]] int context_player_position(lua_State* state) {
    static_cast<void>(luaL_checkudata(state, 1, kContextMetatable));
    Impl* const impl = impl_from_state(state);
    float x = 0.0F;
    float y = 0.0F;
    float z = 0.0F;
    if (impl == nullptr || impl->definitions.playerPosition == nullptr
        || !impl->definitions.playerPosition(impl->definitions.context, x, y, z)) {
        lua_pushnil(state);
        return 1;
    }
    lua_createtable(state, 0, 3);
    lua_pushnumber(state, x);
    lua_setfield(state, -2, "x");
    lua_pushnumber(state, y);
    lua_setfield(state, -2, "y");
    lua_pushnumber(state, z);
    lua_setfield(state, -2, "z");
    return 1;
}

/**
 * Exploratory: the client's raw boot-flow step. Only one value is named so far
 * (`activity:in_world` = 38); this is for observing the real spawn sequence, not a stable API.
 */
[[nodiscard]] int context_bootflow_step(lua_State* state) {
    static_cast<void>(luaL_checkudata(state, 1, kContextMetatable));
    Impl* const impl = impl_from_state(state);
    const std::int32_t step = impl == nullptr || impl->definitions.bootflowStep == nullptr
                                  ? -1
                                  : impl->definitions.bootflowStep(impl->definitions.context);
    lua_pushinteger(state, step);
    return 1;
}

/**
 * Diagnostic tool: resolves the type-31 source of the Nth trigger row the debug "Scriptable
 * Browser" trigger-volume panel would list for one bubble index (its bubble filter value, not a
 * slice-set/region index), assuming no text/scope filter is active there.
 * @return ok (boolean); when true, registry_key, slot_type, slot_index (nil otherwise);
 * match_count; total_rows (bubble-matching row count, so total_rows < visible_index means that
 * row does not exist at all); table_registry_key/table_slot_type/table_slot_index -- the row's
 * own type-60 identity, always populated once the row is found even if match_count ~= 1, so it
 * can be compared against a volume already resolved by `watch_trigger()` elsewhere.
 */
[[nodiscard]] int context_find_trigger_by_bubble(lua_State* state) {
    static_cast<void>(luaL_checkudata(state, 1, kContextMetatable));
    const lua_Integer bubbleIndex = luaL_checkinteger(state, 2);
    const lua_Integer visibleIndex = luaL_checkinteger(state, 3);
    if (visibleIndex <= 0 || visibleIndex > (std::numeric_limits<std::uint32_t>::max)()) {
        return luaL_error(state, "visible_index must be a positive integer");
    }
    Impl* const impl = impl_from_state(state);
    std::uint32_t registryKey = 0;
    std::uint32_t slotType = 0;
    std::uint32_t slotIndex = 0;
    std::uint32_t matchCount = 0;
    std::uint32_t totalRows = 0;
    std::uint32_t tableRegistryKey = 0;
    std::uint32_t tableSlotType = 0;
    std::uint32_t tableSlotIndex = 0;
    const bool ok = impl != nullptr && impl->definitions.findTriggerByBubbleVisibleIndex != nullptr
                   && impl->definitions.findTriggerByBubbleVisibleIndex(
                       impl->definitions.context,
                       static_cast<std::int32_t>(bubbleIndex),
                       static_cast<std::uint32_t>(visibleIndex),
                       registryKey,
                       slotType,
                       slotIndex,
                       matchCount,
                       totalRows,
                       tableRegistryKey,
                       tableSlotType,
                       tableSlotIndex);
    lua_pushboolean(state, ok ? 1 : 0);
    if (ok) {
        lua_pushinteger(state, static_cast<lua_Integer>(registryKey));
        lua_pushinteger(state, static_cast<lua_Integer>(slotType));
        lua_pushinteger(state, static_cast<lua_Integer>(slotIndex));
    } else {
        lua_pushnil(state);
        lua_pushnil(state);
        lua_pushnil(state);
    }
    lua_pushinteger(state, static_cast<lua_Integer>(matchCount));
    lua_pushinteger(state, static_cast<lua_Integer>(totalRows));
    lua_pushinteger(state, static_cast<lua_Integer>(tableRegistryKey));
    lua_pushinteger(state, static_cast<lua_Integer>(tableSlotType));
    lua_pushinteger(state, static_cast<lua_Integer>(tableSlotIndex));
    return 9;
}

/**
 * Diagnostic tool: reads up to 256 bytes of the game's own content-hash resolver's definition
 * blob for `hash` directly out of process memory. Meant for scanning an authored scene's own
 * resourceTag for embedded reference hashes (event-gate node keys) the extracted SDK catalog does
 * not expose -- not a stable API.
 * @return ok (boolean); when true, the raw bytes as a Lua string (may contain embedded zero
 * bytes -- use string.byte, not string patterns that assume a C string).
 */
[[nodiscard]] int context_resolve_hash(lua_State* state) {
    static_cast<void>(luaL_checkudata(state, 1, kContextMetatable));
    const lua_Integer hash = luaL_checkinteger(state, 2);
    if (hash < 0 || hash > (std::numeric_limits<std::uint32_t>::max)()) {
        return luaL_error(state, "hash must fit in an unsigned 32-bit integer");
    }
    Impl* const impl = impl_from_state(state);
    static constexpr std::uint32_t kCapacity = 256;
    std::array<std::uint8_t, kCapacity> bytes{};
    std::uint32_t length = 0;
    const bool ok = impl != nullptr && impl->definitions.resolveContentHash != nullptr
                   && impl->definitions.resolveContentHash(impl->definitions.context,
                                                            static_cast<std::uint32_t>(hash),
                                                            bytes.data(),
                                                            kCapacity,
                                                            length);
    lua_pushboolean(state, ok ? 1 : 0);
    if (!ok) {
        lua_pushnil(state);
        return 2;
    }
    lua_pushlstring(state, reinterpret_cast<const char*>(bytes.data()), length);
    return 2;
}

/**
 * Development diagnostic: `context:dump_hash{hash = ..., depth = ...}` writes the definition blob
 * for `hash`, and every content tag it carries down to `depth` levels, beside the running game,
 * logging each blob's size and how many authored event-gate nodes it names. Locating an authored
 * scene's graph needs the whole blob, which is far larger than `resolve_hash` returns.
 * @return The number of blobs written.
 */
[[nodiscard]] int context_dump_hash(lua_State* state) {
    static_cast<void>(luaL_checkudata(state, 1, kContextMetatable));
    static constexpr std::array<std::string_view, 2> kDeclared{"hash", "depth"};
    refuse_unknown_arguments(state, kDeclared);
    const lua_Integer hash = checked_integer_argument(state, "hash");
    const lua_Integer depth = optional_integer_argument(state, "depth", 1);
    if (hash <= 0 || hash > (std::numeric_limits<std::uint32_t>::max)() || depth < 0 || depth > 4) {
        return luaL_error(state, "dump_hash needs an unsigned 32-bit hash and a depth of 0..4");
    }
    Impl* const impl = impl_from_state(state);
    const std::uint32_t written =
        (impl == nullptr || impl->definitions.dumpContentHash == nullptr)
            ? 0U
            : impl->definitions.dumpContentHash(impl->definitions.context,
                                                static_cast<std::uint32_t>(hash),
                                                static_cast<std::uint32_t>(depth));
    lua_pushinteger(state, static_cast<lua_Integer>(written));
    return 1;
}

/**
 * Development diagnostic: `context:find_event_gate_keys{hash = ...}` scans an authored scene's
 * graph blob for every `kEventGateNodeClass` marker and returns the FNV-1 event key sitting 12
 * bytes after each one (the graph header's own declared array offset/count for that class is
 * empty even when it declares N such nodes -- the keys live inline in the graph body instead).
 * @return An array (1-based) of the keys found, in the order their nodes appear in the graph.
 */
[[nodiscard]] int context_find_event_gate_keys(lua_State* state) {
    static_cast<void>(luaL_checkudata(state, 1, kContextMetatable));
    static constexpr std::array<std::string_view, 1> kDeclared{"hash"};
    refuse_unknown_arguments(state, kDeclared);
    const lua_Integer hash = checked_integer_argument(state, "hash");
    if (hash <= 0 || hash > (std::numeric_limits<std::uint32_t>::max)()) {
        return luaL_error(state, "find_event_gate_keys needs an unsigned 32-bit hash");
    }
    Impl* const impl = impl_from_state(state);
    static constexpr std::uint32_t kCapacity = 32;
    std::array<std::uint32_t, kCapacity> keys{};
    const std::uint32_t found =
        (impl == nullptr || impl->definitions.findEventGateKeys == nullptr)
            ? 0U
            : impl->definitions.findEventGateKeys(
                impl->definitions.context, static_cast<std::uint32_t>(hash), keys.data(), kCapacity);
    lua_newtable(state);
    for (std::uint32_t index = 0; index < found && index < kCapacity; ++index) {
        lua_pushinteger(state, static_cast<lua_Integer>(keys[index]));
        lua_rawseti(state, -2, static_cast<lua_Integer>(index + 1));
    }
    return 1;
}

/**
 * `context:clock_ms()` -> milliseconds since system boot (GetTickCount64). A monotonic source
 * for per-object generations that must keep increasing across mission restarts in one game
 * process, which durable mission variables do not survive.
 */
[[nodiscard]] int context_clock_ms(lua_State* state) {
    static_cast<void>(luaL_checkudata(state, 1, kContextMetatable));
    lua_pushinteger(state, static_cast<lua_Integer>(GetTickCount64()));
    return 1;
}

/**
 * `context:squad_state_names{squad = <row>}` lists the distinct actor states the squad's member
 * classes declare, as `{group =, name =, ordinal =}` rows -- candidates for
 * `slot:play_actor_action{group =, action =}` on that squad's type-2 cell.
 */
[[nodiscard]] int context_squad_state_names(lua_State* state) {
    static_cast<void>(luaL_checkudata(state, 1, kContextMetatable));
    static constexpr std::array<std::string_view, 1> kDeclared{"squad"};
    refuse_unknown_arguments(state, kDeclared);
    const lua_Integer row = checked_integer_argument(state, "squad");
    if (row <= 0 || row > (std::numeric_limits<std::uint32_t>::max)()) {
        return luaL_error(state, "squad_state_names needs a positive squad row");
    }
    Impl* const impl = impl_from_state(state);
    static constexpr std::uint32_t kCapacity = 256;
    std::array<ActorStateNameDefinition, kCapacity> names{};
    const std::uint32_t found =
        (impl == nullptr || impl->definitions.squadStateNames == nullptr)
            ? 0U
            : impl->definitions.squadStateNames(
                impl->definitions.context, static_cast<std::uint32_t>(row), names.data(), kCapacity);
    lua_newtable(state);
    for (std::uint32_t index = 0; index < found && index < kCapacity; ++index) {
        lua_createtable(state, 0, 3);
        lua_pushinteger(state, static_cast<lua_Integer>(names[index].groupHash));
        lua_setfield(state, -2, "group");
        lua_pushinteger(state, static_cast<lua_Integer>(names[index].nameHash));
        lua_setfield(state, -2, "name");
        lua_pushinteger(state, static_cast<lua_Integer>(names[index].ordinal));
        lua_setfield(state, -2, "ordinal");
        lua_rawseti(state, -2, static_cast<lua_Integer>(index + 1));
    }
    return 1;
}

/**
 * Arms client-side geometric detection for one type-31 slot named by its raw wire identity
 * (registry_key, slot_type, slot_index) rather than a resolved Lua slot handle -- for a trigger
 * `context:slot(...)` cannot name, such as an unnamed one found via `find_trigger_by_bubble`.
 * @return armed (boolean), and when armed the resolved type-60 volume's registry_key, slot_type
 * and slot_index, exactly like `slot:watch_trigger()`.
 */
/**
 * `context:clear_trigger_watches()` drops every watch of this session binding. The client table
 * holds 64 entries and survives a mission restart in the same process, so a script that arms
 * watches it may never fire (beats skipped by starting later in the mission) must clear on start.
 */
[[nodiscard]] int context_clear_trigger_watches(lua_State* state) {
    static_cast<void>(luaL_checkudata(state, 1, kContextMetatable));
    Impl* const impl = impl_from_state(state);
    if (impl != nullptr && impl->definitions.clearTriggerWatches != nullptr) {
        impl->definitions.clearTriggerWatches(impl->definitions.context);
    }
    return 0;
}

/** `context:unwatch_trigger_identity(registry_key, slot_type, slot_index)` -> released (boolean). */
[[nodiscard]] int context_unwatch_trigger_identity(lua_State* state) {
    static_cast<void>(luaL_checkudata(state, 1, kContextMetatable));
    const lua_Integer registryKey = luaL_checkinteger(state, 2);
    const lua_Integer slotType = luaL_checkinteger(state, 3);
    const lua_Integer slotIndex = luaL_checkinteger(state, 4);
    Impl* const impl = impl_from_state(state);
    const bool released = impl != nullptr && impl->definitions.unregisterTriggerWatch != nullptr
                          && impl->definitions.unregisterTriggerWatch(
                              impl->definitions.context,
                              static_cast<std::uint32_t>(registryKey),
                              static_cast<std::uint32_t>(slotType),
                              static_cast<std::uint32_t>(slotIndex));
    lua_pushboolean(state, released ? 1 : 0);
    return 1;
}

[[nodiscard]] int context_watch_trigger_identity(lua_State* state) {
    static_cast<void>(luaL_checkudata(state, 1, kContextMetatable));
    const lua_Integer registryKey = luaL_checkinteger(state, 2);
    const lua_Integer slotType = luaL_checkinteger(state, 3);
    const lua_Integer slotIndex = luaL_checkinteger(state, 4);
    Impl* const impl = impl_from_state(state);
    std::uint32_t volumeRegistryKey = 0;
    std::uint32_t volumeSlotType = 0;
    std::uint32_t volumeSlotIndex = 0;
    const bool armed = impl != nullptr && impl->definitions.registerTriggerWatch != nullptr
                       && impl->definitions.registerTriggerWatch(
                           impl->definitions.context,
                           static_cast<std::uint32_t>(registryKey),
                           static_cast<std::uint32_t>(slotType),
                           static_cast<std::uint32_t>(slotIndex),
                           volumeRegistryKey,
                           volumeSlotType,
                           volumeSlotIndex);
    lua_pushboolean(state, armed ? 1 : 0);
    if (!armed) {
        return 1;
    }
    lua_pushinteger(state, static_cast<lua_Integer>(volumeRegistryKey));
    lua_pushinteger(state, static_cast<lua_Integer>(volumeSlotType));
    lua_pushinteger(state, static_cast<lua_Integer>(volumeSlotIndex));
    return 4;
}

/** Resolves the squad one Lua argument names, by handle or by index. */
[[nodiscard]] bool resolve_squad(lua_State* state, int selector, SquadDefinition& output) {
    Impl* const impl = impl_from_state(state);
    if (impl == nullptr) {
        return false;
    }
    if (lua_isinteger(state, selector)) {
        const lua_Integer row = lua_tointeger(state, selector);
        return row > 0
               && static_cast<std::uint64_t>(row) <= (std::numeric_limits<std::uint32_t>::max)()
               && impl->definitions.resolveSquadRow != nullptr
               && impl->definitions.resolveSquadRow(
                   impl->definitions.context, static_cast<std::uint32_t>(row), output);
    }
    return impl->definitions.resolveSquadId != nullptr
           && impl->definitions.resolveSquadId(
               impl->definitions.context, lua_string_view(state, selector), output);
}

/** Resolves the scene one Lua argument names, by handle or by index. */
[[nodiscard]] bool resolve_scene(lua_State* state, int selector, SceneDefinition& output) {
    Impl* const impl = impl_from_state(state);
    if (impl == nullptr) {
        return false;
    }
    if (lua_isinteger(state, selector)) {
        const lua_Integer row = lua_tointeger(state, selector);
        return row > 0
               && static_cast<std::uint64_t>(row) <= (std::numeric_limits<std::uint32_t>::max)()
               && impl->definitions.resolveSceneRow != nullptr
               && impl->definitions.resolveSceneRow(
                   impl->definitions.context, static_cast<std::uint32_t>(row), output);
    }
    return impl->definitions.resolveSceneId != nullptr
           && impl->definitions.resolveSceneId(
               impl->definitions.context, lua_string_view(state, selector), output);
}

/** Resolves the slot one Lua argument names, by handle or by index. */
[[nodiscard]] bool resolve_slot(lua_State* state, int selector, SlotDefinition& output) {
    Impl* const impl = impl_from_state(state);
    if (impl == nullptr) {
        return false;
    }
    if (lua_isinteger(state, selector)) {
        const lua_Integer row = lua_tointeger(state, selector);
        return row > 0
               && static_cast<std::uint64_t>(row) <= (std::numeric_limits<std::uint32_t>::max)()
               && impl->definitions.resolveSlotRow != nullptr
               && impl->definitions.resolveSlotRow(
                   impl->definitions.context, static_cast<std::uint32_t>(row), output);
    }
    return impl->definitions.resolveSlotId != nullptr
           && impl->definitions.resolveSlotId(
               impl->definitions.context, lua_string_view(state, selector), output);
}

[[nodiscard]] bool
resolve_message_name(lua_State* state, std::string_view name, ActivityMessageDefinition& output) {
    Impl* const impl = impl_from_state(state);
    return impl != nullptr && impl->definitions.resolveActivityMessageName != nullptr
           && impl->definitions.resolveActivityMessageName(impl->definitions.context, name, output);
}

/**
 * Reads the optional omit list: generated slots whose owning object stays out of the seed.
 * @return False with the Lua error already raised.
 */
[[nodiscard]] bool parse_seed_omissions(lua_State* state, int index, Intent& intent) {
    if (lua_isnoneornil(state, index)) {
        return true;
    }
    luaL_checktype(state, index, LUA_TTABLE);
    const lua_Integer count = static_cast<lua_Integer>(lua_rawlen(state, index));
    if (count < 0
        || static_cast<std::size_t>(count)
               > ::sunrise::state::activity::mission::kMissionSeedOmitCapacity) {
        static_cast<void>(luaL_argerror(state, index, "mission seed omit list is too long"));
        return false;
    }
    for (lua_Integer entry = 1; entry <= count; ++entry) {
        lua_rawgeti(state, index, entry);
        SlotDefinition definition{};
        const bool resolved = resolve_slot(state, lua_gettop(state), definition);
        lua_pop(state, 1);
        if (!resolved) {
            static_cast<void>(luaL_argerror(state, index, "unknown or ambiguous activity slot"));
            return false;
        }
        intent.seedOmissions[static_cast<std::size_t>(entry - 1)] = {definition.objectTag,
                                                                     definition.registryKey};
    }
    intent.seedOmissionCount = static_cast<std::uint8_t>(count);
    return true;
}

/** Queues one generated mission state by its authored effective region. */
[[nodiscard]] int context_select_state(lua_State* state) {
    static_cast<void>(luaL_checkudata(state, 1, kContextMetatable));
    luaL_checktype(state, 2, LUA_TTABLE);
    lua_getfield(state, 2, "region_index");
    if (!lua_isinteger(state, -1)) {
        return luaL_argerror(state, 2, "generated mission state has no integer region_index");
    }
    const lua_Integer region = lua_tointeger(state, -1);
    lua_pop(state, 1);
    if (region < 0
        || static_cast<std::uint64_t>(region)
               > static_cast<std::uint64_t>((std::numeric_limits<std::int32_t>::max)())) {
        return luaL_argerror(state, 2, "generated mission state region_index is outside i32");
    }
    CallFrame& frame = active_frame(state);
    Intent intent{};
    intent.kind = IntentKind::selectMissionState;
    intent.effectiveRegion = static_cast<std::int32_t>(region);
    if (!parse_seed_omissions(state, 3, intent)) {
        return 0;
    }
    if (lua_istable(state, 3)) {
        lua_getfield(state, 3, "retire_placed_props");
        if (!lua_isnil(state, -1) && !lua_isboolean(state, -1)) {
            return luaL_argerror(state, 3, "retire_placed_props must be a boolean");
        }
        intent.retirePlacedProps = lua_toboolean(state, -1) != 0;
        lua_pop(state, 1);
    }
    return queue_intent(state, frame, intent);
}

/** Lua index for the mission context: its collections, phase, variables and timers. */
[[nodiscard]] int context_index(lua_State* state) {
    static_cast<void>(luaL_checkudata(state, 1, kContextMetatable));
    Impl* const impl = impl_from_state(state);
    const std::string_view key = lua_string_view(state, 2);
    if (key == "sdk_build_id") {
        lua_pushstring(state, impl->identity.sdkBuildId.data());
    } else if (key == "activity_id") {
        lua_pushstring(state, impl->identity.activityId.data());
    } else if (key == "activity_row") {
        lua_pushinteger(state, impl->identity.activityRow);
    } else if (key == "definition_hash") {
        lua_pushinteger(state, impl->identity.definitionHash);
    } else if (key == "activity_role") {
        lua_pushstring(state, impl->identity.publicTarget ? "public" : "private");
    } else if (key == "player_key") {
        push_u64_string(state, impl->identity.playerKey);
    } else if (key == "sdk") {
        push_activity(state);
    } else if (key == "lifetime") {
        push_lifetime(state);
    } else if (key == "peers") {
        push_peers(state);
    } else if (key == "squad") {
        lua_pushcfunction(state, &context_squad);
    } else if (key == "scene") {
        lua_pushcfunction(state, &context_scene);
    } else if (key == "slot") {
        lua_pushcfunction(state, &context_slot);
    } else if (key == "select_state") {
        lua_pushcfunction(state, &context_select_state);
    } else if (key == "restart_checkpoint") {
        lua_pushcfunction(state, &context_restart_checkpoint);
    } else if (key == "set_phase") {
        lua_pushcfunction(state, &context_set_phase);
    } else if (key == "set_variable") {
        lua_pushcfunction(state, &context_set_variable);
    } else if (key == "clear_variable") {
        lua_pushcfunction(state, &context_clear_variable);
    } else if (key == "start_timer") {
        lua_pushcfunction(state, &context_start_timer);
    } else if (key == "cancel_timer") {
        lua_pushcfunction(state, &context_cancel_timer);
    } else if (key == "probe") {
        lua_pushcfunction(state, &context_probe);
    } else if (key == "poll_command") {
        lua_pushcfunction(state, &context_poll_command);
    } else if (key == "read_artifact_text") {
        lua_pushcfunction(state, &context_read_artifact_text);
    } else if (key == "region_arrival_pending") {
        lua_pushcfunction(state, &context_region_arrival_pending);
    } else if (key == "player_position_present") {
        lua_pushcfunction(state, &context_player_position_present);
    } else if (key == "player_position") {
        lua_pushcfunction(state, &context_player_position);
    } else if (key == "bootflow_step") {
        lua_pushcfunction(state, &context_bootflow_step);
    } else if (key == "find_trigger_by_bubble") {
        lua_pushcfunction(state, &context_find_trigger_by_bubble);
    } else if (key == "watch_trigger_identity") {
        lua_pushcfunction(state, &context_watch_trigger_identity);
    } else if (key == "unwatch_trigger_identity") {
        lua_pushcfunction(state, &context_unwatch_trigger_identity);
    } else if (key == "clear_trigger_watches") {
        lua_pushcfunction(state, &context_clear_trigger_watches);
    } else if (key == "resolve_hash") {
        lua_pushcfunction(state, &context_resolve_hash);
    } else if (key == "dump_hash") {
        lua_pushcfunction(state, &context_dump_hash);
    } else if (key == "find_event_gate_keys") {
        lua_pushcfunction(state, &context_find_event_gate_keys);
    } else if (key == "squad_state_names") {
        lua_pushcfunction(state, &context_squad_state_names);
    } else if (key == "clock_ms") {
        lua_pushcfunction(state, &context_clock_ms);
    } else if (!push_key_context_member(state, key)) {
        lua_pushnil(state);
    }
    return 1;
}

void register_context_metatables(lua_State* state) {
    register_metatable(state, kContextMetatable, &context_index);
}

} // namespace sunrise::server::activity::mission::lua_vm::detail
