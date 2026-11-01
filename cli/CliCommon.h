#pragma once

// Shared pieces of acestudio-cli: arguments, output (human or --json) and
// exit codes.

#include "acestudio/core/Result.h"

#include <nlohmann/json.hpp>

#include <charconv>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace acestudio::cli {

using nlohmann::json;

enum Exit : int { kOk = 0, kUsage = 1, kIo = 2, kCorrupt = 3, kUnsupported = 4, kInternal = 5, kScript = 6, kLicence = 7 };

inline int exitFor(ErrorCode c) {
    switch (c) {
    case ErrorCode::Ok: return kOk;
    case ErrorCode::InvalidArgument: return kUsage;
    case ErrorCode::IoError:
    case ErrorCode::NotFound:
    case ErrorCode::PermissionDenied: return kIo;
    case ErrorCode::CorruptData: return kCorrupt;
    case ErrorCode::UnsupportedVersion: return kUnsupported;
    case ErrorCode::Cancelled:
    case ErrorCode::Internal: return kInternal;
    }
    return kInternal;
}

struct Args {
    std::vector<std::string> positional;
    std::map<std::string, std::string> options;   // --key value
    bool json = false;
    bool force = false;
    std::set<std::string> flags;   // boolean options such as --trim

    std::optional<std::string> opt(const std::string& k) const {
        auto it = options.find(k);
        if (it == options.end()) return std::nullopt;
        return it->second;
    }
};

class Output {
public:
    explicit Output(bool asJson) : m_json(asJson) {}

    int fail(int code, const std::string& message) {
        if (m_json) std::cout << json{{"ok", false}, {"exitCode", code}, {"error", message}}.dump() << "\n";
        else std::cerr << "error: " << message << "\n";
        return code;
    }
    int fail(const Error& e) { return fail(exitFor(e.code), e.describe()); }
    // A failure with extra JSON fields (JSON mode only).
    int fail(int code, const std::string& message, const json& extra) {
        if (!m_json) return fail(code, message);
        json j{{"ok", false}, {"exitCode", code}, {"error", message}};
        for (const auto& [k, v] : extra.items()) j[k] = v;
        std::cout << j.dump() << "\n";
        return code;
    }

    int success(json payload, const std::string& human) {
        if (m_json) {
            payload["ok"] = true;
            std::cout << payload.dump() << "\n";
        } else {
            std::cout << human;
        }
        return kOk;
    }

    bool isJson() const { return m_json; }

private:
    bool m_json;
};

inline std::optional<int> parseInt(const std::string& s) {
    int v = 0;
    auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
    if (ec != std::errc() || p != s.data() + s.size()) return std::nullopt;
    return v;
}

inline std::optional<int> optInt(const Args& a, const std::string& key) {
    auto v = a.opt(key);
    return v ? parseInt(*v) : std::nullopt;
}

int cmdAi(const Args& a, Output& out);
int cmdTileset(const Args& a, Output& out);
int cmdSeams(const Args& a, Output& out);
int cmdAtlas(const Args& a, Output& out);
int cmdSlice(const Args& a, Output& out);
int cmdUiKit(const Args& a, Output& out);
int cmdAnimate(const Args& a, Output& out);
int cmdTemplate(const Args& a, Output& out);
int cmdRun(const Args& a, Output& out);

} // namespace acestudio::cli
