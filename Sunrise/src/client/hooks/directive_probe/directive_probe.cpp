/**
 * Read-only probe on the type-68 directive HUD entry builder (client `0x7ff741df8570` in the
 * reference image). It is called with the directive sensor component, one lane of the stored
 * Auth body, the authored directive table and the 0x11c-byte HUD entry to fill: the lane's name
 * hash and element select the authored element, the lane's state (0 enters, 1 completes, 2 the
 * alternate exit) becomes the entry's display state at `+0x35` (3 / 5 / 4, or 1 when the
 * audience sensor refuses), and when the element declares a progress display the lane's first
 * two values at `+0x48` become the counter. The detour calls through unchanged and only records.
 */

#include "directive_probe.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <span>
#include <string_view>

#include "../../../core/logging/log.h"
#include "../../hooking/detour.h"
#include "../../patterns/image_scan.h"
#include "../../patterns/signature_text.h"

namespace sunrise::client::hooks::directive_probe {
namespace {

using patterns::scan_main_image_unique;
using patterns::signature;
using patterns::signature_length;

/** Entry builder: reads the lane's element index, then zeroes the 0x11c-byte entry. */
constexpr std::string_view kBuildText =
    "48 89 5C 24 10 48 89 74 24 18 57 41 56 41 57 48 83 EC 40 49 8B 40 18 49 8B D8 4C 63 52 04 "
    "49 83 C0 18 49 03 C0 48 8B F2 4C 8B F1 33 D2 41 B8 1C 01 00 00";
/** Compiled form of the pattern above; the scan requires one match. */
constexpr auto kBuild = signature<signature_length(kBuildText)>(kBuildText);

/** Lane layout (client lane, 0xF8 bytes). */
constexpr std::size_t kLaneNameHash = 0x00;
constexpr std::size_t kLaneElement = 0x04;
constexpr std::size_t kLaneState = 0x08;
constexpr std::size_t kLaneProgress = 0x48;
/** Entry layout (0x11c bytes). */
constexpr std::size_t kEntryProgressShown = 0x34;
constexpr std::size_t kEntryDisplayState = 0x35;
constexpr std::size_t kEntryCounter = 0x38;
/** Process-wide line cap, so a rebuilt-every-frame entry cannot flood the log. */
constexpr unsigned kMaxReports = 600;
/** Sensor components remembered for change detection. */
constexpr std::size_t kMaxComponents = 8;

using Build = void*(__fastcall*)(void*, const std::byte*, const std::byte*, std::byte*);

/** The last lane content logged for one sensor component. */
struct Last {
    const void* component{};
    std::uint32_t nameHash{};
    std::int32_t element{};
    std::int8_t state{};
    std::array<std::int32_t, 4> progress{};
    std::uint8_t display{};
};

hooking::detour::Handle g_handle{};
std::atomic_bool g_installed{false};
// Diagnostic counter only; a lost increment across threads costs nothing.
unsigned g_reports{0};
std::array<Last, kMaxComponents> g_last{};
std::size_t g_lastCount{0};
/** Component the builder was last called for: a change is the HUD switching instances. */
const void* g_active{nullptr};

[[nodiscard]] std::uint32_t read_u32(const std::byte* base, std::size_t offset) noexcept {
    std::uint32_t value = 0;
    std::memcpy(&value, base + offset, sizeof(value));
    return value;
}

[[nodiscard]] std::int32_t read_i32(const std::byte* base, std::size_t offset) noexcept {
    std::int32_t value = 0;
    std::memcpy(&value, base + offset, sizeof(value));
    return value;
}

/** @return The change slot for this component, appending one while there is room. */
[[nodiscard]] Last* slot_for(const void* component) noexcept {
    for (std::size_t index = 0; index < g_lastCount; ++index) {
        if (g_last[index].component == component) {
            return &g_last[index];
        }
    }
    if (g_lastCount >= g_last.size()) {
        return nullptr;
    }
    g_last[g_lastCount] = {};
    g_last[g_lastCount].component = component;
    return &g_last[g_lastCount++];
}

/** Records the lane and the entry it produced, once per change per sensor component. */
void* __fastcall build(void* component,
                       const std::byte* lane,
                       const std::byte* table,
                       std::byte* entry) noexcept {
    auto* original = reinterpret_cast<Build>(g_handle.original);
    if (original == nullptr) {
        return nullptr;
    }
    void* const result = original(component, lane, table, entry);
    if (lane == nullptr || entry == nullptr || g_reports >= kMaxReports) {
        return result;
    }
    // The HUD switching to another instance of the sensor is what replays a stale directive,
    // whether or not that instance's content changed, so the switch itself is logged.
    if (component != g_active) {
        const void* const previous = g_active;
        g_active = component;
        if (previous != nullptr) {
            ++g_reports;
            std::array<char, core::log::kLineCapacity> line{};
            const int written =
                std::snprintf(line.data(), line.size(),
                              "ev=probe stage=directive result=switch from=%p to=%p name=0x%08X "
                              "progress=%d,%d",
                              previous, component, read_u32(lane, kLaneNameHash),
                              read_i32(lane, kLaneProgress),
                              read_i32(lane, kLaneProgress + sizeof(std::int32_t)));
            if (written > 0) {
                core::log::write(core::log::Channel::client, core::log::Level::warn,
                                 {line.data(), static_cast<std::size_t>(written)});
            }
        }
    }
    Last now{};
    now.component = component;
    now.nameHash = read_u32(lane, kLaneNameHash);
    now.element = read_i32(lane, kLaneElement);
    now.state = static_cast<std::int8_t>(lane[kLaneState]);
    for (std::size_t index = 0; index < now.progress.size(); ++index) {
        now.progress[index] = read_i32(lane, kLaneProgress + index * sizeof(std::int32_t));
    }
    now.display = static_cast<std::uint8_t>(entry[kEntryDisplayState]);
    Last* const last = slot_for(component);
    if (last != nullptr && last->nameHash == now.nameHash && last->element == now.element
        && last->state == now.state && last->progress == now.progress
        && last->display == now.display) {
        return result;
    }
    if (last != nullptr) {
        *last = now;
    }
    ++g_reports;
    std::uint32_t handle = 0;
    if (component != nullptr) {
        std::memcpy(&handle, component, sizeof(handle));
    }
    std::array<char, core::log::kLineCapacity> line{};
    const int written = std::snprintf(
        line.data(),
        line.size(),
        "ev=probe stage=directive comp=%p entity=0x%08X lane=%p name=0x%08X element=%d state=%d "
        "progress=%d,%d,%d,%d display=%u progress_shown=%u counter=%d/%d table=%p",
        component,
        handle,
        static_cast<const void*>(lane),
        now.nameHash,
        now.element,
        static_cast<int>(now.state),
        now.progress[0],
        now.progress[1],
        now.progress[2],
        now.progress[3],
        static_cast<unsigned>(now.display),
        static_cast<unsigned>(entry[kEntryProgressShown]),
        read_i32(entry, kEntryCounter),
        read_i32(entry, kEntryCounter + sizeof(std::int32_t)),
        static_cast<const void*>(table));
    if (written > 0) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::warn,
                         {line.data(), static_cast<std::size_t>(written)});
    }
    return result;
}

