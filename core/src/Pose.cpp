#include "acestudio/core/Pose.h"

#include <algorithm>
#include <cmath>

namespace acestudio {

namespace {
constexpr double kPi = 3.14159265358979323846;
constexpr const char* kJointNames[kJointCount] = {
    "nose", "neck", "rShoulder", "rElbow", "rWrist", "lShoulder", "lElbow", "lWrist", "rHip",
    "rKnee", "rAnkle", "lHip", "lKnee", "lAnkle", "rEye", "lEye", "rEar", "lEar"};

// The 18 colours of the conditioning convention, by joint index; limb i uses
// colour i.
constexpr std::array<Rgba8, kJointCount> kColors{{
    {255, 0, 0, 255},   {255, 85, 0, 255},  {255, 170, 0, 255}, {255, 255, 0, 255}, {170, 255, 0, 255},
    {85, 255, 0, 255},  {0, 255, 0, 255},   {0, 255, 85, 255},  {0, 255, 170, 255}, {0, 255, 255, 255},
    {0, 170, 255, 255}, {0, 85, 255, 255},  {0, 0, 255, 255},   {85, 0, 255, 255},  {170, 0, 255, 255},
    {255, 0, 255, 255}, {255, 0, 170, 255}, {255, 0, 85, 255}}};

std::uint8_t lerp8(std::uint8_t a, std::uint8_t b, double t) {
    return static_cast<std::uint8_t>(std::clamp(std::lround(a + (b - a) * t), 0L, 255L));
}

// Distance from p to segment ab.
double segDist(double px, double py, double ax, double ay, double bx, double by) {
    const double vx = bx - ax, vy = by - ay, len2 = vx * vx + vy * vy;
    double t = len2 > 0 ? ((px - ax) * vx + (py - ay) * vy) / len2 : 0.0;
    t = std::clamp(t, 0.0, 1.0);
    const double dx = px - (ax + t * vx), dy = py - (ay + t * vy);
    return std::sqrt(dx * dx + dy * dy);
}

struct Pt { double x, y; };
Pt add(Pt p, double len, double angle) {   // angle from straight down, + towards +x
    return {p.x + len * std::sin(angle), p.y + len * std::cos(angle)};
}
double rad(double deg) { return deg * kPi / 180.0; }

// Builds a side-view figure (facing +x) from a few parameters.
struct Rig {
    double bob = 0, lean = 0, liftY = 0;       // vertical offset of the body, forward lean, extra lift
    double legR = 0, legL = 0;                 // thigh angles (rad, + forward)
    double kneeR = 0, kneeL = 0;               // knee flexion (rad)
    double armR = 0, armL = 0;                 // upper-arm angles
    double elbowR = rad(15), elbowL = rad(15); // forearm bend (forward)
    bool ground = true;                        // put the lowest ankle on the ground line
};

Pose build(const Rig& r) {
    constexpr double kThigh = 0.18, kShin = 0.19, kUpperArm = 0.13, kForearm = 0.12, kGround = 0.9;
    const Pt hip{0.5, 0.53 + r.bob - r.liftY};
    const Pt neck{0.5 + r.lean, 0.26 + r.bob - r.liftY};
    Pose p;
    auto set = [&](Joint j, Pt q, bool vis = true) { p[j] = {q.x, q.y, vis}; };
    set(Joint::Neck, neck);
    set(Joint::Nose, {neck.x + 0.035, neck.y - 0.09});
    set(Joint::REye, {neck.x + 0.02, neck.y - 0.105});
    set(Joint::LEye, {neck.x + 0.045, neck.y - 0.105});
    set(Joint::REar, {neck.x - 0.01, neck.y - 0.095});
    set(Joint::LEar, {neck.x + 0.06, neck.y - 0.095}, false);   // hidden behind the head in side view
    const Pt rs{neck.x - 0.04, neck.y + 0.01}, ls{neck.x + 0.04, neck.y + 0.01};
    set(Joint::RShoulder, rs);
    set(Joint::LShoulder, ls);
    const Pt re = add(rs, kUpperArm, r.armR), le = add(ls, kUpperArm, r.armL);
    set(Joint::RElbow, re);
    set(Joint::LElbow, le);
    set(Joint::RWrist, add(re, kForearm, r.armR + r.elbowR));
    set(Joint::LWrist, add(le, kForearm, r.armL + r.elbowL));
    const Pt rh{hip.x - 0.02, hip.y}, lh{hip.x + 0.02, hip.y};
    set(Joint::RHip, rh);
    set(Joint::LHip, lh);
    const Pt rk = add(rh, kThigh, r.legR), lk = add(lh, kThigh, r.legL);
    set(Joint::RKnee, rk);
    set(Joint::LKnee, lk);
    set(Joint::RAnkle, add(rk, kShin, r.legR - r.kneeR));
    set(Joint::LAnkle, add(lk, kShin, r.legL - r.kneeL));
    if (r.ground) {
        const double dy = kGround - p.groundY();
        for (auto& k : p.points) k.y += dy;
    }
    return p;
}
} // namespace

std::string_view toString(Joint j) noexcept { return kJointNames[static_cast<int>(j)]; }

const std::vector<std::pair<Joint, Joint>>& poseLimbs() {
    static const std::vector<std::pair<Joint, Joint>> limbs = {
        {Joint::Neck, Joint::RShoulder}, {Joint::Neck, Joint::LShoulder},   {Joint::RShoulder, Joint::RElbow},
        {Joint::RElbow, Joint::RWrist},  {Joint::LShoulder, Joint::LElbow}, {Joint::LElbow, Joint::LWrist},
        {Joint::Neck, Joint::RHip},      {Joint::RHip, Joint::RKnee},       {Joint::RKnee, Joint::RAnkle},
        {Joint::Neck, Joint::LHip},      {Joint::LHip, Joint::LKnee},       {Joint::LKnee, Joint::LAnkle},
        {Joint::Neck, Joint::Nose},      {Joint::Nose, Joint::REye},        {Joint::REye, Joint::REar},
        {Joint::Nose, Joint::LEye},      {Joint::LEye, Joint::LEar}};
    return limbs;
}

Rgba8 limbColor(std::size_t i) noexcept { return kColors[i % kColors.size()]; }
Rgba8 jointColor(Joint j) noexcept { return kColors[static_cast<std::size_t>(j)]; }

Pose Pose::mirrored() const {
    Pose m = *this;
    auto swap = [&](Joint a, Joint b) { std::swap(m[a], m[b]); };
    swap(Joint::RShoulder, Joint::LShoulder); swap(Joint::RElbow, Joint::LElbow); swap(Joint::RWrist, Joint::LWrist);
    swap(Joint::RHip, Joint::LHip); swap(Joint::RKnee, Joint::LKnee); swap(Joint::RAnkle, Joint::LAnkle);
    swap(Joint::REye, Joint::LEye); swap(Joint::REar, Joint::LEar);
    for (auto& k : m.points) k.x = 1.0 - k.x;
    return m;
}

double Pose::groundY() const {
    double g = 0;
    for (Joint j : {Joint::RAnkle, Joint::LAnkle})
        if ((*this)[j].visible) g = std::max(g, (*this)[j].y);
    return g;
}

Pose Pose::transformed(double scale, double dx, double dy) const {
    Pose t = *this;
    for (auto& k : t.points) { k.x = k.x * scale + dx; k.y = k.y * scale + dy; }
    return t;
}

Image renderPose(const Pose& pose, int width, int height) {
    Image out(std::max(1, width), std::max(1, height), Rgba8{0, 0, 0, 255});
    const double unit = std::min(width, height) / 512.0;
    const double stick = std::max(1.0, 4.0 * unit), radius = std::max(1.0, 4.0 * unit);
    auto px = [&](const Keypoint& k) { return Pt{k.x * width, k.y * height}; };
    // Joints first, then limbs blended at 60% over them.
    for (int j = 0; j < kJointCount; ++j) {
        const Keypoint& k = pose.points[static_cast<std::size_t>(j)];
        if (!k.visible) continue;
        const Pt c = px(k);
        const Rgba8 col = jointColor(static_cast<Joint>(j));
        for (int y = static_cast<int>(c.y - radius - 1); y <= static_cast<int>(c.y + radius + 1); ++y)
            for (int x = static_cast<int>(c.x - radius - 1); x <= static_cast<int>(c.x + radius + 1); ++x)
                if (out.contains(x, y) && std::hypot(x + 0.5 - c.x, y + 0.5 - c.y) <= radius) out.setPixel(x, y, col);
    }
    const auto& limbs = poseLimbs();
    for (std::size_t i = 0; i < limbs.size(); ++i) {
        const Keypoint &a = pose[limbs[i].first], &b = pose[limbs[i].second];
        if (!a.visible || !b.visible) continue;
        const Pt p = px(a), q = px(b);
        const Rgba8 col = limbColor(i);
        const int x0 = static_cast<int>(std::floor(std::min(p.x, q.x) - stick - 1)), x1 = static_cast<int>(std::ceil(std::max(p.x, q.x) + stick + 1));
        const int y0 = static_cast<int>(std::floor(std::min(p.y, q.y) - stick - 1)), y1 = static_cast<int>(std::ceil(std::max(p.y, q.y) + stick + 1));
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x) {
                if (!out.contains(x, y) || segDist(x + 0.5, y + 0.5, p.x, p.y, q.x, q.y) > stick) continue;
                const Rgba8 d = out.pixel(x, y);
                out.setPixel(x, y, {lerp8(d.r, col.r, 0.6), lerp8(d.g, col.g, 0.6), lerp8(d.b, col.b, 0.6), 255});
            }
    }
    return out;
}

