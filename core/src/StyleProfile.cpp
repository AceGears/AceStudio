#include "acestudio/core/StyleProfile.h"

#include "acestudio/core/FileUtil.h"
#include "acestudio/core/PaletteIO.h"
#include "acestudio/core/PngCodec.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <numeric>
#include <set>

namespace acestudio::style {

using nlohmann::json;
namespace fs = std::filesystem;

namespace {

struct Hsv { double h, s, v; };
Hsv toHsv(Rgba8 c) {
    const double r = c.r / 255.0, g = c.g / 255.0, b = c.b / 255.0;
    const double mx = std::max({r, g, b}), mn = std::min({r, g, b}), d = mx - mn;
    double h = 0;
    if (d > 1e-9) {
        if (mx == r) h = std::fmod((g - b) / d, 6.0);
        else if (mx == g) h = (b - r) / d + 2;
        else h = (r - g) / d + 4;
        h *= 60;
        if (h < 0) h += 360;
    }
    return {h, mx > 0 ? d / mx : 0, mx};
}
double luma(Rgba8 c) { return (0.299 * c.r + 0.587 * c.g + 0.114 * c.b) / 255.0; }
bool sameRgb(Rgba8 a, Rgba8 b) { return a.r == b.r && a.g == b.g && a.b == b.b && (a.a >= 128) == (b.a >= 128); }

// The art's pixel size: the largest k in 2..8 such that ~all colour
// boundaries fall on one grid (x mod k and y mod k each take one value).
// Run lengths alone are not enough: a symmetric shape has even widths.
int detectPixelScale(const Image& img) {
    std::vector<int> xs, ys;   // boundary positions
    const int stepY = std::max(1, img.height() / 96), stepX = std::max(1, img.width() / 96);
    for (int y = 0; y < img.height(); y += stepY)
        for (int x = 1; x < img.width(); ++x)
            if (!sameRgb(img.pixel(x, y), img.pixel(x - 1, y))) xs.push_back(x);
    for (int x = 0; x < img.width(); x += stepX)
        for (int y = 1; y < img.height(); ++y)
            if (!sameRgb(img.pixel(x, y), img.pixel(x, y - 1))) ys.push_back(y);
    if (xs.size() + ys.size() < 40) return 1;
    auto aligned = [](const std::vector<int>& pos, int k) {
        if (pos.empty()) return true;
        std::vector<int> hist(static_cast<std::size_t>(k), 0);
        for (int p : pos) ++hist[static_cast<std::size_t>(p % k)];
        const int top = *std::max_element(hist.begin(), hist.end());
        return static_cast<double>(top) / static_cast<double>(pos.size()) >= 0.9;
    };
    for (int k = 8; k >= 2; --k)
        if (aligned(xs, k) && aligned(ys, k)) return k;
    return 1;
}

Image downsample(const Image& img, int k) {
    if (k <= 1) return img;
    Image out(std::max(1, img.width() / k), std::max(1, img.height() / k));
    for (int y = 0; y < out.height(); ++y)
        for (int x = 0; x < out.width(); ++x) out.setPixel(x, y, img.pixel(x * k + k / 2, y * k + k / 2));
    return out;
}

} // namespace

const char* toString(RefRole r) {
    switch (r) {
    case RefRole::Character: return "character";
    case RefRole::Style: return "style";
    case RefRole::Palette: return "palette";
    case RefRole::Environment: return "environment";
    case RefRole::Creature: return "creature";
    case RefRole::Equipment: return "equipment";
    case RefRole::Animation: return "animation";
    case RefRole::Composition: return "composition";
    }
    return "style";
}

std::optional<RefRole> refRoleFromString(std::string_view s) {
    for (RefRole r : {RefRole::Character, RefRole::Style, RefRole::Palette, RefRole::Environment, RefRole::Creature,
                      RefRole::Equipment, RefRole::Animation, RefRole::Composition})
        if (s == toString(r)) return r;
    return std::nullopt;
}

const char* roleLabel(RefRole r) {
    switch (r) {
    case RefRole::Character: return "Character";
    case RefRole::Style: return "Art style";
    case RefRole::Palette: return "Colour palette";
    case RefRole::Environment: return "Environment";
    case RefRole::Creature: return "Creature";
    case RefRole::Equipment: return "Equipment";
    case RefRole::Animation: return "Pose / animation";
    case RefRole::Composition: return "Composition";
    }
    return "Art style";
}

StyleFeatures analyze(const Image& src) {
    StyleFeatures f;
    if (src.isNull()) return f;
    f.pixelScale = detectPixelScale(src);
    const Image img = downsample(src, f.pixelScale);
    f.sampleCount = 1;
    f.palette = extractPalette(img, 32);
    const int W = img.width(), H = img.height();
    bool hasAlpha = false;
    for (const Rgba8 p : img.pixels())
        if (p.a < 128) { hasAlpha = true; break; }
    auto opaque = [&](int x, int y) { return x >= 0 && y >= 0 && x < W && y < H && img.pixel(x, y).a >= 128; };

    // Colour statistics and the tones per hue band.
    std::array<std::set<int>, 13> tones;   // 12 hue bands + greys
    std::array<long, 13> bandCount{};
    double sumS = 0, sumV = 0, sumL = 0, wx = 0, wy = 0;
    long n = 0, edges = 0;
    int minX = W, minY = H, maxX = -1, maxY = -1;
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            if (!opaque(x, y)) continue;
            const Rgba8 p = img.pixel(x, y);
            const Hsv h = toHsv(p);
            // Near-black pixels are ink (outlines), not shading tones.
            if (h.v >= 0.15) {
                const int band = h.s < 0.15 ? 12 : static_cast<int>(h.h / 30.0) % 12;
                tones[static_cast<std::size_t>(band)].insert(static_cast<int>(h.v * 15.99));
                ++bandCount[static_cast<std::size_t>(band)];
            }
            sumS += h.s;
            sumV += h.v;
            const double l = luma(p);
            sumL += l;
            wx += l * x;
            wy += l * y;
            ++n;
            minX = std::min(minX, x); maxX = std::max(maxX, x);
            minY = std::min(minY, y); maxY = std::max(maxY, y);
            if ((opaque(x + 1, y) && !sameRgb(img.pixel(x + 1, y), p)) || (opaque(x, y + 1) && !sameRgb(img.pixel(x, y + 1), p))) ++edges;
        }
    if (n == 0) return f;
    const double nd = static_cast<double>(n);
    f.saturation = sumS / nd;
    f.value = sumV / nd;
    f.detail = std::min(1.0, static_cast<double>(edges) / nd);
    // Tones per hue band, weighted by how much of the image each band covers.
    double toneSum = 0, weight = 0;
    for (std::size_t b = 0; b < tones.size(); ++b)
        if (static_cast<double>(bandCount[b]) >= 0.02 * nd) {
            toneSum += static_cast<double>(tones[b].size()) * static_cast<double>(bandCount[b]);
            weight += static_cast<double>(bandCount[b]);
        }
    f.tonesPerHue = weight > 0 ? toneSum / weight : 0;
    f.shading = f.tonesPerHue <= 2.0 ? Shading::Flat : f.tonesPerHue <= 4.5 ? Shading::Cel : Shading::Soft;

