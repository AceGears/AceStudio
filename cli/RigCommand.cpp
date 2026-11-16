// acestudio-cli rig: show, auto-rig and pose a project's rig (Rig.h, RigTools.h).

#include "CliCommon.h"

#include "acestudio/core/ProjectIO.h"
#include "acestudio/core/Rig.h"
#include "acestudio/core/RigTools.h"

#include <cmath>
#include <filesystem>
#include <sstream>

namespace fs = std::filesystem;

namespace acestudio::cli {

namespace {

constexpr double kPi = 3.14159265358979323846;

std::optional<double> parseDouble(const std::string& s) {
    try {
        std::size_t used = 0;
        const double v = std::stod(s, &used);
        if (used != s.size() || !std::isfinite(v)) return std::nullopt;
        return v;
    } catch (...) {
        return std::nullopt;
    }
}

std::vector<std::string> splitOn(const std::string& s, char c) {
    std::vector<std::string> out;
    std::string cur;
    std::istringstream in(s);
    while (std::getline(in, cur, c))
        if (!cur.empty()) out.push_back(cur);
    return out;
}

// A layer by id or by (unique) name.
std::optional<LayerId> findLayer(const Document& d, const std::string& key, std::string& err) {
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

std::string layerName(const Document& d, LayerId id) {
    const Layer* l = d.layer(id);
    return l ? l->name : "#" + std::to_string(id);
}

int writeProject(const Args& a, Output& out, const Document& d, const std::string& path) {
    std::error_code ec;
    if (fs::exists(path, ec) && !a.force) return out.fail(kIo, path + " already exists (use --force)");
    if (auto st = saveProject(d, path); !st) return out.fail(st.error());
    return kOk;
}

int rigShow(const Args& a, Output& out) {
    if (a.positional.size() != 3) return out.fail(kUsage, "rig show: expected one project file");
    auto doc = loadProject(a.positional[2]);
    if (!doc) return out.fail(doc.error());
    const Document& d = doc.value();
    if (!d.rig()) return out.success({{"rig", nullptr}}, a.positional[2] + ": no rig\n");
    const Rig& rig = *d.rig();
    json j = rigs::toJson(rig);
    json names = json::object();
    for (const auto& [layer, bone] : rig.bindings) names[std::to_string(layer)] = layerName(d, layer);
    j["layerNames"] = names;

    std::string human = a.positional[2] + ": " + std::to_string(rig.bones.size()) + " bones, " +
                        std::to_string(rig.bindings.size()) + " parts, " + std::to_string(rig.chains.size()) + " IK chains\n";
    // The tree, parents first, indented by depth.
    std::map<BoneId, int> depth;
    for (const Bone& b : rig.bones) {
        const int dpt = b.parent == kNoBone ? 0 : depth[b.parent] + 1;
        depth[b.id] = dpt;
        std::ostringstream line;
        line.precision(1);
        line << std::fixed << std::string(static_cast<std::size_t>(2 + 2 * dpt), ' ') << b.name << "  head " << b.x << "," << b.y
             << "  length " << b.length << "  angle " << b.angle * 180 / kPi;
        if (b.limited) line << "  limits " << b.minAngle * 180 / kPi << ".." << b.maxAngle * 180 / kPi;
        if (b.locked) line << "  locked";
        std::string parts;
        for (LayerId l : rig.parts(b.id)) parts += (parts.empty() ? "" : ", ") + layerName(d, l);
        if (!parts.empty()) line << "  parts: " << parts;
        human += line.str() + "\n";
    }
    for (const IkChain& c : rig.chains) {
        const Bone* t = rig.bone(c.tip);
        human += "  IK " + c.name + ": " + (t ? t->name : "?") + (c.bend > 0 ? " (bends clockwise)\n" : " (bends anticlockwise)\n");
    }
    return out.success({{"rig", j}}, human);
}

int rigAuto(const Args& a, Output& out) {
    if (a.positional.size() != 3) return out.fail(kUsage, "rig auto: expected one project file");
    const auto outPath = a.opt("out");
    const auto layerKey = a.opt("layer");
    if (!outPath || !layerKey) return out.fail(kUsage, "rig auto: --layer and --out are required");
    const int frame = optInt(a, "frame").value_or(0);
    auto doc = loadProject(a.positional[2]);
    if (!doc) return out.fail(doc.error());
    std::string err;
    const auto layer = findLayer(doc.value(), *layerKey, err);
    if (!layer) return out.fail(kUsage, "rig auto: " + err);
    auto r = rigs::autoRig(doc.value(), *layer, frame);
    if (!r) return out.fail(r.error());
    if (int code = writeProject(a, out, r.value().document, *outPath); code != kOk) return code;
    std::string human = "Rigged '" + layerName(doc.value(), *layer) + "' into " + *outPath + "\n";
    for (const auto& n : r.value().notes) human += "  " + n + "\n";
    return out.success({{"path", *outPath}, {"group", r.value().group}, {"notes", r.value().notes},
                        {"bones", r.value().document.rig()->bones.size()}},
                       human);
}

// --frame N --rest R --turn "bone=deg,bone=deg" --move dx,dy --reach "chain=x,y;chain=x,y"
int rigPose(const Args& a, Output& out) {
    if (a.positional.size() != 3) return out.fail(kUsage, "rig pose: expected one project file");
    const auto outPath = a.opt("out");
    if (!outPath) return out.fail(kUsage, "rig pose: --out is required");
    const int frame = optInt(a, "frame").value_or(0);
    const int rest = optInt(a, "rest").value_or(0);
    auto doc = loadProject(a.positional[2]);
    if (!doc) return out.fail(doc.error());
    if (!doc.value().rig()) return out.fail(kUsage, "rig pose: the project has no rig");
    const Rig& rig = *doc.value().rig();
    RigPose pose;
    if (auto t = a.opt("turn"))
        for (const std::string& item : splitOn(*t, ',')) {
            const auto eq = item.rfind('=');
            const Bone* b = eq == std::string::npos ? nullptr : rig.bone(item.substr(0, eq));
            const auto deg = eq == std::string::npos ? std::nullopt : parseDouble(item.substr(eq + 1));
            if (!b || !deg) return out.fail(kUsage, "rig pose: --turn wants bone=degrees, got '" + item + "'");
            pose.turn[b->id] = *deg * kPi / 180;
        }
    if (auto m = a.opt("move")) {
        const auto xy = splitOn(*m, ',');
        const auto dx = xy.size() == 2 ? parseDouble(xy[0]) : std::nullopt;
        const auto dy = xy.size() == 2 ? parseDouble(xy[1]) : std::nullopt;
        if (!dx || !dy) return out.fail(kUsage, "rig pose: --move wants dx,dy");
        pose.dx = *dx;
        pose.dy = *dy;
    }
    if (auto r = a.opt("reach"))
        for (const std::string& item : splitOn(*r, ';')) {
            const auto eq = item.rfind('=');
            std::optional<std::size_t> chain;
            if (eq != std::string::npos)
                for (std::size_t i = 0; i < rig.chains.size(); ++i)
                    if (rig.chains[i].name == item.substr(0, eq)) chain = i;
            const auto xy = eq == std::string::npos ? std::vector<std::string>{} : splitOn(item.substr(eq + 1), ',');
            const auto x = xy.size() == 2 ? parseDouble(xy[0]) : std::nullopt;
            const auto y = xy.size() == 2 ? parseDouble(xy[1]) : std::nullopt;
            if (!chain || !x || !y) return out.fail(kUsage, "rig pose: --reach wants chain=x,y, got '" + item + "'");
            if (auto st = rigs::reach(rig, pose, *chain, *x, *y); !st) return out.fail(st.error());
        }
    auto baked = rigs::bakePose(doc.value(), rest, frame, pose);
    if (!baked) return out.fail(baked.error());
    if (int code = writeProject(a, out, baked.value(), *outPath); code != kOk) return code;
    json turns = json::object();
    for (const auto& [id, t] : pose.turn)
        if (const Bone* b = rig.bone(id)) turns[b->name] = rigs::clampTurn(*b, t) * 180 / kPi;
    return out.success({{"path", *outPath}, {"frame", frame}, {"rest", rest}, {"turns", turns}},
                       "Posed frame " + std::to_string(frame) + " (parts from rest frame " + std::to_string(rest) + ") into " +
                           *outPath + "\n");
}

} // namespace

int cmdRig(const Args& a, Output& out) {
    const std::string sub = a.positional.size() > 1 ? a.positional[1] : "";
    if (sub == "show") return rigShow(a, out);
    if (sub == "auto") return rigAuto(a, out);
    if (sub == "pose") return rigPose(a, out);
    return out.fail(kUsage, "rig: expected show, auto or pose");
}

} // namespace acestudio::cli
