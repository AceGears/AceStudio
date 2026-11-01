#include "acestudio/core/Settings.h"
#include "acestudio/core/FileUtil.h"
#include "acestudio/core/Log.h"

#include <algorithm>

namespace fs = std::filesystem;
using nlohmann::json;

namespace acestudio {

namespace {
bool sameKind(const json& a, const json& b) {
    if (a.is_number() && b.is_number()) {
        // Integer settings must stay integers; floats accept any number.
        return !(a.is_number_integer() && b.is_number_float());
    }
    return a.type() == b.type();
}
} // namespace

Settings::Settings() {
    // General
    registerDefault("general.language", "en");
    registerDefault("general.theme", "dark");                 // dark | light | highContrast
    registerDefault("general.textScale", 100);                // interface text size, % of the system font
    registerDefault("general.accentColor", "#0A6CF5");          // electric blue from the logo
    registerDefault("general.startup", "welcome");            // welcome | lastProject | empty
    registerDefault("general.recentProjects", json::array());
    registerDefault("general.maxRecentProjects", 12);
    registerDefault("general.autosaveIntervalSec", 120);
    registerDefault("ui.reducedMotion", false);               // also on when Windows turns animations off
    registerDefault("ui.animationSpeed", 100);                // percent: 50 = twice as fast, 200 = half speed
    registerDefault("ui.workspace", "home");                  // the workspace shown at start
    registerDefault("ui.showSplash", true);
    registerDefault("library.folders", json::array());        // extra folders the Asset Library scans
    registerDefault("library.tags", json::object());          // asset path -> tags
    registerDefault("library.favorites", json::array());
    // Canvas
    registerDefault("canvas.grid.visible", true);
    registerDefault("canvas.grid.color", "#FFFFFF1F");
    registerDefault("canvas.grid.minZoom", 8);
    registerDefault("canvas.checker.color1", "#2A2A31");
    registerDefault("canvas.checker.color2", "#34343C");
    registerDefault("canvas.checker.size", 8);
    registerDefault("canvas.background", "#1B1B20");
    registerDefault("canvas.zoomStep", 1.25);
    // Animation
    registerDefault("animation.defaultFps", 10);
    registerDefault("animation.onionSkin.enabled", false);
    registerDefault("animation.onionSkin.opacity", 0.35);
    registerDefault("animation.onionSkin.before", 1);
    registerDefault("animation.onionSkin.after", 1);
    registerDefault("animation.onionSkin.tint", true);
    registerDefault("animation.onionSkin.wrap", false);
    registerDefault("animation.preview.zoom", 4);
    // AI
    registerDefault("ai.modelDirectory", "");
    registerDefault("mcp.bridgeEnabled", true);               // AI assistants (acestudio-mcp) may use the open document
    registerDefault("ai.pythonPath", "");                     // empty = find Python 3.10+ automatically
    registerDefault("ai.autoStart", false);                   // start the worker when the app opens
    registerDefault("ai.outputDirectory", "");
    registerDefault("ai.vramLimitMb", 0);                     // 0 = auto
    registerDefault("ai.generate.lora", true);                // use the Pixel Art XL LoRA when installed
    registerDefault("ai.generate.loraStrength", 100);         // percent, 0..200
    registerDefault("ai.generate.cropSubject", true);         // frame the subject before pixelizing
    registerDefault("ai.generateOptionsOpen", false);          // Generate tab: Options section expanded
    // Export
    registerDefault("export.defaultEngine", "godot");
    // MCP
    registerDefault("mcp.enabled", false);
    registerDefault("mcp.port", 0);                           // 0 = stdio only
    // Advanced
    registerDefault("advanced.logLevel", "info");
    // Keyboard shortcut overrides {actionId: "Ctrl+Shift+N"}
    registerDefault("keyboard.shortcuts", json::object());
    registerDefault("export.presets", json::object());   // last-used options per export kind
    // Window state (opaque Qt blobs stored as base64)
    registerDefault("window.geometry", "");
    registerDefault("window.state", "");
    registerDefault("window.workspaces", json::object());
}

void Settings::registerDefault(const std::string& key, json value) {
    std::lock_guard lock(m_mutex);
    m_defaults[key] = std::move(value);
}

bool Settings::hasKey(const std::string& key) const {
    std::lock_guard lock(m_mutex);
    return m_defaults.count(key) > 0;
}

std::vector<std::string> Settings::keys() const {
    std::lock_guard lock(m_mutex);
    std::vector<std::string> out;
    out.reserve(m_defaults.size());
    for (const auto& [k, v] : m_defaults) out.push_back(k);
    return out;
}

json Settings::get(const std::string& key) const {
    std::lock_guard lock(m_mutex);
    if (auto it = m_values.find(key); it != m_values.end()) return it->second;
    if (auto it = m_defaults.find(key); it != m_defaults.end()) return it->second;
    return nullptr;
}

Status Settings::set(const std::string& key, json value) {
    std::lock_guard lock(m_mutex);
    auto def = m_defaults.find(key);
    if (def == m_defaults.end()) return Error{ErrorCode::InvalidArgument, "Unknown setting: " + key};
    if (!sameKind(def->second, value))
        return Error{ErrorCode::InvalidArgument, "Wrong type for setting " + key + ": expected " +
                                                     def->second.type_name() + ", got " + value.type_name()};
    if (value == def->second) m_values.erase(key);
    else m_values[key] = std::move(value);
    m_dirty = true;
    return {};
}

void Settings::reset(const std::string& key) {
    std::lock_guard lock(m_mutex);
    if (m_values.erase(key)) m_dirty = true;
}

void Settings::resetAll() {
    std::lock_guard lock(m_mutex);
    if (!m_values.empty()) m_dirty = true;
    m_values.clear();
}

Status Settings::load(const fs::path& path, std::string* warning) {
    {
        std::lock_guard lock(m_mutex);
        m_path = path;
        m_values.clear();
        m_dirty = false;
    }
    std::error_code ec;
    if (!fs::exists(path, ec)) return {};

    auto bytes = readFileBytes(path, 16u << 20);
    if (!bytes) return bytes.error();

    auto quarantine = [&](const std::string& reason) -> Status {
        std::string stamp = nowUtcIso8601();
        std::replace(stamp.begin(), stamp.end(), ':', '-');
        const fs::path aside = path.string() + ".corrupt-" + stamp;
        std::error_code mvEc;
        fs::rename(path, aside, mvEc);
        const std::string msg = "Settings file was unreadable (" + reason + "). Defaults restored; the old file was " +
                                (mvEc ? "left in place" : "kept as " + aside.string()) + ".";
        ACE_LOG_WARN("settings", msg);
        if (warning) *warning = msg;
        return {};
    };

    json doc = json::parse(bytes.value().begin(), bytes.value().end(), nullptr, false);
    if (doc.is_discarded() || !doc.is_object()) return quarantine("invalid JSON");
    const int version = doc.value("schemaVersion", 0);
    if (version > kSchemaVersion)
        return Error{ErrorCode::UnsupportedVersion,
                     "Settings were written by a newer AceStudio (schema " + std::to_string(version) + ")"};
    if (!doc.contains("values") || !doc["values"].is_object()) return quarantine("missing 'values'");

    std::lock_guard lock(m_mutex);
    std::vector<std::string> skipped;
    for (const auto& [k, v] : doc["values"].items()) {
        auto def = m_defaults.find(k);
        if (def == m_defaults.end() || !sameKind(def->second, v)) { skipped.push_back(k); continue; }
        m_values[k] = v;
    }
    if (!skipped.empty()) {
        std::string list;
        for (const auto& s : skipped) list += (list.empty() ? "" : ", ") + s;
        ACE_LOG_WARN("settings", "Ignored unknown or mistyped settings: " + list);
    }
    return {};
}

Status Settings::save() const {
    if (m_path.empty()) return Error{ErrorCode::InvalidArgument, "Settings path not set"};
    return saveTo(m_path);
}

Status Settings::saveTo(const fs::path& path) const {
    json doc;
    {
        std::lock_guard lock(m_mutex);
        doc["schemaVersion"] = kSchemaVersion;
        doc["values"] = json::object();
        for (const auto& [k, v] : m_values) doc["values"][k] = v;
    }
    const std::string text = doc.dump(2);
    auto st = writeFileAtomic(path, std::span(reinterpret_cast<const std::uint8_t*>(text.data()), text.size()));
    if (st) m_dirty = false;
    return st;
}

} // namespace acestudio
