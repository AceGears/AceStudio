// acestudio-cli retarget: 3D skeletal motion (BVH, glTF/GLB) onto a 2D sprite.

#include "CliCommon.h"

#include "acestudio/core/AnimCodec.h"
#include "acestudio/core/DirectionSet.h"
#include "acestudio/core/FileUtil.h"
#include "acestudio/core/Mocap.h"
#include "acestudio/core/MotionLibrary.h"
#include "acestudio/core/PngCodec.h"
#include "acestudio/core/ProjectIO.h"
#include "acestudio/core/RigMotion.h"
#include "acestudio/core/RigTools.h"
#include "ProjectArgs.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace acestudio::cli {

namespace fs = std::filesystem;

namespace {

std::optional<double> optDouble(const Args& a, const std::string& key, std::string& err) {
    auto v = a.opt(key);
    if (!v) return std::nullopt;
    try {
        std::size_t used = 0;
        const double d = std::stod(*v, &used);
        if (used != v->size()) throw std::invalid_argument(key);
        return d;
    } catch (...) {
        err = "--" + key + " expects a number";
        return std::nullopt;
    }
}

Status writeClip(const motion::Clip& c, const fs::path& png, const fs::path& gif) {
    const int fw = c.frames[0].width(), fh = c.frames[0].height();
    Image strip(fw * static_cast<int>(c.frames.size()), fh);
    std::vector<AnimFrame> anim;
    for (std::size_t i = 0; i < c.frames.size(); ++i) {
        strip.blit(c.frames[i], static_cast<int>(i) * fw, 0);
        anim.push_back({c.frames[i], c.durations[i]});
    }
    if (auto st = writePng(strip, png); !st) return st;
    auto bytes = encodeGif(anim, 0);
    if (!bytes) return bytes.error();
    return writeFileAtomic(gif, bytes.value());
}

// --library <folder>, else "<user data>/motions".
Result<fs::path> libraryDir(const Args& a) {
    if (auto d = a.opt("library")) return fs::path(*d);
    return mocap::defaultPresetDir();
}

// The motion argument: a .bvh/.glb/.gltf file or "preset:<name>" (a built-in
// motion or one saved in the motion library), with --animation for glTF.
struct LoadedMotion {
    mocap::Motion motion;
    bool preset = false, oneShot = false;
    std::string name;   // the preset name or the file's stem
};
std::optional<LoadedMotion> loadMotionArg(const Args& a, Output& out, const std::string& cmd, const std::string& arg, int& code) {
    LoadedMotion lm;
    lm.preset = arg.rfind("preset:", 0) == 0;
    const fs::path motionPath = arg;
    lm.name = lm.preset ? arg.substr(7) : motionPath.stem().string();
    std::string ext = motionPath.extension().string();
    for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (!lm.preset && ext != ".bvh" && ext != ".glb" && ext != ".gltf") {
        code = out.fail(kUnsupported, cmd + ": motion is read from BVH, glTF or GLB, or a preset (preset:walk; see 'motion list'). Convert FBX first "
                                            "(Blender: File > Import > FBX, then File > Export > glTF 2.0 (.glb)).");
        return std::nullopt;
    }
    mocap::GltfOptions gopt;
    if (auto anim = a.opt("animation")) {
        if (lm.preset || ext == ".bvh") {
            code = out.fail(kUsage, cmd + ": --animation is for glTF/GLB files (a BVH or a preset holds one motion)");
            return std::nullopt;
        }
        gopt.animation = *anim;   // a name, or an index
    }
    Result<mocap::Motion> motion = Error{ErrorCode::Internal, "no motion"};
    if (lm.preset) {
        auto dir = libraryDir(a);
        if (!dir) {
            code = out.fail(dir.error());
            return std::nullopt;
        }
        motion = mocap::loadPreset(lm.name, dir.value());
        for (const auto& p : mocap::builtInPresets())
            if (p.name == lm.name) lm.oneShot = !p.loops;
    } else {
        motion = mocap::loadMotion(motionPath, gopt);
    }
    if (!motion) {
        code = out.fail(motion.error());
        return std::nullopt;
    }
    lm.motion = std::move(motion).value();
    return lm;
}

// Timing, camera and mapping options shared by retarget and rig motion:
// --view, --turn/--tilt/--roll/--fov/--perspective, --start, --length,
// --amount, --fps, --frames, --travel, --no-loop, --map. `drawnTurn` gives
// the turn to use when only tilt, roll or fov are given.
struct MotionArgs {
    std::optional<mocap::Camera> camera;
    double start = 0, length = 0, amount = 1;
    int fps = 12, frames = 0;
    bool inPlace = true, loop = true;
    std::vector<std::pair<mocap::Bone, std::string>> mapOverrides;
};
int readMotionArgs(const Args& a, Output& out, const std::string& cmd, bool oneShot, const std::function<double()>& drawnTurn, MotionArgs& o) {
    std::string err;
    if (auto v = a.opt("view")) {
        if (*v != "auto") {
            o.camera = mocap::cameraPreset(*v);
            if (!o.camera) {
                std::string names;
                for (const auto& n : mocap::cameraPresetNames()) names += (names.empty() ? "" : ", ") + n;
                return out.fail(kUsage, cmd + ": --view expects auto or one of " + names);
            }
        }
    }
    const auto turn = optDouble(a, "turn", err), tilt = optDouble(a, "tilt", err), roll = optDouble(a, "roll", err);
    const auto start = optDouble(a, "start", err), length = optDouble(a, "length", err), amount = optDouble(a, "amount", err);
    const auto fov = optDouble(a, "fov", err);
    if (!err.empty()) return out.fail(kUsage, cmd + ": " + err);
    if (turn || tilt || roll || fov || a.flags.count("perspective")) {
        if (!o.camera) o.camera = mocap::Camera{};
        // Turn defaults to the drawn view when only tilt/roll are given.
        if ((!a.opt("view") || *a.opt("view") == "auto") && !turn) o.camera->turn = drawnTurn();
        if (turn) o.camera->turn = *turn;
        if (tilt) o.camera->tilt = *tilt;
        if (roll) o.camera->roll = *roll;
        if (fov) o.camera->fov = *fov;
        if (a.flags.count("perspective")) o.camera->perspective = true;
    }
    if (start) o.start = *start;
    if (length) o.length = *length;
    if (amount) o.amount = *amount;
    o.fps = optInt(a, "fps").value_or(12);
    o.frames = optInt(a, "frames").value_or(0);
    o.inPlace = a.flags.count("travel") == 0;
    o.loop = a.flags.count("no-loop") == 0 && !oneShot;   // jump, attack and hurt play once
    // --map Bone=joint,Bone=joint
    if (auto mp = a.opt("map")) {
        std::size_t s = 0;
        while (s <= mp->size()) {
            const std::size_t comma = mp->find(',', s);
            const std::string item = mp->substr(s, comma == std::string::npos ? std::string::npos : comma - s);
            const std::size_t eq = item.find('=');
            const auto bone = eq == std::string::npos ? std::nullopt : mocap::boneFromString(item.substr(0, eq));
            if (!bone) return out.fail(kUsage, cmd + ": --map expects Bone=joint pairs (Bone: Hips, LThigh, RShin, LUpperArm, ...): '" + item + "'");
            o.mapOverrides.emplace_back(*bone, item.substr(eq + 1));
            if (comma == std::string::npos) break;
            s = comma + 1;
        }
    }
    return kOk;
}

// --body auto|biped|quadruped|flyer|swimmer: the kind of figure the sprite
// is. auto reads it from the motion and the sprite (mocap::autoBody) when a
// motion is given: wing bones pick flyer, a legless skeleton swimmer, else a
// four-legged sprite takes four-legged motion. `why` gets a note when the
// motion decided. Without a motion, auto is by the sprite's legs only.
int readBody(const Args& a, Output& out, const std::string& cmd, const Image& sprite, motion::Body& body,
             const mocap::Motion* mo = nullptr, std::string* why = nullptr) {
    const std::string v = a.opt("body").value_or("auto");
    if (v == "biped") body = motion::Body::Biped;
    else if (v == "quadruped") body = motion::Body::Quadruped;
    else if (v == "flyer") body = motion::Body::Flyer;
    else if (v == "swimmer") body = motion::Body::Swimmer;
    else if (v == "auto")
        body = mo ? mocap::autoBody(sprite, *mo, why)
                  : (motion::analyzeSubject(sprite).body == motion::Body::Quadruped ? motion::Body::Quadruped : motion::Body::Biped);
    else return out.fail(kUsage, cmd + ": --body expects auto, biped, quadruped, flyer or swimmer");
    return kOk;
}

} // namespace

