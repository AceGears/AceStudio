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

Result<RetargetResult> retarget(const Image& sprite, const Motion& m, const RetargetOptions& opt) {
    if (sprite.isNull() || sprite.isFullyTransparent()) return Error(ErrorCode::InvalidArgument, "the sprite has no visible pixels");
    if (m.frameCount < 1 || m.joints.empty()) return Error(ErrorCode::InvalidArgument, "the motion is empty");
    if (opt.body != motion::Body::Biped)
        return Error(ErrorCode::InvalidArgument, "humanoid motion can be put on a two-legged (biped) sprite only, for now");
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
    const double drawnTurn = g.front ? 0 : (g.dir < 0 ? -90 : 90);
    res.camera = opt.camera ? *opt.camera : Camera{};
    if (!opt.camera) res.camera.turn = drawnTurn;
    const Camera& cam = res.camera;
    {
        double diff = std::abs(wrapAngle((cam.turn - drawnTurn) * kDeg)) / kDeg;
        if (g.front) diff = std::min(diff, std::abs(wrapAngle((cam.turn - 180) * kDeg)) / kDeg);
        if (diff > 30 || std::abs(cam.tilt) > 40) {
            res.approximateView = true;
            res.notes.push_back(std::string("The sprite is drawn ") + (g.front ? "from the front" : "from the side") +
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
