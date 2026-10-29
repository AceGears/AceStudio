#pragma once

// Typed, versioned, JSON-backed settings store.
//
// Keys are dotted paths ("canvas.grid.color"). Every key has a registered
// default and a type; set() rejects values of the wrong type, so a typo or
// a bad value from a plugin cannot corrupt the store. A corrupt settings
// file is never silently discarded: it is moved aside to
// "<file>.corrupt-<timestamp>" and the reason is reported.

#include "acestudio/core/Result.h"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace acestudio {

class Settings {
public:
    static constexpr int kSchemaVersion = 1;

    Settings();   // registers all built-in defaults

    // Loads from disk. Missing file -> defaults (ok). Corrupt file ->
    // quarantined, defaults used, and a warning returned in `warning`.
    Status load(const std::filesystem::path& path, std::string* warning = nullptr);
    Status save() const;
    Status saveTo(const std::filesystem::path& path) const;
    const std::filesystem::path& path() const noexcept { return m_path; }

    void registerDefault(const std::string& key, nlohmann::json value);
    bool hasKey(const std::string& key) const;
    std::vector<std::string> keys() const;

    nlohmann::json get(const std::string& key) const;   // value or default; null if unknown
    template <typename T> T get(const std::string& key) const { return get(key).template get<T>(); }

    Status set(const std::string& key, nlohmann::json value);
    void reset(const std::string& key);
    void resetAll();

    bool isDirty() const noexcept { return m_dirty; }

private:
    mutable std::mutex m_mutex;
    std::filesystem::path m_path;
    std::map<std::string, nlohmann::json> m_defaults;
    std::map<std::string, nlohmann::json> m_values;   // only overrides
    mutable bool m_dirty = false;
};

} // namespace acestudio
