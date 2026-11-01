// Licensing: Ed25519 keys (RFC 8032 vectors), key format, tampering, expiry
// and the 14-day trial.
#include "TestSupport.h"

#include "acestudio/core/Licence.h"

#include <catch2/catch_test_macros.hpp>

#include <fstream>

using namespace acestudio;
using namespace std::chrono;

namespace {
LicenceSecretKey seedFrom(const char* hex) { return *key32FromHex(hex); }
const LicenceSecretKey kTestSeed = seedFrom("4ccd089b28ff96da9db6c346ec114e0f5b8a319f35aba624da8cf6ed4fb8a6fb");

LicenceKey sample() {
    LicenceKey k;
    k.id = "AG-0001";
    k.name = "Test Studio";
    k.email = "test@example.com";
    k.issued = "2026-11-01";
    return k;
}
system_clock::time_point onDay(int d) { return system_clock::time_point(milliseconds(1793491200000LL + d * 86400000LL)); }   // 2026-11-01
} // namespace

TEST_CASE("Licence: Ed25519 matches RFC 8032", "[licence]") {
    // RFC 8032 section 7.1, test 1 (empty message) and test 2 (one byte).
    CHECK(toHex(licencePublicKeyFor(seedFrom("9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60")).data(), 32) ==
          "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a");
    CHECK(toHex(licencePublicKeyFor(kTestSeed).data(), 32) == "3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c");
}

TEST_CASE("Licence: keys verify, and changes are caught", "[licence]") {
    const LicencePublicKey pub = licencePublicKeyFor(kTestSeed);
    const std::string key = signLicenceKey(sample(), kTestSeed);
    CHECK(key.rfind("ACESTUDIO-1.", 0) == 0);
    auto k = verifyLicenceKey(key, pub, "2026-11-02");
    REQUIRE(k);
    CHECK(k.value().name == "Test Studio");
    CHECK(k.value().edition == "standard");
    // Pasted with line breaks and spaces: still fine.
    std::string wrapped = key.substr(0, 30) + "\n  " + key.substr(30, 40) + "\r\n" + key.substr(70);
    CHECK(verifyLicenceKey(wrapped, pub, "2026-11-02"));
    // Any change, a different signer or a truncated key fails.
    std::string changed = key;
    changed[20] = changed[20] == 'A' ? 'B' : 'A';
    CHECK_FALSE(verifyLicenceKey(changed, pub, "2026-11-02"));
    CHECK_FALSE(verifyLicenceKey(key, licencePublicKeyFor(seedFrom("9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60")), "2026-11-02"));
    CHECK_FALSE(verifyLicenceKey(key.substr(0, key.size() - 5), pub, "2026-11-02"));
    CHECK_FALSE(verifyLicenceKey("hello", pub, "2026-11-02"));
    CHECK_FALSE(verifyLicenceKey("", pub, "2026-11-02"));
    // Expiry.
    LicenceKey e = sample();
    e.expires = "2027-01-31";
    const std::string ek = signLicenceKey(e, kTestSeed);
    CHECK(verifyLicenceKey(ek, pub, "2027-01-31"));
    auto expired = verifyLicenceKey(ek, pub, "2027-02-01");
    REQUIRE_FALSE(expired);
    CHECK(expired.error().message.find("expired") != std::string::npos);
}

TEST_CASE("Licence: the 14-day trial, then read-only", "[licence]") {
    TempDir dir;
    auto s = licenceStatus(dir.path(), onDay(0), false);
    CHECK(s.mode == LicenceMode::Trial);
    CHECK(s.trialDaysLeft == 14);
    CHECK(writesAllowed(s));
    CHECK(licenceStatus(dir.path(), onDay(13), false).trialDaysLeft == 1);
    auto ended = licenceStatus(dir.path(), onDay(14), false);
    CHECK(ended.mode == LicenceMode::TrialEnded);
    CHECK_FALSE(writesAllowed(ended));
    CHECK(licenceRequiredMessage("Exporting").find("Exporting needs a licence key") == 0);
    // Setting the clock back does not restart it.
    CHECK(licenceStatus(dir.path(), onDay(-5), false).mode == LicenceMode::TrialEnded);
    CHECK(licenceStatus(dir.path(), onDay(0), false).trialStarted == "2026-11-01");

    // A development-key build accepts development keys; a saved bad key is reported, not used.
    std::ofstream(dir.path() / "licence.key") << "ACESTUDIO-1.garbage.garbage";
    auto bad = licenceStatus(dir.path(), onDay(20), false);
    CHECK(bad.mode == LicenceMode::TrialEnded);
    CHECK_FALSE(bad.problem.empty());
    CHECK_FALSE(installLicenceKey(dir.path(), "nonsense"));

    if (usingDevelopmentLicenceKey()) {
        auto ok = installLicenceKey(dir.path(), signLicenceKey(sample(), developmentLicenceSeed()));
        REQUIRE(ok);
        auto lic = licenceStatus(dir.path(), onDay(20), false);
        CHECK(lic.mode == LicenceMode::Licensed);
        CHECK(writesAllowed(lic));
        CHECK(describeLicence(lic).find("Licensed to Test Studio") == 0);
        REQUIRE(removeLicenceKey(dir.path()));
        CHECK(licenceStatus(dir.path(), onDay(20), false).mode == LicenceMode::TrialEnded);
    }
}
