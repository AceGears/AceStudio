#pragma once

#include "acestudio/core/Color.h"
#include "acestudio/core/Result.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace acestudio {

struct IntRect {
    int x = 0, y = 0, w = 0, h = 0;
    constexpr bool empty() const noexcept { return w <= 0 || h <= 0; }
    constexpr bool operator==(const IntRect&) const = default;
    IntRect intersected(const IntRect& o) const noexcept;
};

// Dense RGBA8 pixel buffer, row-major, origin top-left.
// Hard limits keep a malicious or corrupt file from exhausting memory.
//
// Copies share their pixels until one of them is changed (copy-on-write), so
// copying a document (undo snapshots, autosave, scripts working on a copy) is
// cheap. Every non-const member detaches first. Rules that follow:
//  - a span from the non-const pixels() is valid only until the image is next
//    copied or assigned; do not copy the image while writing through it;
//  - threads may read shared images freely, and each may change its own
//    Image objects; one Image object must not be changed from two threads
//    unless it was detached first (call pixels() once before fanning out).
class Image {
public:
    static constexpr int kMaxDimension = 16384;

    Image() = default;
    Image(int width, int height, Rgba8 fill = Rgba8::transparent());

    static Result<Image> create(int width, int height, Rgba8 fill = Rgba8::transparent());
    static Result<Image> fromRgba(int width, int height, std::span<const std::uint8_t> rgba);

    int width() const noexcept { return m_width; }
    int height() const noexcept { return m_height; }
    bool isNull() const noexcept { return m_width == 0 || m_height == 0; }
    IntRect bounds() const noexcept { return {0, 0, m_width, m_height}; }
    bool contains(int x, int y) const noexcept { return x >= 0 && y >= 0 && x < m_width && y < m_height; }

    Rgba8 pixel(int x, int y) const noexcept;               // transparent if out of range
    void setPixel(int x, int y, Rgba8 c) noexcept;          // no-op if out of range
    void fill(Rgba8 c) noexcept;
    void fillRect(const IntRect& r, Rgba8 c) noexcept;

    Image copy(const IntRect& r) const;
    // Copies src into this image at (dx, dy), replacing pixels (no blending).
    void blit(const Image& src, int dx, int dy) noexcept;

    // Smallest rect containing all pixels with alpha > 0; empty if none.
    IntRect opaqueBounds() const noexcept;
    bool isFullyTransparent() const noexcept;

    std::span<const Rgba8> pixels() const noexcept { return data(); }
    std::span<Rgba8> pixels() { return mut(); }
    const std::uint8_t* rawBytes() const noexcept;
    std::size_t byteSize() const noexcept { return data().size() * 4; }

    // Same size and pixels (shared buffers compare equal without a scan).
    bool operator==(const Image& o) const noexcept;
    // True when both share one pixel buffer (for tests and diagnostics).
    bool sharesPixelsWith(const Image& o) const noexcept { return m_data && m_data == o.m_data; }

private:
    std::size_t index(int x, int y) const noexcept {
        return static_cast<std::size_t>(y) * static_cast<std::size_t>(m_width) + static_cast<std::size_t>(x);
    }
    const std::vector<Rgba8>& data() const noexcept;
    std::vector<Rgba8>& mut();   // detaches a shared buffer

    int m_width = 0;
    int m_height = 0;
    std::shared_ptr<std::vector<Rgba8>> m_data;
};

static_assert(sizeof(Rgba8) == 4, "Rgba8 must be tightly packed");

} // namespace acestudio
