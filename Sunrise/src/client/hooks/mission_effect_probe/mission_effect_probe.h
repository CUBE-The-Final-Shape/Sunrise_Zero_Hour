#pragma once

namespace sunrise::client::hooks::mission_effect_probe {

/**
 * Attaches the read-only probe on the type-26 mission effect (hop-on) apply chain.
 * Each processed Auth body is logged with its fields, its filter reference, and every entity
 * the filter selected together with the attach outcome, at warn level.
 * @return True when every target is found and all detours attach, or they already did.
 */
[[nodiscard]] bool install() noexcept;

/** Detaches the probe so a later unload cannot leave a detour into unmapped code. */
[[nodiscard]] bool uninstall() noexcept;

} // namespace sunrise::client::hooks::mission_effect_probe
