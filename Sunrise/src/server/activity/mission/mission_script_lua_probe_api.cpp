#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdio>
#include <span>

#include "../../../core/filesystem/path.h"
#include "../../../core/logging/log.h"
#include "mission_script_lua_internal.h"

namespace sunrise::server::activity::mission::lua_vm::detail {
namespace {

// Stays comfortably under core::log::kLineCapacity (1024) once the prefix core::log adds itself
// (channel, level, tick) is accounted for.
constexpr std::size_t kLineCapacity = 900;
constexpr std::size_t kMaxTextLength = 700;
// A remote command is a short Lua snippet, not a script file; this is generous headroom.
constexpr std::size_t kMaxCommandLength = 4096;

/** Replaces control characters so one probe call can never fake a second log line. */
void sanitize(std::span<char> text) noexcept {
    for (char& ch : text) {
        if (static_cast<unsigned char>(ch) < 0x20) {
            ch = ' ';
        }
    }
}

} // namespace

int context_probe(lua_State* state) {
    static_cast<void>(luaL_checkudata(state, 1, kContextMetatable));
    std::size_t length = 0;
    const char* const text = luaL_checklstring(state, 2, &length);
    Impl* const impl = impl_from_state(state);

    std::array<char, kMaxTextLength> safeText{};
    const std::size_t copyLength = (std::min)(length, safeText.size() - 1);
    std::copy_n(text, copyLength, safeText.begin());
    sanitize(std::span(safeText).first(copyLength));

    std::array<char, kLineCapacity> line{};
    const int written = std::snprintf(line.data(),
                                      line.size(),
                                      "ev=script_probe activity=%s text=%.*s",
                                      impl != nullptr ? impl->identity.activityId.data() : "?",
                                      static_cast<int>(copyLength),
                                      safeText.data());
    if (written > 0) {
        core::log::write(core::log::Channel::server,
                         core::log::Level::warn,
                         {line.data(), static_cast<std::size_t>(written)});
    }
    return 0;
}

/**
 * Lua: context:poll_command() — reads and consumes one pending remote command.
 * Development tooling only, mirroring the read side of an external file-drop channel: an
 * operator (or an agent) drops a Lua snippet at Sunrise/rt_cmd.txt; the next timer tick calling
 * this picks it up, deletes the file so it never runs twice, and hands the raw text to the
 * caller to `load` and `pcall`. No polling happens on its own — the calling script decides when
 * and how often to check.
 * @return The command text, or nil when no command file is currently present.
 */
int context_poll_command(lua_State* state) {
    static_cast<void>(luaL_checkudata(state, 1, kContextMetatable));

    core::path::Buffer path{};
    if (!core::path::artifact_file(L"rt_cmd.txt", path)) {
        lua_pushnil(state);
        return 1;
    }

    const HANDLE file = CreateFileW(path.chars.data(),
                                    GENERIC_READ,
                                    FILE_SHARE_READ | FILE_SHARE_DELETE,
                                    nullptr,
                                    OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL,
                                    nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        lua_pushnil(state);
        return 1;
    }

    LARGE_INTEGER size{};
    const bool sizeReady = GetFileSizeEx(file, &size) != FALSE;
    static std::array<char, kMaxCommandLength> buffer{};
    DWORD read = 0;
    bool loaded = false;
    if (sizeReady && size.QuadPart > 0
        && static_cast<unsigned long long>(size.QuadPart) <= buffer.size()) {
        const auto requested = static_cast<DWORD>(size.QuadPart);
        loaded = ReadFile(file, buffer.data(), requested, &read, nullptr) != FALSE
                 && read == requested;
    }
    CloseHandle(file);
    // Consumed either way: a command this build cannot read (too large, unreadable) must not be
    // retried forever on every following tick.
    DeleteFileW(path.chars.data());

    if (!loaded || read == 0) {
        lua_pushnil(state);
        return 1;
    }
    lua_pushlstring(state, buffer.data(), read);
    return 1;
}

} // namespace sunrise::server::activity::mission::lua_vm::detail
