#include "acestudio/core/Image.h"

#include <algorithm>
#include <cstring>

namespace acestudio {

IntRect IntRect::intersected(const IntRect& o) const noexcept {
    const int x0 = std::max(x, o.x);
    const int y0 = std::max(y, o.y);
    const int x1 = std::min(x + w, o.x + o.w);
    const int y1 = std::min(y + h, o.y + o.h);
    if (x1 <= x0 || y1 <= y0) return {};
    return {x0, y0, x1 - x0, y1 - y0};
}

Image::Image(int width, int height, Rgba8 fillColor) {
    if (width <= 0 || height <= 0 || width > kMaxDimension || height > kMaxDimension) return;
    m_width = width;
    m_height = height;
    m_pixels.assign(static_cast<std::size_t>(width) * static_cast<std::size_t>(height), fillColor);
}

Result<Image> Image::create(int width, int height, Rgba8 fillColor) {
    if (width <= 0 || height <= 0)
        return Error{ErrorCode::InvalidArgument, "Image dimensions must be positive"};
    if (width > kMaxDimension || height > kMaxDimension)
        return Error{ErrorCode::InvalidArgument,
                     "Image dimensions exceed limit of " + std::to_string(kMaxDimension)};
    return Image(width, height, fillColor);
}

Result<Image> Image::fromRgba(int width, int height, std::span<const std::uint8_t> rgba) {
    auto img = create(width, height);
    if (!img) return img.error();
    if (rgba.size() != img.value().byteSize())
        return Error{ErrorCode::InvalidArgument, "RGBA buffer size does not match dimensions"};
    std::memcpy(img.value().m_pixels.data(), rgba.data(), rgba.size());
    return img;
}

Rgba8 Image::pixel(int x, int y) const noexcept {
    return contains(x, y) ? m_pixels[index(x, y)] : Rgba8::transparent();
}

void Image::setPixel(int x, int y, Rgba8 c) noexcept {
    if (contains(x, y)) m_pixels[index(x, y)] = c;
}

void Image::fill(Rgba8 c) noexcept { std::fill(m_pixels.begin(), m_pixels.end(), c); }

void Image::fillRect(const IntRect& r, Rgba8 c) noexcept {
    const IntRect clip = r.intersected(bounds());
    for (int y = clip.y; y < clip.y + clip.h; ++y)
        std::fill_n(m_pixels.begin() + static_cast<std::ptrdiff_t>(index(clip.x, y)), clip.w, c);
}

Image Image::copy(const IntRect& r) const {
    const IntRect clip = r.intersected(bounds());
    if (clip.empty()) return {};
    Image out(clip.w, clip.h);
    for (int y = 0; y < clip.h; ++y)
        std::copy_n(m_pixels.begin() + static_cast<std::ptrdiff_t>(index(clip.x, clip.y + y)), clip.w,
                    out.m_pixels.begin() + static_cast<std::ptrdiff_t>(out.index(0, y)));
    return out;
}

void Image::blit(const Image& src, int dx, int dy) noexcept {
    const IntRect dst = IntRect{dx, dy, src.width(), src.height()}.intersected(bounds());
    for (int y = dst.y; y < dst.y + dst.h; ++y)
        std::copy_n(src.m_pixels.begin() + static_cast<std::ptrdiff_t>(src.index(dst.x - dx, y - dy)), dst.w,
                    m_pixels.begin() + static_cast<std::ptrdiff_t>(index(dst.x, y)));
}

IntRect Image::opaqueBounds() const noexcept {
    int minX = m_width, minY = m_height, maxX = -1, maxY = -1;
    for (int y = 0; y < m_height; ++y)
        for (int x = 0; x < m_width; ++x)
            if (m_pixels[index(x, y)].a != 0) {
                minX = std::min(minX, x); maxX = std::max(maxX, x);
                minY = std::min(minY, y); maxY = std::max(maxY, y);
            }
    if (maxX < 0) return {};
    return {minX, minY, maxX - minX + 1, maxY - minY + 1};
}

bool Image::isFullyTransparent() const noexcept {
    return std::all_of(m_pixels.begin(), m_pixels.end(), [](Rgba8 p) { return p.a == 0; });
}

const std::uint8_t* Image::rawBytes() const noexcept {
    return reinterpret_cast<const std::uint8_t*>(m_pixels.data());
}

} // namespace acestudio
