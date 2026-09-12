#include <algorithm>
#include <cstdint>

#include "../../encoding/bit_reader.h"
#include "../../encoding/bit_writer.h"
#include "player_trigger_incident.h"

namespace sunrise::middleware::bap::activity_message::player_trigger_incident {
namespace {

namespace bits = encoding::bits;

/** Bit offset of the client reference inside the incident body. */
constexpr std::size_t kClientReferenceOffset = 335;
/** Field widths of the client reference and the trigger tail, in bits. */
constexpr std::uint8_t kRegistryKeyBits = 32;
constexpr std::uint8_t kSlotTypeBits = 7;
constexpr std::uint8_t kSlotIndexBits = 16;
constexpr std::uint8_t kResolvedObjectBits = 32;
constexpr std::uint8_t kPaddingBits = 2;
/** The slot type and index are sent unsigned, biased so their absent value is zero. */
constexpr std::int32_t kSlotTypeBias = 1;
constexpr std::int32_t kSlotIndexBias = 32'768;

} // namespace

/**
 * Decodes the fixed 422-bit payload; the two trailing padding bits must be zero.
 * @param input Exact payload bytes.
 * @param output Cleared first. Receives the ClientRef and resolved target id.
 * @return True when the whole payload decoded.
 */
bool decode(std::span<const std::byte> input, Payload& output) noexcept {
    output = {};
    if (input.size() != kPayloadBytes) {
        return false;
    }

    bits::Reader reader(input);
    std::uint64_t registryKey = 0;
    std::uint64_t slotType = 0;
    std::uint64_t slotIndex = 0;
    std::uint64_t resolvedObjectId = 0;
    std::uint64_t padding = 0;
    if (!reader.skip(kClientReferenceOffset) || !reader.read(kRegistryKeyBits, registryKey)
        || !reader.read(kSlotTypeBits, slotType) || !reader.read(kSlotIndexBits, slotIndex)
        || !reader.read(kResolvedObjectBits, resolvedObjectId)
        || !reader.read(kPaddingBits, padding) || padding != 0 || reader.remaining_bits() != 0) {
        return false;
    }

    Payload parsed{};
    parsed.registryKey = static_cast<std::uint32_t>(registryKey);
    parsed.slotType = static_cast<std::int8_t>(static_cast<std::int32_t>(slotType) - kSlotTypeBias);
    parsed.slotIndex =
        static_cast<std::int16_t>(static_cast<std::int32_t>(slotIndex) - kSlotIndexBias);
    parsed.resolvedObjectId = static_cast<std::uint32_t>(resolvedObjectId);
    output = parsed;
    return true;
}

/**
 * Encodes the fixed 422-bit payload; the leading offset and trailing two bits are zero.
 * @param input The ClientRef and resolved target id to encode.
 * @param output Exactly `kPayloadBytes` bytes, cleared first.
 * @return True when every field fit its wire width and the buffer size matched.
 */
bool encode(const Payload& input, std::span<std::byte> output) noexcept {
    if (output.size() != kPayloadBytes) {
        return false;
    }
    for (std::byte& byte : output) {
        byte = std::byte{0};
    }

    const std::int32_t biasedSlotType = static_cast<std::int32_t>(input.slotType) + kSlotTypeBias;
    const std::int32_t biasedSlotIndex = static_cast<std::int32_t>(input.slotIndex) + kSlotIndexBias;
    if (biasedSlotType < 0 || biasedSlotType > ((1 << kSlotTypeBits) - 1) || biasedSlotIndex < 0
        || biasedSlotIndex > ((1 << kSlotIndexBits) - 1)) {
        return false;
    }

    bits::Writer writer(output);
    std::size_t skipped = 0;
    while (skipped < kClientReferenceOffset) {
        const std::uint8_t chunk =
            static_cast<std::uint8_t>((std::min)(kClientReferenceOffset - skipped, std::size_t{32}));
        if (!writer.write(0, chunk)) {
            return false;
        }
        skipped += chunk;
    }
    if (!writer.write(input.registryKey, kRegistryKeyBits)
        || !writer.write(static_cast<std::uint64_t>(biasedSlotType), kSlotTypeBits)
        || !writer.write(static_cast<std::uint64_t>(biasedSlotIndex), kSlotIndexBits)
        || !writer.write(input.resolvedObjectId, kResolvedObjectBits)
        || !writer.write(0, kPaddingBits)) {
        return false;
    }
    std::size_t written = 0;
    return writer.finish(written) && written == kPayloadBytes;
}

} // namespace sunrise::middleware::bap::activity_message::player_trigger_incident
