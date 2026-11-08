#include "PixelRig.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace acestudio::motion::detail {

namespace {

constexpr double kPi = 3.14159265358979323846;

double smoothstep(double q) { return q * q * (3 - 2 * q); }

int iround(double v) { return static_cast<int>(std::lround(v)); }

// Rows of `len` source rows after `delta` rows are dropped (delta < 0) or
// doubled (delta > 0), spread evenly over [from, to) (indices into the
// source rows). Returns, in order, the source row index of each output row.
std::vector<int> resizeRows(int len, int delta, int from, int to) {
    std::vector<int> rows;
    rows.reserve(static_cast<std::size_t>(std::max(0, len + delta)));
    from = std::clamp(from, 0, len);
    to = std::clamp(to, from, len);
    const int zone = to - from;
    delta = std::clamp(delta, -std::max(0, zone - 1), zone);
    const int n = std::abs(delta);
    std::vector<char> mark(static_cast<std::size_t>(std::max(0, len)), 0);
    for (int k = 0; k < n; ++k) {
        const int r = from + static_cast<int>((k + 0.5) * zone / static_cast<double>(n));
        mark[static_cast<std::size_t>(std::clamp(r, from, to - 1))] = 1;
    }
    for (int r = 0; r < len; ++r) {
        if (delta < 0 && mark[static_cast<std::size_t>(r)]) continue;   // dropped
        rows.push_back(r);
        if (delta > 0 && mark[static_cast<std::size_t>(r)]) rows.push_back(r);   // doubled
    }
    return rows;
}

struct Leg {
    int x0 = 0, x1 = 0;     // the foot's columns
    int hip = 0;            // first row
    int bottom = 0;         // lowest row (the foot's sole)
    int knee = 0;           // first shin row
    int footTop = 0;        // first foot row
    double phase = 0;       // 0..1 offset in the cycle
};

struct LegPose {
    int fx = 0;      // foot offset, px (positive = forward)
    int lift = 0;    // foot raised, px
};

} // namespace