int cmdRetarget(const Args& a, Output& out) {
    if (a.positional.size() != 3) return out.fail(kUsage, "retarget: expected a sprite PNG and a motion file (.bvh, .glb or .gltf)");
    const auto outDir = a.opt("out");
    if (!outDir) return out.fail(kUsage, "retarget: --out (a folder) is required");
    const fs::path spritePath = a.positional[1];
    int code = kOk;
    auto loaded = loadMotionArg(a, out, "retarget", a.positional[2], code);
    if (!loaded) return code;
    auto sprite = readPng(spritePath);
    if (!sprite) return out.fail(sprite.error());
    const mocap::Motion& mo = loaded->motion;

    MotionArgs ma;
    const auto spriteTurn = [&] {
        const motion::Subject s = motion::analyzeSubject(sprite.value());
        return s.frontView ? 0.0 : (s.facing < 0 ? -90.0 : 90.0);
    };
    if (int c = readMotionArgs(a, out, "retarget", loaded->oneShot, spriteTurn, ma); c != kOk) return c;
    mocap::RetargetOptions o;
    o.camera = ma.camera;
    o.start = ma.start;
    o.length = ma.length;
    o.amount = ma.amount;
    o.fps = ma.fps;
    o.frames = ma.frames;
    o.inPlace = ma.inPlace;
    o.loop = ma.loop;
    o.mapOverrides = ma.mapOverrides;
    std::string whyBody;
    if (int c = readBody(a, out, "retarget", sprite.value(), o.body, &mo, &whyBody); c != kOk) return c;
    if (auto v = a.opt("wing-line")) {
        try {
            o.wingLine = std::stod(*v);
        } catch (const std::exception&) {
            o.wingLine = -1;
        }
        if (!(o.wingLine > 0 && o.wingLine < 1)) return out.fail(kUsage, "retarget: --wing-line expects a fraction of the height, 0 to 1");
    }

    auto res = mocap::retarget(sprite.value(), mo, o);
    if (!res) return out.fail(res.error());
    mocap::RetargetResult made = std::move(res).value();
    if (!whyBody.empty()) made.notes.insert(made.notes.begin(), whyBody);
    const mocap::RetargetResult& r = made;

    const fs::path dir = *outDir;
    std::error_code ec;
    fs::create_directories(dir, ec);
    const std::string stem = spritePath.stem().string() + "_" + a.opt("name").value_or(loaded->name);
    const fs::path png = dir / (stem + ".png"), gif = dir / (stem + ".gif");
    const fs::path srcPng = dir / (stem + "_source.png"), srcGif = dir / (stem + "_source.gif");
    for (const fs::path& p : {png, gif, srcPng, srcGif})
        if (!a.force && fs::exists(p, ec)) return out.fail(kIo, p.string() + " already exists (use --force)");
    if (auto st = writeClip(r.clip, png, gif); !st) return out.fail(st.error());
    if (auto st = writeClip(r.source, srcPng, srcGif); !st) return out.fail(st.error());

    const motion::ClipCheck chk = motion::checkClip(r.clip);
    json mapped = json::object();
    for (int b = 0; b < mocap::kBoneCount; ++b) {
        const int j = r.map.joint[static_cast<std::size_t>(b)];
        mapped[std::string(mocap::toString(static_cast<mocap::Bone>(b)))] = j >= 0 ? json(mo.joints[static_cast<std::size_t>(j)].name) : json(nullptr);
    }
    // Four-legged motion: each leg by its top joint, hind legs first.
    json legs = json::array();
    for (const auto& l : r.creature.legs)
        legs.push_back({{"top", mo.joints[static_cast<std::size_t>(l.joints.front())].name}, {"hind", l.hind}});
    std::string text = "Wrote " + png.string() + " and " + gif.string() + " (" + std::to_string(r.clip.frames.size()) + " frames), and the source skeleton beside them\n";
    for (const auto& n : r.notes) text += "note: " + n + "\n";
    for (const auto& i : chk.issues) text += "check: " + i + "\n";
    return out.success({{"files", {png.string(), gif.string(), srcPng.string(), srcGif.string()}},
                        {"frames", r.clip.frames.size()},
                        {"frameWidth", r.clip.frames[0].width()},
                        {"frameHeight", r.clip.frames[0].height()},
                        {"frameMs", r.clip.durations[0]},
                        {"camera", {{"turn", r.camera.turn}, {"tilt", r.camera.tilt}, {"roll", r.camera.roll}, {"perspective", r.camera.perspective}}},
                        {"approximateView", r.approximateView},
                        {"bones", mapped},
                        {"body", motion::toString(o.body)},
                        {"legs", legs},
                        {"notes", r.notes},
                        {"issues", chk.issues}},
                       text);
}

