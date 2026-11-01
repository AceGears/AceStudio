#pragma once

// What acestudio-mcp offers: tools, prompts, resources and resource
// templates. File tools run acestudio-cli (so the MCP server and the CLI
// share one implementation); app_* tools and acestudio://app resources talk
// to a running AceStudio over the local bridge (core/AppBridge.h).

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
    // Called after a tool changed a resource (subscriptions).
    std::function<void(const std::string& uri)> resourceChanged;
};

struct ServerContent {
    std::vector<Tool> tools;
    std::vector<Prompt> prompts;
    std::vector<Resource> resources;
    std::vector<ResourceTemplate> templates;
};

ServerContent aceStudioContent(const ToolEnv& env);

// Resource URIs of the app's open document (subscribable).
inline constexpr const char* kAppDocumentUri = "acestudio://app/document";
inline constexpr const char* kAppFrameUri = "acestudio://app/document/frame.png";

// The Lua API reference (docs/lua-api.md, built in).
const std::string& luaApiReference();

// Exposed for tests.
std::string base64(const std::vector<std::uint8_t>& bytes);
// Resolves `p` (relative to the first root) and checks it is inside a root.
Result<std::filesystem::path> resolveInRoots(const std::vector<std::filesystem::path>& roots, const std::string& p);
// Files under the roots whose path (relative to the first root, '/'
// separators) starts with `prefix` and ends with one of `extensions`.
std::vector<std::string> completeFiles(const std::vector<std::filesystem::path>& roots, const std::string& prefix,
                                       const std::vector<std::string>& extensions, std::size_t max = 50);

} // namespace acestudio::mcp
