#pragma once

namespace sunrise::client::hooks::directive_probe {

/**
 * Attaches the read-only probe on the type-68 directive HUD entry builder.
 * Each time the client turns a directive lane into a HUD entry, the lane's fields (name hash,
 * element, state, the four progress values) and the entry's resulting display state are logged
 * at warn level, once per change per sensor component.
 * @return True when the target is found and the detour attaches, or it already did.
 */
[[nodiscard]] bool install() noexcept;

/** Detaches the probe so a later unload cannot leave a detour into unmapped code. */
[[nodiscard]] bool uninstall() noexcept;

} // namespace sunrise::client::hooks::directive_probe
