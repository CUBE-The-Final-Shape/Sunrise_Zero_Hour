#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace sunrise::client::hooks::content_resolver {

/** Resolves the game's own content-hash resolver once. Safe to call repeatedly. */
void install() noexcept;

/**
 * Copies the definition blob for `hash` into `output`, never reading past the end of the
 * committed memory region the resolver points into.
 * @return The number of bytes copied, or zero when the hash does not resolve.
 */
[[nodiscard]] std::size_t resolve(std::uint32_t hash, std::span<std::byte> output) noexcept;

/** Marks the class-id band; every value in it names a package class, never a content tag. */
inline constexpr std::uint32_t kClassBand = 0x80800000U;
inline constexpr std::uint32_t kClassBandMask = 0xFFFF0000U;
/** Authored-scene external event-gate node class: the one that carries FNV-1 event keys. */
inline constexpr std::uint32_t kEventGateNodeClass = 0x8080637DU;

/**
 * Development diagnostic: writes the blob for `hash`, and for every content tag it carries, down
 * to `depth` levels, into `hashdump\hash_XXXXXXXX.bin` in the Sunrise artifact directory. Each is
 * logged with its size and whether it contains the event-gate node class, so the authored scene's
 * graph tag can be identified without opening a single file. Not a stable API.
 * @return The number of blobs written.
 */
std::size_t dump_tree(std::uint32_t hash, std::uint32_t depth) noexcept;

/**
 * Scans the blob for `hash` (an authored scene's graph tag) for every `kEventGateNodeClass`
 * marker dword and reads the FNV-1 event key sitting exactly 12 bytes after each one. This is
 * where a type-43 scene's external event-gate keys actually live: NOT behind the graph header's
 * declared array offset/count for that node class (which is empty/zero even when the header still
 * declares N such nodes), but inline in the graph body, one 0x60-byte node per event, key at
 * marker+12. Confirmed against a public reference implementation's own test fixture, which reads
 * a known scene's graph this exact way and recovers its four documented event names' hashes.
 * The scan is bounded to the record (its own uint32 length at offset 0), not to the mapped
 * window the resolver points into, and skips zero, 0xFFFFFFFF and class-band values -- the
 * header's own declaration entry for the node class would otherwise read as a key of 0.
 * @param outKeys Receives one entry per event-gate node found, up to its capacity.
 * @return The number of keys found (may exceed outKeys.size(), which then only holds the first
 * outKeys.size() of them).
 */
std::size_t find_event_gate_keys(std::uint32_t hash, std::span<std::uint32_t> outKeys) noexcept;

} // namespace sunrise::client::hooks::content_resolver
