#include "acestudio/core/AnimCodec.h"
#include "acestudio/core/PaletteIO.h"

#include "StbImage.h"
#include <miniz.h>
#include <webp/decode.h>
#include <webp/demux.h>
#include <webp/encode.h>
#include <webp/mux.h>

#include <algorithm>
#include <climits>
#include <cstring>
#include <memory>
#include <unordered_map>

namespace acestudio {

namespace {

Status checkFrames(const std::vector<AnimFrame>& frames) {
    if (frames.empty()) return Error{ErrorCode::InvalidArgument, "No frames to encode"};
    const int w = frames[0].image.width(), h = frames[0].image.height();
    if (w <= 0 || h <= 0) return Error{ErrorCode::InvalidArgument, "Frames are empty"};
    for (const auto& f : frames) {
        if (f.image.width() != w || f.image.height() != h)
            return Error{ErrorCode::InvalidArgument, "All frames must have the same size"};
        if (f.durationMs < 1 || f.durationMs > 65535) return Error{ErrorCode::InvalidArgument, "Frame duration must be 1..65535 ms"};
    }
    if (w > 65535 || h > 65535) return Error{ErrorCode::InvalidArgument, "Image too large for this format"};
    return {};
}

void put16(std::vector<std::uint8_t>& o, unsigned v) {
    o.push_back(static_cast<std::uint8_t>(v & 0xFF));
    o.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFF));
}

void put32be(std::vector<std::uint8_t>& o, std::uint32_t v) {
    for (int s = 24; s >= 0; s -= 8) o.push_back(static_cast<std::uint8_t>((v >> s) & 0xFF));
}

std::uint32_t get32be(const std::uint8_t* p) {
    return (std::uint32_t{p[0]} << 24) | (std::uint32_t{p[1]} << 16) | (std::uint32_t{p[2]} << 8) | p[3];
}

std::uint32_t key(Rgba8 c) { return (std::uint32_t{c.r} << 16) | (std::uint32_t{c.g} << 8) | c.b; }

// ------------------------------------------------------------------ GIF LZW

class LzwWriter {
public:
    LzwWriter(std::vector<std::uint8_t>& out, int minCodeSize) : m_out(out), m_min(minCodeSize) {}

    void encode(const std::vector<std::uint8_t>& indices) {
        const int clear = 1 << m_min, eoi = clear + 1;
        resetTable();
        emit(clear);
        if (indices.empty()) { emit(eoi); flush(); return; }
        int prefix = indices[0];
        for (std::size_t i = 1; i < indices.size(); ++i) {
            const int k = indices[i];
            const std::uint32_t entry = (static_cast<std::uint32_t>(prefix) << 8) | static_cast<std::uint32_t>(k);
            auto it = m_table.find(entry);
            if (it != m_table.end()) { prefix = it->second; continue; }
            emit(prefix);
            if (m_next < 4096) {
                m_table.emplace(entry, m_next++);
                if (m_next > (1 << m_width) && m_width < 12) ++m_width;
            } else {
                emit(clear);
                resetTable();
            }
            prefix = k;
        }
        emit(prefix);
        emit(eoi);
        flush();
    }

private:
    void resetTable() {
        m_table.clear();
        m_next = (1 << m_min) + 2;
        m_width = m_min + 1;
    }
    void emit(int code) {
        m_bits |= static_cast<std::uint32_t>(code) << m_nbits;
        m_nbits += m_width;
        while (m_nbits >= 8) {
            pushByte(static_cast<std::uint8_t>(m_bits & 0xFF));
            m_bits >>= 8;
            m_nbits -= 8;
        }
    }
    void pushByte(std::uint8_t b) {
        m_block.push_back(b);
        if (m_block.size() == 255) writeBlock();
    }
    void writeBlock() {
        if (m_block.empty()) return;
        m_out.push_back(static_cast<std::uint8_t>(m_block.size()));
        m_out.insert(m_out.end(), m_block.begin(), m_block.end());
        m_block.clear();
    }
    void flush() {
        if (m_nbits > 0) pushByte(static_cast<std::uint8_t>(m_bits & 0xFF));
        m_bits = 0;
        m_nbits = 0;
        writeBlock();
        m_out.push_back(0);   // block terminator
    }

