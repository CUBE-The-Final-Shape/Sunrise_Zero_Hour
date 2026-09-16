#include "activity_host_sequencer_view.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <imgui.h>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "../../../core/filesystem/path.h"
#include "../../../state/build_data/scriptables/scriptable_catalog.h"
#include "../../activity/mission/mission_script_runtime.h"
#include "activity_host_sdk_mission_text.h"

namespace sunrise::server::ui::activity_host::sequencer_view {
namespace {

namespace sdk = state::activity_sdk;
namespace format = state::activity_sdk::format;
namespace catalog = state::build_data::scriptables;
namespace mission = server::activity::mission;

//--------------------------------------------------------------------------------------------
// Model. Mirrors the file shape documented in scripts/mission_towerfall/sequencer.lua.
//--------------------------------------------------------------------------------------------

constexpr std::array<const char*, 6> kStartKinds{
    "trigger_enter", "trigger_exit", "squads_clear", "sequence_end", "spawn", "manual"};
constexpr std::array<const char*, 13> kStepKinds{"squad",
                                                 "retire",
                                                 "cue",
                                                 "scene",
                                                 "pose",
                                                 "effect",
                                                 "object",
                                                 "device",
                                                 "directive",
                                                 "sequence",
                                                 "clear",
                                                 "probe",
                                                 "despawn"};
constexpr std::array<const char*, 4> kSceneModes{"full", "activate", "keys", "clear"};

/** One step. Fields are shared across kinds; each kind documents which ones it reads. */
struct Step final {
    int kind{};              // index into kStepKinds
    int delayMs{};           // relative to the previous step
    std::string target{};    // squad | scene name | cell | effect slot | object/device slot | sequence name
    std::string second{};    // objective | scene slot id | filter slot
    std::string text{};      // probe text | directive label | clear name
    int count{1};            // squad count (0 = package default) | cue index | pose sends
    int mode{};              // scene mode index
    bool flagA{};            // squad hold | effect enabled | object active | device snap | players
    bool flagB{true};        // pose/scene new_generation
    float position{1.0F};    // device position
    std::uint32_t hashA{};   // pose group | directive hash | scene resource tag
    std::uint32_t hashB{};   // pose action
    std::vector<std::uint32_t> keys{};   // explicit scene keys
    std::vector<std::string> squads{};   // clear step squads
    std::array<int, 4> progress{};       // directive lane values (counter = first two)
};

struct Sequence final {
    std::string name{};
    bool enabled{true};
    bool once{true};
    int startKind{5};   // manual
    std::string startName{};   // trigger name or sequence name
    std::uint32_t registryKey{};
    std::uint32_t slotType{60};
    std::uint32_t slotIndex{};
    /** Region the trigger start is armed in, or -1 to arm it at mission start. */
    int startRegion{-1};
    std::vector<std::string> startSquads{};
    std::vector<Step> steps{};
};

struct Document final {
    std::vector<Sequence> sequences{};
};

//--------------------------------------------------------------------------------------------
// JSON writer / reader (tiny, only what this file needs).
//--------------------------------------------------------------------------------------------

void write_escaped(std::string& out, std::string_view text) {
    out.push_back('"');
    for (const char ch : text) {
        switch (ch) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (static_cast<unsigned char>(ch) < 0x20) {
                char buffer[8];
                std::snprintf(buffer, sizeof buffer, "\\u%04x", static_cast<unsigned>(ch));
                out += buffer;
            } else {
                out.push_back(ch);
            }
        }
    }
    out.push_back('"');
}

void write_key(std::string& out, const char* key) {
    write_escaped(out, key);
    out += ": ";
}

void write_uint(std::string& out, std::uint64_t value) {
    out += std::to_string(value);
}

void write_string_array(std::string& out, const std::vector<std::string>& values) {
    out.push_back('[');
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (i != 0) {
            out += ", ";
        }
        write_escaped(out, values[i]);
    }
    out.push_back(']');
}

