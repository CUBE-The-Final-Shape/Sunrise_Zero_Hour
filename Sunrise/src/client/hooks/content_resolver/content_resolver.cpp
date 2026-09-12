#include "content_resolver.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <unordered_set>
#include <vector>

#include "../../../core/filesystem/path.h"
#include "../../../core/logging/log.h"

namespace sunrise::client::hooks::content_resolver {
namespace {

using TagResolver = const std::byte*(__fastcall*)(std::uint32_t) noexcept;

/**
 * The game's own generic hash-to-definition resolver. Known-stable for this build (RVA is a
 * property of the game binary, not of Sunrise): resolves both small object-placement tags and
 * full 32-bit content hashes in the 0x80xxxxxx shape through the same function.
 */
constexpr std::uintptr_t kTagResolverRva = 0x1258970;

/** Diagnostic bounds; one malformed or enormous blob must not fill the disk. */
constexpr std::size_t kMaximumBlobBytes = 256U * 1024U;
constexpr std::size_t kMaximumBlobs = 256;

std::atomic<TagResolver> g_resolver{nullptr};

/** @return The resolved blob pointer, or null when the hash is unknown or the call faults. */
[[nodiscard]] const std::byte* resolve_pointer(std::uint32_t hash) noexcept {
    TagResolver const resolver = g_resolver.load(std::memory_order_acquire);
    if (resolver == nullptr) {
        return nullptr;
    }
    __try {
        return resolver(hash);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

/** @return How many of `want` bytes at `address` stay inside one committed region. */
[[nodiscard]] std::size_t committed_length(const std::byte* address, std::size_t want) noexcept {
    MEMORY_BASIC_INFORMATION info{};
    if (address == nullptr || VirtualQuery(address, &info, sizeof(info)) != sizeof(info)
        || info.State != MEM_COMMIT || (info.Protect & PAGE_NOACCESS) != 0
        || (info.Protect & PAGE_GUARD) != 0) {
        return 0;
    }
    const auto* const regionEnd = static_cast<const std::byte*>(info.BaseAddress) + info.RegionSize;
    if (regionEnd <= address) {
        return 0;
    }
    return (std::min)(want, static_cast<std::size_t>(regionEnd - address));
}

[[nodiscard]] bool copy_guarded(const std::byte* source,
                                std::byte* destination,
                                std::size_t length) noexcept {
    __try {
        std::memcpy(destination, source, length);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

/** @return True when `value` names a content tag rather than a package class id. */
[[nodiscard]] bool is_content_tag(std::uint32_t value) noexcept {
    return value >= 0x80000000U && value != 0xFFFFFFFFU
           && (value & kClassBandMask) != kClassBand;
}

/**
 * @return True when `value` can be a real FNV-1 event key. Zero and 0xFFFFFFFF are the encoder's
 * own reserved values, and the class band is the graph header's own declaration entry for the
 * node class (marker followed by counts, not a node), which would otherwise be read as a key.
 */
[[nodiscard]] bool is_event_key(std::uint32_t value) noexcept {
    return value != 0U && value != 0xFFFFFFFFU && (value & kClassBandMask) != kClassBand;
}

/** Reads one blob into `bytes`, bounded by the committed region behind it. */
[[nodiscard]] bool read_blob(std::uint32_t hash, std::vector<std::byte>& bytes) noexcept {
    bytes.clear();
    const std::byte* const definition = resolve_pointer(hash);
    const std::size_t length = committed_length(definition, kMaximumBlobBytes);
    if (length == 0) {
        return false;
    }
    bytes.resize(length);
    if (!copy_guarded(definition, bytes.data(), length)) {
        bytes.clear();
        return false;
    }
    return true;
}

/**
 * The resolver hands back a pointer into a large mapped region, so a blob read is a window over
 * many consecutive records, not one record. Every record carries its own byte length as the
 * uint32 at its offset 0 (verified on scene graphs, resource entities and placed-object configs).
 * @return The length of the record at the start of `bytes`, clamped to what was actually read;
 * the whole window when the header is missing or implausible (zero, or smaller than itself).
 */
[[nodiscard]] std::size_t record_length(const std::vector<std::byte>& bytes) noexcept {
    if (bytes.size() < sizeof(std::uint32_t)) {
        return bytes.size();
    }
    std::uint32_t declared = 0;
    std::memcpy(&declared, bytes.data(), sizeof(declared));
    if (declared < sizeof(std::uint32_t)) {
        return bytes.size();
    }
    return (std::min)(static_cast<std::size_t>(declared), bytes.size());
}

void write_blob(std::uint32_t hash, const std::vector<std::byte>& bytes) noexcept {
    std::array<wchar_t, 64> relative{};
    static_cast<void>(
        std::swprintf(relative.data(), relative.size(), L"hashdump\\hash_%08X.bin", hash));
    core::path::Buffer path{};
    if (!core::path::artifact_file(relative.data(), path)) {
        return;
    }
    HANDLE const file = CreateFileW(
        path.chars.data(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return;
    }
    DWORD written = 0;
    static_cast<void>(WriteFile(
        file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr));
    static_cast<void>(CloseHandle(file));
}

/**
 * @return How many event-gate node markers the first `end` bytes of the blob hold, as a cheap
 * "is this the graph?" signal. Includes the header's declaration entry, so a graph that declares
 * gates but holds none still counts 1.
 */
[[nodiscard]] std::size_t count_event_gates(const std::vector<std::byte>& bytes,
                                            std::size_t end) noexcept {
    std::size_t count = 0;
    for (std::size_t offset = 0; offset + sizeof(std::uint32_t) <= end; offset += 4) {
        std::uint32_t value = 0;
        std::memcpy(&value, bytes.data() + offset, sizeof(value));
        if (value == kEventGateNodeClass) {
            ++count;
        }
    }
    return count;
}

/**
 * Bytes from an event-gate node's class marker to its FNV-1 event key. Nodes are 0x60 bytes
 * apart, but that stride is not relied on here -- every dword in the blob is checked instead.
 */
constexpr std::size_t kEventGateKeyOffset = 12;

void collect_children(const std::vector<std::byte>& bytes,
                      std::vector<std::uint32_t>& output) noexcept {
    for (std::size_t offset = 0; offset + sizeof(std::uint32_t) <= bytes.size(); offset += 4) {
        std::uint32_t value = 0;
        std::memcpy(&value, bytes.data() + offset, sizeof(value));
        if (is_content_tag(value)) {
            output.push_back(value);
        }
    }
}

} // namespace

void install() noexcept {
    if (g_resolver.load(std::memory_order_acquire) != nullptr) {
        return;
    }
    HMODULE const module = GetModuleHandleW(nullptr);
    if (module == nullptr) {
        return;
    }
    auto* const base = reinterpret_cast<std::byte*>(module);
    g_resolver.store(reinterpret_cast<TagResolver>(base + kTagResolverRva),
                     std::memory_order_release);
    core::log::write(core::log::Channel::client,
                     core::log::Level::info,
                     "ev=content_resolver stage=install result=ok");
}

std::size_t resolve(std::uint32_t hash, std::span<std::byte> output) noexcept {
    if (output.empty()) {
        return 0;
    }
    const std::byte* const definition = resolve_pointer(hash);
    const std::size_t length = committed_length(definition, output.size());
    if (length == 0 || !copy_guarded(definition, output.data(), length)) {
        return 0;
    }
    return length;
}

std::size_t dump_tree(std::uint32_t hash, std::uint32_t depth) noexcept {
    install();
    std::unordered_set<std::uint32_t> seen{};
    std::vector<std::uint32_t> frontier{hash};
    std::vector<std::byte> bytes{};
    std::vector<std::uint32_t> children{};
    std::size_t written = 0;
    seen.insert(hash);
    for (std::uint32_t level = 0; level <= depth && !frontier.empty(); ++level) {
        children.clear();
        for (const std::uint32_t tag : frontier) {
            if (written >= kMaximumBlobs) {
                break;
            }
            if (!read_blob(tag, bytes)) {
                continue;
            }
            const std::size_t record = record_length(bytes);
            const std::size_t gates = count_event_gates(bytes, record);
            write_blob(tag, bytes);
            ++written;
            std::array<char, 160> line{};
            static_cast<void>(std::snprintf(line.data(),
                                            line.size(),
                                            "ev=content_dump tag=0x%08X level=%u window=%zu "
                                            "record=%zu event_gates=%zu",
                                            tag,
                                            level,
                                            bytes.size(),
                                            record,
                                            gates));
            core::log::write(core::log::Channel::client, core::log::Level::info, line.data());
            if (level < depth) {
                collect_children(bytes, children);
            }
        }
        frontier.clear();
        for (const std::uint32_t child : children) {
            if (seen.insert(child).second) {
                frontier.push_back(child);
            }
        }
    }
    return written;
}

std::size_t find_event_gate_keys(std::uint32_t hash, std::span<std::uint32_t> outKeys) noexcept {
    install();
    std::vector<std::byte> bytes{};
    if (!read_blob(hash, bytes)) {
        return 0;
    }
    // Scan the record only: past it lie unrelated neighbouring records whose marker dwords would
    // otherwise be reported as this scene's keys.
    const std::size_t end = record_length(bytes);
    std::size_t found = 0;
    for (std::size_t offset = 0; offset + kEventGateKeyOffset + sizeof(std::uint32_t) <= end;
         offset += 4) {
        std::uint32_t marker = 0;
        std::memcpy(&marker, bytes.data() + offset, sizeof(marker));
        if (marker != kEventGateNodeClass) {
            continue;
        }
        std::uint32_t key = 0;
        std::memcpy(&key, bytes.data() + offset + kEventGateKeyOffset, sizeof(key));
        if (!is_event_key(key)) {
            continue;
        }
        if (found < outKeys.size()) {
            outKeys[found] = key;
        }
        ++found;
    }
    return found;
}

} // namespace sunrise::client::hooks::content_resolver