std::optional<RigClip> rigRender(const Image& sprite, const Subject& subj, Body body, Action action, int frames, double amount,
                                 const WingSpec& wing, SwimStyle swim) {
    if (action == Action::Flap) return flapRender(sprite, subj, frames, amount, wing);
    if (body == Body::Swimmer) return swimRender(sprite, subj, action, frames, amount, swim);
    const bool footed = (body == Body::Biped || body == Body::Quadruped) && subj.legTop >= 0 && subj.feet.size() >= 2;
    const bool locomotion = action == Action::Walk || action == Action::Run || action == Action::Sprint;
    if (!footed || !(locomotion || action == Action::Idle) || frames < 1) return std::nullopt;

    const IntRect bb = subj.bounds;
    const int H = bb.h, W = sprite.width();
    amount = std::clamp(amount, 0.25, 2.0);
    // Forward: where the toes point. When the toes do not tell, a sprite
    // that is close to its own mirror image is seen from the front; anything
    // else is taken as facing right.
    const int dir = subj.facing < 0 ? -1 : 1;
    bool front = subj.frontView;
    if (!front && subj.facing == 0) {
        long same = 0, all = 0;
        for (int y = bb.y; y < bb.y + bb.h; ++y)
            for (int x = bb.x; x < bb.x + bb.w; ++x) {
                const bool a = sprite.pixel(x, y).a > 0, b = sprite.pixel(bb.x + bb.x + bb.w - 1 - x, y).a > 0;
                all += (a || b) ? 1 : 0;
                same += (a && b) ? 1 : 0;
            }
        front = all > 0 && same * 10 >= all * 8;
        // One held item (a shield on one arm, a sword in the other hand)
        // breaks the whole sprite's mirror image. A standing biped's legs
        // decide instead: below the hips, between the outer feet, mirrored
        // about the legs' own middle, leaving out the arms' boxes. Feet that
        // end on different rows are a three-quarter stance, not a front view.
        const bool staggered = subj.footBottoms.size() >= 2 &&
                               *std::min_element(subj.footBottoms.begin(), subj.footBottoms.end()) !=
                                   *std::max_element(subj.footBottoms.begin(), subj.footBottoms.end());
        if (!front && body == Body::Biped && subj.feet.size() == 2 && !staggered) {
            const int lx0 = subj.feet.front().first, lx1 = subj.feet.back().second;
            auto solid = [&](int px, int py) {
                if (sprite.pixel(px, py).a == 0) return false;
                for (const IntRect& r : subj.arms)
                    if (px >= r.x && py >= r.y && px < r.x + r.w && py < r.y + r.h) return false;
                return true;
            };
            long legSame = 0, legAll = 0;
            for (int y = subj.legTop; y <= subj.footRow; ++y)
                for (int x = lx0; x <= lx1; ++x) {
                    const bool p = solid(x, y), q = solid(lx0 + lx1 - x, y);
                    legAll += (p || q) ? 1 : 0;
                    legSame += (p && q) ? 1 : 0;
                }
            front = legAll > 0 && legSame * 10 >= legAll * 8;
        }
    }

    // Legs and their geometry.
    std::vector<Leg> legs;
    for (std::size_t i = 0; i < subj.feet.size(); ++i) {
        Leg l;
        l.x0 = subj.feet[i].first;
        l.x1 = subj.feet[i].second;
        l.hip = subj.legTop;
        l.bottom = i < subj.footBottoms.size() ? subj.footBottoms[i] : subj.footRow;
        const int len = l.bottom - l.hip + 1;
        l.knee = l.hip + std::max(1, len / 2);
        l.footTop = l.bottom - std::max(1, iround(len * 0.2)) + 1;
        // Bipeds alternate; four legs move in diagonal pairs.
        if (subj.feet.size() == 4) l.phase = (i == 0 || i == 3) ? 0.0 : 0.5;
        else l.phase = (i % 2 == 0) ? 0.0 : 0.5;
        legs.push_back(l);
    }
    // A biped starts from its drawn pose: the foot drawn further forward is
    // the forward one in the first (contact) frame, so the legs do not cross.
    // Feet are sorted left to right; phase 0 is forward at the start.
    if (legs.size() == 2 && dir > 0) std::swap(legs[0].phase, legs[1].phase);
    const int legLen = subj.footRow - subj.legTop + 1;

    // Which part each pixel belongs to: -2 ground shadow, -1 body, 0.. leg,
    // 100.. arm.
    const int sw = sprite.width(), sh = sprite.height();
    std::vector<int> part(static_cast<std::size_t>(sw) * static_cast<std::size_t>(sh), -1);
    auto at = [&](int x, int y) -> int& { return part[static_cast<std::size_t>(y) * static_cast<std::size_t>(sw) + static_cast<std::size_t>(x)]; };
    const bool hasGround = subj.groundTop >= 0;
    for (int y = bb.y; y < bb.y + bb.h; ++y)
        for (int x = bb.x; x < bb.x + bb.w; ++x) {
            if (sprite.pixel(x, y).a == 0) continue;
            if (hasGround && y >= subj.groundTop) { at(x, y) = -2; continue; }
            bool arm = false;
            if (locomotion)
                for (std::size_t a = 0; a < subj.arms.size(); ++a) {
                    const IntRect& r = subj.arms[a];
                    if (x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h) { at(x, y) = 100 + static_cast<int>(a); arm = true; break; }
                }
            if (arm || y < subj.legTop) continue;
            // A leg pixel lies within (or next to) its foot's columns; other
            // pixels below the hips (a cloak hem, a tail) stay with the body.
            int best = -1, bestD = 2;
            for (std::size_t i = 0; i < legs.size(); ++i) {
                const int d = x < legs[i].x0 ? legs[i].x0 - x : x > legs[i].x1 ? x - legs[i].x1 : 0;
                if (d < bestD) { bestD = d; best = static_cast<int>(i); }
            }
            if (best >= 0) at(x, y) = best;
        }

    // Amplitudes in whole pixels.
    const bool run = action == Action::Run || action == Action::Sprint;
    const int bob = std::max(1, iround(H / 48.0 * amount));
    const int stride = front ? 0 : std::max(2, iround(legLen * (run ? 0.8 : 0.5) * amount));
    const int liftMax = std::max(1, iround(legLen * (run ? 0.35 : 0.22) * amount));

    const int margin = std::max(4, std::max(stride, liftMax) + bob + 2);
    RigClip out;
    out.box = {bb.x - margin, bb.y - margin, bb.w + 2 * margin, bb.h + 2 * margin};

    auto bodyDy = [&](double t) {   // + = lower
        if (action == Action::Idle) {
            // Breathe out (rise) through the middle of the loop.
            return (t >= 0.375 && t < 0.875) ? -bob : 0;
        }
        // Lowest just after each contact, highest at the up pose.
        const double s = std::sin(4 * kPi * t);
        return iround(s * bob * (run ? 1.5 : 1.0));
    };
    auto legPose = [&](const Leg& l, double t) {
        LegPose p;
        if (!locomotion) return p;
        const double q = std::fmod(t + l.phase, 1.0);
        if (q < 0.5) {
            // Planted: moves back at constant speed (the body moves forward).
            p.fx = iround(stride * (0.5 - 2 * q));
        } else {
            const double s = (q - 0.5) * 2;
            p.fx = iround(stride * (-0.5 + smoothstep(s)));
            p.lift = iround(liftMax * std::pow(std::sin(kPi * s), 0.8));
        }
        p.fx *= dir;
        return p;
    };

    // Only an upright body has its head on top; an animal's top rows hold
    // its back and tail, which move with the body.
    const double headLag = body == Body::Biped ? 1.0 / std::max(1, frames) : 0.0;
    const int headBottom = bb.y + iround(H * 0.3);
    const int waist = subj.legTop;

    for (int f = 0; f < frames; ++f) {
        const double t = static_cast<double>(f) / frames;
        Image img(out.box.w, out.box.h);
        auto put = [&](int x, int y, Rgba8 c) {
            const int dx = x - out.box.x, dy = y - out.box.y;
            if (dx >= 0 && dy >= 0 && dx < img.width() && dy < img.height() && c.a) img.setPixel(dx, dy, c);
        };
        const int dy = bodyDy(t);
        const int dyHead = bodyDy(std::fmod(t - headLag + 1.0, 1.0));

        // Ground shadow: stays.
        if (hasGround)
            for (int y = subj.groundTop; y < bb.y + bb.h; ++y)
                for (int x = bb.x; x < bb.x + bb.w; ++x)
                    if (at(x, y) == -2) put(x, y, sprite.pixel(x, y));

        // Legs, far (second group) first.
        std::vector<std::size_t> order(legs.size());
        for (std::size_t i = 0; i < legs.size(); ++i) order[i] = i;
        std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) { return legs[a].phase > legs[b].phase; });
        for (std::size_t li : order) {
            const Leg& l = legs[li];
            const LegPose p = legPose(l, t);
            const int len = l.bottom - l.hip + 1;
            // The hip moves with the body; the sole sits on the ground minus the lift.
            // At least 65 % of its length stays (a deeper lift tears the shin).
            const int destLen = std::max(std::max(2, iround(len * 0.65)), len - p.lift - dy);
            const int delta = destLen - len;
            // Rows change in the thigh and shin, never in the foot.
            const std::vector<int> rows = resizeRows(len, delta, 1, l.footTop - l.hip);
            // Knee: a shorter leg bends, the knee pushes forward.
            const double half = len / 2.0, shorter = std::max(0, len - destLen);
            const int bend = front ? 0 : iround(std::sqrt(std::max(0.0, half * half - (half - shorter / 2.0) * (half - shorter / 2.0))));
            // Knees bend forward; a four-legged animal's hind legs bend back
            // at the hock.
            const bool hind = body == Body::Quadruped && ((l.x0 + l.x1) / 2.0 - (bb.x + bb.w / 2.0)) * dir < 0;
            int kneeX = iround(p.fx / 2.0) + (hind ? -dir : dir) * bend;
            const int kneeRow = l.knee - l.hip, footRow = l.footTop - l.hip;
            // Offsets are spread over the rows actually drawn in each segment,
            // and no row moves more than the leg is wide past the one above,
            // so the leg stays one connected shape.
            int thighRows = 0, shinRows = 0;
            for (int r : rows) {
                if (r < kneeRow) ++thighRows;
                else if (r < footRow) ++shinRows;
            }
            const int maxStep = std::max(1, (l.x1 - l.x0 + 1) / 2);
            kneeX = std::clamp(kneeX, -maxStep * std::max(1, thighRows), maxStep * std::max(1, thighRows));
            const int fx = std::clamp(p.fx, kneeX - maxStep * std::max(1, shinRows), kneeX + maxStep * std::max(1, shinRows));
            int ti = 0, si = 0;
            for (std::size_t k = 0; k < rows.size(); ++k) {
                const int r = rows[k];
                int ox;
                if (r < kneeRow) ox = iround(kneeX * static_cast<double>(ti++) / std::max(1, thighRows));
                else if (r < footRow) ox = kneeX + iround((fx - kneeX) * static_cast<double>(++si) / std::max(1, shinRows));
                else ox = fx;
                const int sy = l.hip + r, ty = l.hip + dy + static_cast<int>(k);
                for (int x = std::max(bb.x, l.x0 - 2); x <= std::min(W - 1, l.x1 + 2); ++x)
                    if (at(x, sy) == static_cast<int>(li)) put(x + ox, ty, sprite.pixel(x, sy));
            }
        }

        // Body: rigid, the head a frame behind; above the waist for an idle.
        for (int y = bb.y; y < bb.y + bb.h; ++y)
            for (int x = bb.x; x < bb.x + bb.w; ++x) {
                if (at(x, y) != -1) continue;
                int o = dy;
                if (action == Action::Idle) o = y < waist ? (y < headBottom ? dyHead : dy) : 0;
                else if (y < headBottom) o = dyHead;
                put(x, y + o, sprite.pixel(x, y));
            }
        // A head that lags higher than the torso leaves a gap at the neck:
        // the head's lowest row is stretched down across it.
        {
            const int torsoTop = dy;   // the torso's offset
            if (dyHead < torsoTop)
                for (int k = 1; k <= torsoTop - dyHead; ++k)
                    for (int x = bb.x; x < bb.x + bb.w; ++x)
                        if (at(x, headBottom - 1) == -1) put(x, headBottom - 1 + dyHead + k, sprite.pixel(x, headBottom - 1));
        }
        // Idle: the gap a risen torso leaves above the waist is filled by
        // doubling the waist row (the belt stretches, nothing tears).
        if (action == Action::Idle && dy < 0)
            for (int k = 0; k < -dy; ++k)
                for (int x = bb.x; x < bb.x + bb.w; ++x)
                    if (at(x, waist - 1) == -1) put(x, waist - 1 - k, sprite.pixel(x, waist - 1));

        // Arms: swing against the leg on their side. An arm moves as one
        // rigid block, by half the hand's swing: it may hold a shield or a
        // sword, and shearing it from the shoulder bent held items into steps
        // in every other frame.
        for (std::size_t a = 0; a < subj.arms.size() && locomotion; ++a) {
            const IntRect& r = subj.arms[a];
            const bool leftArm = r.x + r.w / 2 < bb.x + bb.w / 2;
            // Against the leg on the same side (left arm with the right leg's phase).
            const Leg& opposite = legs[leftArm ? std::min<std::size_t>(1, legs.size() - 1) : 0];
            const LegPose lp = legPose(opposite, t);
            const int hand = front ? 0 : iround(0.6 * lp.fx);
            const int armLift = front ? -iround(0.5 * lp.lift) : 0;
            const int ox = iround(hand / 2.0);
            for (int y = r.y; y < r.y + r.h; ++y)
                for (int x = r.x; x < r.x + r.w; ++x)
                    if (at(x, y) == 100 + static_cast<int>(a)) put(x + ox, y + dy + armLift, sprite.pixel(x, y));
        }
        out.frames.push_back(std::move(img));
    }
    return out;
}

