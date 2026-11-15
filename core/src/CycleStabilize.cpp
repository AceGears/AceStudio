#include "acestudio/core/CycleStabilize.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>
#include <vector>

namespace acestudio::motion {

namespace {

// What is measured on one pose. Rows and columns are the pose's own.
struct Pose {
    const Image* img = nullptr;
    IntRect bounds;
    int ground = 0;      // lowest opaque row
    int headCx = 0;      // middle column of the head (the top third)
    int height = 0;
    int legRows = -1;    // rows from the leg line to the ground, -1 = not found
};

Pose measure(const Image& img) {
    Pose p;
    p.img = &img;
    p.bounds = img.opaqueBounds();
    const IntRect& bb = p.bounds;
    p.ground = bb.y + bb.h - 1;
    p.height = bb.h;
    int x0 = bb.x + bb.w, x1 = bb.x - 1;
    for (int y = bb.y; y < bb.y + std::max(1, bb.h / 3); ++y)
        for (int x = bb.x; x < bb.x + bb.w; ++x)
            if (img.pixel(x, y).a > 0) {
                x0 = std::min(x0, x);
                x1 = std::max(x1, x);
            }
    p.headCx = x1 >= x0 ? (x0 + x1) / 2 : bb.x + bb.w / 2;
    const Subject s = analyzeSubject(img);
    if (s.legTop >= 0 && s.footRow >= s.legTop) p.legRows = s.footRow - s.legTop + 1;
    return p;
}

// The legs of a pose: the opaque pixels within `slab` rows of the ground
// that hang together with the feet. A hand or a coat tail that reaches below
// the leg line is not connected to the feet's rows and stays out.
Mask legsOf(const Pose& p, int slab, int footBand) {
    const Image& img = *p.img;
    const int w = img.width(), h = img.height();
    Mask legs(w, h);
    const int top = std::max(p.bounds.y, p.ground - slab + 1);
    std::vector<std::pair<int, int>> stack;
    for (int y = std::max(top, p.ground - footBand + 1); y <= p.ground; ++y)
        for (int x = p.bounds.x; x < p.bounds.x + p.bounds.w; ++x)
            if (img.pixel(x, y).a > 0 && !legs.get(x, y)) {
                legs.set(x, y, true);
                stack.push_back({x, y});
            }
    while (!stack.empty()) {
        const auto [x, y] = stack.back();
        stack.pop_back();
        for (int dy = -1; dy <= 1; ++dy)
            for (int dx = -1; dx <= 1; ++dx) {
                const int nx = x + dx, ny = y + dy;
                if (ny < top || ny > p.ground || nx < 0 || nx >= w) continue;
                if (img.pixel(nx, ny).a == 0 || legs.get(nx, ny)) continue;
                legs.set(nx, ny, true);
                stack.push_back({nx, ny});
            }
    }
    return legs;
}

// How far each foot is off the ground: the lowest leg pixel left and right
// of the head's middle column.
std::pair<int, int> footLift(const Pose& p, const Mask& legs) {
    int left = -1, right = -1;
    for (int y = p.ground; y >= p.bounds.y && (left < 0 || right < 0); --y)
        for (int x = p.bounds.x; x < p.bounds.x + p.bounds.w; ++x) {
            if (!legs.get(x, y)) continue;
            if (x < p.headCx && left < 0) left = p.ground - y;
            if (x >= p.headCx && right < 0) right = p.ground - y;
        }
    return {std::max(0, left), std::max(0, right)};
}

// How wide the legs are, feet and all: seen from the side a stride spreads
// them; seen from the front a lifted foot stays where it was.
int spreadOf(const Pose& p, const Mask& legs, int rows) {
    int x0 = 1 << 20, x1 = -1;
    for (int y = std::max(p.bounds.y, p.ground - rows + 1); y <= p.ground; ++y)
        for (int x = p.bounds.x; x < p.bounds.x + p.bounds.w; ++x)
            if (legs.get(x, y)) {
                x0 = std::min(x0, x);
                x1 = std::max(x1, x);
            }
    return x1 >= x0 ? x1 - x0 + 1 : 0;
}

} // namespace

Result<CycleResult> stabilizeCycle(const std::vector<Image>& poses, const CycleOptions& opt) {
    std::vector<Pose> ps;
    for (const Image& img : poses) {
        if (img.isNull() || img.isFullyTransparent())
            return Error(ErrorCode::InvalidArgument, "Every pose needs visible pixels on a transparent background.");
        ps.push_back(measure(img));
    }
    const int n = static_cast<int>(ps.size());
    if (n < 2) return Error(ErrorCode::InvalidArgument, "A walk needs at least two poses.");
    if (opt.master >= n) return Error(ErrorCode::InvalidArgument, "The master pose does not exist.");
    for (int i : opt.order)
        if (i < 0 || i >= n) return Error(ErrorCode::InvalidArgument, "The order names a pose that does not exist.");

    CycleResult res;
    int tallest = 0;
    for (const Pose& p : ps) tallest = std::max(tallest, p.height);

    // The leg line, as rows above the ground: the same in every pose, so the
    // legs of one pose fit under the body of another.
    std::vector<int> found;
    for (const Pose& p : ps)
        if (p.legRows > 0) found.push_back(p.legRows);
    int legRows = tallest / 4;
    if (found.empty()) {
        res.notes.push_back("The legs could not be measured; the lowest quarter of each pose is taken as the legs.");
    } else {
        std::sort(found.begin(), found.end());
        legRows = found[found.size() / 2];
    }
    legRows = std::clamp(legRows, 2, std::max(2, tallest * 3 / 5));
    // The legs' pieces go a little above the leg line, so that no gap opens
    // under an upper body that is drawn a pixel or two higher.
    const int extra = std::max(2, tallest / 40);
    const int slab = legRows + extra;
    const int footBand = std::max(2, legRows * 35 / 100);
    std::vector<Mask> legs;
    for (const Pose& p : ps) legs.push_back(legsOf(p, slab, footBand));

    // Front or side. Seen from the front (or back) a step lifts one foot
    // well off the ground and the legs get no wider; seen from the side a
    // stride spreads them and both feet stay near the ground.
    std::vector<int> spread(static_cast<std::size_t>(n));
    int minSpread = 1 << 20;
    for (int i = 0; i < n; ++i) {
        const auto k = static_cast<std::size_t>(i);
        spread[k] = spreadOf(ps[k], legs[k], legRows);
        minSpread = std::min(minSpread, spread[k]);
        
    }
    const int liftMin = std::max(2, tallest / 16);
    std::vector<int> lifted(static_cast<std::size_t>(n), 0);   // -1 = left foot up, +1 = right foot up
    int widestLifted = 0, widestGrounded = 0;
    for (int i = 0; i < n; ++i) {
        const auto k = static_cast<std::size_t>(i);
        const auto [l, r] = footLift(ps[k], legs[k]);
        lifted[k] = l - r >= liftMin ? -1 : r - l >= liftMin ? 1 : 0;
        (lifted[k] != 0 ? widestLifted : widestGrounded) = std::max(lifted[k] != 0 ? widestLifted : widestGrounded, spread[k]);
    }
    const bool looksFront = widestLifted > 0 && (widestGrounded == 0 || widestLifted <= widestGrounded);
    const bool front = opt.view == CycleView::Front || (opt.view == CycleView::Auto && looksFront);
    res.frontView = front;

    // The upper body of each pose, placed head over head on one ground line,
    // for choosing the master: the pose most like the others.
    const int cut = legRows;   // rows of the legs proper
    auto upperOverlap = [&](const Pose& a, const Pose& b) {
        long inter = 0, uni = 0;
        const int rows = std::min(a.ground - cut - a.bounds.y, b.ground - cut - b.bounds.y);
        const int half = std::max(a.bounds.w, b.bounds.w);
        for (int r = 0; r <= rows; ++r)
            for (int c = -half; c <= half; ++c) {
                const bool p = a.img->pixel(a.headCx + c, a.ground - cut - r).a > 0;
                const bool q = b.img->pixel(b.headCx + c, b.ground - cut - r).a > 0;
                inter += p && q;
                uni += p || q;
            }
        return uni ? static_cast<double>(inter) / static_cast<double>(uni) : 1.0;
    };
    int master = opt.master;
    if (master < 0) {
        double best = -1;
        for (int i = 0; i < n; ++i) {
            double sum = 0;
            for (int j = 0; j < n; ++j)
                if (j != i) sum += upperOverlap(ps[static_cast<std::size_t>(i)], ps[static_cast<std::size_t>(j)]);
            if (sum > best + 1e-9) {
                best = sum;
                master = i;
            }
        }
    }
    res.master = master;
    const Pose& mp = ps[static_cast<std::size_t>(master)];

    // The order: neutral, a step, neutral, the other step.
    std::vector<int> order = opt.order;
    if (order.empty()) {
        std::vector<int> neutral, stepA, stepB;   // front: A = left foot lifted, B = right; side: A = strides
        for (int i = 0; i < n; ++i) {
            const auto k = static_cast<std::size_t>(i);
            if (front) {
                (lifted[k] < 0 ? stepA : lifted[k] > 0 ? stepB : neutral).push_back(i);
            } else {
                (spread[k] * 100 >= minSpread * 112 ? stepA : neutral).push_back(i);
            }
        }
        if (!front && stepA.size() >= 2) {
            // Strides alternate legs in the order they were drawn.
            stepB.assign(stepA.begin() + 1, stepA.end());
            stepA.resize(1);
        }
        const int a = stepA.empty() ? -1 : stepA.front(), b = stepB.empty() ? -1 : stepB.front();
        const int n1 = neutral.empty() ? -1 : neutral.front();
        const int n2 = neutral.size() >= 2 ? neutral[1] : n1;
        if (a >= 0 && b >= 0) {
            if (n1 >= 0) order = {n1, a, n2, b};
            else order = {a, b};
        } else if (a >= 0 || b >= 0) {
            const int s = a >= 0 ? a : b;
            if (n1 >= 0) order = {n1, s};
            else order = {s};
            res.notes.push_back("Only one step pose was found; the walk steps with one leg only.");
        } else {
            for (int i = 0; i < n; ++i) order.push_back(i);
            res.notes.push_back("No step pose was told apart; the poses are played in the order given.");
        }
    }
    res.order = order;

    // The master's upper body: everything above the leg line, and what hangs
    // below it without being legs (hands, a coat).
    const Mask& masterLegs = legs[static_cast<std::size_t>(master)];
    const int bobMax = opt.bob ? std::min(extra, std::max(1, tallest / 50)) : 0;

    // Compose on a roomy canvas (head middle at `ax`, ground at `ay`), then
    // crop all frames to what any of them uses.
    int widest = 0;
    for (const Pose& p : ps) widest = std::max(widest, p.bounds.w);
    const int cw = widest * 3 + 8, ch = tallest + 2 * bobMax + 8;
    const int ax = cw / 2, ay = ch - 3;
    std::vector<Image> frames;
    for (int idx : order) {
        const auto k = static_cast<std::size_t>(idx);
        const Pose& p = ps[k];
        Image f(cw, ch);
        // The pose's legs, its head over the master's head, on the ground.
        for (int y = std::max(p.bounds.y, p.ground - slab + 1); y <= p.ground; ++y)
            for (int x = p.bounds.x; x < p.bounds.x + p.bounds.w; ++x)
                if (legs[k].get(x, y)) f.setPixel(ax + x - p.headCx, ay + y - p.ground, p.img->pixel(x, y));
        // The master's upper body over them, as high as this pose was drawn.
        const int rise = std::clamp(p.height - mp.height, -bobMax, bobMax);
        for (int y = mp.bounds.y; y <= mp.ground; ++y)
            for (int x = mp.bounds.x; x < mp.bounds.x + mp.bounds.w; ++x) {
                const Rgba8 c = mp.img->pixel(x, y);
                if (c.a == 0) continue;
                if (y > mp.ground - cut && masterLegs.get(x, y)) continue;
                f.setPixel(ax + x - mp.headCx, ay + y - mp.ground - rise, c);
            }
        frames.push_back(std::move(f));
    }
    IntRect used;
    for (const Image& f : frames) {
        const IntRect b = f.opaqueBounds();
        if (b.empty()) continue;
        if (used.empty()) {
            used = b;
            continue;
        }
        const int x1 = std::max(used.x + used.w, b.x + b.w), y1 = std::max(used.y + used.h, b.y + b.h);
        used.x = std::min(used.x, b.x);
        used.y = std::min(used.y, b.y);
        used.w = x1 - used.x;
        used.h = y1 - used.y;
    }
    const IntRect box{used.x - 1, used.y - 1, used.w + 2, used.h + 2};
    res.clip.name = "walk";
    res.clip.loops = true;
    res.clip.body = Body::Biped;
    res.clip.frontView = front;
    for (const Image& f : frames) {
        res.clip.frames.push_back(f.copy(box));
        res.clip.durations.push_back(std::max(10, opt.frameMs));
    }
    return res;
}

} // namespace acestudio::motion
