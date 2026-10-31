#pragma once

// Palette file formats and palette extraction.
//   .gpl  GIMP palette           (text)
//   .pal  JASC-PAL (Paint Shop Pro, Aseprite, many tools; text)
//   .hex  one RRGGBB per line    (Lospec export)
//   .txt  Paint.NET palette      (AARRGGBB per line, ';' comments)
//   .ase  Adobe Swatch Exchange  (binary; RGB, CMYK, LAB and Gray swatches read, RGB written)
// Readers are strict about structure but tolerant of whitespace and
// comments, and never accept more than 256 colours.

#include "acestudio/core/Image.h"
#include "acestudio/core/Palette.h"
#include "acestudio/core/Result.h"

#include <filesystem>
#include <string>
#include <string_view>

namespace acestudio {

Result<Palette> parsePalette(std::string_view text, std::string_view extension);
Result<std::string> formatPalette(const Palette& p, std::string_view extension);

Result<Palette> loadPaletteFile(const std::filesystem::path& path);
Status savePaletteFile(const Palette& p, const std::filesystem::path& path);

// Opaque colours of `img` (alpha >= 128). If there are at most `maxColors`
// distinct colours they are returned exactly (in order of first
// appearance); otherwise they are reduced with median cut.
Palette extractPalette(const Image& img, std::size_t maxColors = 256);

} // namespace acestudio
