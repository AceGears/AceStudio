#pragma once
// Helpers shared by the commands that load, edit and save projects
// (rig, vector): numbers and lists from arguments, layers by id or name,
// and saving without overwriting unless --force.

#include "CliCommon.h"

#include "acestudio/core/ProjectIO.h"

#include <cmath>
#include <filesystem>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace acestudio::cli {

inline std::optional<double> parseDouble(const std::string& s) {
    try {
        std::size_t used = 0;
        const double v = std::stod(s, &used);
        if (used != s.size() || !std::isfinite(v)) return std::nullopt;
        return v;
    } catch (...) {
        return std::nullopt;
    }
}

inline std::vector<std::string> splitOn(const std::string& s, char c) {
    std::vector<std::string> out;
    std::string cur;
    std::istringstream in(s);
    while (std::getline(in, cur, c))
        if (!cur.empty()) out.push_back(cur);
    return out;
}

// A layer by id or by (unique) name.
inline std::optional<LayerId> findLayer(const Document& d, const std::string& key, std::string& err) {
    if (auto id = parseInt(key); id && *id > 0 && d.layer(static_cast<LayerId>(*id))) return static_cast<LayerId>(*id);
    std::optional<LayerId> found;
    for (const Layer& l : d.layers())
        if (l.name == key) {
            if (found) {
                err = "Two layers are named '" + key + "'; use its id";
                return std::nullopt;
            }
            found = l.id;
        }
    if (!found) err = "No layer '" + key + "'";
    return found;
}

inline std::string layerName(const Document& d, LayerId id) {
    const Layer* l = d.layer(id);
    return l ? l->name : "#" + std::to_string(id);
}

inline int writeProject(const Args& a, Output& out, const Document& d, const std::string& path) {
    std::error_code ec;
    if (std::filesystem::exists(path, ec) && !a.force) return out.fail(kIo, path + " already exists (use --force)");
    if (auto st = saveProject(d, path); !st) return out.fail(st.error());
    return kOk;
}

} // namespace acestudio::cli
