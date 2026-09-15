#include "../../encoding/bit_reader.h"
#include "scriptable_auth_body.h"
#include "sensor_auth_update.h"

namespace sunrise::middleware::bap::activity_message::sensor_auth_update {

/** Development diagnostic: names the `valid()` check that refused the last snapshot. */
const char* g_lastValidationFailure = nullptr;

const char* last_validation_failure() noexcept {
    return g_lastValidationFailure;
}

namespace {

/** Records which check refused, so a refusal is diagnosable from the caller's log. */
[[nodiscard]] bool refuse(const char* reason) noexcept {
    g_lastValidationFailure = reason;
    return false;
}

namespace bits = encoding::bits;

/** The type-13 slot type, which is the only one that may carry the player key. */
constexpr std::uint8_t kSlotTypeParticipation = 13;
/** The participation region rides a signed field, so this is the widest index it accepts. */
constexpr std::uint32_t kMaximumRegion = 0x7FFFFFFF;

/** @return True when the packed counts and body width close one complete type-43 shape. */
/**
 * @return True when an SDK-compiled body has one exact, non-truncated physical extent.
 * This arm pins no slot type and no schema, so it refuses every slot type with a faulting lane.
 */
/**
 * Checks the per-bubble sub-blocks against what the client's own arrays hold.
 * An empty sub-block would publish a zero count and a zero mask, which registers nothing and
 * spends a slot, so it is refused rather than encoded.
 * @param subBlocks Sub-blocks the body would carry.
 * @return True when every sub-block names a usable bubble and a bounded key set.
 */
[[nodiscard]] bool valid_sub_blocks(std::span<const BubbleSubBlock> subBlocks) noexcept {
    if (subBlocks.size() > kBubbleSubBlockCapacity) {
        return refuse("sub_block_count");
    }
    for (std::size_t index = 0; index < subBlocks.size(); ++index) {
        const BubbleSubBlock& block = subBlocks[index];
        if (block.bubble > kMaximumSubBlockBubble || block.keys.empty()
            || block.keys.size() > kBubbleKeyCapacity) {
            return refuse("sub_block_shape");
        }
        // The client's array holds one element per bubble, and its sweep walks every element that
        // matches. A repeated bubble would register the same keys twice in one apply.
        for (std::size_t earlier = 0; earlier < index; ++earlier) {
            if (subBlocks[earlier].bubble == block.bubble) {
                return false;
            }
        }
        for (std::size_t key = 0; key < block.keys.size(); ++key) {
            for (std::size_t earlier = 0; earlier < key; ++earlier) {
                if (block.keys[earlier] == block.keys[key]) {
                    return false;
                }
            }
        }
    }
    return true;
}

/** Adds one active group's records without crossing the client's fixed record pool. */
[[nodiscard]] bool add_client_records(std::size_t slots, std::size_t& total) noexcept {
    if (total > kClientRecordCapacity || slots > kClientRecordCapacity - total) {
        return false;
    }
    total += slots;
    return true;
}

/**
 * Checks every possible manager set: the ungated top-level groups plus one bubble sub-block.
 * Inactive bubble groups share snapshot storage but never share ClientRef manager capacity.
 */
[[nodiscard]] bool valid_client_sets(const Roster& roster) noexcept {
    if (roster.topLevelGroupCount > kClientGroupCapacity) {
        return refuse("top_level_capacity");
    }
    std::size_t topLevelRecords = 0;
    for (std::size_t index = 0; index < roster.topLevelGroupCount; ++index) {
        if (roster.groups[index].retired
            || !add_client_records(roster.groups[index].slotTypes.size(), topLevelRecords)) {
            return refuse("top_level_records");
        }
    }
    std::array<bool, kPublishedGroupCapacity> referenced{};
    for (const BubbleSubBlock& block : roster.bubbleSubBlocks) {
        std::size_t activeGroups = roster.topLevelGroupCount;
        std::size_t activeRecords = topLevelRecords;
        for (const std::uint32_t key : block.keys) {
            std::size_t matched = roster.groupCount;
            for (std::size_t index = roster.topLevelGroupCount; index < roster.groupCount;
                 ++index) {
                const Group& group = roster.groups[index];
                if (group.key != key) {
                    continue;
                }
                if (matched != roster.groupCount) {
                    return refuse("subkey_ambiguous");
                }
                matched = index;
            }
            if (matched == roster.groupCount) return refuse("subkey_unmatched");
            if (!roster.groups[matched].retired
                && (++activeGroups > kClientGroupCapacity
                    || !add_client_records(roster.groups[matched].slotTypes.size(), activeRecords)))
                return refuse("client_set_capacity");
            referenced[matched] = true;
        }
    }
    for (std::size_t index = roster.topLevelGroupCount; index < roster.groupCount; ++index) {
        if (!referenced[index]) {
            return refuse("group_unreferenced");
        }
    }
    return true;
}

/**
 * Checks the scalars whose out-of-range values would encode with no complaint.
 * @param snapshot Message input.
 * @return True when every scalar fits its field.
 */
[[nodiscard]] bool valid(const Snapshot& snapshot) noexcept {
    if (snapshot.lifetime > kMaximumLifetimeState) {
        return refuse("lifetime");
    }
    if (snapshot.hasRegion && snapshot.region > kMaximumRegion) {
        return refuse("region");
    }
    if (snapshot.hasSpawnOverride
        && (snapshot.spawnSliceSet > kMaximumSpawnSliceSet || snapshot.spawnSetHash == 0
            || snapshot.spawnSetHash == kAbsentSpawnSetHash)) {
        return refuse("spawn_override");
    }
    if (snapshot.stateSequence > kMaximumStateSequence) {
        return refuse("state_sequence");
    }
    // The grant is a change, not a value: the client compares it against a mirror that starts at
    // zero, so a token of zero grants nothing.
    if (snapshot.hasGrant
        && (snapshot.grant.bubble > kMaximumGrantBubble
            || snapshot.grant.token < kMinimumGrantToken)) {
        return refuse("grant");
    }
    if (snapshot.roster.groupCount > kPublishedGroupCapacity
        || snapshot.roster.topLevelGroupCount > snapshot.roster.groupCount
        || snapshot.roster.topLevelGroupCount > kTopLevelGroupCapacity
        || snapshot.authOverrides.size() > kAuthOverrideCapacity
        || !valid_sub_blocks(snapshot.roster.bubbleSubBlocks)) {
        return g_lastValidationFailure == nullptr ? refuse("roster_capacity") : false;
    }
    for (std::size_t group = 0; group < snapshot.roster.groupCount; ++group) {
        const Group& row = snapshot.roster.groups[group];
        if (row.slotTypes.size() != row.slotFlags.size()
            || row.slotTypes.size() != row.slotIndices.size() || row.slotTypes.empty()
            || (row.hasStateSequence && row.stateSequence > kMaximumStateSequence)) {
            return refuse("group_slots");
        }
        for (std::size_t earlier = 0; earlier < group; ++earlier) {
            if (snapshot.roster.groups[earlier].key == row.key) {
                return refuse("group_key_repeat");
            }
        }
        // An index past the field's range wraps into another slot's, which seeds the wrong
        // object rather than refusing.
        for (const std::uint16_t index : row.slotIndices) {
            if (index > kMaximumSlotIndex) {
                return refuse("slot_index");
            }
        }
    }
    for (std::size_t index = 0; index < snapshot.authOverrides.size(); ++index) {
        const AuthOverride& value = snapshot.authOverrides[index];
        const std::size_t requiredBytes = (value.bitCount + 7U) / 8U;
        // A body's shape is not re-derived here: its own encoder owns it, and a second copy of
        // that sum drifted once and refused every squad body Sunrise sent. Only the row's own
        // consistency is checked, which no encoder owns.
        if (!value.present || value.byteCount != requiredBytes
            || requiredBytes > value.body.size()) {
            return refuse("auth_body");
        }
        for (std::size_t earlier = 0; earlier < index; ++earlier) {
            const AuthOverride& prior = snapshot.authOverrides[earlier];
            if (prior.objectTag == value.objectTag && prior.key == value.key
                && prior.slotType == value.slotType && prior.slotIndex == value.slotIndex) {
                return refuse("auth_override_repeat");
            }
        }
        std::size_t matches = 0;
        for (std::size_t group = 0; group < snapshot.roster.groupCount; ++group) {
            const Group& row = snapshot.roster.groups[group];
            if (row.objectTag != value.objectTag || row.key != value.key) {
                continue;
            }
            for (std::size_t slot = 0; slot < row.slotTypes.size(); ++slot) {
                if (row.slotTypes[slot] == value.slotType
                    && row.slotIndices[slot] == value.slotIndex
                    && (row.slotFlags[slot] & kSlotAuthFlag) != 0) {
                    ++matches;
                }
            }
        }
        if (matches != 1) {
            return refuse("auth_override_unmatched");
        }
    }
    if (snapshot.senseOverrides.size() > kAuthOverrideCapacity) {
        return refuse("sense_capacity");
    }
    for (std::size_t index = 0; index < snapshot.senseOverrides.size(); ++index) {
        const SenseOverride& value = snapshot.senseOverrides[index];
        if (value.bitCount == 0 || value.byteCount != (value.bitCount + 7U) / 8U
            || value.byteCount > value.body.size()) {
            return refuse("sense_body");
        }
        for (std::size_t earlier = 0; earlier < index; ++earlier) {
            const SenseOverride& prior = snapshot.senseOverrides[earlier];
            if (prior.objectTag == value.objectTag && prior.key == value.key
                && prior.slotType == value.slotType && prior.slotIndex == value.slotIndex) {
                return refuse("sense_override_repeat");
            }
        }
        std::size_t matches = 0;
        for (const Group& group :
             std::span(snapshot.roster.groups).first(snapshot.roster.groupCount)) {
            if (group.key != value.key || group.objectTag != value.objectTag) {
                continue;
            }
            for (std::size_t slot = 0; slot < group.slotTypes.size(); ++slot) {
                if (group.slotTypes[slot] == value.slotType
                    && group.slotIndices[slot] == value.slotIndex
                    && (group.slotFlags[slot] & kSlotSenseFlag) != 0) {
                    ++matches;
                }
            }
        }
        if (matches != 1) {
            return refuse("sense_override_unmatched");
        }
    }
    return valid_client_sets(snapshot.roster);
}

/**
 * Writes every group's object blocks, in publish order, per-bubble groups included.
 * Every registered object must be seeded before any auth state applies, because the client's gate
 * walks the whole sync-record pool. A partial message seeds nothing that applies.
 * @param writer Body writer sitting after the phase-1 delta.
 * @param snapshot Message input.
 * @return True when every block fits.
 */
[[nodiscard]] bool write_phase_two(bits::Writer& writer, const Snapshot& snapshot) noexcept {
    bool encoded = true;
    bool keyPlaced = false;
    for (std::size_t group = 0; encoded && group < snapshot.roster.groupCount; ++group) {
        const Group& row = snapshot.roster.groups[group];
        if (row.retired) continue;
        // The filler word after the key is read and discarded.
        encoded = writer.write(1, kPresenceWidth) && writer.write(row.key, kKeyWidth)
                  && writer.write(0, kKeyWidth);
        for (std::size_t slot = 0; encoded && slot < row.slotTypes.size(); ++slot) {
            const std::uint8_t slotType = row.slotTypes[slot];
            const bool firstOrEvery = !keyPlaced || snapshot.keyOnEveryParticipationSlot;
            const bool carriesPlayerKey = slotType == kSlotTypeParticipation
                                          && row.key == snapshot.roster.playerKeyGroup
                                          && firstOrEvery;
            keyPlaced = keyPlaced || carriesPlayerKey;
            encoded = write_object_block(writer,
                                         snapshot,
                                         row.objectTag,
                                         row.key,
                                         slotType,
                                         row.slotIndices[slot],
                                         row.slotFlags[slot],
                                         row.missionSeedOnly,
                                         carriesPlayerKey);
        }
        encoded = encoded && writer.write(0, kPresenceWidth);
    }
    return encoded;
}

/**
 * Writes the whole body through one writer.
 * @param writer Real or measuring writer positioned at the first bit.
 * @param snapshot Message input.
 * @return True when every field fit.
 */
[[nodiscard]] bool write_body(bits::Writer& writer, const Snapshot& snapshot) noexcept {
    // The hardwipe token is unchecked unless the client's `use_hardwipe_tokens` config is on.
    bool encoded = writer.write(0, kHardwipeWidth)
                   && writer.write(snapshot.patchEpoch.first, kEpochWidth)
                   && writer.write(snapshot.patchEpoch.second, kEpochWidth)
                   && writer.write(snapshot.hasGrant ? 1U : 0U, kPresenceWidth);
    if (encoded && snapshot.hasGrant) {
        encoded = write_bubble_block(writer, snapshot.grant);
    }
    const std::size_t latchBit = kLatchBitWithoutGrant + (snapshot.hasGrant ? kBubbleBlockBits : 0);
    // The token at the activity object's element 10 is not checked.
    encoded = encoded && writer.write(0, kActivityTokenWidth) && writer.bit_count() == latchBit;
    // The enable latch is not sticky, so it goes on every message.
    encoded = encoded && writer.write(1, kPresenceWidth)
              && write_roster_delta(writer, snapshot.roster, snapshot.stateSequence)
              && writer.bit_count()
                     == latchBit + 1
                            + delta_bits(snapshot.roster.topLevelGroupCount,
                                         snapshot.roster.bubbleSubBlocks);
    if (encoded && !snapshot.phaseOneOnly) {
        encoded = write_phase_two(writer, snapshot);
    }
    // The entity-group loop end, then the trailing pair, which short-circuits to one bit.
    return encoded && writer.write(0, kPresenceWidth) && writer.write(0, kPresenceWidth);
}

} // namespace

/** Encodes one `sensor_auth_update` body. */
bool encode_sensor_auth_update(const Snapshot& snapshot,
                               std::span<std::byte> output,
                               std::size_t& written) noexcept {
    written = 0;
    g_lastValidationFailure = "unreported";
    if (output.empty()) {
        return refuse("output_empty");
    }
    if (!valid(snapshot)) {
        return false;
    }

    // Measure first. The writer clears and fills the caller's storage as it goes, so a body that
    // does not fit would leave a partial one behind.
    bits::Writer measure = bits::Writer::measuring();
    std::size_t required = 0;
    if (!write_body(measure, snapshot)) {
        return refuse("measure_write");
    }
    if (!measure.finish(required)) {
        return refuse("measure_finish");
    }
    if (required > output.size()) {
        return refuse("body_too_large");
    }

    bits::Writer writer(output);
    std::size_t produced = 0;
    if (!write_body(writer, snapshot)) {
        return refuse("write");
    }
    if (!writer.finish(produced)) {
        return refuse("write_finish");
    }
    if (produced != required) {
        return refuse("size_mismatch");
    }
    written = produced;
    return true;
}

} // namespace sunrise::middleware::bap::activity_message::sensor_auth_update
