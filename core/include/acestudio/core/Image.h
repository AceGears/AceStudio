#pragma once

#include "acestudio/core/Color.h"
#include "acestudio/core/Result.h"

#include <cstddef>
#include <cstdint>
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

    std::span<const Rgba8> pixels() const noexcept { return m_pixels; }
    std::span<Rgba8> pixels() noexcept { return m_pixels; }
    const std::uint8_t* rawBytes() const noexcept;
    std::size_t byteSize() const noexcept { return m_pixels.size() * 4; }

    bool operator==(const Image&) const = default;

private:
    std::size_t index(int x, int y) const noexcept {
        return static_cast<std::size_t>(y) * static_cast<std::size_t>(m_width) + static_cast<std::size_t>(x);
    }

    int m_width = 0;
    int m_height = 0;
    std::vector<Rgba8> m_pixels;
};

static_assert(sizeof(Rgba8) == 4, "Rgba8 must be tightly packed");

} // namespace acestudio