// acestudio-cli rig motion <project> <motion|preset:NAME> --out <project>:
// the motion as keys on the project's rig (RigMotion.h).
int cmdRigMotion(const Args& a, Output& out) {
    if (a.positional.size() != 4) return out.fail(kUsage, "rig motion: expected a project and a motion (.bvh, .glb, .gltf or preset:NAME)");
    const auto outPath = a.opt("out");
    if (!outPath) return out.fail(kUsage, "rig motion: --out is required");
    int code = kOk;
    auto loaded = loadMotionArg(a, out, "rig motion", a.positional[3], code);
    if (!loaded) return code;
    auto doc = loadProject(a.positional[2]);
    if (!doc) return out.fail(doc.error());
    if (!doc.value().rig()) return out.fail(kUsage, "rig motion: the project has no rig (rig auto, or rig one in the app)");
    const Rig& rig = *doc.value().rig();

    MotionArgs ma;
    if (int c = readMotionArgs(a, out, "rig motion", loaded->oneShot, [&] { return rigs::drawnCamera(rig).turn; }, ma); c != kOk) return c;
    rigs::MotionKeysOptions o;
    o.camera = ma.camera;
    o.start = ma.start;
    o.length = ma.length;
    o.amount = ma.amount;
    o.fps = ma.fps;
    o.frames = ma.frames;
    o.inPlace = ma.inPlace;
    o.loop = ma.loop;
    o.mapOverrides = ma.mapOverrides;
    o.armsFromDrawing = a.flags.count("absolute-arms") == 0;
    if (a.opt("first")) {
        const auto f = optInt(a, "first");
        if (!f || *f < 0) return out.fail(kUsage, "rig motion: --first expects a frame number");
        o.first = *f;
    }
    // --bones "rig bone=Joint,..."
    if (auto bs = a.opt("bones"))
        for (const std::string& item : splitOn(*bs, ',')) {
            const auto eq = item.rfind('=');
            const auto joint = eq == std::string::npos ? std::nullopt : mocap::boneFromString(item.substr(eq + 1));
            if (!joint) return out.fail(kUsage, "rig motion: --bones expects rig bone=Joint pairs (Joint: Hips, LThigh, RShin, LUpperArm, ...): '" + item + "'");
            o.bones.emplace_back(item.substr(0, eq), *joint);
        }

    // --facing left|right (four-legged motion: overrides the facing read from the art)
    if (auto v = a.opt("facing")) {
        if (*v == "left") o.facing = -1;
        else if (*v == "right") o.facing = 1;
        else return out.fail(kUsage, "rig motion: --facing expects left or right");
    }
    // --legs "0=hind-near,1=hind-far,2=front-near,3=front-far"
    if (auto ls = a.opt("legs"))
        for (const std::string& item : splitOn(*ls, ',')) {
            const auto eq = item.find('=');
            const std::string num = eq == std::string::npos ? std::string() : item.substr(0, eq);
            const std::string which = eq == std::string::npos ? std::string() : item.substr(eq + 1);
            const auto q = rigs::quadLegFromString(which);
            const bool digits = !num.empty() && num.size() <= 3 && std::all_of(num.begin(), num.end(), [](char c) { return c >= '0' && c <= '9'; });
            if (!q || !digits)
                return out.fail(kUsage, "rig motion: --legs expects N=hind-near|hind-far|front-near|front-far pairs: '" + item + "'");
            o.legs.emplace_back(std::stoi(num), *q);
        }

    auto res = rigs::keysFromMotion(doc.value(), loaded->motion, o);
    if (!res) return out.fail(res.error());
    const rigs::MotionKeysResult& r = res.value();
    const int last = r.first + r.count - 1;
    const bool bake = a.flags.count("bake") > 0;
    std::optional<Document> baked;
    if (bake) {
        auto b = rigs::bakeKeys(r.document, r.first, last);
        if (!b) return out.fail(b.error());
        baked = std::move(b).value();
    }
    if (int c = writeProject(a, out, baked ? *baked : r.document, *outPath); c != kOk) return c;

    json matched = json::object();
    std::string text = "Keyed frames " + std::to_string(r.first) + "-" + std::to_string(last) + " from " + loaded->name +
                       (r.framesAdded ? " (" + std::to_string(r.framesAdded) + " frames added)" : std::string()) + (bake ? ", baked," : "") +
                       " into " + *outPath + "\n";
    for (const auto& [bone, what] : r.matched) {
        matched[bone] = what;
        text += "  " + bone + ": " + what + "\n";
    }
    for (const auto& n : r.notes) text += "note: " + n + "\n";
    return out.success({{"path", *outPath},
                        {"first", r.first},
                        {"last", last},
                        {"keys", r.count},
                        {"framesAdded", r.framesAdded},
                        {"baked", bake},
                        {"camera", {{"turn", r.camera.turn}, {"tilt", r.camera.tilt}, {"roll", r.camera.roll}, {"perspective", r.camera.perspective}}},
                        {"matched", matched},
                        {"notes", r.notes}},
                       text);
}

