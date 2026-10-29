#include <catch2/catch_test_macros.hpp>

#include "TestSupport.h"
#include "acestudio/core/FileUtil.h"
#include "acestudio/core/ProjectIO.h"
#include "acestudio/core/Recovery.h"

#include <nlohmann/json.hpp>

using namespace acestudio;
namespace fs = std::filesystem;

namespace {
// Rewrites a recovery entry's metadata so it appears to come from another
// (dead or live) process.
void setWriterPid(const fs::path& dir, const std::string& id, std::int64_t pid) {
    auto bytes = readFileBytes(dir / (id + ".json")).value();
    auto meta = nlohmann::json::parse(bytes.begin(), bytes.end());
    meta["pid"] = pid;
    const std::string text = meta.dump();
    REQUIRE(writeFileAtomic(dir / (id + ".json"), std::vector<std::uint8_t>(text.begin(), text.end())));
}
} // namespace

TEST_CASE("Recovery ids are unique and filesystem safe", "[recovery]") {
    const auto a = RecoveryStore::newId(), b = RecoveryStore::newId();
    CHECK(a != b);
    CHECK(a.rfind("doc-", 0) == 0);
}

TEST_CASE("Recovery snapshots round-trip and are only orphaned once the writer is gone", "[recovery]") {
    TempDir dir;
    RecoveryStore store(dir / "recovery");
    auto doc = Document::create(4, 4).value();
    const auto l = doc.addLayer("ink").value();
    REQUIRE(doc.setCel(l, 0, Cel{Image(1, 1, Rgba8::opaque(9, 9, 9)), 2, 2}));

    const auto id = RecoveryStore::newId();
    REQUIRE(store.write(id, doc, "C:/art/hero.acestudio", "hero.acestudio"));

    // Written by this (live) process: not offered for recovery.
    CHECK(store.orphaned().empty());

    // Simulate a crashed writer with a pid that cannot be running.
    setWriterPid(store.directory(), id, 0x7FFFFFF0);
    auto entries = store.orphaned();
    REQUIRE(entries.size() == 1);
    CHECK(entries[0].originalPath == "C:/art/hero.acestudio");
    CHECK(entries[0].title == "hero.acestudio");
    auto recovered = loadProject(entries[0].projectFile);
    REQUIRE(recovered);
    CHECK(recovered.value().composite(0).pixel(2, 2) == Rgba8::opaque(9, 9, 9));

    REQUIRE(store.remove(id));
    CHECK(store.orphaned().empty());
    CHECK_FALSE(fs::exists(store.directory() / (id + ".acestudio")));
}

TEST_CASE("Recovery store rejects unsafe ids and reports bad metadata", "[recovery]") {
    TempDir dir;
    RecoveryStore store(dir.path());
    auto doc = Document::create(1, 1).value();
    CHECK_FALSE(store.write("../escape", doc, "", ""));
    CHECK_FALSE(store.write("", doc, "", ""));
    CHECK_FALSE(store.remove("..\\x"));

    const std::string junk = "not json";
    REQUIRE(writeFileAtomic(dir / "doc-bad.json", std::vector<std::uint8_t>(junk.begin(), junk.end())));
    std::vector<std::string> problems;
    CHECK(store.orphaned(&problems).empty());
    CHECK(problems.size() == 1);
}

TEST_CASE("Process liveness", "[recovery]") {
    CHECK(isProcessRunning(currentProcessId()));
    CHECK_FALSE(isProcessRunning(0));
    CHECK_FALSE(isProcessRunning(0x7FFFFFF0));
}