Pose standingPose() { return build(Rig{}); }

std::string_view toString(PoseCycle c) noexcept {
    switch (c) {
    case PoseCycle::Idle: return "idle";
    case PoseCycle::Walk: return "walk";
    case PoseCycle::Run: return "run";
    case PoseCycle::Jump: return "jump";
    }
    return "idle";
}

std::optional<PoseCycle> poseCycleFromString(std::string_view s) noexcept {
    for (auto c : {PoseCycle::Idle, PoseCycle::Walk, PoseCycle::Run, PoseCycle::Jump})
        if (toString(c) == s) return c;
    return std::nullopt;
}

Pose cyclePose(PoseCycle cycle, double t) {
    t = t - std::floor(t);
    const double w = 2 * kPi * t;
    Rig r;
    switch (cycle) {
    case PoseCycle::Idle:
        r.bob = 0.006 * std::sin(w);                 // breathing
        r.armR = rad(4) + rad(2) * std::sin(w);
        r.armL = -rad(4) - rad(2) * std::sin(w);
        break;
    case PoseCycle::Walk: {
        const double a = rad(24), k = rad(38);
        r.legR = a * std::sin(w);
        r.legL = a * std::sin(w + kPi);
        r.kneeR = k * std::max(0.0, std::cos(w));     // the forward-swinging leg bends
        r.kneeL = k * std::max(0.0, std::cos(w + kPi));
        r.armR = -0.6 * a * std::sin(w);              // arms swing against the legs
        r.armL = -0.6 * a * std::sin(w + kPi);
        r.bob = 0.008 * std::cos(2 * w);
        r.lean = 0.01;
        break;
    }
    case PoseCycle::Run: {
        const double a = rad(40), k = rad(75);
        r.legR = a * std::sin(w);
        r.legL = a * std::sin(w + kPi);
        r.kneeR = k * std::max(0.0, std::cos(w)) + rad(10);
        r.kneeL = k * std::max(0.0, std::cos(w + kPi)) + rad(10);
        r.armR = -0.8 * a * std::sin(w);
        r.armL = -0.8 * a * std::sin(w + kPi);
        r.elbowR = r.elbowL = rad(75);
        r.bob = 0.02 * std::cos(2 * w);
        r.lean = 0.05;
        break;
    }
    case PoseCycle::Jump: {
        // crouch (0-0.2), airborne (0.2-0.8), land (0.8-1)
        const double crouch = t < 0.2 ? std::sin(kPi * t / 0.2) : (t > 0.8 ? std::sin(kPi * (t - 0.8) / 0.2) : 0.0);
        const double air = t > 0.2 && t < 0.8 ? std::sin(kPi * (t - 0.2) / 0.6) : 0.0;
        r.legR = rad(25) * crouch + rad(10) * air;
        r.legL = rad(20) * crouch - rad(5) * air;
        r.kneeR = rad(55) * crouch + rad(40) * air;
        r.kneeL = rad(50) * crouch + rad(30) * air;
        r.armR = -rad(30) * crouch + rad(150) * air;   // arms up in the air
        r.armL = -rad(25) * crouch + rad(140) * air;
        r.elbowR = r.elbowL = rad(10);
        r.bob = 0.03 * crouch;
        r.ground = false;
        Pose p = build(r);
        // Feet on the ground line, lifted by the jump height while airborne.
        const double dy = (0.9 - 0.18 * air) - p.groundY();
        for (auto& kp : p.points) kp.y += dy;
        return p;
    }
    }
    return build(r);
}

