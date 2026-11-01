// libFuzzer targets for everything AceStudio reads from untrusted input
// (Phase 8 security review). Build with clang:
//   cmake -S . -B build-fuzz -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_C_COMPILER=clang -DACESTUDIO_FUZZ=ON
// One executable per target (FUZZ_TARGET picks the function).

#include "acestudio/core/AnimCodec.h"
#include "acestudio/core/PaletteIO.h"
#include "acestudio/core/PngCodec.h"
#include "acestudio/core/ProjectIO.h"
#include "acestudio/core/Licence.h"
#include "acestudio/core/Script.h"

#ifdef FUZZ_MCP
#include "McpProtocol.h"
#endif

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

using namespace acestudio;

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const std::span<const std::uint8_t> bytes(data, size);
    const std::string_view text(reinterpret_cast<const char*>(data), size);
#if defined(FUZZ_PROJECT)
    (void)deserializeProject(bytes);
#elif defined(FUZZ_PNG)
    (void)decodePng(bytes);
#elif defined(FUZZ_GIF)
    (void)decodeGif(bytes);
#elif defined(FUZZ_APNG)
    (void)decodeApng(bytes);
#elif defined(FUZZ_WEBP)
    (void)decodeWebp(bytes);
#elif defined(FUZZ_PALETTE)
    // The first byte picks the format.
    if (size < 1) return 0;
    static const char* exts[] = {".gpl", ".pal", ".hex", ".txt", ".ase", ".act", ".json"};
    (void)parsePalette(text.substr(1), exts[data[0] % 7]);
#elif defined(FUZZ_LICENCE)
    (void)verifyLicenceKey(std::string(text), licencePublicKey(), "2026-11-01");
#elif defined(FUZZ_LUA)
    ScriptOptions o;
    o.limits.memoryBytes = 32u << 20;
    o.limits.seconds = 0.5;
    (void)runScript(std::string(text), "fuzz.lua", o);
#elif defined(FUZZ_MCP)
    static mcp::McpServer server({}, {});
    const auto msg = nlohmann::json::parse(text, nullptr, false);
    if (msg.is_discarded()) return 0;
    mcp::Connection c;
    (void)server.handle(msg, c);
    mcp::Connection c2;
    (void)server.handle(msg, c2);
    (void)server.listenRequest(msg);
#endif
    return 0;
}
