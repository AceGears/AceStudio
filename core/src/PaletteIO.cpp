#include "acestudio/core/PaletteIO.h"
#include "acestudio/core/FileUtil.h"

#include <algorithm>
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
