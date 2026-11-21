// 3D skeletal motion onto a 2D sprite (Mocap.h, ADR-ANIM-002).

#include "acestudio/core/Mocap.h"

#include "acestudio/core/FileUtil.h"

#include "PartRig.h"
#include "RigModules.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <set>
#include <cstdio>

namespace acestudio::mocap {

namespace rg = motion::rig;

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kDeg = kPi / 180.0;

int iround(double v) { return static_cast<int>(std::lround(v)); }
std::size_t uz(int v) { return static_cast<std::size_t>(v); }

Vec3 operator+(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
Vec3 operator-(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
Vec3 operator*(Vec3 a, double k) { return {a.x * k, a.y * k, a.z * k}; }
double dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
Vec3 cross(Vec3 a, Vec3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
double len(Vec3 a) { return std::sqrt(dot(a, a)); }
Vec3 norm(Vec3 a) {
    const double l = len(a);
    return l > 1e-12 ? a * (1.0 / l) : Vec3{};
}
Vec3 horizontal(Vec3 a) { return {a.x, 0, a.z}; }

// Row-major 3x3.
struct Mat3 {
    double m[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
};
Mat3 mul(const Mat3& a, const Mat3& b) {
    Mat3 r;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) {
            double s = 0;
            for (int k = 0; k < 3; ++k) s += a.m[uz(i * 3 + k)] * b.m[uz(k * 3 + j)];
            r.m[uz(i * 3 + j)] = s;
        }
    return r;
}
Vec3 mul(const Mat3& a, Vec3 v) {
    return {a.m[0] * v.x + a.m[1] * v.y + a.m[2] * v.z, a.m[3] * v.x + a.m[4] * v.y + a.m[5] * v.z,
            a.m[6] * v.x + a.m[7] * v.y + a.m[8] * v.z};
}
Mat3 rotation(Channel c, double degrees) {
    const double s = std::sin(degrees * kDeg), co = std::cos(degrees * kDeg);
    Mat3 r;
    switch (c) {
    case Channel::Xrotation: r = Mat3{{1, 0, 0, 0, co, -s, 0, s, co}}; break;
    case Channel::Yrotation: r = Mat3{{co, 0, s, 0, 1, 0, -s, 0, co}}; break;
    case Channel::Zrotation: r = Mat3{{co, -s, 0, s, co, 0, 0, 0, 1}}; break;
    default: break;
    }
    return r;
}

// ---- BVH tokens -------------------------------------------------------------------

struct Tok {
    std::string_view s;
    int line = 0;
};

std::vector<Tok> tokenize(std::string_view text) {
    std::vector<Tok> out;
    int line = 1;
    std::size_t i = 0;
    while (i < text.size()) {
        const char c = text[i];
        if (c == '\n') { ++line; ++i; continue; }
        if (std::isspace(static_cast<unsigned char>(c))) { ++i; continue; }
        const std::size_t start = i;
        if (c == '{' || c == '}') {
            ++i;
        } else {
            while (i < text.size() && !std::isspace(static_cast<unsigned char>(text[i])) && text[i] != '{' && text[i] != '}') ++i;
        }
        out.push_back({text.substr(start, i - start), line});
    }
    return out;
}

// Locale-free decimal number ("-1.5e-3"); nullopt for anything else.
std::optional<double> number(std::string_view s) {
    std::size_t i = 0;
    double sign = 1;
    if (i < s.size() && (s[i] == '-' || s[i] == '+')) { sign = s[i] == '-' ? -1 : 1; ++i; }
    double v = 0;
    int digits = 0;
    while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) { v = v * 10 + (s[i] - '0'); ++i; ++digits; }
    if (i < s.size() && s[i] == '.') {
        ++i;
        double scale = 0.1;
        while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) { v += (s[i] - '0') * scale; scale *= 0.1; ++i; ++digits; }
    }
    if (digits == 0) return std::nullopt;
    if (i < s.size() && (s[i] == 'e' || s[i] == 'E')) {
        ++i;
        int esign = 1, e = 0, ed = 0;
        if (i < s.size() && (s[i] == '-' || s[i] == '+')) { esign = s[i] == '-' ? -1 : 1; ++i; }
        while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) { if (e < 400) e = e * 10 + (s[i] - '0'); ++i; ++ed; }
        if (ed == 0) return std::nullopt;
        v *= std::pow(10.0, esign * e);
    }
    if (i != s.size()) return std::nullopt;
    v *= sign;
    if (!std::isfinite(v)) return std::nullopt;
    return v;
}

Error corrupt(int line, const std::string& what) {
    return Error(ErrorCode::CorruptData, "BVH line " + std::to_string(line) + ": " + what);
}

std::optional<Channel> channelFrom(std::string_view s) {
    if (s == "Xposition") return Channel::Xposition;
    if (s == "Yposition") return Channel::Yposition;
    if (s == "Zposition") return Channel::Zposition;
    if (s == "Xrotation") return Channel::Xrotation;
    if (s == "Yrotation") return Channel::Yrotation;
    if (s == "Zrotation") return Channel::Zrotation;
    return std::nullopt;
}

} // namespace

int Motion::find(std::string_view name) const {
    for (std::size_t i = 0; i < joints.size(); ++i)
        if (joints[i].name == name) return static_cast<int>(i);
    return -1;
}

Result<Motion> parseBvh(std::string_view text, const BvhLimits& limits) {
    if (text.size() > limits.maxBytes) return Error(ErrorCode::CorruptData, "BVH file is larger than the limit");
    const std::vector<Tok> t = tokenize(text);
    std::size_t p = 0;
    auto lineAt = [&]() { return p < t.size() ? t[p].line : (t.empty() ? 1 : t.back().line); };
    auto expect = [&](std::string_view w) -> bool {
        if (p < t.size() && t[p].s == w) { ++p; return true; }
        return false;
    };
    Motion m;
    if (!expect("HIERARCHY")) return corrupt(lineAt(), "expected HIERARCHY");
    if (p >= t.size() || t[p].s != "ROOT") return corrupt(lineAt(), "expected ROOT");

    // Iterative descent: a stack of open joints.
    std::vector<int> stack;
    std::set<std::string> names;
    bool rootDone = false;
    while (p < t.size() && !rootDone) {
        const Tok& k = t[p];
        if (k.s == "ROOT" || k.s == "JOINT") {
            if ((k.s == "ROOT") != stack.empty()) return corrupt(k.line, k.s == "ROOT" ? "a second ROOT" : "JOINT outside ROOT");
            ++p;
            // The name is the rest of the line ("Bip01 L Thigh").
            std::string name;
            while (p < t.size() && t[p].line == k.line && t[p].s != "{") {
                if (!name.empty()) name += ' ';
                name += std::string(t[p].s);
                ++p;
            }
            if (name.empty()) return corrupt(k.line, "joint without a name");
            if (!names.insert(name).second) return corrupt(k.line, "joint name '" + name + "' used twice");
            if (!expect("{")) return corrupt(lineAt(), "expected '{' after " + name);
            if (static_cast<int>(m.joints.size()) >= limits.maxJoints) return corrupt(k.line, "too many joints");
            if (static_cast<int>(stack.size()) >= limits.maxDepth) return corrupt(k.line, "joints nested too deep");
            Joint j;
            j.name = name;
            j.parent = stack.empty() ? -1 : stack.back();
            if (!expect("OFFSET")) return corrupt(lineAt(), "expected OFFSET in " + name);
            double v[3];
            for (double& d : v) {
                auto n = p < t.size() ? number(t[p].s) : std::nullopt;
                if (!n) return corrupt(lineAt(), "bad OFFSET number in " + name);
                d = *n;
                ++p;
            }
            j.offset = {v[0], v[1], v[2]};
            if (!expect("CHANNELS")) return corrupt(lineAt(), "expected CHANNELS in " + name);
            auto cn = p < t.size() ? number(t[p].s) : std::nullopt;
            if (!cn || *cn < 0 || *cn > 6 || *cn != std::floor(*cn)) return corrupt(lineAt(), "channel count must be 0..6");
            ++p;
            for (int c = 0; c < static_cast<int>(*cn); ++c) {
                auto ch = p < t.size() ? channelFrom(t[p].s) : std::nullopt;
                if (!ch) {
                    return corrupt(lineAt(), "unsupported channel '" + (p < t.size() ? std::string(t[p].s) : std::string()) +
                                                 "' (only X/Y/Z position and rotation are read)");
                }
                if (std::find(j.channels.begin(), j.channels.end(), *ch) != j.channels.end()) return corrupt(lineAt(), "a channel listed twice");
                j.channels.push_back(*ch);
                ++p;
            }
            j.firstChannel = m.channelCount;
            m.channelCount += static_cast<int>(j.channels.size());
            m.joints.push_back(std::move(j));
            stack.push_back(static_cast<int>(m.joints.size()) - 1);
        } else if (k.s == "End") {
            if (stack.empty()) return corrupt(k.line, "End Site outside a joint");
            ++p;
            if (!expect("Site")) return corrupt(lineAt(), "expected 'End Site'");
            if (!expect("{") || !expect("OFFSET")) return corrupt(lineAt(), "expected '{ OFFSET' in End Site");
            double v[3];
            for (double& d : v) {
                auto n = p < t.size() ? number(t[p].s) : std::nullopt;
                if (!n) return corrupt(lineAt(), "bad End Site OFFSET number");
                d = *n;
                ++p;
            }
            if (!expect("}")) return corrupt(lineAt(), "expected '}' after End Site");
            if (static_cast<int>(m.joints.size()) >= limits.maxJoints) return corrupt(k.line, "too many joints");
            Joint j;
            j.parent = stack.back();
            j.name = m.joints[uz(stack.back())].name + "_end";
            j.offset = {v[0], v[1], v[2]};
            j.endSite = true;
            j.firstChannel = m.channelCount;
            m.joints.push_back(std::move(j));
        } else if (k.s == "}") {
            if (stack.empty()) return corrupt(k.line, "unbalanced '}'");
            ++p;
            stack.pop_back();
            if (stack.empty()) rootDone = true;
        } else {
            return corrupt(k.line, "unexpected '" + std::string(k.s) + "'");
        }
    }
    if (!rootDone) return corrupt(lineAt(), "the hierarchy is not closed");
    if (m.channelCount == 0) return corrupt(lineAt(), "no channels: there is no motion to read");

    if (!expect("MOTION")) return corrupt(lineAt(), "expected MOTION");
    if (!expect("Frames:")) return corrupt(lineAt(), "expected 'Frames:'");
    auto fc = p < t.size() ? number(t[p].s) : std::nullopt;
    if (!fc || *fc < 1 || *fc != std::floor(*fc) || *fc > limits.maxFrames) return corrupt(lineAt(), "frame count must be 1.." + std::to_string(limits.maxFrames));
    ++p;
    m.frameCount = static_cast<int>(*fc);
    if (!expect("Frame") || !expect("Time:")) return corrupt(lineAt(), "expected 'Frame Time:'");
    auto ft = p < t.size() ? number(t[p].s) : std::nullopt;
    if (!ft || *ft <= 0 || *ft > 10) return corrupt(lineAt(), "frame time must be above 0 and at most 10 seconds");
    ++p;
    m.frameTime = *ft;
    const std::size_t need = uz(m.frameCount) * uz(m.channelCount);
    if (need > 32u * 1024u * 1024u) return corrupt(lineAt(), "too much motion data");
    if (t.size() - p != need) {
        return corrupt(lineAt(), "expected " + std::to_string(need) + " motion values (" + std::to_string(m.frameCount) + " frames x " +
                                     std::to_string(m.channelCount) + " channels), found " + std::to_string(t.size() - p));
    }
    m.values.resize(need);
    for (std::size_t i = 0; i < need; ++i, ++p) {
        auto n = number(t[p].s);
        if (!n || std::abs(*n) > 1e7) return corrupt(t[p].line, "bad motion value '" + std::string(t[p].s) + "'");
        m.values[i] = *n;
    }
    return m;
}

Result<Motion> loadBvh(const std::filesystem::path& path, const BvhLimits& limits) {
    auto bytes = readFileBytes(path, limits.maxBytes);
    if (!bytes) return bytes.error();
    const auto& b = bytes.value();
    return parseBvh(std::string_view(reinterpret_cast<const char*>(b.data()), b.size()), limits);
}

namespace {

std::vector<Vec3> framePositions(const Motion& m, int frame) {
    std::vector<Vec3> pos(m.joints.size());
    std::vector<Mat3> rot(m.joints.size());
    const double* v = frame >= 0 && !m.values.empty() ? &m.values[uz(frame) * uz(m.channelCount)] : nullptr;
    for (std::size_t i = 0; i < m.joints.size(); ++i) {
        const Joint& j = m.joints[i];
        Vec3 tr = j.offset;
        Mat3 r;
        if (v)
            for (std::size_t c = 0; c < j.channels.size(); ++c) {
                const double val = v[uz(j.firstChannel) + c];
                switch (j.channels[c]) {
                case Channel::Xposition: tr.x += val; break;
                case Channel::Yposition: tr.y += val; break;
                case Channel::Zposition: tr.z += val; break;
                default: r = mul(r, rotation(j.channels[c], val)); break;
                }
            }
        if (j.parent < 0) {
            pos[i] = tr;
            rot[i] = r;
        } else {
            const std::size_t pi = uz(j.parent);
            pos[i] = pos[pi] + mul(rot[pi], tr);
            rot[i] = mul(rot[pi], r);
        }
    }
    return pos;
}

} // namespace

std::vector<Vec3> restPositions(const Motion& m) { return framePositions(m, -1); }

std::vector<Vec3> jointPositions(const Motion& m, double seconds) {
    if (m.frameCount <= 0) return restPositions(m);
    const double f = std::clamp(seconds / m.frameTime, 0.0, static_cast<double>(m.frameCount - 1));
    const int i0 = static_cast<int>(std::floor(f));
    const int i1 = std::min(i0 + 1, m.frameCount - 1);
    const double a = f - i0;
    std::vector<Vec3> p0 = framePositions(m, i0);
    if (a < 1e-9 || i1 == i0) return p0;
    const std::vector<Vec3> p1 = framePositions(m, i1);
    for (std::size_t i = 0; i < p0.size(); ++i) p0[i] = p0[i] * (1 - a) + p1[i] * a;
    return p0;
}

// ---- Bone mapping ------------------------------------------------------------------

