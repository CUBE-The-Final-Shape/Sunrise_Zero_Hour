#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "../../encoding/bit_writer.h"
#include "auth_fields.h"
#include "scriptable_auth_body.h"
#include "squad_auth_body.h"

// Type-2 combatant Auth bodies that drive one named actor: a movement path, a custom action, a
// passenger delivery and a retirement. All four share the root prefix in fields .0 to .3.

namespace sunrise::middleware::bap::activity_message::combatant_auth {

namespace fields = auth_fields;

inline constexpr std::uint32_t kSchema = scriptable_auth::kType2Schema;
/** Root .1 mode and .2 marker are written as one; the client keeps the placement it has. */
inline constexpr std::uint8_t kRootModeWidth = 2;
inline constexpr std::uint8_t kRootMarkerWidth = 3;
inline constexpr std::uint32_t kRootModeValue = 1;
inline constexpr std::uint32_t kRootMarkerValue = 1;
/** Field .6 program header: two 6-bit values written as 0 then 1. Meaning unverified. */
inline constexpr std::uint8_t kProgramHeaderWidth = 6;
inline constexpr std::uint32_t kProgramHeaderFirst = 0;
inline constexpr std::uint32_t kProgramHeaderSecond = 1;
/** Field .6 program kind, biased by one on the wire: 3 follows a path, 9 runs an action. */
inline constexpr std::uint8_t kProgramKindWidth = 4;
inline constexpr std::uint32_t kProgramKindBias = 1;
inline constexpr std::uint32_t kPathProgramKind = 3;
inline constexpr std::uint32_t kActionProgramKind = 9;
/** Field .6 completion selector: logical zero with bias one, the native completion. */
inline constexpr std::uint8_t kCompletionWidth = 2;
inline constexpr std::uint32_t kCompletionValue = 1;
/** A path program names a type-58 authored path and one of its two markers. */
inline constexpr std::uint32_t kPathSlotType = 58;
inline constexpr std::uint32_t kPathComponentClass = 0x80807D9BU;
inline constexpr std::uint8_t kPathMarkerWidth = 8;
/** Marker 0 is the path start, marker 1 its destination. */
inline constexpr std::uint32_t kPathDestinationMarker = 1;
/** An action program names an authored group and action hash, with an optional spatial target. */
inline constexpr std::uint8_t kActionTargetModeWidth = 3;
inline constexpr std::uint8_t kActionTargetMarkerWidth = 8;
/** The 8-bit target marker stores -1 with a bias of 128. */
inline constexpr std::uint32_t kActionNoTargetMarker = 127;
/**
 * Both the path handler (client rva `0xa97bb0`) and the action handler (`0xa97800`) resolve
 * their ClientRef through the same routine, `0x4ffec0`, which accepts exactly two slot types:
 * a point set and an authored path. Every other type leaves the handle at `0xffffffff`, and the
 * handler then indexes its entity table with that value — the main-loop stall observed live.
 * Refuse anything else before sending.
 */
inline constexpr std::uint32_t kPointSetSlotType = 48;
/** @return True when `slotType` is a spatial reference either program's decoder accepts. */
[[nodiscard]] constexpr bool spatial_reference_slot_type(std::uint32_t slotType) noexcept {
    return slotType == kPointSetSlotType || slotType == kPathSlotType;
}
/** Field .7 delivery manifest: a 4-bit squad count, then one type-1 squad ClientRef each. */
inline constexpr std::uint8_t kManifestCountWidth = 4;
inline constexpr std::size_t kMaximumManifestSquads = 8;
/** Bits one manifest squad adds: its ClientRef. */
inline constexpr std::size_t kManifestSquadBits = fields::kClientRefBits;

/** Fixed bit and byte counts of each body. */
inline constexpr std::size_t kPathBits = 156;
inline constexpr std::size_t kPathBytes = 20;
inline constexpr std::size_t kActionBits = 254;
inline constexpr std::size_t kActionBytes = 32;
inline constexpr std::size_t kDeliveryBits = 132;
inline constexpr std::size_t kDeliveryMaximumBytes =
    (kDeliveryBits + kManifestSquadBits * (kMaximumManifestSquads - 1) + 7) / 8;
inline constexpr std::size_t kRetireBits = 77;
inline constexpr std::size_t kRetireBytes = 10;

/**
 * One experimental program, for sweeping the eight program kinds whose body class is still
 * unnamed (see RE-ACTOR-PROGRAMS.md). The ten decoded body shapes are all subsets of the
 * pieces below, written in this order, so one encoder covers every kind:
 * `word`, `floatBits`, reference, `marker` (8 bits), `bits6`, `bit`.
 * Nothing here is validated by the client beyond its bit count: a shape that does not match
 * the kind's real class is simply refused, which is exactly what makes the sweep informative.
 */
struct ProbeRequest final {
    std::uint32_t generation{};
    std::uint32_t revision{};
    std::uint32_t kind{};
    bool hasWord{};
    std::uint32_t word{};
    bool hasFloatBits{};
    std::uint32_t floatBits{};
    bool hasReference{};
    std::uint32_t registryKey{};
    std::uint32_t slotType{};
    std::uint16_t slotIndex{};
    bool hasMarker{};
    std::uint32_t marker{};
    bool hasBits6{};
    std::uint32_t bits6{};
    bool hasBit{};
    bool bit{};
};

/** One squad the delivery manifest names. */
struct SquadReference final {
    std::uint32_t registryKey{};
    std::uint16_t squadIndex{};
};

struct PathRequest final {
    std::uint32_t generation{};
    std::uint32_t revision{};
    std::uint32_t registryKey{};
    std::uint16_t pathIndex{};
    /**
     * The reference kind: an authored path (type 58, the shipped form) or a point set
     * (type 48), which the same resolver accepts — that is how one point becomes a movement
     * destination in a region with no authored path.
     */
    std::uint32_t slotType{kPathSlotType};
    /** Path marker (0 its start, 1 its destination) or, for a point set, the point index. */
    std::uint32_t marker{kPathDestinationMarker};
    /**
     * The body's trailing bit. Written as 1 for an authored type-58 path, where it reads as
     * "follow the authored curve". A point set has no curve, and live the actor only turned
     * toward the point and handed control back — so this is the first thing to try at 0.
     */
    bool followCurve{true};
};

struct ActionRequest final {
    std::uint32_t generation{};
    std::uint32_t revision{};
    std::uint32_t group{};
    std::uint32_t action{};
    /**
     * Optional spatial target: the ClientRef the action plays relative to, with its 3-bit mode
     * and 8-bit marker. `targetRegistryKey == 0` keeps the shipped no-target form (absent ref,
     * mode 0, marker -1). A present reference must be a point set (type 48, the marker is the
     * point index) or an authored path (type 58, marker 0 its start and 1 its destination):
     * those are the only two types the native decoder resolves, and the type-43 scene slot and
     * type-1 squad slot tried live stalled the client because the handler consumed the failed
     * handle. See `spatial_reference_slot_type`.
     */
    std::uint32_t targetRegistryKey{};
    std::uint32_t targetSlotType{};
    std::uint16_t targetSlotIndex{};
    std::uint32_t targetMode{};
    std::uint32_t targetMarker{kActionNoTargetMarker};
    /**
     * Root field .3, the cell's enabled bit. Every shipped program is sent enabled; `false` on
     * a fresh generation is the candidate for removing a program-created actor (the shipped
     * encoders never wrote it, so its client effect is established live).
     */
    bool enabled{true};
};

/** @return True when a 31-bit counter is positive and representable. */
[[nodiscard]] constexpr bool valid_counter(std::uint32_t value) noexcept {
    return value != 0 && value <= fields::kMaximumCounter;
}

/** Writes root fields .0 to .3: the spawn generation, mode one, marker one and the enabled bit. */
[[nodiscard]] inline bool
write_root(encoding::bits::Writer& writer, std::uint32_t generation, bool enabled) noexcept {
    const std::array<fields::Field, 5> root{{
        {1, fields::kPresenceWidth},
        {generation, fields::kCounterWidth},
        {kRootModeValue, kRootModeWidth},
        {kRootMarkerValue, kRootMarkerWidth},
        {enabled ? 1U : 0U, fields::kBoolWidth},
    }};
    return fields::write_fields(writer, root);
}

/** Writes the .6 program header up to and including its kind and completion selector. */
[[nodiscard]] inline bool write_program_header(encoding::bits::Writer& writer,
                                               std::uint32_t revision,
                                               std::uint32_t kind) noexcept {
    const std::array<fields::Field, 9> header{{
        {0, fields::kPresenceWidth}, // .4 absent
        {0, fields::kPresenceWidth}, // .5 absent
        {1, fields::kPresenceWidth}, // .6 present
        {revision, fields::kCounterWidth},
        {kProgramHeaderFirst, kProgramHeaderWidth},
        {kProgramHeaderSecond, kProgramHeaderWidth},
        {1, fields::kPresenceWidth},
        {kind + kProgramKindBias, kProgramKindWidth},
        {kCompletionValue, kCompletionWidth},
    }};
    return fields::write_fields(writer, header);
}

/**
 * Encodes one movement program. The spawn generation stays, the program revision advances.
 * @param output Exactly kPathBytes.
 * @return False on an out-of-range counter or index, or a size mismatch.
 */
[[nodiscard]] inline bool encode_path(const PathRequest& request,
                                      std::span<std::byte> output) noexcept {
    if (output.size() != kPathBytes || !valid_counter(request.generation)
        || !valid_counter(request.revision) || request.registryKey == 0
        || request.pathIndex > fields::kMaximumClientRefIndex
        || !spatial_reference_slot_type(request.slotType)
        || request.marker >= (1U << kPathMarkerWidth)) {
        return false;
    }
    encoding::bits::Writer writer(output);
    std::size_t written = 0;
    const std::array<fields::Field, 3> tail{{
        {request.marker, kPathMarkerWidth},
        {request.followCurve ? 1U : 0U, fields::kBoolWidth},
        {0, fields::kPresenceWidth}, // .7 absent
    }};
    return write_root(writer, request.generation, true)
           && write_program_header(writer, request.revision, kPathProgramKind)
           && fields::write_client_ref(
               writer, request.registryKey, request.slotType, request.pathIndex)
           && fields::write_fields(writer, tail)
           && fields::finish_exact(writer, kPathBits, kPathBytes, written);
}

/** Widest probe body: the action shape plus slack. */
inline constexpr std::size_t kProbeMaximumBytes = 32;
/** Program kinds the client dispatches, read from its jump table at rva `0xa977cc`. */
inline constexpr std::uint32_t kMaximumProgramKind = 9;

/**
 * Encodes one experimental program.
 * @param written Receives the byte count. @param bits Receives the meaningful bit count.
 * @return False on an out-of-range counter, kind or piece, or when the body does not close.
 */
/**
 * @return True when the pieces set on `request` are the body shape that kind is known to take.
 * A body of the wrong bit length does not get refused by the client: it misparses and stalls the
 * main loop (kind 8 with one extra bit did exactly that, live). So every kind whose shape we
 * know is enforced here, and an unknown kind may only be probed one shape per game run.
 */
[[nodiscard]] constexpr bool probe_shape_allowed(const ProbeRequest& request) noexcept {
    const bool refAndMarker = request.hasReference && request.hasMarker && !request.hasWord
                              && !request.hasFloatBits && !request.hasBits6;
    switch (request.kind) {
    case kPathProgramKind: // 3: ClientRef, marker, follow bit -- use encode_path instead
        return refAndMarker && request.hasBit;
    case 8: // ClientRef, marker: verified live, and one extra bit stalls the client
        return refAndMarker && !request.hasBit;
    case kActionProgramKind: // 9: the action shape has its own encoder
        return false;
    default:
        return true;
    }
}

[[nodiscard]] inline bool encode_probe(const ProbeRequest& request,
                                       std::span<std::byte> output,
                                       std::size_t& written,
                                       std::size_t& bits) noexcept {
    if (output.size() < kProbeMaximumBytes || !valid_counter(request.generation)
        || !valid_counter(request.revision) || request.kind > kMaximumProgramKind
        || (request.hasMarker && request.marker >= (1U << kPathMarkerWidth))
        || (request.hasBits6 && request.bits6 >= (1U << 6))
        || request.slotIndex > fields::kMaximumClientRefIndex
        || (request.hasReference && !spatial_reference_slot_type(request.slotType))
        || !probe_shape_allowed(request)) {
        return false;
    }
    encoding::bits::Writer writer(output);
    if (!write_root(writer, request.generation, true)
        || !write_program_header(writer, request.revision, request.kind)) {
        return false;
    }
    if (request.hasWord && !writer.write(request.word, 32)) {
        return false;
    }
    if (request.hasFloatBits && !writer.write(request.floatBits, 32)) {
        return false;
    }
    if (request.hasReference
        && !fields::write_client_ref(
            writer, request.registryKey, request.slotType, request.slotIndex)) {
        return false;
    }
    if (request.hasMarker && !writer.write(request.marker, kPathMarkerWidth)) {
        return false;
    }
    if (request.hasBits6 && !writer.write(request.bits6, 6)) {
        return false;
    }
    if (request.hasBit && !writer.write(request.bit ? 1U : 0U, fields::kBoolWidth)) {
        return false;
    }
    if (!writer.write(0, fields::kPresenceWidth)) { // .7 absent
        return false;
    }
    bits = writer.bit_count();
    return writer.finish(written) && fields::bytes_match_bits(written, bits);
}

/**
 * Encodes one custom action program, with or without a spatial target (see ActionRequest).
 * @param output Exactly kActionBytes.
 * @return False on an out-of-range counter, a zero or no-name action, an out-of-range target
 * field, or a size mismatch.
 */
[[nodiscard]] inline bool encode_action(const ActionRequest& request,
                                        std::span<std::byte> output) noexcept {
    if (output.size() != kActionBytes || !valid_counter(request.generation)
        || !valid_counter(request.revision) || request.action == 0
        || request.action == fields::kClientRefAbsentKey
        || request.targetMode >= (1U << kActionTargetModeWidth)
        || request.targetMarker >= (1U << kActionTargetMarkerWidth)
        || request.targetSlotIndex > fields::kMaximumClientRefIndex
        || (request.targetRegistryKey != 0
            && !spatial_reference_slot_type(request.targetSlotType))) {
        return false;
    }
    encoding::bits::Writer writer(output);
    std::size_t written = 0;
    const std::array<fields::Field, 3> identities{{
        {request.group, 32},
        {request.action, 32},
        {fields::kClientRefAbsentKey, 32}, // no additional identity
    }};
    const std::array<fields::Field, 3> tail{{
        {request.targetMode, kActionTargetModeWidth},
        {request.targetMarker, kActionTargetMarkerWidth},
        {0, fields::kPresenceWidth}, // .7 absent
    }};
    // Evaluated in sequence with the other writes: the target sits after the identities.
    const auto writeTarget = [&]() noexcept {
        return request.targetRegistryKey == 0
                   ? fields::write_absent_client_ref(writer)
                   : fields::write_client_ref(writer,
                                              request.targetRegistryKey,
                                              request.targetSlotType,
                                              request.targetSlotIndex);
    };
    return write_root(writer, request.generation, request.enabled)
           && write_program_header(writer, request.revision, kActionProgramKind)
           && fields::write_fields(writer, identities) && writeTarget()
           && fields::write_fields(writer, tail)
           && fields::finish_exact(writer, kActionBits, kActionBytes, written);
}

/**
 * Encodes one delivery manifest: the reserved squads the actor carries. Field .6 stays absent
 * so a delivery never replaces the movement program.
 * @param output At least the manifest's byte count.
 * @param written Receives the byte count. @param bits Receives the meaningful bit count.
 * @return False on an empty, oversized or repeated manifest, or an out-of-range counter.
 */
[[nodiscard]] inline bool encode_delivery(std::uint32_t generation,
                                          std::uint32_t revision,
                                          std::span<const SquadReference> squads,
                                          std::span<std::byte> output,
                                          std::size_t& written,
                                          std::size_t& bits) noexcept {
    written = 0;
    bits = 0;
    if (!valid_counter(generation) || !valid_counter(revision) || squads.empty()
        || squads.size() > kMaximumManifestSquads) {
        return false;
    }
    for (std::size_t index = 0; index < squads.size(); ++index) {
        if (squads[index].registryKey == 0
            || squads[index].squadIndex > fields::kMaximumClientRefIndex) {
            return false;
        }
        for (std::size_t prior = 0; prior < index; ++prior) {
            if (squads[index].registryKey == squads[prior].registryKey
                && squads[index].squadIndex == squads[prior].squadIndex) {
                return false;
            }
        }
    }
    const std::size_t expectedBits = kDeliveryBits + kManifestSquadBits * (squads.size() - 1);
    const std::size_t expectedBytes = (expectedBits + 7) / 8;
    if (output.size() < expectedBytes) {
        return false;
    }
    encoding::bits::Writer writer(output.first(expectedBytes));
    const std::array<fields::Field, 5> manifestHeader{{
        {0, fields::kPresenceWidth}, // .4 absent
        {0, fields::kPresenceWidth}, // .5 absent
        {0, fields::kPresenceWidth}, // .6 absent
        {1, fields::kPresenceWidth}, // .7 present
        {squads.size(), kManifestCountWidth},
    }};
    if (!write_root(writer, generation, true) || !fields::write_fields(writer, manifestHeader)) {
        return false;
    }
    for (const SquadReference& squad : squads) {
        if (!fields::write_client_ref(writer,
                                      squad.registryKey,
                                      static_cast<std::uint32_t>(squad_auth::kSlotType),
                                      squad.squadIndex)) {
            return false;
        }
    }
    if (!writer.write(revision, fields::kCounterWidth)
        || !fields::finish_exact(writer, expectedBits, expectedBytes, written)) {
        return false;
    }
    bits = expectedBits;
    return true;
}

/**
 * Encodes one retirement: root .3 disabled, an empty delivery manifest and the delivery
 * revision set to the generation. The client retires the actor on this new generation.
 * @param output Exactly kRetireBytes.
 */
[[nodiscard]] inline bool encode_retire(std::uint32_t generation,
                                        std::span<std::byte> output) noexcept {
    if (output.size() != kRetireBytes || !valid_counter(generation)) {
        return false;
    }
    encoding::bits::Writer writer(output);
    std::size_t written = 0;
    const std::array<fields::Field, 6> tail{{
        {0, fields::kPresenceWidth}, // .4 absent
        {0, fields::kPresenceWidth}, // .5 absent
        {0, fields::kPresenceWidth}, // .6 absent
        {1, fields::kPresenceWidth}, // .7 present
        {0, kManifestCountWidth},    // no passengers
        {generation, fields::kCounterWidth},
    }};
    return write_root(writer, generation, false) && fields::write_fields(writer, tail)
           && fields::finish_exact(writer, kRetireBits, kRetireBytes, written);
}

} // namespace sunrise::middleware::bap::activity_message::combatant_auth