    std::vector<std::uint8_t>& m_out;
    int m_min;
    int m_width = 0;
    int m_next = 0;
    std::unordered_map<std::uint32_t, int> m_table;
    std::uint32_t m_bits = 0;
    int m_nbits = 0;
    std::vector<std::uint8_t> m_block;
};

} // namespace

int gifEffectiveDurationMs(int ms) noexcept { return std::max(2, (ms + 5) / 10) * 10; }

Result<std::vector<std::uint8_t>> encodeGif(const std::vector<AnimFrame>& frames, int loopCount) {
    if (auto st = checkFrames(frames); !st) return st.error();
    const int w = frames[0].image.width(), h = frames[0].image.height();

    // Global palette from every frame (opaque pixels only).
    Image all(w, static_cast<int>(std::min<long long>(Image::kMaxDimension, static_cast<long long>(h) * static_cast<long long>(frames.size()))));
    for (std::size_t i = 0; i < frames.size(); ++i)
        if (static_cast<long long>(h) * static_cast<long long>(i) < all.height()) all.blit(frames[i].image, 0, h * static_cast<int>(i));
    const Palette pal = extractPalette(all, 255);
    const auto& colors = pal.colors();
    const int transparent = static_cast<int>(colors.size());
    int bits = 1;
    while ((1 << bits) < transparent + 1) ++bits;
    const int tableSize = 1 << bits;

    std::unordered_map<std::uint32_t, std::uint8_t> nearestCache;
    auto indexOf = [&](Rgba8 c) -> std::uint8_t {
        if (c.a < 128) return static_cast<std::uint8_t>(transparent);
        auto it = nearestCache.find(key(c));
        if (it != nearestCache.end()) return it->second;
        long best = LONG_MAX;
        std::size_t bi = 0;
        for (std::size_t i = 0; i < colors.size(); ++i) {
            const long dr = long{colors[i].r} - c.r, dg = long{colors[i].g} - c.g, db = long{colors[i].b} - c.b;
            const long d = dr * dr + dg * dg + db * db;
            if (d < best) { best = d; bi = i; }
        }
        const auto idx = static_cast<std::uint8_t>(bi);
        nearestCache.emplace(key(c), idx);
        return idx;
    };

    std::vector<std::uint8_t> o;
    const char* header = "GIF89a";
    o.insert(o.end(), header, header + 6);
    put16(o, static_cast<unsigned>(w));
    put16(o, static_cast<unsigned>(h));
    o.push_back(static_cast<std::uint8_t>(0x80 | (7 << 4) | (bits - 1)));   // global table, 8-bit colour res
    o.push_back(static_cast<std::uint8_t>(transparent));                     // background = transparent
    o.push_back(0);
    for (int i = 0; i < tableSize; ++i) {
        const Rgba8 c = i < transparent ? colors[static_cast<std::size_t>(i)] : Rgba8{0, 0, 0, 255};
        o.push_back(c.r); o.push_back(c.g); o.push_back(c.b);
    }
    // NETSCAPE2.0 looping extension
    const std::uint8_t netscape[] = {0x21, 0xFF, 0x0B, 'N', 'E', 'T', 'S', 'C', 'A', 'P', 'E', '2', '.', '0', 0x03, 0x01};
    o.insert(o.end(), std::begin(netscape), std::end(netscape));
    put16(o, static_cast<unsigned>(std::clamp(loopCount, 0, 65535)));
    o.push_back(0);

    const int minCode = std::max(2, bits);
    for (const auto& f : frames) {
        // Graphic control: dispose to background (frames are full canvas), transparency on.
        o.insert(o.end(), {0x21, 0xF9, 0x04, static_cast<std::uint8_t>((2 << 2) | 1)});
        put16(o, static_cast<unsigned>(gifEffectiveDurationMs(f.durationMs) / 10));
        o.push_back(static_cast<std::uint8_t>(transparent));
        o.push_back(0);
        // Image descriptor
        o.push_back(0x2C);
        put16(o, 0); put16(o, 0);
        put16(o, static_cast<unsigned>(w)); put16(o, static_cast<unsigned>(h));
        o.push_back(0);
        std::vector<std::uint8_t> idx;
        idx.reserve(static_cast<std::size_t>(w) * static_cast<std::size_t>(h));
        for (const Rgba8 p : f.image.pixels()) idx.push_back(indexOf(p));
        o.push_back(static_cast<std::uint8_t>(minCode));
        LzwWriter(o, minCode).encode(idx);
    }
    o.push_back(0x3B);
    return o;
}

