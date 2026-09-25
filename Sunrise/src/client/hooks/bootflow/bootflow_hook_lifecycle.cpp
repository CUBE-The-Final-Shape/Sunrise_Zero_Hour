#include "bootflow_hook_lifecycle.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <span>

#include "internal.h"
#include "spawn/slice_set_sample.h"

namespace sunrise::client::hooks::bootflow {
namespace {

std::atomic_bool g_installed{false};

} // namespace

struct Fix {
    StageResult (*stage)(hooking::detour::Spec&) noexcept;
    void (*publish)(const hooking::detour::Handle&) noexcept;
};

constexpr std::array kFixes{
    Fix{&stage_owner_activity_slot, &publish_owner_activity_slot},
};

/** Marks a fix that staged nothing, so no handle is ever published to it. */
constexpr std::size_t kNotStaged = kFixes.size();

/** One fix's place in the batch, and what it already was before staging. */
struct Placement {
    std::size_t slot{kNotStaged};
    StageResult result{StageResult::unavailable};
};

/**
 * Finds the boot-step accessor and the slice-set sample targets.
 * Nothing is detoured: the boot steps run as shipped and the host answers them.
 * @return True when both targets were found.
 */
bool install() noexcept {
	std::array<hooking::detour::Spec, kFixes.size()> specs{};
    std::array<hooking::detour::Handle, kFixes.size()> handles{};
    std::array<Placement, kFixes.size()> placement{};
    std::size_t staged = 0;
    for (std::size_t index = 0; index < kFixes.size(); ++index) {
        hooking::detour::Spec spec{};
        const StageResult result = kFixes[index].stage(spec);
        placement[index].result = result;
        if (result != StageResult::staged) {
            continue;
        }
        specs[staged] = spec;
        placement[index].slot = staged;
        ++staged;
    }

    if (staged != 0
        && !hooking::detour::install(std::span(specs).first(staged),
                                     std::span(handles).first(staged))) {
        // One refused target must not cost the others their fix, so the slow path stands them up
        // separately. It runs only when the whole batch failed, which no supported build does.
        for (std::size_t slot = 0; slot < staged; ++slot) {
            handles[slot] = {};
            (void)hooking::detour::install(specs[slot], handles[slot]);
        }
    }

    bool anyFix = false;
    bool everyFix = true;
    for (std::size_t index = 0; index < kFixes.size(); ++index) {
        const Placement& place = placement[index];
        if (place.slot == kNotStaged) {
            // An already-attached fix stays attached; only a missing target is a failure.
            anyFix = anyFix || place.result == StageResult::attached;
            everyFix = everyFix && place.result == StageResult::attached;
            continue;
        }
        const hooking::detour::Handle& handle = handles[place.slot];
        kFixes[index].publish(handle);
        anyFix = anyFix || handle.attached;
        everyFix = everyFix && handle.attached;
    }
	
    const bool worldStep = install_world_step();
    const bool sliceSet = spawn::install_targets();
    const bool probe = install_lifetime_gate_probe();
    g_installed.store(worldStep || sliceSet || probe, std::memory_order_release);
    return worldStep && sliceSet && probe;
}

/** Clears both accessors, in the reverse order of install. */
void uninstall() noexcept {
	uninstall_owner_activity_slot();
    uninstall_lifetime_gate_probe();
    spawn::uninstall_targets();
    uninstall_world_step();
    g_installed.store(false, std::memory_order_release);
}

/** @return True while at least one accessor is found. */
bool is_installed() noexcept {
    return g_installed.load(std::memory_order_acquire);
}

} // namespace sunrise::client::hooks::bootflow
