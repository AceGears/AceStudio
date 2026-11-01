#include "acestudio/core/Licence.h"

#include "acestudio/core/FileUtil.h"
#include "acestudio/core/LicenceConfig.h"

#include <nlohmann/json.hpp>

#include <monocypher-ed25519.h>
#include <monocypher.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <span>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace acestudio {

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

constexpr const char* kPrefix = "ACESTUDIO-1.";
constexpr long long kDayMs = 24LL * 3600 * 1000;

// The development key's seed is public on purpose: keys signed with it work
// only in builds without a release key (which say so in About).
constexpr LicenceSecretKey kDevelopmentSeed = {'A', 'c', 'e', 'S', 't', 'u', 'd', 'i', 'o', ' ', 'd', 'e', 'v', 'e', 'l', 'o',
                                               'p', 'm', 'e', 'n', 't', ' ', 'k', 'e', 'y', ' ', '(', 'p', 'u', 'b', ')', '!'};

const char* kB64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

std::string b64url(const std::uint8_t* d, std::size_t n) {
    std::string out;
    std::uint32_t buf = 0;
    int bits = 0;
    for (std::size_t i = 0; i < n; ++i) {
        buf = (buf << 8) | d[i];
        bits += 8;
        while (bits >= 6) {
            bits -= 6;
            out += kB64[(buf >> bits) & 63];
        }
    }
    if (bits > 0) out += kB64[(buf << (6 - bits)) & 63];
    return out;
}

std::optional<std::string> unb64url(const std::string& s) {
    std::string out;
    std::uint32_t buf = 0;
    int bits = 0;
    for (char c : s) {
        const char* p = std::strchr(kB64, c);
        if (!p || c == '\0') return std::nullopt;
        buf = (buf << 6) | static_cast<std::uint32_t>(p - kB64);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out += static_cast<char>((buf >> bits) & 0xff);
        }
    }
    return out;
}

std::string dateOf(std::chrono::system_clock::time_point t) {
    const std::time_t tt = std::chrono::system_clock::to_time_t(t);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &tt);
#else
    gmtime_r(&tt, &tm);
#endif
    char buf[32];
    std::snprintf(buf, sizeof buf, "%04d-%02d-%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
    return buf;
}

bool isDate(const std::string& s) {
    return s.size() == 10 && s[4] == '-' && s[7] == '-' &&
           std::all_of(s.begin(), s.end(), [](char c) { return c == '-' || std::isdigit(static_cast<unsigned char>(c)); });
}

long long msOf(std::chrono::system_clock::time_point t) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(t.time_since_epoch()).count();
}

std::string readText(const fs::path& p) {
    auto b = readFileBytes(p, 1u << 20);
    return b ? std::string(b.value().begin(), b.value().end()) : std::string();
}

Status writeText(const fs::path& p, const std::string& s) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    return writeFileAtomic(p, std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(s.data()), s.size()));
}

#ifdef _WIN32
const wchar_t* kRegKey = L"Software\\AceGears\\AceStudio";
std::optional<long long> registryTrialStart() {
    long long v = 0;
    DWORD size = sizeof v;
    if (RegGetValueW(HKEY_CURRENT_USER, kRegKey, L"TrialStartMs", RRF_RT_REG_QWORD, nullptr, &v, &size) != ERROR_SUCCESS) return std::nullopt;
    return v;
}
void setRegistryTrialStart(long long ms) {
    HKEY k = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRegKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &k, nullptr) != ERROR_SUCCESS) return;
    RegSetValueExW(k, L"TrialStartMs", 0, REG_QWORD, reinterpret_cast<const BYTE*>(&ms), sizeof ms);
    RegCloseKey(k);
}
#else
std::optional<long long> registryTrialStart() { return std::nullopt; }
void setRegistryTrialStart(long long) {}
#endif

} // namespace

std::string toHex(const std::uint8_t* data, std::size_t size) {
    static const char* h = "0123456789abcdef";
    std::string s;
    for (std::size_t i = 0; i < size; ++i) {
        s += h[data[i] >> 4];
        s += h[data[i] & 15];
    }
    return s;
}