Result<DecodedAnimation> decodeGif(std::span<const std::uint8_t> bytes) {
    if (bytes.size() < 6 || bytes.size() > static_cast<std::size_t>(INT_MAX))
        return Error{ErrorCode::CorruptData, "Not a GIF"};
    int* delays = nullptr;
    int w = 0, h = 0, n = 0, comp = 0;
    stbi_uc* data = stbi_load_gif_from_memory(bytes.data(), static_cast<int>(bytes.size()), &delays, &w, &h, &n, &comp, 4);
    if (!data) return Error{ErrorCode::CorruptData, std::string("GIF decode failed: ") + stbi_failure_reason()};
    std::unique_ptr<stbi_uc, decltype(&stbi_image_free)> guard(data, &stbi_image_free);
    std::unique_ptr<int, decltype(&stbi_image_free)> dguard(delays, &stbi_image_free);
    if (w <= 0 || h <= 0 || w > Image::kMaxDimension || h > Image::kMaxDimension)
        return Error{ErrorCode::CorruptData, "GIF dimensions exceed limits"};
    DecodedAnimation out;
    const std::size_t frameBytes = static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4;
    for (int i = 0; i < n; ++i) {
        auto img = Image::fromRgba(w, h, std::span<const std::uint8_t>(data + frameBytes * static_cast<std::size_t>(i), frameBytes));
        if (!img) return img.error();
        out.frames.push_back({std::move(img).value(), delays ? std::max(10, delays[i]) : 100});
    }
    return out;
}

// ------------------------------------------------------------------ APNG

namespace {

void chunk(std::vector<std::uint8_t>& o, const char type[4], const std::vector<std::uint8_t>& data) {
    put32be(o, static_cast<std::uint32_t>(data.size()));
    const std::size_t start = o.size();
    o.insert(o.end(), type, type + 4);
    o.insert(o.end(), data.begin(), data.end());
    const auto crc = static_cast<std::uint32_t>(mz_crc32(MZ_CRC32_INIT, o.data() + start, data.size() + 4));
    put32be(o, crc);
}

Result<std::vector<std::uint8_t>> zlibFrame(const Image& img) {
    const std::size_t stride = static_cast<std::size_t>(img.width()) * 4;
    std::vector<std::uint8_t> raw;
    raw.reserve((stride + 1) * static_cast<std::size_t>(img.height()));
    for (int y = 0; y < img.height(); ++y) {
        raw.push_back(0);   // filter: none
        const auto* row = img.rawBytes() + stride * static_cast<std::size_t>(y);
        raw.insert(raw.end(), row, row + stride);
    }
    mz_ulong len = mz_compressBound(static_cast<mz_ulong>(raw.size()));
    std::vector<std::uint8_t> out(len);
    if (mz_compress2(out.data(), &len, raw.data(), static_cast<mz_ulong>(raw.size()), 9) != MZ_OK)
        return Error{ErrorCode::Internal, "zlib compression failed"};
    out.resize(len);
    return out;
}

std::uint8_t paeth(int a, int b, int c) {
    const int p = a + b - c, pa = std::abs(p - a), pb = std::abs(p - b), pc = std::abs(p - c);
    return static_cast<std::uint8_t>(pa <= pb && pa <= pc ? a : pb <= pc ? b : c);
}

// Inflates and unfilters RGBA8 scanlines.
Result<Image> inflateRgba(const std::vector<std::uint8_t>& z, int w, int h) {
    const std::size_t stride = static_cast<std::size_t>(w) * 4;
    mz_ulong len = static_cast<mz_ulong>((stride + 1) * static_cast<std::size_t>(h));
    std::vector<std::uint8_t> raw(len);
    if (mz_uncompress(raw.data(), &len, z.data(), static_cast<mz_ulong>(z.size())) != MZ_OK || len != raw.size())
        return Error{ErrorCode::CorruptData, "APNG frame data is damaged"};
    std::vector<std::uint8_t> px(stride * static_cast<std::size_t>(h));
    for (int y = 0; y < h; ++y) {
        const std::uint8_t ft = raw[(stride + 1) * static_cast<std::size_t>(y)];
        const std::uint8_t* src = raw.data() + (stride + 1) * static_cast<std::size_t>(y) + 1;
        std::uint8_t* dst = px.data() + stride * static_cast<std::size_t>(y);
        const std::uint8_t* up = y > 0 ? dst - stride : nullptr;
        for (std::size_t i = 0; i < stride; ++i) {
            const int a = i >= 4 ? dst[i - 4] : 0, b = up ? up[i] : 0, c = (up && i >= 4) ? up[i - 4] : 0;
            int v = src[i];
            switch (ft) {
            case 0: break;
            case 1: v += a; break;
            case 2: v += b; break;
            case 3: v += (a + b) / 2; break;
            case 4: v += paeth(a, b, c); break;
            default: return Error{ErrorCode::CorruptData, "Unknown PNG filter"};
            }
            dst[i] = static_cast<std::uint8_t>(v & 0xFF);
        }
    }
    return Image::fromRgba(w, h, px);
}

} // namespace

