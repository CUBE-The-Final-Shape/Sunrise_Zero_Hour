#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "../../state/activity/definition.h"

namespace sunrise::client::activity::player_trigger_watch {

/** Largest authored trigger-volume footprint this watcher tests against. Generous for a prism. */
inline constexpr std::size_t kMaxVertices = 64;
inline constexpr std::size_t kMaxTriangles = 64;

/** One world-space vertex of a watched volume's authored footprint (extruded along +Z). */
struct Vertex final {
    float x{};
    float y{};
    float z{};
};

/** One triangle of the footprint, indexing into the same watch's `vertices`. */
struct Triangle final {
    std::uint8_t a{};
    std::uint8_t b{};
    std::uint8_t c{};
};

/** Bounded copy of one authored trigger-volume instance's exact extruded prism. */
struct Geometry final {
    std::array<Vertex, kMaxVertices> vertices{};
    std::array<Triangle, kMaxTriangles> triangles{};
    std::size_t vertexCount{};
    std::size_t triangleCount{};
    float extrusion{};
};

/**
 * The wire identity a crossing is reported under, and the session it belongs to.
 * These are exactly the fields `player_trigger_incident::Payload` carries for a type-31 source:
 * the owning object's registryKey, and the source slot's own type (31) and index.
 */
struct Identity final {
    state::activity::SessionBinding binding{};
    /** The mission's ActivityClient generation at watch time; stamped onto the synthesized
     * incident so the feed's generation-match gate accepts it the same way it would a real one. */
    std::uint64_t activityClientGeneration{};
    std::uint32_t registryKey{};
    std::uint8_t slotType{};
    std::uint16_t slotIndex{};
};

/**
 * Registers (or replaces) one watched trigger volume for the local player.
 * Safe to call from any thread; the actual containment test runs on the next `poll`.
 * @return False when the geometry exceeds the bounded capacity or the watch table is full.
 */
[[nodiscard]] bool register_watch(const Identity& identity, const Geometry& geometry) noexcept;

/**
 * Drops the one watch registered under `identity` (binding + registryKey/slotType/slotIndex), so a
 * one-shot trigger stops costing a table entry once it has fired. The table holds 64 entries and
 * is otherwise only cleared with the mission instance, so long-lived scripts should release.
 * @return False when no such watch exists.
 */
bool unregister_watch(const Identity& identity) noexcept;

/** Drops every watch belonging to one session, e.g. before a fresh mission attach. */
void clear_watches(const state::activity::SessionBinding& binding) noexcept;

/** Drops every watch. */
void clear_all() noexcept;

/**
 * Tests the live local player position against every watched volume.
 * On a containment transition, synthesizes and submits the same schema-0x8080879F player-trigger
 * incident the retail client would send, through the same ingestion path a real one takes.
 * Call once per frame from a game thread, alongside `player::position::poll()`.
 */
void poll() noexcept;

} // namespace sunrise::client::activity::player_trigger_watch