/** @param reason Short name of the step that failed. @return Always false. */
[[nodiscard]] bool fail(const char* reason) noexcept {
    std::array<char, core::log::kLineCapacity> line{};
    const int written = std::snprintf(
        line.data(), line.size(), "ev=probe stage=directive result=fail reason=%s", reason);
    if (written > 0) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::warn,
                         {line.data(), static_cast<std::size_t>(written)});
    }
    return false;
}

} // namespace

/** Attaches the read-only detour on the entry builder. */
bool install() noexcept {
    if (g_installed.load(std::memory_order_acquire)) {
        return true;
    }
    std::byte* const found = scan_main_image_unique(kBuild, "directive_probe_build");
    if (found == nullptr) {
        return fail("directive_probe_build");
    }
    const hooking::detour::Spec spec{found, reinterpret_cast<void*>(&build)};
    if (!hooking::detour::install(spec, g_handle)) {
        return fail("attach");
    }
    g_installed.store(true, std::memory_order_release);
    core::log::write(core::log::Channel::client,
                     core::log::Level::warn,
                     "ev=probe stage=directive result=installed");
    return true;
}

/** Detaches the probe detour. */
bool uninstall() noexcept {
    if (!g_installed.load(std::memory_order_acquire)) {
        return true;
    }
    const bool detached = hooking::detour::uninstall(g_handle);
    g_installed.store(!detached, std::memory_order_release);
    return detached;
}

} // namespace sunrise::client::hooks::directive_probe