Result<std::vector<std::uint8_t>> encodeApng(const std::vector<AnimFrame>& frames, int loopCount) {
    if (auto st = checkFrames(frames); !st) return st.error();
    const auto w = static_cast<std::uint32_t>(frames[0].image.width()), h = static_cast<std::uint32_t>(frames[0].image.height());
    std::vector<std::uint8_t> o = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    std::vector<std::uint8_t> ihdr;
    put32be(ihdr, w); put32be(ihdr, h);
    ihdr.insert(ihdr.end(), {8, 6, 0, 0, 0});
    chunk(o, "IHDR", ihdr);
    std::vector<std::uint8_t> actl;
    put32be(actl, static_cast<std::uint32_t>(frames.size()));
    put32be(actl, static_cast<std::uint32_t>(std::max(0, loopCount)));
    chunk(o, "acTL", actl);

    std::uint32_t seq = 0;
    for (std::size_t i = 0; i < frames.size(); ++i) {
        std::vector<std::uint8_t> fctl;
        put32be(fctl, seq++);
        put32be(fctl, w); put32be(fctl, h);
        put32be(fctl, 0); put32be(fctl, 0);
        fctl.push_back(static_cast<std::uint8_t>(frames[i].durationMs >> 8));
        fctl.push_back(static_cast<std::uint8_t>(frames[i].durationMs & 0xFF));
        fctl.push_back(0x03); fctl.push_back(0xE8);   // delay denominator 1000
        fctl.push_back(0);   // dispose: none
        fctl.push_back(0);   // blend: source (full replacement)
        chunk(o, "fcTL", fctl);
        auto z = zlibFrame(frames[i].image);
        if (!z) return z.error();
        if (i == 0) {
            chunk(o, "IDAT", z.value());
        } else {
            std::vector<std::uint8_t> fdat;
            put32be(fdat, seq++);
            fdat.insert(fdat.end(), z.value().begin(), z.value().end());
            chunk(o, "fdAT", fdat);
        }
    }
    chunk(o, "IEND", {});
    return o;
}

