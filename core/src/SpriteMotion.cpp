#include "acestudio/core/SpriteMotion.h"

#include "PixelRig.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <limits>

namespace acestudio::motion {

namespace {

constexpr double kPi = 3.14159265358979323846;

const std::vector<ActionInfo> kActions = {
    {Action::Idle, "idle", "Idle", 4, 180, true},
    {Action::Walk, "walk", "Walk", 8, 100, true},
    {Action::Run, "run", "Run", 8, 80, true},
    {Action::Sprint, "sprint", "Sprint", 6, 70, true},
    {Action::Jump, "jump", "Jump", 8, 90, false},
    {Action::Fall, "fall", "Fall", 4, 100, true},
    {Action::Land, "land", "Land", 4, 80, false},
    {Action::Attack1, "attack1", "Attack 1 (slash)", 6, 80, false},
    {Action::Attack2, "attack2", "Attack 2 (overhead)", 6, 90, false},
    {Action::Attack3, "attack3", "Attack 3 (thrust)", 6, 80, false},
    {Action::Dodge, "dodge", "Dodge", 5, 80, false},
    {Action::Roll, "roll", "Roll", 8, 60, true},
    {Action::Dash, "dash", "Dash", 5, 70, false},
    {Action::Block, "block", "Block", 4, 90, false},
    {Action::Hurt, "hurt", "Hurt", 4, 90, false},
    {Action::Death, "death", "Death", 8, 110, false},
    {Action::Cast, "cast", "Cast", 6, 100, false},
    {Action::Swim, "swim", "Swim", 6, 120, true},
    {Action::Fly, "fly", "Fly / hover", 6, 110, true},
    {Action::Flap, "flap", "Wing flap", 8, 75, true},
    {Action::Climb, "climb", "Climb", 6, 110, true},
    {Action::Sit, "sit", "Sit", 4, 110, false},
    {Action::Sleep, "sleep", "Sleep", 4, 300, true},
    {Action::Custom, "custom", "Custom", 8, 100, true},
};

// ------------------------------------------------------------- 2-D affine

struct Affine {
    double a = 1, b = 0, c = 0;   // x' = a x + b y + c
    double d = 0, e = 1, f = 0;   // y' = d x + e y + f
    static Affine translate(double x, double y) { return {1, 0, x, 0, 1, y}; }
    static Affine scale(double sx, double sy) { return {sx, 0, 0, 0, sy, 0}; }
    static Affine rotate(double t) { return {std::cos(t), -std::sin(t), 0, std::sin(t), std::cos(t), 0}; }
    static Affine shearX(double k) { return {1, k, 0, 0, 1, 0}; }   // x += k * y
    Affine operator*(const Affine& o) const {   // this ∘ o
        return {a * o.a + b * o.d, a * o.b + b * o.e, a * o.c + b * o.f + c,
                d * o.a + e * o.d, d * o.b + e * o.e, d * o.c + e * o.f + f};
    }
    Affine inverse() const {
        const double det = a * e - b * d;
        if (std::abs(det) < 1e-12) return {};
        const double ia = e / det, ib = -b / det, id = -d / det, ie = a / det;
        return {ia, ib, -(ia * c + ib * f), id, ie, -(id * c + ie * f)};
    }
    std::pair<double, double> apply(double x, double y) const { return {a * x + b * y + c, d * x + e * y + f}; }
};
// The same transform about a pivot.
Affine about(double px, double py, const Affine& m) { return Affine::translate(px, py) * m * Affine::translate(-px, -py); }

// ------------------------------------------------------------- frame poses

enum class Pivot { Feet, Centre, BackFoot };

struct FramePose {
    double dx = 0, dy = 0;          // fractions of the sprite width / height
    double sx = 1, sy = 1;          // scale about the pivot
    double rot = 0;                 // radians, clockwise (forward for a right-facing sprite)
    double lean = 0;                // shear: higher pixels move forward by lean * height above the pivot
    double swingA = 0, swingB = 0;  // leg groups (alternate feet), radians
    double liftA = 0, liftB = 0;    // leg lift, fraction of the height
    double flash = 0;               // > 0.5 draws the frame white
    double wing = 1;                // wing flap: vertical scale of the part above the wing line (negative = folded down)
    double wave = 0;                // swimmer body wave: tail amplitude, fraction of the height
    double wavePhase = 0;           // radians
};

FramePose lerp(const FramePose& p, const FramePose& q, double t) {
    auto L = [t](double x, double y) { return x + (y - x) * t; };
    return {L(p.dx, q.dx), L(p.dy, q.dy), L(p.sx, q.sx), L(p.sy, q.sy), L(p.rot, q.rot), L(p.lean, q.lean),
            L(p.swingA, q.swingA), L(p.swingB, q.swingB), L(p.liftA, q.liftA), L(p.liftB, q.liftB), L(p.flash, q.flash), L(p.wing, q.wing),
            L(p.wave, q.wave), L(p.wavePhase, q.wavePhase)};
}

FramePose keyed(const std::vector<std::pair<double, FramePose>>& keys, double t) {
    if (t <= keys.front().first) return keys.front().second;
    for (std::size_t i = 1; i < keys.size(); ++i)
        if (t <= keys[i].first) {
            const double span = keys[i].first - keys[i - 1].first;
            const double u = span > 0 ? (t - keys[i - 1].first) / span : 1.0;
            // Ease in/out between keys.
            return lerp(keys[i - 1].second, keys[i].second, u * u * (3 - 2 * u));
        }
    return keys.back().second;
}

FramePose fp(std::initializer_list<std::pair<const char*, double>> kv) {
    FramePose p;
    for (const auto& [k, v] : kv) {
        const std::string_view key(k);
        if (key == "dx") p.dx = v;
        else if (key == "dy") p.dy = v;
        else if (key == "sx") p.sx = v;
        else if (key == "sy") p.sy = v;
        else if (key == "rot") p.rot = v;
        else if (key == "lean") p.lean = v;
        else if (key == "swingA") p.swingA = v;
        else if (key == "swingB") p.swingB = v;
        else if (key == "liftA") p.liftA = v;
        else if (key == "liftB") p.liftB = v;
        else if (key == "flash") p.flash = v;
        else if (key == "wing") p.wing = v;
        else if (key == "wave") p.wave = v;
        else if (key == "wavePhase") p.wavePhase = v;
    }
    return p;
}

// Locomotion cycles depend on the body.
FramePose gait(Body body, double phase, double swing, double lift, double bob, double lean) {
    const double s = std::sin(phase), c = std::cos(phase);
    FramePose p;
    switch (body) {
    case Body::Biped:
    case Body::Quadruped:
        p.swingA = swing * s;
        p.swingB = -swing * s;
        p.liftA = lift * std::max(0.0, -c);   // the leg swinging forward is lifted
        p.liftB = lift * std::max(0.0, c);
        p.dy = -bob * std::abs(c);            // highest when the legs pass each other
        p.lean = lean;
        break;
    case Body::Blob: {   // hop: in the air for half the cycle, squashed on landing
        const double u = std::sin(phase);
        if (u > 0) { p.dy = -bob * 3 * u; p.sy = 1 + 0.06 * u * (swing / 0.35); p.sx = 1 - 0.04 * u * (swing / 0.35); }
        else { p.sy = 1 + 0.12 * u * (swing / 0.35); p.sx = 1 - 0.08 * u * (swing / 0.35); }
        p.lean = lean * 0.5;
        break;
    }
    case Body::Swimmer:
        p.wave = 0.06 * (swing / 0.35);
        p.wavePhase = phase;
        p.dy = -bob * 0.5 * s;
        break;
    case Body::Flyer:
        p.dy = -bob * 1.5 * s;
        p.sy = 1 + 0.04 * c;
        p.lean = lean;
        break;
    case Body::Object:
        p.rot = 0.1 * (swing / 0.35) * s;
        p.dy = -bob * 0.6 * std::abs(s);
        break;
    }
    return p;
}

struct ActionPlan {
    Pivot pivot = Pivot::Feet;
    std::function<FramePose(double)> pose;   // t in [0, 1]
};

ActionPlan plan(Action a, Body body, const Subject& subj, const CustomMotion& cm) {
    const double legLen = subj.legTop >= 0 ? double(subj.bounds.y + subj.bounds.h - subj.legTop) / subj.bounds.h : 0.0;
    using K = std::vector<std::pair<double, FramePose>>;
    auto keys = [](K k) { return [k = std::move(k)](double t) { return keyed(k, t); }; };
    constexpr double tau = 2 * kPi;
    if (body == Body::Swimmer) {
        // A wave from the head to the tail: gentle when idle, strong when swimming.
        switch (a) {
        case Action::Idle:
            return {Pivot::Centre, [](double t) {
                        return fp({{"wave", 0.035}, {"wavePhase", tau * t}, {"dy", 0.015 * std::sin(tau * t)}});
                    }};
        case Action::Swim:
            return {Pivot::Centre, [](double t) {
                        return fp({{"wave", 0.075}, {"wavePhase", tau * t}, {"dy", -0.02 * std::sin(tau * t)},
                                   {"rot", -0.03 * std::cos(tau * t)}});
                    }};
        case Action::Dash:
            return {Pivot::Centre, [](double t) {
                        return fp({{"wave", 0.11}, {"wavePhase", 2 * tau * t}, {"dx", 0.12 * std::sin(kPi * t)}, {"sx", 1 + 0.06 * std::sin(kPi * t)},
                                   {"sy", 1 - 0.04 * std::sin(kPi * t)}});
                    }};
        default: break;
        }
    }
    switch (a) {
    case Action::Idle:
        return {Pivot::Feet, [](double t) {
                    const double b = 0.5 - 0.5 * std::cos(tau * t);
                    return fp({{"sy", 1 - 0.035 * b}, {"sx", 1 + 0.015 * b}});
                }};
    case Action::Walk: return {Pivot::Feet, [body](double t) { return gait(body, tau * t, 0.35, 0.05, 0.03, 0.03); }};
    case Action::Run: return {Pivot::Feet, [body](double t) { return gait(body, tau * t, 0.6, 0.1, 0.06, 0.12); }};
    case Action::Sprint: return {Pivot::Feet, [body](double t) { return gait(body, tau * t, 0.75, 0.12, 0.07, 0.2); }};
    case Action::Jump:
        return {Pivot::Feet, keys({{0, {}},
                                   {0.15, fp({{"sy", 0.85}, {"sx", 1.1}})},
                                   {0.3, fp({{"sy", 1.12}, {"sx", 0.92}, {"dy", -0.25}, {"swingA", 0.25}, {"swingB", 0.1}})},
                                   {0.5, fp({{"dy", -0.45}, {"swingA", 0.4}, {"swingB", 0.2}, {"liftA", 0.06}, {"liftB", 0.06}})},
                                   {0.7, fp({{"dy", -0.25}, {"sy", 1.08}, {"sx", 0.95}, {"swingA", -0.1}, {"swingB", 0.1}})},
                                   {0.85, fp({{"sy", 0.85}, {"sx", 1.1}})},
                                   {1, {}}})};
    case Action::Fall:
        return {Pivot::Centre, [](double t) {
                    const double s = std::sin(tau * t);
                    return fp({{"sy", 1.06}, {"sx", 0.96}, {"swingA", 0.15 * s}, {"swingB", -0.15 * s}, {"rot", 0.04 * s}});
                }};
    case Action::Land:
        return {Pivot::Feet, keys({{0, fp({{"sy", 0.78}, {"sx", 1.15}})},
                                   {0.4, fp({{"sy", 0.9}, {"sx", 1.06}})},
                                   {0.7, fp({{"sy", 1.04}, {"sx", 0.98}})},
                                   {1, {}}})};
    case Action::Attack1:
        return {Pivot::Feet, keys({{0, {}},
                                   {0.2, fp({{"lean", -0.12}, {"dx", -0.04}, {"sy", 0.96}})},
                                   {0.45, fp({{"lean", 0.22}, {"dx", 0.14}, {"sx", 1.08}, {"sy", 0.95}, {"swingA", -0.3}, {"swingB", 0.2}})},
                                   {0.65, fp({{"lean", 0.18}, {"dx", 0.12}, {"swingA", -0.3}, {"swingB", 0.2}})},
                                   {1, {}}})};
    case Action::Attack2:
        return {Pivot::Feet, keys({{0, {}},
                                   {0.25, fp({{"rot", -0.18}, {"dy", -0.03}, {"sy", 1.05}})},
                                   {0.5, fp({{"rot", 0.25}, {"sy", 0.92}, {"dx", 0.06}, {"swingA", -0.25}, {"swingB", 0.2}})},
                                   {0.7, fp({{"rot", 0.2}, {"dx", 0.06}, {"swingA", -0.25}, {"swingB", 0.2}})},
                                   {1, {}}})};
    case Action::Attack3:
        return {Pivot::Feet, keys({{0, {}},
                                   {0.25, fp({{"dx", -0.06}, {"sx", 0.94}, {"lean", -0.08}})},
                                   {0.5, fp({{"dx", 0.22}, {"sx", 1.18}, {"sy", 0.92}, {"lean", 0.1}, {"swingA", -0.4}, {"swingB", 0.3}})},
                                   {0.7, fp({{"dx", 0.18}, {"sx", 1.05}, {"swingA", -0.4}, {"swingB", 0.3}})},
                                   {1, {}}})};
    case Action::Dodge:
        return {Pivot::Feet, keys({{0, {}},
                                   {0.3, fp({{"dx", -0.22}, {"lean", -0.18}, {"sy", 0.9}, {"liftA", 0.05}})},
                                   {0.6, fp({{"dx", -0.25}, {"lean", -0.1}, {"sy", 0.95}})},
                                   {1, {}}})};
    case Action::Roll:
        return {Pivot::Centre, [](double t) { return fp({{"rot", tau * t}, {"sy", 0.9}, {"sx", 0.95}}); }};
    case Action::Dash:
        return {Pivot::Feet, keys({{0, {}},
                                   {0.25, fp({{"sx", 1.25}, {"sy", 0.9}, {"lean", 0.2}, {"dx", 0.15}, {"swingA", -0.3}, {"swingB", 0.4}})},
                                   {0.6, fp({{"sx", 1.2}, {"sy", 0.92}, {"lean", 0.18}, {"dx", 0.3}, {"swingA", -0.3}, {"swingB", 0.4}})},
                                   {1, {}}})};
    case Action::Block:
        return {Pivot::Feet, keys({{0, {}}, {0.4, fp({{"lean", -0.07}, {"dx", -0.03}, {"sx", 0.96}, {"sy", 0.97}})},
                                   {1, fp({{"lean", -0.07}, {"dx", -0.03}, {"sx", 0.96}, {"sy", 0.97}})}})};
    case Action::Hurt:
        return {Pivot::Feet, keys({{0, fp({{"flash", 1}, {"dx", -0.1}, {"rot", -0.1}})},
                                   {0.35, fp({{"dx", -0.12}, {"rot", -0.12}})},
                                   {0.7, fp({{"dx", -0.05}, {"rot", -0.05}})},
                                   {1, {}}})};
    case Action::Death:
        return {Pivot::BackFoot, keys({{0, {}},
                                       {0.2, fp({{"rot", -0.15}, {"dx", -0.05}, {"sy", 0.97}})},
                                       {0.5, fp({{"rot", -0.8}})},
                                       {0.8, fp({{"rot", -kPi / 2}})},
                                       {0.9, fp({{"rot", -kPi / 2}, {"sx", 0.92}})},
                                       {1, fp({{"rot", -kPi / 2}})}})};
    case Action::Cast:
        return {Pivot::Feet, keys({{0, {}},
                                   {0.3, fp({{"dy", -0.04}, {"sy", 1.06}, {"lean", -0.06}})},
                                   {0.55, fp({{"dy", -0.06}, {"sy", 1.08}, {"lean", -0.04}})},
                                   {0.8, fp({{"sy", 0.95}, {"lean", 0.08}})},
                                   {1, {}}})};
    case Action::Swim:
        return {Pivot::Centre, [](double t) {
                    const double s = std::sin(tau * t);
                    return fp({{"rot", 0.35}, {"dy", -0.03 * s}, {"swingA", 0.3 * s}, {"swingB", -0.3 * s}});
                }};
    case Action::Fly:
        return {Pivot::Feet, [](double t) {
                    const double s = std::sin(tau * t), c = std::cos(tau * t);
                    return fp({{"dy", -0.08 * s - 0.04}, {"sy", 1 + 0.05 * c}, {"sx", 1 - 0.03 * c}, {"swingA", 0.12 * s},
                               {"swingB", 0.08 * s}, {"lean", 0.05}});
                }};
    case Action::Flap:
        return {Pivot::Feet, [](double t) {
                    const double c = std::cos(tau * t);
                    // Up (1) to folded below the line (-0.6); the body rises on the down stroke.
                    return fp({{"wing", 0.2 + 0.8 * c}, {"dy", -0.04 * (0.5 - 0.5 * c)}});
                }};
    case Action::Climb:
        return {Pivot::Feet, [](double t) {
                    const double s = std::sin(tau * t);
                    return fp({{"dy", -0.04 * std::abs(s)}, {"liftA", 0.1 * std::max(0.0, s)}, {"liftB", 0.1 * std::max(0.0, -s)},
                               {"dx", 0.02 * s}});
                }};
    case Action::Sit:
        if (legLen > 0)
            return {Pivot::Feet, keys({{0, {}},
                                       {0.5, fp({{"swingA", -0.6}, {"swingB", -0.6}, {"dy", 0.3 * legLen}})},
                                       {1, fp({{"swingA", -1.3}, {"swingB", -1.3}, {"dy", 0.6 * legLen}})}})};
        return {Pivot::Feet, keys({{0, {}}, {1, fp({{"sy", 0.8}, {"sx", 1.08}})}})};
    case Action::Sleep:
        return {Pivot::Feet, [](double t) {
                    const double b = 0.5 - 0.5 * std::cos(tau * t);
                    return fp({{"rot", -kPi / 2}, {"sx", 1 + 0.04 * b}});
                }};
    case Action::Custom:
        return {Pivot::Feet, [cm](double t) {
                    const double s = std::sin(tau * t), b = 0.5 - 0.5 * std::cos(tau * t);
                    return fp({{"dy", -cm.bob * b}, {"dx", cm.sway * s}, {"sy", 1 - cm.squash * s}, {"sx", 1 + cm.squash * 0.5 * s},
                               {"rot", cm.tilt * s}, {"swingA", cm.legSwing * s}, {"swingB", -cm.legSwing * s}});
                }};
    }
    return {Pivot::Feet, [](double) { return FramePose{}; }};
}

FramePose scaled(FramePose p, double amount) {
    p.dx *= amount;
    p.dy *= amount;
    p.sx = 1 + (p.sx - 1) * amount;
    p.sy = 1 + (p.sy - 1) * amount;
    p.lean *= amount;
    p.swingA *= amount;
    p.swingB *= amount;
    p.liftA *= amount;
    p.liftB *= amount;
    p.wave *= amount;
    p.wing = 1 + (p.wing - 1) * amount;
    if (std::abs(p.wing) < 0.1) p.wing = p.wing < 0 ? -0.1 : 0.1;   // never flat (the transform must stay invertible)
    // Rotations that define the action (lying down, a full roll) stay whole;
    // small ones scale.
    if (std::abs(p.rot) < 0.5) p.rot *= amount;
    return p;
}

std::vector<std::pair<int, int>> columnRuns(const std::vector<char>& cols, int minWidth) {
    std::vector<std::pair<int, int>> runs;
    int start = -1;
    for (int x = 0; x <= static_cast<int>(cols.size()); ++x) {
        const bool on = x < static_cast<int>(cols.size()) && cols[static_cast<std::size_t>(x)];
        if (on && start < 0) start = x;
        if (!on && start >= 0) {
            if (x - start >= minWidth) runs.push_back({start, x - 1});
            start = -1;
        }
    }
    return runs;
}

int rowRuns(const Image& img, int y, int x0, int x1) {
    int runs = 0;
    bool in = false;
    for (int x = x0; x <= x1; ++x) {
        const bool on = img.pixel(x, y).a > 0;
        if (on && !in) ++runs;
        in = on;
    }
    return runs;
}

struct Part {
    int index = 0;            // 0 = body, 1.. = foot run + 1
    Affine toSource;          // canvas -> sprite coordinates
    IntRect destBox;          // where it can land in the canvas
    double waveAmp = 0;       // swimmer: tail amplitude in pixels (0 = none)
    double wavePhase = 0;
};

struct RenderedClip {
    std::vector<Image> frames;   // each `box` sized
    IntRect box;                 // in sprite coordinates (may extend outside the sprite)
};

RenderedClip render(const Image& sprite, const Subject& subj, Body body, Action action, int frames, bool loops, double amount,
                    const CustomMotion& cm, double wingLine, double wingFrom, double wingTo, const Mask& wingMask) {
    const IntRect bb = subj.bounds;
    const double W = bb.w, H = bb.h;
    const bool legs = (body == Body::Biped || body == Body::Quadruped) && subj.legTop >= 0 && !subj.feet.empty();
    // Part map: which part each sprite pixel belongs to.
    std::vector<std::uint8_t> partOf(static_cast<std::size_t>(sprite.width()) * static_cast<std::size_t>(sprite.height()), 0);
    // Parts: 0 body, 1..n one per foot, n + 1 the ground shadow under the feet (stays on the ground).
    const std::size_t groundPart = 1 + subj.feet.size();
    // A blob's flat bottom is part of it; for other bodies the pixels of the
    // ground rows that stick out beyond the row above are a shadow.
    const bool hasGround = subj.groundTop >= 0 && body != Body::Blob && body != Body::Flyer;
    int shadowX0 = 0, shadowX1 = -1;
    if (hasGround) {
        for (int x = bb.x; x < bb.x + bb.w; ++x)
            if (sprite.pixel(x, subj.groundTop - 1).a > 0) { if (shadowX1 < 0) shadowX0 = x; shadowX1 = x; }
    }
    // Wing flap: the part above the wing line, n + 2.
    const std::size_t wingPart = groundPart + 1;
    const bool flap = action == Action::Flap;
    const int wingRow = bb.y + static_cast<int>(std::lround(std::clamp(wingLine, 0.05, 0.95) * bb.h));
    const int wingX0 = bb.x + static_cast<int>(std::floor(std::clamp(std::min(wingFrom, wingTo), 0.0, 1.0) * bb.w));
    const int wingX1 = bb.x + static_cast<int>(std::ceil(std::clamp(std::max(wingFrom, wingTo), 0.0, 1.0) * bb.w));
    // Arms, n + 3 onwards: they swing against the legs in walk cycles.
    const bool locomotion = action == Action::Walk || action == Action::Run || action == Action::Sprint;
    const std::size_t armPart0 = wingPart + 1;
    const std::vector<IntRect> arms = legs && locomotion ? subj.arms : std::vector<IntRect>{};
    std::vector<IntRect> partBounds(armPart0 + arms.size(), IntRect{});
    auto grow = [](IntRect& r, int x, int y) {
        if (r.empty()) { r = {x, y, 1, 1}; return; }
        const int x1 = std::max(r.x + r.w, x + 1), y1 = std::max(r.y + r.h, y + 1);
        r.x = std::min(r.x, x); r.y = std::min(r.y, y);
        r.w = x1 - r.x; r.h = y1 - r.y;
    };
    for (int y = bb.y; y < bb.y + bb.h; ++y)
        for (int x = bb.x; x < bb.x + bb.w; ++x) {
            if (sprite.pixel(x, y).a == 0) continue;
            std::size_t part = 0;
            if (hasGround && y >= subj.groundTop && (x < shadowX0 || x > shadowX1)) {
                part = groundPart;
            } else if (flap && (wingMask.isNull() ? (y < wingRow && x >= wingX0 && x < wingX1) : wingMask.get(x, y))) {
                part = wingPart;
            } else if (auto arm = std::find_if(arms.begin(), arms.end(), [x, y](const IntRect& r) { return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h; });
                       arm != arms.end()) {
                part = armPart0 + static_cast<std::size_t>(arm - arms.begin());
            } else if (legs && y >= subj.legTop) {
                int best = 0, bestDist = std::numeric_limits<int>::max();
                for (std::size_t i = 0; i < subj.feet.size(); ++i) {
                    const auto [x0, x1] = subj.feet[i];
                    const int d = x < x0 ? x0 - x : x > x1 ? x - x1 : 0;
                    if (d < bestDist) { bestDist = d; best = static_cast<int>(i); }
                }
                part = static_cast<std::size_t>(best) + 1;
            }
            partOf[static_cast<std::size_t>(y) * static_cast<std::size_t>(sprite.width()) + static_cast<std::size_t>(x)] = static_cast<std::uint8_t>(part);
            grow(partBounds[part], x, y);
        }

    const ActionPlan pl = plan(action, body, subj, cm);
    const double headX = subj.headLeft ? bb.x : bb.x + bb.w;
    const double bottom = bb.y + bb.h;
    double px = bb.x + W / 2, py = bottom;
    if (pl.pivot == Pivot::Centre) py = bb.y + H / 2;
    if (pl.pivot == Pivot::BackFoot) px = bb.x;

    std::vector<std::vector<Part>> parts(static_cast<std::size_t>(frames));
    std::vector<bool> flash(static_cast<std::size_t>(frames), false);
    IntRect all;
    for (int f = 0; f < frames; ++f) {
        const double t = loops ? double(f) / frames : (frames > 1 ? double(f) / (frames - 1) : 0.0);
        FramePose p = scaled(pl.pose(t), amount);
        if (subj.frontView && legs) p.lean = 0;   // seen from the front there is no forward to lean into
        flash[static_cast<std::size_t>(f)] = p.flash > 0.5;
        // Offsets snap to whole pixels so parts move together.
        const double dx = std::round(p.dx * W), dy = std::round(p.dy * H);
        const Affine global = Affine::translate(dx, dy) *
                              about(px, py, Affine::rotate(p.rot) * Affine::scale(p.sx, p.sy) * Affine::shearX(-p.lean));
        for (std::size_t i = 0; i < partBounds.size(); ++i) {
            if (partBounds[i].empty()) continue;
            Affine local;
            Affine m;
            if (i == groundPart) {
                m = Affine::translate(dx, 0);
            } else if (i == wingPart) {
                local = about(px, wingRow, Affine::scale(1, p.wing));
            } else if (i >= armPart0) {
                // Arms swing against the leg on the same side: forward as it
                // goes back. Seen from the front, the arm rises a pixel instead.
                const IntRect& r = arms[i - armPart0];
                const bool leftArm = r.x + r.w / 2 < bb.x + bb.w / 2;
                const double legSwing = leftArm ? p.swingA : p.swingB;
                const double legLift = leftArm ? p.liftB : p.liftA;
                if (subj.frontView) local = Affine::translate(0, -std::round(0.5 * legLift * H));
                else local = about(r.x + r.w / 2.0, r.y, Affine::rotate(-0.6 * legSwing));
            } else if (i > 0) {
                const auto [x0, x1] = subj.feet[i - 1];
                const bool groupA = (i - 1) % 2 == 0;
                // Seen from the front, legs step up in turn instead of swinging.
                const double swing = subj.frontView ? 0.0 : groupA ? p.swingA : p.swingB;
                const double lift = std::round((groupA ? p.liftA : p.liftB) * H);
                const double hipX = (x0 + x1 + 1) / 2.0;
                double stretch = 1;
                if (locomotion && lift == 0) {
                    // The standing leg keeps its foot on the ground: a rigid
                    // leg would rise with the body bob and the swing; it is
                    // stretched along its length instead (a pixel or two).
                    const int footBottom = i - 1 < subj.footBottoms.size() ? subj.footBottoms[i - 1] : subj.footRow;
                    const double len = footBottom + 1 - subj.legTop;
                    const double halfFoot = (x1 - x0 + 1) / 2.0;
                    if (len >= 3)
                        stretch = std::clamp((len - dy - halfFoot * std::abs(std::sin(swing))) / (len * std::cos(swing)), 0.85, 1.35);
                }
                local = Affine::translate(0, -lift) * about(hipX, subj.legTop, Affine::rotate(swing) * Affine::scale(1, stretch));
            }
            if (i != groundPart) m = global * local;
            const IntRect& r = partBounds[i];
            double minX = 1e9, minY = 1e9, maxX = -1e9, maxY = -1e9;
            for (auto [cx, cy] : {std::pair<double, double>{r.x, r.y}, {r.x + r.w, r.y}, {r.x, r.y + r.h}, {r.x + r.w, r.y + r.h}}) {
                const auto [qx, qy] = m.apply(cx, cy);
                minX = std::min(minX, qx); maxX = std::max(maxX, qx);
                minY = std::min(minY, qy); maxY = std::max(maxY, qy);
            }
            const double amp = body == Body::Swimmer && i == 0 ? p.wave * H : 0.0;
            const int waveGrow = static_cast<int>(std::ceil(std::abs(amp))) + 1;
            const IntRect dest{static_cast<int>(std::floor(minX)) - 1, static_cast<int>(std::floor(minY)) - 1 - waveGrow,
                               static_cast<int>(std::ceil(maxX - minX)) + 3, static_cast<int>(std::ceil(maxY - minY)) + 3 + 2 * waveGrow};
            parts[static_cast<std::size_t>(f)].push_back({static_cast<int>(i), m.inverse(), dest, amp, p.wavePhase});
            if (all.empty()) all = dest;
            else {
                const int x1 = std::max(all.x + all.w, dest.x + dest.w), y1 = std::max(all.y + all.h, dest.y + dest.h);
                all.x = std::min(all.x, dest.x); all.y = std::min(all.y, dest.y);
                all.w = x1 - all.x; all.h = y1 - all.y;
            }
        }
    }

    RenderedClip out;
    out.box = all;
    for (int f = 0; f < frames; ++f) {
        Image img(all.w, all.h);
        auto& ps = parts[static_cast<std::size_t>(f)];
        // Draw order: legs of group B (far), group A, then the body on top so it covers the hips.
        std::vector<const Part*> order;
        for (const Part& p : ps) if (p.index == static_cast<int>(groundPart)) order.push_back(&p);
        auto isLeg = [&](const Part& p) {
            return p.index > 0 && p.index != static_cast<int>(groundPart) && p.index < static_cast<int>(wingPart);
        };
        for (const Part& p : ps) if (isLeg(p) && (p.index - 1) % 2 == 1) order.push_back(&p);
        for (const Part& p : ps) if (isLeg(p) && (p.index - 1) % 2 == 0) order.push_back(&p);
        for (const Part& p : ps) if (p.index == 0) order.push_back(&p);
        for (const Part& p : ps) if (p.index == static_cast<int>(wingPart)) order.push_back(&p);
        for (const Part& p : ps) if (p.index >= static_cast<int>(armPart0)) order.push_back(&p);
        for (const Part* p : order) {
            for (int y = p->destBox.y; y < p->destBox.y + p->destBox.h; ++y)
                for (int x = p->destBox.x; x < p->destBox.x + p->destBox.w; ++x) {
                    auto [sxf, syf] = p->toSource.apply(x + 0.5, y + 0.5);
                    if (p->waveAmp != 0) {
                        // Displacement grows towards the tail (u = 0 at the head, 1 at the tail)
                        // and travels backwards along the body.
                        const double u = std::clamp(std::abs(sxf - headX) / std::max(1.0, W), 0.0, 1.0);
                        syf -= p->waveAmp * u * u * std::sin(p->wavePhase - 2 * kPi * 0.85 * u);
                    }
                    const int sx = static_cast<int>(std::floor(sxf)), sy = static_cast<int>(std::floor(syf));
                    if (!sprite.contains(sx, sy)) continue;
                    const Rgba8 c = sprite.pixel(sx, sy);
                    if (c.a == 0) continue;
                    if (partOf[static_cast<std::size_t>(sy) * static_cast<std::size_t>(sprite.width()) + static_cast<std::size_t>(sx)] != p->index) continue;
                    img.setPixel(x - all.x, y - all.y, flash[static_cast<std::size_t>(f)] ? Rgba8{255, 255, 255, c.a} : c);
                }
        }
        out.frames.push_back(std::move(img));
    }
    return out;
}

Image place(const Image& frame, const IntRect& from, const IntRect& to) {
    Image out(to.w, to.h);
    out.blit(frame, from.x - to.x, from.y - to.y);
    return out;
}

Image mirror(const Image& img) {
    Image out(img.width(), img.height());
    for (int y = 0; y < img.height(); ++y)
        for (int x = 0; x < img.width(); ++x) out.setPixel(img.width() - 1 - x, y, img.pixel(x, y));
    return out;
}

// Shrinks the shared box to what any frame actually covers (plus 1 px).
IntRect usedBox(const std::vector<RenderedClip>& clips) {
    IntRect used;
    for (const RenderedClip& c : clips)
        for (const Image& f : c.frames) {
            IntRect b = f.opaqueBounds();
            if (b.empty()) continue;
            b.x += c.box.x; b.y += c.box.y;
            if (used.empty()) used = b;
            else {
                const int x1 = std::max(used.x + used.w, b.x + b.w), y1 = std::max(used.y + used.h, b.y + b.h);
                used.x = std::min(used.x, b.x); used.y = std::min(used.y, b.y);
                used.w = x1 - used.x; used.h = y1 - used.y;
            }
        }
    return {used.x - 1, used.y - 1, used.w + 2, used.h + 2};
}

double iou(const Image& a, const Image& b) {
    std::size_t inter = 0, uni = 0;
    for (int y = 0; y < std::max(a.height(), b.height()); ++y)
        for (int x = 0; x < std::max(a.width(), b.width()); ++x) {
            const bool p = a.pixel(x, y).a > 0, q = b.pixel(x, y).a > 0;
            inter += p && q;
            uni += p || q;
        }
    return uni ? double(inter) / double(uni) : 1.0;
}

std::size_t area(const Image& img) {
    std::size_t n = 0;
    for (const Rgba8& c : img.pixels()) n += c.a > 0;
    return n;
}

} // namespace

const std::vector<ActionInfo>& actions() { return kActions; }

const ActionInfo& actionInfo(Action a) {
    for (const ActionInfo& i : kActions)
        if (i.action == a) return i;
    return kActions.front();
}

std::optional<Action> actionFromKey(std::string_view key) {
    for (const ActionInfo& i : kActions)
        if (key == i.key) return i.action;
    return std::nullopt;
}

const char* toString(Body b) {
    switch (b) {
    case Body::Biped: return "biped";
    case Body::Quadruped: return "quadruped";
    case Body::Blob: return "blob";
    case Body::Flyer: return "flyer";
    case Body::Swimmer: return "swimmer";
    case Body::Object: return "object";
    }
    return "object";
}

std::optional<Body> bodyFromString(std::string_view s) {
    for (Body b : {Body::Biped, Body::Quadruped, Body::Blob, Body::Flyer, Body::Object, Body::Swimmer})
        if (s == toString(b)) return b;
    return std::nullopt;
}

Subject analyzeSubject(const Image& sprite) {
    Subject s;
    s.bounds = sprite.opaqueBounds();
    if (s.bounds.empty()) return s;
    const IntRect& bb = s.bounds;
    const int bottom = bb.y + bb.h - 1;
    // A ground shadow or base line: the lowest rows, when they suddenly get
    // wider than the row above. It stays on the ground while the sprite moves.
    auto span = [&](int y) {
        int x0 = -1, x1 = -1;
        for (int x = bb.x; x < bb.x + bb.w; ++x)
            if (sprite.pixel(x, y).a > 0) { if (x0 < 0) x0 = x; x1 = x; }
        return x0 < 0 ? 0 : x1 - x0 + 1;
    };
    for (int y = bottom; y > bottom - std::max(1, bb.h / 8) && y > bb.y + 1; --y)
        if (span(y) * 4 >= span(y - 1) * 5 + 4) { s.groundTop = y; break; }
    s.footRow = s.groundTop >= 0 ? s.groundTop - 1 : bottom;
    // Feet: column runs in the lowest tenth above the ground. Thin runs (a
    // sword tip, a staff) are not feet.
    const int band = std::max(1, bb.h / 10);
    std::vector<char> cols(static_cast<std::size_t>(bb.w), 0);
    for (int y = s.footRow - band + 1; y <= s.footRow; ++y)
        for (int x = 0; x < bb.w; ++x)
            if (sprite.pixel(bb.x + x, y).a > 0) cols[static_cast<std::size_t>(x)] = 1;
    for (auto [x0, x1] : columnRuns(cols, std::max(2, bb.w / 12))) s.feet.push_back({bb.x + x0, bb.x + x1});
    if (s.feet.size() >= 2) {
        // Legs go up as long as the rows stay split. Fins (a fish's belly and
        // tail fins on the test PC) split for only a row or two from the
        // bottom; they are not legs.
        const int x0 = s.feet.front().first, x1 = s.feet.back().second;
        int top = s.footRow;
        while (top - 1 >= bb.y && rowRuns(sprite, top - 1, x0, x1) >= 2) --top;
        // The legs must be apart in some row of the foot band, not only in
        // the lowest one: in a three-quarter stance the back foot ends a row or
        // two higher (a generated 64 px rogue on the test PC).
        int split = -1;
        for (int y = s.footRow; y > s.footRow - band && y >= bb.y; --y)
            if (rowRuns(sprite, y, x0, x1) >= 2) { split = y; break; }
        if (split >= 0) {
            top = split;
            while (top - 1 >= bb.y && rowRuns(sprite, top - 1, x0, x1) >= 2) --top;
        }
        if (split < 0 || split + 1 - top < std::max(2, bb.h / 6)) s.feet.clear();
    }
    if (s.feet.size() >= 2) {
        const int x0 = s.feet.front().first, x1 = s.feet.back().second;
        int top = s.footRow;
        while (top > s.footRow - band && top > bb.y && rowRuns(sprite, top, x0, x1) < 2) --top;   // staggered feet
        while (top - 1 >= bb.y && rowRuns(sprite, top - 1, x0, x1) >= 2) --top;
        const int minLeg = std::max(2, bb.h / 5);
        top = std::min(top, s.footRow + 1 - minLeg);
        top = std::max(top, bb.y + (bb.h * 2) / 5);
        s.legTop = top;
        s.body = (s.feet.size() >= 3 || bb.w > bb.h * 5 / 4) ? Body::Quadruped : Body::Biped;
    } else if (s.feet.size() == 1) {
        // Front view: the legs touch, so the feet are one run. A tall figure
        // whose lower part is a narrow column (at most 70% of the width)
        // under a wider upper body stands on two legs; split them at the
        // middle. A tree trunk under a canopy is much narrower than the
        // canopy, so it is not taken for legs.
        const auto [fx0, fx1] = s.feet.front();
        const int fw = fx1 - fx0 + 1;
        if (bb.h * 10 >= bb.w * 14 && fw >= 4 && fw * 10 <= bb.w * 7) {
            int top = s.footRow;
            while (top - 1 > bb.y && span(top - 1) <= fw + 2) --top;
            int upper = 0;
            for (int y = bb.y; y < top; ++y) upper = std::max(upper, span(y));
            const int legLen = s.footRow + 1 - top;
            if (legLen >= std::max(3, bb.h / 6) && top >= bb.y + (bb.h * 2) / 5 && upper > fw + 2 && upper * 10 <= fw * 26) {
                const int mid = (fx0 + fx1 + 1) / 2;
                s.feet = {{fx0, mid - 1}, {mid, fx1}};
                s.legTop = top;
                s.frontView = true;
                s.body = Body::Biped;
            }
        }
    }
    if (s.body == Body::Biped && s.feet.size() >= 2) {
        // Arms: pixels of the upper body to the left of the left leg and to
        // the right of the right leg. Not when the head is wider than the
        // body below it (a big-headed chibi: its sides are head, not arms).
        // The legs' outer edges at mid-leg (boots are often wider).
        int lx0 = s.feet.front().first, lx1 = s.feet.back().second;
        {
            const int midRow = (s.legTop + s.footRow) / 2;
            int m0 = -1, m1 = -1;
            for (int x = lx0; x <= lx1; ++x)
                if (sprite.pixel(x, midRow).a > 0) { if (m0 < 0) m0 = x; m1 = x; }
            if (m0 >= 0) { lx0 = m0; lx1 = m1; }
        }
        const int bandTop = bb.y + bb.h / 5, bandBottom = s.footRow - std::max(1, bb.h / 10);
        int headSpan = 0, bodySpan = 0;
        for (int y = bb.y; y < bb.y + (bb.h * 45) / 100; ++y) headSpan = std::max(headSpan, span(y));
        for (int y = bb.y + (bb.h * 45) / 100; y < s.legTop; ++y) bodySpan = std::max(bodySpan, span(y));
        if (bodySpan >= headSpan) {
            for (int side = 0; side < 2; ++side) {
                IntRect r;
                int rows = 0;
                for (int y = bandTop; y <= bandBottom; ++y) {
                    bool any = false;
                    for (int x = side == 0 ? bb.x : lx1 + 2; x < (side == 0 ? lx0 - 1 : bb.x + bb.w); ++x)
                        if (sprite.pixel(x, y).a > 0) {
                            any = true;
                            if (r.empty()) r = {x, y, 1, 1};
                            const int x1 = std::max(r.x + r.w, x + 1), y1 = std::max(r.y + r.h, y + 1);
                            r.x = std::min(r.x, x);
                            r.y = std::min(r.y, y);
                            r.w = x1 - r.x;
                            r.h = y1 - r.y;
                        }
                    rows += any ? 1 : 0;
                }
                // The arm's inner columns lie over the legs' edge: take a
                // column while nothing sits right above the shoulder (the
                // torso goes on up; an arm starts at the shoulder).
                if (!r.empty()) {
                    auto armColumn = [&](int x) {
                        if (x < bb.x || x >= bb.x + bb.w) return false;
                        for (int y = std::max(bb.y, r.y - 2); y < r.y; ++y)
                            if (sprite.pixel(x, y).a > 0) return false;
                        for (int y = r.y; y < r.y + r.h; ++y)
                            if (sprite.pixel(x, y).a > 0) return true;
                        return false;
                    };
                    if (side == 0) while (r.x + r.w < lx0 + 1 && armColumn(r.x + r.w)) ++r.w;
                    else while (r.x - 1 > lx1 - 1 && armColumn(r.x - 1)) { --r.x; ++r.w; }
                }
                // An arm is drawn apart from the torso beside it: its inner
                // edge differs in colour (or an outline) from the torso column
                // next to it in most rows. A shirt or coat wider than the legs
                // is the same colour on both sides and stays with the body.
                if (!r.empty()) {
                    const int inner = side == 0 ? r.x + r.w - 1 : r.x, next = side == 0 ? inner + 1 : inner - 1;
                    int both = 0, differ = 0;
                    for (int y = r.y; y < r.y + r.h; ++y) {
                        if (next < bb.x || next >= bb.x + bb.w) break;
                        const Rgba8 a = sprite.pixel(inner, y), b = sprite.pixel(next, y);
                        if (a.a == 0 || b.a == 0) continue;
                        ++both;
                        const int dl = std::abs((a.r * 299 + a.g * 587 + a.b * 114) - (b.r * 299 + b.g * 587 + b.b * 114)) / 1000;
                        const int dc = std::abs(a.r - b.r) + std::abs(a.g - b.g) + std::abs(a.b - b.b);
                        if (dl > 24 || dc > 60) ++differ;
                    }
                    if (both > 0 && differ * 2 < both) r = {};
                }
                // Long and thin, mostly continuous, not most of the figure.
                if (!r.empty() && r.w >= 2 && r.h >= 2 * r.w && r.h >= bb.h / 5 && r.w * 10 <= bb.w * 3 && rows * 4 >= r.h * 3)
                    s.arms.push_back(r);
            }
        }
    }
    if (s.feet.size() < 2 || s.legTop < 0) {
        if (s.feet.size() >= 2) s.feet.clear();
        const int bottomWidth = s.feet.empty() ? 0 : s.feet.front().second - s.feet.front().first + 1;
        const bool round = bb.w * 10 >= bb.h * 6 && bb.w * 10 <= bb.h * 16;
        s.body = (round && bottomWidth * 10 >= bb.w * 4) ? Body::Blob : Body::Object;
        // Footless and wider than tall (but not a thin stick): a fish-like swimmer.
        if (s.body == Body::Object && bb.w * 4 >= bb.h * 5 && bb.w <= bb.h * 7 / 2) s.body = Body::Swimmer;
    }
    if (!s.feet.empty() && s.legTop >= 0)
        for (const auto& [x0, x1] : s.feet) {
            int lowest = s.legTop;
            for (int y = s.legTop; y <= s.footRow; ++y)
                for (int x = x0; x <= x1; ++x)
                    if (sprite.pixel(x, y).a > 0) lowest = std::max(lowest, y);
            s.footBottoms.push_back(lowest);
        }
    // Facing: a foot sticks out past its leg on the side the toes point to.
    if (s.legTop >= 0 && s.feet.size() >= 2 && !s.frontView) {
        double sum = 0;
        int n = 0;
        const int midRow = (s.legTop + s.footRow) / 2;
        for (std::size_t i = 0; i < s.feet.size(); ++i) {
            const auto [x0, x1] = s.feet[i];
            int m0 = -1, m1 = -1;
            for (int x = x0; x <= x1; ++x)
                if (sprite.pixel(x, midRow).a > 0) { if (m0 < 0) m0 = x; m1 = x; }
            const int sole = i < s.footBottoms.size() ? s.footBottoms[i] : s.footRow;
            int f0 = -1, f1 = -1;
            for (int y = sole - std::max(1, (sole - s.legTop) / 5); y <= sole; ++y)
                for (int x = x0; x <= x1; ++x)
                    if (sprite.pixel(x, y).a > 0) { f0 = f0 < 0 ? x : std::min(f0, x); f1 = std::max(f1, x); }
            if (m0 < 0 || f0 < 0) continue;
            // Toe overhang forward minus heel overhang back.
            sum += (f1 - m1) - (m0 - f0);
            ++n;
        }
        if (n > 0) s.facing = sum / n > 0.5 ? 1 : sum / n < -0.5 ? -1 : 0;
    }
    s.headLeft = spriteFacesLeft(sprite);
    return s;
}

bool spriteFacesLeft(const Image& sprite) {
    const IntRect bb = sprite.opaqueBounds();
    if (bb.w < 6) return false;
    std::vector<int> cols(static_cast<std::size_t>(bb.w), 0);
    long mass = 0;
    double sum = 0;
    for (int x = 0; x < bb.w; ++x)
        for (int y = bb.y; y < bb.y + bb.h; ++y)
            if (sprite.pixel(bb.x + x, y).a > 0) { ++cols[static_cast<std::size_t>(x)]; ++mass; sum += x; }
    // The tail stalk: the thinnest column in the inner 15..85 %, when it is a
    // real waist (clearly thinner than the body on its inner side).
    const int lo = bb.w * 15 / 100, hi = bb.w * 85 / 100;
    int waist = -1;
    for (int x = lo; x < hi; ++x)
        if (waist < 0 || cols[static_cast<std::size_t>(x)] < cols[static_cast<std::size_t>(waist)]) waist = x;
    if (waist >= 0) {
        const bool right = waist * 2 > bb.w;
        int inner = 0;
        if (right) for (int x = lo; x < waist; ++x) inner = std::max(inner, cols[static_cast<std::size_t>(x)]);
        else for (int x = waist + 1; x < hi; ++x) inner = std::max(inner, cols[static_cast<std::size_t>(x)]);
        const bool offCentre = std::abs(waist * 2 - bb.w) * 10 >= bb.w * 2;   // at least 10 % from the middle
        if (offCentre && cols[static_cast<std::size_t>(waist)] * 2 <= inner) return right;   // tail on the right: faces left
    }
    // Otherwise the head end holds more of the body.
    return mass > 0 && sum / static_cast<double>(mass) < (bb.w - 1) * 0.47;
}

namespace {
// Scale2x (EPX): doubles an image, rounding diagonal edges without new colours.
Image scale2x(const Image& in) {
    const int W = in.width(), H = in.height();
    Image out(W * 2, H * 2);
    auto px = [&](int x, int y) { return in.pixel(std::clamp(x, 0, W - 1), std::clamp(y, 0, H - 1)); };
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            const Rgba8 B = px(x, y - 1), D = px(x - 1, y), E = px(x, y), F = px(x + 1, y), Hh = px(x, y + 1);
            Rgba8 e0 = E, e1 = E, e2 = E, e3 = E;
            if (!(B == Hh) && !(D == F)) {
                if (D == B) e0 = D;
                if (B == F) e1 = F;
                if (D == Hh) e2 = D;
                if (Hh == F) e3 = F;
            }
            out.setPixel(2 * x, 2 * y, e0);
            out.setPixel(2 * x + 1, 2 * y, e1);
            out.setPixel(2 * x, 2 * y + 1, e2);
            out.setPixel(2 * x + 1, 2 * y + 1, e3);
        }
    return out;
}
} // namespace