// acestudio-cli directions <views folder> <motion|preset:NAME> --out <project>:
// the motion in every direction from a direction set (DirectionSet.h).
int cmdDirections(const Args& a, Output& out) {
    if (a.positional.size() != 3)
        return out.fail(kUsage, "directions: expected a folder of drawn views (s.png, se.png, e.png, ... or front.png, right.png, ...) and a motion");
    const auto outPath = a.opt("out");
    if (!outPath) return out.fail(kUsage, "directions: --out <project.acestudio> is required");
    if (a.opt("view") || a.opt("turn"))
        return out.fail(kUsage, "directions: the camera turns to each direction by itself (--view and --turn are for retarget; --tilt, --roll and --perspective work)");

    // The drawn views: PNG files named after their view.
    const fs::path folder = a.positional[1];
    std::error_code ec;
    if (!fs::is_directory(folder, ec)) return out.fail(kIo, "directions: " + folder.string() + " is not a folder");
    std::vector<directions::DrawnView> views;
    std::vector<std::string> skipped;
    int seen = 0;
    for (fs::directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec)) {
        if (++seen > 4096) return out.fail(kUsage, "directions: the views folder holds too many files");
        std::string ext = it->path().extension().string();
        for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        std::error_code fec;
        if (ext != ".png" || !it->is_regular_file(fec)) continue;
        const std::string stem = it->path().stem().string();
        const auto turn = directions::viewTurn(stem);
        if (!turn) {
            skipped.push_back(it->path().filename().string());
            continue;
        }
        if (static_cast<int>(views.size()) >= directions::kMaxViews) return out.fail(kUsage, "directions: too many views");
        auto art = readPng(it->path());
        if (!art) return out.fail(art.error());
        views.push_back({stem, *turn, std::move(art).value()});
    }
    if (ec) return out.fail(kIo, "directions: cannot read " + folder.string() + ": " + ec.message());
    if (views.empty())
        return out.fail(kUsage, "directions: no view found in " + folder.string() +
                                    " (name the PNGs s, se, e, ne, n, nw, w, sw or front, right, back-left, ...)");
    std::sort(views.begin(), views.end(), [](const auto& x, const auto& y) { return x.turn < y.turn; });
    if (auto st = directions::checkViews(views); !st) return out.fail(kUsage, "directions: " + st.error().message);

    int code = kOk;
    auto loaded = loadMotionArg(a, out, "directions", a.positional[2], code);
    if (!loaded) return code;
    MotionArgs ma;
    if (int c = readMotionArgs(a, out, "directions", loaded->oneShot, [] { return 0.0; }, ma); c != kOk) return c;

    directions::DirectionOptions o;
    o.allowMirror = a.flags.count("no-mirror") == 0;
    o.retarget.camera = ma.camera;   // tilt, roll, perspective; the turn is set per direction
    o.retarget.start = ma.start;
    o.retarget.length = ma.length;
    o.retarget.amount = ma.amount;
    o.retarget.fps = ma.fps;
    o.retarget.frames = ma.frames;
    o.retarget.inPlace = ma.inPlace;
    o.retarget.loop = ma.loop;
    o.retarget.mapOverrides = ma.mapOverrides;
    if (int c = readBody(a, out, "directions", views.front().art, o.retarget.body); c != kOk) return c;
    // --dirs 8 (default) | 4 | s,e,n,...
    const std::string dirs = a.opt("dirs").value_or("8");
    if (dirs == "8" || dirs == "4") {
        o.turns = directions::compassTurns(dirs == "8" ? 8 : 4);
    } else {
        for (const std::string& n : splitOn(dirs, ',')) {
            const auto t = directions::viewTurn(n);
            if (!t) return out.fail(kUsage, "directions: --dirs expects 8, 4 or direction names (s, se, e, ne, n, nw, w, sw): '" + n + "'");
            o.turns.push_back(*t);
        }
    }

    auto made = directions::retargetDirections(views, loaded->motion, o);
    if (!made) return out.fail(made.error());
    const std::string prefix = a.opt("name").value_or(loaded->name);
    std::vector<directions::NamedClip> clips;
    for (const auto& d : made.value()) clips.push_back({prefix + "_" + d.direction, &d.result.clip});

    // --turn-clip FROM,TO: a turn on the spot between two directions.
    std::optional<motion::Clip> turn;
    if (auto tc = a.opt("turn-clip")) {
        const auto ends = splitOn(*tc, ',');
        const auto from = ends.size() == 2 ? directions::viewTurn(ends[0]) : std::nullopt;
        const auto to = ends.size() == 2 ? directions::viewTurn(ends[1]) : std::nullopt;
        if (!from || !to) return out.fail(kUsage, "directions: --turn-clip expects two direction names, e.g. s,n");
        std::string err;
        const double step = optDouble(a, "turn-step", err).value_or(45);
        if (!err.empty()) return out.fail(kUsage, "directions: " + err);
        auto t = directions::turnClip(views, *from, *to, step, optInt(a, "turn-ms").value_or(100), o.allowMirror);
        if (!t) return out.fail(kUsage, "directions: " + t.error().message);
        turn = std::move(t).value();
        clips.push_back({"turn_" + ends[0] + "_" + ends[1], &*turn});
    }

    auto doc = directions::clipsDocument(clips);
    if (!doc) return out.fail(doc.error());
    if (int c = writeProject(a, out, doc.value(), *outPath); c != kOk) return c;

    json list = json::array();
    std::string text = "Wrote " + *outPath + " (" + std::to_string(doc.value().frameCount()) + " frames)\n";
    std::size_t at = 0;
    for (const auto& d : made.value()) {
        const AnimationTag& tag = doc.value().tags()[at++];
        list.push_back({{"direction", d.direction},
                        {"tag", tag.name},
                        {"turn", d.turn},
                        {"view", views[static_cast<std::size_t>(d.choice.view)].name},
                        {"mirrored", d.choice.mirrored},
                        {"offBy", d.choice.offBy},
                        {"approximateView", d.approximateView},
                        {"firstFrame", tag.fromFrame},
                        {"frames", d.result.clip.frames.size()},
                        {"notes", d.result.notes}});
        text += "  " + tag.name + ": view " + d.viewName + (d.approximateView ? " (approximate: draw this view)" : "") + "\n";
    }
    std::vector<std::string> viewNames;
    for (const auto& v : views) viewNames.push_back(v.name);
    json result = {{"path", *outPath},       {"frames", doc.value().frameCount()}, {"width", doc.value().width()},
                   {"height", doc.value().height()}, {"directions", list},          {"views", viewNames},
                   {"skipped", skipped}};
    if (turn) {
        const AnimationTag& tag = doc.value().tags().back();
        result["turnClip"] = {{"tag", tag.name}, {"firstFrame", tag.fromFrame}, {"frames", turn->frames.size()}};
        text += "  " + tag.name + ": " + std::to_string(turn->frames.size()) + " drawn views\n";
    }
    for (const auto& s : skipped) text += "skipped " + s + " (not a view name)\n";
    return out.success(result, text);
}

