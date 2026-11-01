#pragma once

// Licensing (Phase 8): offline licence keys signed by AceGears, and a 14-day
// trial.
//
// A key is text: "ACESTUDIO-1." + base64url(JSON payload) + "." +
// base64url(Ed25519 signature of everything before the last dot). The app
// holds only the public key, so keys can be checked offline but not forged.
// Whitespace and line breaks in a pasted key are ignored.
//
// Without a valid key the trial runs for 14 days from the first start. After
// that, documents still open and can be viewed and edited, but saving,
// exporting and writing asset files need a key (writesAllowed()). Autosave
// and crash recovery keep working, so no work is ever lost.

#include "acestudio/core/Result.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace acestudio {

inline constexpr int kTrialDays = 14;

struct LicenceKey {
    std::string id;          // AceGears' reference for the key
    std::string name;        // licensee
    std::string email;
    std::string edition = "standard";
    std::string issued;      // YYYY-MM-DD
    std::string expires;     // YYYY-MM-DD, or empty: does not expire
};

using LicencePublicKey = std::array<std::uint8_t, 32>;
using LicenceSecretKey = std::array<std::uint8_t, 32>;   // the Ed25519 seed

// The key the app checks against (the release key, or the development key).
const LicencePublicKey& licencePublicKey();
bool usingDevelopmentLicenceKey();
// The development key's secret half (public on purpose; tests and local builds).
const LicenceSecretKey& developmentLicenceSeed();

// Checks a key's signature, format and expiry date (`today` = YYYY-MM-DD).
Result<LicenceKey> verifyLicenceKey(const std::string& text, const LicencePublicKey& publicKey, const std::string& today);
Result<LicenceKey> verifyLicenceKey(const std::string& text);

enum class LicenceMode { Licensed, Trial, TrialEnded };

struct LicenceStatus {
    LicenceMode mode = LicenceMode::Trial;
    int trialDaysLeft = kTrialDays;       // Trial: whole days left (at least 1)
    std::optional<LicenceKey> key;        // Licensed
    std::string trialStarted;             // YYYY-MM-DD
    std::string problem;                  // why a saved key was not accepted (expired, damaged, ...)
};

// Reads the saved key (<dataDir>/licence.key) and the trial start (recorded
// on first use in <dataDir>/trial.json and, on Windows, the registry;
// the earlier one counts; tests pass useRegistry = false, and ACESTUDIO_DATA_DIR
// leaves the registry out too). A clock set back
// before the trial start ends the trial.
LicenceStatus licenceStatus(const std::filesystem::path& dataDir,
                            std::chrono::system_clock::time_point now = std::chrono::system_clock::now(), bool useRegistry = true);
// Verifies `text` and saves it as the key. Fails without saving a bad key.
Result<LicenceKey> installLicenceKey(const std::filesystem::path& dataDir, const std::string& text);
Status removeLicenceKey(const std::filesystem::path& dataDir);

inline bool writesAllowed(const LicenceStatus& s) { return s.mode != LicenceMode::TrialEnded; }
// The message shown when a write is refused, naming the action ("Exporting").
std::string licenceRequiredMessage(const std::string& action);
std::string describeLicence(const LicenceStatus& s);

// For AceGears' key tool (tools/licence): never needed by the app.
LicencePublicKey licencePublicKeyFor(const LicenceSecretKey& seed);
std::string signLicenceKey(const LicenceKey& key, const LicenceSecretKey& seed);
std::string toHex(const std::uint8_t* data, std::size_t size);
std::optional<std::array<std::uint8_t, 32>> key32FromHex(const std::string& hex);

} // namespace acestudio
