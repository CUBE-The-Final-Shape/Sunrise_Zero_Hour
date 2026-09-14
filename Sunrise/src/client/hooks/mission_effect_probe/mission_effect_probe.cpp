/**
 * Read-only probe on the type-26 mission effect (hop-on) component, class 0x8080953F.
 * The Auth body (112 bytes, schema 0x8080954B) is copied to component `+0x180` and a dirty flag
 * is raised; the tick then calls the process routine, which detaches, and unless the body is
 * disabled walks the entities the type-34 filter selects and attaches the effect to each.
 * Every detour calls through unchanged and only records; nothing in the game is written.
 */

#include "mission_effect_probe.h"

#include <array>
#include <atomic>
#include <cstdarg>
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

namespace sunrise::client::hooks::mission_effect_probe {
namespace {

using patterns::scan_main_image_unique;
using patterns::signature;
using patterns::signature_length;

/** Process: detach, attach unless disabled, then the revision latches. */
constexpr std::string_view kProcessText =
    "40 53 48 83 EC 20 48 8B D9 E8 ? ? ? ? 80 BB 81 01 00 00 00 75 ? 48 8B CB E8 ? ? ? ? 8B 83 "
    "90 01 00 00 89 83 F8 01 00 00";
/** Compiled form of the pattern above; the scan requires one match. */
constexpr auto kProcess = signature<signature_length(kProcessText)>(kProcessText);

/** Attach all: resolves the filter reference at `+0x194` and walks its selection. */
constexpr std::string_view kAttachAllText =
    "48 89 5C 24 10 57 48 81 EC 20 01 00 00 48 8B 05 ? ? ? ? 48 33 C4 48 89 84 24 10 01 00 00 48 "
    "8B D9 48 81 C1 94 01 00 00";
/** Compiled form of the pattern above; the scan requires one match. */
constexpr auto kAttachAll = signature<signature_length(kAttachAllText)>(kAttachAllText);

/** Attach one: (component, entity handle) -> 1 when the effect was attached. */
constexpr std::string_view kAttachOneText =
    "48 89 5C 24 18 57 48 83 EC 20 8B C2 8B DA 25 FF 1F 00 00 48 8B F9 44 8B C0 8B D0 49 C1 E8 05 "
    "48 8D 05 ? ? ? ? 83 E2 1F";
/** Compiled form of the pattern above; the scan requires one match. */
constexpr auto kAttachOne = signature<signature_length(kAttachOneText)>(kAttachOneText);

/** Stored Auth body inside the component. */
constexpr std::size_t kBodyOffset = 0x180;
/** Filter ClientRef inside the component (body `+0x14`). */
constexpr std::size_t kFilterRefOffset = 0x194;
/** Inline predicate handle; -1 when the body carries none. */
constexpr std::size_t kInlineOffset = 0x1A0;
/** Latched revisions the process routine compares against. */
constexpr std::size_t kLatchOffset = 0x1F0;
/** Entities remembered per process call. */
constexpr std::size_t kMaxEntities = 8;
/** Process-wide line cap, so a looping component cannot flood the log. */
constexpr unsigned kMaxReports = 400;

using Process = void(__fastcall*)(void*);
using AttachAll = void(__fastcall*)(void*);
using AttachOne = char(__fastcall*)(void*, std::uint32_t);

/** Detour slots, one per target above. */
enum Index : std::size_t {
    kIdxProcess,
    kIdxAttachAll,
    kIdxAttachOne,
    kIdxCount,
};

/** What the attach calls inside one process call produced. */
struct Selection {
    unsigned depth{};
    unsigned attachAllCalls{};
    unsigned selected{};
    unsigned attached{};
    std::array<std::uint32_t, kMaxEntities> entities{};
    std::array<char, kMaxEntities> results{};
};

std::array<hooking::detour::Handle, kIdxCount> g_handles{};
std::atomic_bool g_installed{false};
// Diagnostic counter only; a lost increment across threads costs nothing.
unsigned g_reports{0};
thread_local Selection t_selection{};

/** Formats and writes one probe line at warn level, under the shared line cap. */
void write_line(const char* format, ...) noexcept {
    if (g_reports >= kMaxReports) {
        return;
    }
    ++g_reports;
    std::array<char, core::log::kLineCapacity> line{};
    va_list args;
    va_start(args, format);
    const int written = std::vsnprintf(line.data(), line.size(), format, args);
    va_end(args);
    if (written > 0) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::warn,
                         {line.data(), static_cast<std::size_t>(written)});
    }
}

[[nodiscard]] std::uint32_t read_u32(const std::byte* base, std::size_t offset) noexcept {
    std::uint32_t value = 0;
    std::memcpy(&value, base + offset, sizeof(value));
    return value;
}