std::string serialize(const Document& document) {
    std::string out = "{\n  \"version\": 1,\n  \"sequences\": [";
    for (std::size_t s = 0; s < document.sequences.size(); ++s) {
        const Sequence& sequence = document.sequences[s];
        out += s == 0 ? "\n    {\n" : ",\n    {\n";
        out += "      ";
        write_key(out, "name");
        write_escaped(out, sequence.name);
        out += ",\n      ";
        write_key(out, "enabled");
        out += sequence.enabled ? "true" : "false";
        out += ",\n      ";
        write_key(out, "once");
        out += sequence.once ? "true" : "false";
        out += ",\n      ";
        write_key(out, "start");
        out += "{ ";
        write_key(out, "kind");
        write_escaped(out, kStartKinds[static_cast<std::size_t>(sequence.startKind)]);
        const std::string_view startKind = kStartKinds[static_cast<std::size_t>(sequence.startKind)];
        if (startKind == "trigger_enter" || startKind == "trigger_exit") {
            out += ", ";
            write_key(out, "name");
            write_escaped(out, sequence.startName);
            if (sequence.registryKey != 0) {
                out += ", ";
                write_key(out, "registry_key");
                write_uint(out, sequence.registryKey);
                out += ", ";
                write_key(out, "slot_type");
                write_uint(out, sequence.slotType);
                out += ", ";
                write_key(out, "slot_index");
                write_uint(out, sequence.slotIndex);
            }
            if (sequence.startRegion >= 0) {
                out += ", ";
                write_key(out, "region");
                write_uint(out, static_cast<std::uint32_t>(sequence.startRegion));
            }
        } else if (startKind == "squads_clear") {
            out += ", ";
            write_key(out, "squads");
            write_string_array(out, sequence.startSquads);
        } else if (startKind == "sequence_end") {
            out += ", ";
            write_key(out, "sequence");
            write_escaped(out, sequence.startName);
        }
        out += " },\n      ";
        write_key(out, "steps");
        out += "[";
        for (std::size_t i = 0; i < sequence.steps.size(); ++i) {
            const Step& step = sequence.steps[i];
            const std::string_view kind = kStepKinds[static_cast<std::size_t>(step.kind)];
            out += i == 0 ? "\n        { " : ",\n        { ";
            write_key(out, "delay_ms");
            write_uint(out, static_cast<std::uint64_t>(std::max(step.delayMs, 0)));
            out += ", ";
            write_key(out, "kind");
            write_escaped(out, kind);
            auto field = [&](const char* key) {
                out += ", ";
                write_key(out, key);
            };
            if (kind == "squad") {
                field("squad");
                write_escaped(out, step.target);
                field("count");
                write_uint(out, static_cast<std::uint64_t>(std::max(step.count, 0)));
                if (!step.second.empty()) {
                    field("objective");
                    write_escaped(out, step.second);
                    field("hold");
                    out += step.flagA ? "true" : "false";
                }
            } else if (kind == "retire") {
                field("squad");
                write_escaped(out, step.target);
            } else if (kind == "cue") {
                field("cue");
                write_uint(out, static_cast<std::uint64_t>(std::max(step.count, 0)));
            } else if (kind == "scene") {
                field("scene");
                write_escaped(out, step.target);
                if (!step.second.empty()) {
                    field("slot");
                    write_escaped(out, step.second);
                }
                field("mode");
                write_escaped(out, kSceneModes[static_cast<std::size_t>(step.mode)]);
                field("new_generation");
                out += step.flagB ? "true" : "false";
                if (step.hashA != 0) {
                    field("resource");
                    write_uint(out, step.hashA);
                }
                if (!step.keys.empty()) {
                    field("keys");
                    out.push_back('[');
                    for (std::size_t k = 0; k < step.keys.size(); ++k) {
                        if (k != 0) {
                            out += ", ";
                        }
                        write_uint(out, step.keys[k]);
                    }
                    out.push_back(']');
                }
            } else if (kind == "pose") {
                field("cell");
                write_escaped(out, step.target);
                field("group");
                write_uint(out, step.hashA);
                field("action");
                write_uint(out, step.hashB);
                field("new_generation");
                out += step.flagB ? "true" : "false";
                field("sends");
                write_uint(out, static_cast<std::uint64_t>(std::max(step.count, 1)));
            } else if (kind == "despawn") {
                field("cell");
                write_escaped(out, step.target);
                if (step.hashA != 0) {
                    field("group");
                    write_uint(out, step.hashA);
                    field("action");
                    write_uint(out, step.hashB);
                }
            } else if (kind == "effect") {
                field("slot");
                write_escaped(out, step.target);
                field("filter");
                write_escaped(out, step.second);
                field("enabled");
                out += step.flagB ? "true" : "false";
                field("players");
                out += step.flagA ? "true" : "false";
            } else if (kind == "object") {
                field("slot");
                write_escaped(out, step.target);
                field("active");
                out += step.flagA ? "true" : "false";
            } else if (kind == "device") {
                field("slot");
                write_escaped(out, step.target);
                field("position");
                char buffer[32];
                std::snprintf(buffer, sizeof buffer, "%.3f", static_cast<double>(step.position));
                out += buffer;
                field("snap");
                out += step.flagA ? "true" : "false";
            } else if (kind == "directive") {
                field("hash");
                write_uint(out, step.hashA);
                field("label");
                write_escaped(out, step.text);
                if (step.flagA) {
                    field("raw");
                    out += "true";
                }
                if (step.progress != std::array<int, 4>{}) {
                    field("progress");
                    out.push_back('[');
                    for (std::size_t k = 0; k < step.progress.size(); ++k) {
                        if (k != 0) {
                            out += ", ";
                        }
                        out += std::to_string(step.progress[k]);
                    }
                    out.push_back(']');
                }
            } else if (kind == "sequence") {
                field("name");
                write_escaped(out, step.target);
            } else if (kind == "clear") {
                field("name");
                write_escaped(out, step.text);
                field("squads");
                write_string_array(out, step.squads);
            } else if (kind == "probe") {
                field("text");
                write_escaped(out, step.text);
            }
            out += " }";
        }
        out += sequence.steps.empty() ? "]\n    }" : "\n      ]\n    }";
    }
    out += document.sequences.empty() ? "]\n}\n" : "\n  ]\n}\n";
    return out;
}

/** A parsed JSON value; only what the sequence file uses. */
struct Value final {
    enum class Type : std::uint8_t { null, boolean, number, string, array, object };
    Type type{Type::null};
    bool boolean{};
    double number{};
    std::string string{};
    std::vector<Value> array{};
    std::vector<std::pair<std::string, Value>> object{};

    [[nodiscard]] const Value* get(std::string_view key) const noexcept {
        for (const auto& [name, value] : object) {
            if (name == key) {
                return &value;
            }
        }
        return nullptr;
    }
    [[nodiscard]] std::string str(std::string_view key, std::string fallback = {}) const {
        const Value* value = get(key);
        return value != nullptr && value->type == Type::string ? value->string : fallback;
    }
    [[nodiscard]] double num(std::string_view key, double fallback = 0.0) const noexcept {
        const Value* value = get(key);
        return value != nullptr && value->type == Type::number ? value->number : fallback;
    }
    [[nodiscard]] bool flag(std::string_view key, bool fallback) const noexcept {
        const Value* value = get(key);
        return value != nullptr && value->type == Type::boolean ? value->boolean : fallback;
    }
};

class Parser final {
public:
    explicit Parser(std::string_view text) noexcept : text_(text) {}

    bool parse(Value& out) {
        skip();
        if (!value(out)) {
            return false;
        }
        skip();
        return pos_ == text_.size();
    }

private:
    std::string_view text_;
    std::size_t pos_{};

