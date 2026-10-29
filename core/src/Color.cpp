#include "acestudio/core/Color.h"
#include "acestudio/core/Result.h"

#include <cstdio>

namespace acestudio {

const char* toString(ErrorCode code) noexcept {
    switch (code) {
    case ErrorCode::Ok: return "Ok";
    case ErrorCode::InvalidArgument: return "InvalidArgument";
    case ErrorCode::IoError: return "IoError";
    case ErrorCode::NotFound: return "NotFound";
    case ErrorCode::CorruptData: return "CorruptData";
    case ErrorCode::UnsupportedVersion: return "UnsupportedVersion";
    case ErrorCode::PermissionDenied: return "PermissionDenied";
    case ErrorCode::Cancelled: return "Cancelled";
    case ErrorCode::Internal: return "Internal";
    }
    return "Unknown";
}

namespace {
int hexDigit(char c) noexcept {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
} // namespace

std::optional<Rgba8> Rgba8::fromHex(std::string_view hex) {
    if (!hex.empty() && hex.front() == '#') hex.remove_prefix(1);
    if (hex.size() != 6 && hex.size() != 8) return std::nullopt;
    std::uint8_t ch[4] = {0, 0, 0, 255};
    for (std::size_t i = 0; i < hex.size() / 2; ++i) {
        const int hi = hexDigit(hex[i * 2]);
        const int lo = hexDigit(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) return std::nullopt;
        ch[i] = static_cast<std::uint8_t>(hi * 16 + lo);
    }
    return Rgba8{ch[0], ch[1], ch[2], ch[3]};
}

std::string Rgba8::toHex(bool withAlpha) const {
    char buf[10];
    if (withAlpha)
        std::snprintf(buf, sizeof buf, "#%02X%02X%02X%02X", r, g, b, a);
    else
        std::snprintf(buf, sizeof buf, "#%02X%02X%02X", r, g, b);
    return buf;
}

std::uint8_t Rgba8::luma() const noexcept {
    // Integer Rec.709: 0.2126 R + 0.7152 G + 0.0722 B, scaled by 10000.
    const unsigned v = (2126u * r + 7152u * g + 722u * b + 5000u) / 10000u;
    return static_cast<std::uint8_t>(v > 255u ? 255u : v);
}

} // namespace acestudio