Result<DecodedAnimation> decodeApng(std::span<const std::uint8_t> b) {
    static const std::uint8_t sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    if (b.size() < 8 || std::memcmp(b.data(), sig, 8) != 0) return Error{ErrorCode::CorruptData, "Not a PNG"};
    int W = 0, H = 0;
    DecodedAnimation out;
    bool animated = false;
    struct Pending { int w = 0, h = 0, x = 0, y = 0, delay = 100, dispose = 0, blend = 0; std::vector<std::uint8_t> z; bool active = false; };
    Pending cur;
    Image canvas;
    auto finish = [&]() -> Status {
        if (!cur.active) return {};
        auto img = inflateRgba(cur.z, cur.w, cur.h);
        if (!img) return img.error();
        const Image before = canvas;
        if (cur.blend == 0) {
            for (int y = 0; y < cur.h; ++y)
                for (int x = 0; x < cur.w; ++x) canvas.setPixel(cur.x + x, cur.y + y, img.value().pixel(x, y));
        } else {
            for (int y = 0; y < cur.h; ++y)
                for (int x = 0; x < cur.w; ++x) {
                    const Rgba8 s = img.value().pixel(x, y);
                    if (s.a == 255) canvas.setPixel(cur.x + x, cur.y + y, s);
                    else if (s.a) {
                        const Rgba8 d = canvas.pixel(cur.x + x, cur.y + y);
                        const unsigned sa = s.a, da = d.a * (255u - sa) / 255u, oa = sa + da;
                        auto mix = [&](unsigned cs, unsigned cd) { return static_cast<std::uint8_t>((cs * sa + cd * da) / oa); };
                        canvas.setPixel(cur.x + x, cur.y + y, {mix(s.r, d.r), mix(s.g, d.g), mix(s.b, d.b), static_cast<std::uint8_t>(oa)});
                    }
                }
        }
        out.frames.push_back({canvas, cur.delay});
        if (cur.dispose == 1) canvas.fillRect({cur.x, cur.y, cur.w, cur.h}, Rgba8::transparent());
        else if (cur.dispose == 2) canvas = before;
        cur = Pending{};
        return {};
    };

    std::size_t pos = 8;
    while (pos + 12 <= b.size()) {
        const std::uint32_t len = get32be(b.data() + pos);
        if (len > b.size() - pos - 12) return Error{ErrorCode::CorruptData, "PNG chunk overruns file"};
        const std::string type(reinterpret_cast<const char*>(b.data() + pos + 4), 4);
        const std::uint8_t* d = b.data() + pos + 8;
        const auto crc = static_cast<std::uint32_t>(mz_crc32(MZ_CRC32_INIT, b.data() + pos + 4, len + 4));
        if (crc != get32be(d + len)) return Error{ErrorCode::CorruptData, "PNG chunk CRC mismatch in " + type};
        if (type == "IHDR") {
            if (len < 13) return Error{ErrorCode::CorruptData, "Bad IHDR"};
            W = static_cast<int>(get32be(d));
            H = static_cast<int>(get32be(d + 4));
            if (d[8] != 8 || d[9] != 6 || d[12] != 0)
                return Error{ErrorCode::InvalidArgument, "Only non-interlaced 8-bit RGBA APNG is supported"};
            if (W <= 0 || H <= 0 || W > Image::kMaxDimension || H > Image::kMaxDimension)
                return Error{ErrorCode::CorruptData, "PNG dimensions exceed limits"};
            canvas = Image(W, H);
        } else if (type == "acTL") {
            animated = true;
            out.loopCount = static_cast<int>(get32be(d + 4));
        } else if (type == "fcTL") {
            if (auto st = finish(); !st) return st.error();
            if (len < 26) return Error{ErrorCode::CorruptData, "Bad fcTL"};
            cur.active = true;
            cur.w = static_cast<int>(get32be(d + 4));
            cur.h = static_cast<int>(get32be(d + 8));
            cur.x = static_cast<int>(get32be(d + 12));
            cur.y = static_cast<int>(get32be(d + 16));
            const int num = (d[20] << 8) | d[21], den = (d[22] << 8) | d[23];
            cur.delay = std::max(1, num * 1000 / (den == 0 ? 100 : den));
            cur.dispose = d[24];
            cur.blend = d[25];
            if (cur.w <= 0 || cur.h <= 0 || cur.x + cur.w > W || cur.y + cur.h > H) return Error{ErrorCode::CorruptData, "fcTL region outside canvas"};
        } else if (type == "IDAT") {
            if (cur.active) cur.z.insert(cur.z.end(), d, d + len);
            else if (!animated) { cur.active = true; cur.w = W; cur.h = H; cur.z.insert(cur.z.end(), d, d + len); }
        } else if (type == "fdAT") {
            if (cur.active && len >= 4) cur.z.insert(cur.z.end(), d + 4, d + len);
        } else if (type == "IEND") {
            break;
        }
        pos += 12 + len;
    }
    if (auto st = finish(); !st) return st.error();
    if (out.frames.empty()) return Error{ErrorCode::CorruptData, "PNG contains no frames"};
    return out;
}

// ------------------------------------------------------------------ WebP