    void skip() noexcept {
        while (pos_ < text_.size() && std::isspace(static_cast<unsigned char>(text_[pos_])) != 0) {
            ++pos_;
        }
    }
    [[nodiscard]] bool consume(char ch) noexcept {
        if (pos_ < text_.size() && text_[pos_] == ch) {
            ++pos_;
            return true;
        }
        return false;
    }
    bool string(std::string& out) {
        if (!consume('"')) {
            return false;
        }
        while (pos_ < text_.size()) {
            const char ch = text_[pos_++];
            if (ch == '"') {
                return true;
            }
            if (ch == '\\') {
                if (pos_ >= text_.size()) {
                    return false;
                }
                const char escaped = text_[pos_++];
                switch (escaped) {
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'u': {
                    if (pos_ + 4 > text_.size()) {
                        return false;
                    }
                    const unsigned code =
                        static_cast<unsigned>(std::strtoul(std::string(text_.substr(pos_, 4)).c_str(), nullptr, 16));
                    pos_ += 4;
                    if (code < 0x80) {
                        out.push_back(static_cast<char>(code));
                    } else if (code < 0x800) {
                        out.push_back(static_cast<char>(0xC0 | (code >> 6)));
                        out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
                    } else {
                        out.push_back(static_cast<char>(0xE0 | (code >> 12)));
                        out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
                        out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
                    }
                    break;
                }
                default: out.push_back(escaped); break;
                }
            } else {
                out.push_back(ch);
            }
        }
        return false;
    }
    bool value(Value& out) {
        skip();
        if (pos_ >= text_.size()) {
            return false;
        }
        const char ch = text_[pos_];
        if (ch == '{') {
            ++pos_;
            out.type = Value::Type::object;
            skip();
            if (consume('}')) {
                return true;
            }
            while (true) {
                skip();
                std::string key;
                if (!string(key)) {
                    return false;
                }
                skip();
                if (!consume(':')) {
                    return false;
                }
                Value child;
                if (!value(child)) {
                    return false;
                }
                out.object.emplace_back(std::move(key), std::move(child));
                skip();
                if (consume('}')) {
                    return true;
                }
                if (!consume(',')) {
                    return false;
                }
            }
        }
        if (ch == '[') {
            ++pos_;
            out.type = Value::Type::array;
            skip();
            if (consume(']')) {
                return true;
            }
            while (true) {
                Value child;
                if (!value(child)) {
                    return false;
                }
                out.array.push_back(std::move(child));
                skip();
                if (consume(']')) {
                    return true;
                }
                if (!consume(',')) {
                    return false;
                }
            }
        }
        if (ch == '"') {
            out.type = Value::Type::string;
            return string(out.string);
        }
        if (text_.substr(pos_, 4) == "true") {
            pos_ += 4;
            out.type = Value::Type::boolean;
            out.boolean = true;
            return true;
        }
        if (text_.substr(pos_, 5) == "false") {
            pos_ += 5;
            out.type = Value::Type::boolean;
            out.boolean = false;
            return true;
        }
        if (text_.substr(pos_, 4) == "null") {
            pos_ += 4;
            out.type = Value::Type::null;
            return true;
        }
        const char* begin = text_.data() + pos_;
        char* end = nullptr;
        const double number = std::strtod(begin, &end);
        if (end == begin) {
            return false;
        }
        pos_ += static_cast<std::size_t>(end - begin);
        out.type = Value::Type::number;
        out.number = number;
        return true;
    }
};

int index_of(std::span<const char* const> names, std::string_view value, int fallback) noexcept {
    for (std::size_t i = 0; i < names.size(); ++i) {
        if (value == names[i]) {
            return static_cast<int>(i);
        }
    }
    return fallback;
}

bool deserialize(std::string_view text, Document& document, std::string& error) {
    Value root;
    Parser parser(text);
    if (!parser.parse(root) || root.type != Value::Type::object) {
        error = "not a JSON object";
        return false;
    }
    document = {};
    const Value* sequences = root.get("sequences");
    if (sequences == nullptr || sequences->type != Value::Type::array) {
        error = "no \"sequences\" array";
        return false;
    }
    for (const Value& node : sequences->array) {
        if (node.type != Value::Type::object) {
            continue;
        }
        Sequence sequence;
        sequence.name = node.str("name");
        sequence.enabled = node.flag("enabled", true);
        sequence.once = node.flag("once", true);
        if (const Value* start = node.get("start"); start != nullptr && start->type == Value::Type::object) {
            sequence.startKind = index_of(kStartKinds, start->str("kind"), 5);
            sequence.startName = start->str("name", start->str("sequence"));
            sequence.registryKey = static_cast<std::uint32_t>(start->num("registry_key"));
            sequence.slotType = static_cast<std::uint32_t>(start->num("slot_type", 60));
            sequence.slotIndex = static_cast<std::uint32_t>(start->num("slot_index"));
            sequence.startRegion = static_cast<int>(start->num("region", -1.0));
            if (const Value* squads = start->get("squads"); squads != nullptr && squads->type == Value::Type::array) {
                for (const Value& squad : squads->array) {
                    if (squad.type == Value::Type::string) {
                        sequence.startSquads.push_back(squad.string);
                    }
                }
            }
        }
        if (const Value* steps = node.get("steps"); steps != nullptr && steps->type == Value::Type::array) {
            for (const Value& item : steps->array) {
                if (item.type != Value::Type::object) {
                    continue;
                }
                Step step;
                step.kind = index_of(kStepKinds, item.str("kind"), 11);
                step.delayMs = static_cast<int>(item.num("delay_ms"));
                const std::string_view kind = kStepKinds[static_cast<std::size_t>(step.kind)];
                if (kind == "squad" || kind == "retire") {
                    step.target = item.str("squad");
                    step.count = static_cast<int>(item.num("count", 1));
                    step.second = item.str("objective");
                    step.flagA = item.flag("hold", false);
                } else if (kind == "cue") {
                    step.count = static_cast<int>(item.num("cue"));
                } else if (kind == "scene") {
                    step.target = item.str("scene");
                    step.second = item.str("slot");
                    step.mode = index_of(kSceneModes, item.str("mode", "full"), 0);
                    step.flagB = item.flag("new_generation", true);
                    step.hashA = static_cast<std::uint32_t>(item.num("resource"));
                    if (const Value* keys = item.get("keys"); keys != nullptr && keys->type == Value::Type::array) {
                        for (const Value& key : keys->array) {
                            step.keys.push_back(static_cast<std::uint32_t>(key.number));
                        }
                    }
                } else if (kind == "pose") {
                    step.target = item.str("cell");
                    step.hashA = static_cast<std::uint32_t>(item.num("group"));
                    step.hashB = static_cast<std::uint32_t>(item.num("action"));
                    step.flagB = item.flag("new_generation", true);
                    step.count = static_cast<int>(item.num("sends", 2));
                } else if (kind == "despawn") {
                    step.target = item.str("cell");
                    step.hashA = static_cast<std::uint32_t>(item.num("group"));
                    step.hashB = static_cast<std::uint32_t>(item.num("action"));
                } else if (kind == "effect") {
                    step.target = item.str("slot");
                    step.second = item.str("filter");
                    step.flagB = item.flag("enabled", true);
                    step.flagA = item.flag("players", false);
                } else if (kind == "object") {
                    step.target = item.str("slot");
                    step.flagA = item.flag("active", true);
                } else if (kind == "device") {
                    step.target = item.str("slot");
                    step.position = static_cast<float>(item.num("position", 1.0));
                    step.flagA = item.flag("snap", false);
                } else if (kind == "directive") {
                    step.hashA = static_cast<std::uint32_t>(item.num("hash"));
                    step.text = item.str("label");
                    step.flagA = item.flag("raw", false);
                    if (const Value* values = item.get("progress"); values != nullptr && values->type == Value::Type::array) {
                        for (std::size_t k = 0; k < values->array.size() && k < step.progress.size(); ++k) {
                            step.progress[k] = static_cast<int>(values->array[k].number);
                        }
                    }
                } else if (kind == "sequence") {
                    step.target = item.str("name");
                } else if (kind == "clear") {
                    step.text = item.str("name");
                    if (const Value* squads = item.get("squads"); squads != nullptr && squads->type == Value::Type::array) {
                        for (const Value& squad : squads->array) {
                            if (squad.type == Value::Type::string) {
                                step.squads.push_back(squad.string);
                            }
                        }
                    }
                } else if (kind == "probe") {
                    step.text = item.str("text");
                }
                sequence.steps.push_back(std::move(step));
            }
        }
        document.sequences.push_back(std::move(sequence));
    }
    return true;
}

