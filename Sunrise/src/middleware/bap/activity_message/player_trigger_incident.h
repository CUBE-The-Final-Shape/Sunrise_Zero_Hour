#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace sunrise::middleware::bap::activity_message::player_trigger_incident {

/** Global incident-table row whose payload uses schema 0x8080879F. */
inline constexpr std::uint32_t kPrimaryTarget = 6'685;
/** Runtime metadata schema carried by the player-trigger incident payload. */
inline constexpr std::uint32_t kSchema = 0x8080879FU;
/** The native schema consumes 422 meaningful bits and two zero padding bits. */
inline constexpr std::size_t kPayloadBits = 422;
inline constexpr std::size_t kPayloadBytes = 53;

/** The authored type-31 source ClientRef and resolved target id retained from one payload. */
struct Payload final {
    std::uint32_t registryKey{};
    std::uint32_t resolvedObjectId{};
    std::int16_t slotIndex{-1};
    std::int8_t slotType{-1};
};

/** Decodes the fixed schema-0x8080879F payload, including its required zero padding. */
[[nodiscard]] bool decode(std::span<const std::byte> input, Payload& output) noexcept;

/**
 * Encodes the fixed schema-0x8080879F payload for a synthesized incident.
 * The leading client-reference-offset bits carry no meaning `decode` reads, so they are written
 * zero; a round trip through `decode` recovers exactly the fields given here.
 * @param input The ClientRef and resolved target id to encode. `resolvedObjectId` is accepted for
 * wire completeness only; native resolution never reads it back.
 * @param output Exactly `kPayloadBytes` bytes, cleared first.
 * @return True when every field fit its wire width and the buffer size matched.
 */
[[nodiscard]] bool encode(const Payload& input, std::span<std::byte> output) noexcept;

} // namespace sunrise::middleware::bap::activity_message::player_trigger_incident
