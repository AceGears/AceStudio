// SHA-256 (FIPS 180-4) and HMAC (RFC 2104) for MCP requestState integrity.

#include "McpProtocol.h"

#include <array>
#include <cstdint>
#include <cstdio>

namespace acestudio::mcp {

namespace {

constexpr std::array<std::uint32_t, 64> K = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01,
    0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
    0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08,
    0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

std::uint32_t rotr(std::uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

std::array<std::uint8_t, 32> sha256(const std::string& data) {
    std::array<std::uint32_t, 8> h = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    std::string m = data;
    const std::uint64_t bits = static_cast<std::uint64_t>(data.size()) * 8;
    m.push_back(static_cast<char>(0x80));
    while (m.size() % 64 != 56) m.push_back('\0');
    for (int i = 7; i >= 0; --i) m.push_back(static_cast<char>((bits >> (i * 8)) & 0xff));
    for (std::size_t off = 0; off < m.size(); off += 64) {
        std::array<std::uint32_t, 64> w{};
        for (int i = 0; i < 16; ++i)
            w[static_cast<std::size_t>(i)] = (std::uint32_t(std::uint8_t(m[off + std::size_t(i) * 4])) << 24) |
                                             (std::uint32_t(std::uint8_t(m[off + std::size_t(i) * 4 + 1])) << 16) |
                                             (std::uint32_t(std::uint8_t(m[off + std::size_t(i) * 4 + 2])) << 8) |
                                             std::uint32_t(std::uint8_t(m[off + std::size_t(i) * 4 + 3]));
        for (std::size_t i = 16; i < 64; ++i) {
            const std::uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const std::uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        auto [a, b, c, d, e, f, g, hh] = h;
        for (std::size_t i = 0; i < 64; ++i) {
            const std::uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
            const std::uint32_t ch = (e & f) ^ (~e & g);
            const std::uint32_t t1 = hh + S1 + ch + K[i] + w[i];
            const std::uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
            const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            const std::uint32_t t2 = S0 + maj;
            hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }
    std::array<std::uint8_t, 32> out{};
    for (std::size_t i = 0; i < 8; ++i)
        for (std::size_t j = 0; j < 4; ++j) out[i * 4 + j] = static_cast<std::uint8_t>(h[i] >> (24 - 8 * j));
    return out;
}

} // namespace

std::string hmacSha256Hex(const std::string& keyIn, const std::string& message) {
    std::string key = keyIn;
    if (key.size() > 64) {
        const auto d = sha256(key);
        key.assign(d.begin(), d.end());
    }
    key.resize(64, '\0');
    std::string ipad(64, '\0'), opad(64, '\0');
    for (std::size_t i = 0; i < 64; ++i) {
        ipad[i] = static_cast<char>(key[i] ^ 0x36);
        opad[i] = static_cast<char>(key[i] ^ 0x5c);
    }
    const auto inner = sha256(ipad + message);
    const auto outer = sha256(opad + std::string(inner.begin(), inner.end()));
    std::string hex;
    char buf[3];
    for (auto b : outer) {
        std::snprintf(buf, sizeof buf, "%02x", b);
        hex += buf;
    }
    return hex;
}

} // namespace acestudio::mcp