//--------------------------------------------------------------------------------------------
// Files under Sunrise/sequences/.
//--------------------------------------------------------------------------------------------

bool sequence_file_path(std::string_view fileName, core::path::Buffer& out) noexcept {
    std::array<wchar_t, 200> wide{};
    const int converted = MultiByteToWideChar(CP_UTF8,
                                              0,
                                              fileName.data(),
                                              static_cast<int>(fileName.size()),
                                              wide.data(),
                                              static_cast<int>(wide.size() - 1));
    if (converted <= 0) {
        return false;
    }
    std::wstring relative = L"sequences\\";
    relative.append(wide.data(), static_cast<std::size_t>(converted));
    return core::path::artifact_file(relative, out);
}

bool write_file(std::string_view fileName, const std::string& text, std::string& error) {
    core::path::Buffer path{};
    if (!sequence_file_path(fileName, path)) {
        error = "path error";
        return false;
    }
    const HANDLE file = CreateFileW(
        path.chars.data(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        error = "cannot create file";
        return false;
    }
    DWORD written = 0;
    const bool ok = WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &written, nullptr) != FALSE
                    && written == text.size();
    CloseHandle(file);
    if (!ok) {
        error = "write failed";
    }
    return ok;
}

bool read_file(std::string_view fileName, std::string& text, std::string& error) {
    core::path::Buffer path{};
    if (!sequence_file_path(fileName, path)) {
        error = "path error";
        return false;
    }
    const HANDLE file = CreateFileW(path.chars.data(),
                                    GENERIC_READ,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE,
                                    nullptr,
                                    OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL,
                                    nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        error = "no such file";
        return false;
    }
    LARGE_INTEGER size{};
    if (GetFileSizeEx(file, &size) == FALSE || size.QuadPart <= 0 || size.QuadPart > (4 << 20)) {
        CloseHandle(file);
        error = "empty or oversized file";
        return false;
    }
    text.resize(static_cast<std::size_t>(size.QuadPart));
    DWORD read = 0;
    const bool ok = ReadFile(file, text.data(), static_cast<DWORD>(text.size()), &read, nullptr) != FALSE
                    && read == text.size();
    CloseHandle(file);
    if (!ok) {
        error = "read failed";
    }
    return ok;
}

//--------------------------------------------------------------------------------------------
// Pickers: SDK slots of the bound scenario by slot type, and package trigger volumes.
//--------------------------------------------------------------------------------------------

struct Choice final {
    std::string label{};   // what the combo shows
    std::string value{};   // what the file stores
    std::uint32_t extra{}; // scene resource tag, dialogue cue count, ...
    std::string second{};  // scene slot id
};

struct TriggerChoice final {
    std::string label{};
    std::uint32_t registryKey{};
    std::uint32_t slotType{60};
    std::uint32_t slotIndex{};
    std::uint32_t bubble{};
};

struct Lists final {
    const sdk::Catalog* catalog{};
    std::uint32_t scenarioRow{format::kAbsentIndex};
    std::vector<Choice> squads{}, cells{}, objectives{}, objects{}, devices{}, effects{}, filters{},
        scenes{}, dialogue{};
    std::vector<TriggerChoice> triggers{};
    bool triggersReady{};
};

Lists g_lists{};