    // Light direction: where the luma-weighted centre lies relative to the shape's centre.
    if (sumL > 0 && maxX > minX && maxY > minY) {
        double cx = 0, cy = 0;
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x)
                if (opaque(x, y)) { cx += x; cy += y; }
        cx /= nd;
        cy /= nd;
        const double lx = (wx / sumL - cx) / ((maxX - minX) / 2.0 + 1), ly = (wy / sumL - cy) / ((maxY - minY) / 2.0 + 1);
        f.lightX = std::clamp(lx * 8, -1.0, 1.0);
        f.lightY = std::clamp(ly * 8, -1.0, 1.0);
    }

    // Outline: the subject's border pixels (sprites with transparency only).
    if (hasAlpha) {
        long border = 0, dark = 0, darker = 0;
        const double meanL = sumL / nd;
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) {
                if (!opaque(x, y)) continue;
                if (opaque(x - 1, y) && opaque(x + 1, y) && opaque(x, y - 1) && opaque(x, y + 1)) continue;
                ++border;
                const double l = luma(img.pixel(x, y));
                if (l < 0.22 && l < meanL * 0.6) ++dark;
                // Darker than the pixel just inside?
                double inner = -1;
                for (auto [dx, dy] : {std::pair{1, 0}, std::pair{-1, 0}, std::pair{0, 1}, std::pair{0, -1}})
                    if (opaque(x + dx, y + dy) && opaque(x - dx, y - dy) == false) inner = luma(img.pixel(x + dx, y + dy));
                if (inner >= 0 && l < inner * 0.8) ++darker;
            }
        if (border > 0) {
            f.outlineRatio = static_cast<double>(dark) / static_cast<double>(border);
            if (f.outlineRatio > 0.55) f.outline = Outline::Dark;
            else if (static_cast<double>(darker) / static_cast<double>(border) > 0.55) f.outline = Outline::Coloured;
        }
    }
    return f;
}

