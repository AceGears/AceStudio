#include <catch2/catch_test_macros.hpp>

#include "TestSupport.h"
#include "acestudio/core/FileUtil.h"
#include "acestudio/core/Log.h"

#include <filesystem>
#include <string>
#include <vector>

using namespace acestudio;
namespace fs = std::filesystem;

TEST_CASE("Logger writes / filters and notifies", "[log]") {
    TempDir dir;
    auto& log = Logger::instance();
    log.configure({dir / "logs" / "acestudio.log", 1u << 20, 3, false, LogLevel::Info});

    std::vector<std::string> seen;
    log.setListener([&](LogLevel, std::string_view, std::string_view line) { seen.emplace_back(line); });

    ACE_LOG_DEBUG("test", "hidden");
    ACE_LOG_INFO("test", "hello");
    ACE_LOG_ERROR("test", "boom");
    log.flush();

    REQUIRE(seen.size() == 2);
    CHECK(seen[0].find("[INFO] [test] hello") != std::string::npos);

    auto bytes = readFileBytes(dir / "logs" / "acestudio.log");
    REQUIRE(bytes);
    const std::string text(bytes.value().begin(), bytes.value().end());
    CHECK(text.find("hello") != std::string::npos);
    CHECK(text.find("boom") != std::string::npos);
    CHECK(text.find("hidden") == std::string::npos);

    log.setListener({});
    log.configure({});
}

TEST_CASE("Logger rotates files at the size limit", "[log]") {
    TempDir dir;
    auto& log = Logger::instance();
    const auto file = dir / "rot.log";
    log.configure({file, 200, 2, false, LogLevel::Info});
    for (int i = 0; i < 50; ++i) ACE_LOG_INFO("rotate", "line number " + std::to_string(i));
    log.flush();
    CHECK(fs::exists(file));
    CHECK(fs::exists(file.string() + ".1"));
    CHECK(fs::exists(file.string() + ".2"));
    CHECK_FALSE(fs::exists(file.string() + ".3"));
    log.configure({});
}
