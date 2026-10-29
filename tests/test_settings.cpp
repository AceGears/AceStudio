#include <catch2/catch_test_macros.hpp>

#include "TestSupport.h"
#include "acestudio/core/FileUtil.h"
#include "acestudio/core/Settings.h"

#include <filesystem>
#include <string>

using namespace acestudio;
namespace fs = std::filesystem;

namespace {
void writeText(const fs::path& p, const std::string& s) {
    REQUIRE(writeFileAtomic(p, std::vector<std::uint8_t>(s.begin(), s.end())));
}
} // namespace

TEST_CASE("Settings defaults and typed access", "[settings]") {
    Settings s;
    CHECK(s.get<std::string>("general.theme") == "dark");
    CHECK(s.get<int>("general.autosaveIntervalSec") == 120);
    CHECK(s.get("no.such.key").is_null());
    CHECK(s.hasKey("canvas.zoomStep"));
    CHECK_FALSE(s.isDirty());
}

TEST_CASE("Settings reject unknown keys and wrong types", "[settings]") {
    Settings s;
    CHECK_FALSE(s.set("no.such.key", 1));
    CHECK_FALSE(s.set("general.theme", 5));
    CHECK_FALSE(s.set("general.autosaveIntervalSec", 1.5));     // int stays int
    CHECK(s.set("canvas.zoomStep", 2));                           // float accepts int
    CHECK(s.set("general.theme", "light"));
    CHECK(s.get<std::string>("general.theme") == "light");
    CHECK(s.isDirty());
    s.reset("general.theme");
    CHECK(s.get<std::string>("general.theme") == "dark");
}

TEST_CASE("Settings persist and reload", "[settings]") {
    TempDir dir;
    const auto path = dir / "settings.json";
    {
        Settings s;
        REQUIRE(s.load(path));                                    // missing file is fine
        REQUIRE(s.set("general.theme", "light"));
        REQUIRE(s.set("animation.defaultFps", 24));
        REQUIRE(s.save());
        CHECK_FALSE(s.isDirty());
    }
    Settings again;
    REQUIRE(again.load(path));
    CHECK(again.get<std::string>("general.theme") == "light");
    CHECK(again.get<int>("animation.defaultFps") == 24);
}

TEST_CASE("Corrupt settings are quarantined / not silently lost", "[settings]") {
    TempDir dir;
    const auto path = dir / "settings.json";
    writeText(path, "{ this is not json");
    Settings s;
    std::string warning;
    REQUIRE(s.load(path, &warning));
    CHECK_FALSE(warning.empty());
    CHECK_FALSE(fs::exists(path));
    bool quarantined = false;
    for (const auto& e : fs::directory_iterator(dir.path()))
        if (e.path().filename().string().rfind("settings.json.corrupt-", 0) == 0) quarantined = true;
    CHECK(quarantined);
    CHECK(s.get<std::string>("general.theme") == "dark");
}

TEST_CASE("Settings skip unknown and mistyped stored values", "[settings]") {
    TempDir dir;
    const auto path = dir / "settings.json";
    writeText(path, R"({"schemaVersion":1,"values":{"general.theme":"light","general.autosaveIntervalSec":"often","gone.key":1}})");
    Settings s;
    REQUIRE(s.load(path));
    CHECK(s.get<std::string>("general.theme") == "light");
    CHECK(s.get<int>("general.autosaveIntervalSec") == 120);
}

TEST_CASE("Settings from a newer schema are not overwritten", "[settings]") {
    TempDir dir;
    const auto path = dir / "settings.json";
    writeText(path, R"({"schemaVersion":42,"values":{}})");
    Settings s;
    auto st = s.load(path);
    REQUIRE_FALSE(st);
    CHECK(st.error().code == ErrorCode::UnsupportedVersion);
    CHECK(fs::exists(path));
}
