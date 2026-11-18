#include "acestudio/core/DirectionSet.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <set>
#include <utility>

namespace acestudio::directions {

namespace {

struct NamedTurn { const char* key; const char* words; double turn; };
constexpr std::array<NamedTurn, 8> kCompass = {{
    {"s", "front", 0},
    {"se", "front-right", 45},
    {"e", "right", 90},
    {"ne", "back-right", 135},
    {"n", "back", 180},
    {"nw", "back-left", -135},
    {"w", "left", -90},
    {"sw", "front-left", -45},
}};

double angleBetween(double a, double b) { return std::abs(wrapTurn(a - b)); }

Image mirrored(const Image& img) {
    Image out(img.width(), img.height());
    for (int y = 0; y < img.height(); ++y)
        for (int x = 0; x < img.width(); ++x) out.setPixel(img.width() - 1 - x, y, img.pixel(x, y));
    return out;
}

// Opaque bounds over every frame of a clip (the frames of one clip share a
// size and place, so one offset keeps them still against each other).
IntRect unionBounds(const std::vector<Image>& frames) {
    IntRect u;
    bool any = false;
    for (const Image& f : frames) {
        const IntRect b = f.opaqueBounds();
        if (b.empty()) continue;
        if (!any) {
            u = b;
            any = true;
            continue;
        }
        const int x0 = std::min(u.x, b.x), y0 = std::min(u.y, b.y);
        const int x1 = std::max(u.x + u.w, b.x + b.w), y1 = std::max(u.y + u.h, b.y + b.h);
        u = {x0, y0, x1 - x0, y1 - y0};
    }
    return u;
}

// Frames of several groups on one canvas: each group's opaque union centred
// and standing on one row. Returns the canvas size and each group's offset
// (added to a frame's own pixel positions after cropping to the union).
struct Placement {
    int width = 0, height = 0;
    std::vector<IntRect> unions;
    std::vector<std::pair<int, int>> at;   // top-left of each union on the canvas
};
Placement place(const std::vector<const std::vector<Image>*>& groups) {
    constexpr int kPad = 2;
    Placement p;
    int maxW = 1, maxH = 1;
    for (const auto* g : groups) {
        p.unions.push_back(unionBounds(*g));
        maxW = std::max(maxW, p.unions.back().w);
        maxH = std::max(maxH, p.unions.back().h);
    }
    p.width = maxW + 2 * kPad;
    p.height = maxH + 2 * kPad;
    const int ground = p.height - kPad;   // the row below the feet
    for (const IntRect& u : p.unions) p.at.emplace_back((p.width - u.w) / 2, ground - u.h);
    return p;
}

} // namespace

double wrapTurn(double turn) {
    if (!std::isfinite(turn)) return turn;
    double t = std::fmod(turn, 360.0);
    if (t <= -180) t += 360;
    if (t > 180) t -= 360;
    return t;
}

std::optional<double> viewTurn(std::string_view name) {
    std::string n(name);
    for (char& c : n) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    for (const NamedTurn& c : kCompass)
        if (n == c.key || n == c.words) return c.turn;
    return std::nullopt;
}

std::string compassName(double turn) {
    const NamedTurn* best = &kCompass[0];
    for (const NamedTurn& c : kCompass)
        if (angleBetween(turn, c.turn) < angleBetween(turn, best->turn)) best = &c;
    return best->key;
}

std::string directionName(double turn) {
    for (const NamedTurn& c : kCompass)
        if (angleBetween(turn, c.turn) <= 1) return c.key;
    return "turn" + std::to_string(static_cast<int>(std::lround(wrapTurn(turn))));
}

std::vector<double> compassTurns(int count) {
    std::vector<double> out;
    for (const NamedTurn& c : kCompass)
        if (count != 4 || std::fmod(std::abs(c.turn), 90.0) == 0) out.push_back(c.turn);
    return out;
}

Status checkViews(const std::vector<DrawnView>& views) {
    if (views.empty()) return Error(ErrorCode::InvalidArgument, "a direction set needs at least one drawn view");
    if (static_cast<int>(views.size()) > kMaxViews)
        return Error(ErrorCode::InvalidArgument, "a direction set holds at most " + std::to_string(kMaxViews) + " views");
    for (std::size_t i = 0; i < views.size(); ++i) {
        const DrawnView& v = views[i];
        const std::string what = v.name.empty() ? "view " + std::to_string(i + 1) : "the view '" + v.name + "'";
        if (v.name.empty()) return Error(ErrorCode::InvalidArgument, what + " has no name");
        if (!std::isfinite(v.turn)) return Error(ErrorCode::InvalidArgument, what + " has no valid turn");
        if (v.art.isNull() || v.art.isFullyTransparent()) return Error(ErrorCode::InvalidArgument, what + " has no visible pixels");
        for (std::size_t j = 0; j < i; ++j)
            if (angleBetween(views[j].turn, v.turn) < 1)
                return Error(ErrorCode::InvalidArgument, what + " is drawn at the same turn as '" + views[j].name + "'");
    }
    return {};
}

ViewChoice chooseView(const std::vector<DrawnView>& views, double turn, bool allowMirror) {
    ViewChoice best;
    double bestScore = 1e9;
    for (std::size_t i = 0; i < views.size(); ++i)
        for (int m = 0; m < (allowMirror ? 2 : 1); ++m) {
            const double t = wrapTurn(m ? -views[i].turn : views[i].turn);
            // A mirror loses a tie with drawn art.
            const double score = angleBetween(turn, t) + (m ? 1e-6 : 0);
            if (score < bestScore) {
                bestScore = score;
                best = {static_cast<int>(i), m == 1, t, angleBetween(turn, t)};
            }
        }
    return best;
}

Image choiceArt(const std::vector<DrawnView>& views, const ViewChoice& choice) {
    if (choice.view < 0 || choice.view >= static_cast<int>(views.size())) return {};
    const Image& art = views[static_cast<std::size_t>(choice.view)].art;
    return choice.mirrored ? mirrored(art) : art;
}

Result<std::vector<DirectionClip>> retargetDirections(const std::vector<DrawnView>& views, const mocap::Motion& motion,
                                                     const DirectionOptions& opt) {
    if (auto st = checkViews(views); !st) return st.error();
    if (opt.turns.empty()) return Error(ErrorCode::InvalidArgument, "no directions were asked for");
    if (opt.turns.size() > 16) return Error(ErrorCode::InvalidArgument, "at most 16 directions can be made at once");
    for (std::size_t i = 0; i < opt.turns.size(); ++i) {
        if (!std::isfinite(opt.turns[i])) return Error(ErrorCode::InvalidArgument, "a direction's turn is not a number");
        for (std::size_t j = 0; j < i; ++j)
            if (angleBetween(opt.turns[i], opt.turns[j]) < 1)
                return Error(ErrorCode::InvalidArgument, "the direction " + directionName(opt.turns[i]) + " is asked for twice");
    }
    std::vector<DirectionClip> out;
    for (const double asked : opt.turns) {
        DirectionClip d;
        d.turn = wrapTurn(asked);
        d.direction = directionName(d.turn);
        d.choice = chooseView(views, d.turn, opt.allowMirror);
        d.viewName = views[static_cast<std::size_t>(d.choice.view)].name + (d.choice.mirrored ? " (mirrored)" : "");
        mocap::RetargetOptions o = opt.retarget;
        mocap::Camera cam = o.camera.value_or(mocap::Camera{});
        cam.turn = d.turn;
        o.camera = cam;
        o.drawnTurn = d.choice.turn;
        auto r = mocap::retarget(choiceArt(views, d.choice), motion, o);
        if (!r) return Error(r.error().code, "direction " + d.direction + " (view " + d.viewName + "): " + r.error().message);
        d.result = std::move(r).value();
        d.result.clip.name = d.direction;
        d.approximateView = d.result.approximateView;
        if (d.choice.mirrored)
            d.result.notes.push_back("Drawn as the mirror of the view '" + views[static_cast<std::size_t>(d.choice.view)].name +
                                     "': the character's left and right are swapped.");
        out.push_back(std::move(d));
    }
    return out;
}

Result<motion::Clip> turnClip(const std::vector<DrawnView>& views, double fromTurn, double toTurn, double stepDeg, int frameMs,
                              bool allowMirror) {
    if (auto st = checkViews(views); !st) return st.error();
    if (!std::isfinite(fromTurn) || !std::isfinite(toTurn)) return Error(ErrorCode::InvalidArgument, "the turn's ends must be numbers");
    if (!std::isfinite(stepDeg) || stepDeg < 1 || stepDeg > 180)
        return Error(ErrorCode::InvalidArgument, "the turn step must be 1 to 180 degrees");
    if (frameMs < 10 || frameMs > 10000) return Error(ErrorCode::InvalidArgument, "the frame time must be 10 to 10000 ms");
    double delta = wrapTurn(toTurn - fromTurn);
    const int steps = std::max(1, static_cast<int>(std::ceil(std::abs(delta) / stepDeg - 1e-9)));
    std::vector<Image> arts;
    ViewChoice last{-2};
    for (int i = 0; i <= steps; ++i) {
        const ViewChoice c = chooseView(views, fromTurn + delta * i / steps, allowMirror);
        if (c.view == last.view && c.mirrored == last.mirrored) continue;
        arts.push_back(choiceArt(views, c));
        last = c;
    }
    // Every frame is its own group: each view is centred on its own pixels.
    std::vector<std::vector<Image>> single;
    for (const Image& a : arts) single.push_back({a});
    std::vector<const std::vector<Image>*> groups;
    for (const auto& s : single) groups.push_back(&s);
    const Placement p = place(groups);
    motion::Clip clip;
    clip.name = "turn";
    clip.loops = false;
    for (std::size_t i = 0; i < arts.size(); ++i) {
        Image f(p.width, p.height);
        f.blit(arts[i].copy(p.unions[i]), p.at[i].first, p.at[i].second);
        clip.frames.push_back(std::move(f));
        clip.durations.push_back(frameMs);
    }
    return clip;
}

Result<Document> clipsDocument(const std::vector<NamedClip>& clips) {
    if (clips.empty()) return Error(ErrorCode::InvalidArgument, "no clips to put on a document");
    std::set<std::string> names;
    std::size_t total = 0;
    std::vector<const std::vector<Image>*> groups;
    for (const NamedClip& c : clips) {
        if (!c.clip || c.clip->frames.empty()) return Error(ErrorCode::InvalidArgument, "the clip '" + c.tag + "' has no frames");
        if (c.tag.empty() || !names.insert(c.tag).second)
            return Error(ErrorCode::InvalidArgument, "every clip needs its own tag name ('" + c.tag + "')");
        total += c.clip->frames.size();
        groups.push_back(&c.clip->frames);
    }
    if (total > static_cast<std::size_t>(Document::kMaxFrames))
        return Error(ErrorCode::InvalidArgument, "the clips have more frames than a document holds");
    const Placement p = place(groups);
    auto made = Document::create(p.width, p.height);
    if (!made) return made.error();
    Document doc = std::move(made).value();
    auto layer = doc.addLayer("Directions");
    if (!layer) return layer.error();
    int frame = 0;
    for (std::size_t g = 0; g < clips.size(); ++g) {
        const motion::Clip& clip = *clips[g].clip;
        const int from = frame;
        for (std::size_t i = 0; i < clip.frames.size(); ++i, ++frame) {
            const int ms = i < clip.durations.size() ? clip.durations[i] : 100;
            if (frame > 0) {
                auto f = doc.insertFrame(frame, ms);
                if (!f) return f.error();
            } else if (auto st = doc.setFrameDuration(0, ms); !st) {
                return st.error();
            }
            const IntRect u = p.unions[g];
            if (u.empty()) continue;
            const Image part = clip.frames[i].copy(u);
            if (part.isFullyTransparent()) continue;
            if (auto st = doc.setCel(layer.value(), frame, Cel{part, p.at[g].first, p.at[g].second}); !st) return st.error();
        }
        AnimationTag tag;
        tag.name = clips[g].tag;
        tag.fromFrame = from;
        tag.toFrame = frame - 1;
        tag.repeat = clip.loops ? 0 : 1;
        if (auto st = doc.addTag(std::move(tag)); !st) return st.error();
    }
    return doc;
}

} // namespace acestudio::directions