namespace {

constexpr const char* kBoneNames[kBoneCount] = {
    "Hips", "Spine", "Chest", "Neck", "Head",
    "LShoulder", "LUpperArm", "LForearm", "LHand",
    "RShoulder", "RUpperArm", "RForearm", "RHand",
    "LThigh", "LShin", "LFoot", "LToe",
    "RThigh", "RShin", "RFoot", "RToe",
};

// "mixamorig:LeftUpLeg" -> {left, up, leg}; "thigh.L" -> {thigh, l}.
std::vector<std::string> words(const std::string& raw) {
    std::string s = raw;
    if (auto c = s.rfind(':'); c != std::string::npos) s = s.substr(c + 1);
    std::vector<std::string> out;
    std::string cur;
    auto flush = [&]() {
        if (!cur.empty()) out.push_back(cur);
        cur.clear();
    };
    for (std::size_t i = 0; i < s.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (!std::isalnum(c)) { flush(); continue; }
        if (!cur.empty()) {
            const unsigned char prev = static_cast<unsigned char>(s[i - 1]);
            const bool upper = std::isupper(c) != 0;
            const bool next = i + 1 < s.size() && std::islower(static_cast<unsigned char>(s[i + 1])) != 0;
            if ((upper && (std::islower(prev) || (std::isupper(prev) && next))) || (std::isdigit(c) != 0) != (std::isdigit(prev) != 0)) flush();
        }
        cur += static_cast<char>(std::tolower(c));
    }
    flush();
    return out;
}

enum class Kind { None, Hips, Neck, Head, Shoulder, UpperArm, Forearm, Hand, Thigh, Shin, Foot, Toe, OldShoulder, OldHip };

struct NameGuess {
    Kind kind = Kind::None;
    int side = 0;   // -1 left, +1 right
};

NameGuess guessName(const std::string& raw) {
    NameGuess g;
    std::vector<std::string> w = words(raw);
    std::vector<std::string> rest;
    static const std::set<std::string> skip = {"bip", "bip01", "bip001", "mixamorig", "def", "org", "mch", "jnt", "joint", "bone",
                                               "b", "bn", "character", "ctrl", "bend", "01", "001", "0", "1", "2", "3", "4", "5"};
    static const std::set<std::string> reject = {"thumb", "index", "middle", "ring", "pinky", "little", "finger", "fingers", "twist",
                                                 "roll", "ik", "pole", "target", "end", "nub", "eye", "jaw", "tongue", "carpal",
                                                 "metacarpal", "breast", "hair", "weapon", "prop"};
    for (const std::string& s : w) {
        if (reject.count(s)) return {};
        if (s == "left" || s == "l" || s == "lft") { g.side = -1; continue; }
        if (s == "right" || s == "r" || s == "rgt") { g.side = 1; continue; }
        if (skip.count(s)) continue;
        rest.push_back(s);
    }
    std::string key;
    for (const std::string& s : rest) key += s;
    // Daz/Poser style "lThigh" splits into {l, thigh}; "LHand" into {l, hand}.
    if (key.empty()) return {};
    struct Rule {
        const char* key;
        Kind kind;
        bool sided;
    };
    static const Rule rules[] = {
        {"hips", Kind::Hips, false}, {"hip", Kind::Hips, false}, {"pelvis", Kind::Hips, false},
        {"neck", Kind::Neck, false}, {"necklower", Kind::Neck, false}, {"head", Kind::Head, false},
        {"shoulder", Kind::OldShoulder, true}, {"clavicle", Kind::Shoulder, true}, {"collar", Kind::Shoulder, true},
        {"clav", Kind::Shoulder, true},
        {"arm", Kind::UpperArm, true}, {"uparm", Kind::UpperArm, true}, {"upperarm", Kind::UpperArm, true},
        {"shldr", Kind::UpperArm, true}, {"humerus", Kind::UpperArm, true},
        {"forearm", Kind::Forearm, true}, {"lowerarm", Kind::Forearm, true}, {"lowarm", Kind::Forearm, true},
        {"elbow", Kind::Forearm, true}, {"hand", Kind::Hand, true}, {"wrist", Kind::Hand, true},
        {"upleg", Kind::Thigh, true}, {"upperleg", Kind::Thigh, true}, {"thigh", Kind::Thigh, true},
        {"femur", Kind::Thigh, true}, {"hip", Kind::OldHip, true},
        {"leg", Kind::Shin, true}, {"lowerleg", Kind::Shin, true}, {"lowleg", Kind::Shin, true}, {"calf", Kind::Shin, true},
        {"shin", Kind::Shin, true}, {"knee", Kind::Shin, true}, {"tibia", Kind::Shin, true},
        {"foot", Kind::Foot, true}, {"ankle", Kind::Foot, true},
        {"toe", Kind::Toe, true}, {"toebase", Kind::Toe, true}, {"toes", Kind::Toe, true}, {"ball", Kind::Toe, true},
    };
    for (const Rule& r : rules) {
        if (key != r.key) continue;
        if (r.sided != (g.side != 0)) continue;
        g.kind = r.kind;
        return g;
    }
    return {};
}

std::vector<int> depthOf(const Motion& m) {
    std::vector<int> d(m.joints.size(), 0);
    for (std::size_t i = 0; i < m.joints.size(); ++i)
        if (m.joints[i].parent >= 0) d[i] = d[uz(m.joints[i].parent)] + 1;
    return d;
}

std::vector<int> childrenOf(const Motion& m, int j) {
    std::vector<int> c;
    for (std::size_t i = 0; i < m.joints.size(); ++i)
        if (m.joints[i].parent == j) c.push_back(static_cast<int>(i));
    return c;
}

bool isAncestor(const Motion& m, int a, int j) {
    for (int k = j; k >= 0; k = m.joints[uz(k)].parent)
        if (k == a) return true;
    return false;
}

// The first child that is a real joint (else the first end site).
int nextInChain(const Motion& m, int j, bool allowEnd) {
    int endChild = -1;
    for (int c : childrenOf(m, j)) {
        if (!m.joints[uz(c)].endSite) return c;
        if (endChild < 0) endChild = c;
    }
    return allowEnd ? endChild : -1;
}

} // namespace

std::string_view toString(Bone b) noexcept { return kBoneNames[static_cast<int>(b)]; }

std::optional<Bone> boneFromString(std::string_view s) noexcept {
    for (int i = 0; i < kBoneCount; ++i)
        if (s == kBoneNames[i]) return static_cast<Bone>(i);
    return std::nullopt;
}

bool BoneMap::hasLegs() const {
    for (Bone b : {Bone::Hips, Bone::LThigh, Bone::LShin, Bone::LFoot, Bone::RThigh, Bone::RShin, Bone::RFoot})
        if ((*this)[b] < 0) return false;
    return true;
}

BoneMap autoMap(const Motion& m, const std::vector<std::pair<Bone, std::string>>& overrides) {
    BoneMap map;
    map.joint.fill(-1);
    const std::vector<int> depth = depthOf(m);
    // Name guesses; the shallowest joint wins each bone.
    auto take = [&](Bone b, int j) {
        int& cur = map[b];
        if (cur < 0 || depth[uz(j)] < depth[uz(cur)]) cur = j;
    };
    std::vector<NameGuess> guesses(m.joints.size());
    bool oldShoulders = false, oldHips = false;
    for (std::size_t i = 0; i < m.joints.size(); ++i) {
        if (m.joints[i].endSite) continue;
        const NameGuess g = guessName(m.joints[i].name);
        guesses[i] = g;
        const int j = static_cast<int>(i);
        const bool L = g.side < 0;
        switch (g.kind) {
        case Kind::Hips: take(Bone::Hips, j); break;
        case Kind::Neck: take(Bone::Neck, j); break;
        case Kind::Head: take(Bone::Head, j); break;
        case Kind::Shoulder: take(L ? Bone::LShoulder : Bone::RShoulder, j); break;
        case Kind::UpperArm: take(L ? Bone::LUpperArm : Bone::RUpperArm, j); break;
        case Kind::Forearm: take(L ? Bone::LForearm : Bone::RForearm, j); break;
        case Kind::Hand: take(L ? Bone::LHand : Bone::RHand, j); break;
        case Kind::Thigh: take(L ? Bone::LThigh : Bone::RThigh, j); break;
        case Kind::Shin: take(L ? Bone::LShin : Bone::RShin, j); break;
        case Kind::Foot: take(L ? Bone::LFoot : Bone::RFoot, j); break;
        case Kind::Toe: take(L ? Bone::LToe : Bone::RToe, j); break;
        case Kind::OldShoulder: oldShoulders = true; break;
        case Kind::OldHip: oldHips = true; break;
        case Kind::None: break;
        }
    }
    // "LeftShoulder": a collar bone when an upper arm ("LeftArm") follows,
    // else (older files: LeftShoulder, LeftElbow, LeftWrist) the upper arm.
    // "LeftHip" likewise is the thigh when no other thigh is named.
    if (oldShoulders || oldHips)
        for (std::size_t i = 0; i < guesses.size(); ++i) {
            const int j = static_cast<int>(i);
            const bool L = guesses[i].side < 0;
            if (guesses[i].kind == Kind::OldShoulder) {
                if (map[L ? Bone::LUpperArm : Bone::RUpperArm] >= 0) take(L ? Bone::LShoulder : Bone::RShoulder, j);
                else take(L ? Bone::LUpperArm : Bone::RUpperArm, j);
            }
            if (guesses[i].kind == Kind::OldHip && map[L ? Bone::LThigh : Bone::RThigh] < 0) take(L ? Bone::LThigh : Bone::RThigh, j);
        }
    for (const auto& [bone, name] : overrides) {
        const int j = m.find(name);
        if (j >= 0) map[bone] = j;
        else map.notes.push_back("--map: no joint named '" + name + "'");
    }

    // Structure, for what names did not give.
    const std::vector<Vec3> rest = restPositions(m);
    if (map[Bone::Hips] < 0 && !m.joints.empty()) {
        map[Bone::Hips] = 0;
        map.notes.push_back("Hips: the root joint '" + m.joints[0].name + "'");
    }
    const int hips = map[Bone::Hips];
    double minY = 0, maxY = 0;
    for (const Vec3& p : rest) { minY = std::min(minY, p.y); maxY = std::max(maxY, p.y); }
    const double height = std::max(1e-6, maxY - minY);
    const Vec3 up{0, 1, 0};

    // Legs: two branches below the hips that reach down to near the floor.
    for (int side = 0; side < 2; ++side) {
        const Bone thigh = side == 0 ? Bone::LThigh : Bone::RThigh;
        if (map[thigh] >= 0 || hips < 0) continue;
        std::vector<int> cands;
        std::vector<int> frontier = childrenOf(m, hips);
        // A pelvis joint with two leg branches below it: look one deeper.
        for (int guard = 0; guard < 3 && !frontier.empty(); ++guard) {
            std::vector<int> next;
            for (int c : frontier) {
                if (m.joints[uz(c)].endSite) continue;
                int lowest = c;
                for (std::size_t i = 0; i < m.joints.size(); ++i)
                    if (isAncestor(m, c, static_cast<int>(i)) && rest[i].y < rest[uz(lowest)].y) lowest = static_cast<int>(i);
                if (rest[uz(lowest)].y < rest[uz(hips)].y - 0.3 * height) {
                    const auto kids = childrenOf(m, c);
                    int legKids = 0;
                    for (int k : kids)
                        if (!m.joints[uz(k)].endSite && rest[uz(k)].y < rest[uz(c)].y + 0.05 * height) ++legKids;
                    if (legKids >= 2) next.insert(next.end(), kids.begin(), kids.end());
                    else cands.push_back(c);
                }
            }
            if (!cands.empty()) break;
            frontier = next;
        }
        if (cands.size() < 2) continue;
        // Which is left: the larger x in the rest pose (BVH characters
        // face +Z, their left is +X), unless names already put one side.
        std::sort(cands.begin(), cands.end(), [&](int a, int b) { return rest[uz(a)].x > rest[uz(b)].x; });
        const int pick = side == 0 ? cands.front() : cands.back();
        if (pick == map[side == 0 ? Bone::RThigh : Bone::LThigh]) continue;
        map[thigh] = pick;
        map.notes.push_back(std::string(toString(thigh)) + ": by shape, '" + m.joints[uz(pick)].name + "'");
    }
    // Chains below each thigh / upper arm.
    auto chain = [&](Bone a, Bone b, Bone c, Bone d) {
        if (map[a] < 0) return;
        if (map[b] < 0) map[b] = nextInChain(m, map[a], false);
        if (map[b] >= 0 && map[c] < 0) map[c] = nextInChain(m, map[b], false);
        if (map[c] >= 0 && map[d] < 0) map[d] = nextInChain(m, map[c], true);
    };
    chain(Bone::LThigh, Bone::LShin, Bone::LFoot, Bone::LToe);
    chain(Bone::RThigh, Bone::RShin, Bone::RFoot, Bone::RToe);

    // Spine: the branch of the hips that rises highest; head at its top.
    if (hips >= 0 && map[Bone::Head] < 0) {
        int top = -1;
        for (std::size_t i = 0; i < m.joints.size(); ++i) {
            const int j = static_cast<int>(i);
            if (j == hips || !isAncestor(m, hips, j)) continue;
            bool limb = false;
            for (Bone b : {Bone::LThigh, Bone::RThigh}) limb = limb || (map[b] >= 0 && isAncestor(m, map[b], j));
            if (!limb && (top < 0 || rest[i].y > rest[uz(top)].y)) top = j;
        }
        if (top >= 0) {
            map[Bone::Head] = m.joints[uz(top)].endSite ? m.joints[uz(top)].parent : top;
            map.notes.push_back("Head: by shape, '" + m.joints[uz(map[Bone::Head])].name + "'");
        }
    }
    if (map[Bone::Head] >= 0 && map[Bone::Neck] < 0) {
        const int pn = m.joints[uz(map[Bone::Head])].parent;
        if (pn >= 0 && pn != hips) map[Bone::Neck] = pn;
    }
    const int neckOrHead = map[Bone::Neck] >= 0 ? map[Bone::Neck] : map[Bone::Head];
    if (hips >= 0 && neckOrHead >= 0 && isAncestor(m, hips, neckOrHead)) {
        // Spine: the hips' child on the way up; chest: the neck's parent.
        int k = neckOrHead;
        while (k >= 0 && m.joints[uz(k)].parent != hips) k = m.joints[uz(k)].parent;
        if (k >= 0 && k != neckOrHead && map[Bone::Spine] < 0) map[Bone::Spine] = k;
        const int pc = m.joints[uz(neckOrHead)].parent;
        if (pc >= 0 && pc != hips && pc != map[Bone::Spine] && map[Bone::Chest] < 0) map[Bone::Chest] = pc;
    }
    // Arms: branches off the spine (between the hips and the neck) that
    // reach furthest sideways.
    for (int side = 0; side < 2; ++side) {
        const Bone upper = side == 0 ? Bone::LUpperArm : Bone::RUpperArm;
        const Bone shoulder = side == 0 ? Bone::LShoulder : Bone::RShoulder;
        if (map[upper] >= 0 || neckOrHead < 0) continue;
        int best = -1;
        double bestX = 0;
        for (int k = neckOrHead; k >= 0 && k != hips; k = m.joints[uz(k)].parent)
            for (int c : childrenOf(m, k)) {
                if (m.joints[uz(c)].endSite || isAncestor(m, c, neckOrHead)) continue;
                double reach = 0;
                for (std::size_t i = 0; i < m.joints.size(); ++i)
                    if (isAncestor(m, c, static_cast<int>(i))) reach = side == 0 ? std::max(reach, rest[i].x) : std::min(reach, rest[i].x);
                if (side == 0 ? reach > bestX : reach < bestX) { bestX = reach; best = c; }
            }
        if (best < 0 || std::abs(bestX) < 0.02 * height) continue;
        // Collar, upper arm, forearm, hand when the branch is four long.
        int n = 0;
        for (int k = best; k >= 0; k = nextInChain(m, k, false)) ++n;
        if (n >= 4) {
            if (map[shoulder] < 0) map[shoulder] = best;
            map[upper] = nextInChain(m, best, false);
        } else {
            map[upper] = best;
        }
        map.notes.push_back(std::string(toString(upper)) + ": by shape, '" + m.joints[uz(map[upper])].name + "'");
    }
    for (int side = 0; side < 2; ++side) {
        const Bone shoulder = side == 0 ? Bone::LShoulder : Bone::RShoulder;
        const Bone upper = side == 0 ? Bone::LUpperArm : Bone::RUpperArm;
        // A collar named but no upper arm: the collar's child is the arm.
        if (map[upper] < 0 && map[shoulder] >= 0) map[upper] = nextInChain(m, map[shoulder], false);
    }
    chain(Bone::LUpperArm, Bone::LForearm, Bone::LHand, Bone::LHand);
    chain(Bone::RUpperArm, Bone::RForearm, Bone::RHand, Bone::RHand);
    (void)up;

    for (int b = 0; b < kBoneCount; ++b)
        if (map.joint[uz(b)] < 0 && b != static_cast<int>(Bone::LShoulder) && b != static_cast<int>(Bone::RShoulder) &&
            b != static_cast<int>(Bone::Chest))
            map.notes.push_back(std::string(kBoneNames[b]) + ": not found");
    return map;
}

// ---- Camera --------------------------------------------------------------------------

std::optional<Camera> cameraPreset(std::string_view n) {
    Camera c;
    if (n == "front") c.turn = 0;
    else if (n == "back") c.turn = 180;
    else if (n == "right") c.turn = 90;
    else if (n == "left") c.turn = -90;
    else if (n == "front-right") c.turn = 45;
    else if (n == "front-left") c.turn = -45;
    else if (n == "back-right") c.turn = 135;
    else if (n == "back-left") c.turn = -135;
    else if (n == "three-quarter") { c.turn = 45; c.tilt = 30; }
    else if (n == "top-down") { c.turn = 0; c.tilt = 90; }
    else if (n == "isometric") { c.turn = 45; c.tilt = 35.264; }
    else return std::nullopt;
    return c;
}

std::vector<std::string> cameraPresetNames() {
    return {"front", "back", "right", "left", "front-right", "front-left", "back-right", "back-left", "three-quarter", "top-down", "isometric"};
}

