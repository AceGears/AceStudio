// Path confinement for AI worker jobs (SEC-005). See AiJobPaths.h.

#include "acestudio/core/AiJobPaths.h"

#include <algorithm>
#include <cctype>
#include <cwctype>
#include <string>

namespace fs = std::filesystem;
using nlohmann::json;

namespace acestudio::ai {

namespace {

enum class Kind { Input, Location, Name };

fs::path fromUtf8(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }

std::string utf8(const fs::path& p) {
    const auto u = p.u8string();
    return std::string(u.begin(), u.end());
}

// The path's components after resolving "..", "." and existing symbolic
// links, without empty trailing elements; case-folded on Windows, where
// file names are case-insensitive.
std::vector<fs::path::string_type> components(const fs::path& p) {
    std::error_code ec;
    fs::path r = fs::weakly_canonical(p, ec);
    if (ec) r = p.lexically_normal();
    r.make_preferred();
    std::vector<fs::path::string_type> out;
    for (const fs::path& e : r) {
        fs::path::string_type s = e.native();
        if (s.empty()) continue;
#ifdef _WIN32
        for (auto& ch : s) ch = static_cast<wchar_t>(std::towlower(static_cast<std::wint_t>(ch)));
#endif
        out.push_back(std::move(s));
    }
    return out;
}

bool samePath(const fs::path& a, const fs::path& b) {
    if (a.empty() || b.empty() || !a.is_absolute() || !b.is_absolute()) return false;
    return components(a) == components(b);
}

// A name relative to a folder: no root, no drive, no "..".
bool safeRelativeName(const std::string& s) {
    if (s.empty()) return false;
    const fs::path p = fromUtf8(s);
    if (p.is_absolute() || p.has_root_name() || p.has_root_directory()) return false;
    std::size_t start = 0;
    for (std::size_t i = 0; i <= s.size(); ++i) {
        if (i < s.size() && s[i] != '/' && s[i] != '\\') continue;
        if (s.compare(start, i - start, "..") == 0 && i - start == 2) return false;
        start = i + 1;
    }
    return true;
}

Status bad(const std::string& key, const std::string& why) {
    return Error{ErrorCode::PermissionDenied, "AI job parameter '" + key + "' " + why};
}

Status checkValue(const std::string& key, const json& v, Kind kind, const JobPathPolicy& policy) {
    if (v.is_null()) return Status::success();
    if (!v.is_string()) return Error{ErrorCode::InvalidArgument, "AI job parameter '" + key + "' must be a path"};
    const std::string s = v.get<std::string>();
    if (s.empty()) return Status::success();   // the worker treats "" as not given
    if (kind == Kind::Name)
        return safeRelativeName(s) ? Status::success() : bad(key, "must be a relative name without '..': " + s);
    const fs::path p = fromUtf8(s);
    if (!p.is_absolute()) return bad(key, "must be an absolute path: " + s);
    if (!policy.jobDir.empty() && pathIsInside(p, policy.jobDir)) return Status::success();
    for (const fs::path& root : policy.roots)
        if (pathIsInside(p, root)) return Status::success();
    if (kind == Kind::Input)
        for (const fs::path& f : policy.userFiles)
            if (samePath(p, f)) return Status::success();
    return bad(key, "is outside AceStudio's AI folders: " + utf8(p));
}

Status checkArray(const std::string& key, const json& v, Kind kind, const JobPathPolicy& policy) {
    if (v.is_null()) return Status::success();
    if (!v.is_array()) return Error{ErrorCode::InvalidArgument, "AI job parameter '" + key + "' must be a list of paths"};
    for (std::size_t i = 0; i < v.size(); ++i)
        if (auto st = checkValue(key + "[" + std::to_string(i) + "]", v[i], kind, policy); !st) return st;
    return Status::success();
}

// "input12" for prefix "input".
bool numbered(const std::string& key, const char* prefix) {
    const std::string pre(prefix);
    if (key.size() <= pre.size() || key.compare(0, pre.size(), pre) != 0) return false;
    return std::all_of(key.begin() + static_cast<std::ptrdiff_t>(pre.size()), key.end(),
                       [](char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; });
}

} // namespace

bool pathIsInside(const fs::path& path, const fs::path& root) {
    if (path.empty() || root.empty() || !path.is_absolute() || !root.is_absolute()) return false;
    const auto p = components(path);
    const auto r = components(root);
    return !r.empty() && r.size() <= p.size() && std::equal(r.begin(), r.end(), p.begin());
}

Status checkJobPaths(const json& params, const JobPathPolicy& policy) {
    if (!params.is_object()) return Status::success();
    for (const auto& [key, value] : params.items()) {
        Status st;
        if (key == "input" || key == "init_image" || key == "mask" || numbered(key, "input") || numbered(key, "reference"))
            st = checkValue(key, value, Kind::Input, policy);
        else if (key == "references")
            st = checkArray(key, value, Kind::Input, policy);
        else if (key == "model" || key == "model_dir" || key == "vae_dir" || key == "dest_dir")
            st = checkValue(key, value, Kind::Location, policy);
        else if ((key == "lora" || key == "lora_preload") && value.is_object() && value.contains("path"))
            st = checkValue(key + ".path", value["path"], Kind::Location, policy);
        else if (key == "preview" && value.is_object() && value.contains("dir"))
            st = checkValue("preview.dir", value["dir"], Kind::Location, policy);
        else if (key == "cutout" && value.is_object() && value.contains("model"))
            st = checkValue("cutout.model", value["model"], Kind::Location, policy);
        else if (key == "controlnet" && value.is_object()) {
            if (value.contains("dir")) st = checkValue("controlnet.dir", value["dir"], Kind::Location, policy);
            if (st && value.contains("image")) st = checkValue("controlnet.image", value["image"], Kind::Input, policy);
        } else if (key == "ip_adapter" && value.is_object()) {
            if (value.contains("dir")) st = checkValue("ip_adapter.dir", value["dir"], Kind::Location, policy);
            if (st && value.contains("images")) st = checkArray("ip_adapter.images", value["images"], Kind::Input, policy);
            if (st && value.contains("weight")) st = checkValue("ip_adapter.weight", value["weight"], Kind::Name, policy);
            if (st && value.contains("encoder")) st = checkValue("ip_adapter.encoder", value["encoder"], Kind::Name, policy);
        } else if (key == "files" && value.is_array()) {
            for (std::size_t i = 0; st && i < value.size(); ++i)
                if (value[i].is_object() && value[i].contains("path"))
                    st = checkValue("files[" + std::to_string(i) + "].path", value[i]["path"], Kind::Name, policy);
        }
        if (!st) return st;
    }
    return Status::success();
}

} // namespace acestudio::ai