RigClip flapRender(const Image& sprite, const Subject& subj, int frames, double amount, const WingSpec& wing) {
    const IntRect bb = subj.bounds;
    const int sw = sprite.width();
    amount = std::clamp(amount, 0.25, 2.0);
    frames = std::max(2, frames);
    // Wing pixels and the root row they fold about.
    const int rootRow = bb.y + iround(std::clamp(wing.line, 0.05, 0.95) * bb.h);
    const int wx0 = bb.x + static_cast<int>(std::floor(std::clamp(std::min(wing.from, wing.to), 0.0, 1.0) * bb.w));
    const int wx1 = bb.x + static_cast<int>(std::ceil(std::clamp(std::max(wing.from, wing.to), 0.0, 1.0) * bb.w));
    std::vector<char> isWing(static_cast<std::size_t>(sw) * static_cast<std::size_t>(sprite.height()), 0);
    auto wingAt = [&](int x, int y) { return isWing[static_cast<std::size_t>(y) * static_cast<std::size_t>(sw) + static_cast<std::size_t>(x)] != 0; };
    int wingTop = rootRow, root = bb.y;
    for (int y = bb.y; y < bb.y + bb.h; ++y)
        for (int x = bb.x; x < bb.x + bb.w; ++x) {
            if (sprite.pixel(x, y).a == 0) continue;
            const bool w = wing.mask && !wing.mask->isNull() ? wing.mask->get(x, y) : (y < rootRow && x >= wx0 && x < wx1);
            if (!w) continue;
            isWing[static_cast<std::size_t>(y) * static_cast<std::size_t>(sw) + static_cast<std::size_t>(x)] = 1;
            wingTop = std::min(wingTop, y);
            root = std::max(root, y);
        }
    const int R = wing.mask && !wing.mask->isNull() ? root + 1 : rootRow;   // the fold line (first row below the wings)
    const int D = std::max(1, R - wingTop);                                  // wing length in rows
    // How far the wing is open: 1 = as drawn (raised), 0 = the lowest pose.
    // Fast down, slow up; eight different poses.
    auto openness = [](double t) {
        static const double k[] = {1.0, 0.74, 0.23, 0.0, 0.06, 0.26, 0.63, 0.86, 1.0};
        const double x = std::fmod(t + 1.0, 1.0) * 8.0;
        const int i = std::min(7, static_cast<int>(x));
        const double u = smoothstep(x - i);
        return k[i] + (k[i + 1] - k[i]) * u;
    };
    // The wing's projected height, as a fraction of its drawn height. It
    // never drops under 42 % (and two rows): a wing folded flat reads as a
    // line, and one folded past its root hangs upside down below the back.
    const double minFold = std::min(1.0, std::max(0.42, 2.0 / D));
    auto fold = [&](double t) { return minFold + (1.0 - minFold) * openness(t); };
    // A lowered wing is a spread wing: its rows widen about the body's middle
    // (whole columns repeated), so the silhouette keeps most of its area.
    const double spread = 0.65;
    const int cx = bb.x + bb.w / 2;
    const double lag = 1.0 / 16.0;   // the tips trail the root by half a key
    const int lift = std::max(1, iround(bb.h / 32.0 * amount));
    const int margin = D + lift + 4;
    const int side = static_cast<int>(std::ceil(spread * (1.0 - minFold) * (bb.w / 2.0 + 1.0))) + 2;
    RigClip out;
    out.box = {bb.x - side, bb.y - margin, bb.w + 2 * side, bb.h + 2 * margin};
    std::vector<std::pair<int, int>> cols;   // (target column, source column) of one wing row
    for (int f = 0; f < frames; ++f) {
        const double t = static_cast<double>(f) / frames;
        Image img(out.box.w, out.box.h);
        auto put = [&](int x, int y, Rgba8 c) {
            const int dx = x - out.box.x, dy = y - out.box.y;
            if (dx >= 0 && dy >= 0 && dx < img.width() && dy < img.height() && c.a) img.setPixel(dx, dy, c);
        };
        // The body rises on the downstroke (as the wings pass below half open).
        const double o0 = openness(t);
        const int dy = -iround(lift * std::clamp((0.43 - o0) / 0.34, 0.0, 1.0));
        // Wing rows, from the root outwards so the tip's outline ends on top.
        // A row that lands more than one row away from the previous one is
        // drawn across the gap (the membrane stretches; no stripes). Every
        // row stays above the root.
        int prev = R + dy;
        for (int y = R - 1; y >= wingTop; --y) {
            const double d = R - y;   // rows from the root
            const double u = d / D;   // 0 at the root, 1 at the tip
            const double sc = fold(t - lag * u * amount);
            const int ty = R + dy - std::max(1, iround(d * sc));
            const double hs = 1.0 + spread * (1.0 - sc);
            cols.clear();
            for (int x = bb.x; x < bb.x + bb.w; ++x) {
                if (!wingAt(x, y)) continue;
                const int lo = cx + iround((x - cx) * hs), hi = cx + iround((x + 1 - cx) * hs);
                for (int tx = lo; tx < std::max(hi, lo + 1); ++tx) cols.push_back({tx, x});
            }
            const int step = ty < prev ? 1 : -1;
            const int from = std::abs(ty - prev) > 1 ? prev - step : ty;
            for (int yy = from; yy != ty - step; yy -= step)
                for (const auto& [tx, sx] : cols) put(tx, yy, sprite.pixel(sx, y));
            prev = ty;
        }
        // The body (head, neck, back) is drawn over the wings: a wing never
        // hides the head.
        for (int y = bb.y; y < bb.y + bb.h; ++y)
            for (int x = bb.x; x < bb.x + bb.w; ++x)
                if (sprite.pixel(x, y).a && !wingAt(x, y)) put(x, y + dy, sprite.pixel(x, y));
        out.frames.push_back(std::move(img));
    }
    return out;
}