Image rotateSprite(const Image& sprite, double radians) {
    if (sprite.isNull()) return sprite;
    const bool small = std::max(sprite.width(), sprite.height()) <= 256;
    const int up = small ? 8 : 1;
    Image big = sprite;
    if (small) for (int i = 0; i < 3; ++i) big = scale2x(big);
    const double c = std::cos(radians), s = std::sin(radians);
    const double w = sprite.width(), h = sprite.height();
    // Sized exactly (a quarter turn is then a lossless transpose).
    const int ow = std::max(1, static_cast<int>(std::ceil(std::abs(w * c) + std::abs(h * s) - 1e-6)));
    const int oh = std::max(1, static_cast<int>(std::ceil(std::abs(w * s) + std::abs(h * c) - 1e-6)));
    Image out(ow, oh);
    const double cxIn = big.width() / 2.0, cyIn = big.height() / 2.0;
    for (int y = 0; y < oh; ++y)
        for (int x = 0; x < ow; ++x) {
            // The centre of the output pixel, in the up-scaled source (inverse rotation).
            const double dx = (x + 0.5 - ow / 2.0) * up, dy = (y + 0.5 - oh / 2.0) * up;
            const double sx = c * dx + s * dy + cxIn, sy = -s * dx + c * dy + cyIn;
            const int ix = static_cast<int>(std::floor(sx)), iy = static_cast<int>(std::floor(sy));
            if (big.contains(ix, iy)) out.setPixel(x, y, big.pixel(ix, iy));
        }
    return out;
}