void fitPoses(std::vector<Pose>& poses, double margin) {
    // The head is drawn above the eyes and ears: reserve room for it.
    double x0 = 1e9, y0 = 1e9, x1 = -1e9, y1 = -1e9;
    for (const Pose& p : poses)
        for (const Keypoint& k : p.points)
            if (k.visible) { x0 = std::min(x0, k.x); y0 = std::min(y0, k.y); x1 = std::max(x1, k.x); y1 = std::max(y1, k.y); }
    if (x1 < x0) return;
    y0 -= 0.06;   // top of the head above the eye/ear keypoints
    const double lo = margin, hi = 1.0 - margin;
    if (x0 >= lo && y0 >= lo && x1 <= hi && y1 <= hi) return;
    // One scale for all frames about the shared bottom-centre, so relative
    // motion (a jump's height) is kept; then shift inside the frame.
    const double s = std::min({1.0, (hi - lo) / std::max(1e-6, x1 - x0), (hi - lo) / std::max(1e-6, y1 - y0)});
    const double cx = (x0 + x1) / 2, by = y1;
    for (Pose& p : poses)
        for (Keypoint& k : p.points) {
            k.x = 0.5 + (k.x - cx) * s;
            k.y = hi + (k.y - by) * s;
        }
}

std::vector<Pose> cyclePoses(PoseCycle cycle, int frames) {
    std::vector<Pose> out;
    frames = std::clamp(frames, 1, 256);
    for (int i = 0; i < frames; ++i) out.push_back(cyclePose(cycle, static_cast<double>(i) / frames));
    fitPoses(out);
    return out;
}

