#pragma once

#include "../../../state/activity_sdk/runtime.h"

namespace sunrise::server::activity::host {
struct InstanceSnapshot;
}

namespace sunrise::server::ui::activity_host::sequencer_view {

/**
 * Draws the visual sequencer: named chains of timed steps (squads, cues, scenes, poses, effects,
 * objects, devices, directives) started by a trigger crossing, a squad clear, another sequence's
 * end, the spawn, or by hand. The page edits one JSON file under Sunrise/sequences/ that the
 * mission script's sequencer module plays back; pickers come from the bound SDK view and the
 * package trigger-volume catalog.
 */
void draw(const state::activity_sdk::BoundView& view,
          const server::activity::host::InstanceSnapshot& instance) noexcept;

} // namespace sunrise::server::ui::activity_host::sequencer_view