// acestudio-cli motion: the motion library (built-in presets and saved ones).
int cmdMotion(const Args& a, Output& out) {
    const std::string sub = a.positional.size() > 1 ? a.positional[1] : std::string();
    auto dir = libraryDir(a);
    if (!dir) return out.fail(dir.error());
    if (sub == "list") {
        if (a.positional.size() != 2) return out.fail(kUsage, "motion list: no arguments (--library <folder> lists another library)");
        auto saved = mocap::savedPresets(dir.value());
        if (!saved) return out.fail(saved.error());
        json items = json::array();
        std::string text = "Built in:\n";
        for (const auto& p : mocap::builtInPresets()) {
            items.push_back({{"name", p.name}, {"builtIn", true}, {"seconds", p.seconds}, {"loops", p.loops}, {"description", p.description}});
            text += "  " + p.name + " - " + p.description + "\n";
        }
        text += "Saved in " + dir.value().string() + ":\n";
        if (saved.value().empty()) text += "  (none)\n";
        for (const auto& p : saved.value()) {
            json item = {{"name", p.name}, {"builtIn", false}, {"seconds", p.seconds}, {"file", p.file.string()}};
            if (!p.error.empty()) item["error"] = p.error;
            items.push_back(item);
            text += "  " + p.name + (p.error.empty() ? " - " + p.description : " - unreadable: " + p.error) + "\n";
        }
        return out.success({{"library", dir.value().string()}, {"presets", items}}, text);
    }
    if (sub == "export") {
        if (a.positional.size() != 3) return out.fail(kUsage, "motion export: expected a preset name");
        const auto file = a.opt("out");
        if (!file) return out.fail(kUsage, "motion export: --out <file.bvh> is required");
        auto m = mocap::loadPreset(a.positional[2], dir.value());
        if (!m) return out.fail(m.error());
        auto text = mocap::writeBvh(m.value());
        if (!text) return out.fail(text.error());
        std::error_code ec;
        if (!a.force && fs::exists(*file, ec)) return out.fail(kIo, *file + " already exists (use --force)");
        const std::string& t = text.value();
        if (auto st = writeFileAtomic(*file, std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(t.data()), t.size())); !st)
            return out.fail(st.error());
        return out.success({{"file", *file}, {"frames", m.value().frameCount}, {"seconds", m.value().duration()}},
                           "Wrote " + *file + " (" + std::to_string(m.value().frameCount) + " frames)\n");
    }
    if (sub == "save") {
        if (a.positional.size() != 3) return out.fail(kUsage, "motion save: expected a motion file (.bvh, .glb or .gltf)");
        const auto name = a.opt("name");
        if (!name) return out.fail(kUsage, "motion save: --name <preset name> is required");
        mocap::GltfOptions gopt;
        if (auto anim = a.opt("animation")) gopt.animation = *anim;
        auto m = mocap::loadMotion(a.positional[2], gopt);
        if (!m) return out.fail(m.error());
        if (auto st = mocap::savePreset(dir.value(), *name, m.value(), a.force); !st) return out.fail(st.error());
        const fs::path file = dir.value() / (*name + ".bvh");
        return out.success({{"name", *name}, {"file", file.string()}, {"frames", m.value().frameCount}, {"seconds", m.value().duration()}},
                           "Saved preset '" + *name + "' as " + file.string() + "\n");
    }
    if (sub == "remove") {
        if (a.positional.size() != 3) return out.fail(kUsage, "motion remove: expected a saved preset's name");
        if (auto st = mocap::removePreset(dir.value(), a.positional[2]); !st) return out.fail(st.error());
        return out.success({{"removed", a.positional[2]}}, "Removed preset '" + a.positional[2] + "'\n");
    }
    return out.fail(kUsage, "motion: expected list, export, save or remove");
}

} // namespace acestudio::cli
