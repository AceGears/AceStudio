#include "acestudio/core/PngCodec.h"
#include "acestudio/core/FileUtil.h"

#include "StbImage.h"
#include <miniz.h>

#include <climits>
#include <memory>

namespace acestudio {

Result<std::vector<std::uint8_t>> encodePng(const Image& image) {
    if (image.isNull()) return Error{ErrorCode::InvalidArgument, "Cannot encode an empty image"};
    std::size_t len = 0;
    void* data = tdefl_write_image_to_png_file_in_memory_ex(
        image.rawBytes(), image.width(), image.height(), 4, &len, 6 /*level*/, MZ_FALSE);
    if (!data) return Error{ErrorCode::Internal, "PNG encoding failed"};
    std::unique_ptr<void, decltype(&mz_free)> guard(data, &mz_free);
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    return std::vector<std::uint8_t>(bytes, bytes + len);
}

Result<Image> decodePng(std::span<const std::uint8_t> bytes) {
    if (bytes.empty() || bytes.size() > static_cast<std::size_t>(INT_MAX))
        return Error{ErrorCode::CorruptData, "PNG data is empty or too large"};
    const int len = static_cast<int>(bytes.size());
    int w = 0, h = 0, comp = 0;
    if (!stbi_info_from_memory(bytes.data(), len, &w, &h, &comp))
        return Error{ErrorCode::CorruptData, std::string("Not a readable PNG: ") + stbi_failure_reason()};
    if (w <= 0 || h <= 0 || w > Image::kMaxDimension || h > Image::kMaxDimension)
        return Error{ErrorCode::CorruptData, "PNG dimensions exceed limits"};

    std::unique_ptr<stbi_uc, decltype(&stbi_image_free)> px(
        stbi_load_from_memory(bytes.data(), len, &w, &h, &comp, 4), &stbi_image_free);
    if (!px) return Error{ErrorCode::CorruptData, std::string("PNG decode failed: ") + stbi_failure_reason()};
    const std::size_t n = static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4;
    return Image::fromRgba(w, h, std::span<const std::uint8_t>(px.get(), n));
}

Status writePng(const Image& image, const std::filesystem::path& path) {
    auto bytes = encodePng(image);
    if (!bytes) return bytes.error();
    return writeFileAtomic(path, bytes.value());
}

Result<Image> readPng(const std::filesystem::path& path) {
    auto bytes = readFileBytes(path, 512ull << 20);
    if (!bytes) return bytes.error();
    return decodePng(bytes.value());
}

} // namespace acestudio
