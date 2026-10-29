#pragma once

#include "acestudio/core/Image.h"
#include "acestudio/core/Result.h"

#include <cstdint>
#include <filesystem>
#include <span>
#include <vector>

namespace acestudio {

// Lossless RGBA PNG encode/decode. Decoding enforces Image::kMaxDimension.
Result<std::vector<std::uint8_t>> encodePng(const Image& image);
Result<Image> decodePng(std::span<const std::uint8_t> bytes);

Status writePng(const Image& image, const std::filesystem::path& path);
Result<Image> readPng(const std::filesystem::path& path);

} // namespace acestudio
