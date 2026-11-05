#include "acestudio/core/KeyPoses.h"

#include <algorithm>
#include <cmath>
#include <map>

namespace acestudio::keypose {

Plan planFor(motion::Action action, motion::Body body) {
    Plan p;
    (void)body;
    if (action == motion::Action::Flap) {
        // A wingbeat: up (as drawn) -> level on the way down -> down (the
        // power stroke ends) -> rising (wings fold slightly to recover).
        // The downstroke is quicker than the recovery.
        p.name = "flap";
        p.poses = {
            {"up", "", 120, true},
            {"level", "with both wings spread out level to the sides at shoulder height, mid-downstroke", 70},
            {"down", "with both wings swept fully down below its body at the bottom of a powerful downstroke", 100},
            {"rising", "with both wings half raised and slightly folded at the elbows as they rise for the next beat", 90},
        };
        p.anchorFrom = 0.62;
        p.anchorTo = 1.0;
    }
    return p;
}

std::string posePrompt(const std::string& subject, const PoseSpec& pose) {
    const std::string who = subject.empty() ? "creature" : subject;
    return "Pixel art game sprite of the same " + who +
           " as in the reference image, identical colours, markings, proportions and art style, the same side view and the same size, " +
           pose.description + ". Full body, centred, plain white background.";
}

Mask wingMask(const Image& sprite, double line, double from, double to, const Mask& mask) {
    Mask m(sprite.width(), sprite.height());
    const IntRect bb = sprite.opaqueBounds();
    if (bb.empty()) return m;
    const int row = bb.y + static_cast<int>(std::lround(std::clamp(line, 0.05, 0.95) * bb.h));
    const int x0 = bb.x + static_cast<int>(std::floor(std::clamp(std::min(from, to), 0.0, 1.0) * bb.w));
    const int x1 = bb.x + static_cast<int>(std::ceil(std::clamp(std::max(from, to), 0.0, 1.0) * bb.w));
    for (int y = bb.y; y < bb.y + bb.h; ++y)
        for (int x = bb.x; x < bb.x + bb.w; ++x) {
            if (!sprite.pixel(x, y).a) continue;
            const bool w = !mask.isNull() && mask.width() == sprite.width() && mask.height() == sprite.height() ? mask.get(x, y)
                                                                                                                : (y < row && x >= x0 && x < x1);
            m.set(x, y, w);
        }
    return m;
}

InpaintInputs inpaintInputs(const Image& sprite, const Mask& wings, int targetSize) {
    InpaintInputs in;
    const int longest = std::max(sprite.width(), sprite.height());
    in.scale = std::max(1, targetSize / std::max(1, longest));
    const int W = sprite.width() * in.scale, H = sprite.height() * in.scale;
    // Sizes the model accepts: multiples of 16.
    const int cw = (W + 15) / 16 * 16, ch = (H + 15) / 16 * 16;
    const int ox = (cw - W) / 2, oy = (ch - H) / 2;
    in.init = Image(cw, ch, Rgba8{255, 255, 255, 255});
    in.mask = Image(cw, ch, Rgba8{255, 255, 255, 255});
    for (int y = 0; y < sprite.height(); ++y)
        for (int x = 0; x < sprite.width(); ++x) {
            const Rgba8 c = sprite.pixel(x, y);
            if (!c.a) continue;
            const bool body = !(wings.width() == sprite.width() && wings.get(x, y));
            for (int yy = 0; yy < in.scale; ++yy)
                for (int xx = 0; xx < in.scale; ++xx) {
                    const int px = ox + x * in.scale + xx, py = oy + y * in.scale + yy;
                    in.init.setPixel(px, py, Rgba8{c.r, c.g, c.b, 255});
                    if (body) in.mask.setPixel(px, py, Rgba8{0, 0, 0, 255});
                }
        }
    return in;
}

Image reduceToSprite(const Image& raw, const InpaintInputs& in, const Image& sprite) {
    Image out(sprite.width(), sprite.height());
    if (raw.isNull() || in.scale < 1) return out;
    // The model may return a different size: map canvas coordinates.
    const double fx = static_cast<double>(raw.width()) / std::max(1, in.init.width());
    const double fy = static_cast<double>(raw.height()) / std::max(1, in.init.height());
    const int ox = (in.init.width() - sprite.width() * in.scale) / 2, oy = (in.init.height() - sprite.height() * in.scale) / 2;
    std::vector<Rgba8> palette;
    for (const Rgba8& c : sprite.pixels())
        if (c.a && std::find(palette.begin(), palette.end(), c) == palette.end()) palette.push_back(c);
    if (palette.empty()) return out;
    for (int y = 0; y < sprite.height(); ++y)
        for (int x = 0; x < sprite.width(); ++x) {
            const int cx = static_cast<int>((ox + (x + 0.5) * in.scale) * fx), cy = static_cast<int>((oy + (y + 0.5) * in.scale) * fy);
            if (cx < 0 || cy < 0 || cx >= raw.width() || cy >= raw.height()) continue;
            const Rgba8 c = raw.pixel(cx, cy);
            if (c.a < 128) continue;
            const Rgba8* bestC = &palette.front();
            int bestD = 1 << 30;
            for (const Rgba8& p : palette) {
                const int d = (c.r - p.r) * (c.r - p.r) * 3 + (c.g - p.g) * (c.g - p.g) * 4 + (c.b - p.b) * (c.b - p.b) * 2;
                if (d < bestD) { bestD = d; bestC = &p; }
            }
            out.setPixel(x, y, *bestC);
        }
    return out;
}

namespace {

std::uint32_t key(const Rgba8& c) { return (std::uint32_t(c.r) << 16) | (std::uint32_t(c.g) << 8) | c.b; }

double paletteOverlap(const Image& a, const Image& b) {
    // Share of b's opaque pixels whose colour also occurs in a, weighted by count.
    std::map<std::uint32_t, int> ca;
    for (const Rgba8& c : a.pixels())
        if (c.a) ++ca[key(c)];
    long all = 0, shared = 0;
    for (const Rgba8& c : b.pixels())
        if (c.a) { ++all; shared += ca.count(key(c)) ? 1 : 0; }
    return all ? static_cast<double>(shared) / static_cast<double>(all) : 0.0;
}

long opaqueCount(const Image& img) {
    long n = 0;
    for (const Rgba8& c : img.pixels()) n += c.a ? 1 : 0;
    return n;
}

} // namespace

Aligned align(const Image& pose, const Image& original, const Plan& plan, int range) {
    Aligned out;
    const IntRect ob = original.opaqueBounds();
    if (ob.empty() || pose.isNull()) {
        out.image = pose;
        out.issues.push_back("empty pose");
        return out;
    }
    const int y0 = ob.y + static_cast<int>(ob.h * plan.anchorFrom), y1 = ob.y + static_cast<int>(std::ceil(ob.h * plan.anchorTo));
    // Score each offset: anchor pixels of the sprite that the moved pose
    // covers with the same opacity (silhouette) and similar colour.
    double bestScore = -1;
    for (int dy = -range; dy <= range; ++dy)
        for (int dx = -range; dx <= range; ++dx) {
            long both = 0, either = 0;
            for (int y = y0; y < y1; ++y)
                for (int x = ob.x; x < ob.x + ob.w; ++x) {
                    const Rgba8 oc = original.pixel(x, y);
                    const bool a = oc.a > 0;
                    const int sx = x - dx, sy = y - dy;
                    const bool inside = sx >= 0 && sy >= 0 && sx < pose.width() && sy < pose.height();
                    const Rgba8 pc = inside ? pose.pixel(sx, sy) : Rgba8{0, 0, 0, 0};
                    const bool b = pc.a > 0;
                    // A match needs the same thing there: similar colour (a
                    // wing swept over the legs is not the legs).
                    const bool same = a && b && std::abs(oc.r - pc.r) + std::abs(oc.g - pc.g) + std::abs(oc.b - pc.b) < 110;
                    both += same ? 1 : 0;
                    either += (a || b) ? 1 : 0;
                }
            const double iou = either ? static_cast<double>(both) / static_cast<double>(either) : 0.0;
            // Prefer small moves on ties.
            const double score = iou - 0.001 * (std::abs(dx) + std::abs(dy));
            if (score > bestScore) {
                bestScore = score;
                out.dx = dx;
                out.dy = dy;
                out.anchorMatch = iou;
            }
        }
    out.image = Image(original.width(), original.height());
    out.image.blit(pose, out.dx, out.dy);
    // Identity: the same colours, a similar amount of body.
    const double colours = paletteOverlap(original, out.image);
    const double size = static_cast<double>(std::min(opaqueCount(original), opaqueCount(out.image))) /
                        static_cast<double>(std::max<long>(1, std::max(opaqueCount(original), opaqueCount(out.image))));
    out.identity = 0.6 * colours + 0.4 * size;
    if (out.anchorMatch < 0.55) out.issues.push_back("the body does not line up with the sprite (legs or tail moved)");
    if (size < 0.6) out.issues.push_back("the pose is much larger or smaller than the sprite");
    if (colours < 0.8) out.issues.push_back("the pose has colours the sprite does not");
    return out;
}

int restoreBody(Image& pose, const Image& original, const Mask& wings) {
    int restored = 0;
    for (int y = 0; y < original.height() && y < pose.height(); ++y)
        for (int x = 0; x < original.width() && x < pose.width(); ++x) {
            const Rgba8 o = original.pixel(x, y);
            if (!o.a || (wings.width() == original.width() && wings.get(x, y))) continue;
            const Rgba8 p = pose.pixel(x, y);
            const int d = std::abs(o.r - p.r) + std::abs(o.g - p.g) + std::abs(o.b - p.b);
            // Empty (the body was cut away) or a near colour: restore.
            if (!p.a || d < 110) {
                if (p != o) ++restored;
                pose.setPixel(x, y, o);
            }
        }
    return restored;
}

std::size_t best(const std::vector<Aligned>& variants) {
    std::size_t bi = 0;
    double bs = -1;
    for (std::size_t i = 0; i < variants.size(); ++i) {
        const double s = 0.6 * variants[i].anchorMatch + 0.4 * variants[i].identity;
        if (s > bs) { bs = s; bi = i; }
    }
    return bi;
}

motion::Clip assemble(const Plan& plan, const Image& original, const std::vector<Aligned>& generated) {
    motion::Clip c;
    c.name = plan.name;
    c.loops = true;
    c.body = motion::Body::Flyer;
    std::size_t g = 0;
    for (const PoseSpec& p : plan.poses) {
        if (p.original) c.frames.push_back(original);
        else if (g < generated.size()) c.frames.push_back(generated[g++].image);
        else continue;
        c.durations.push_back(p.durationMs);
    }
    return c;
}

} // namespace acestudio::keypose
