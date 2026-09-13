#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "../../encoding/bit_writer.h"
#include "auth_fields.h"

/**
 * Type-26 "hop on" sensors (`ho_*` in the authored data). The package uses them to impose a
 * looping state on an actor that is already in the world -- Homecoming's plaza has
 * `ho_zavala_looping_bunker_anim`, `ho_zavala_looping_angry_emotion` and `ho_bubble_shield`,
 * and the bazaar has `ho_noweapon_the_path` and `ho_no_combat_abilities` for the player.
 *
 * The Auth body is class `0x8080954B`, decoded from the client's own reflection database
 * (`tools/reflect_dump.py rec 8080954b`): two booleans, four signed 32-bit values, one
 * ClientRef, then a trailing dynamic field. Its widths add up exactly to the slot metadata's
 * declared 186..276 bits -- 1 + 1 + 4*32 + 55 = 185, so the trailing field is one bit when it
 * carries nothing, which is the form this encoder writes. That check is what makes the layout
 * trustworthy without a single live send: a body of the wrong length does not get refused by
 * the client, it misparses and stalls the main loop.
 *
 * What the four values and two flags *mean* is not established. They are therefore passed
 * through verbatim from the script, so the meaning can be swept live without a rebuild.
 */
namespace sunrise::middleware::bap::activity_message::hop_on_auth {

namespace fields = auth_fields;

/** Authored slot type and the exact schema this encoder writes. */
inline constexpr std::uint8_t kSlotType = 26;
inline constexpr std::uint32_t kComponentClass = 0x8080953FU;
inline constexpr std::uint32_t kSchema = 0x8080954BU;
/** Signed 32-bit schema fields store zero at the middle of the unsigned wire range. */
inline constexpr std::uint8_t kValueWidth = 32;
inline constexpr std::size_t kValueCount = 4;
/** The trailing dynamic field, written empty. */
inline constexpr std::uint8_t kTrailingWidth = 1;
/** 1 + 1 + 4*32 + 55 + 1, the slot metadata's own declared minimum. */
inline constexpr std::size_t kBitCount = 186;
inline constexpr std::size_t kByteCount = (kBitCount + 7) / 8;

/** One hop-on control. `targetRegistryKey == 0` writes the unset reference. */
struct Request final {
    bool first{};
    bool second{};
    std::array<std::int32_t, kValueCount> values{};
    std::uint32_t targetRegistryKey{};
    std::uint32_t targetSlotType{};
    std::uint16_t targetSlotIndex{};
};

/**
 * Encodes one hop-on body.
 * @param output Exactly kByteCount.
 * @return False on an out-of-range reference index or a size mismatch.
 */
[[nodiscard]] inline bool encode(const Request& request, std::span<std::byte> output) noexcept {
    if (output.size() != kByteCount
        || request.targetSlotIndex > fields::kMaximumClientRefIndex) {
        return false;
    }
    encoding::bits::Writer writer(output);
    std::size_t written = 0;
    if (!writer.write(request.first ? 1U : 0U, fields::kBoolWidth)
        || !writer.write(request.second ? 1U : 0U, fields::kBoolWidth)) {
        return false;
    }
    for (const std::int32_t value : request.values) {
        const std::uint32_t biased =
            static_cast<std::uint32_t>(value) + fields::kSigned32Bias;
        if (!writer.write(biased, kValueWidth)) {
            return false;
        }
    }
    const bool reference =
        request.targetRegistryKey == 0
            ? fields::write_absent_client_ref(writer)
            : fields::write_client_ref(
                writer, request.targetRegistryKey, request.targetSlotType, request.targetSlotIndex);
    return reference && writer.write(0, kTrailingWidth)
           && fields::finish_exact(writer, kBitCount, kByteCount, written);
}

} // namespace sunrise::middleware::bap::activity_message::hop_on_auth