std::optional<std::array<std::uint8_t, 32>> key32FromHex(const std::string& hexIn) {
    std::string hex;
    for (char c : hexIn)
        if (!std::isspace(static_cast<unsigned char>(c))) hex += c;
    if (hex.size() != 64) return std::nullopt;
    std::array<std::uint8_t, 32> k{};
    for (std::size_t i = 0; i < 32; ++i) {
        auto nib = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        };
        const int a = nib(hex[2 * i]), b = nib(hex[2 * i + 1]);
        if (a < 0 || b < 0) return std::nullopt;
        k[i] = static_cast<std::uint8_t>(a * 16 + b);
    }
    return k;
}

LicencePublicKey licencePublicKeyFor(const LicenceSecretKey& seed) {
    std::uint8_t secret[64], pub[32], s[32];
    std::copy(seed.begin(), seed.end(), s);
    crypto_ed25519_key_pair(secret, pub, s);   // wipes s
    crypto_wipe(secret, sizeof secret);
    LicencePublicKey out{};
    std::copy(pub, pub + 32, out.begin());
    return out;
}

const LicenceSecretKey& developmentLicenceSeed() { return kDevelopmentSeed; }

bool usingDevelopmentLicenceKey() { return std::string(ACESTUDIO_LICENCE_PUBLIC_KEY).empty(); }

const LicencePublicKey& licencePublicKey() {
    static const LicencePublicKey key = [] {
        if (auto k = key32FromHex(ACESTUDIO_LICENCE_PUBLIC_KEY)) return *k;
        return licencePublicKeyFor(kDevelopmentSeed);
    }();
    return key;
}

std::string signLicenceKey(const LicenceKey& key, const LicenceSecretKey& seed) {
    json p{{"product", "acestudio"}, {"id", key.id}, {"name", key.name}, {"email", key.email}, {"edition", key.edition}, {"issued", key.issued}};
    if (!key.expires.empty()) p["expires"] = key.expires;
    const std::string payload = p.dump();
    const std::string body = std::string(kPrefix) + b64url(reinterpret_cast<const std::uint8_t*>(payload.data()), payload.size());
    std::uint8_t secret[64], pub[32], s[32], sig[64];
    std::copy(seed.begin(), seed.end(), s);
    crypto_ed25519_key_pair(secret, pub, s);
    crypto_ed25519_sign(sig, secret, reinterpret_cast<const std::uint8_t*>(body.data()), body.size());
    crypto_wipe(secret, sizeof secret);
    return body + "." + b64url(sig, sizeof sig);
}

Result<LicenceKey> verifyLicenceKey(const std::string& textIn, const LicencePublicKey& publicKey, const std::string& today) {
    std::string text;
    for (char c : textIn)
        if (!std::isspace(static_cast<unsigned char>(c))) text += c;
    const auto bad = [](const std::string& why) { return Error{ErrorCode::InvalidArgument, "This is not a valid AceStudio licence key: " + why}; };
    if (text.rfind(kPrefix, 0) != 0) return bad("it should start with " + std::string(kPrefix));
    const auto dot = text.rfind('.');
    if (dot <= std::string(kPrefix).size()) return bad("it is incomplete");
    const std::string body = text.substr(0, dot);
    const auto sig = unb64url(text.substr(dot + 1));
    if (!sig || sig->size() != 64) return bad("the signature is damaged");
    if (crypto_ed25519_check(reinterpret_cast<const std::uint8_t*>(sig->data()), publicKey.data(),
                             reinterpret_cast<const std::uint8_t*>(body.data()), body.size()) != 0)
        return bad("the signature does not match (was it changed or copied incompletely?)");
    const auto payload = unb64url(body.substr(std::string(kPrefix).size()));
    if (!payload) return bad("the contents are damaged");
    const json p = json::parse(*payload, nullptr, false);
    if (!p.is_object() || p.value("product", std::string()) != "acestudio") return bad("it is for another product");
    LicenceKey k;
    k.id = p.value("id", std::string());
    k.name = p.value("name", std::string());
    k.email = p.value("email", std::string());
    k.edition = p.value("edition", std::string("standard"));
    k.issued = p.value("issued", std::string());
    k.expires = p.value("expires", std::string());
    if (k.name.empty() || !isDate(k.issued) || (!k.expires.empty() && !isDate(k.expires))) return bad("required fields are missing");
    if (!k.expires.empty() && today > k.expires)
        return Error{ErrorCode::InvalidArgument, "This licence key expired on " + k.expires + "."};
    return k;
}

Result<LicenceKey> verifyLicenceKey(const std::string& text) {
    return verifyLicenceKey(text, licencePublicKey(), dateOf(std::chrono::system_clock::now()));
}

