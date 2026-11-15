// acestudio-cli retarget: 3D skeletal motion (BVH) onto a 2D sprite.

#include "CliCommon.h"

#include "acestudio/core/AnimCodec.h"
#include "acestudio/core/FileUtil.h"
#include "acestudio/core/Mocap.h"
#include "acestudio/core/PngCodec.h"

#include <cctype>
#include <filesystem>
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

} // namespace

int cmdRetarget(const Args& a, Output& out) {
    if (a.positional.size() != 3) return out.fail(kUsage, "retarget: expected a sprite PNG and a motion file (.bvh)");
    const auto outDir = a.opt("out");
    if (!outDir) return out.fail(kUsage, "retarget: --out (a folder) is required");
    const fs::path spritePath = a.positional[1], motionPath = a.positional[2];
    std::string ext = motionPath.extension().string();
    for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (ext != ".bvh") {
        return out.fail(kUnsupported, "retarget: only BVH motion is read. Convert FBX or glTF/GLB to BVH first "
                                      "(Blender: File > Import, then File > Export > Motion Capture (.bvh)).");
    }
    auto sprite = readPng(spritePath);
    if (!sprite) return out.fail(sprite.error());
    auto motion = mocap::loadBvh(motionPath);
    if (!motion) return out.fail(motion.error());

    mocap::RetargetOptions o;
    std::string err;
    if (auto v = a.opt("view")) {
        if (*v != "auto") {
            o.camera = mocap::cameraPreset(*v);
            if (!o.camera) {
                std::string names;
                for (const auto& n : mocap::cameraPresetNames()) names += (names.empty() ? "" : ", ") + n;
                return out.fail(kUsage, "retarget: --view expects auto or one of " + names);
            }
        }
    }
    const auto turn = optDouble(a, "turn", err), tilt = optDouble(a, "tilt", err), roll = optDouble(a, "roll", err);
    const auto start = optDouble(a, "start", err), length = optDouble(a, "length", err), amount = optDouble(a, "amount", err);
    const auto fov = optDouble(a, "fov", err);
    if (!err.empty()) return out.fail(kUsage, "retarget: " + err);
    if (turn || tilt || roll || fov || a.flags.count("perspective")) {
        if (!o.camera) o.camera = mocap::Camera{};
        if (!a.opt("view") || *a.opt("view") == "auto") {
            // Turn defaults to the sprite's own view when only tilt/roll are given.
            if (!turn) {
                const motion::Subject s = motion::analyzeSubject(sprite.value());
                o.camera->turn = s.frontView ? 0 : (s.facing < 0 ? -90 : 90);
            }
        }
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
    o.loop = a.flags.count("no-loop") == 0;
    // --map Bone=joint,Bone=joint
    if (auto mp = a.opt("map")) {
        std::size_t s = 0;
        while (s <= mp->size()) {
            const std::size_t comma = mp->find(',', s);
            const std::string item = mp->substr(s, comma == std::string::npos ? std::string::npos : comma - s);
            const std::size_t eq = item.find('=');
            const auto bone = eq == std::string::npos ? std::nullopt : mocap::boneFromString(item.substr(0, eq));
            if (!bone) return out.fail(kUsage, "retarget: --map expects Bone=joint pairs (Bone: Hips, LThigh, RShin, LUpperArm, ...): '" + item + "'");
            o.mapOverrides.emplace_back(*bone, item.substr(eq + 1));
            if (comma == std::string::npos) break;
            s = comma + 1;
        }
    }

    auto res = mocap::retarget(sprite.value(), motion.value(), o);
    if (!res) return out.fail(res.error());
    const mocap::RetargetResult& r = res.value();

    const fs::path dir = *outDir;
    std::error_code ec;
    fs::create_directories(dir, ec);
    const std::string stem = spritePath.stem().string() + "_" + a.opt("name").value_or(motionPath.stem().string());
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
        mapped[std::string(mocap::toString(static_cast<mocap::Bone>(b)))] = j >= 0 ? json(motion.value().joints[static_cast<std::size_t>(j)].name) : json(nullptr);
    }
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
                        {"notes", r.notes},
                        {"issues", chk.issues}},
                       text);
}

} // namespace acestudio::cli
