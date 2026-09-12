#include "activity_name_probe.h"

#include <array>
#include <cctype>
#include <cstdio>

#include "../../core/logging/log.h"

namespace sunrise::client::diagnostics {
namespace {

constexpr std::size_t kLineCapacity = 256;

[[nodiscard]] char lower(char value) noexcept {
    return static_cast<char>(std::tolower(static_cast<unsigned char>(value)));
}

[[nodiscard]] bool contains_ignore_case(std::string_view haystack, std::string_view needle) noexcept {
    if (needle.empty()) {
        return false;
    }
    if (needle.size() > haystack.size()) {
        return false;
    }
    for (std::size_t offset = 0; offset + needle.size() <= haystack.size(); ++offset) {
        bool matched = true;
        for (std::size_t index = 0; matched && index < needle.size(); ++index) {
            matched = lower(haystack[offset + index]) == lower(needle[index]);
        }
        if (matched) {
            return true;
        }
    }
    return false;
}

} // namespace

void probe_activity_names(
    std::span<const middleware::content::packages::tables::ActivityDefinition> definitions,
    std::span<const std::string_view> needles) noexcept {
    // warn, not info: default settings threshold every channel at warn, and this probe must be
    // visible with no configuration change.
    if (!core::log::accepts(core::log::Channel::client, core::log::Level::warn)) {
        return;
    }
    for (const auto& definition : definitions) {
        const std::string_view name(definition.internalName.data(), definition.internalNameLength);
        for (const std::string_view needle : needles) {
            if (!contains_ignore_case(name, needle)) {
                continue;
            }
            std::array<char, kLineCapacity> line{};
            const int written = std::snprintf(line.data(),
                                              line.size(),
                                              "ev=activity_probe needle=%.*s index=%u hash=0x%08X "
                                              "destination=%u name=%.*s",
                                              static_cast<int>(needle.size()),
                                              needle.data(),
                                              definition.activityIndex,
                                              definition.definitionHash,
                                              static_cast<unsigned>(definition.destinationIndex),
                                              static_cast<int>(name.size()),
                                              name.data());
            if (written > 0) {
                core::log::write(core::log::Channel::client,
                                 core::log::Level::warn,
                                 {line.data(), static_cast<std::size_t>(written)});
            }
            break;
        }
    }
}

} // namespace sunrise::client::diagnostics