StyleFeatures merge(const std::vector<StyleFeatures>& parts) {
    StyleFeatures m;
    if (parts.empty()) return m;
    std::array<int, 3> outlines{}, shadings{};
    Image swatches(static_cast<int>(parts.size()) * 32, 1);
    for (std::size_t i = 0; i < parts.size(); ++i) {
        const StyleFeatures& p = parts[i];
        m.tonesPerHue += p.tonesPerHue;
        m.lightX += p.lightX;
        m.lightY += p.lightY;
        m.saturation += p.saturation;
        m.value += p.value;
        m.detail += p.detail;
        m.outlineRatio += p.outlineRatio;
        m.pixelScale = std::max(m.pixelScale, p.pixelScale);
        m.sampleCount += p.sampleCount;
        ++outlines[static_cast<std::size_t>(p.outline)];
        ++shadings[static_cast<std::size_t>(p.shading)];
        for (std::size_t c = 0; c < p.palette.size() && c < 32; ++c) swatches.setPixel(static_cast<int>(i * 32 + c), 0, p.palette.at(c));
    }
    const double k = static_cast<double>(parts.size());
    m.tonesPerHue /= k;
    m.lightX /= k;
    m.lightY /= k;
    m.saturation /= k;
    m.value /= k;
    m.detail /= k;
    m.outlineRatio /= k;
    m.outline = static_cast<Outline>(std::max_element(outlines.begin(), outlines.end()) - outlines.begin());
    m.shading = static_cast<Shading>(std::max_element(shadings.begin(), shadings.end()) - shadings.begin());
    m.palette = extractPalette(swatches, 32);
    return m;
}

std::string describe(const StyleFeatures& f) {
    std::vector<std::string> w;
    if (f.outline == Outline::Dark) w.push_back("dark 1-pixel outline");
    else if (f.outline == Outline::Coloured) w.push_back("coloured outline (darker shade of each colour)");
    else if (f.sampleCount) w.push_back("no outline");
    const int tones = static_cast<int>(std::lround(f.tonesPerHue));
    if (f.shading == Shading::Flat) w.push_back("flat colours with minimal shading");
    else if (f.shading == Shading::Cel) w.push_back("cel shading with " + std::to_string(std::max(2, tones)) + " tones per colour");
    else w.push_back("smooth multi-tone shading");
    if (std::abs(f.lightX) > 0.15 || std::abs(f.lightY) > 0.15) {
        std::string dir = "light from the ";
        dir += f.lightY < -0.15 ? "top" : f.lightY > 0.15 ? "bottom" : "";
        if ((f.lightY < -0.15 || f.lightY > 0.15) && std::abs(f.lightX) > 0.15) dir += "-";
        dir += f.lightX < -0.15 ? "left" : f.lightX > 0.15 ? "right" : "";
        w.push_back(dir);
    }
    if (f.saturation < 0.3) w.push_back("muted desaturated colours");
    else if (f.saturation > 0.6) w.push_back("vivid saturated colours");
    if (f.value < 0.35) w.push_back("dark moody tones");
    else if (f.value > 0.75) w.push_back("bright tones");
    if (f.detail > 0.45) w.push_back("high detail");
    else if (f.detail < 0.2 && f.sampleCount) w.push_back("simple shapes, low detail");
    if (f.palette.size() > 0 && f.palette.size() <= 16) w.push_back("limited " + std::to_string(f.palette.size()) + "-colour palette");
    std::string out;
    for (const std::string& s : w) out += (out.empty() ? "" : ", ") + s;
    return out;
}

double styleSimilarity(const StyleFeatures& a, const StyleFeatures& b) {
    double pal = 1.0;
    if (a.palette.size() && b.palette.size()) {
        double sum = 0;
        for (std::size_t i = 0; i < b.palette.size(); ++i) {
            const Rgba8 p = b.palette.at(i);
            double best = 1e9;
            for (std::size_t j = 0; j < a.palette.size(); ++j) {
                const Rgba8 q = a.palette.at(j);
                best = std::min(best, std::sqrt(std::pow(p.r - q.r, 2) + std::pow(p.g - q.g, 2) + std::pow(p.b - q.b, 2)) / 441.673);
            }
            sum += best;
        }
        pal = std::max(0.0, 1.0 - 4.0 * sum / static_cast<double>(b.palette.size()));
    }
    const double outline = a.outline == b.outline ? 1.0 : 0.4;
    const double shading = std::max(0.0, 1.0 - std::abs(a.tonesPerHue - b.tonesPerHue) / 4.0);
    const double sat = std::max(0.0, 1.0 - std::abs(a.saturation - b.saturation) * 2.5);
    const double val = std::max(0.0, 1.0 - std::abs(a.value - b.value) * 2.5);
    return 0.4 * pal + 0.15 * outline + 0.15 * shading + 0.15 * sat + 0.15 * val;
}

