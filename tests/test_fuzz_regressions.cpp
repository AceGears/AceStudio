// Inputs the fuzzers found (tests/fuzz/regressions), kept as regression tests.
#include "TestSupport.h"

#include "McpProtocol.h"

#include "acestudio/core/AnimCodec.h"
#include "acestudio/core/FileUtil.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <vector>

using namespace acestudio;

namespace {
std::vector<std::uint8_t> regression(const char* name) {
    auto b = readFileBytes(std::filesystem::path(ACESTUDIO_SOURCE_DIR) / "tests" / "fuzz" / "regressions" / name, 1u << 20);
    REQUIRE(b);
    return b.value();
}
} // namespace

TEST_CASE("Fuzz regression: a small GIF asking for gigabytes is refused quickly", "[security]") {
    const auto bytes = regression("gif-oom.gif");
    const auto start = std::chrono::steady_clock::now();
    auto r = decodeGif(bytes);
    CHECK_FALSE(r);
    CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds(2));
    // A GIF claiming 65535x65535 is refused before decoding.
    std::vector<std::uint8_t> big{'G', 'I', 'F', '8', '9', 'a', 0xff, 0xff, 0xff, 0xff, 0, 0, 0, 0x3b};
    CHECK_FALSE(decodeGif(big));
}

TEST_CASE("Fuzz regression: malformed MCP parameters give errors, not crashes", "[security]") {
    using nlohmann::json;
    mcp::McpServer s({}, {});
    const json meta{{"io.modelcontextprotocol/protocolVersion", "2026-07-28"}, {"io.modelcontextprotocol/clientCapabilities", json::object()}};
    for (const json& params : {json("text"), json(5), json::array({1, 2}), json{{"_meta", "x"}}, json{{"_meta", meta}, {"notifications", 3}},
                               json{{"_meta", meta}, {"name", 7}}, json{{"_meta", meta}, {"uri", json::object()}}}) {
        for (const char* method : {"subscriptions/listen", "tools/call", "resources/read", "prompts/get", "completion/complete", "tasks/get",
                                   "initialize", "server/discover"}) {
            INFO(method << " " << params.dump());
            const json msg{{"jsonrpc", "2.0"}, {"id", 1}, {"method", method}, {"params", params}};
            mcp::Connection c;
            auto r = s.handle(msg, c);
            if (r) CHECK(r->is_object());
            auto l = s.listenRequest(msg);
            if (auto* e = std::get_if<json>(&l)) CHECK(e->contains("error"));
        }
    }
    CHECK_FALSE(regression("mcp-listen-params.json").empty());
}