namespace {

Vec3 forwardAt(const Motion& m, const BoneMap& map, const std::vector<Vec3>& p) {
    const Vec3 up{0, 1, 0};
    // 1. The left thigh is on the character's left: forward = left x up.
    if (map[Bone::LThigh] >= 0 && map[Bone::RThigh] >= 0) {
        const Vec3 l = horizontal(p[uz(map[Bone::LThigh])] - p[uz(map[Bone::RThigh])]);
        if (len(l) > 1e-6) return norm(cross(l, up));
    }
    // 2. The toes point forward.
    Vec3 toes{};
    for (auto [f, t] : {std::pair{Bone::LFoot, Bone::LToe}, std::pair{Bone::RFoot, Bone::RToe}})
        if (map[f] >= 0 && map[t] >= 0) toes = toes + horizontal(p[uz(map[t])] - p[uz(map[f])]);
    if (len(toes) > 1e-6) return norm(toes);
    (void)m;
    return {0, 0, 1};
}

struct Basis {
    Vec3 d, sx, su;   // towards the camera, screen right, screen up
};

Basis basisFor(const Camera& cam, Vec3 forward) {
    const Vec3 up{0, 1, 0};
    const Vec3 f = norm(horizontal(forward));
    const Vec3 r = cross(f, up);   // the character's right
    const double turn = cam.turn * kDeg, tilt = std::clamp(cam.tilt, -90.0, 90.0) * kDeg;
    const Vec3 dh = norm(f * std::cos(turn) + r * std::sin(turn));
    Basis b;
    b.d = dh * std::cos(tilt) + up * std::sin(tilt);
    b.sx = cross(up, dh);
    b.su = cross(b.d, b.sx);
    return b;
}

} // namespace

Vec3 characterForward(const Motion& m, const BoneMap& map) { return forwardAt(m, map, jointPositions(m, 0)); }

Projected project(const std::vector<Vec3>& world, const Camera& cam, Vec3 forward, double height, Vec3 centre) {
    const Basis b = basisFor(cam, forward);
    height = std::max(height, 1e-6);
    const double roll = cam.roll * kDeg;
    const double cr = std::cos(roll), sr = std::sin(roll);
    const double D = std::max(0.5, cam.distance) * height;
    Projected out;
    out.x.reserve(world.size());
    out.y.reserve(world.size());
    out.depth.reserve(world.size());
    for (const Vec3& w : world) {
        const Vec3 q = w - centre;
        double X = dot(q, b.sx), Y = dot(q, b.su);
        const double depth = dot(q, b.d);
        if (cam.perspective) {
            const double k = D / std::max(0.1 * height, D - depth);
            X *= k;
            Y *= k;
        }
        const double Xr = X * cr + Y * sr, Yr = -X * sr + Y * cr;
        out.x.push_back(Xr / height);
        out.y.push_back(-Yr / height);
        out.depth.push_back(depth / height);
    }
    return out;
}

// ---- Retarget ----------------------------------------------------------------------------

namespace {

constexpr Rgba8 kLeftColour{70, 140, 255, 255}, kRightColour{255, 80, 70, 255}, kSpineColour{250, 210, 60, 255},
    kOtherColour{140, 140, 150, 255};

void drawLine(Image& img, double x0, double y0, double x1, double y1, Rgba8 c, int thick) {
    const double dx = x1 - x0, dy = y1 - y0;
    const int steps = std::max(1, iround(std::max(std::abs(dx), std::abs(dy))));
    for (int i = 0; i <= steps; ++i) {
        const double t = static_cast<double>(i) / steps;
        const int x = static_cast<int>(std::floor(x0 + dx * t)), y = static_cast<int>(std::floor(y0 + dy * t));
        for (int oy = 0; oy < thick; ++oy)
            for (int ox = 0; ox < thick; ++ox) img.setPixel(x + ox - thick / 2, y + oy - thick / 2, c);
    }
}

struct View {
    Vec3 forward, centre;
    double height = 1;
};

// Draws the skeleton of one frame (screen units from `project`) at `scale`
// pixels per character height, the floor point at (ox, oy).
void paintSkeleton(Image& img, const Motion& m, const BoneMap& map, const Projected& pr, double scale, double ox, double oy) {
    const int thick = img.height() >= 96 ? 2 : 1;
    auto colourOf = [&](int j) {
        for (Bone b : {Bone::LShoulder, Bone::LUpperArm, Bone::LForearm, Bone::LHand, Bone::LThigh, Bone::LShin, Bone::LFoot, Bone::LToe})
            if (map[b] >= 0 && (map[b] == j)) return kLeftColour;
        for (Bone b : {Bone::RShoulder, Bone::RUpperArm, Bone::RForearm, Bone::RHand, Bone::RThigh, Bone::RShin, Bone::RFoot, Bone::RToe})
            if (map[b] >= 0 && (map[b] == j)) return kRightColour;
        for (Bone b : {Bone::Hips, Bone::Spine, Bone::Chest, Bone::Neck, Bone::Head})
            if (map[b] == j) return kSpineColour;
        return kOtherColour;
    };
    // Far bones first, so the near side is drawn over them.
    std::vector<std::size_t> order;
    for (std::size_t i = 0; i < m.joints.size(); ++i)
        if (m.joints[i].parent >= 0) order.push_back(i);
    std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) { return pr.depth[a] < pr.depth[b]; });
    for (std::size_t i : order) {
        const std::size_t p = uz(m.joints[i].parent);
        // A bone takes the colour of the joint it starts from.
        drawLine(img, ox + pr.x[p] * scale, oy + pr.y[p] * scale, ox + pr.x[i] * scale, oy + pr.y[i] * scale, colourOf(static_cast<int>(p)), thick);
    }
}

double wrapAngle(double a) {
    while (a > kPi) a -= 2 * kPi;
    while (a < -kPi) a += 2 * kPi;
    return a;
}

} // namespace

Image drawSkeleton(const Motion& m, const BoneMap& map, const Camera& cam, double seconds, int size, bool inPlace) {
    size = std::clamp(size, 16, 2048);
    Image img(size, size);
    const std::vector<Vec3> rest = restPositions(m);
    double minY = 0, maxY = 0;
    for (const Vec3& p : rest) { minY = std::min(minY, p.y); maxY = std::max(maxY, p.y); }
    const double height = std::max(1e-6, maxY - minY);
    const std::vector<Vec3> p0 = jointPositions(m, 0);
    const int hips = map[Bone::Hips] >= 0 ? map[Bone::Hips] : 0;
    Vec3 centre = p0.empty() ? Vec3{} : Vec3{p0[uz(hips)].x, minY, p0[uz(hips)].z};
    std::vector<Vec3> w = jointPositions(m, seconds);
    if (inPlace && !w.empty()) {
        const Vec3 travel = horizontal(w[uz(hips)] - p0[uz(hips)]);
        for (Vec3& q : w) q = q - travel;
    }
    const Projected pr = project(w, cam, forwardAt(m, map, p0), height, centre);
    paintSkeleton(img, m, map, pr, size * 0.75, size / 2.0, size * 0.9);
    return img;
}

// ---- Four-legged skeletons -------------------------------------------------------

CreatureMap creatureMap(const Motion& m) {
    CreatureMap cm;
    const std::size_t n = m.joints.size();
    if (n == 0) {
        cm.notes.push_back("the motion has no joints");
        return cm;
    }
    const std::vector<Vec3> rest = restPositions(m);
    std::vector<std::vector<int>> kids(n);
    for (std::size_t i = 0; i < n; ++i) {
        const int p = m.joints[i].parent;
        if (p >= 0 && uz(p) < n) kids[uz(p)].push_back(static_cast<int>(i));
        else if (p < 0 && cm.root < 0) cm.root = static_cast<int>(i);
    }
    if (cm.root < 0) {
        cm.notes.push_back("the motion has no root joint");
        return cm;
    }
    double minY = 1e300, maxY = -1e300;
    for (const Vec3& p : rest) {
        minY = std::min(minY, p.y);
        maxY = std::max(maxY, p.y);
    }
    const double height = std::max(1e-6, maxY - minY);
    // The highest rest point in each joint's subtree (passes until nothing
    // changes, so any parent-before-child order works).
    std::vector<double> top(n);
    for (std::size_t i = 0; i < n; ++i) top[i] = rest[i].y;
    for (std::size_t pass = 0; pass < n; ++pass) {
        bool changed = false;
        for (std::size_t i = 0; i < n; ++i) {
            const int p = m.joints[i].parent;
            if (p >= 0 && uz(p) < n && top[i] > top[uz(p)]) {
                top[uz(p)] = top[i];
                changed = true;
            }
        }
        if (!changed) break;
    }
    const double floorBand = minY + 0.08 * height, footBand = minY + 0.25 * height;
    std::vector<CreatureLeg> legs;
    for (std::size_t i = 0; i < n; ++i) {
        if (!kids[i].empty() || rest[i].y > floorBand) continue;
        // Climb while the joint above belongs to the same leg: its other
        // children (a heel beside a toe) stay near the floor. Never into the root.
        std::vector<int> chain{static_cast<int>(i)};
        int j = static_cast<int>(i);
        while (chain.size() < 32) {
            const int p = m.joints[uz(j)].parent;
            if (p < 0 || m.joints[uz(p)].parent < 0) break;
            bool sameLeg = true;
            for (int c : kids[uz(p)])
                if (c != j && top[uz(c)] > footBand) sameLeg = false;
            if (!sameLeg) break;
            chain.insert(chain.begin(), p);
            j = p;
        }
        if (chain.size() < 3) continue;   // too short to be a jointed leg
        CreatureLeg leg;
        leg.joints = chain;
        for (std::size_t k = 1; k < chain.size(); ++k) leg.length += len(rest[uz(chain[k])] - rest[uz(chain[k - 1])]);
        bool merged = false;
        for (CreatureLeg& other : legs)
            if (other.joints.front() == leg.joints.front()) {
                if (leg.joints.size() > other.joints.size()) other = leg;
                merged = true;
            }
        if (!merged && leg.length > 0.1 * height) legs.push_back(std::move(leg));
    }
    if (legs.size() != 4 && legs.size() != 6 && legs.size() != 8) {
        cm.notes.push_back("found " + std::to_string(legs.size()) +
                           " leg(s) reaching the floor; a four-legged motion needs four (six or eight are read too)");
        return cm;
    }
    // Front and back lie along the longest horizontal spread of the leg tops,
    // measured in the first frame (the rest pose leaves out the root's turn).
    const std::vector<Vec3> p0 = jointPositions(m, 0), p1 = jointPositions(m, m.duration());
    Vec3 c{};
    for (const CreatureLeg& l : legs) c = c + horizontal(p0[uz(l.joints.front())]) * (1.0 / static_cast<double>(legs.size()));
    double sxx = 0, sxz = 0, szz = 0;
    for (const CreatureLeg& l : legs) {
        const Vec3 d = horizontal(p0[uz(l.joints.front())]) - c;
        sxx += d.x * d.x;
        sxz += d.x * d.z;
        szz += d.z * d.z;
    }
    const double theta = 0.5 * std::atan2(2 * sxz, sxx - szz);
    const Vec3 axis{std::cos(theta), 0, std::sin(theta)};
    std::sort(legs.begin(), legs.end(), [&](const CreatureLeg& a, const CreatureLeg& b) {
        return dot(p0[uz(a.joints.front())] - c, axis) < dot(p0[uz(b.joints.front())] - c, axis);
    });
    // Which end is the front: where the root travels, else the head's end.
    double sign = 0;
    const Vec3 travel = horizontal(p1[uz(cm.root)] - p0[uz(cm.root)]);
    if (len(travel) > 0.1 * height && std::abs(dot(norm(travel), axis)) > 0.3) {
        sign = dot(travel, axis) > 0 ? 1 : -1;
    } else {
        int head = -1;
        for (std::size_t i = 0; i < n; ++i) {
            if (!kids[i].empty() || static_cast<int>(i) == cm.root) continue;
            bool inLeg = false;
            for (const CreatureLeg& l : legs)
                inLeg = inLeg || std::find(l.joints.begin(), l.joints.end(), static_cast<int>(i)) != l.joints.end();
            if (!inLeg && (head < 0 || p0[i].y > p0[uz(head)].y)) head = static_cast<int>(i);
        }
        if (head >= 0 && std::abs(dot(p0[uz(head)] - c, axis)) > 1e-6) {
            sign = dot(p0[uz(head)] - c, axis) > 0 ? 1 : -1;
        } else {
            sign = axis.z >= 0 ? 1 : -1;
            cm.notes.push_back("the front could not be told from the back; +Z is taken as forward");
        }
    }
    if (sign < 0) std::reverse(legs.begin(), legs.end());
    // The two furthest back are the hind pair, the two furthest forward the
    // front pair; any between them are middle pairs.
    std::vector<CreatureLeg> middle(legs.begin() + 2, legs.end() - 2);
    legs.erase(legs.begin() + 2, legs.end() - 2);
    if (!middle.empty())
        cm.notes.push_back("the skeleton has " + std::to_string(4 + middle.size()) + " legs: " + std::to_string(middle.size() / 2) +
                           " middle pair(s) between the hind and front legs");
    for (std::size_t i = 0; i < 4; ++i) legs[i].hind = i < 2;
    Vec3 hindC{}, frontC{};
    for (std::size_t i = 0; i < 4; ++i) (i < 2 ? hindC : frontC) = (i < 2 ? hindC : frontC) + horizontal(p0[uz(legs[i].joints.front())]) * 0.5;
    cm.forward = norm(frontC - hindC);
    if (len(cm.forward) < 1e-9) cm.forward = axis * sign;
    // Head and tail: the ends (not in a leg, above the floor band) furthest
    // forward and back.
    double headAt = 0, tailAt = dot(hindC - c, cm.forward);
    for (std::size_t i = 0; i < n; ++i) {
        if (!kids[i].empty() || static_cast<int>(i) == cm.root || rest[i].y <= floorBand) continue;
        bool inLeg = false;
        for (const CreatureLeg& l : legs)
            inLeg = inLeg || std::find(l.joints.begin(), l.joints.end(), static_cast<int>(i)) != l.joints.end();
        for (const CreatureLeg& l : middle)
            inLeg = inLeg || std::find(l.joints.begin(), l.joints.end(), static_cast<int>(i)) != l.joints.end();
        if (inLeg) continue;
        const double along = dot(horizontal(p0[i]) - c, cm.forward);
        if (along > headAt) {
            headAt = along;
            cm.head = static_cast<int>(i);
        }
        if (along < tailAt) {
            tailAt = along;
            cm.tail = static_cast<int>(i);
        }
    }
    cm.legs = std::move(legs);
    cm.middle = std::move(middle);
    return cm;
}

