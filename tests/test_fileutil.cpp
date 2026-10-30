#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <fstream>

#include "TestSupport.h"
#include "acestudio/core/FileUtil.h"

#include <cstdlib>
#include <fstream>
#include <filesystem>
#include <string>

using namespace acestudio;
namespace fs = std::filesystem;

namespace {
std::vector<std::uint8_t> bytesOf(const std::string& s) { return {s.begin(), s.end()}; }
std::string read(const fs::path& p) {
    auto r = readFileBytes(p);
    REQUIRE(r);
    return {r.value().begin(), r.value().end()};
}
} // namespace

TEST_CASE("Atomic write creates / replaces and backs up", "[fileutil]") {
    TempDir dir;
    const auto target = dir / "sub" / "data.txt";
    const auto backup = dir / "data.bak";

    REQUIRE(writeFileAtomic(target, bytesOf("first"), backup));      // creates parent dir
    CHECK(read(target) == "first");
    CHECK_FALSE(fs::exists(backup));

    REQUIRE(writeFileAtomic(target, bytesOf("second"), backup));
    CHECK(read(target) == "second");
    CHECK(read(backup) == "first");

    REQUIRE(writeFileAtomic(target, bytesOf("")));                    // empty files are valid
    CHECK(read(target).empty());

    // Only the target remains in its directory: temp files were cleaned up.
    int n = 0;
    for (const auto& e : fs::directory_iterator(target.parent_path())) { (void)e; ++n; }
    CHECK(n == 1);
}

TEST_CASE("readFileBytes errors", "[fileutil]") {
    TempDir dir;
    CHECK(readFileBytes(dir / "missing").error().code == ErrorCode::NotFound);
    REQUIRE(writeFileAtomic(dir / "big", bytesOf("0123456789")));
    CHECK(readFileBytes(dir / "big", 5).error().code == ErrorCode::InvalidArgument);
    CHECK_FALSE(writeFileAtomic({}, bytesOf("x")));
}

TEST_CASE("Timestamp format", "[fileutil]") {
    const std::string t = nowUtcIso8601();
    REQUIRE(t.size() == 24);
    CHECK(t[4] == '-');
    CHECK(t[10] == 'T');
    CHECK(t[19] == '.');
    CHECK(t.back() == 'Z');
}

TEST_CASE("userDataDir honours override", "[fileutil]") {
    TempDir dir;
    const auto custom = dir / "profile";
#ifdef _WIN32
    _putenv_s("ACESTUDIO_DATA_DIR", custom.string().c_str());
#else
    setenv("ACESTUDIO_DATA_DIR", custom.c_str(), 1);
#endif
    auto p = userDataDir();
    REQUIRE(p);
    CHECK(p.value() == custom);
    CHECK(fs::is_directory(custom));
#ifdef _WIN32
    _putenv_s("ACESTUDIO_DATA_DIR", "");
#else
    unsetenv("ACESTUDIO_DATA_DIR");
#endif
}

TEST_CASE("Files can be read while another handle has them open for writing", "[fileutil]") {
    // Regression: on Windows, _wfopen_s opens files exclusively, so reading a
    // file that another program (or our own logger) holds open failed.
    TempDir dir;
    const auto path = dir / "busy.txt";
    REQUIRE(writeFileAtomic(path, bytesOf("data")));
    std::ofstream writer(path, std::ios::app);   // stays open during the read
    REQUIRE(writer.is_open());
    auto r = readFileBytes(path);
    REQUIRE(r);
    CHECK(r.value().size() == 4);
}

TEST_CASE("Portable mode keeps data next to the executable", "[fileutil]") {
    namespace fs = std::filesystem;
    const fs::path exe = acestudio::executableDir();
    REQUIRE_FALSE(exe.empty());
    const fs::path marker = exe / "portable";
    REQUIRE_FALSE(fs::exists(marker));
    // ACESTUDIO_DATA_DIR (set by other tests' environment) still wins, so
    // clear it for this check and restore afterwards.
    const char* old = std::getenv("ACESTUDIO_DATA_DIR");
    const std::string saved = old ? old : "";
#ifdef _WIN32
    _putenv_s("ACESTUDIO_DATA_DIR", "");
#else
    unsetenv("ACESTUDIO_DATA_DIR");
#endif
    struct Cleanup {
        fs::path marker, data;
        std::string saved;
        ~Cleanup() {
            std::error_code ec;
            fs::remove(marker, ec);
            fs::remove_all(data, ec);
#ifdef _WIN32
            if (!saved.empty()) _putenv_s("ACESTUDIO_DATA_DIR", saved.c_str());
#else
            if (!saved.empty()) setenv("ACESTUDIO_DATA_DIR", saved.c_str(), 1);
#endif
        }
    } cleanup{marker, exe / "data", saved};
    CHECK_FALSE(acestudio::isPortable());
    { std::ofstream(marker) << ""; }
    CHECK(acestudio::isPortable());
    CHECK(fs::equivalent(acestudio::userDataDir().value(), exe / "data"));
    CHECK(fs::equivalent(acestudio::userLocalDataDir().value(), exe / "data"));
}