std::array<double, 4> poseFramingBox(const std::vector<Pose>& poses, double aspect, double margin) {
    double x0 = 1, y0 = 1, x1 = 0, y1 = 0;
    for (const Pose& p : poses)
        for (const Keypoint& k : p.points)
            if (k.visible) { x0 = std::min(x0, k.x); y0 = std::min(y0, k.y); x1 = std::max(x1, k.x); y1 = std::max(y1, k.y); }
    if (x1 <= x0 || y1 <= y0) return {0, 0, 1, 1};
    // The head extends above the eyes/ears and the feet below the ankles;
    // SDXL also draws plumes, hair and raised weapons above the skeleton, so
    // leave more room on top.
    const double h = y1 - y0;
    y0 -= 0.16 * h;
    y1 += 0.04 * h;
    const double pad = margin * (y1 - y0);
    x0 -= pad; x1 += pad; y0 -= pad; y1 += pad;
    double w = x1 - x0, hh = y1 - y0;
    aspect = aspect > 0 ? aspect : 1.0;
    const double cx = (x0 + x1) / 2, cy = (y0 + y1) / 2;
    if (w / hh < aspect) w = hh * aspect; else hh = w / aspect;
    if (w > 1) { w = 1; hh = std::min(1.0, w / aspect); }
    if (hh > 1) { hh = 1; w = std::min(1.0, hh * aspect); }
    double bx = std::clamp(cx - w / 2, 0.0, 1.0 - w), by = std::clamp(cy - hh / 2, 0.0, 1.0 - hh);
    return {bx, by, bx + w, by + hh};
}

nlohmann::json poseToJson(const Pose& p) {
    nlohmann::json pts = nlohmann::json::object();
    for (int j = 0; j < kJointCount; ++j) {
        const Keypoint& k = p.points[static_cast<std::size_t>(j)];
        pts[std::string(kJointNames[j])] = {{"x", k.x}, {"y", k.y}, {"visible", k.visible}};
    }
    return {{"keypoints", pts}};
}

Result<Pose> poseFromJson(const nlohmann::json& j) {
    if (!j.is_object() || !j.contains("keypoints") || !j["keypoints"].is_object())
        return Error{ErrorCode::CorruptData, "Pose has no keypoints"};
    Pose p = standingPose();
    for (int i = 0; i < kJointCount; ++i) {
        const auto& kp = j["keypoints"];
        if (!kp.contains(kJointNames[i])) continue;
        const auto& v = kp[kJointNames[i]];
        if (!v.is_object() || !v.contains("x") || !v.contains("y") || !v["x"].is_number() || !v["y"].is_number())
            return Error{ErrorCode::CorruptData, std::string("Invalid keypoint ") + kJointNames[i]};
        const double x = v["x"].get<double>(), y = v["y"].get<double>();
        if (!std::isfinite(x) || !std::isfinite(y) || std::abs(x) > 4 || std::abs(y) > 4)
            return Error{ErrorCode::CorruptData, std::string("Keypoint out of range: ") + kJointNames[i]};
        p.points[static_cast<std::size_t>(i)] = {x, y, v.value("visible", true)};
    }
    return p;
}

} // namespace acestudio
