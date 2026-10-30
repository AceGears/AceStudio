#include <catch2/catch_test_macros.hpp>

#include "TestSupport.h"
#include "acestudio/core/AiWorker.h"
#include "acestudio/core/FileUtil.h"
#include "acestudio/core/ModelCatalog.h"

#include <fstream>
#include <random>

using namespace acestudio;
using namespace acestudio::ai;
using nlohmann::json;
using namespace std::chrono_literals;

namespace {
json miniCatalog(const std::string& url, std::uint64_t size, const std::string& sha) {
    return json{{"schemaVersion", 1},
                {"licenses", {{"MIT", {{"name", "MIT License"}, {"commercialUse", true}}}}},
                {"models", json::array({json{{"id", "tiny"}, {"name", "Tiny"}, {"task", "background.remove"},
                                             {"license", "MIT"}, {"entry", "w/model.bin"},
                                             {"files", json::array({json{{"path", "w/model.bin"}, {"url", url},
                                                                         {"size", size}, {"sha256", sha}}})}}})}};
}
void writeFile(const std::filesystem::path& p, const std::string& data) {
    std::filesystem::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << data;
}
} // namespace

TEST_CASE("Shipped model catalogue is valid and fully licensed", "[ai][models]") {
    auto cat = ModelCatalog::load(ACESTUDIO_SOURCE_DIR "/ai_worker/catalog/models.json");
    if (!cat) FAIL(cat.error().message);
    REQUIRE(cat.value().models().size() >= 5);
    for (const auto& m : cat.value().models()) {
        INFO(m.id);
        CHECK(m.license.commercialUse);         // the default set excludes non-commercial models
        CHECK_FALSE(m.license.obligations.empty());
        CHECK_FALSE(m.files.empty());
        if (m.pinned) CHECK(m.knownSize() > 0);
    }
    const ModelEntry* lite = cat.value().find("birefnet-lite");
    REQUIRE(lite);
    CHECK(lite->pinned);
    CHECK(lite->files[0].size == 224005088u);
    CHECK(cat.value().forTask("upscale.esrgan").size() == 2);
    const ModelEntry* sdxl = cat.value().find("sdxl-base-1.0");
    REQUIRE(sdxl);
    CHECK_FALSE(sdxl->pinned);                  // honest about what could not be measured
    CHECK_FALSE(sdxl->fileListVerified);
}

TEST_CASE("Model catalogue rejects unsafe or unknown entries", "[ai][models]") {
    const std::string sha(64, 'a');
    CHECK(ModelCatalog::parse(miniCatalog("https://x/y", 1, sha)));
    CHECK_FALSE(ModelCatalog::parse(miniCatalog("http://insecure/y", 1, sha)));
    auto bad = miniCatalog("https://x/y", 1, sha);
    bad["models"][0]["files"][0]["path"] = "../../evil.dll";
    CHECK_FALSE(ModelCatalog::parse(bad));
    bad = miniCatalog("https://x/y", 1, sha);
    bad["models"][0]["license"] = "NC-ONLY";
    CHECK_FALSE(ModelCatalog::parse(bad));
    bad = miniCatalog("https://x/y", 1, sha);
    bad["models"][0]["id"] = "../x";
    CHECK_FALSE(ModelCatalog::parse(bad));
    CHECK_FALSE(ModelCatalog::parse(json{{"schemaVersion", 2}, {"models", json::array()}}));
}