json StyleFeatures::toJson() const {
    json pal = json::array();
    for (std::size_t i = 0; i < palette.size(); ++i) pal.push_back(palette.at(i).toHex(false));
    return {{"palette", pal}, {"pixelScale", pixelScale}, {"outline", outline == Outline::Dark ? "dark" : outline == Outline::Coloured ? "coloured" : "none"},
            {"outlineRatio", outlineRatio}, {"shading", shading == Shading::Flat ? "flat" : shading == Shading::Cel ? "cel" : "soft"},
            {"tonesPerHue", tonesPerHue}, {"lightX", lightX}, {"lightY", lightY}, {"saturation", saturation}, {"value", value},
            {"detail", detail}, {"samples", sampleCount}};
}

StyleFeatures StyleFeatures::fromJson(const json& j) {
    StyleFeatures f;
    if (!j.is_object()) return f;
    if (j.contains("palette") && j["palette"].is_array())
        for (const auto& c : j["palette"])
            if (c.is_string())
                if (auto rgba = Rgba8::fromHex(c.get<std::string>())) (void)f.palette.add(*rgba);
    f.pixelScale = std::clamp(j.value("pixelScale", 1), 1, 16);
    const std::string o = j.value("outline", std::string("none"));
    f.outline = o == "dark" ? Outline::Dark : o == "coloured" ? Outline::Coloured : Outline::None;
    f.outlineRatio = j.value("outlineRatio", 0.0);
    const std::string s = j.value("shading", std::string("flat"));
    f.shading = s == "cel" ? Shading::Cel : s == "soft" ? Shading::Soft : Shading::Flat;
    f.tonesPerHue = j.value("tonesPerHue", 0.0);
    f.lightX = j.value("lightX", 0.0);
    f.lightY = j.value("lightY", 0.0);
    f.saturation = j.value("saturation", 0.0);
    f.value = j.value("value", 0.0);
    f.detail = j.value("detail", 0.0);
    f.sampleCount = j.value("samples", 0);
    return f;
}

json StyleProfile::toJson() const {
    json refs = json::array();
    for (const auto& r : references) refs.push_back({{"file", r.file}, {"role", toString(r.role)}});
    return {{"format", "acestudio.style"}, {"version", 1}, {"id", id}, {"name", name}, {"perspective", perspective},
            {"notes", notes}, {"strength", strength}, {"variation", variation}, {"references", refs}, {"features", features.toJson()}};
}

Result<StyleProfile> StyleProfile::fromJson(const json& j) {
    if (!j.is_object() || j.value("format", std::string()) != "acestudio.style")
        return Error{ErrorCode::CorruptData, "Not an AceStudio style profile"};
    StyleProfile p;
    p.id = j.value("id", std::string());
    p.name = j.value("name", std::string());
    p.perspective = j.value("perspective", std::string());
    p.notes = j.value("notes", std::string());
    p.strength = std::clamp(j.value("strength", 2), 1, 3);
    p.variation = std::clamp(j.value("variation", 1), 0, 2);
    if (j.contains("references") && j["references"].is_array())
        for (const auto& r : j["references"]) {
            ProfileReference ref;
            ref.file = r.value("file", std::string());
            // Only plain file names: profiles are user files that may come from elsewhere.
            if (ref.file.empty() || ref.file.find_first_of("/\\:") != std::string::npos || ref.file.rfind("..", 0) == 0) continue;
            ref.role = refRoleFromString(r.value("role", std::string("style"))).value_or(RefRole::Style);
            p.references.push_back(ref);
        }
    p.features = StyleFeatures::fromJson(j.value("features", json::object()));
    if (p.id.empty() || p.name.empty()) return Error{ErrorCode::CorruptData, "A style profile needs an id and a name"};
    return p;
}

// ------------------------------------------------------------------ library

namespace {
std::string slug(const std::string& name) {
    std::string s;
    for (const char ch : name) {
        const auto c = static_cast<unsigned char>(ch);
        if (std::isalnum(c)) s += static_cast<char>(std::tolower(c));
        else if (!s.empty() && s.back() != '-') s += '-';
    }
    while (!s.empty() && s.back() == '-') s.pop_back();
    return s.empty() ? std::string("style") : s.substr(0, 40);
}
bool safeId(const std::string& id) {
    return !id.empty() && id.size() <= 64 && std::all_of(id.begin(), id.end(), [](unsigned char c) { return std::isalnum(c) || c == '-'; });
}
} // namespace