void rebuild_slot_lists(const sdk::BoundView& view) {
    const sdk::Catalog& catalog = *view.catalog;
    g_lists.catalog = &catalog;
    g_lists.scenarioRow = view.scenarioRow;
    g_lists.squads.clear();
    g_lists.cells.clear();
    g_lists.objectives.clear();
    g_lists.objects.clear();
    g_lists.devices.clear();
    g_lists.effects.clear();
    g_lists.filters.clear();
    g_lists.scenes.clear();
    g_lists.dialogue.clear();
    const auto occurrences = catalog.occurrences();
    const auto objects = catalog.objects();
    const auto resources = catalog.authored_scene_resources();
    for (const format::Occurrence& occurrence : occurrences) {
        if (occurrence.scenarioIndex != view.scenarioRow || occurrence.objectIndex >= objects.size()) {
            continue;
        }
        const format::Object& object = objects[occurrence.objectIndex];
        for (const format::Slot& slot : sdk::object_slots(catalog, object)) {
            Choice choice;
            choice.label = std::string(sdk_mission_view::display_text(catalog, slot.name));
            choice.value = std::string(catalog.string(slot.id));
            if (choice.label.empty()) {
                choice.label = choice.value;
            }
            switch (slot.slotType) {
            case 1: choice.value = choice.label; g_lists.squads.push_back(std::move(choice)); break;
            case 2: choice.value = choice.label; g_lists.cells.push_back(std::move(choice)); break;
            case 3: g_lists.objectives.push_back(std::move(choice)); break;
            case 4: g_lists.objects.push_back(std::move(choice)); break;
            case 23: g_lists.devices.push_back(std::move(choice)); break;
            case 26: g_lists.effects.push_back(std::move(choice)); break;
            case 34: g_lists.filters.push_back(std::move(choice)); break;
            case 43: {
                choice.second = choice.value;   // slot id for set_scene_events
                choice.value = choice.label;    // symbol name for scene:activate
                for (const format::AuthoredSceneResource& resource : resources) {
                    const std::string_view id = catalog.string(resource.id);
                    if (id.find(choice.second) != std::string_view::npos) {
                        choice.extra = resource.resourceTag;
                        break;
                    }
                }
                g_lists.scenes.push_back(std::move(choice));
                break;
            }
            case 53: choice.extra = slot.reserved; g_lists.dialogue.push_back(std::move(choice)); break;
            default: break;
            }
        }
    }
    auto by_label = [](const Choice& a, const Choice& b) { return a.label < b.label; };
    for (auto* list : {&g_lists.squads, &g_lists.cells, &g_lists.objectives, &g_lists.objects,
                       &g_lists.devices, &g_lists.effects, &g_lists.filters, &g_lists.scenes}) {
        std::sort(list->begin(), list->end(), by_label);
    }
}

std::string_view snapshot_name(const catalog::Snapshot& snapshot, std::uint32_t row) noexcept {
    if (row >= snapshot.names.size()) {
        return {};
    }
    const catalog::Name& name = snapshot.names[row];
    if (name.selectedCandidate >= snapshot.nameCandidates.size()) {
        return {};
    }
    const catalog::NameCandidate& candidate = snapshot.nameCandidates[name.selectedCandidate];
    return {candidate.value.data(), candidate.length};
}

void rebuild_triggers() {
    const catalog::SnapshotView snapshot = catalog::snapshot();
    g_lists.triggers.clear();
    g_lists.triggersReady = snapshot != nullptr && snapshot->status == catalog::BuildStatus::ready;
    if (!g_lists.triggersReady) {
        return;
    }
    for (const catalog::TriggerVolumeOwner& owner : snapshot->triggerVolumeOwners) {
        if (owner.tableRow >= snapshot->triggerVolumeTables.size()
            || owner.objectRow >= snapshot->objects.size()) {
            continue;
        }
        const catalog::TriggerVolumeTable& table = snapshot->triggerVolumeTables[owner.tableRow];
        const catalog::Object& object = snapshot->objects[owner.objectRow];
        TriggerChoice choice;
        choice.registryKey = table.registryKey;
        choice.slotType = table.slotType;
        choice.slotIndex = table.slotIndex;
        choice.bubble = object.bubbleRow < snapshot->bubbles.size() ? snapshot->bubbles[object.bubbleRow].index : 0;
        std::string names;
        const std::size_t first = owner.firstIncomingReference;
        const std::size_t end = std::min(first + owner.incomingReferenceCount,
                                         snapshot->triggerVolumeIncomingReferences.size());
        for (std::size_t row = first; row < end; ++row) {
            const catalog::TriggerVolumeIncomingReference& incoming = snapshot->triggerVolumeIncomingReferences[row];
            if (incoming.sourceSlotRow >= snapshot->slots.size()) {
                continue;
            }
            const std::string_view name = snapshot_name(*snapshot, snapshot->slots[incoming.sourceSlotRow].nameRow);
            if (name.empty()) {
                continue;
            }
            if (!names.empty()) {
                names += " | ";
            }
            names.append(name);
        }
        char identity[64];
        std::snprintf(identity, sizeof identity, "0x%08X/%u/%u  b%u", table.registryKey,
                      static_cast<unsigned>(table.slotType), static_cast<unsigned>(table.slotIndex),
                      static_cast<unsigned>(choice.bubble));
        choice.label = names.empty() ? std::string(identity) : names + "   " + identity;
        g_lists.triggers.push_back(std::move(choice));
    }
    std::sort(g_lists.triggers.begin(), g_lists.triggers.end(),
              [](const TriggerChoice& a, const TriggerChoice& b) { return a.label < b.label; });
}

//--------------------------------------------------------------------------------------------
// Widgets.
//--------------------------------------------------------------------------------------------

/** InputText on a std::string through a fixed scratch buffer. */
bool input_string(const char* label, std::string& value, std::size_t capacity = 256) {
    static std::vector<char> buffer;
    buffer.assign(capacity, '\0');
    std::memcpy(buffer.data(), value.data(), std::min(value.size(), capacity - 1));
    if (ImGui::InputText(label, buffer.data(), capacity)) {
        value = buffer.data();
        return true;
    }
    return false;
}

bool input_hex(const char* label, std::uint32_t& value) {
    char buffer[16];
    std::snprintf(buffer, sizeof buffer, "%08X", value);
    if (ImGui::InputText(label, buffer, sizeof buffer, ImGuiInputTextFlags_CharsHexadecimal)) {
        value = static_cast<std::uint32_t>(std::strtoul(buffer, nullptr, 16));
        return true;
    }
    return false;
}