namespace {

// The rows each column of a four-legged body moves (rg::PartPose::colDy):
// straight between the hind and front leg tops (`endDy` of the hind and front
// ends, the back's pitch), carried on past them, plus the head's and tail's
// own moves fading in over the stretch beyond the legs.
std::vector<int> backColumns(const rg::Part& body, const motion::detail::GaitRig& g, const double endDy[4]) {
    if (body.hiBox.empty()) return {};
    const int x0 = body.hiBox.x + 1, w = body.hiBox.w - 2;
    double hindX = 0, frontX = 0;
    int nh = 0, nf = 0;
    for (const auto& L : g.legs) {
        (L.hind ? hindX : frontX) += L.hip.x;
        ++(L.hind ? nh : nf);
    }
    if (nh == 0 || nf == 0) return {};
    hindX /= nh;
    frontX /= nf;
    const double gap = frontX - hindX;
    if (std::abs(gap) < 2) return {};
    // How far the body reaches past each pair (in the facing direction).
    const double headReach = std::max(1.0, g.dir > 0 ? x0 + w - frontX : frontX - x0);
    const double tailReach = std::max(1.0, g.dir > 0 ? hindX - x0 : x0 + w - hindX);
    auto smooth = [](double k) {
        k = std::clamp(k, 0.0, 1.0);
        return k * k * (3 - 2 * k);
    };
    std::vector<int> out(static_cast<std::size_t>(w));
    bool any = false;
    for (int k = 0; k < w; ++k) {
        const double cx = x0 + k + 0.5;
        const double t = std::clamp((cx - hindX) / gap, -0.5, 1.5);
        double d = endDy[0] + (endDy[1] - endDy[0]) * t;
        const double ahead = (cx - frontX) * g.dir, behind = (hindX - cx) * g.dir;
        if (ahead > 0) d += endDy[2] * smooth(ahead / headReach);
        if (behind > 0) d += endDy[3] * smooth(behind / tailReach);
        out[static_cast<std::size_t>(k)] = iround(d);
        any = any || out[static_cast<std::size_t>(k)] != 0;
    }
    if (!any) out.clear();
    return out;
}

// A four-legged motion on a four-legged sprite's gait rig: each drawn leg
// follows the source leg on its end (hind or front) and side (near or far);
// the body rises and sinks with the root.
// A humanoid motion on a winged sprite: the drawn wing is the top of the
// stroke (the arm's highest point in the span); it turns down as the arm
// falls from there, by the same angle (times amount), the far wing a little
// less. The hand end bends with the elbow, and the body rises and sinks
// with the hips. Arms are matched near to near under the camera.
Result<RetargetResult> retargetFlyer(const Image& sprite, const Motion& m, const RetargetOptions& opt) {
    RetargetResult res;
    res.map = autoMap(m, opt.mapOverrides);
    const BoneMap& map = res.map;
    const std::vector<Vec3> rest = restPositions(m);
    // The skeleton's own wings: the two longest chains of joints named
    // "wing" (shoulder, the joint nearest half-way, tip). They win over arms.
    struct WingChain {
        int shoulder = -1, mid = -1, tip = -1;
        double reach = 0;
    };
    std::vector<WingChain> wingChains;
    {
        const std::size_t nj = m.joints.size();
        std::vector<char> named(nj, 0);
        for (std::size_t j = 0; j < nj; ++j) {
            std::string n = m.joints[j].name;
            for (char& c : n) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            named[j] = !m.joints[j].endSite && n.find("wing") != std::string::npos;
        }
        for (std::size_t j = 0; j < nj; ++j) {
            const int p = m.joints[j].parent;
            if (!named[j] || (p >= 0 && named[uz(p)])) continue;
            WingChain wc;
            wc.shoulder = static_cast<int>(j);
            for (std::size_t k = j + 1; k < nj; ++k) {
                int a = m.joints[k].parent;
                while (a > wc.shoulder) a = m.joints[uz(a)].parent;
                if (a != wc.shoulder) continue;
                const double l = len(rest[k] - rest[j]);
                if (l > wc.reach) {
                    wc.reach = l;
                    wc.tip = static_cast<int>(k);
                }
            }
            if (wc.tip < 0 || wc.reach < 1e-9) continue;
            double best = 1e300;
            for (int a = m.joints[uz(wc.tip)].parent; a > wc.shoulder; a = m.joints[uz(a)].parent) {
                const double d = std::abs(len(rest[uz(a)] - rest[j]) - wc.reach / 2);
                if (d < best) {
                    best = d;
                    wc.mid = a;
                }
            }
            wingChains.push_back(wc);
        }
        std::stable_sort(wingChains.begin(), wingChains.end(), [](const WingChain& a, const WingChain& b) { return a.reach > b.reach; });
        if (wingChains.size() > 2) wingChains.resize(2);
    }
    const bool ownWings = wingChains.size() == 2;
    if (!ownWings) {
        if (creatureMap(m).hasFourLegs())
            return Error(ErrorCode::InvalidArgument, "a four-legged motion has no arms to drive the wings; use a humanoid motion such as preset:flap, "
                                                     "or a skeleton with two chains of joints named wing");
        for (Bone b : {Bone::Hips, Bone::LUpperArm, Bone::LHand, Bone::RUpperArm, Bone::RHand})
            if (map[b] < 0)
                return Error(ErrorCode::InvalidArgument,
                             "the motion's skeleton has no recognisable hips and two arms to drive the wings (map them with Bone=joint overrides), "
                             "and no two chains of joints named wing");
    }
    // The joints that drive: [0], [1] are the two wings (or the left and right arm).
    int shJ[2], elJ[2], handJ[2], hipsJ = map[Bone::Hips];
    for (int a = 0; a < 2; ++a) {
        shJ[a] = ownWings ? wingChains[uz(a)].shoulder : map[a == 0 ? Bone::LUpperArm : Bone::RUpperArm];
        elJ[a] = ownWings ? wingChains[uz(a)].mid : map[a == 0 ? Bone::LForearm : Bone::RForearm];
        handJ[a] = ownWings ? wingChains[uz(a)].tip : map[a == 0 ? Bone::LHand : Bone::RHand];
    }
    if (hipsJ < 0) {
        hipsJ = 0;   // the root the wings hang from
        for (int a = shJ[0]; a >= 0; a = m.joints[uz(a)].parent) hipsJ = a;
    }
    const motion::Subject subj = motion::analyzeSubject(sprite);
    const motion::detail::WingSpec spec{std::clamp(opt.wingLine, 0.05, 0.95), 0.0, 1.0, opt.wingMask.isNull() ? nullptr : &opt.wingMask};
    auto built = motion::detail::buildFlightRig(sprite, subj, spec, false);
    if (!built || built->wings.empty())
        return Error(ErrorCode::InvalidArgument, "no wings were found in the sprite (select the wings, or set the wing line)");
    motion::detail::FlightRig& fr = *built;
    const rg::Rig& rig = fr.rig;
    const IntRect bb = subj.bounds;
    const double amount = std::clamp(opt.amount, 0.25, 2.0);

    // Winged sprites are drawn from the side; the head shows which way.
    const bool turnKnown = opt.drawnTurn && std::isfinite(*opt.drawnTurn);
    const double drawnTurn = turnKnown ? *opt.drawnTurn : (fr.headSide < 0 ? -90 : 90);
    res.camera = opt.camera ? *opt.camera : Camera{};
    if (!opt.camera) res.camera.turn = drawnTurn;
    const Camera& cam = res.camera;
    if (std::abs(wrapAngle((cam.turn - drawnTurn) * kDeg)) / kDeg > 30 || std::abs(cam.tilt) > 40) {
        res.approximateView = true;
        res.notes.push_back("The sprite is drawn " +
                            (turnKnown ? "at turn " + std::to_string(iround(drawnTurn)) + " degrees" : std::string("from the side")) +
                            "; this camera only changes how the motion is seen, the art is not turned. "
                            "A view this far from the drawing needs art drawn for it (a reference view).");
    }
    if (fr.headSide == 0 && !turnKnown) res.notes.push_back("The head end was not found; the sprite is taken to face right.");

    const double start = std::clamp(opt.start, 0.0, m.duration());
    const double span = opt.length > 0 ? std::min(opt.length, m.duration() - start) : m.duration() - start;
    if (span <= 0) return Error(ErrorCode::InvalidArgument, "the time span is empty");
    const int fps = std::clamp(opt.fps, 1, 60);
    const int frames = std::clamp(opt.frames > 0 ? opt.frames : std::max(2, iround(span * fps)), 2, 240);
    const double step = opt.loop ? span / frames : (frames > 1 ? span / (frames - 1) : 0);

    double restMinY = 1e300, restMaxY = -1e300, restSpan = 0;
    for (const Vec3& q : rest) {
        restMinY = std::min(restMinY, q.y);
        restMaxY = std::max(restMaxY, q.y);
        for (const Vec3& o : rest) restSpan = std::max(restSpan, len(q - o));
    }
    // A bird's skeleton is longer and wider than it is tall.
    const double height = std::max(1e-6, ownWings ? restSpan : restMaxY - restMinY);
    auto hipsOf = [&](const std::vector<Vec3>& q) { return q[uz(hipsJ)]; };
    std::vector<std::vector<Vec3>> world;
    double floorY = 1e300;
    for (int f = 0; f < frames; ++f) {
        std::vector<Vec3> w = jointPositions(m, start + f * step);
        for (const Vec3& q : w) floorY = std::min(floorY, q.y);
        world.push_back(std::move(w));
    }
    const Vec3 hips0 = hipsOf(world[0]);
    const Vec3 centre{hips0.x, floorY, hips0.z};
    // With its own wings: forward is from the root to a joint named head,
    // else square to the line between the wings (which way round is a guess).
    Vec3 forward{0, 0, 1};
    bool forwardGuessed = false;
    if (ownWings) {
        Vec3 f{0, 0, 0};
        for (std::size_t j = 0; j < m.joints.size() && len(f) < 1e-6; ++j) {
            std::string n = m.joints[j].name;
            for (char& c : n) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (n.find("head") != std::string::npos) f = horizontal(rest[j] - rest[uz(hipsJ)]);
        }
        if (len(f) < 1e-6) {
            f = cross(horizontal(rest[uz(shJ[0])] - rest[uz(shJ[1])]), Vec3{0, 1, 0});
            forwardGuessed = true;
        }
        if (len(f) > 1e-6) forward = norm(f);
    } else {
        forward = forwardAt(m, map, world[0]);
    }
    std::vector<Projected> prs;
    for (int f = 0; f < frames; ++f) {
        std::vector<Vec3> w = world[uz(f)];
        const Vec3 tr = horizontal(hipsOf(world[uz(f)]) - hips0);
        for (Vec3& q : w) q = q - tr;   // wings flap in place
        prs.push_back(project(w, cam, forward, height, centre));
    }

    // Each arm's elevation (radians above horizontal) and its elbow's bend
    // (the forearm's elevation over the upper arm's), per frame.
    // A wing of its own also folds (its tip nearer its shoulder than at its
    // longest in the span: 0 = spread) and sweeps (its first bone's lean
    // forward, radians).
    auto elevation = [](Vec3 v) { return std::atan2(v.y, std::hypot(v.x, v.z)); };
    std::array<std::vector<double>, 2> elev, elbow, fold, sweep;
    for (int a = 0; a < 2; ++a) {
        double longest = 1e-9;
        for (int f = 0; f < frames; ++f) {
            const auto& w = world[uz(f)];
            const Vec3 sh = w[uz(shJ[a])], hand = w[uz(handJ[a])];
            const Vec3 first = elJ[a] >= 0 ? w[uz(elJ[a])] - sh : hand - sh;
            if (elJ[a] >= 0) {
                const double up = elevation(first);
                elev[uz(a)].push_back(up);
                elbow[uz(a)].push_back(elevation(hand - w[uz(elJ[a])]) - up);
            } else {
                elev[uz(a)].push_back(elevation(hand - sh));
                elbow[uz(a)].push_back(0.0);
            }
            fold[uz(a)].push_back(len(hand - sh));
            longest = std::max(longest, len(hand - sh));
            sweep[uz(a)].push_back(len(first) > 1e-9 ? std::asin(std::clamp(dot(norm(first), forward), -1.0, 1.0)) : 0.0);
        }
        for (double& v : fold[uz(a)]) v = 1.0 - v / longest;
    }
    // Near arm: the one nearer the camera at the first frame.
    const int nearArm = prs[0].depth[uz(shJ[0])] >= prs[0].depth[uz(shJ[1])] ? 0 : 1;
    std::array<int, 2> topFrame{};
    double range = 0;
    for (int a = 0; a < 2; ++a) {
        const auto& e = elev[uz(a)];
        topFrame[uz(a)] = static_cast<int>(std::max_element(e.begin(), e.end()) - e.begin());
        range = std::max(range, *std::max_element(e.begin(), e.end()) - *std::min_element(e.begin(), e.end()));
    }
    if (range < 10 * kDeg) res.notes.push_back("The motion's arms hardly rise or fall; the wings barely move (try preset:flap).");

    // The body's heave: the hips' height from its neutral (the loop's mean,
    // a one-shot's first frame), in pixels of the sprite's height.
    double neutral = 0;
    if (opt.loop) {
        for (int f = 0; f < frames; ++f) neutral += hipsOf(world[uz(f)]).y;
        neutral /= frames;
    } else {
        neutral = hipsOf(world[0]).y;
    }
    const double k = bb.h / height;
    const int maxLift = std::max(1, bb.h / 8);

    // The tail waves with the stroke, as in Animate's flap: the near wing's
    // stroke (0 at the top, 1 at the bottom of the span), later towards the tip.
    const std::vector<int> tail = motion::detail::flightTailColumns(fr, bb);
    const int tailAmp = std::max(1, iround(static_cast<double>(tail.size()) * 0.15 * amount));
    std::vector<double> stroke(uz(frames), 0.0);
    {
        const int a = nearArm;
        const auto& e = elev[uz(a)];
        const double hi = *std::max_element(e.begin(), e.end()), lo = *std::min_element(e.begin(), e.end());
        if (hi - lo >= 10 * kDeg)
            for (int f = 0; f < frames; ++f) stroke[uz(f)] = (hi - e[uz(f)]) / (hi - lo);
    }
    auto strokeAt = [&](double f) {
        int i = static_cast<int>(std::lround(f));
        if (opt.loop) i = ((i % frames) + frames) % frames;
        else i = std::clamp(i, 0, frames - 1);
        // A loop swings either side of mid-stroke, a one-shot from its start.
        return opt.loop ? 2 * (stroke[uz(i)] - 0.5) : stroke[uz(i)] - stroke[0];
    };
    // Which way each drawn wing runs from its shoulder (to its farthest pixel).
    std::vector<char> wingUpright(fr.wings.size(), 1), wingUp(fr.wings.size(), 1);
    for (std::size_t i = 0; i < fr.wings.size(); ++i) {
        const auto& c = fr.wings[i].cut;
        const IntRect mb = c.mask.bounds();
        double best = -1, bx = 0, by = -1;
        for (int y = mb.y; y < mb.y + mb.h; ++y)
            for (int x = mb.x; x < mb.x + mb.w; ++x) {
                if (!c.mask.get(x, y)) continue;
                const double d = std::hypot(x + 0.5 - c.shoulder.x, y + 0.5 - c.shoulder.y);
                if (d > best) {
                    best = d;
                    bx = x + 0.5 - c.shoulder.x;
                    by = y + 0.5 - c.shoulder.y;
                }
            }
        wingUpright[i] = std::abs(by) >= std::abs(bx);
        wingUp[i] = by < 0;
    }
    bool sweepShown = false;
    double reach = 0;
    for (const auto& c : fr.cuts) reach = std::max(reach, c.reach);
    const int margin = static_cast<int>(std::ceil(reach)) + maxLift + tailAmp + 4;
    const IntRect big{bb.x - margin, bb.y - margin, bb.w + 2 * margin, bb.h + 2 * margin};
    std::vector<Image> imgs;
    std::vector<std::vector<rg::PartPose>> poses;
    for (int f = 0; f < frames; ++f) {
        std::vector<rg::PartPose> pose = rig.rest();
        const int dy = std::clamp(-iround(k * (hipsOf(world[uz(f)]).y - neutral) * amount), -maxLift, maxLift);
        pose[uz(fr.bodyPart)].dy = dy;
        if (fr.chestPart >= 0) pose[uz(fr.chestPart)].dy = dy;
        if (!tail.empty())
            pose[uz(fr.bodyPart)].colDy = motion::detail::tailColumnDy(fr, tail, [&](double u) {
                return iround(-tailAmp * u * strokeAt(f - 0.3 * u * frames));
            });
        for (std::size_t wi = 0; wi < fr.wings.size(); ++wi) {
            const auto& w = fr.wings[wi];
            const int a = w.nearWing ? nearArm : 1 - nearArm;
            const double scale = w.nearWing ? 1.0 : 0.8;
            const double down = std::clamp(amount * (elev[uz(a)][uz(topFrame[uz(a)])] - elev[uz(a)][uz(f)]), -0.5, 2.0);
            const double bend = std::clamp(amount * (elbow[uz(a)][uz(topFrame[uz(a)])] - elbow[uz(a)][uz(f)]), -1.2, 1.2);
            rg::PartPose& q = pose[uz(w.part)];
            q.angle = w.cut.sign * down * scale;
            q.bend = w.cut.sign * bend * scale * 0.9;
            q.bendRadius = w.cut.reach;
            q.dy = dy;
            if (w.nearWing && !fr.painted) {
                q.skinTop = fr.rootRow;
                q.skinDepth = fr.skinDepth;
                q.skinX0 = fr.skinX0;
                q.skinX1 = fr.skinX1;
                q.skinFade = std::max(3.0, std::abs(fr.cuts.size() == 2 ? fr.cuts[1 - fr.nearIdx].shoulder.x - w.cut.shoulder.x
                                                                        : fr.skinDepth / 2.0));
            }
            if (!w.nearWing) q.sx = 1.0 - 0.15 * std::clamp(down / 1.6, 0.0, 1.0);
            if (ownWings) {
                // Folded: shorter along its drawn length. Swept: an upright
                // wing leans toward the head (forward) or the tail.
                const int top = topFrame[uz(a)];
                const double shorter = std::clamp(1.0 - amount * (fold[uz(a)][uz(f)] - fold[uz(a)][uz(top)]), 0.55, 1.1);
                (wingUpright[wi] ? q.sy : q.sx) *= shorter;
                const double lean = std::tan(std::clamp(amount * (sweep[uz(a)][uz(f)] - sweep[uz(a)][uz(top)]), -0.5, 0.5));
                if (wingUpright[wi]) {
                    q.shear = (wingUp[wi] ? -1 : 1) * (fr.headSide < 0 ? -1 : 1) * lean * scale;
                    sweepShown = sweepShown || std::abs(lean) > 0.05;
                }
            }
        }
        imgs.push_back(rg::renderPose(rig, pose, big));
        poses.push_back(std::move(pose));
    }

    int x0 = big.w, y0 = big.h, x1 = -1, y1 = -1;
    for (const Image& im : imgs) {
        const IntRect r = im.opaqueBounds();
        if (r.empty()) continue;
        x0 = std::min(x0, r.x);
        y0 = std::min(y0, r.y);
        x1 = std::max(x1, r.x + r.w);
        y1 = std::max(y1, r.y + r.h);
    }
    if (x1 < 0) return Error(ErrorCode::Internal, "retarget produced empty frames");
    const int frameMs = std::max(10, iround(1000.0 * step));
    res.clip.name = "retarget";
    res.clip.loops = opt.loop;
    res.clip.body = motion::Body::Flyer;
    res.clip.depthPops = static_cast<int>(rg::zPops(rig, poses, big).size());
    for (const Image& im : imgs) {
        res.clip.frames.push_back(im.copy({x0, y0, x1 - x0, y1 - y0}));
        res.clip.durations.push_back(frameMs);
    }
    const int fw = x1 - x0, fh = y1 - y0;
    const int side = std::max(fw, fh);
    res.source.name = "source";
    res.source.loops = opt.loop;
    for (const Projected& pr : prs) {
        Image img(side, side);
        paintSkeleton(img, m, map, pr, side * 0.75, side / 2.0, side * 0.9);
        res.source.frames.push_back(img);
        res.source.durations.push_back(frameMs);
    }
    if (fr.wings.size() < 2) res.notes.push_back(ownWings ? "Only one wing was found; it follows the nearer wing." : "Only one wing was found; it follows the nearer arm.");
    if (ownWings) {
        res.notes.push_back("Wings: " + m.joints[uz(shJ[0])].name + " and " + m.joints[uz(shJ[1])].name +
                            " (the skeleton's own wing bones: the drawn wings beat, fold and sweep with them).");
        bool sideways = false;
        for (char u : wingUpright) sideways = sideways || !u;
        if (sideways) res.notes.push_back("A wing drawn out to the side cannot lean: its sweep is not shown.");
        if (forwardGuessed && sweepShown)
            res.notes.push_back("The skeleton has no joint named head: which way it faces is a guess, so the wings may sweep the wrong way round.");
    }
    return res;
}

// A motion's spine on a fish-like sprite (the swim rig of RigSwim.cpp). The
// spine is the chain from the joint named head to the one named tail (the
// longest chain of the skeleton when it has no such names). Its sideways
// (or up-and-down) wave, measured from the head, bends the body column by
// column; the tail fin turns as the chain's end turns against the stalk.
Result<RetargetResult> retargetSwimmer(const Image& sprite, const Motion& m, const RetargetOptions& opt) {
    RetargetResult res;
    res.map.joint.fill(-1);
    const motion::Subject subj = motion::analyzeSubject(sprite);
    auto built = motion::detail::buildSwimRig(sprite, subj, motion::SwimStyle::Auto);
    if (!built) return Error(ErrorCode::InvalidArgument, "the sprite is too small to bend as a swimmer (8 by 4 pixels at least)");
    motion::detail::SwimRig& sr = *built;
    const rg::Rig& rig = sr.rig;
    const IntRect bb = subj.bounds;
    const double amount = std::clamp(opt.amount, 0.25, 2.0);
    const int dir = sr.dir;

    // The spine chain, head first.
    const std::vector<Vec3> rest = restPositions(m);
    const std::size_t nj = m.joints.size();
    auto lowerName = [&](std::size_t j) {
        std::string n = m.joints[j].name;
        for (char& c : n) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return n;
    };
    auto pathBetween = [&](int a, int b) {
        std::vector<int> up, down;
        std::vector<int> depthOf(nj, 0);
        for (std::size_t j = 0; j < nj; ++j) depthOf[j] = m.joints[j].parent >= 0 ? depthOf[uz(m.joints[j].parent)] + 1 : 0;
        while (a != b && a >= 0 && b >= 0) {
            if (depthOf[uz(a)] >= depthOf[uz(b)]) {
                up.push_back(a);
                a = m.joints[uz(a)].parent;
            } else {
                down.push_back(b);
                b = m.joints[uz(b)].parent;
            }
        }
        if (a < 0 || b < 0) return std::vector<int>{};   // two roots
        up.push_back(a);
        up.insert(up.end(), down.rbegin(), down.rend());
        return up;
    };
    auto pathLength = [&](const std::vector<int>& path) {
        double l = 0;
        for (std::size_t i = 1; i < path.size(); ++i) l += len(rest[uz(path[i])] - rest[uz(path[i - 1])]);
        return l;
    };
    // The farthest joint from `from` along the skeleton (and its distance).
    auto farthest = [&](int from) {
        int best = from;
        double bestLen = 0;
        for (std::size_t j = 0; j < nj; ++j) {
            const double l = pathLength(pathBetween(from, static_cast<int>(j)));
            if (l > bestLen) {
                bestLen = l;
                best = static_cast<int>(j);
            }
        }
        return best;
    };
    int headJ = -1, tailJ = -1;
    for (std::size_t j = 0; j < nj; ++j) {
        const std::string n = lowerName(j);
        if (headJ < 0 && n.find("head") != std::string::npos && n.find("_end") == std::string::npos) headJ = static_cast<int>(j);
        if (n.find("tail") != std::string::npos) tailJ = static_cast<int>(j);   // the last (deepest) tail joint
    }
    bool byName = headJ >= 0 && tailJ >= 0;
    std::vector<int> chain;
    if (byName) {
        chain = pathBetween(headJ, tailJ);
    } else {
        const int a = farthest(0), b = farthest(a);
        chain = pathBetween(headJ >= 0 ? headJ : a, headJ >= 0 ? farthest(headJ) : b);
    }
    const double chainLen = pathLength(chain);
    if (chain.size() < 3 || chainLen < 1e-6)
        return Error(ErrorCode::InvalidArgument, "the motion's skeleton has no spine of three joints or more to bend the body with (try preset:swim)");

    const double start = std::clamp(opt.start, 0.0, m.duration());
    const double span = opt.length > 0 ? std::min(opt.length, m.duration() - start) : m.duration() - start;
    if (span <= 0) return Error(ErrorCode::InvalidArgument, "the time span is empty");
    const int fps = std::clamp(opt.fps, 1, 60);
    const int frames = std::clamp(opt.frames > 0 ? opt.frames : std::max(2, iround(span * fps)), 2, 240);
    const double step = opt.loop ? span / frames : (frames > 1 ? span / (frames - 1) : 0);
    std::vector<std::vector<Vec3>> world;
    double floorY = 1e300;
    for (int f = 0; f < frames; ++f) {
        std::vector<Vec3> w = jointPositions(m, start + f * step);
        for (const Vec3& q : w) floorY = std::min(floorY, q.y);
        world.push_back(std::move(w));
    }
    // Without names the head is the end that moves less across the chain's line.
    const Vec3 axis0 = norm(rest[uz(chain.back())] - rest[uz(chain.front())]);
    if (!byName && headJ < 0) {
        auto wander = [&](int j, int other) {
            double sum = 0;
            for (int f = 0; f < frames; ++f) {
                const Vec3 d = (world[uz(f)][uz(j)] - world[uz(f)][uz(other)]) - (world[0][uz(j)] - world[0][uz(other)]);
                sum += len(d - axis0 * dot(d, axis0));
            }
            return sum;
        };
        const std::size_t mid = chain.size() / 2;
        if (wander(chain.front(), chain[mid]) > wander(chain.back(), chain[mid])) std::reverse(chain.begin(), chain.end());
    }
    const Vec3 axis = norm(rest[uz(chain.back())] - rest[uz(chain.front())]);   // head -> tail
    // The two directions across the spine; the wave is in the one it moves in more.
    const Vec3 upv = std::abs(axis.y) < 0.9 ? Vec3{0, 1, 0} : Vec3{1, 0, 0};
    const Vec3 side1 = norm(cross(axis, upv)), side2 = norm(cross(axis, side1));
    std::vector<double> along(chain.size(), 0.0);   // 0 at the head .. 1 at the tail's end
    for (std::size_t i = 1; i < chain.size(); ++i)
        along[i] = along[i - 1] + len(rest[uz(chain[i])] - rest[uz(chain[i - 1])]) / chainLen;
    auto offsets = [&](Vec3 across) {
        std::vector<std::vector<double>> o(uz(frames), std::vector<double>(chain.size(), 0.0));
        for (int f = 0; f < frames; ++f)
            for (std::size_t i = 0; i < chain.size(); ++i)
                o[uz(f)][i] = dot(world[uz(f)][uz(chain[i])] - world[uz(f)][uz(chain.front())], across) / chainLen;
        // From the neutral shape: a loop's mean, a one-shot's first frame.
        for (std::size_t i = 0; i < chain.size(); ++i) {
            double neutral = o[0][i];
            if (opt.loop) {
                neutral = 0;
                for (int f = 0; f < frames; ++f) neutral += o[uz(f)][i];
                neutral /= frames;
            }
            for (int f = 0; f < frames; ++f) o[uz(f)][i] -= neutral;
        }
        return o;
    };
    auto swingOf = [&](const std::vector<std::vector<double>>& o) {
        double s = 0;
        for (const auto& row : o)
            for (double v : row) s = std::max(s, std::abs(v));
        return s;
    };
    std::vector<std::vector<double>> wave = offsets(side1);
    {
        std::vector<std::vector<double>> other = offsets(side2);
        if (swingOf(other) > swingOf(wave)) wave = std::move(other);
    }
    if (swingOf(wave) < 0.02) res.notes.push_back("The motion's spine hardly bends; the body barely moves (try preset:swim).");
    // The wave at `u` along the chain in frame f, as a fraction of the chain's length.
    auto waveAt = [&](int f, double u) {
        u = std::clamp(u, 0.0, 1.0);
        std::size_t i = 1;
        while (i + 1 < chain.size() && along[i] < u) ++i;
        const double a = along[i - 1], b = along[i];
        const double t = b > a ? std::clamp((u - a) / (b - a), 0.0, 1.0) : 0.0;
        return wave[uz(f)][i - 1] * (1 - t) + wave[uz(f)][i] * t;
    };

    // Fin joints: branches off the spine named as fins. Each gives a turn
    // per frame: how far its direction (to its farthest joint) swings toward
    // the tail, or up and down when it moves more that way, from its neutral.
    struct FinDrive {
        std::string name;
        bool top = false;          // points up from the spine (a dorsal fin)
        double u = 0;              // where it joins, 0 head .. 1 tail
        std::vector<double> turn;  // toward the tail is positive
    };
    std::vector<FinDrive> finDrives;
    {
        std::vector<int> chainAt(nj, -1);
        for (std::size_t i = 0; i < chain.size(); ++i) chainAt[uz(chain[i])] = static_cast<int>(i);
        for (std::size_t j = 0; j < nj; ++j) {
            const int p = m.joints[j].parent;
            if (p < 0 || chainAt[uz(p)] < 0 || chainAt[j] >= 0 || m.joints[j].endSite) continue;
            const std::string n = lowerName(j);
            bool named = false;
            for (const char* word : {"fin", "flipper", "pectoral", "pelvic", "dorsal"}) named = named || n.find(word) != std::string::npos;
            if (!named) continue;
            // Its farthest joint (parents come before their children).
            int tip = static_cast<int>(j);
            double reach = 0;
            for (std::size_t k = j + 1; k < nj; ++k) {
                int a = m.joints[k].parent;
                while (a > static_cast<int>(j)) a = m.joints[uz(a)].parent;
                if (a != static_cast<int>(j)) continue;
                const double l = len(rest[k] - rest[j]);
                if (l > reach) {
                    reach = l;
                    tip = static_cast<int>(k);
                }
            }
            const int from = tip == static_cast<int>(j) ? p : static_cast<int>(j), to = tip == static_cast<int>(j) ? static_cast<int>(j) : tip;
            if (len(rest[uz(to)] - rest[uz(from)]) < 1e-9) continue;
            const Vec3 d0 = norm(rest[uz(to)] - rest[uz(from)]);
            const double up0 = dot(d0, upv);
            const Vec3 lateral = d0 - axis * dot(d0, axis) - upv * up0;
            const std::size_t ci = uz(chainAt[uz(p)]);
            const int before = chain[ci > 0 ? ci - 1 : ci], after = chain[ci + 1 < chain.size() ? ci + 1 : ci];
            std::vector<double> back(uz(frames), 0.0), rise(uz(frames), 0.0);
            for (int f = 0; f < frames; ++f) {
                const std::vector<Vec3>& w = world[uz(f)];
                const Vec3 d = norm(w[uz(to)] - w[uz(from)]);
                back[uz(f)] = dot(d, norm(w[uz(after)] - w[uz(before)]));   // against the spine where it joins
                rise[uz(f)] = dot(d, upv);
            }
            double backSwing = 0, riseSwing = 0;
            for (std::vector<double>* v : {&back, &rise}) {
                double neutral = (*v)[0];
                if (opt.loop) {
                    neutral = 0;
                    for (double x : *v) neutral += x;
                    neutral /= frames;
                }
                double swing = 0;
                for (double& x : *v) {
                    x -= neutral;
                    swing = std::max(swing, std::abs(x));
                }
                (v == &back ? backSwing : riseSwing) = swing;
            }
            FinDrive fd;
            fd.name = m.joints[j].name;
            fd.top = up0 > 0 && up0 >= len(lateral);
            fd.u = std::min(1.0, along[ci] / (sr.tailPart >= 0 && sr.stalk >= 0 ? 0.8 : 1.0));
            // A lower fin lifting folds back against the body; a dorsal fin lifting stands up.
            for (int f = 0; f < frames; ++f)
                fd.turn.push_back(backSwing >= riseSwing ? std::asin(std::clamp(back[uz(f)], -1.0, 1.0))
                                                         : std::asin(std::clamp(rise[uz(f)], -1.0, 1.0)) * (fd.top ? -1 : 1));
            if (std::max(backSwing, riseSwing) >= 0.02) finDrives.push_back(std::move(fd));
        }
    }
    // Each drawn fin takes the nearest fin joint of its kind (top or lower).
    std::vector<int> driveOf(sr.fins.size(), -1);
    for (std::size_t i = 0; i < sr.fins.size(); ++i)
        for (std::size_t k = 0; k < finDrives.size(); ++k)
            if (finDrives[k].top == sr.fins[i].top &&
                (driveOf[i] < 0 || std::abs(finDrives[k].u - sr.fins[i].u) < std::abs(finDrives[uz(driveOf[i])].u - sr.fins[i].u)))
                driveOf[i] = static_cast<int>(k);

    // A swimmer is drawn from the side; the camera only shows the source.
    const bool turnKnown = opt.drawnTurn && std::isfinite(*opt.drawnTurn);
    const double drawnTurn = turnKnown ? *opt.drawnTurn : (dir > 0 ? -90 : 90);
    res.camera = opt.camera ? *opt.camera : Camera{};
    if (!opt.camera) res.camera.turn = drawnTurn;
    const Camera& cam = res.camera;
    if (std::abs(wrapAngle((cam.turn - drawnTurn) * kDeg)) / kDeg > 30 || std::abs(cam.tilt) > 40) {
        res.approximateView = true;
        res.notes.push_back("The sprite is drawn from the side; this camera only changes how the motion is shown, the art bends the same way.");
    }
    const Vec3 forward = norm(horizontal(axis * -1.0));
    const Vec3 head0 = world[0][uz(chain.front())];
    const Vec3 centre{head0.x, floorY, head0.z};
    std::vector<Projected> prs;
    for (int f = 0; f < frames; ++f) {
        std::vector<Vec3> w = world[uz(f)];
        if (opt.inPlace) {
            const Vec3 tr = horizontal(world[uz(f)][uz(chain.front())] - head0);
            for (Vec3& q : w) q = q - tr;
        }
        prs.push_back(project(w, cam, len(forward) > 0.5 ? forward : Vec3{0, 0, 1}, chainLen, centre));
    }

    // The body: the chain's first part runs to the tail stalk, the rest is the fin.
    const bool hasFin = sr.tailPart >= 0 && sr.stalk >= 0;
    const double stalkU = hasFin ? 0.8 : 1.0;
    const int bodyLen = hasFin ? sr.stalk + 1 : bb.w;
    const double px = static_cast<double>(bodyLen) / stalkU * amount;   // pixels per chain length
    const int maxBend = std::max(1, bb.h * 35 / 100);
    const rg::Part& bodyP = rig.parts[uz(sr.bodyPart)];
    const IntRect pb{bodyP.hiBox.x + 1, bodyP.hiBox.y + 1, bodyP.hiBox.w - 2, bodyP.hiBox.h - 2};
    auto alongX = [&](int x) { return (x - sr.headX) * dir; };
    // The fin's turn: the chain's end against the stalk before it (down is positive).
    std::vector<double> finTurn(uz(frames), 0.0);
    double finPeak = 0;
    for (int f = 0; f < frames; ++f) {
        const double before = std::atan((waveAt(f, stalkU) - waveAt(f, stalkU - 0.2)) / 0.2);
        const double after = std::atan((waveAt(f, 1.0) - waveAt(f, stalkU)) / std::max(0.05, 1.0 - stalkU));
        finTurn[uz(f)] = std::clamp((after - before) * amount, -0.6, 0.6);
        finPeak = std::max(finPeak, std::abs(finTurn[uz(f)]));
    }
    const int margin = maxBend + std::max(4, bb.h / 4) + 2;
    const IntRect big{bb.x - margin, bb.y - margin, bb.w + 2 * margin, bb.h + 2 * margin};
    std::vector<Image> imgs;
    std::vector<std::vector<rg::PartPose>> poses;
    for (int f = 0; f < frames; ++f) {
        std::vector<rg::PartPose> pose = rig.rest();
        // Columns move by whole pixels, neighbours at most one apart.
        std::vector<int> byK(uz(bb.w), 0);
        int prev = 0;
        for (int k = 0; k < bb.w; ++k) {
            const double u = static_cast<double>(k) / std::max(1, bodyLen - 1) * stalkU;
            const int want = std::clamp(iround(waveAt(f, u) * px), -maxBend, maxBend);
            prev = std::clamp(want, prev - 1, prev + 1);
            byK[uz(k)] = prev;
        }
        std::vector<int> colDy(uz(std::max(0, pb.w)), 0);
        for (int c = 0; c < pb.w; ++c) colDy[uz(c)] = byK[uz(std::clamp(alongX(pb.x + c), 0, bb.w - 1))];
        auto dyAt = [&](double x) {
            if (colDy.empty()) return 0;
            return colDy[uz(std::clamp(static_cast<int>(std::floor(x)) - pb.x, 0, pb.w - 1))];
        };
        pose[uz(sr.bodyPart)].colDy = colDy;
        if (hasFin) {
            rg::PartPose& tq = pose[uz(sr.tailPart)];
            tq.dy = dyAt(sr.stalkPt.x - dir * 1.0);
            tq.angle = dir * finTurn[uz(f)];
            // As Animate's swim: the fin narrows as it sweeps through the middle.
            if (finPeak > 0.05) tq.sx = 1.0 - 0.3 * (1.0 - std::abs(finTurn[uz(f)]) / finPeak);
        }
        for (std::size_t i = 0; i < sr.fins.size(); ++i) {
            const motion::detail::SwimFin& fn = sr.fins[i];
            rg::PartPose& fq = pose[uz(fn.part)];
            fq.dy = dyAt(fn.root.x);   // the fins ride the body, column by column
            {
                const rg::Part& fp = rig.parts[uz(fn.part)];
                fq.colDy.assign(uz(std::max(0, fp.hiBox.w - 2)), 0);
                for (std::size_t c = 0; c < fq.colDy.size(); ++c)
                    fq.colDy[c] = dyAt(fp.hiBox.x + 1 + static_cast<double>(c) + 0.5) - dyAt(fn.root.x);
            }
            // and paddle about their roots (0.35 rad at most: further, and the
            // root drawn behind the body would show past its edge): toward the tail (at +dir) is
            // anticlockwise for a fin hanging down, clockwise for one standing up.
            if (driveOf[i] >= 0)
                fq.angle = std::clamp(finDrives[uz(driveOf[i])].turn[uz(f)] * amount, -0.35, 0.35) * (fn.top ? dir : -dir);
        }
        imgs.push_back(rg::renderPose(rig, pose, big));
        poses.push_back(std::move(pose));
    }

    int x0 = big.w, y0 = big.h, x1 = -1, y1 = -1;
    for (const Image& im : imgs) {
        const IntRect r = im.opaqueBounds();
        if (r.empty()) continue;
        x0 = std::min(x0, r.x);
        y0 = std::min(y0, r.y);
        x1 = std::max(x1, r.x + r.w);
        y1 = std::max(y1, r.y + r.h);
    }
    if (x1 < 0) return Error(ErrorCode::Internal, "retarget produced empty frames");
    const int frameMs = std::max(10, iround(1000.0 * step));
    res.clip.name = "retarget";
    res.clip.loops = opt.loop;
    res.clip.body = motion::Body::Swimmer;
    res.clip.depthPops = static_cast<int>(rg::zPops(rig, poses, big).size());
    for (const Image& im : imgs) {
        res.clip.frames.push_back(im.copy({x0, y0, x1 - x0, y1 - y0}));
        res.clip.durations.push_back(frameMs);
    }
    const int side = std::max(x1 - x0, y1 - y0);
    res.source.name = "source";
    res.source.loops = opt.loop;
    for (const Projected& pr : prs) {
        Image img(side, side);
        paintSkeleton(img, m, res.map, pr, side * 0.75, side / 2.0, side * 0.6);
        res.source.frames.push_back(img);
        res.source.durations.push_back(frameMs);
    }
    res.notes.push_back(std::string("Spine: ") + m.joints[uz(chain.front())].name + " to " + m.joints[uz(chain.back())].name + " (" +
                        std::to_string(chain.size()) + " joints" + (byName ? "" : ", the skeleton's longest chain") + ").");
    if (!hasFin) res.notes.push_back("No tail fin was found behind a narrow tail stalk; the whole body bends.");
    {
        std::string paddling;
        std::size_t riding = 0;
        for (std::size_t i = 0; i < sr.fins.size(); ++i) {
            if (driveOf[i] < 0) {
                ++riding;
                continue;
            }
            const std::string& n = finDrives[uz(driveOf[i])].name;
            if (paddling.find(n) == std::string::npos) paddling += (paddling.empty() ? "" : ", ") + n;
        }
        if (!paddling.empty()) res.notes.push_back("Fins: " + std::to_string(sr.fins.size() - riding) + " paddle with " + paddling + ".");
        if (riding)
            res.notes.push_back(std::to_string(riding) + " fin(s) only ride the body: the motion has no moving fin joint of their kind off its spine "
                                "(joints named fin, flipper, pectoral, pelvic or dorsal; preset:swim has them).");
    }
    return res;
}

Result<RetargetResult> retargetQuadruped(const Image& sprite, const Motion& m, const RetargetOptions& opt) {
    RetargetResult res;
    res.creature = creatureMap(m);
    const CreatureMap& cm = res.creature;
    if (!cm.hasFourLegs())
        return Error(ErrorCode::InvalidArgument,
                     "the motion's skeleton has no four legs standing on the floor" + (cm.notes.empty() ? std::string() : " (" + cm.notes.front() + ")"));
    res.map.joint.fill(-1);
    res.map[Bone::Hips] = cm.root;
    const motion::Subject subj = motion::analyzeSubject(sprite);
    auto built = motion::detail::buildGaitRig(sprite, subj, motion::Body::Quadruped);
    if (!built || built->legs.size() < 2)
        return Error(ErrorCode::InvalidArgument, "no legs were found in the sprite (a four-legged sprite needs at least two legs drawn apart)");
    motion::detail::GaitRig& g = *built;
    const rg::Rig& rig = g.rig;
    const IntRect bb = subj.bounds;
    const double amount = std::clamp(opt.amount, 0.25, 2.0);

    const bool turnKnown = opt.drawnTurn && std::isfinite(*opt.drawnTurn);
    const double drawnTurn = turnKnown ? *opt.drawnTurn : (g.front ? 0 : (g.dir < 0 ? -90 : 90));
    res.camera = opt.camera ? *opt.camera : Camera{};
    if (!opt.camera) res.camera.turn = drawnTurn;
    const Camera& cam = res.camera;
    {
        double diff = std::abs(wrapAngle((cam.turn - drawnTurn) * kDeg)) / kDeg;
        if (!turnKnown && g.front) diff = std::min(diff, std::abs(wrapAngle((cam.turn - 180) * kDeg)) / kDeg);
        if (diff > 30 || std::abs(cam.tilt) > 40) {
            res.approximateView = true;
            res.notes.push_back("The sprite is drawn " +
                                (turnKnown ? "at turn " + std::to_string(iround(drawnTurn)) + " degrees"
                                           : std::string(g.front ? "from the front" : "from the side")) +
                                "; this camera only changes how the motion is seen, the art is not turned. "
                                "A view this far from the drawing needs art drawn for it (a reference view).");
        }
    }

    const double start = std::clamp(opt.start, 0.0, m.duration());
    const double span = opt.length > 0 ? std::min(opt.length, m.duration() - start) : m.duration() - start;
    if (span <= 0) return Error(ErrorCode::InvalidArgument, "the time span is empty");
    const int fps = std::clamp(opt.fps, 1, 60);
    const int frames = std::clamp(opt.frames > 0 ? opt.frames : std::max(2, iround(span * fps)), 2, 240);
    const double step = opt.loop ? span / frames : (frames > 1 ? span / (frames - 1) : 0);

    const std::vector<Vec3> rest = restPositions(m);
    double restMinY = 1e300, restMaxY = -1e300;
    for (const Vec3& p : rest) {
        restMinY = std::min(restMinY, p.y);
        restMaxY = std::max(restMaxY, p.y);
    }
    const double height = std::max(1e-6, restMaxY - restMinY);
    const int root = cm.root;
    auto footOf = [&](int s) { return cm.leg(uz(s)).joints.back(); };
    auto topOf = [&](int s) { return cm.leg(uz(s)).joints.front(); };
    const int srcLegs = static_cast<int>(cm.legCount());

    std::vector<std::vector<Vec3>> world;
    double floorY = 1e300;
    for (int f = 0; f < frames; ++f) {
        std::vector<Vec3> w = jointPositions(m, start + f * step);
        for (int s = 0; s < srcLegs; ++s) floorY = std::min(floorY, w[uz(footOf(s))].y);
        world.push_back(std::move(w));
    }
    const Vec3 root0 = world[0][uz(root)];
    const Vec3 centre{root0.x, floorY, root0.z};
    std::vector<Vec3> travel(uz(frames));
    for (int f = 0; f < frames; ++f) travel[uz(f)] = horizontal(world[uz(f)][uz(root)] - root0);
    std::vector<Projected> prs;
    for (int f = 0; f < frames; ++f) {
        std::vector<Vec3> w = world[uz(f)];
        if (opt.inPlace)
            for (Vec3& q : w) q = q - travel[uz(f)];
        prs.push_back(project(w, cam, cm.forward, height, centre));
    }
    const Projected& restPr = prs[0];

    // Drawn legs to source legs: same end, near to near.
    const std::size_t nLegs = g.legs.size();
    std::vector<int> src(nLegs, -1);
    // With middle pairs in the motion and more than four legs drawn (from
    // the side), the drawn legs are grouped from the back forward: the first
    // group takes the hind pair, the last the front pair, those between
    // the middle pairs.
    const std::size_t srcPairs = cm.legCount() / 2;
    std::vector<int> pairOf(nLegs, -1);   // the source pair's first leg, when grouped
    if (srcPairs > 2 && nLegs > 4 && !g.front) {
        std::vector<std::size_t> order(nLegs);
        for (std::size_t i = 0; i < nLegs; ++i) order[i] = i;
        std::sort(order.begin(), order.end(), [&](std::size_t x, std::size_t y) { return g.legs[x].hip.x * g.dir < g.legs[y].hip.x * g.dir; });
        const std::size_t groups = std::min(srcPairs, nLegs / 2);
        for (std::size_t rank = 0; rank < nLegs; ++rank) {
            const std::size_t grp = rank * groups / nLegs;
            int first = 0;
            if (grp + 1 == groups) first = 2;
            else if (grp > 0)
                first = 4 + 2 * static_cast<int>(std::clamp<long>(std::lround(static_cast<double>(grp * (srcPairs - 1)) / static_cast<double>(groups - 1)) - 1, 0,
                                                                  static_cast<long>(srcPairs) - 3));
            pairOf[order[rank]] = first;
        }
        res.notes.push_back("The motion has " + std::to_string(cm.legCount()) + " legs and the sprite " + std::to_string(nLegs) +
                            ": they are paired from the back forward.");
    } else if (srcPairs > 2) {
        res.notes.push_back("The motion has " + std::to_string(cm.legCount()) + " legs; the sprite's " + std::to_string(nLegs) +
                            " take its hind and front pairs (the middle legs are not used).");
    }
    for (std::size_t i = 0; i < nLegs; ++i) {
        const auto& L = g.legs[i];
        const int a = pairOf[i] >= 0 ? pairOf[i] : (L.hind ? 0 : 2), b = a + 1;
        const bool aNearer = restPr.depth[uz(topOf(a))] >= restPr.depth[uz(topOf(b))];
        int want = (L.nearLeg == aNearer) ? a : b;
        for (std::size_t j = 0; j < i; ++j)
            if (src[j] == want) want = want == a ? b : a;
        src[i] = want;
    }
    // Pixels per screen unit, per leg; the neutral foot offset (the loop's
    // average, or a one-shot's first frame) is where the foot is drawn.
    std::vector<double> k(nLegs), neutral(nLegs, 0.0);
    double kMean = 0;
    for (std::size_t i = 0; i < nLegs; ++i) {
        const auto& L = g.legs[i];
        const double drawn = g.front ? std::max(1.0, L.ankle.y - L.hip.y) : L.l1 + L.l2;
        k[i] = drawn / std::max(1e-6, cm.leg(uz(src[i])).length / height);
        kMean += k[i] / static_cast<double>(nLegs);
        const int used = opt.loop ? frames : 1;
        for (int f = 0; f < used; ++f) neutral[i] += (prs[uz(f)].x[uz(footOf(src[i]))] - prs[uz(f)].x[uz(topOf(src[i]))]) / used;
    }
    double stand = 0;
    {
        const int used = opt.loop ? frames : 1;
        for (int f = 0; f < used; ++f) stand += (world[uz(f)][uz(root)].y - floorY) / used;
    }
    // The back and its ends, as heights above the root: the hind and front
    // leg tops (the back's pitch), the head over the front tops and the tail
    // over the hind ones. Each is measured from its neutral (as the feet).
    enum { kHindEnd, kFrontEnd, kHeadEnd, kTailEnd };
    auto endHeight = [&](const std::vector<Vec3>& w, int which) {
        const double hind = 0.5 * (w[uz(topOf(0))].y + w[uz(topOf(1))].y), front = 0.5 * (w[uz(topOf(2))].y + w[uz(topOf(3))].y);
        switch (which) {
        case kHindEnd: return hind - w[uz(root)].y;
        case kFrontEnd: return front - w[uz(root)].y;
        case kHeadEnd: return cm.head >= 0 ? w[uz(cm.head)].y - front : 0.0;
        default: return cm.tail >= 0 ? w[uz(cm.tail)].y - hind : 0.0;
        }
    };
    double endNeutral[4] = {0, 0, 0, 0};
    {
        const int used = opt.loop ? frames : 1;
        for (int f = 0; f < used; ++f)
            for (int e = 0; e < 4; ++e) endNeutral[e] += endHeight(world[uz(f)], e) / used;
    }
    const double tiltCos = std::cos(std::clamp(cam.tilt, -90.0, 90.0) * kDeg);
    double legPx = 0;
    for (const auto& L : g.legs) legPx += (L.l1 + L.l2) / static_cast<double>(nLegs);
    if (legPx < 6) res.notes.push_back("The sprite's legs are only a few pixels long; steps will be small.");
    const int travelPx = opt.inPlace ? 0 : iround(kMean * span * 4);
    const int margin = iround(legPx) + bb.h / 2 + travelPx + 8;
    const IntRect big{bb.x - margin, bb.y - margin, bb.w + 2 * margin, bb.h + 2 * margin};

    std::vector<Image> imgs;
    std::vector<std::vector<rg::PartPose>> poses;
    int planted = 0, footFrames = 0;
    for (int f = 0; f < frames; ++f) {
        const Projected& pr = prs[uz(f)];
        const std::vector<Vec3>& w = world[uz(f)];
        const std::vector<Vec3> w2 = jointPositions(m, start + f * step + m.frameTime);
        std::vector<rg::PartPose> pose = rig.rest();
        int dy = iround(kMean * tiltCos * (stand - (w[uz(root)].y - floorY)) / height * amount);
        // Screen rows each end of the back moves (down is +), and the head
        // and tail over them; side views only (a front view shows no pitch).
        double endDy[4] = {0, 0, 0, 0};
        if (!g.front)
            for (int e = 0; e < 4; ++e)
                endDy[e] = std::clamp(-kMean * tiltCos * (endHeight(w, e) - endNeutral[e]) / height * amount, -bb.h / 4.0, bb.h / 4.0);
        auto legEndDy = [&](const motion::detail::GaitLeg& L) { return iround(endDy[L.hind ? kHindEnd : kFrontEnd]); };
        struct FootT {
            double x = 0, y = 0;
            bool planted = false;
        };
        std::vector<FootT> ft(nLegs);
        for (std::size_t i = 0; i < nLegs; ++i) {
            const auto& L = g.legs[i];
            const int s = src[i];
            const CreatureLeg& sl = cm.leg(uz(s));
            const double off = pr.x[uz(footOf(s))] - pr.x[uz(topOf(s))];
            FootT& t = ft[i];
            t.x = L.ankle.x + k[i] * (off - neutral[i]) * amount;
            const double footUp = w[uz(footOf(s))].y - floorY;
            t.y = L.ankle.y - std::max(0.0, k[i] * tiltCos * footUp / height * amount);
            const double speed = len(horizontal(w2[uz(footOf(s))] - w[uz(footOf(s))])) / m.frameTime;
            t.planted = footUp < 0.06 * sl.length && speed < 1.2 * sl.length;
            ++footFrames;
            planted += t.planted ? 1 : 0;
        }
        // Unless the body is in the air, the lowest foot carries it.
        bool any = false;
        for (const FootT& t : ft) any = any || t.planted;
        if (!any) {
            std::size_t low = 0;
            double lowH = 1e300;
            for (std::size_t i = 0; i < nLegs; ++i) {
                const double h = w[uz(footOf(src[i]))].y - floorY;
                if (h < lowH) {
                    lowH = h;
                    low = i;
                }
            }
            if (lowH < 0.15 * cm.leg(uz(src[low])).length) {
                ft[low].planted = true;
                ++planted;
            }
        }
        // Low enough for every planted foot to reach the ground.
        if (!g.front)
            for (std::size_t i = 0; i < nLegs; ++i) {
                if (!ft[i].planted) continue;
                const auto& L = g.legs[i];
                const double reach = 0.97 * (L.l1 + L.l2);
                const double dx = ft[i].x - L.hip.x;
                const double vertical = std::sqrt(std::max(0.0, reach * reach - dx * dx));
                dy = std::max(dy, static_cast<int>(std::ceil((L.ankle.y - L.hip.y) - vertical - 1e-9)) - legEndDy(L));
            }
        if (g.front) dy = std::max(0, dy);
        dy = std::clamp(dy, -bb.h / 2, bb.h / 3);
        for (int part : {g.bodyPart, g.shoulderPart, g.pelvisPart})
            if (part >= 0) pose[uz(part)].dy = dy;
        if (!g.front && g.bodyPart >= 0) pose[uz(g.bodyPart)].colDy = backColumns(rig.parts[uz(g.bodyPart)], g, endDy);
        const int maxShift = std::max(1, bb.h / 24);
        for (std::size_t i = 0; i < nLegs; ++i) {
            const auto& L = g.legs[i];
            const FootT& t = ft[i];
            auto& thigh = pose[uz(L.thigh)];
            auto& shin = pose[uz(L.shin)];
            auto& foot = pose[uz(L.foot)];
            const int lift = std::max(0, iround(L.ankle.y - t.y));
            const int liftNow = t.planted ? 0 : lift;
            if (g.front) {
                const double thighRows = std::max(1.0, L.knee.y - L.hip.y);
                const int lf = std::min(liftNow, std::max(0, static_cast<int>(thighRows) - 1 - dy));
                const int sideDx = std::clamp(iround(t.x - L.ankle.x), -maxShift, maxShift);
                thigh.dy = dy;
                thigh.sy = std::max(0.35, (thighRows - dy - lf) / thighRows);
                shin.dy = foot.dy = -lf;
                shin.dx = foot.dx = sideDx;
                continue;
            }
            const rg::Vec hip{L.hip.x, L.hip.y + dy + legEndDy(L)};
            const rg::Vec ankle{t.x, t.planted ? L.ankle.y : t.y};
            const int bend = (L.hind ? -g.dir : g.dir) > 0 ? -1 : 1;
            const rg::Vec knee = rg::solveTwoBone(hip, ankle, L.l1, L.l2, bend);
            const double shinA = std::atan2(ankle.y - knee.y, ankle.x - knee.x);
            const rg::Vec reached{knee.x + L.l2 * std::cos(shinA), knee.y + L.l2 * std::sin(shinA)};
            thigh.dx = hip.x - L.hip.x;
            thigh.dy = hip.y - L.hip.y;
            thigh.angle = std::atan2(knee.y - hip.y, knee.x - hip.x) - std::atan2(L.knee.y - L.hip.y, L.knee.x - L.hip.x);
            shin.dx = knee.x - L.knee.x;
            shin.dy = knee.y - L.knee.y;
            shin.angle = shinA - std::atan2(L.ankle.y - L.knee.y, L.ankle.x - L.knee.x);
            foot.dx = reached.x - L.ankle.x;
            foot.dy = t.planted ? 0 : std::min(0.0, reached.y - L.ankle.y);
            thigh.maxY = shin.maxY = foot.maxY = L.ground;
        }
        if (!opt.inPlace) {
            const int rootDx = iround(kMean * (dot(travel[uz(f)], basisFor(cam, cm.forward).sx) / height));
            for (auto& q : pose) q.dx += rootDx;
        }
        imgs.push_back(rg::renderPose(rig, pose, big));
        poses.push_back(std::move(pose));
    }

    int x0 = big.w, y0 = big.h, x1 = -1, y1 = -1;
    for (const Image& im : imgs) {
        const IntRect r = im.opaqueBounds();
        if (r.empty()) continue;
        x0 = std::min(x0, r.x);
        y0 = std::min(y0, r.y);
        x1 = std::max(x1, r.x + r.w);
        y1 = std::max(y1, r.y + r.h);
    }
    if (x1 < 0) return Error(ErrorCode::Internal, "retarget produced empty frames");
    const int frameMs = std::max(10, iround(1000.0 * step));
    int groundLo = g.legs[0].ground, groundHi = g.legs[0].ground;
    for (const auto& L : g.legs) {
        groundLo = std::min(groundLo, L.ground);
        groundHi = std::max(groundHi, L.ground);
    }
    res.clip.name = "retarget";
    res.clip.loops = opt.loop;
    res.clip.body = motion::Body::Quadruped;
    res.clip.footStagger = groundHi - groundLo;
    res.clip.depthPops = static_cast<int>(rg::zPops(rig, poses, big).size());
    for (const Image& im : imgs) {
        res.clip.frames.push_back(im.copy({x0, y0, x1 - x0, y1 - y0}));
        res.clip.durations.push_back(frameMs);
    }
    const int fw = x1 - x0, fh = y1 - y0;
    const int side = std::max(fw, fh);
    res.source.name = "source";
    res.source.loops = opt.loop;
    const double groundY = (groundHi + 1) - (big.y + y0);
    for (const Projected& pr : prs) {
        Image img(side, fh);
        paintSkeleton(img, m, res.map, pr, std::min(static_cast<double>(bb.h), groundY - 1), side / 2.0, groundY);
        res.source.frames.push_back(img);
        res.source.durations.push_back(frameMs);
    }
    if (footFrames > 0 && planted == 0) res.notes.push_back("No foot of the motion touches its floor: nothing is held on the ground.");
    if (nLegs < 4) res.notes.push_back("Only " + std::to_string(nLegs) + " legs are drawn apart; they follow the nearer source legs.");
    for (const std::string& n : cm.notes) res.notes.push_back("Legs: " + n);
    return res;
}

} // namespace