Result<Clip> animate(const Image& sprite, const Options& opt) {
    auto pack = animatePack(sprite, {opt.action}, opt);
    if (!pack) return pack.error();
    return std::move(pack.value().front());
}

Result<std::vector<Clip>> animatePack(const Image& sprite, const std::vector<Action>& list, const Options& base) {
    if (sprite.isNull() || sprite.isFullyTransparent())
        return Error(ErrorCode::InvalidArgument, "The sprite has no visible pixels to animate.");
    if (list.empty()) return Error(ErrorCode::InvalidArgument, "Choose at least one action.");
    const Subject subj = analyzeSubject(sprite);
    const Body body = base.body.value_or(subj.body);
    const double amount = std::clamp(base.amount, 0.1, 3.0);
    std::vector<RenderedClip> rendered;
    for (Action a : list) {
        const ActionInfo& info = actionInfo(a);
        // Swimmers get longer, smoother cycles for the wave.
        const bool smooth = body == Body::Swimmer && (a == Action::Idle || a == Action::Swim);
        const int frames = std::clamp(base.frames > 0 && list.size() == 1 ? base.frames : smooth ? 12 : info.frames, 1, 64);
        // Walks, runs and idles of bodies with legs: whole-pixel moves only.
        const detail::WingSpec wing{base.wingLine, base.wingFrom, base.wingTo, base.wingMask.isNull() ? nullptr : &base.wingMask};
        if (auto rig = detail::rigRender(sprite, subj, body, a, frames, amount, wing)) {
            rendered.push_back({std::move(rig->frames), rig->box});
            continue;
        }
        rendered.push_back(render(sprite, subj, body, a, frames, info.loops, amount, base.custom, base.wingLine, base.wingFrom, base.wingTo, base.wingMask));
    }
    const IntRect box = usedBox(rendered);
    std::vector<Clip> out;
    for (std::size_t i = 0; i < list.size(); ++i) {
        const ActionInfo& info = actionInfo(list[i]);
        Clip c;
        c.name = info.key;
        c.loops = info.loops;
        c.body = body;
        if (!subj.footBottoms.empty())
            c.footStagger = *std::max_element(subj.footBottoms.begin(), subj.footBottoms.end()) -
                            *std::min_element(subj.footBottoms.begin(), subj.footBottoms.end());
        for (const Image& f : rendered[i].frames) {
            Image placed = place(f, rendered[i].box, box);
            c.frames.push_back(base.faceLeft ? mirror(placed) : std::move(placed));
            const bool smooth = body == Body::Swimmer && (list[i] == Action::Idle || list[i] == Action::Swim);
            c.durations.push_back(smooth ? (list[i] == Action::Swim ? 70 : 100) : info.frameMs);
        }
        out.push_back(std::move(c));
    }
    return out;
}