/** A filterable combo over one choice list; writes the chosen value (and optional second/extra). */
bool pick(const char* label, std::string& value, const std::vector<Choice>& choices,
          std::string* second = nullptr, std::uint32_t* extra = nullptr) {
    bool changed = false;
    const char* preview = value.empty() ? "(none)" : value.c_str();
    for (const Choice& choice : choices) {
        if (choice.value == value) {
            preview = choice.label.c_str();
            break;
        }
    }
    if (ImGui::BeginCombo(label, preview)) {
        static ImGuiTextFilter filter;
        filter.Draw("##filter", 220.0F);
        for (const Choice& choice : choices) {
            if (!filter.PassFilter(choice.label.c_str())) {
                continue;
            }
            const bool selected = choice.value == value;
            if (ImGui::Selectable(choice.label.c_str(), selected)) {
                value = choice.value;
                if (second != nullptr) {
                    *second = choice.second;
                }
                if (extra != nullptr) {
                    *extra = choice.extra;
                }
                changed = true;
            }
            if (selected) {
                ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::EndCombo();
    }
    return changed;
}

/** Checkbox list over the squad choices for a multi-squad field. */
void pick_squads(const char* label, std::vector<std::string>& values) {
    std::string preview = values.empty() ? "(none)" : std::to_string(values.size()) + " squad(s)";
    if (ImGui::BeginCombo(label, preview.c_str())) {
        static ImGuiTextFilter filter;
        filter.Draw("##squadfilter", 220.0F);
        for (const Choice& choice : g_lists.squads) {
            if (!filter.PassFilter(choice.label.c_str())) {
                continue;
            }
            const auto found = std::find(values.begin(), values.end(), choice.value);
            bool checked = found != values.end();
            if (ImGui::Checkbox(choice.label.c_str(), &checked)) {
                if (checked) {
                    values.push_back(choice.value);
                } else if (found != values.end()) {
                    values.erase(found);
                }
            }
        }
        ImGui::EndCombo();
    }
    if (!values.empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled("%s", [&] {
            static std::string joined;
            joined.clear();
            for (const std::string& v : values) {
                if (!joined.empty()) {
                    joined += ", ";
                }
                joined += v;
            }
            return joined.c_str();
        }());
    }
}

//--------------------------------------------------------------------------------------------
// Page state.
//--------------------------------------------------------------------------------------------

Document g_document{};
int g_selected{-1};
std::string g_fileName{"mission_towerfall.json"};
std::string g_status{};
bool g_dirty{};
bool g_loadedOnce{};
bool g_reloadQueued{};

void set_status(std::string text) {
    g_status = std::move(text);
}

void load_document() {
    std::string text;
    std::string error;
    if (!read_file(g_fileName, text, error)) {
        set_status("load: " + error);
        return;
    }
    Document loaded;
    if (!deserialize(text, loaded, error)) {
        set_status("load: " + error);
        return;
    }
    g_document = std::move(loaded);
    g_selected = g_document.sequences.empty() ? -1 : 0;
    g_dirty = false;
    set_status("loaded " + std::to_string(g_document.sequences.size()) + " sequence(s) from " + g_fileName);
}

void save_document() {
    std::string error;
    if (write_file(g_fileName, serialize(g_document), error)) {
        g_dirty = false;
        set_status("saved Sunrise/sequences/" + g_fileName);
    } else {
        set_status("save: " + error);
    }
}

void draw_start(Sequence& sequence) {
    ImGui::SetNextItemWidth(160.0F);
    if (ImGui::Combo("Start", &sequence.startKind, kStartKinds.data(), static_cast<int>(kStartKinds.size()))) {
        g_dirty = true;
    }
    const std::string_view kind = kStartKinds[static_cast<std::size_t>(sequence.startKind)];
    if (kind == "trigger_enter" || kind == "trigger_exit") {
        char preview[160];
        if (sequence.registryKey != 0) {
            std::snprintf(preview, sizeof preview, "%s  0x%08X/%u/%u", sequence.startName.c_str(),
                          sequence.registryKey, sequence.slotType, sequence.slotIndex);
        } else {
            std::snprintf(preview, sizeof preview, "%s", sequence.startName.empty() ? "(pick a volume)" : sequence.startName.c_str());
        }
        ImGui::SetNextItemWidth(520.0F);
        if (ImGui::BeginCombo("Volume", preview)) {
            static ImGuiTextFilter filter;
            filter.Draw("##volfilter", 260.0F);
            if (!g_lists.triggersReady) {
                ImGui::TextDisabled("World data not ready (open the Triggers page once).");
            }
            for (const TriggerChoice& choice : g_lists.triggers) {
                if (!filter.PassFilter(choice.label.c_str())) {
                    continue;
                }
                if (ImGui::Selectable(choice.label.c_str())) {
                    sequence.startName = choice.label.substr(0, choice.label.find("   "));
                    sequence.registryKey = choice.registryKey;
                    sequence.slotType = choice.slotType;
                    sequence.slotIndex = choice.slotIndex;
                    g_dirty = true;
                }
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Refresh")) {
            rebuild_triggers();
        }
        ImGui::SetNextItemWidth(200.0F);
        if (input_string("Name (fallback, arms by type-31 name)", sequence.startName)) {
            g_dirty = true;
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Clear identity")) {
            sequence.registryKey = 0;
            sequence.slotIndex = 0;
            g_dirty = true;
        }
        // An activity-level volume is resident from the start and can be crossed far from its
        // beat, so a start can wait for the client to hold the beat's region before arming.
        ImGui::SetNextItemWidth(120.0F);
        if (ImGui::InputInt("Arm in region (-1 = at start)", &sequence.startRegion)) {
            if (sequence.startRegion < -1) {
                sequence.startRegion = -1;
            }
            g_dirty = true;
        }
    } else if (kind == "squads_clear") {
        pick_squads("Squads (all seen alive, then 0)", sequence.startSquads);
    } else if (kind == "sequence_end") {
        std::vector<Choice> names;
        for (const Sequence& other : g_document.sequences) {
            if (&other != &sequence) {
                names.push_back({other.name, other.name, 0, {}});
            }
        }
        ImGui::SetNextItemWidth(260.0F);
        if (pick("After sequence", sequence.startName, names)) {
            g_dirty = true;
        }
    } else if (kind == "spawn") {
        ImGui::TextDisabled("Runs when the player spawns (bootflow 38).");
    } else {
        ImGui::TextDisabled("Started by hand: rt_cmd  require(\"mission_towerfall.sequencer\").start(context, \"%s\", \"manual\")",
                            sequence.name.c_str());
    }
}

void draw_step(Step& step, const Sequence& sequence) {
    ImGui::SetNextItemWidth(110.0F);
    if (ImGui::Combo("##kind", &step.kind, kStepKinds.data(), static_cast<int>(kStepKinds.size()))) {
        g_dirty = true;
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(90.0F);
    if (ImGui::InputInt("ms##delay", &step.delayMs, 0, 0)) {
        step.delayMs = std::max(step.delayMs, 0);
        g_dirty = true;
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Delay after the previous step (0 = same tick).");
    }
    ImGui::SameLine();
    const std::string_view kind = kStepKinds[static_cast<std::size_t>(step.kind)];
    ImGui::SetNextItemWidth(260.0F);
    if (kind == "squad") {
        if (pick("##squad", step.target, g_lists.squads)) {
            g_dirty = true;
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(60.0F);
        if (ImGui::InputInt("n##count", &step.count, 0, 0)) {
            g_dirty = true;
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Members per row (0 = package default).");
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(220.0F);
        if (pick("##objective", step.second, g_lists.objectives)) {
            g_dirty = true;
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("x##obj")) {
            step.second.clear();
            g_dirty = true;
        }
        ImGui::SameLine();
        if (ImGui::Checkbox("hold", &step.flagA)) {
            g_dirty = true;
        }
    } else if (kind == "retire") {
        if (pick("##squad", step.target, g_lists.squads)) {
            g_dirty = true;
        }
    } else if (kind == "cue") {
        ImGui::SetNextItemWidth(80.0F);
        if (ImGui::InputInt("cue", &step.count, 0, 0)) {
            g_dirty = true;
        }
        if (!g_lists.dialogue.empty()) {
            ImGui::SameLine();
            ImGui::TextDisabled("%u cues on %s", g_lists.dialogue.front().extra, g_lists.dialogue.front().label.c_str());
        }
    } else if (kind == "scene") {
        if (pick("##scene", step.target, g_lists.scenes, &step.second, &step.hashA)) {
            g_dirty = true;
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(90.0F);
        if (ImGui::Combo("##mode", &step.mode, kSceneModes.data(), static_cast<int>(kSceneModes.size()))) {
            g_dirty = true;
        }
        ImGui::SameLine();
        if (ImGui::Checkbox("new gen", &step.flagB)) {
            g_dirty = true;
        }
        ImGui::SameLine();
        ImGui::TextDisabled("res 0x%08X  keys %zu", step.hashA, step.keys.size());
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Keys: explicit list below, else discovered from the resource tag at spawn.\nSlot: %s",
                              step.second.c_str());
        }
        {
            static char keysText[512];
            std::string joined;
            for (const std::uint32_t key : step.keys) {
                char hex[12];
                std::snprintf(hex, sizeof hex, "%08X ", key);
                joined += hex;
            }
            std::snprintf(keysText, sizeof keysText, "%s", joined.c_str());
            ImGui::SetNextItemWidth(420.0F);
            if (ImGui::InputText("keys (hex, space separated)##keys", keysText, sizeof keysText)) {
                step.keys.clear();
                const char* cursor = keysText;
                while (*cursor != '\0') {
                    char* end = nullptr;
                    const unsigned long value = std::strtoul(cursor, &end, 16);
                    if (end == cursor) {
                        ++cursor;
                        continue;
                    }
                    step.keys.push_back(static_cast<std::uint32_t>(value));
                    cursor = end;
                }
                g_dirty = true;
            }
        }
    } else if (kind == "pose") {
        if (pick("##cell", step.target, g_lists.cells)) {
            g_dirty = true;
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(90.0F);
        if (input_hex("group", step.hashA)) {
            g_dirty = true;
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(90.0F);
        if (input_hex("action", step.hashB)) {
            g_dirty = true;
        }
        ImGui::SameLine();
        if (ImGui::Checkbox("new gen", &step.flagB)) {
            g_dirty = true;
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("A new program generation creates a fresh actor and replaces the previous one.");
        }
    } else if (kind == "despawn") {
        if (pick("##cell", step.target, g_lists.cells)) {
            g_dirty = true;
        }
        ImGui::SameLine();
        ImGui::TextDisabled("enabled=false on a fresh generation (removal probe)");
    } else if (kind == "effect") {
        if (pick("##effect", step.target, g_lists.effects)) {
            g_dirty = true;
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(220.0F);
        if (pick("##filter", step.second, g_lists.filters)) {
            g_dirty = true;
        }
        ImGui::SameLine();
        if (ImGui::Checkbox("enabled", &step.flagB)) {
            g_dirty = true;
        }
        ImGui::SameLine();
        if (ImGui::Checkbox("players", &step.flagA)) {
            g_dirty = true;
        }
    } else if (kind == "object") {
        if (pick("##object", step.target, g_lists.objects)) {
            g_dirty = true;
        }
        ImGui::SameLine();
        if (ImGui::Checkbox("active", &step.flagA)) {
            g_dirty = true;
        }
    } else if (kind == "device") {
        if (pick("##device", step.target, g_lists.devices)) {
            g_dirty = true;
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(80.0F);
        if (ImGui::InputFloat("pos", &step.position, 0.0F, 0.0F, "%.2f")) {
            g_dirty = true;
        }
        ImGui::SameLine();
        if (ImGui::Checkbox("snap", &step.flagA)) {
            g_dirty = true;
        }
    } else if (kind == "directive") {
        ImGui::SetNextItemWidth(100.0F);
        if (input_hex("hash", step.hashA)) {
            g_dirty = true;
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(200.0F);
        if (input_string("label", step.text)) {
            g_dirty = true;
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(160.0F);
        if (ImGui::InputInt2("progress", step.progress.data())) {
            g_dirty = true;
        }
        ImGui::SameLine();
        if (ImGui::Checkbox("raw", &step.flagA)) {
            g_dirty = true;
        }
    } else if (kind == "sequence") {
        std::vector<Choice> names;
        for (const Sequence& other : g_document.sequences) {
            if (&other != &sequence) {
                names.push_back({other.name, other.name, 0, {}});
            }
        }
        if (pick("##sequence", step.target, names)) {
            g_dirty = true;
        }
    } else if (kind == "clear") {
        ImGui::SetNextItemWidth(160.0F);
        if (input_string("name##clear", step.text)) {
            g_dirty = true;
        }
        ImGui::SameLine();
        pick_squads("##clearsquads", step.squads);
    } else if (kind == "probe") {
        ImGui::SetNextItemWidth(420.0F);
        if (input_string("##probe", step.text)) {
            g_dirty = true;
        }
    }
}

void draw_sequence(Sequence& sequence) {
    ImGui::SetNextItemWidth(220.0F);
    if (input_string("Name", sequence.name, 96)) {
        g_dirty = true;
    }
    ImGui::SameLine();
    if (ImGui::Checkbox("enabled", &sequence.enabled)) {
        g_dirty = true;
    }
    ImGui::SameLine();
    if (ImGui::Checkbox("once", &sequence.once)) {
        g_dirty = true;
    }
    draw_start(sequence);
    ImGui::SeparatorText("Steps");
    if (ImGui::SmallButton("+ step")) {
        sequence.steps.push_back({});
        g_dirty = true;
    }
    ImGui::SameLine();
    int total = 0;
    for (const Step& step : sequence.steps) {
        total += step.delayMs;
    }
    ImGui::TextDisabled("%zu step(s), %.1f s end to end", sequence.steps.size(), static_cast<double>(total) / 1000.0);
    int remove = -1;
    int moveUp = -1;
    int moveDown = -1;
    int cumulative = 0;
    for (std::size_t i = 0; i < sequence.steps.size(); ++i) {
        Step& step = sequence.steps[i];
        cumulative += step.delayMs;
        ImGui::PushID(static_cast<int>(i));
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("%2zu  t+%6.1fs", i + 1, static_cast<double>(cumulative) / 1000.0);
        ImGui::SameLine();
        if (ImGui::SmallButton("^")) {
            moveUp = static_cast<int>(i);
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("v")) {
            moveDown = static_cast<int>(i);
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("x")) {
            remove = static_cast<int>(i);
        }
        ImGui::SameLine();
        draw_step(step, sequence);
        ImGui::PopID();
    }
    if (remove >= 0) {
        sequence.steps.erase(sequence.steps.begin() + remove);
        g_dirty = true;
    } else if (moveUp > 0) {
        std::swap(sequence.steps[static_cast<std::size_t>(moveUp)], sequence.steps[static_cast<std::size_t>(moveUp) - 1]);
        g_dirty = true;
    } else if (moveDown >= 0 && static_cast<std::size_t>(moveDown) + 1 < sequence.steps.size()) {
        std::swap(sequence.steps[static_cast<std::size_t>(moveDown)], sequence.steps[static_cast<std::size_t>(moveDown) + 1]);
        g_dirty = true;
    }
}

} // namespace

void draw(const sdk::BoundView& view, const server::activity::host::InstanceSnapshot&) noexcept {
    if (view.catalog == nullptr) {
        ImGui::TextDisabled("No SDK view.");
        return;
    }
    if (g_lists.catalog != view.catalog.get() || g_lists.scenarioRow != view.scenarioRow) {
        rebuild_slot_lists(view);
        rebuild_triggers();
    } else if (!g_lists.triggersReady) {
        rebuild_triggers();
    }
    if (!g_loadedOnce) {
        g_loadedOnce = true;
        load_document();
    }

    // File bar.
    ImGui::SetNextItemWidth(220.0F);
    input_string("File##seqfile", g_fileName, 96);
    ImGui::SameLine();
    if (ImGui::Button("Load")) {
        load_document();
    }
    ImGui::SameLine();
    if (ImGui::Button(g_dirty ? "Save*" : "Save")) {
        save_document();
    }
    ImGui::SameLine();
    if (ImGui::Button("Save + hot reload")) {
        save_document();
        g_reloadQueued = mission::reload();
        set_status(g_status + (g_reloadQueued ? "; hot reload queued" : "; runtime unavailable"));
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Saves Sunrise/sequences/%s and restarts the mission script in place: every squad the\n"
                          "sequences place is retired, generations are rebased, triggers re-armed and the spawn\n"
                          "sequence replays. Program-created actors are replaced by their next pose. Sequences\n"
                          "started by a volume need the player to cross it again.",
                          g_fileName.c_str());
    }
    if (!g_status.empty()) {
        ImGui::TextDisabled("%s", g_status.c_str());
    }
    ImGui::Separator();

    // Sequence list on the left, editor on the right.
    if (ImGui::BeginChild("##seqlist", {230.0F, 0.0F}, ImGuiChildFlags_Borders)) {
        if (ImGui::SmallButton("+ sequence")) {
            Sequence fresh;
            fresh.name = "sequence_" + std::to_string(g_document.sequences.size() + 1);
            g_document.sequences.push_back(std::move(fresh));
            g_selected = static_cast<int>(g_document.sequences.size()) - 1;
            g_dirty = true;
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("dup") && g_selected >= 0) {
            Sequence copy = g_document.sequences[static_cast<std::size_t>(g_selected)];
            copy.name += "_copy";
            g_document.sequences.insert(g_document.sequences.begin() + g_selected + 1, std::move(copy));
            ++g_selected;
            g_dirty = true;
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("del") && g_selected >= 0) {
            g_document.sequences.erase(g_document.sequences.begin() + g_selected);
            g_selected = std::min(g_selected, static_cast<int>(g_document.sequences.size()) - 1);
            g_dirty = true;
        }
        for (std::size_t i = 0; i < g_document.sequences.size(); ++i) {
            const Sequence& sequence = g_document.sequences[i];
            ImGui::PushID(static_cast<int>(i));
            char label[160];
            std::snprintf(label, sizeof label, "%s%s  (%s, %zu)", sequence.enabled ? "" : "[off] ",
                          sequence.name.c_str(), kStartKinds[static_cast<std::size_t>(sequence.startKind)],
                          sequence.steps.size());
            if (ImGui::Selectable(label, g_selected == static_cast<int>(i))) {
                g_selected = static_cast<int>(i);
            }
            ImGui::PopID();
        }
    }
    ImGui::EndChild();
    ImGui::SameLine();
    if (ImGui::BeginChild("##seqeditor", {0.0F, 0.0F})) {
        if (g_selected >= 0 && static_cast<std::size_t>(g_selected) < g_document.sequences.size()) {
            draw_sequence(g_document.sequences[static_cast<std::size_t>(g_selected)]);
        } else {
            ImGui::TextDisabled("Add or select a sequence.");
        }
    }
    ImGui::EndChild();
}

} // namespace sunrise::server::ui::activity_host::sequencer_view