Result<RetargetResult> retarget(const Image& sprite, const Motion& m, const RetargetOptions& opt) {
    if (sprite.isNull() || sprite.isFullyTransparent()) return Error(ErrorCode::InvalidArgument, "the sprite has no visible pixels");
    if (m.frameCount < 1 || m.joints.empty()) return Error(ErrorCode::InvalidArgument, "the motion is empty");
    if (opt.body == motion::Body::Quadruped) return retargetQuadruped(sprite, m, opt);
    if (opt.body == motion::Body::Flyer) return retargetFlyer(sprite, m, opt);
    if (opt.body == motion::Body::Swimmer) return retargetSwimmer(sprite, m, opt);
    if (opt.body != motion::Body::Biped)
        return Error(ErrorCode::InvalidArgument,
                     "motion can be put on two-legged (biped), four-legged (quadruped), winged (flyer) and fish-like (swimmer) sprites only, for now");
    RetargetResult res;
    res.map = autoMap(m, opt.mapOverrides);
    if (!res.map.hasLegs()) {
        return Error(ErrorCode::InvalidArgument,
                     "the motion's skeleton has no recognisable hips and two legs (map them with Bone=joint overrides)");
    }
    const motion::Subject subj = motion::analyzeSubject(sprite);
    auto built = motion::detail::buildGaitRig(sprite, subj, motion::Body::Biped);
    if (!built || built->legs.size() != 2)
        return Error(ErrorCode::InvalidArgument, "no two legs were found in the sprite (it needs a standing figure with its feet apart)");
    motion::detail::GaitRig& g = *built;
    const rg::Rig& rig = g.rig;
    const IntRect bb = subj.bounds;
    const BoneMap& map = res.map;
    const double amount = std::clamp(opt.amount, 0.25, 2.0);

    // Camera: chosen from the sprite unless given.
    const bool turnKnown = opt.drawnTurn && std::isfinite(*opt.drawnTurn);
    const double drawnTurn = turnKnown ? *opt.drawnTurn : (g.front ? 0 : (g.dir < 0 ? -90 : 90));
    res.camera = opt.camera ? *opt.camera : Camera{};
    if (!opt.camera) res.camera.turn = drawnTurn;
    const Camera& cam = res.camera;
    {
        double diff = std::abs(wrapAngle((cam.turn - drawnTurn) * kDeg)) / kDeg;
        // A front view read from the art may as well be a back view.
        if (!turnKnown && g.front) diff = std::min(diff, std::abs(wrapAngle((cam.turn - 180) * kDeg)) / kDeg);
        if (diff > 30 || std::abs(cam.tilt) > 40) {
            res.approximateView = true;
            const std::string drawn = turnKnown ? "at turn " + std::to_string(iround(drawnTurn)) + " degrees"
                                                : (g.front ? "from the front" : "from the side");
            res.notes.push_back("The sprite is drawn " + drawn +
                                "; this camera only changes how the motion is seen, the art is not turned. "
                                "A view this far from the drawing needs art drawn for it (a reference view).");
        }
    }

    // Time span and frames.
    const double start = std::clamp(opt.start, 0.0, m.duration());
    const double span = opt.length > 0 ? std::min(opt.length, m.duration() - start) : m.duration() - start;
    if (span <= 0) return Error(ErrorCode::InvalidArgument, "the time span is empty");
    const int fps = std::clamp(opt.fps, 1, 60);
    const int frames = std::clamp(opt.frames > 0 ? opt.frames : std::max(2, iround(span * fps)), 2, 240);
    const double step = opt.loop ? span / frames : (frames > 1 ? span / (frames - 1) : 0);

    // The character: rest proportions, forward, floor.
    const std::vector<Vec3> rest = restPositions(m);
    auto at = [&](const std::vector<Vec3>& p, Bone b) { return p[uz(map[b])]; };
    const double legL = len(at(rest, Bone::LShin) - at(rest, Bone::LThigh)) + len(at(rest, Bone::LFoot) - at(rest, Bone::LShin));
    const double legR = len(at(rest, Bone::RShin) - at(rest, Bone::RThigh)) + len(at(rest, Bone::RFoot) - at(rest, Bone::RShin));
    const double leg3 = std::max(1e-6, (legL + legR) / 2);
    double restMinY = 0, restMaxY = 0;
    for (const Vec3& p : rest) { restMinY = std::min(restMinY, p.y); restMaxY = std::max(restMaxY, p.y); }
    const double height = std::max(leg3, restMaxY - restMinY);
    const double standHip = at(rest, Bone::Hips).y - std::min(at(rest, Bone::LFoot).y, at(rest, Bone::RFoot).y);

    std::vector<std::vector<Vec3>> world;
    const std::vector<Vec3> first = jointPositions(m, start);
    const Vec3 forward = forwardAt(m, map, first);
    double floorY = 1e300;
    for (int f = 0; f < frames; ++f) {
        std::vector<Vec3> w = jointPositions(m, start + f * step);
        floorY = std::min({floorY, at(w, Bone::LFoot).y, at(w, Bone::RFoot).y});
        world.push_back(std::move(w));
    }
    const Vec3 hips0 = at(first, Bone::Hips);
    const Vec3 centre{hips0.x, floorY, hips0.z};
    // Root travel (removed for an in-place cycle).
    std::vector<Vec3> travel(uz(frames));
    for (int f = 0; f < frames; ++f) travel[uz(f)] = horizontal(at(world[uz(f)], Bone::Hips) - hips0);

    // Which source leg drives which drawn leg, and which arm which arm.
    const Projected restPr = project(first, cam, forward, height, centre);
    auto px = [&](const Projected& p, Bone b) { return p.x[uz(map[b])]; };
    auto py = [&](const Projected& p, Bone b) { return p.y[uz(map[b])]; };
    auto pd = [&](const Projected& p, Bone b) { return p.depth[uz(map[b])]; };
    const double sepX = std::abs(px(restPr, Bone::LThigh) - px(restPr, Bone::RThigh));
    const double sepD = std::abs(pd(restPr, Bone::LThigh) - pd(restPr, Bone::RThigh));
    const bool byScreen = sepX >= sepD;
    std::array<int, 2> legSide{};   // 0 = source left, 1 = right, per drawn leg
    {
        const bool leftIsNear = pd(restPr, Bone::LThigh) >= pd(restPr, Bone::RThigh);
        const bool leftIsScreenLeft = px(restPr, Bone::LThigh) <= px(restPr, Bone::RThigh);
        const bool leg0ScreenLeft = g.legs[0].hip.x <= g.legs[1].hip.x;
        for (int i = 0; i < 2; ++i) {
            const auto& L = g.legs[uz(i)];
            bool wantLeft;
            if (byScreen) wantLeft = ((i == 0) == leg0ScreenLeft) == leftIsScreenLeft;
            else wantLeft = L.nearLeg == leftIsNear;
            legSide[uz(i)] = wantLeft ? 0 : 1;
        }
        if (legSide[0] == legSide[1]) legSide[1] = 1 - legSide[0];
    }
    const Bone thighB[2] = {Bone::LThigh, Bone::RThigh}, shinB[2] = {Bone::LShin, Bone::RShin}, footB[2] = {Bone::LFoot, Bone::RFoot},
               toeB[2] = {Bone::LToe, Bone::RToe};
    const Bone upperB[2] = {Bone::LUpperArm, Bone::RUpperArm}, handB[2] = {Bone::LHand, Bone::RHand};
    std::vector<int> armSide(g.armParts.size(), -1);
    {
        const bool armsMapped = map[Bone::LUpperArm] >= 0 && map[Bone::LHand] >= 0 && map[Bone::RUpperArm] >= 0 && map[Bone::RHand] >= 0;
        if (armsMapped && !g.armParts.empty()) {
            const bool leftNear = pd(restPr, Bone::LUpperArm) >= pd(restPr, Bone::RUpperArm);
            const bool leftScreenLeft = px(restPr, Bone::LUpperArm) <= px(restPr, Bone::RUpperArm);
            std::vector<std::size_t> idx(g.armParts.size());
            for (std::size_t a = 0; a < idx.size(); ++a) idx[a] = a;
            if (byScreen) {
                std::sort(idx.begin(), idx.end(), [&](std::size_t a, std::size_t b) { return subj.arms[a].x < subj.arms[b].x; });
                armSide[idx[0]] = leftScreenLeft ? 0 : 1;
                if (idx.size() > 1) armSide[idx.back()] = 1 - armSide[idx[0]];
            } else {
                std::sort(idx.begin(), idx.end(), [&](std::size_t a, std::size_t b) {
                    return rg::count(rig.parts[uz(g.armParts[a])].own) > rg::count(rig.parts[uz(g.armParts[b])].own);
                });
                armSide[idx[0]] = leftNear ? 0 : 1;
                if (idx.size() > 1) armSide[idx[1]] = 1 - armSide[idx[0]];
            }
        } else if (!g.armParts.empty()) {
            res.notes.push_back("The motion's arms were not found; the sprite's arms keep still.");
        }
    }
    // Each drawn arm's rest direction: from its pivot to its furthest pixel.
    std::vector<double> armRestAngle(g.armParts.size(), kPi / 2);
    for (std::size_t a = 0; a < g.armParts.size(); ++a) {
        const auto& part = rig.parts[uz(g.armParts[a])];
        double best = -1;
        for (int y = 0; y < part.own.height(); ++y)
            for (int x = 0; x < part.own.width(); ++x)
                if (part.own.get(x, y)) {
                    const double dx = x + 0.5 - part.pivot.x, dy = y + 0.5 - part.pivot.y;
                    const double d2 = dx * dx + dy * dy;
                    if (d2 > best) { best = d2; armRestAngle[a] = std::atan2(dy, dx); }
                }
    }

    // Pixels per screen unit: the drawn legs' length over the source's.
    double legPx = 0;
    for (const auto& L : g.legs) legPx += g.stubby ? std::max(1.0, L.ankle.y - L.hip.y) : L.l1 + L.l2;
    legPx /= 2;
    const double k = legPx / (leg3 / height);
    const double tiltCos = std::cos(std::clamp(cam.tilt, -90.0, 90.0) * kDeg);
    const int maxShift = std::max(1, bb.h / 24);
    if (legPx < 6) res.notes.push_back("The sprite's legs are only a few pixels long; steps will be small.");

    // Rest offsets (feet from thighs) of the source, seen by this camera.
    std::array<double, 2> restAx{}, restAy{};
    for (int i = 0; i < 2; ++i) {
        const int s = legSide[uz(i)];
        restAx[uz(i)] = k * (px(restPr, footB[s]) - px(restPr, thighB[s]));
        restAy[uz(i)] = k * (py(restPr, footB[s]) - py(restPr, thighB[s]));
    }
    (void)restAy;

    const int travelPx = opt.inPlace ? 0 : iround(k * span * 4);
    const int margin = iround(legPx) + bb.h / 2 + travelPx + 8;
    const IntRect big{bb.x - margin, bb.y - margin, bb.w + 2 * margin, bb.h + 2 * margin};
    std::vector<Image> imgs;
    std::vector<std::vector<rg::PartPose>> poses;
    std::vector<Projected> projs;
    std::vector<double> armPrev(g.armParts.size(), 0.0);
    int planted = 0, footFrames = 0;
    for (int f = 0; f < frames; ++f) {
        std::vector<Vec3> w = world[uz(f)];
        if (opt.inPlace)
            for (Vec3& q : w) q = q - travel[uz(f)];
        const Projected pr = project(w, cam, forward, height, centre);
        projs.push_back(pr);
        std::vector<rg::PartPose> pose = rig.rest();

        // Hips height: the source's drop below its standing height, scaled.
        const double hipAbove = at(world[uz(f)], Bone::Hips).y - floorY;
        int dy = iround(k * tiltCos * (standHip - hipAbove) / height * amount);
        // Feet: targets from the thighs; contact from the source.
        struct FootT {
            double x = 0, y = 0;
            bool planted = false;
            int bend = 0;
            double toeAngle = 0;
        };
        std::array<FootT, 2> ft{};
        int pelvisDx = 0, shoulderDx = 0;
        if (g.twist) {
            const int nearIdx = g.legs[0].nearLeg ? 0 : 1;
            const int sn = legSide[uz(nearIdx)], sf = 1 - sn;
            pelvisDx = std::clamp(iround(0.25 * k * (px(pr, thighB[sn]) - px(pr, thighB[sf])) * amount), -maxShift, maxShift);
            if (map[upperB[0]] >= 0 && map[upperB[1]] >= 0)
                shoulderDx = std::clamp(iround(0.25 * k * (px(pr, upperB[sn]) - px(pr, upperB[sf])) * amount), -maxShift, maxShift);
        }
        for (int i = 0; i < 2; ++i) {
            const auto& L = g.legs[uz(i)];
            const int s = legSide[uz(i)];
            double ax = k * (px(pr, footB[s]) - px(pr, thighB[s]));
            const double ay = k * (py(pr, footB[s]) - py(pr, thighB[s]));
            ax = restAx[uz(i)] + (ax - restAx[uz(i)]) * amount;
            FootT& t = ft[uz(i)];
            // Short and front-view legs move from where their feet are drawn;
            // jointed side-view legs reach from the hip.
            t.x = (g.stubby || g.front) ? L.ankle.x + (ax - restAx[uz(i)]) : L.hip.x + pelvisDx + ax;
            // Lift: the source foot's height over its floor, on this leg's own
            // ground row (three-quarter views stand the far foot higher).
            const double footUp = at(world[uz(f)], footB[s]).y - floorY;
            t.y = L.ankle.y - std::max(0.0, k * tiltCos * footUp / height * amount);
            (void)ay;
            // Planted: near the floor and barely moving in the world.
            const Vec3 fw = at(world[uz(f)], footB[s]);
            const std::vector<Vec3> w2 = jointPositions(m, start + f * step + m.frameTime);
            const double speed = len(horizontal(at(w2, footB[s]) - fw)) / m.frameTime;
            t.planted = fw.y - floorY < 0.06 * leg3 && speed < 1.2 * leg3;
            // The knee bends to the side the source's knee is on.
            const double vx = px(pr, footB[s]) - px(pr, thighB[s]), vy = py(pr, footB[s]) - py(pr, thighB[s]);
            const double kx = px(pr, shinB[s]) - px(pr, thighB[s]), ky = py(pr, shinB[s]) - py(pr, thighB[s]);
            const double c = vx * ky - vy * kx;
            t.bend = std::abs(c) > 1e-4 ? (c < 0 ? -1 : 1) : (g.dir > 0 ? -1 : 1);
            if (map[toeB[s]] >= 0) {
                const double a1 = std::atan2(py(pr, toeB[s]) - py(pr, footB[s]), px(pr, toeB[s]) - px(pr, footB[s]));
                const double a0 = std::atan2(py(restPr, toeB[s]) - py(restPr, footB[s]), px(restPr, toeB[s]) - px(restPr, footB[s]));
                const double tl = std::hypot(px(pr, toeB[s]) - px(pr, footB[s]), py(pr, toeB[s]) - py(pr, footB[s]));
                if (tl > 0.02) t.toeAngle = std::clamp(wrapAngle(a1 - a0), -0.6, 0.6);
            }
            ++footFrames;
            planted += t.planted ? 1 : 0;
        }
        // Weight passes from foot to foot: unless the source is in the air,
        // the lower foot carries it (a foot can be planted while it rolls).
        if (!ft[0].planted && !ft[1].planted) {
            const double h0 = at(world[uz(f)], footB[legSide[0]]).y - floorY, h1 = at(world[uz(f)], footB[legSide[1]]).y - floorY;
            const int low = h0 <= h1 ? 0 : 1;
            if (std::min(h0, h1) < 0.15 * leg3) {
                ft[uz(low)].planted = true;
                ++planted;
            }
        }
        // Low enough for every planted foot to reach the ground.
        if (!g.front && !g.stubby)
            for (int i = 0; i < 2; ++i) {
                if (!ft[uz(i)].planted) continue;
                const auto& L = g.legs[uz(i)];
                const double reach = 0.97 * (L.l1 + L.l2);
                const double dx = ft[uz(i)].x - (L.hip.x + pelvisDx);
                const double vertical = std::sqrt(std::max(0.0, reach * reach - dx * dx));
                dy = std::max(dy, static_cast<int>(std::ceil((L.ankle.y - L.hip.y) - vertical - 1e-9)));
            }
        if (g.front) dy = std::max(0, dy);
        dy = std::clamp(dy, -bb.h / 2, bb.h / 3);

        // The body leans as the spine does.
        double lean = 0;
        const Bone spineTop = map[Bone::Neck] >= 0 ? Bone::Neck : map[Bone::Chest] >= 0 ? Bone::Chest : Bone::Head;
        if (map[spineTop] >= 0) {
            auto leanOf = [&](const Projected& p) {
                return std::atan2(px(p, spineTop) - px(p, Bone::Hips), -(py(p, spineTop) - py(p, Bone::Hips)));
            };
            lean = std::clamp(wrapAngle(leanOf(pr) - leanOf(restPr)) * amount, -0.4, 0.4);
            if (std::abs(lean) < 0.03) lean = 0;
        }
        for (int part : {g.bodyPart, g.shoulderPart, g.pelvisPart}) {
            if (part < 0) continue;
            auto& q = pose[uz(part)];
            q.dy = dy;
            q.angle = lean;
        }
        if (g.shoulderPart >= 0) pose[uz(g.shoulderPart)].dx = shoulderDx;
        if (g.pelvisPart >= 0) pose[uz(g.pelvisPart)].dx = pelvisDx;

        // Legs.
        for (int i = 0; i < 2; ++i) {
            const auto& L = g.legs[uz(i)];
            const FootT& t = ft[uz(i)];
            auto& thigh = pose[uz(L.thigh)];
            auto& shin = pose[uz(L.shin)];
            auto& foot = pose[uz(L.foot)];
            const int lift = std::max(0, iround(L.ankle.y - t.y));
            const int liftNow = t.planted ? 0 : lift;
            if (g.front) {
                const double thighRows = std::max(1.0, L.knee.y - L.hip.y);
                const int lf = std::min(liftNow, std::max(0, static_cast<int>(thighRows) - 1 - dy));
                const int sideDx = std::clamp(iround(t.x - L.ankle.x), -maxShift, maxShift);
                thigh.dy = dy;
                thigh.sy = std::max(0.35, (thighRows - dy - lf) / thighRows);
                shin.dy = foot.dy = -lf;
                shin.dx = foot.dx = sideDx;
                continue;
            }
            if (g.stubby) {
                const double rows = std::max(1.0, L.ankle.y - L.hip.y);
                const double fx = t.x - L.ankle.x;
                // The piece above the boot stays on the hip and leans over
                // to the boot, so its lower edge goes where the boot goes.
                foot.dx = fx;
                foot.dy = -liftNow;
                thigh.dy = dy;
                thigh.sy = std::clamp((rows - liftNow - dy) / rows, 0.35, 2.0);
                thigh.shear = fx / (rows * thigh.sy);
                thigh.maxY = L.ground;
                continue;
            }
            const rg::Vec hip{L.hip.x + pelvisDx, L.hip.y + dy};
            const rg::Vec ankle{t.x, t.planted ? L.ankle.y : t.y};
            const rg::Vec knee = rg::solveTwoBone(hip, ankle, L.l1, L.l2, t.bend);
            const double shinA = std::atan2(ankle.y - knee.y, ankle.x - knee.x);
            const rg::Vec reached{knee.x + L.l2 * std::cos(shinA), knee.y + L.l2 * std::sin(shinA)};
            thigh.dx = hip.x - L.hip.x;
            thigh.dy = hip.y - L.hip.y;
            thigh.angle = std::atan2(knee.y - hip.y, knee.x - hip.x) - std::atan2(L.knee.y - L.hip.y, L.knee.x - L.hip.x);
            shin.dx = knee.x - L.knee.x;
            shin.dy = knee.y - L.knee.y;
            shin.angle = shinA - std::atan2(L.ankle.y - L.knee.y, L.ankle.x - L.knee.x);
            foot.dx = reached.x - L.ankle.x;
            foot.dy = t.planted ? 0 : std::min(0.0, reached.y - L.ankle.y);
            if (!t.planted) foot.angle = t.toeAngle;
            thigh.maxY = shin.maxY = foot.maxY = L.ground;
        }

        // Arms: pivot carried by the leaning body, pointing where the source arm points.
        for (std::size_t a = 0; a < g.armParts.size(); ++a) {
            const int part = g.armParts[a];
            auto& q = pose[uz(part)];
            const auto& pv = rig.parts[uz(part)].pivot;
            const rg::Vec moved =
                rg::transformPoint(rig, g.bodyPart, pose[uz(g.bodyPart)], {pv.x, pv.y});
            q.dx = moved.x - pv.x + shoulderDx;
            q.dy = moved.y - pv.y;
            const int s = armSide[a];
            if (s < 0) { q.angle = lean; continue; }
            const double vx = px(pr, handB[s]) - px(pr, upperB[s]), vy = py(pr, handB[s]) - py(pr, upperB[s]);
            const Vec3 armRest = at(rest, handB[s]) - at(rest, upperB[s]);
            const double full = len(armRest) / height;
            double angle = armPrev[a];
            if (std::hypot(vx, vy) > 0.3 * full) angle = std::clamp(wrapAngle(std::atan2(vy, vx) - armRestAngle[a]) * amount, -2.4, 2.4);
            armPrev[a] = angle;
            q.angle = angle;
        }

        if (!opt.inPlace) {
            const int rootDx = iround(k * (dot(travel[uz(f)], basisFor(cam, forward).sx) / height));
            for (auto& q : pose) q.dx += rootDx;
        }
        imgs.push_back(rg::renderPose(rig, pose, big));
        poses.push_back(std::move(pose));
    }

    // One crop for every frame.
    int x0 = big.w, y0 = big.h, x1 = -1, y1 = -1;
    for (const Image& im : imgs) {
        const IntRect r = im.opaqueBounds();
        if (r.empty()) continue;
        x0 = std::min(x0, r.x);
        y0 = std::min(y0, r.y);
        x1 = std::max(x1, r.x + r.w);
        y1 = std::max(y1, r.y + r.h);
    }
    if (x1 < 0) return Error(ErrorCode::Internal, "retarget produced empty frames");
    const int frameMs = std::max(10, iround(1000.0 * step));
    res.clip.name = "retarget";
    res.clip.loops = opt.loop;
    res.clip.body = motion::Body::Biped;
    res.clip.footStagger = std::abs(g.legs[0].ground - g.legs[1].ground);
    res.clip.depthPops = static_cast<int>(rg::zPops(rig, poses, big).size());
    for (const Image& im : imgs) {
        res.clip.frames.push_back(im.copy({x0, y0, x1 - x0, y1 - y0}));
        res.clip.durations.push_back(frameMs);
    }
    // The source, the same size and frames, the floor on the sprite's ground.
    const int fw = x1 - x0, fh = y1 - y0;
    const int side = std::max(fw, fh);
    res.source.name = "source";
    res.source.loops = opt.loop;
    const double groundY = (std::max(g.legs[0].ground, g.legs[1].ground) + 1) - (big.y + y0);
    for (const Projected& pr : projs) {
        Image img(side, fh);
        // As tall as the sprite (its proportions may differ: a big-headed
        // sprite has short legs).
        const double previewScale = std::min(static_cast<double>(bb.h), groundY - 1);
        paintSkeleton(img, m, map, pr, previewScale, side / 2.0, groundY);
        res.source.frames.push_back(img);
        res.source.durations.push_back(frameMs);
    }
    if (footFrames > 0 && planted == 0) res.notes.push_back("No foot of the motion touches its floor: nothing is held on the ground.");
    for (const std::string& n : map.notes)
        if (n.find("not found") != std::string::npos) res.notes.push_back("Bone " + n);
    return res;
}

} // namespace acestudio::mocap