ClipCheck checkClip(const Clip& clip) {
    ClipCheck r;
    if (clip.frames.empty()) {
        r.issues.push_back("The clip has no frames.");
        return r;
    }
    const double a0 = static_cast<double>(area(clip.frames.front()));
    for (std::size_t i = 0; i < clip.frames.size(); ++i) {
        const double ai = static_cast<double>(area(clip.frames[i]));
        if (a0 > 0) r.maxAreaChange = std::max(r.maxAreaChange, std::abs(ai - a0) / a0);
        if (i > 0) r.minNeighbourIoU = std::min(r.minNeighbourIoU, iou(clip.frames[i - 1], clip.frames[i]));
    }
    if (clip.loops && clip.frames.size() > 1) r.loopIoU = iou(clip.frames.back(), clip.frames.front());
    // A flap foreshortens the wings on purpose; elsewhere a big change means parts were lost or doubled.
    const double allowed = clip.name == "flap" ? 0.7 : 0.35;
    if (r.maxAreaChange > allowed)
        r.issues.push_back("The sprite's size changes by " + std::to_string(static_cast<int>(r.maxAreaChange * 100)) +
                           "% between frames; parts may be stretched or hidden.");
    // Flicker: pixels that change in one frame and change straight back.
    // A smooth cycle moves edges steadily; single-frame blips read as noise.
    if (clip.frames.size() >= 3) {
        for (std::size_t i = 0; i < clip.frames.size(); ++i) {
            if (!clip.loops && (i == 0 || i + 1 == clip.frames.size())) continue;
            const Image& a = clip.frames[(i + clip.frames.size() - 1) % clip.frames.size()];
            const Image& b = clip.frames[i];
            const Image& c = clip.frames[(i + 1) % clip.frames.size()];
            if (a.width() != b.width() || a.height() != b.height() || c.width() != b.width() || c.height() != b.height()) continue;
            // Only isolated blips count: a part that moves for one frame and
            // back (a bob, a lifted foot) changes a whole connected edge.
            auto same = [](Rgba8 p, Rgba8 q) { return (p.a == 0 && q.a == 0) || (p.r == q.r && p.g == q.g && p.b == q.b && p.a == q.a); };
            const int w = b.width(), h = b.height();
            std::vector<char> blip(static_cast<std::size_t>(w) * static_cast<std::size_t>(h), 0);
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x)
                    blip[static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x)] =
                        same(a.pixel(x, y), c.pixel(x, y)) && !same(a.pixel(x, y), b.pixel(x, y));
            std::size_t blips = 0;
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x) {
                    if (!blip[static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x)]) continue;
                    // Motion moves colours by a pixel; noise makes them up. A
                    // blip whose colour sits next to it in the frame before
                    // is an edge that moved, not flicker.
                    bool moved = false;
                    for (int dy = -1; dy <= 1 && !moved; ++dy)
                        for (int dx = -1; dx <= 1 && !moved; ++dx)
                            if ((dx || dy) && x + dx >= 0 && y + dy >= 0 && x + dx < w && y + dy < h && same(a.pixel(x + dx, y + dy), b.pixel(x, y)))
                                moved = true;
                    // ... and a part that moves fast changes a whole patch: only
                    // lone blips are speckle noise.
                    int near = 0;
                    for (int dy = -1; dy <= 1; ++dy)
                        for (int dx = -1; dx <= 1; ++dx)
                            if ((dx || dy) && x + dx >= 0 && y + dy >= 0 && x + dx < w && y + dy < h &&
                                blip[static_cast<std::size_t>(y + dy) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x + dx)])
                                ++near;
                    if (!moved && near == 0) ++blips;
                }
            const double ar = std::max<double>(1.0, static_cast<double>(area(b)));
            r.flicker = std::max(r.flicker, static_cast<double>(blips) / ar);
        }
    }
    if (r.flicker > 0.03)
        r.issues.push_back("About " + std::to_string(static_cast<int>(r.flicker * 100)) +
                           "% of the pixels flicker for a single frame; the motion will look noisy.");
    // Ground contact: in a walk, run or idle of a body with feet, some foot
    // stays on the ground line in every frame.
    const bool footed = clip.body == Body::Biped || clip.body == Body::Quadruped;
    if (footed && (clip.name == "walk" || clip.name == "run" || clip.name == "sprint" || clip.name == "idle")) {
        auto lowest = [](const Image& img) {
            for (int y = img.height() - 1; y >= 0; --y)
                for (int x = 0; x < img.width(); ++x)
                    if (img.pixel(x, y).a > 0) return y;
            return -1;
        };
        int ground = -1;
        std::vector<int> rows;
        for (const Image& f : clip.frames) rows.push_back(lowest(f)), ground = std::max(ground, rows.back());
        for (int row : rows) r.floatingFrames += row < ground - clip.footStagger ? 1 : 0;
        if (r.floatingFrames > 0)
            r.issues.push_back("The feet leave the ground in " + std::to_string(r.floatingFrames) + " frame(s); the character will float.");
    }
    // A loop seam is a problem when it jumps more than any step inside the clip.
    if (clip.loops && clip.frames.size() > 2 && r.loopIoU < std::min(0.5, r.minNeighbourIoU - 0.1))
        r.issues.push_back("The last frame does not lead back into the first; the loop will jump.");
    return r;
}

} // namespace acestudio::motion