std::vector<StyleProfile> StyleLibrary::list() const {
    std::vector<StyleProfile> out;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(m_dir, ec)) {
        if (!e.is_directory(ec)) continue;
        if (auto p = load(e.path().filename().string())) out.push_back(std::move(p).value());
    }
    std::sort(out.begin(), out.end(), [](const StyleProfile& a, const StyleProfile& b) { return a.name < b.name; });
    return out;
}

Result<StyleProfile> StyleLibrary::load(const std::string& id) const {
    if (!safeId(id)) return Error{ErrorCode::InvalidArgument, "Invalid style id"};
    auto bytes = readFileBytes(m_dir / id / "profile.json", 1u << 20);
    if (!bytes) return bytes.error();
    const json j = json::parse(bytes.value().begin(), bytes.value().end(), nullptr, false);
    auto p = StyleProfile::fromJson(j);
    if (!p) return p.error();
    if (p.value().id != id) return Error{ErrorCode::CorruptData, "Style profile id does not match its folder"};
    return p;
}

Status StyleLibrary::save(const StyleProfile& p) const {
    if (!safeId(p.id)) return Error{ErrorCode::InvalidArgument, "Invalid style id"};
    std::error_code ec;
    fs::create_directories(m_dir / p.id, ec);
    if (ec) return Error{ErrorCode::IoError, "Cannot create " + (m_dir / p.id).string() + ": " + ec.message()};
    const std::string text = p.toJson().dump(2);
    return writeFileAtomic(m_dir / p.id / "profile.json", std::span(reinterpret_cast<const std::uint8_t*>(text.data()), text.size()));
}

Result<StyleProfile> StyleLibrary::create(const std::string& name, const std::vector<std::pair<Image, RefRole>>& refs) {
    if (name.empty() || name.size() > 80) return Error{ErrorCode::InvalidArgument, "A style needs a name of up to 80 characters"};
    StyleProfile p;
    p.name = name;
    std::string id = slug(name);
    std::error_code ec;
    for (int i = 2; fs::exists(m_dir / id, ec); ++i) id = slug(name) + "-" + std::to_string(i);
    p.id = id;
    if (auto st = save(p); !st) return st.error();
    return addReferences(p.id, refs);
}

Result<StyleProfile> StyleLibrary::addReferences(const std::string& id, const std::vector<std::pair<Image, RefRole>>& refs) {
    auto p = load(id);
    if (!p) return p.error();
    StyleProfile prof = std::move(p).value();
    for (const auto& [img, role] : refs) {
        if (img.isNull()) continue;
        if (prof.references.size() >= 16) return Error{ErrorCode::InvalidArgument, "A style holds up to 16 references"};
        std::string file;
        std::error_code ec;
        for (int i = static_cast<int>(prof.references.size()) + 1;; ++i) {
            file = "ref-" + std::to_string(i) + ".png";
            if (!fs::exists(m_dir / id / file, ec)) break;
        }
        if (auto st = writePng(img, m_dir / id / file); !st) return st.error();
        prof.references.push_back({file, role});
    }
    if (auto st = save(prof); !st) return st.error();
    return remeasure(id);
}

Result<StyleProfile> StyleLibrary::remeasure(const std::string& id) const {
    auto p = load(id);
    if (!p) return p.error();
    StyleProfile prof = std::move(p).value();
    std::vector<StyleFeatures> parts;
    for (const auto& r : prof.references) {
        // Character, creature and equipment references show subjects, not the
        // style: they do not change the measured style.
        if (r.role == RefRole::Character || r.role == RefRole::Creature || r.role == RefRole::Equipment || r.role == RefRole::Animation)
            continue;
        auto img = referenceImage(prof, r);
        if (img) parts.push_back(analyze(img.value()));
    }
    prof.features = merge(parts);
    if (auto st = save(prof); !st) return st.error();
    return prof;
}

Status StyleLibrary::remove(const std::string& id) const {
    if (!safeId(id)) return Error{ErrorCode::InvalidArgument, "Invalid style id"};
    std::error_code ec;
    fs::remove_all(m_dir / id, ec);
    if (ec) return Error{ErrorCode::IoError, "Cannot remove the style: " + ec.message()};
    return {};
}

Result<Image> StyleLibrary::referenceImage(const StyleProfile& p, const ProfileReference& r) const {
    if (!safeId(p.id)) return Error{ErrorCode::InvalidArgument, "Invalid style id"};
    return readPng(m_dir / p.id / r.file);
}

} // namespace acestudio::style