LicenceStatus licenceStatus(const fs::path& dataDir, std::chrono::system_clock::time_point now, bool useRegistry) {
    // ACESTUDIO_DATA_DIR keeps everything in one folder (tests, CI): the
    // registry, shared by every data folder of the user, stays out of it.
#ifdef _WIN32
    if (GetEnvironmentVariableW(L"ACESTUDIO_DATA_DIR", nullptr, 0) > 0) useRegistry = false;
#else
    if (std::getenv("ACESTUDIO_DATA_DIR")) useRegistry = false;
#endif
    LicenceStatus st;
    const std::string today = dateOf(now);
    if (const std::string text = readText(dataDir / "licence.key"); !text.empty()) {
        auto k = verifyLicenceKey(text, licencePublicKey(), today);
        if (k) {
            st.mode = LicenceMode::Licensed;
            st.key = k.value();
            return st;
        }
        st.problem = k.error().message;
    }
    // The trial: the earliest recorded start counts.
    const long long nowMs = msOf(now);
    std::optional<long long> start;
    const json t = json::parse(readText(dataDir / "trial.json"), nullptr, false);
    if (t.is_object() && t.contains("startedMs") && t["startedMs"].is_number_integer()) start = t["startedMs"].get<long long>();
    if (auto r = useRegistry ? registryTrialStart() : std::nullopt; r && (!start || *r < *start)) start = r;
    if (!start) {
        start = nowMs;
        (void)writeText(dataDir / "trial.json", json{{"started", today}, {"startedMs", nowMs}}.dump());
        if (useRegistry) setRegistryTrialStart(nowMs);
    } else {
        // Keep both records at the earliest start.
        if (!(t.is_object() && t.value("startedMs", 0LL) == *start))
            (void)writeText(dataDir / "trial.json",
                            json{{"started", dateOf(std::chrono::system_clock::time_point(std::chrono::milliseconds(*start)))}, {"startedMs", *start}}.dump());
        if (useRegistry && registryTrialStart() != start) setRegistryTrialStart(*start);
    }
    st.trialStarted = dateOf(std::chrono::system_clock::time_point(std::chrono::milliseconds(*start)));
    const long long elapsed = nowMs - *start;
    if (elapsed < -kDayMs) {   // the clock was set back
        st.mode = LicenceMode::TrialEnded;
        st.trialDaysLeft = 0;
        return st;
    }
    const long long left = kTrialDays - std::max(0LL, elapsed) / kDayMs;
    st.trialDaysLeft = static_cast<int>(std::max(0LL, left));
    st.mode = left > 0 ? LicenceMode::Trial : LicenceMode::TrialEnded;
    return st;
}

Result<LicenceKey> installLicenceKey(const fs::path& dataDir, const std::string& text) {
    auto k = verifyLicenceKey(text);
    if (!k) return k.error();
    std::string clean;
    for (char c : text)
        if (!std::isspace(static_cast<unsigned char>(c))) clean += c;
    if (auto s = writeText(dataDir / "licence.key", clean + "\n"); !s) return s.error();
    return k;
}

Status removeLicenceKey(const fs::path& dataDir) {
    std::error_code ec;
    fs::remove(dataDir / "licence.key", ec);
    if (ec) return Error{ErrorCode::IoError, "Could not remove the licence key: " + ec.message()};
    return {};
}

std::string licenceRequiredMessage(const std::string& action) {
    return action + " needs a licence key: the " + std::to_string(kTrialDays) +
           "-day trial has ended. You can still open and view your work, and nothing is lost. Enter a key in Help › Licence "
           "(or run acestudio-cli licence install KEY).";
}

std::string describeLicence(const LicenceStatus& s) {
    std::string out;
    switch (s.mode) {
    case LicenceMode::Licensed:
        out = "Licensed to " + s.key->name + (s.key->email.empty() ? "" : " <" + s.key->email + ">") + " (" + s.key->edition + " edition" +
              (s.key->expires.empty() ? "" : ", until " + s.key->expires) + ")";
        break;
    case LicenceMode::Trial:
        out = "Trial: " + std::to_string(s.trialDaysLeft) + (s.trialDaysLeft == 1 ? " day" : " days") + " left";
        break;
    case LicenceMode::TrialEnded:
        out = "Trial ended: saving and exporting need a licence key";
        break;
    }
    if (usingDevelopmentLicenceKey()) out += " [development build: keys are signed with the public development key]";
    return out;
}

} // namespace acestudio