/** Records the stored body, the filter reference, and what the attach walk selected. */
void __fastcall process(void* component) noexcept {
    auto* original = reinterpret_cast<Process>(g_handles[kIdxProcess].original);
    if (original == nullptr) {
        return;
    }
    const Selection outer = t_selection;
    t_selection = {};
    t_selection.depth = 1;
    original(component);
    const Selection seen = t_selection;
    t_selection = outer;
    if (component == nullptr) {
        return;
    }
    const auto* comp = static_cast<const std::byte*>(component);
    const auto* body = comp + kBodyOffset;
    std::array<char, 8 * 20 + 1> list{};
    std::size_t used = 0;
    for (std::size_t index = 0; index < seen.selected && index < kMaxEntities; ++index) {
        const int written = std::snprintf(list.data() + used,
                                          list.size() - used,
                                          "%s%08X:%d",
                                          index == 0 ? "" : ",",
                                          seen.entities[index],
                                          static_cast<int>(seen.results[index]));
        if (written <= 0 || static_cast<std::size_t>(written) >= list.size() - used) {
            break;
        }
        used += static_cast<std::size_t>(written);
    }
    write_line("ev=probe stage=mission_effect at=process comp=0x%llX handle=0x%08X b0=%d "
               "disabled=%d v0=%d v1=%d v2=%d revision=%d ref=%08X.%08X.%08X inline=%08X "
               "latch=%d,%d,%d attach_all=%u selected=%u attached=%u entities=[%s]",
               static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(component)),
               read_u32(comp, 0),
               static_cast<int>(body[0]),
               static_cast<int>(body[1]),
               static_cast<int>(read_u32(body, 0x04)),
               static_cast<int>(read_u32(body, 0x08)),
               static_cast<int>(read_u32(body, 0x0C)),
               static_cast<int>(read_u32(body, 0x10)),
               read_u32(comp, kFilterRefOffset),
               read_u32(comp, kFilterRefOffset + 4),
               read_u32(comp, kFilterRefOffset + 8),
               read_u32(comp, kInlineOffset),
               static_cast<int>(read_u32(comp, kLatchOffset)),
               static_cast<int>(read_u32(comp, kLatchOffset + 4)),
               static_cast<int>(read_u32(comp, kLatchOffset + 8)),
               seen.attachAllCalls,
               seen.selected,
               seen.attached,
               list.data());
}

/** Counts the attach walks inside a process call. */
void __fastcall attach_all(void* component) noexcept {
    auto* original = reinterpret_cast<AttachAll>(g_handles[kIdxAttachAll].original);
    if (original == nullptr) {
        return;
    }
    ++t_selection.attachAllCalls;
    original(component);
}

/** Records each selected entity and whether the effect attached; logs attaches from elsewhere. */
char __fastcall attach_one(void* component, std::uint32_t entity) noexcept {
    auto* original = reinterpret_cast<AttachOne>(g_handles[kIdxAttachOne].original);
    if (original == nullptr) {
        return 0;
    }
    const char result = original(component, entity);
    if (t_selection.depth == 0) {
        write_line("ev=probe stage=mission_effect at=attach_outside comp=0x%llX entity=%08X ret=%d",
                   static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(component)),
                   entity,
                   static_cast<int>(result));
        return result;
    }
    if (t_selection.selected < kMaxEntities) {
        t_selection.entities[t_selection.selected] = entity;
        t_selection.results[t_selection.selected] = result;
    }
    ++t_selection.selected;
    if (result != 0) {
        ++t_selection.attached;
    }
    return result;
}

/** @param reason Short name of the step that failed. @return Always false. */
[[nodiscard]] bool fail(const char* reason) noexcept {
    std::array<char, core::log::kLineCapacity> line{};
    const int written = std::snprintf(
        line.data(), line.size(), "ev=probe stage=mission_effect result=fail reason=%s", reason);
    if (written > 0) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::warn,
                         {line.data(), static_cast<std::size_t>(written)});
    }
    return false;
}

} // namespace

/** Attaches all three read-only detours in one transaction, or none of them. */
bool install() noexcept {
    if (g_installed.load(std::memory_order_acquire)) {
        return true;
    }
    struct Target {
        std::span<const patterns::PatternByte> pattern;
        const char* name;
        void* replacement;
    };
    const std::array<Target, kIdxCount> targets{{
        {kProcess, "mission_effect_probe_process", reinterpret_cast<void*>(&process)},
        {kAttachAll, "mission_effect_probe_attach_all", reinterpret_cast<void*>(&attach_all)},
        {kAttachOne, "mission_effect_probe_attach_one", reinterpret_cast<void*>(&attach_one)},
    }};
    std::array<hooking::detour::Spec, kIdxCount> specs{};
    for (std::size_t index = 0; index < kIdxCount; ++index) {
        std::byte* const found =
            scan_main_image_unique(targets[index].pattern, targets[index].name);
        if (found == nullptr) {
            return fail(targets[index].name);
        }
        specs[index] = {found, targets[index].replacement};
    }
    if (!hooking::detour::install(specs, g_handles)) {
        return fail("attach");
    }
    g_installed.store(true, std::memory_order_release);
    core::log::write(core::log::Channel::client,
                     core::log::Level::warn,
                     "ev=probe stage=mission_effect result=installed");
    return true;
}

/** Detaches every probe detour in one transaction. */
bool uninstall() noexcept {
    if (!g_installed.load(std::memory_order_acquire)) {
        return true;
    }
    const bool detached = hooking::detour::uninstall(g_handles);
    g_installed.store(!detached, std::memory_order_release);
    return detached;
}

} // namespace sunrise::client::hooks::mission_effect_probe
