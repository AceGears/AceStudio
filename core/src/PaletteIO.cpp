#include "acestudio/core/PaletteIO.h"
#include "acestudio/core/FileUtil.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cctype>
#include <cstdio>
#include <sstream>
#include <unordered_set>
#include <vector>

namespace fs = std::filesystem;

namespace acestudio {

namespace {

std::string lower(std::string_view s) {
    std::string out(s);
    for (auto& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

std::string_view trim(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
    return s;
}

std::vector<std::string_view> lines(std::string_view text) {
    std::vector<std::string_view> out;
    while (!text.empty()) {
        const auto nl = text.find('\n');
        out.push_back(trim(text.substr(0, nl)));
        if (nl == std::string_view::npos) break;
        text.remove_prefix(nl + 1);
    }
    return out;
}

Error bad(const std::string& m) { return Error{ErrorCode::CorruptData, m}; }

bool parseInts(std::string_view line, int (&v)[3]) {
    std::istringstream in{std::string(line)};
    return static_cast<bool>(in >> v[0] >> v[1] >> v[2]) && v[0] >= 0 && v[0] <= 255 && v[1] >= 0 &&
           v[1] <= 255 && v[2] >= 0 && v[2] <= 255;
}

Result<Palette> parseGpl(std::string_view text) {
    auto ls = lines(text);
    if (ls.empty() || ls[0] != "GIMP Palette") return bad("Not a GIMP palette (missing header)");
    Palette p("Imported");
    for (std::size_t i = 1; i < ls.size(); ++i) {
        const auto l = ls[i];
        if (l.empty() || l[0] == '#') continue;
        if (l.rfind("Name:", 0) == 0) { p.setName(std::string(trim(l.substr(5)))); continue; }
        if (l.rfind("Columns:", 0) == 0) continue;
        int v[3];
        if (!parseInts(l, v)) return bad("Invalid colour on line " + std::to_string(i + 1));
        if (!p.add(Rgba8::opaque(static_cast<std::uint8_t>(v[0]), static_cast<std::uint8_t>(v[1]), static_cast<std::uint8_t>(v[2]))))
            return bad("Palette has more than 256 colours");
    }
    return p;
}

Result<Palette> parseJasc(std::string_view text) {
    auto ls = lines(text);
    if (ls.size() < 3 || ls[0] != "JASC-PAL") return bad("Not a JASC palette (missing header)");
    if (ls[1] != "0100") return bad("Unsupported JASC palette version");
    int count = 0;
    try { count = std::stoi(std::string(ls[2])); } catch (...) { return bad("Invalid JASC colour count"); }
    if (count < 0 || count > 256) return bad("JASC palette colour count out of range");
    if (ls.size() < static_cast<std::size_t>(3 + count)) return bad("JASC palette is truncated");
    Palette p("Imported");
    for (int i = 0; i < count; ++i) {
        int v[3];
        if (!parseInts(ls[static_cast<std::size_t>(3 + i)], v)) return bad("Invalid colour on line " + std::to_string(4 + i));
        (void)p.add(Rgba8::opaque(static_cast<std::uint8_t>(v[0]), static_cast<std::uint8_t>(v[1]), static_cast<std::uint8_t>(v[2])));
    }
    return p;
}

Result<Palette> parseHexLike(std::string_view text, bool paintNet) {
    Palette p("Imported");
    int lineNo = 0;
    for (auto l : lines(text)) {
        ++lineNo;
        if (l.empty() || l[0] == ';' || l[0] == '#') {
            if (!(l.size() == 7 && l[0] == '#')) continue;   // "#RRGGBB" lines are colours, not comments
        }
        if (!l.empty() && l[0] == '#') l.remove_prefix(1);
        std::optional<Rgba8> c;
        if (paintNet && l.size() == 8) {   // AARRGGBB
            const std::string rgba = std::string(l.substr(2)) + std::string(l.substr(0, 2));
            c = Rgba8::fromHex(rgba);
        } else if (l.size() == 6) {
            c = Rgba8::fromHex(l);
        }
        if (!c) return bad("Invalid colour on line " + std::to_string(lineNo));
        if (!p.add(*c)) return bad("Palette has more than 256 colours");
    }
    if (p.empty()) return bad("No colours found");
    return p;
}

// ------------------------------------------------------------------ Adobe Swatch Exchange
// Big-endian binary: "ASEF", version 1.0, block count, then blocks
//   0x0001 colour entry: name (u16 length incl. NUL, UTF-16BE), model
//          ("RGB ", "CMYK", "LAB ", "Gray"), float32 components, u16 type
//   0xC001 group start (name), 0xC002 group end.

struct BeReader {
    std::string_view d;
    std::size_t pos = 0;
    bool ok = true;
    std::uint32_t u(int bytes) {
        if (pos + static_cast<std::size_t>(bytes) > d.size()) { ok = false; return 0; }
        std::uint32_t v = 0;
        for (int i = 0; i < bytes; ++i) v = (v << 8) | static_cast<std::uint8_t>(d[pos++]);
        return v;
    }
    float f32() {
        const std::uint32_t bits = u(4);
        float f;
        std::memcpy(&f, &bits, 4);
        return f;
    }
    std::string utf16name() {
        const std::uint32_t n = u(2);
        std::string out;
        for (std::uint32_t i = 0; i < n && ok; ++i) {
            const std::uint32_t c = u(2);
            if (c == 0) continue;
            if (c < 0x80) out += static_cast<char>(c);
            else if (c < 0x800) { out += static_cast<char>(0xC0 | (c >> 6)); out += static_cast<char>(0x80 | (c & 0x3F)); }
            else { out += static_cast<char>(0xE0 | (c >> 12)); out += static_cast<char>(0x80 | ((c >> 6) & 0x3F)); out += static_cast<char>(0x80 | (c & 0x3F)); }
        }
        return out;
    }
};

std::uint8_t unit8(double v) { return static_cast<std::uint8_t>(std::clamp(std::lround(v * 255.0), 0L, 255L)); }

Rgba8 labToSrgb(double L, double a, double b) {
    // CIE L*a*b* (D50, as used by Adobe) -> XYZ -> Bradford to D65 -> sRGB.
    const double fy = (L + 16.0) / 116.0, fx = fy + a / 500.0, fz = fy - b / 200.0;
    auto finv = [](double t) { return t * t * t > 0.008856 ? t * t * t : (116.0 * t - 16.0) / 903.3; };
    const double X50 = 0.9642 * finv(fx), Y50 = L > 8.0 ? fy * fy * fy : L / 903.3, Z50 = 0.8249 * finv(fz);
    const double X = 0.9555766 * X50 - 0.0230393 * Y50 + 0.0631636 * Z50;
    const double Y = -0.0282895 * X50 + 1.0099416 * Y50 + 0.0210077 * Z50;
    const double Z = 0.0122982 * X50 - 0.0204830 * Y50 + 1.3299098 * Z50;
    double r = 3.2404542 * X - 1.5371385 * Y - 0.4985314 * Z;
    double g = -0.9692660 * X + 1.8760108 * Y + 0.0415560 * Z;
    double bl = 0.0556434 * X - 0.2040259 * Y + 1.0572252 * Z;
    auto gamma = [](double c) { c = std::clamp(c, 0.0, 1.0); return c <= 0.0031308 ? 12.92 * c : 1.055 * std::pow(c, 1 / 2.4) - 0.055; };
    return {unit8(gamma(r)), unit8(gamma(g)), unit8(gamma(bl)), 255};
}

Result<Palette> parseAse(std::string_view data) {
    BeReader r{data};
    if (data.substr(0, 4) != "ASEF") return Error{ErrorCode::InvalidArgument, "Not an Adobe Swatch Exchange file"};
    r.pos = 4;
    const std::uint32_t major = r.u(2);
    (void)r.u(2);
    if (major != 1) return Error{ErrorCode::InvalidArgument, "Unsupported .ase version " + std::to_string(major)};
    const std::uint32_t blocks = r.u(4);
    Palette p;
    p.setName("Imported");
    bool named = false;
    for (std::uint32_t i = 0; i < blocks && r.ok; ++i) {
        const std::uint32_t type = r.u(2), len = r.u(4);
        const std::size_t end = r.pos + len;
        if (end > data.size()) return Error{ErrorCode::CorruptData, "Truncated .ase block"};
        if (type == 0xC001) {
            const std::string name = r.utf16name();
            if (!named && !name.empty()) { p.setName(name); named = true; }
        } else if (type == 0x0001) {
            (void)r.utf16name();
            const std::string model(data.substr(r.pos, 4));
            r.pos += 4;
            Rgba8 c{0, 0, 0, 255};
            if (model == "RGB ") {
                const float R = r.f32(), G = r.f32(), B = r.f32();
                c = {unit8(R), unit8(G), unit8(B), 255};
            } else if (model == "CMYK") {
                const double C = r.f32(), M = r.f32(), Y = r.f32(), K = r.f32();
                c = {unit8((1 - C) * (1 - K)), unit8((1 - M) * (1 - K)), unit8((1 - Y) * (1 - K)), 255};
            } else if (model == "LAB ") {
                const double L = r.f32() * 100.0, A = r.f32(), B = r.f32();
                c = labToSrgb(L, A, B);
            } else if (model == "Gray") {
                const std::uint8_t g = unit8(r.f32());
                c = {g, g, g, 255};
            } else {
                return Error{ErrorCode::InvalidArgument, "Unknown .ase colour model '" + model + "'"};
            }
            if (!r.ok) break;
            if (auto st = p.add(c); !st) return Error{ErrorCode::InvalidArgument, "The palette has more than 256 colours"};
        }
        r.pos = end;   // skip the rest (colour type, unknown blocks)
    }
    if (!r.ok) return Error{ErrorCode::CorruptData, "Truncated .ase file"};
    if (p.size() == 0) return Error{ErrorCode::InvalidArgument, "The .ase file has no colours"};
    return p;
}

void putBe(std::string& out, std::uint32_t v, int bytes) {
    for (int i = bytes - 1; i >= 0; --i) out += static_cast<char>((v >> (8 * i)) & 0xFF);
}
void putName(std::string& out, const std::string& utf8) {
    // ASCII and 2-byte UTF-8 are converted; other characters become '?'.
    std::vector<std::uint16_t> u;
    for (std::size_t i = 0; i < utf8.size(); ++i) {
        const auto ch = static_cast<unsigned char>(utf8[i]);
        if (ch < 0x80) u.push_back(ch);
        else if ((ch & 0xE0) == 0xC0 && i + 1 < utf8.size()) { u.push_back(static_cast<std::uint16_t>(((ch & 0x1F) << 6) | (utf8[i + 1] & 0x3F))); ++i; }
        else { u.push_back('?'); while (i + 1 < utf8.size() && (static_cast<unsigned char>(utf8[i + 1]) & 0xC0) == 0x80) ++i; }
    }
    putBe(out, static_cast<std::uint32_t>(u.size() + 1), 2);
    for (auto c : u) putBe(out, c, 2);
    putBe(out, 0, 2);
}

std::string formatAse(const Palette& p) {
    std::string blocks;
    auto block = [&](std::uint32_t type, const std::string& body) {
        putBe(blocks, type, 2);
        putBe(blocks, static_cast<std::uint32_t>(body.size()), 4);
        blocks += body;
    };
    std::string g;
    putName(g, p.name());
    block(0xC001, g);
    for (const auto& c : p.colors()) {
        std::string e;
        putName(e, c.toHex(false));
        e += "RGB ";
        for (std::uint8_t v : {c.r, c.g, c.b}) {
            const float f = v / 255.0f;
            std::uint32_t bits;
            std::memcpy(&bits, &f, 4);
            putBe(e, bits, 4);
        }
        putBe(e, 2, 2);   // normal colour
        block(0x0001, e);
    }
    block(0xC002, "");
    std::string out = "ASEF";
    putBe(out, 1, 2);
    putBe(out, 0, 2);
    putBe(out, static_cast<std::uint32_t>(p.size() + 2), 4);
    return out + blocks;
}

char hexDigit(int v) { return "0123456789ABCDEF"[v & 15]; }
std::string hex2(std::uint8_t v) { return {hexDigit(v >> 4), hexDigit(v)}; }

} // namespace

Result<Palette> parsePalette(std::string_view text, std::string_view extension) {
    const std::string ext = lower(extension);
    // Normalise Windows line endings.
    std::string norm(text);
    std::erase(norm, '\r');
    if (ext == ".gpl") return parseGpl(norm);
    if (ext == ".pal") {
        if (norm.rfind("RIFF", 0) == 0) return Error{ErrorCode::InvalidArgument, "Binary RIFF .pal palettes are not supported; export as JASC-PAL"};
        return parseJasc(norm);
    }
    if (ext == ".hex") return parseHexLike(norm, false);
    if (ext == ".txt") return parseHexLike(norm, true);
    if (ext == ".ase") return parseAse(text);   // binary: use the raw bytes
    return Error{ErrorCode::InvalidArgument, "Unsupported palette format: " + std::string(extension)};
}

Result<std::string> formatPalette(const Palette& p, std::string_view extension) {
    const std::string ext = lower(extension);
    std::string out;
    if (ext == ".gpl") {
        out = "GIMP Palette\nName: " + p.name() + "\nColumns: 8\n#\n";
        for (const auto& c : p.colors()) {
            char buf[32];
            std::snprintf(buf, sizeof buf, "%3d %3d %3d\t%s\n", c.r, c.g, c.b, c.toHex(false).c_str() + 1);
            out += buf;
        }
    } else if (ext == ".pal") {
        out = "JASC-PAL\n0100\n" + std::to_string(p.size()) + "\n";
        for (const auto& c : p.colors()) out += std::to_string(c.r) + " " + std::to_string(c.g) + " " + std::to_string(c.b) + "\n";
    } else if (ext == ".hex") {
        for (const auto& c : p.colors()) out += hex2(c.r) + hex2(c.g) + hex2(c.b) + "\n";
    } else if (ext == ".ase") {
        out = formatAse(p);
    } else if (ext == ".txt") {
        out = "; paint.net Palette File\n; Exported by AceStudio\n";
        for (const auto& c : p.colors()) out += hex2(c.a) + hex2(c.r) + hex2(c.g) + hex2(c.b) + "\n";
    } else {
        return Error{ErrorCode::InvalidArgument, "Unsupported palette format: " + std::string(extension)};
    }
    return out;
}

Result<Palette> loadPaletteFile(const fs::path& path) {
    auto bytes = readFileBytes(path, 1u << 20);
    if (!bytes) return bytes.error();
    const std::string text(bytes.value().begin(), bytes.value().end());
    auto p = parsePalette(text, path.extension().string());
    if (p && (p.value().name() == "Imported")) p.value().setName(path.stem().string());
    return p;
}

Status savePaletteFile(const Palette& p, const fs::path& path) {
    auto text = formatPalette(p, path.extension().string());
    if (!text) return text.error();
    return writeFileAtomic(path, std::span(reinterpret_cast<const std::uint8_t*>(text.value().data()), text.value().size()));
}

// ------------------------------------------------------------------ extraction

namespace {
struct Box {
    std::vector<Rgba8> colors;
    int range(int ch) const {
        int lo = 255, hi = 0;
        for (const auto& c : colors) {
            const int v = ch == 0 ? c.r : ch == 1 ? c.g : c.b;
            lo = std::min(lo, v);
            hi = std::max(hi, v);
        }
        return hi - lo;
    }
};
} // namespace

Palette extractPalette(const Image& img, std::size_t maxColors) {
    maxColors = std::clamp<std::size_t>(maxColors, 1, Palette::kMaxColors);
    std::vector<Rgba8> unique;
    std::unordered_set<std::uint32_t> seen;
    std::vector<Rgba8> all;
    for (const Rgba8 p : img.pixels()) {
        if (p.a < 128) continue;
        const Rgba8 o{p.r, p.g, p.b, 255};
        all.push_back(o);
        const std::uint32_t key = (std::uint32_t{o.r} << 16) | (std::uint32_t{o.g} << 8) | o.b;
        if (seen.insert(key).second) unique.push_back(o);
    }
    Palette out("Extracted");
    if (unique.size() <= maxColors) {
        for (const auto& c : unique) (void)out.add(c);
        return out;
    }
    // Median cut over all opaque pixels (weighted by frequency).
    std::vector<Box> boxes{Box{std::move(all)}};
    while (boxes.size() < maxColors) {
        // Split the box with the widest channel range.
        std::size_t best = 0;
        int bestRange = -1, bestCh = 0;
        for (std::size_t i = 0; i < boxes.size(); ++i) {
            if (boxes[i].colors.size() < 2) continue;
            for (int ch = 0; ch < 3; ++ch) {
                const int r = boxes[i].range(ch);
                if (r > bestRange) { bestRange = r; best = i; bestCh = ch; }
            }
        }
        if (bestRange <= 0) break;
        auto& cs = boxes[best].colors;
        std::sort(cs.begin(), cs.end(), [bestCh](Rgba8 a, Rgba8 b) {
            return bestCh == 0 ? a.r < b.r : bestCh == 1 ? a.g < b.g : a.b < b.b;
        });
        Box upper{std::vector<Rgba8>(cs.begin() + static_cast<std::ptrdiff_t>(cs.size() / 2), cs.end())};
        cs.resize(cs.size() / 2);
        boxes.push_back(std::move(upper));
    }
    for (const auto& b : boxes) {
        if (b.colors.empty()) continue;
        unsigned long long r = 0, g = 0, bl = 0;
        for (const auto& c : b.colors) { r += c.r; g += c.g; bl += c.b; }
        const auto n = b.colors.size();
        (void)out.add(Rgba8::opaque(static_cast<std::uint8_t>((r + n / 2) / n), static_cast<std::uint8_t>((g + n / 2) / n),
                                    static_cast<std::uint8_t>((bl + n / 2) / n)));
    }
    return out;
}

} // namespace acestudio