TEST_CASE("Downloads require licence approval; install state follows the files", "[ai][models]") {
    TempDir dir;
    auto cat = ModelCatalog::parse(miniCatalog("https://example.invalid/m", 5, std::string(64, 'b'))).value();
    const ModelEntry& m = *cat.find("tiny");
    ModelStore store(dir.path() / "models");
    CHECK(store.state(m) == InstallState::NotInstalled);
    auto refused = store.downloadParams(m);
    REQUIRE_FALSE(refused);
    CHECK(refused.error().code == ErrorCode::PermissionDenied);
    REQUIRE(store.approve(m));
    CHECK(store.isApproved(m));
    auto params = store.downloadParams(m);
    REQUIRE(params);
    CHECK(params.value()["license_accepted"] == "MIT");
    CHECK(params.value()["files"][0]["sha256"] == std::string(64, 'b'));

    writeFile(store.dir(m) / "w" / "model.bin.part", "12");
    CHECK(store.state(m) == InstallState::Partial);
    std::filesystem::remove(store.dir(m) / "w" / "model.bin.part");
    writeFile(store.dir(m) / "w" / "model.bin", "12345");
    CHECK(store.state(m) == InstallState::Partial);    // present but never verified
    CHECK_FALSE(store.recordDownload(m, {{"w/model.bin", std::string(64, 'c')}}));   // catalogue mismatch
    REQUIRE(store.recordDownload(m, {{"w/model.bin", std::string(64, 'b')}}));
    CHECK(store.state(m) == InstallState::Installed);
    CHECK(store.entryPath(m) == store.dir(m) / "w" / "model.bin");
    CHECK(store.bytesOnDisk(m) == 5);

    // A licence change in a newer catalogue needs a fresh approval.
    auto changed = miniCatalog("https://example.invalid/m", 5, std::string(64, 'b'));
    changed["licenses"]["Apache-2.0"] = {{"name", "Apache"}, {"commercialUse", true}};
    changed["models"][0]["license"] = "Apache-2.0";
    auto cat2 = ModelCatalog::parse(changed).value();
    CHECK_FALSE(store.isApproved(*cat2.find("tiny")));

    REQUIRE(store.remove(m));
    CHECK(store.state(m) == InstallState::NotInstalled);
    CHECK(store.isApproved(m));   // approval survives removal
    store.revoke(m);
    CHECK_FALSE(store.isApproved(m));
}

TEST_CASE("Hashes recorded at first download pin later downloads", "[ai][models]") {
    TempDir dir;
    auto cat = ModelCatalog::parse(miniCatalog("https://example.invalid/m", 0, "")).value();
    const ModelEntry& m = *cat.find("tiny");
    CHECK_FALSE(m.pinned);
    ModelStore store(dir.path());
    REQUIRE(store.approve(m));
    CHECK(store.downloadParams(m).value()["files"][0]["sha256"] == "");
    writeFile(store.dir(m) / "w" / "model.bin", "abc");
    REQUIRE(store.recordDownload(m, {{"w/model.bin", std::string(64, 'd')}}));
    CHECK(store.downloadParams(m).value()["files"][0]["sha256"] == std::string(64, 'd'));
}

TEST_CASE("End to end: approved model downloads through the worker and installs", "[ai][models]") {
    auto py = findPython();
    if (!py) SKIP("Python 3.10+ not found");
    TempDir dir;
    const std::string payload = [] {
        std::string s(300000, '\0');
        std::mt19937 rng(7);
        for (auto& c : s) c = static_cast<char>(rng());
        return s;
    }();
    writeFile(dir.path() / "www" / "model.bin", payload);
    // The worker computes the hash; the catalogue entry is unpinned here.
    std::mt19937 rng(std::random_device{}());
    const int port = 41000 + static_cast<int>(rng() % 20000);
    ProcessSpec http;
    http.program = py.value().executable;
    http.args = py.value().prefixArgs;
    http.args.insert(http.args.end(), {"-m", "http.server", std::to_string(port), "--bind", "127.0.0.1",
                                       "--directory", (dir.path() / "www").string()});
    auto server = Subprocess::start(http);
    REQUIRE(server);
    std::this_thread::sleep_for(1500ms);
    // A local test server is plain http; the catalogue only allows https,
    // so build the entry and params directly.
    auto cat = ModelCatalog::parse(miniCatalog("https://placeholder.invalid/m", payload.size(), "")).value();
    const ModelEntry& m = *cat.find("tiny");
    ModelStore store(dir.path() / "models");
    REQUIRE(store.approve(m));
    json params = store.downloadParams(m).value();
    params["files"][0]["url"] = "http://127.0.0.1:" + std::to_string(port) + "/model.bin";

    WorkerOptions o;
    o.process = workerProcess(py.value().executable, ACESTUDIO_SOURCE_DIR "/ai_worker");
    o.process.args.insert(o.process.args.begin(), py.value().prefixArgs.begin(), py.value().prefixArgs.end());
    o.jobsRoot = dir.path() / "jobs";
    WorkerSupervisor sup(o);
    sup.start();
    auto id = sup.submit("model.download", params, "Download Tiny").value();
    REQUIRE(sup.runUntil([&] { return isFinished(sup.job(id)->state); }, 60s));
    const Job* j = sup.job(id);
    INFO(j->error);
    REQUIRE(j->state == JobState::Succeeded);
    REQUIRE(store.recordDownload(m, j->info["sha256"]));
    CHECK(store.state(m) == InstallState::Installed);
    CHECK(readFileBytes(store.entryPath(m)).value().size() == payload.size());
    CHECK(store.manifest(m)["files"]["w/model.bin"]["sha256"].get<std::string>().size() == 64);
}
