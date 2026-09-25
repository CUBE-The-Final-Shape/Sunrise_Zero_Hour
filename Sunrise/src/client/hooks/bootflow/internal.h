#pragma once

#include "../../hooking/detour.h"
#include "../../patterns/image_scan.h"

namespace sunrise::client::hooks::bootflow {

using patterns::resolve_relative;
using patterns::scan_main_image_unique;
using patterns::signature;
using patterns::signature_length;

enum class StageResult : unsigned char {
    /** The target is missing. The fix reported that itself and staged nothing. */
    unavailable,
    /** An earlier install already attached this fix, so there is nothing to stage. */
    attached,
    /** The spec is filled and the fix wants attaching. */
    staged,
};

[[nodiscard]] StageResult stage_owner_activity_slot(hooking::detour::Spec& spec) noexcept;

/** Takes the owner activity slot force's attached handle, or a detached one. */
void publish_owner_activity_slot(const hooking::detour::Handle& handle) noexcept;

/** Detaches the owner activity slot force. */
void uninstall_owner_activity_slot() noexcept;

/**
 * Finds the boot-flow step accessor behind `in_world`.
 * Nothing is detoured: the accessor is called, so a miss reads as out of world.
 * @return True when the target was found.
 */
[[nodiscard]] bool install_world_step() noexcept;

/** Clears the boot-flow step accessor. */
void uninstall_world_step() noexcept;

/**
 * Attaches the read-only lifetime gate probe, a diagnostic on step 38's joinability gate.
 * @return True when the reader and its helpers were found and the detour attached.
 */
[[nodiscard]] bool install_lifetime_gate_probe() noexcept;

/** Detaches the lifetime gate probe. */
void uninstall_lifetime_gate_probe() noexcept;

} // namespace sunrise::client::hooks::bootflow
