#include "mission_script_sdk_bridge.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <span>

#include "../../../client/activity/player_trigger_watch.h"
#include "../../../client/content/activity/scriptable_catalog_worker.h"
#include "../../../client/hooks/bootflow/bootflow_hook_lifecycle.h"
#include "../../../client/hooks/content_resolver/content_resolver.h"
#include "../../../client/player/player_position.h"
#include "../../../core/logging/log.h"
#include "../../../state/build_data/runtime.h"
#include "../../../state/build_data/scriptables/scriptable_catalog.h"
#include "../activity_sdk_mission_runtime.h"
#include "mission_script_catalog_sdk_bridge.h"
#include "mission_script_message_catalog.h"
#include "mission_script_player_trigger.h"

namespace sunrise::server::activity::mission::sdk_bridge {
namespace {

namespace sdk = state::activity_sdk;
namespace format = state::activity_sdk::format;
namespace scenes = activity_sdk_mission;
namespace scriptables = state::build_data::scriptables;
namespace trigger_watch = client::activity::player_trigger_watch;
// Named apart from `scriptables` above: this is the client-side worker that fills the
// state::build_data::scriptables catalog on request, not the catalog/data namespace itself.
namespace scriptable_extraction = client::content::activity::scriptables;

// Every occurrence text id starts with this family prefix.
constexpr std::string_view kOccurrencePrefix = "object-occurrence/";

[[nodiscard]] const sdk::BoundView* context_view(const void* context) noexcept {
    return static_cast<const sdk::BoundView*>(context);
}

[[nodiscard]] bool valid_view(const sdk::BoundView* view) noexcept {
    return view != nullptr && view->catalog != nullptr && sdk::bound_activity(*view) != nullptr
           && sdk::bound_scenario(*view) != nullptr;
}

[[nodiscard]] const format::Activity* bound_activity(const sdk::BoundView* view) noexcept {
    return view != nullptr && view->catalog != nullptr ? sdk::bound_activity(*view) : nullptr;
}

/** Copies only authenticated pack fields; no mapped pointer is exposed to Lua. */
[[nodiscard]] bool resolve_activity_binding(const void* context,
                                            lua_vm::ActivityBindingDefinition& output) noexcept {
    output = {};
    const sdk::BoundView* const view = context_view(context);
    const format::Activity* const activity = bound_activity(view);
    return activity != nullptr && activity_binding_definition(*view->catalog, *activity, output);
}

[[nodiscard]] std::size_t activity_binding_tag_count(const void* context,
                                                     lua_vm::ActivityBindingTagKind kind) noexcept {
    const sdk::BoundView* const view = context_view(context);
    const format::Activity* const activity = bound_activity(view);
    return activity != nullptr ? activity_binding_tags(*view->catalog, *activity, kind).size() : 0;
}

/** Resolves the package tag of the activity binding the view names. */
[[nodiscard]] bool resolve_activity_binding_tag(const void* context,
                                                lua_vm::ActivityBindingTagKind kind,
                                                std::uint32_t localRow,
                                                std::uint32_t& output) noexcept {
    output = 0;
    const sdk::BoundView* const view = context_view(context);
    if (view == nullptr || view->catalog == nullptr || localRow == 0) {
        return false;
    }
    const format::Activity* const activity = sdk::bound_activity(*view);
    if (activity == nullptr) {
        return false;
    }
    const auto rows = activity_binding_tags(*view->catalog, *activity, kind);
    if (localRow > rows.size()) {
        return false;
    }
    output = rows[localRow - 1U].tag;
    return output != 0;
}

[[nodiscard]] std::span<const format::ActivityBindingLocator>
binding_locators(const sdk::BoundView& view) noexcept {
    const format::Activity* const activity = sdk::bound_activity(view);
    return activity != nullptr ? sdk::activity_binding_locators(*view.catalog, *activity)
                               : std::span<const format::ActivityBindingLocator>{};
}

[[nodiscard]] std::size_t activity_binding_locator_count(const void* context) noexcept {
    const sdk::BoundView* const view = context_view(context);
    return view != nullptr && view->catalog != nullptr ? binding_locators(*view).size() : 0;
}

/** Copies one 1-based binding locator of the bound activity. @return False when absent. */
[[nodiscard]] bool
resolve_activity_binding_locator(const void* context,
                                 std::uint32_t localRow,
                                 lua_vm::ActivityBindingLocatorDefinition& output) noexcept {
    output = {};
    const sdk::BoundView* const view = context_view(context);
    if (view == nullptr || view->catalog == nullptr || localRow == 0) {
        return false;
    }
    const auto rows = binding_locators(*view);
    if (localRow > rows.size()) {
        return false;
    }
    const format::ActivityBindingLocator& row = rows[localRow - 1U];
    output = {.tag = row.tag, .offset = row.offset, .localRow = localRow};
    return row.tag != 0;
}

/** Rejects incomplete squad rows so Lua never receives a partly usable definition. */
[[nodiscard]] bool squad_definition(const sdk::BoundView& view,
                                    const format::Squad& squad,
                                    std::uint32_t localRow,
                                    lua_vm::SquadDefinition& output) noexcept {
    output = {};
    const sdk::Catalog& catalog = *view.catalog;
    const auto allSquads = catalog.squads();
    const auto members = sdk::squad_members(catalog, squad);
    if (members.empty() || members.size() > output.defaultCounts.size()
        || (squad.flags & format::kSquadRunnableMask) != format::kSquadRunnableMask
        || &squad < allSquads.data() || &squad >= allSquads.data() + allSquads.size()) {
        return false;
    }
    for (std::size_t index = 0; index < members.size(); ++index) {
        output.defaultCounts[index] = members[index].defaultCount;
    }
    output.id = catalog.string(squad.id);
    const auto slots = catalog.slots();
    if (squad.slotIndex >= slots.size()) {
        return false;
    }
    const format::Slot& slot = slots[squad.slotIndex];
    output.name = catalog.string(slot.name);
    output.nativeRow = static_cast<std::uint32_t>(&squad - allSquads.data());
    output.localRow = localRow;
    output.memberCount = members.size();
    const auto allObjects = catalog.objects();
    if (slot.objectIndex < allObjects.size()) {
        output.registryKey = allObjects[slot.objectIndex].objectKey;
    }
    output.slotType = slot.slotType;
    output.slotIndex = static_cast<std::uint16_t>(slot.slotIndex);
    return !output.id.empty();
}

/** Treats Lua squad rows as one-based positions inside the bound scenario. */
[[nodiscard]] bool resolve_squad_row(const void* context,
                                     std::uint32_t localRow,
                                     lua_vm::SquadDefinition& output) noexcept {
    output = {};
    const sdk::BoundView* const view = context_view(context);
    if (!valid_view(view) || localRow == 0) {
        return false;
    }
    const format::Scenario* const scenario = sdk::bound_scenario(*view);
    const auto squads = sdk::scenario_squads(*view->catalog, *scenario);
    return localRow <= squads.size()
           && squad_definition(*view, squads[localRow - 1], localRow, output);
}

/** Refuses squad names and aliases that do not resolve to one runnable squad. */
[[nodiscard]] bool resolve_squad_id(const void* context,
                                    std::string_view id,
                                    lua_vm::SquadDefinition& output) noexcept {
    output = {};
    const sdk::BoundView* const view = context_view(context);
    if (!valid_view(view) || id.empty()) {
        return false;
    }
    const format::Scenario* const scenario = sdk::bound_scenario(*view);
    const auto squads = sdk::scenario_squads(*view->catalog, *scenario);
    std::size_t matches = 0;
    lua_vm::SquadDefinition selected{};
    for (std::size_t index = 0; index < squads.size(); ++index) {
        const auto slots = view->catalog->slots();
        if (squads[index].slotIndex >= slots.size()) {
            return false;
        }
        const format::Slot& slot = slots[squads[index].slotIndex];
        bool names = view->catalog->string(squads[index].id) == id
                     || view->catalog->string(slot.id) == id
                     || view->catalog->string(slot.name) == id;
        for (const format::Text& alias : sdk::slot_aliases(*view->catalog, slot)) {
            names = names || view->catalog->string(alias.value) == id;
        }
        if (!names) {
            continue;
        }
        lua_vm::SquadDefinition candidate{};
        if (!squad_definition(
                *view, squads[index], static_cast<std::uint32_t>(index + 1), candidate)) {
            continue;
        }
        selected = candidate;
        ++matches;
    }
    if (matches != 1) {
        return false;
    }
    output = selected;
    return true;
}

[[nodiscard]] bool object_seen_before(std::span<const format::Occurrence> occurrences,
                                      std::size_t selected) noexcept {
    for (std::size_t index = 0; index < selected; ++index) {
        if (occurrences[index].objectIndex == occurrences[selected].objectIndex) {
            return true;
        }
    }
    return false;
}

/** Rejects cross-table rows before exposing stable native indices to Lua. */
[[nodiscard]] bool slot_definition(const sdk::BoundView& view,
                                   const format::Object& object,
                                   const format::Slot& slot,
                                   std::uint32_t localRow,
                                   lua_vm::SlotDefinition& output) noexcept {
    output = {};
    const sdk::Catalog& catalog = *view.catalog;
    const auto allObjects = catalog.objects();
    const auto allSlots = catalog.slots();
    if (&object < allObjects.data() || &object >= allObjects.data() + allObjects.size()
        || &slot < allSlots.data() || &slot >= allSlots.data() + allSlots.size()
        || slot.objectIndex != static_cast<std::uint32_t>(&object - allObjects.data())) {
        return false;
    }
    output.id = catalog.string(slot.id);
    output.name = catalog.string(slot.name);
    output.objectId = catalog.string(object.id);
    output.senseSchemaId = catalog.string(slot.senseSchemaId);
    output.authSchemaId = catalog.string(slot.authSchemaId);
    output.nativeRow = static_cast<std::uint32_t>(&slot - allSlots.data());
    output.localRow = localRow;
    output.objectTag = object.objectTag;
    output.registryKey = object.objectKey;
    output.slotIndex = slot.slotIndex;
    output.slotType = slot.slotType;
    output.componentClass = slot.componentClass;
    output.senseSchema = slot.senseSchema;
    output.authSchema = slot.authSchema;
    output.flags = slot.flags;
    return !output.id.empty() && !output.objectId.empty();
}

/** Deduplicates repeated object occurrences and requires one selected slot. */
template <typename Select>
[[nodiscard]] bool
resolve_slot(const sdk::BoundView& view, Select&& select, lua_vm::SlotDefinition& output) noexcept {
    output = {};
    if (!valid_view(&view)) {
        return false;
    }
    const sdk::Catalog& catalog = *view.catalog;
    const format::Scenario* const scenario = sdk::bound_scenario(view);
    const auto occurrences = sdk::scenario_occurrences(catalog, *scenario);
    const auto objects = catalog.objects();
    std::uint32_t localRow = 0;
    std::size_t matches = 0;
    lua_vm::SlotDefinition selected{};
    for (std::size_t occurrenceIndex = 0; occurrenceIndex < occurrences.size(); ++occurrenceIndex) {
        const format::Occurrence& occurrence = occurrences[occurrenceIndex];
        if (occurrence.objectIndex >= objects.size()) {
            return false;
        }
        if (object_seen_before(occurrences, occurrenceIndex)) {
            continue;
        }
        const format::Object& object = objects[occurrence.objectIndex];
        for (const format::Slot& slot : sdk::object_slots(catalog, object)) {
            if (localRow == (std::numeric_limits<std::uint32_t>::max)()) {
                return false;
            }
            ++localRow;
            lua_vm::SlotDefinition candidate{};
            if (!slot_definition(view, object, slot, localRow, candidate)) {
                return false;
            }
            if (!select(candidate)) {
                continue;
            }
            selected = candidate;
            ++matches;
        }
    }
    if (matches != 1) {
        return false;
    }
    output = selected;
    return true;
}

/** Treats Lua slot rows as one-based positions in the deduplicated scenario view. */
[[nodiscard]] bool resolve_slot_row(const void* context,
                                    std::uint32_t localRow,
                                    lua_vm::SlotDefinition& output) noexcept {
    const sdk::BoundView* const view = context_view(context);
    return valid_view(view) && localRow != 0
           && resolve_slot(
               *view,
               [localRow](const lua_vm::SlotDefinition& value) noexcept {
                   return value.localRow == localRow;
               },
               output);
}

/** Requires one exact slot ID, name, or alias in the bound scenario. */
[[nodiscard]] bool
resolve_slot_id(const void* context, std::string_view id, lua_vm::SlotDefinition& output) noexcept {
    const sdk::BoundView* const view = context_view(context);
    if (!valid_view(view) || id.empty()) {
        return false;
    }
    return resolve_slot(
        *view,
        [view, id](const lua_vm::SlotDefinition& value) noexcept {
            if (value.id == id || value.name == id) {
                return true;
            }
            const format::Slot& slot = view->catalog->slots()[value.nativeRow];
            for (const format::Text& alias : sdk::slot_aliases(*view->catalog, slot)) {
                if (view->catalog->string(alias.value) == id) {
                    return true;
                }
            }
            return false;
        },
        output);
}

/** Resolves sensor keys only when every authored slot identity field agrees. */
[[nodiscard]] bool resolve_sense_slot(const void* context,
                                      const host::SenseObservationKey& key,
                                      lua_vm::SlotDefinition& output) noexcept {
    const sdk::BoundView* const view = context_view(context);
    return valid_view(view) && key.senseSchema != 0
           && resolve_slot(
               *view,
               [&key](const lua_vm::SlotDefinition& value) noexcept {
                   return value.registryKey == key.registryKey && value.objectTag == key.objectTag
                          && value.slotIndex == key.slotIndex && value.slotType == key.slotType
                          && value.senseSchema == key.senseSchema;
               },
               output);
}

/** @return True when a catalog-global slot belongs to the bound scenario. */
[[nodiscard]] bool scenario_has_slot(const sdk::BoundView& view, std::uint32_t slotRow) noexcept {
    if (!valid_view(&view) || slotRow >= view.catalog->slots().size()) {
        return false;
    }
    const sdk::Catalog& catalog = *view.catalog;
    const auto objects = catalog.objects();
    const format::Slot& selected = catalog.slots()[slotRow];
    for (const format::Occurrence& occurrence :
         sdk::scenario_occurrences(catalog, *sdk::bound_scenario(view))) {
        if (occurrence.objectIndex == selected.objectIndex
            && occurrence.objectIndex < objects.size()) {
            return true;
        }
    }
    return false;
}

/** Copies one exact generated task target into its value-owned VM shape. */
[[nodiscard]] bool task_definition(const sdk::BoundView& view,
                                   const format::TaskTarget& row,
                                   std::uint32_t localRow,
                                   lua_vm::TaskSensorDefinition& output) noexcept {
    output = {};
    const format::Slot* const objective = sdk::task_linked_objective_slot(*view.catalog, row);
    if (objective == nullptr || !scenario_has_slot(view, row.taskSlotIndex)
        || !scenario_has_slot(view, row.objectiveSlotIndex)) {
        return false;
    }
    output.id = view.catalog->string(row.id);
    output.localRow = localRow;
    output.slotRow = row.taskSlotIndex;
    output.targetObjectKey = row.targetObjectKey;
    output.bitIndex = row.bitIndex;
    return !output.id.empty();
}

/** Treats task rows as one-based positions inside the bound scenario. */
[[nodiscard]] bool resolve_task_sensor_row(const void* context,
                                           std::uint32_t localRow,
                                           lua_vm::TaskSensorDefinition& output) noexcept {
    output = {};
    const sdk::BoundView* const view = context_view(context);
    if (!valid_view(view) || localRow == 0) {
        return false;
    }
    std::uint32_t current = 0;
    for (const format::TaskTarget& row : view->catalog->task_targets()) {
        if (!scenario_has_slot(*view, row.taskSlotIndex)) {
            continue;
        }
        ++current;
        if (current == localRow) {
            return task_definition(*view, row, current, output);
        }
    }
    return false;
}

/** Resolves one generated task ID without accepting an ambiguous alias. */
[[nodiscard]] bool resolve_task_sensor_id(const void* context,
                                          std::string_view id,
                                          lua_vm::TaskSensorDefinition& output) noexcept {
    output = {};
    const sdk::BoundView* const view = context_view(context);
    if (!valid_view(view) || id.empty()) {
        return false;
    }
    std::uint32_t current = 0;
    std::size_t matches = 0;
    lua_vm::TaskSensorDefinition selected{};
    for (const format::TaskTarget& row : view->catalog->task_targets()) {
        if (!scenario_has_slot(*view, row.taskSlotIndex)) {
            continue;
        }
        ++current;
        if (view->catalog->string(row.id) == id) {
            lua_vm::TaskSensorDefinition candidate{};
            if (task_definition(*view, row, current, candidate)) {
                selected = candidate;
                ++matches;
            }
        }
    }
    if (matches != 1) {
        return false;
    }
    output = selected;
    return true;
}

/** Copies one generated type-68 element into its crash-safe VM shape. */
[[nodiscard]] bool directive_definition(const sdk::BoundView& view,
                                        const format::DirectiveElement& row,
                                        std::uint32_t localRow,
                                        lua_vm::DirectiveElementDefinition& output) noexcept {
    output = {};
    if (!scenario_has_slot(view, row.slotIndex) || row.nameHash == 0
        || row.nameHash == format::kAbsentIndex || row.elementIndex < 0
        || static_cast<std::uint32_t>(row.elementIndex) >= row.elementCount) {
        return false;
    }
    output.id = view.catalog->string(row.id);
    output.localRow = localRow;
    output.slotRow = row.slotIndex;
    output.nameHash = row.nameHash;
    output.elementIndex = row.elementIndex;
    output.elementCount = row.elementCount;
    return !output.id.empty();
}

/** Treats directive rows as one-based positions inside the bound scenario. */
[[nodiscard]] bool
resolve_directive_element_row(const void* context,
                              std::uint32_t localRow,
                              lua_vm::DirectiveElementDefinition& output) noexcept {
    output = {};
    const sdk::BoundView* const view = context_view(context);
    if (!valid_view(view) || localRow == 0) {
        return false;
    }
    std::uint32_t current = 0;
    for (const format::DirectiveElement& row : view->catalog->directive_elements()) {
        if (!scenario_has_slot(*view, row.slotIndex)) {
            continue;
        }
        ++current;
        if (current == localRow) {
            return directive_definition(*view, row, current, output);
        }
    }
    return false;
}

/** Resolves the exact type-68 slot/hash/index triple before the native dereference. */
[[nodiscard]] bool resolve_directive_element(const void* context,
                                             std::uint32_t slotRow,
                                             std::uint32_t nameHash,
                                             std::int32_t elementIndex,
                                             lua_vm::DirectiveElementDefinition& output) noexcept {
    output = {};
    const sdk::BoundView* const view = context_view(context);
    if (!valid_view(view)) {
        return false;
    }
    std::uint32_t current = 0;
    std::size_t matches = 0;
    lua_vm::DirectiveElementDefinition selected{};
    for (const format::DirectiveElement& row : view->catalog->directive_elements()) {
        if (!scenario_has_slot(*view, row.slotIndex)) {
            continue;
        }
        ++current;
        if (row.slotIndex == slotRow && row.nameHash == nameHash
            && row.elementIndex == elementIndex) {
            lua_vm::DirectiveElementDefinition candidate{};
            if (directive_definition(*view, row, current, candidate)) {
                selected = candidate;
                ++matches;
            }
        }
    }
    if (matches != 1) {
        return false;
    }
    output = selected;
    return true;
}

/** Distinct state names the largest guard actor declares fit here with room to spare. */
inline constexpr std::size_t kPerformanceStateCapacity = 256;

/**
 * Resolves one state name on the squad a type-42 sensor drives. A zero hash selects the single
 * name the target declares. A given hash must be declared by every member that declares names.
 */
[[nodiscard]] bool resolve_performance_state(const void* context,
                                             std::uint32_t slotRow,
                                             std::uint32_t nameHash,
                                             lua_vm::PerformanceStateDefinition& output) noexcept {
    output = {};
    const sdk::BoundView* const view = context_view(context);
    if (!valid_view(view) || !scenario_has_slot(*view, slotRow)) {
        return false;
    }
    const sdk::Catalog& catalog = *view->catalog;
    const format::Slot& slot = catalog.slots()[slotRow];
    if (slot.slotType != format::kPerformanceSlotType
        || slot.componentClass != format::kPerformanceComponentClass
        || slot.authSchema != format::kPerformanceAuthSchema
        || (slot.flags & format::kSlotSchemaJoinExact) == 0) {
        return false;
    }
    std::uint32_t squadSlot = format::kAbsentIndex;
    std::size_t edges = 0;
    for (const format::AuthoredSceneSquadEdge& edge :
         sdk::slot_authored_scene_squad_edges(catalog, slot)) {
        if ((edge.flags & format::kAuthoredSceneSquadPerformanceTargetExact) != 0) {
            squadSlot = edge.squadSlotIndex;
            ++edges;
        }
    }
    if (edges != 1) {
        return false;
    }
    std::array<std::uint32_t, kPerformanceStateCapacity> seen{};
    std::size_t distinct = 0;
    std::size_t declaringMembers = 0;
    for (const format::Squad& squad : sdk::scenario_squads(catalog, *sdk::bound_scenario(*view))) {
        if (squad.slotIndex != squadSlot) {
            continue;
        }
        for (const format::SquadMember& member : sdk::squad_members(catalog, squad)) {
            const auto names = sdk::actor_class_state_names(catalog, member.actorClassIndex);
            if (names.empty()) {
                continue;
            }
            ++declaringMembers;
            bool holds = false;
            for (const format::ActorStateName& name : names) {
                holds = holds || name.nameHash == nameHash;
                const auto end = seen.begin() + static_cast<std::ptrdiff_t>(distinct);
                if (std::find(seen.begin(), end, name.nameHash) != end) {
                    continue;
                }
                if (distinct == seen.size()) {
                    return false;
                }
                seen[distinct++] = name.nameHash;
            }
            if (nameHash != 0 && !holds) {
                return false;
            }
        }
    }
    if (declaringMembers == 0 || (nameHash == 0 && distinct != 1)) {
        return false;
    }
    output.slotRow = slotRow;
    output.nameHash = nameHash != 0 ? nameHash : seen[0];
    output.stateCount = static_cast<std::uint32_t>(distinct);
    return true;
}

/** Compares every value-owned field captured for one generation-bound Slot handle. */
[[nodiscard]] bool same_slot_definition(const lua_vm::SlotDefinition& left,
                                        const lua_vm::SlotDefinition& right) noexcept {
    return left.id == right.id && left.name == right.name && left.objectId == right.objectId
           && left.senseSchemaId == right.senseSchemaId && left.authSchemaId == right.authSchemaId
           && left.nativeRow == right.nativeRow && left.localRow == right.localRow
           && left.objectTag == right.objectTag && left.registryKey == right.registryKey
           && left.slotIndex == right.slotIndex && left.slotType == right.slotType
           && left.componentClass == right.componentClass && left.senseSchema == right.senseSchema
           && left.authSchema == right.authSchema && left.flags == right.flags;
}

[[nodiscard]] bool scene_slot(const sdk::Catalog& catalog, const format::Slot& slot) noexcept {
    return slot.slotType == format::kAuthoredSceneSlotType
           && slot.componentClass == format::kAuthoredSceneComponentClass
           && slot.senseSchema == format::kAuthoredSceneSenseSchema
           && slot.authSchema == format::kAuthoredSceneAuthSchema
           && (slot.flags & format::kSlotSchemaJoinExact) != 0
           && sdk::slot_authored_scene_resources(catalog, slot).size() == 1;
}

/** Refuses scene identities that cannot fit the fixed Lua definition buffer. */
[[nodiscard]] bool scene_id(const sdk::Catalog& catalog,
                            const format::Occurrence& occurrence,
                            const format::Slot& slot,
                            lua_vm::SceneDefinition& output) noexcept {
    const std::string_view occurrenceId = catalog.string(occurrence.id);
    const std::string_view suffix = occurrenceId.starts_with(kOccurrencePrefix)
                                        ? occurrenceId.substr(kOccurrencePrefix.size())
                                        : occurrenceId;
    if (suffix.empty() || suffix.size() > 400
        || slot.slotIndex > (std::numeric_limits<std::uint16_t>::max)()
        || slot.slotType > (std::numeric_limits<std::uint16_t>::max)()) {
        return false;
    }
    const int written = std::snprintf(output.id.data(),
                                      output.id.size(),
                                      "symbol/%.*s/%04x/%04x",
                                      static_cast<int>(suffix.size()),
                                      suffix.data(),
                                      slot.slotIndex,
                                      slot.slotType);
    if (written <= 0 || static_cast<std::size_t>(written) >= output.id.size()) {
        output.id = {};
        return false;
    }
    output.idLength = static_cast<std::size_t>(written);
    return true;
}

/**
 * Resolves one authored scene of the bound activity into its Lua-facing definition.
 * Requires one matching exact scene slot across all scenario occurrences.
 */
template <typename Select>
[[nodiscard]] bool resolve_scene(const sdk::BoundView& view,
                                 Select&& select,
                                 lua_vm::SceneDefinition& output) noexcept {
    output = {};
    const sdk::Catalog& catalog = *view.catalog;
    const format::Scenario* const scenario = sdk::bound_scenario(view);
    const auto allOccurrences = catalog.occurrences();
    const auto allSlots = catalog.slots();
    const auto occurrences = sdk::scenario_occurrences(catalog, *scenario);
    std::uint32_t localRow = 0;
    std::size_t matches = 0;
    lua_vm::SceneDefinition selected{};
    for (const format::Occurrence& occurrence : occurrences) {
        if (occurrence.objectIndex >= catalog.objects().size()
            || &occurrence < allOccurrences.data()
            || &occurrence >= allOccurrences.data() + allOccurrences.size()) {
            return false;
        }
        const format::Object& object = catalog.objects()[occurrence.objectIndex];
        for (const format::Slot& slot : sdk::object_slots(catalog, object)) {
            if (!scene_slot(catalog, slot)) {
                continue;
            }
            ++localRow;
            lua_vm::SceneDefinition candidate{};
            candidate.localRow = localRow;
            candidate.occurrenceRow =
                static_cast<std::uint32_t>(&occurrence - allOccurrences.data());
            candidate.slotRow = static_cast<std::uint32_t>(&slot - allSlots.data());
            if (!scene_id(catalog, occurrence, slot, candidate)) {
                return false;
            }
            // scene_slot() already proved exactly one resource exists for this slot.
            const sdk::format::AuthoredSceneResource& resource =
                sdk::slot_authored_scene_resources(catalog, slot).front();
            candidate.resourceTag = resource.resourceTag;
            candidate.configTag = resource.configTag;
            candidate.descriptorOffset = resource.descriptorOffset;
            if (!select(candidate)) {
                continue;
            }
            selected = candidate;
            ++matches;
        }
    }
    if (matches != 1) {
        return false;
    }
    output = selected;
    return true;
}

/** Treats Lua scene rows as one-based positions inside the bound scenario. */
[[nodiscard]] bool resolve_scene_row(const void* context,
                                     std::uint32_t localRow,
                                     lua_vm::SceneDefinition& output) noexcept {
    const sdk::BoundView* const view = context_view(context);
    return valid_view(view) && localRow != 0
           && resolve_scene(
               *view,
               [localRow](const lua_vm::SceneDefinition& value) noexcept {
                   return value.localRow == localRow;
               },
               output);
}

/** Accepts the generated scene symbol or the generated ID/name of its owning slot. */
[[nodiscard]] bool resolve_scene_id(const void* context,
                                    std::string_view id,
                                    lua_vm::SceneDefinition& output) noexcept {
    const sdk::BoundView* const view = context_view(context);
    return valid_view(view) && !id.empty()
           && resolve_scene(
               *view,
               [view, id](const lua_vm::SceneDefinition& value) noexcept {
                   if (std::string_view(value.id.data(), value.idLength) == id
                       || value.slotRow >= view->catalog->slots().size()) {
                       return std::string_view(value.id.data(), value.idLength) == id;
                   }
                   const format::Slot& slot = view->catalog->slots()[value.slotRow];
                   if (view->catalog->string(slot.id) == id
                       || view->catalog->string(slot.name) == id) {
                       return scenes::authored_scene_availability(
                                  *view, value.occurrenceRow, value.slotRow)
                              == scenes::SceneStatus::ready;
                   }
                   for (const format::Text& alias : sdk::slot_aliases(*view->catalog, slot)) {
                       if (view->catalog->string(alias.value) == id) {
                           return scenes::authored_scene_availability(
                                      *view, value.occurrenceRow, value.slotRow)
                                  == scenes::SceneStatus::ready;
                       }
                   }
                   return false;
               },
               output);
}

[[nodiscard]] std::size_t squad_count(const void* context) noexcept {
    const sdk::BoundView* const view = context_view(context);
    if (!valid_view(view)) {
        return 0;
    }
    return sdk::scenario_squads(*view->catalog, *sdk::bound_scenario(*view)).size();
}

/** Returns zero when any scenario occurrence cannot be resolved safely. */
[[nodiscard]] std::size_t scene_count(const void* context) noexcept {
    const sdk::BoundView* const view = context_view(context);
    if (!valid_view(view)) {
        return 0;
    }
    std::size_t count = 0;
    const sdk::Catalog& catalog = *view->catalog;
    for (const format::Occurrence& occurrence :
         sdk::scenario_occurrences(catalog, *sdk::bound_scenario(*view))) {
        if (occurrence.objectIndex >= catalog.objects().size()) {
            return 0;
        }
        for (const format::Slot& slot :
             sdk::object_slots(catalog, catalog.objects()[occurrence.objectIndex])) {
            if (scene_slot(catalog, slot)) {
                ++count;
            }
        }
    }
    return count;
}

/** Counts repeated object definitions once and rejects an invalid occurrence set. */
[[nodiscard]] std::size_t slot_count(const void* context) noexcept {
    const sdk::BoundView* const view = context_view(context);
    if (!valid_view(view)) {
        return 0;
    }
    const sdk::Catalog& catalog = *view->catalog;
    const auto occurrences = sdk::scenario_occurrences(catalog, *sdk::bound_scenario(*view));
    const auto objects = catalog.objects();
    std::size_t count = 0;
    for (std::size_t occurrenceIndex = 0; occurrenceIndex < occurrences.size(); ++occurrenceIndex) {
        const format::Occurrence& occurrence = occurrences[occurrenceIndex];
        if (occurrence.objectIndex >= objects.size()) {
            return 0;
        }
        if (!object_seen_before(occurrences, occurrenceIndex)) {
            count += sdk::object_slots(catalog, objects[occurrence.objectIndex]).size();
        }
    }
    return count;
}

/** @return Sensor count of the bound task, or 0 when nothing is bound. */
[[nodiscard]] std::size_t task_sensor_count(const void* context) noexcept {
    const sdk::BoundView* const view = context_view(context);
    if (!valid_view(view)) {
        return 0;
    }
    return static_cast<std::size_t>(std::count_if(view->catalog->task_targets().begin(),
                                                  view->catalog->task_targets().end(),
                                                  [view](const format::TaskTarget& row) {
                                                      return scenario_has_slot(*view,
                                                                               row.taskSlotIndex);
                                                  }));
}

/** @return Element count of the bound directive, or 0 when nothing is bound. */
[[nodiscard]] std::size_t directive_element_count(const void* context) noexcept {
    const sdk::BoundView* const view = context_view(context);
    if (!valid_view(view)) {
        return 0;
    }
    return static_cast<std::size_t>(std::count_if(view->catalog->directive_elements().begin(),
                                                  view->catalog->directive_elements().end(),
                                                  [view](const format::DirectiveElement& row) {
                                                      return scenario_has_slot(*view,
                                                                               row.slotIndex);
                                                  }));
}

/** @return True only for the bit-exact identity transform the corpus proves for this data. */
[[nodiscard]] bool identity_transform(const scriptables::TriggerVolumeInstance& instance) noexcept {
    constexpr std::array<std::uint32_t, 4> identity{0, 0, 0, 0x3F800000U};
    for (std::size_t lane = 0; lane < identity.size(); ++lane) {
        if (std::bit_cast<std::uint32_t>(instance.rotation[lane]) != identity[lane]
            || std::bit_cast<std::uint32_t>(instance.position[lane]) != identity[lane]) {
            return false;
        }
    }
    return true;
}

/** Copies one authored trigger-volume instance's exact geometry into the bounded watcher shape. */
[[nodiscard]] bool copy_trigger_geometry(const scriptables::Snapshot& snapshot,
                                         const scriptables::TriggerVolumeInstance& instance,
                                         trigger_watch::Geometry& output) noexcept {
    output = {};
    if (!instance.complete || instance.active == 0 || !identity_transform(instance)
        || !std::isfinite(instance.extrusion) || instance.extrusion < 0.0F
        || instance.vertexCount == 0 || instance.triangleCount == 0
        || instance.vertexCount > trigger_watch::kMaxVertices
        || instance.triangleCount > trigger_watch::kMaxTriangles
        || instance.firstVertex + instance.vertexCount > snapshot.triggerVolumeVertices.size()
        || instance.firstTriangle + instance.triangleCount
               > snapshot.triggerVolumeTriangles.size()) {
        return false;
    }
    output.extrusion = instance.extrusion;
    output.vertexCount = instance.vertexCount;
    for (std::size_t index = 0; index < instance.vertexCount; ++index) {
        const scriptables::TriggerVolumeVertex& vertex =
            snapshot.triggerVolumeVertices[instance.firstVertex + index];
        output.vertices[index] = {vertex.value[0], vertex.value[1], vertex.value[2]};
    }
    output.triangleCount = instance.triangleCount;
    for (std::size_t index = 0; index < instance.triangleCount; ++index) {
        const scriptables::TriggerVolumeTriangle& triangle =
            snapshot.triggerVolumeTriangles[instance.firstTriangle + index];
        output.triangles[index] = {
            triangle.indices[0], triangle.indices[1], triangle.indices[2]};
    }
    return true;
}

/**
 * Arms client-side detection for one type-31 trigger slot.
 * Resolves the same type-31-to-type-60 mapping a real player-trigger incident resolves against,
 * copies that volume's authored geometry, and registers it with the client-side watcher, which
 * from then on tests the live local player position against it every frame.
 */
[[nodiscard]] bool register_trigger_watch(const void* context,
                                          std::uint32_t registryKey,
                                          std::uint32_t slotType,
                                          std::uint32_t slotIndex,
                                          std::uint32_t& outVolumeRegistryKey,
                                          std::uint32_t& outVolumeSlotType,
                                          std::uint32_t& outVolumeSlotIndex) noexcept {
    const sdk::BoundView* const view = context_view(context);
    if (!valid_view(view)) {
        core::log::writef(core::log::Channel::server,
                          core::log::Level::warn,
                          "ev=mission_script stage=trigger_watch_diag reason=invalid_view");
        return false;
    }
    // This catalog is only ever built on request (normally from the debug "Scriptable Browser"
    // panel); nothing else in the mission runtime asks for it, so it starts out empty. Requesting
    // it here is free once it is already built or building for this scenario.
    const auto& destination = view->binding.destination;
    const std::string_view scenarioName(
        reinterpret_cast<const char*>(destination.packageName.data()), destination.packageNameLength);
    state::build_data::scenarios::Definition layout{};
    if (scenarioName.empty() || !state::build_data::find_scenario_layout(scenarioName, layout)) {
        core::log::writef(core::log::Channel::server,
                          core::log::Level::warn,
                          "ev=mission_script stage=trigger_watch_diag reason=no_scenario_layout");
        return false;
    }
    static_cast<void>(scriptable_extraction::request(layout.tag, scenarioName, false));

    // Not yet built or still building: an ordinary, expected state while the background worker
    // catches up, not a problem worth logging. The caller (a Lua timer loop) retries.
    const scriptables::SnapshotView snapshot = scriptables::snapshot();
    if (snapshot == nullptr || snapshot->scenarioTag != layout.tag
        || snapshot->status != scriptables::BuildStatus::ready) {
        return false;
    }
    middleware::bap::activity_message::player_trigger_incident::Payload payload{};
    payload.registryKey = registryKey;
    payload.slotType = static_cast<std::int8_t>(slotType);
    payload.slotIndex = static_cast<std::int16_t>(slotIndex);
    player_trigger::Source source{};
    const player_trigger::ResolveStatus resolveStatus =
        player_trigger::resolve(*snapshot, payload, source);
    if (resolveStatus != player_trigger::ResolveStatus::ready) {
        core::log::writef(core::log::Channel::server,
                          core::log::Level::warn,
                          "ev=mission_script stage=trigger_watch_diag reason=resolve_failed "
                          "status=%u registry_key=%u slot_type=%u slot_index=%u",
                          static_cast<unsigned>(resolveStatus),
                          registryKey,
                          slotType,
                          slotIndex);
        return false;
    }

    const scriptables::TriggerVolumeTable* table = nullptr;
    for (const scriptables::TriggerVolumeTable& candidate : snapshot->triggerVolumeTables) {
        if (candidate.registryKey == source.volumeRegistryKey
            && candidate.slotType == source.volumeSlotType
            && candidate.slotIndex == source.volumeSlotIndex) {
            table = &candidate;
            break;
        }
    }
    if (table == nullptr || !table->complete || table->instanceCount == 0) {
        core::log::writef(
            core::log::Channel::server,
            core::log::Level::warn,
            "ev=mission_script stage=trigger_watch_diag reason=table_unavailable found=%d "
            "complete=%d instances=%u volume_registry_key=%u volume_slot_type=%u "
            "volume_slot_index=%u table_count=%zu",
            table != nullptr ? 1 : 0,
            table != nullptr ? static_cast<int>(table->complete) : -1,
            table != nullptr ? table->instanceCount : 0U,
            source.volumeRegistryKey,
            static_cast<unsigned>(source.volumeSlotType),
            static_cast<unsigned>(source.volumeSlotIndex),
            snapshot->triggerVolumeTables.size());
        return false;
    }

    trigger_watch::Geometry geometry{};
    bool built = false;
    for (std::uint32_t offset = 0; offset < table->instanceCount; ++offset) {
        const std::uint32_t instanceRow = table->firstInstance + offset;
        if (instanceRow >= snapshot->triggerVolumeInstances.size()) {
            core::log::writef(core::log::Channel::server,
                              core::log::Level::warn,
                              "ev=mission_script stage=trigger_watch_diag "
                              "reason=instance_row_out_of_range row=%u count=%zu",
                              instanceRow,
                              snapshot->triggerVolumeInstances.size());
            return false;
        }
        if (copy_trigger_geometry(*snapshot, snapshot->triggerVolumeInstances[instanceRow], geometry)) {
            built = true;
            break;
        }
    }
    if (!built) {
        const scriptables::TriggerVolumeInstance& first =
            snapshot->triggerVolumeInstances[table->firstInstance];
        core::log::writef(
            core::log::Channel::server,
            core::log::Level::warn,
            "ev=mission_script stage=trigger_watch_diag reason=geometry_unavailable "
            "complete=%d active=%u identity=%d extrusion=%f vertices=%u triangles=%u",
            static_cast<int>(first.complete),
            static_cast<unsigned>(first.active),
            static_cast<int>(identity_transform(first)),
            static_cast<double>(first.extrusion),
            first.vertexCount,
            first.triangleCount);
        return false;
    }

    trigger_watch::Identity identity{};
    identity.binding = view->binding;
    identity.activityClientGeneration = view->activityClientGeneration;
    identity.registryKey = registryKey;
    identity.slotType = static_cast<std::uint8_t>(slotType);
    identity.slotIndex = static_cast<std::uint16_t>(slotIndex);
    if (!trigger_watch::register_watch(identity, geometry)) {
        return false;
    }
    outVolumeRegistryKey = source.volumeRegistryKey;
    outVolumeSlotType = source.volumeSlotType;
    outVolumeSlotIndex = source.volumeSlotIndex;
    return true;
}

/**
 * Diagnostic tool: resolves the type-31 source of the Nth trigger row the debug "Scriptable
 * Browser" trigger-volume panel would list for one bubble index. Mirrors
 * activity_host_trigger_volumes.cpp's own row enumeration (bubble filter only; assumes no
 * text/scope filter is active there): each owner contributes one row per instance, or one row if
 * it has none.
 */
[[nodiscard]] bool find_trigger_by_bubble_visible_index(const void* context,
                                                        std::int32_t bubbleIndex,
                                                        std::uint32_t oneBasedVisibleIndex,
                                                        std::uint32_t& outRegistryKey,
                                                        std::uint32_t& outSlotType,
                                                        std::uint32_t& outSlotIndex,
                                                        std::uint32_t& outMatchCount,
                                                        std::uint32_t& outTotalRows,
                                                        std::uint32_t& outTableRegistryKey,
                                                        std::uint32_t& outTableSlotType,
                                                        std::uint32_t& outTableSlotIndex) noexcept {
    outMatchCount = 0;
    outTotalRows = 0;
    outTableRegistryKey = 0;
    outTableSlotType = 0;
    outTableSlotIndex = 0;
    const sdk::BoundView* const view = context_view(context);
    if (!valid_view(view) || oneBasedVisibleIndex == 0) {
        return false;
    }
    const auto& destination = view->binding.destination;
    const std::string_view scenarioName(
        reinterpret_cast<const char*>(destination.packageName.data()), destination.packageNameLength);
    state::build_data::scenarios::Definition layout{};
    if (scenarioName.empty() || !state::build_data::find_scenario_layout(scenarioName, layout)) {
        return false;
    }
    static_cast<void>(scriptable_extraction::request(layout.tag, scenarioName, false));
    const scriptables::SnapshotView snapshot = scriptables::snapshot();
    if (snapshot == nullptr || snapshot->scenarioTag != layout.tag
        || snapshot->status != scriptables::BuildStatus::ready) {
        return false;
    }

    std::uint32_t seen = 0;
    bool found = false;
    for (const scriptables::TriggerVolumeOwner& owner : snapshot->triggerVolumeOwners) {
        if (owner.tableRow >= snapshot->triggerVolumeTables.size()
            || owner.objectRow >= snapshot->objects.size()) {
            continue;
        }
        const scriptables::TriggerVolumeTable& table = snapshot->triggerVolumeTables[owner.tableRow];
        const scriptables::Object& object = snapshot->objects[owner.objectRow];
        if (object.bubbleRow >= snapshot->bubbles.size()
            || snapshot->bubbles[object.bubbleRow].index
                   != static_cast<std::uint32_t>(bubbleIndex)) {
            continue;
        }
        const std::uint32_t browserCount = table.instanceCount == 0 ? 1 : table.instanceCount;
        for (std::uint32_t offset = 0; offset < browserCount; ++offset) {
            ++seen;
            if (found || seen != oneBasedVisibleIndex) {
                continue;
            }
            found = true;
            outMatchCount = owner.incomingReferenceCount;
            outTableRegistryKey = table.registryKey;
            outTableSlotType = table.slotType;
            outTableSlotIndex = table.slotIndex;
            if (owner.incomingReferenceCount != 1
                || owner.firstIncomingReference
                       >= snapshot->triggerVolumeIncomingReferences.size()) {
                continue;
            }
            const scriptables::TriggerVolumeIncomingReference& incoming =
                snapshot->triggerVolumeIncomingReferences[owner.firstIncomingReference];
            if (incoming.sourceObjectRow >= snapshot->objects.size()
                || incoming.sourceSlotRow >= snapshot->slots.size()) {
                continue;
            }
            const scriptables::Object& sourceObject = snapshot->objects[incoming.sourceObjectRow];
            const scriptables::Slot& sourceSlot = snapshot->slots[incoming.sourceSlotRow];
            outRegistryKey = sourceObject.registryKey;
            outSlotType = sourceSlot.slotType;
            outSlotIndex = sourceSlot.slotIndex;
        }
    }
    outTotalRows = seen;
    return found && outMatchCount == 1;
}

[[nodiscard]] char hex_digit(std::uint8_t value) noexcept {
    // Lowercase hex digits; every digest and byte field is spelled this way.
    constexpr char digits[] = "0123456789abcdef";
    return digits[value & 0xFU];
}

} // namespace

/** Copies the bound activity's binding definition out of the catalog. */
bool activity_binding_definition(const sdk::Catalog& catalog,
                                 const format::Activity& activity,
                                 lua_vm::ActivityBindingDefinition& output) noexcept {
    output = {};
    const auto activities = catalog.activities();
    if (activities.empty()) {
        return false;
    }
    const auto first = reinterpret_cast<std::uintptr_t>(activities.data());
    const auto selected = reinterpret_cast<std::uintptr_t>(&activity);
    const std::size_t bytes = activities.size_bytes();
    if (selected < first || selected - first >= bytes
        || (selected - first) % sizeof(format::Activity) != 0) {
        return false;
    }
    output.internalName = catalog.string(activity.internalName);
    output.displayName = catalog.string(activity.displayName);
    output.selectedActivityRootTag = activity.selectedActivityRootTag;
    output.selectedScenarioTag = activity.selectedScenarioTag;
    output.matchmakingConfigTag = activity.matchmakingConfigTag;
    output.joinStatus = activity.joinStatus;
    output.bindingDisposition = activity.bindingDisposition;
    output.bindingReason = activity.bindingReason;
    output.bindingEvidenceBasis = activity.bindingEvidenceBasis;
    output.runnableStatus = activity.runnableStatus;
    output.fullSdkAcceptable =
        (activity.bindingFlags & format::kActivityBindingFullSdkAcceptable) != 0;
    output.hasInternalName = (activity.bindingFlags & format::kActivityBindingHasInternalName) != 0;
    output.hasMatchmakingConfig =
        (activity.bindingFlags & format::kActivityBindingHasMatchmakingConfig) != 0;
    return true;
}

/** @return The binding tag span one kind names, or empty when the kind is unknown. */
std::span<const format::ActivityBindingTag>
activity_binding_tags(const sdk::Catalog& catalog,
                      const format::Activity& activity,
                      lua_vm::ActivityBindingTagKind kind) noexcept {
    switch (kind) {
    case lua_vm::ActivityBindingTagKind::activityRootCandidates:
        return sdk::activity_root_candidate_tags(catalog, activity);
    case lua_vm::ActivityBindingTagKind::scenarioNameCandidates:
        return sdk::activity_scenario_name_candidate_tags(catalog, activity);
    case lua_vm::ActivityBindingTagKind::evidenceRoots:
        return sdk::activity_evidence_root_tags(catalog, activity);
    }
    return {};
}

/** Publishes only bound activity identities with a complete 32-byte SDK digest. */
bool program_identity(const sdk::BoundView& view,
                      bool publicTarget,
                      lua_vm::ProgramIdentity& output) noexcept {
    output = {};
    const format::Activity* const activity = sdk::bound_activity(view);
    if (activity == nullptr || view.catalog == nullptr) {
        return false;
    }
    const auto digest = view.catalog->sdk_build_sha256();
    const std::string_view activityId = view.catalog->string(activity->id);
    if (digest.size() != 32 || activityId.empty() || activityId.size() >= output.activityId.size()
        || view.activityRow == (std::numeric_limits<std::uint32_t>::max)()) {
        return false;
    }
    // The generated SDK build identity is spelled as this prefix and 64 hex digits.
    constexpr std::string_view prefix = "sha256:";
    std::copy(prefix.begin(), prefix.end(), output.sdkBuildId.begin());
    std::size_t cursor = prefix.size();
    for (const std::byte byte : digest) {
        const auto value = static_cast<std::uint8_t>(byte);
        output.sdkBuildId[cursor++] = hex_digit(static_cast<std::uint8_t>(value >> 4U));
        output.sdkBuildId[cursor++] = hex_digit(value);
    }
    std::copy(activityId.begin(), activityId.end(), output.activityId.begin());
    output.activityRow = view.activityRow + 1;
    output.definitionHash = activity->definitionHash;
    output.publicTarget = publicTarget;
    return true;
}

/**
 * True while the client has not yet physically streamed into the selected/initial_state region.
 * An unresolved query (stale view, no lease yet) reports pending rather than arrived, so a script
 * polling this can never fire on a false "arrived" reading.
 */
[[nodiscard]] bool region_arrival_pending(const void* context) noexcept {
    const sdk::BoundView* const view = context_view(context);
    if (!valid_view(view)) {
        return true;
    }
    scenes::Snapshot snapshot{};
    if (scenes::query(*view, snapshot) != scenes::Status::ready) {
        return true;
    }
    return snapshot.regionArrivalPending;
}

/**
 * Exploratory alternative spawn signal: true once the client's physics hook has a live local
 * player body to read a position from. Not reset on activity attach, so it can already read true
 * from a previous activity; a script should watch for its own true-to-false-to-true transition
 * rather than trust a single reading.
 */
[[nodiscard]] bool player_position_present(const void* /*context*/) noexcept {
    return client::player::position::snapshot().present;
}

/** Exploratory: the client's raw boot-flow step, for observing the real spawn sequence. */
[[nodiscard]] std::int32_t bootflow_step(const void* /*context*/) noexcept {
    return client::hooks::bootflow::raw_step();
}

/**
 * Diagnostic tool: reads the game's own content-hash resolver's definition blob for `hash`
 * (e.g. an authored scene's resourceTag) directly out of process memory, so a script can scan it
 * for embedded reference hashes the extracted SDK catalog does not expose (an authored scene's
 * own event-gate node keys, in particular).
 */
[[nodiscard]] bool resolve_content_hash(const void* /*context*/,
                                        std::uint32_t hash,
                                        std::uint8_t* outBytes,
                                        std::uint32_t outCapacity,
                                        std::uint32_t& outLength) noexcept {
    namespace resolver = client::hooks::content_resolver;
    outLength = 0;
    if (outBytes == nullptr || outCapacity == 0) {
        return false;
    }
    resolver::install();
    const std::size_t copied =
        resolver::resolve(hash, std::span(reinterpret_cast<std::byte*>(outBytes), outCapacity));
    outLength = static_cast<std::uint32_t>(copied);
    return copied != 0;
}

/** Development diagnostic: see `lua_vm::DumpContentHash`. */
[[nodiscard]] std::uint32_t dump_content_hash(const void* /*context*/,
                                              std::uint32_t hash,
                                              std::uint32_t depth) noexcept {
    namespace resolver = client::hooks::content_resolver;
    resolver::install();
    return static_cast<std::uint32_t>(resolver::dump_tree(hash, depth));
}

/** Development diagnostic: see `lua_vm::FindEventGateKeys`. */
[[nodiscard]] std::uint32_t find_event_gate_keys(const void* /*context*/,
                                                 std::uint32_t hash,
                                                 std::uint32_t* outKeys,
                                                 std::uint32_t outCapacity) noexcept {
    namespace resolver = client::hooks::content_resolver;
    if (outKeys == nullptr || outCapacity == 0) {
        return 0;
    }
    resolver::install();
    return static_cast<std::uint32_t>(
        resolver::find_event_gate_keys(hash, std::span(outKeys, outCapacity)));
}

/** See `lua_vm::UnregisterTriggerWatch`. */
[[nodiscard]] bool unregister_trigger_watch(const void* context,
                                            std::uint32_t registryKey,
                                            std::uint32_t slotType,
                                            std::uint32_t slotIndex) noexcept {
    const sdk::BoundView* const view = context_view(context);
    if (!valid_view(view)) {
        return false;
    }
    trigger_watch::Identity identity{};
    identity.binding = view->binding;
    identity.activityClientGeneration = view->activityClientGeneration;
    identity.registryKey = registryKey;
    identity.slotType = static_cast<std::uint8_t>(slotType);
    identity.slotIndex = static_cast<std::uint16_t>(slotIndex);
    return trigger_watch::unregister_watch(identity);
}

/** See `lua_vm::SquadStateNames`: (group, name) pairs from the SDK actor-state-name table. */
[[nodiscard]] std::uint32_t squad_state_names(const void* context,
                                              std::uint32_t localRow,
                                              lua_vm::ActorStateNameDefinition* outNames,
                                              std::uint32_t outCapacity) noexcept {
    const sdk::BoundView* const view = context_view(context);
    if (!valid_view(view) || localRow == 0 || outNames == nullptr) {
        return 0;
    }
    const sdk::Catalog& catalog = *view->catalog;
    const auto squads = sdk::scenario_squads(catalog, *sdk::bound_scenario(*view));
    if (localRow > squads.size()) {
        return 0;
    }
    std::uint32_t found = 0;
    for (const format::SquadMember& member : sdk::squad_members(catalog, squads[localRow - 1])) {
        for (const format::ActorStateName& name :
             sdk::actor_class_state_names(catalog, member.actorClassIndex)) {
            bool seen = false;
            for (std::uint32_t index = 0; index < found && index < outCapacity; ++index) {
                seen = seen || (outNames[index].groupHash == name.groupHash
                                && outNames[index].nameHash == name.nameHash);
            }
            if (seen) {
                continue;
            }
            if (found < outCapacity) {
                outNames[found] = {.groupHash = name.groupHash,
                                   .nameHash = name.nameHash,
                                   .ordinal = name.ordinal};
            }
            ++found;
        }
    }
    return found;
}

/** The caller must keep the immutable view alive while Lua uses the returned callbacks. */
lua_vm::DefinitionApi definition_api(const sdk::BoundView& view) noexcept {
    lua_vm::DefinitionApi output{
        .context = &view,
        .resolveSquadRow = &resolve_squad_row,
        .resolveSquadId = &resolve_squad_id,
        .resolveSceneRow = &resolve_scene_row,
        .resolveSceneId = &resolve_scene_id,
        .resolveSlotRow = &resolve_slot_row,
        .resolveSlotId = &resolve_slot_id,
        .resolveSenseSlot = &resolve_sense_slot,
        .resolveTaskSensorRow = &resolve_task_sensor_row,
        .resolveTaskSensorId = &resolve_task_sensor_id,
        .resolveDirectiveElementRow = &resolve_directive_element_row,
        .resolveDirectiveElement = &resolve_directive_element,
        .resolvePerformanceState = &resolve_performance_state,
        .resolveActivityBinding = &resolve_activity_binding,
        .activityBindingTagCount = &activity_binding_tag_count,
        .resolveActivityBindingTag = &resolve_activity_binding_tag,
        .activityBindingLocatorCount = &activity_binding_locator_count,
        .resolveActivityBindingLocator = &resolve_activity_binding_locator,
        .squadCount = &squad_count,
        .sceneCount = &scene_count,
        .slotCount = &slot_count,
        .taskSensorCount = &task_sensor_count,
        .directiveElementCount = &directive_element_count,
        .regionArrivalPending = &region_arrival_pending,
        .registerTriggerWatch = &register_trigger_watch,
        .unregisterTriggerWatch = &unregister_trigger_watch,
        .playerPositionPresent = &player_position_present,
        .bootflowStep = &bootflow_step,
        .findTriggerByBubbleVisibleIndex = &find_trigger_by_bubble_visible_index,
        .resolveContentHash = &resolve_content_hash,
        .dumpContentHash = &dump_content_hash,
        .findEventGateKeys = &find_event_gate_keys,
        .squadStateNames = &squad_state_names,
    };
    message_catalog::attach(output);
    output.catalog = catalog_definition_api(view);
    return output;
}

} // namespace sunrise::server::activity::mission::sdk_bridge
