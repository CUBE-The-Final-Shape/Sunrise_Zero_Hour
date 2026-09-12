#pragma once

#include <span>
#include <string_view>

#include "../../middleware/content/packages/tables/activity_definition_reader.h"

namespace sunrise::client::diagnostics {

/**
 * Logs one line per activity definition whose internal name contains any of `needles`
 * (case-insensitive substring match), so a real activityIndex/definitionHash pair can be found
 * without stepping through the in-game Mission Launch panel by eye.
 * A no-op when the client log channel is below info, so this carries no cost in a normal run.
 * @param definitions Every installed activity definition, in activity-index order.
 * @param needles Lowercase or mixed-case substrings to search for in each internal name.
 */
void probe_activity_names(
    std::span<const middleware::content::packages::tables::ActivityDefinition> definitions,
    std::span<const std::string_view> needles) noexcept;

} // namespace sunrise::client::diagnostics