Result<std::vector<std::uint8_t>> encodeWebp(const std::vector<AnimFrame>& frames, int loopCount) {
    if (auto st = checkFrames(frames); !st) return st.error();
    const int w = frames[0].image.width(), h = frames[0].image.height();
    if (w > WEBP_MAX_DIMENSION || h > WEBP_MAX_DIMENSION) return Error{ErrorCode::InvalidArgument, "Image too large for WebP"};
    WebPAnimEncoderOptions aopt;
    if (!WebPAnimEncoderOptionsInit(&aopt)) return Error{ErrorCode::Internal, "libwebp version mismatch"};
    aopt.anim_params.loop_count = std::max(0, loopCount);
    aopt.allow_mixed = 0;
    std::unique_ptr<WebPAnimEncoder, decltype(&WebPAnimEncoderDelete)> enc(WebPAnimEncoderNew(w, h, &aopt), &WebPAnimEncoderDelete);
    if (!enc) return Error{ErrorCode::Internal, "Could not create WebP encoder"};
    WebPConfig cfg;
    if (!WebPConfigInit(&cfg)) return Error{ErrorCode::Internal, "libwebp version mismatch"};
    cfg.lossless = 1;
    cfg.exact = 1;
    cfg.quality = 100;
    cfg.method = 4;

    int timestamp = 0;
    for (const auto& f : frames) {
        WebPPicture pic;
        if (!WebPPictureInit(&pic)) return Error{ErrorCode::Internal, "libwebp version mismatch"};
        pic.use_argb = 1;
        pic.width = w;
        pic.height = h;
        if (!WebPPictureImportRGBA(&pic, f.image.rawBytes(), w * 4)) {
            WebPPictureFree(&pic);
            return Error{ErrorCode::Internal, "WebP frame import failed"};
        }
        const int ok = WebPAnimEncoderAdd(enc.get(), &pic, timestamp, &cfg);
        WebPPictureFree(&pic);
        if (!ok) return Error{ErrorCode::Internal, std::string("WebP encode failed: ") + WebPAnimEncoderGetError(enc.get())};
        timestamp += f.durationMs;
    }
    if (!WebPAnimEncoderAdd(enc.get(), nullptr, timestamp, nullptr))
        return Error{ErrorCode::Internal, "WebP finalise failed"};
    WebPData data;
    WebPDataInit(&data);
    if (!WebPAnimEncoderAssemble(enc.get(), &data)) return Error{ErrorCode::Internal, "WebP assemble failed"};
    std::vector<std::uint8_t> out(data.bytes, data.bytes + data.size);
    WebPDataClear(&data);
    return out;
}

Result<DecodedAnimation> decodeWebp(std::span<const std::uint8_t> bytes) {
    WebPData data{bytes.data(), bytes.size()};
    WebPAnimDecoderOptions dopt;
    if (!WebPAnimDecoderOptionsInit(&dopt)) return Error{ErrorCode::Internal, "libwebp version mismatch"};
    dopt.color_mode = MODE_RGBA;
    dopt.use_threads = 0;
    std::unique_ptr<WebPAnimDecoder, decltype(&WebPAnimDecoderDelete)> dec(WebPAnimDecoderNew(&data, &dopt), &WebPAnimDecoderDelete);
    if (!dec) return Error{ErrorCode::CorruptData, "Not a readable WebP"};
    WebPAnimInfo info;
    if (!WebPAnimDecoderGetInfo(dec.get(), &info)) return Error{ErrorCode::CorruptData, "WebP info unreadable"};
    const int w = static_cast<int>(info.canvas_width), h = static_cast<int>(info.canvas_height);
    if (w <= 0 || h <= 0 || w > Image::kMaxDimension || h > Image::kMaxDimension)
        return Error{ErrorCode::CorruptData, "WebP dimensions exceed limits"};
    DecodedAnimation out;
    out.loopCount = static_cast<int>(info.loop_count);
    int prevTs = 0;
    while (WebPAnimDecoderHasMoreFrames(dec.get())) {
        std::uint8_t* buf = nullptr;
        int ts = 0;
        if (!WebPAnimDecoderGetNext(dec.get(), &buf, &ts)) return Error{ErrorCode::CorruptData, "WebP frame decode failed"};
        auto img = Image::fromRgba(w, h, std::span<const std::uint8_t>(buf, static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4));
        if (!img) return img.error();
        out.frames.push_back({std::move(img).value(), std::max(1, ts - prevTs)});
        prevTs = ts;
    }
    if (out.frames.empty()) return Error{ErrorCode::CorruptData, "WebP contains no frames"};
    return out;
}

} // namespace acestudio
