/**
 * Client-side player-trigger-volume detection.
 *
 * The retail client never reports a local player crossing a type-31 trigger volume (confirmed:
 * no schema-0x8080879F incident is ever observed on the wire for it). Sunrise already tracks the
 * local player's live world position (`player::position`) and already extracts the exact
 * world-space extruded-prism geometry of every trigger volume (`state::build_data::scriptables`,
 * the same data `package_trigger_volume_geometry` draws as a debug wireframe). This module
 * combines the two: it tests the live position against every volume a mission script asked to
 * watch, and on a containment transition it synthesizes the same incident payload the retail
 * client would have sent, and submits it through the same ingestion path a real one takes
 * (`host::submit_incident`), so nothing downstream of that has to know the crossing was detected
 * locally rather than reported by the game.
 */

#include "player_trigger_watch.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <span>

#include "../../core/logging/log.h"
#include "../../middleware/bap/activity_message/incident.h"
#include "../../middleware/bap/activity_message/player_trigger_incident.h"
#include "../../server/activity/host_runtime.h"
#include "../player/player_position.h"

namespace sunrise::client::activity::player_trigger_watch {
namespace {

namespace incident = middleware::bap::activity_message::incident;
namespace player_trigger_incident = middleware::bap::activity_message::player_trigger_incident;

/** Watches this session is unlikely to ever need more of at once. */
constexpr std::size_t kMaxWatches = 64;
/** A point must clear the footprint's Z band by at least this much to count as inside. */
constexpr float kVerticalSlack = 0.05F;

struct WatchEntry final {
    Identity identity{};
    Geometry geometry{};
    bool occupied{};
    bool inside{};
};

SRWLOCK g_lock = SRWLOCK_INIT;
std::array<WatchEntry, kMaxWatches> g_watches{};

/** @return True when `(px, py)` lies inside the closed triangle, ignoring Z. */
[[nodiscard]] bool point_in_triangle_xy(float px,
                                        float py,
                                        const Vertex& a,
                                        const Vertex& b,
                                        const Vertex& c) noexcept {
    const float d1 = (px - b.x) * (a.y - b.y) - (a.x - b.x) * (py - b.y);
    const float d2 = (px - c.x) * (b.y - c.y) - (b.x - c.x) * (py - c.y);
    const float d3 = (px - a.x) * (c.y - a.y) - (c.x - a.x) * (py - a.y);
    const bool hasNegative = d1 < 0.0F || d2 < 0.0F || d3 < 0.0F;
    const bool hasPositive = d1 > 0.0F || d2 > 0.0F || d3 > 0.0F;
    return !(hasNegative && hasPositive);
}

/** @return True when the world point lies inside the watch's extruded prism. */
[[nodiscard]] bool contains(const Geometry& geometry, float x, float y, float z) noexcept {
    if (geometry.vertexCount == 0 || geometry.triangleCount == 0) {
        return false;
    }
    float minZ = geometry.vertices[0].z;
    float maxZ = geometry.vertices[0].z;
    for (std::size_t index = 1; index < geometry.vertexCount; ++index) {
        minZ = (std::min)(minZ, geometry.vertices[index].z);
        maxZ = (std::max)(maxZ, geometry.vertices[index].z);
    }
    if (z < minZ - kVerticalSlack || z > maxZ + geometry.extrusion + kVerticalSlack) {
        return false;
    }
    for (std::size_t index = 0; index < geometry.triangleCount; ++index) {
        const Triangle& triangle = geometry.triangles[index];
        if (triangle.a >= geometry.vertexCount || triangle.b >= geometry.vertexCount
            || triangle.c >= geometry.vertexCount) {
            continue;
        }
        if (point_in_triangle_xy(x,
                                 y,
                                 geometry.vertices[triangle.a],
                                 geometry.vertices[triangle.b],
                                 geometry.vertices[triangle.c])) {
            return true;
        }
    }
    return false;
}

/**
 * Encodes and submits one synthetic schema-0x8080879F incident for a containment transition.
 * The real wire schema carries no enter/exit bit (the retail client only ever seems to report
 * one direction), so `resolvedObjectId` -- otherwise unused by `resolve()` -- carries it here:
 * 0 for entered, 1 for exited. `on_event_player_trigger`'s `resolved_object_id` field already
 * exposes this to Lua, so no event-schema change was needed to add exit reporting.
 */
void submit_transition(const Identity& identity, bool entering) noexcept {
    player_trigger_incident::Payload payload{};
    payload.registryKey = identity.registryKey;
    payload.slotType = static_cast<std::int8_t>(identity.slotType);
    payload.slotIndex = static_cast<std::int16_t>(identity.slotIndex);
    payload.resolvedObjectId = entering ? 0U : 1U;

    std::array<std::byte, player_trigger_incident::kPayloadBytes> body{};
    if (!player_trigger_incident::encode(payload, body)) {
        core::log::writef(core::log::Channel::client,
                          core::log::Level::warn,
                          "ev=activity stage=trigger_watch_submit_diag reason=encode_failed");
        return;
    }

    incident::Incident wire{};
    wire.primaryTarget = player_trigger_incident::kPrimaryTarget;
    wire.payloadLength = static_cast<std::uint32_t>(body.size());
    std::copy(body.begin(), body.end(), wire.payload.begin());
    if (!incident::outer_valid(wire)) {
        core::log::writef(core::log::Channel::client,
                          core::log::Level::warn,
                          "ev=activity stage=trigger_watch_submit_diag reason=outer_invalid "
                          "target=%u payload_length=%u",
                          wire.primaryTarget,
                          wire.payloadLength);
        return;
    }

    // Round-tripped through the real decoder rather than assigned directly, so this exercises
    // exactly the same decode the server runs for a wire-delivered incident.
    player_trigger_incident::Payload decoded{};
    if (!player_trigger_incident::decode(std::span(wire.payload).first(wire.payloadLength),
                                         decoded)) {
        core::log::writef(core::log::Channel::client,
                          core::log::Level::warn,
                          "ev=activity stage=trigger_watch_submit_diag reason=decode_failed");
        return;
    }

    server::activity::host::IncidentInput input{};
    input.binding = identity.binding;
    input.incident = wire;
    input.sourceGeneration = identity.activityClientGeneration;
    input.payloadBytes = wire.payloadLength;
    input.hasPlayerTrigger = true;
    input.playerTrigger = decoded;
    if (!server::activity::host::submit_incident(input)) {
        core::log::writef(core::log::Channel::client,
                          core::log::Level::warn,
                          "ev=activity stage=trigger_watch_submit_diag reason=submit_refused "
                          "registry_key=%u",
                          identity.registryKey);
    }
}

} // namespace

bool register_watch(const Identity& identity, const Geometry& geometry) noexcept {
    if (geometry.vertexCount > kMaxVertices || geometry.triangleCount > kMaxTriangles) {
        return false;
    }
    AcquireSRWLockExclusive(&g_lock);
    WatchEntry* freeSlot = nullptr;
    for (WatchEntry& entry : g_watches) {
        if (entry.occupied && same_binding(entry.identity.binding, identity.binding)
            && entry.identity.registryKey == identity.registryKey
            && entry.identity.slotType == identity.slotType
            && entry.identity.slotIndex == identity.slotIndex) {
            entry.identity = identity;
            entry.geometry = geometry;
            entry.inside = false;
            ReleaseSRWLockExclusive(&g_lock);
            return true;
        }
        if (freeSlot == nullptr && !entry.occupied) {
            freeSlot = &entry;
        }
    }
    if (freeSlot == nullptr) {
        ReleaseSRWLockExclusive(&g_lock);
        return false;
    }
    freeSlot->identity = identity;
    freeSlot->geometry = geometry;
    freeSlot->occupied = true;
    freeSlot->inside = false;
    ReleaseSRWLockExclusive(&g_lock);
    return true;
}

void clear_watches(const state::activity::SessionBinding& binding) noexcept {
    AcquireSRWLockExclusive(&g_lock);
    for (WatchEntry& entry : g_watches) {
        if (entry.occupied && same_binding(entry.identity.binding, binding)) {
            entry = {};
        }
    }
    ReleaseSRWLockExclusive(&g_lock);
}

void clear_all() noexcept {
    AcquireSRWLockExclusive(&g_lock);
    for (WatchEntry& entry : g_watches) {
        entry = {};
    }
    ReleaseSRWLockExclusive(&g_lock);
}

void poll() noexcept {
    const player::position::Snapshot position = player::position::snapshot();
    if (!position.present) {
        return;
    }
    // Transitions are collected under the lock and submitted after it is released, since
    // `submit_incident` takes the host runtime's own separate lock.
    struct Transition final {
        Identity identity{};
        bool entering{};
    };
    std::array<Transition, kMaxWatches> transitioned{};
    std::size_t transitionCount = 0;

    AcquireSRWLockExclusive(&g_lock);
    for (WatchEntry& entry : g_watches) {
        if (!entry.occupied) {
            continue;
        }
        const bool inside = contains(entry.geometry,
                                     position.position[0],
                                     position.position[1],
                                     position.position[2]);
        if (inside != entry.inside) {
            entry.inside = inside;
            if (transitionCount < transitioned.size()) {
                transitioned[transitionCount++] = {entry.identity, inside};
            }
        }
    }
    ReleaseSRWLockExclusive(&g_lock);

    // The mission's own type-60 resolve fans this single wire identity out to whichever authored
    // volumes actually reference this source slot, the same way it would for a real
    // client-sent incident; the entering/exiting direction rides along in resolvedObjectId.
    for (std::size_t index = 0; index < transitionCount; ++index) {
        submit_transition(transitioned[index].identity, transitioned[index].entering);
    }
}

} // namespace sunrise::client::activity::player_trigger_watch