std::optional<RigClip> swimRender(const Image& sprite, const Subject& subj, Action action, int frames, double amount,
                                  SwimStyle style) {
    if (action != Action::Idle && action != Action::Swim && action != Action::Dash) return std::nullopt;
    const IntRect bb = subj.bounds;
    if (bb.empty() || frames < 1) return std::nullopt;
    amount = std::clamp(amount, 0.25, 2.0);
    // Mode from the build: length over depth.
    const double aspect = static_cast<double>(bb.w) / std::max(1, bb.h);
    SwimStyle mode = style;
    if (mode == SwimStyle::Auto)
        mode = aspect >= 3.2 ? SwimStyle::Anguilliform : aspect <= 1.6 ? SwimStyle::Carangiform : SwimStyle::Subcarangiform;
    using Mode = SwimStyle;
    // Tail amplitude in pixels and beats per loop.
    const double tailAmp = bb.h * (action == Action::Dash ? 0.16 : action == Action::Swim ? 0.11 : 0.05) * amount;
    // Thunniform swimmers beat fast; everyone beats faster in a dash.
    const int beats = (action == Action::Dash ? 2 : 1) * (mode == Mode::Thunniform ? 2 : 1);
    auto envelope = [&](double u) {   // 0 at the head, 1 at the tail
        switch (mode) {
        case Mode::Anguilliform: return 0.3 + 0.7 * u;
        case Mode::Carangiform: return std::pow(std::max(0.0, (u - 0.45) / 0.55), 2.0);
        case Mode::Thunniform: return std::pow(std::max(0.0, (u - 0.72) / 0.28), 1.5);
        case Mode::Subcarangiform:
        case Mode::Auto: break;
        }
        return std::pow(std::max(0.0, (u - 0.2) / 0.8), 1.5);
    };
    const double wavelength = mode == Mode::Anguilliform ? 0.7 : mode == Mode::Carangiform ? 1.25 : mode == Mode::Thunniform ? 1.6 : 1.0;
    // A fish bends sideways, towards and away from a side-view camera. Seen
    // from the side that shows as foreshortening: where the body turns
    // towards the viewer its columns crowd together (whole columns are
    // dropped, never resampled). Games look at water from slightly above, so
    // the sideways bend also shows as a small up-and-down wave.
    const double lateral = bb.w * (action == Action::Dash ? 0.16 : action == Action::Swim ? 0.11 : 0.05) * amount;
    const double vertical = std::min(0.3 * lateral, tailAmp);
    const int margin = static_cast<int>(std::ceil(vertical)) + 4;
    const int dir = subj.headLeft ? 1 : -1;   // from the head towards the tail
    const int headX = subj.headLeft ? bb.x : bb.x + bb.w - 1;
    RigClip out;
    out.box = {bb.x - margin, bb.y - margin, bb.w + 2 * margin, bb.h + 2 * margin};
    std::vector<double> z(static_cast<std::size_t>(bb.w));
    std::vector<int> fromCol;
    for (int f = 0; f < frames; ++f) {
        const double t = static_cast<double>(f) / frames;
        Image img(out.box.w, out.box.h);
        // A dash lunges forward and glides back; an idle hovers.
        const int forward = action == Action::Dash ? iround(bb.w * 0.08 * amount * std::sin(kPi * t)) * -dir : 0;
        const int hover = action == Action::Idle ? iround(std::max(1.0, bb.h / 40.0) * std::sin(2 * kPi * t)) : 0;
        // Sideways bend of each column, counted from the head (k = 0).
        for (int k = 0; k < bb.w; ++k) {
            const double u = static_cast<double>(k) / std::max(1, bb.w - 1);
            // The tail fin (last eighth) trails the body by a tenth of a beat.
            const double lag = u > 0.875 ? 0.1 * (u - 0.875) / 0.125 : 0.0;
            const double phase = 2 * kPi * (beats * t - lag) - 2 * kPi * u / wavelength;
            z[static_cast<std::size_t>(k)] = lateral * envelope(u) * std::sin(phase);
        }
        // Where each column lands: one step per column, shortened by the
        // bend. Each landing column shows exactly one source column (the
        // one nearest the tail), so no two columns mix.
        fromCol.assign(static_cast<std::size_t>(bb.w), -1);
        double pos = 0;
        int last = 0;
        for (int k = 0; k < bb.w; ++k) {
            if (k > 0) {
                const double dz = z[static_cast<std::size_t>(k)] - z[static_cast<std::size_t>(k - 1)];
                pos += 1.0 / std::sqrt(1.0 + dz * dz);
            }
            last = std::clamp(iround(pos), 0, bb.w - 1);
            fromCol[static_cast<std::size_t>(last)] = k;
        }
        for (int d = 0; d <= last; ++d) {
            const int k = fromCol[static_cast<std::size_t>(d)];
            if (k < 0) continue;
            const int sx = headX + dir * k;
            const int dy = iround(vertical / std::max(1e-9, lateral) * z[static_cast<std::size_t>(k)]) + hover;
            const int tx = headX + dir * d + forward - out.box.x;
            for (int y = bb.y; y < bb.y + bb.h; ++y) {
                const Rgba8 c = sprite.pixel(sx, y);
                if (!c.a) continue;
                const int ty = y + dy - out.box.y;
                if (tx >= 0 && ty >= 0 && tx < img.width() && ty < img.height()) img.setPixel(tx, ty, c);
            }
        }
        out.frames.push_back(std::move(img));
    }
    return out;
}

} // namespace acestudio::motion::detail
