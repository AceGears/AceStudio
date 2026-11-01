#pragma once

// The AceStudio tools of acestudio-mcp. File tools run acestudio-cli (so the
// MCP server and the CLI share one implementation); app_* tools talk to a
// running AceStudio over the local bridge (core/AppBridge.h).

#include "McpProtocol.h"

#include "acestudio/core/Result.h"

#include <filesystem>
#include <functional>
#include <vector>

namespace acestudio::mcp {

struct ToolEnv {
    std::filesystem::path cli;                         // acestudio-cli executable
    std::vector<std::filesystem::path> roots;          // folders the tools may read and write
    // One request to the running app; an error when it is not running.
    std::function<Result<json>(const std::string& method, const json& params, int timeoutMs)> bridge;
};

std::vector<Tool> aceStudioTools(const ToolEnv& env);

// Exposed for tests.
std::string base64(const std::vector<std::uint8_t>& bytes);
// Resolves `p` (relative to the first root) and checks it is inside a root.
Result<std::filesystem::path> resolveInRoots(const std::vector<std::filesystem::path>& roots, const std::string& p);

} // namespace acestudio::mcp
