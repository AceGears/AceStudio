// acestudio-cli rig: show, auto-rig and pose a project's rig (Rig.h, RigTools.h).

#include "CliCommon.h"
#include "ProjectArgs.h"

#include "acestudio/core/ProjectIO.h"
#include "acestudio/core/Rig.h"
#include "acestudio/core/RigTools.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <sstream>

namespace fs = std::filesystem;

namespace acestudio::cli {

namespace {

constexpr double kPi = 3.14159265358979323846;

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
    const auto kind = rigs::autoRigBodyFromString(a.opt("as").value_or("auto"));
    if (!kind) return out.fail(kUsage, "rig auto: --as wants auto, legs, wings or fins");
    auto doc = loadProject(a.positional[2]);
    if (!doc) return out.fail(doc.error());
    std::string err;
    const auto layer = findLayer(doc.value(), *layerKey, err);
    if (!layer) return out.fail(kUsage, "rig auto: " + err);
    rigs::AutoRigOptions opt;
    opt.body = *kind;
    // --arms "x,y,w,h;x,y,w,h" (';' or spaces between boxes): arms the
    // analysis missed, in canvas pixels.
    if (auto armsOpt = a.opt("arms")) {
        std::string arms = *armsOpt;
        std::replace(arms.begin(), arms.end(), ' ', ';');
        for (const std::string& item : splitOn(arms, ';')) {
            const auto v = splitOn(item, ',');
            std::vector<std::optional<int>> n;
            for (const std::string& s : v) n.push_back(parseInt(s));
            if (v.size() != 4 || !n[0] || !n[1] || !n[2] || !n[3]) return out.fail(kUsage, "rig auto: --arms wants x,y,w,h boxes separated by ';' or spaces, got '" + item + "'");
            opt.arms.push_back({*n[0], *n[1], *n[2], *n[3]});
        }
    }
    auto r = rigs::autoRig(doc.value(), *layer, frame, opt);
    if (!r) return out.fail(r.error());
    if (int code = writeProject(a, out, r.value().document, *outPath); code != kOk) return code;
    std::string human = "Rigged '" + layerName(doc.value(), *layer) + "' into " + *outPath + "\n";
    for (const auto& n : r.value().notes) human += "  " + n + "\n";
    return out.success({{"path", *outPath}, {"group", r.value().group}, {"notes", r.value().notes},
                        {"bones", r.value().document.rig()->bones.size()}},
                       human);
}


// Reads --turn "bone=deg,bone=deg" --move dx,dy --reach "chain=x,y;chain=x,y"
// into `pose`. Returns kOk, or the exit code of the reported failure.
int readPose(const Args& a, Output& out, const std::string& cmd, const Rig& rig, RigPose& pose) {
    if (auto t = a.opt("turn"))
        for (const std::string& item : splitOn(*t, ',')) {
            const auto eq = item.rfind('=');
            const Bone* b = eq == std::string::npos ? nullptr : rig.bone(item.substr(0, eq));
            const auto deg = eq == std::string::npos ? std::nullopt : parseDouble(item.substr(eq + 1));
            if (!b || !deg) return out.fail(kUsage, cmd + ": --turn wants bone=degrees, got '" + item + "'");
            pose.turn[b->id] = *deg * kPi / 180;
        }
    if (auto m = a.opt("move")) {
        const auto xy = splitOn(*m, ',');
        const auto dx = xy.size() == 2 ? parseDouble(xy[0]) : std::nullopt;
        const auto dy = xy.size() == 2 ? parseDouble(xy[1]) : std::nullopt;
        if (!dx || !dy) return out.fail(kUsage, cmd + ": --move wants dx,dy");
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
            if (!chain || !x || !y) return out.fail(kUsage, cmd + ": --reach wants chain=x,y, got '" + item + "'");
            if (auto st = rigs::reach(rig, pose, *chain, *x, *y); !st) return out.fail(st.error());
        }
    return kOk;
}

json turnsJson(const Rig& rig, const RigPose& pose) {
    json turns = json::object();
    for (const auto& [id, t] : pose.turn)
        if (const Bone* b = rig.bone(id)) turns[b->name] = rigs::clampTurn(*b, t) * 180 / kPi;
    return turns;
}

// Loads the project named by positional[2], which must have a rig, and the
// required --out path.
struct RigJob {
    Document doc;
    std::string outPath;
};
std::optional<RigJob> openRigJob(const Args& a, Output& out, const std::string& cmd, int& code) {
    if (a.positional.size() != 3) {
        code = out.fail(kUsage, cmd + ": expected one project file");
        return std::nullopt;
    }
    const auto outPath = a.opt("out");
    if (!outPath) {
        code = out.fail(kUsage, cmd + ": --out is required");
        return std::nullopt;
    }
    auto doc = loadProject(a.positional[2]);
    if (!doc) {
        code = out.fail(doc.error());
        return std::nullopt;
    }
    if (!doc.value().rig()) {
        code = out.fail(kUsage, cmd + ": the project has no rig");
        return std::nullopt;
    }
    return RigJob{std::move(doc).value(), *outPath};
}

// --frame N [--rest R] and a pose (readPose)
int rigPose(const Args& a, Output& out) {
    int code = kOk;
    auto job = openRigJob(a, out, "rig pose", code);
    if (!job) return code;
    const Rig& rig = *job->doc.rig();
    const int frame = optInt(a, "frame").value_or(0);
    const int rest = optInt(a, "rest").value_or(rig.restFrame);
    RigPose pose;
    if (int c = readPose(a, out, "rig pose", rig, pose); c != kOk) return c;
    auto baked = rigs::bakePose(job->doc, rest, frame, pose);
    if (!baked) return out.fail(baked.error());
    if (int c = writeProject(a, out, baked.value(), job->outPath); c != kOk) return c;
    return out.success({{"path", job->outPath}, {"frame", frame}, {"rest", rest}, {"turns", turnsJson(rig, pose)}},
                       "Posed frame " + std::to_string(frame) + " (parts from rest frame " + std::to_string(rest) + ") into " +
                           job->outPath + "\n");
}

// --frame N, a pose, [--curve linear|ease-in|ease-out|ease-in-out|stepped|bezier:x1,y1,x2,y2]
int rigKey(const Args& a, Output& out) {
    int code = kOk;
    auto job = openRigJob(a, out, "rig key", code);
    if (!job) return code;
    const auto frame = optInt(a, "frame");
    if (!frame) return out.fail(kUsage, "rig key: --frame is required");
    Rig rig = *job->doc.rig();
    RigPose pose;
    if (int c = readPose(a, out, "rig key", rig, pose); c != kOk) return c;
    Curve curve;
    if (auto cv = a.opt("curve")) {
        const std::string name = cv->substr(0, cv->find(':'));
        const auto type = rigs::curveFromString(name);
        if (!type) return out.fail(kUsage, "rig key: unknown --curve '" + name + "'");
        curve.type = *type;
        if (*type == CurveType::Bezier) {
            const auto colon = cv->find(':');
            const auto pts = colon == std::string::npos ? std::vector<std::string>{} : splitOn(cv->substr(colon + 1), ',');
            std::vector<double> v;
            for (const auto& p : pts)
                if (auto d = parseDouble(p)) v.push_back(*d);
            if (v.size() != 4 || pts.size() != 4) return out.fail(kUsage, "rig key: --curve bezier:x1,y1,x2,y2");
            curve.x1 = v[0];
            curve.y1 = v[1];
            curve.x2 = v[2];
            curve.y2 = v[3];
        }
    }
    if (auto st = rigs::setKey(rig, *frame, pose, curve); !st) return out.fail(st.error());
    job->doc.setRig(rig);
    if (auto st = job->doc.validate(); !st) return out.fail(kUsage, "rig key: " + st.error().message);
    if (int c = writeProject(a, out, job->doc, job->outPath); c != kOk) return c;
    return out.success({{"path", job->outPath}, {"frame", *frame}, {"keys", rig.keys.size()}, {"curve", rigs::toString(curve.type)},
                        {"turns", turnsJson(rig, pose)}},
                       "Key on frame " + std::to_string(*frame) + " (" + std::to_string(rig.keys.size()) + " keys) in " + job->outPath + "\n");
}

int rigUnkey(const Args& a, Output& out) {
    int code = kOk;
    auto job = openRigJob(a, out, "rig unkey", code);
    if (!job) return code;
    const auto frame = optInt(a, "frame");
    if (!frame) return out.fail(kUsage, "rig unkey: --frame is required");
    Rig rig = *job->doc.rig();
    if (auto st = rigs::removeKey(rig, *frame); !st) return out.fail(st.error());
    job->doc.setRig(rig);
    if (int c = writeProject(a, out, job->doc, job->outPath); c != kOk) return c;
    return out.success({{"path", job->outPath}, {"keys", rig.keys.size()}}, "Removed the key on frame " + std::to_string(*frame) + "\n");
}

int rigRest(const Args& a, Output& out) {
    int code = kOk;
    auto job = openRigJob(a, out, "rig rest", code);
    if (!job) return code;
    const auto frame = optInt(a, "frame");
    if (!frame) return out.fail(kUsage, "rig rest: --frame is required");
    Rig rig = *job->doc.rig();
    if (auto st = rigs::setRestFrame(rig, *frame); !st) return out.fail(st.error());
    job->doc.setRig(rig);
    if (auto st = job->doc.validate(); !st) return out.fail(kUsage, "rig rest: " + st.error().message);
    if (int c = writeProject(a, out, job->doc, job->outPath); c != kOk) return c;
    return out.success({{"path", job->outPath}, {"restFrame", *frame}}, "Rest frame is now " + std::to_string(*frame) + "\n");
}

// [--from F --to L]: default every frame from the first key to the last
int rigBake(const Args& a, Output& out) {
    int code = kOk;
    auto job = openRigJob(a, out, "rig bake", code);
    if (!job) return code;
    const int first = optInt(a, "from").value_or(0);
    const int last = optInt(a, "to").value_or(-1);
    auto baked = rigs::bakeKeys(job->doc, first, last);
    if (!baked) return out.fail(baked.error());
    if (int c = writeProject(a, out, baked.value(), job->outPath); c != kOk) return c;
    const Rig& rig = *job->doc.rig();
    const int f0 = first > last ? rig.keys.front().frame : first, f1 = first > last ? rig.keys.back().frame : last;
    return out.success({{"path", job->outPath}, {"from", f0}, {"to", f1}, {"restFrame", rig.restFrame}},
                       "Baked frames " + std::to_string(f0) + "-" + std::to_string(f1) + " into " + job->outPath + "\n");
}

} // namespace

int cmdRig(const Args& a, Output& out) {
    const std::string sub = a.positional.size() > 1 ? a.positional[1] : "";
    if (sub == "show") return rigShow(a, out);
    if (sub == "auto") return rigAuto(a, out);
    if (sub == "pose") return rigPose(a, out);
    if (sub == "key") return rigKey(a, out);
    if (sub == "unkey") return rigUnkey(a, out);
    if (sub == "rest") return rigRest(a, out);
    if (sub == "bake") return rigBake(a, out);
    if (sub == "motion") return cmdRigMotion(a, out);
    return out.fail(kUsage, "rig: expected show, auto, pose, key, unkey, rest, bake or motion");
}

} // namespace acestudio::cli
