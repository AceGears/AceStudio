#pragma once

// Ready-to-paste acestudio-mcp setups for MCP clients (Phase 7), checked
// against each client's documentation in October 2026:
//   Claude Code  https://code.claude.com/docs/en/mcp           (.mcp.json, claude mcp add)
//   Cursor       https://cursor.com/docs/context/mcp           (.cursor/mcp.json)
//   VS Code      https://code.visualstudio.com/docs/agents/reference/mcp-configuration (.vscode/mcp.json)
//   Codex        https://learn.chatgpt.com/docs/extend/mcp     (~/.codex/config.toml)
//   OpenCode     https://opencode.ai/docs/mcp-servers          (opencode.json, checked November 2026)

#include <filesystem>
#include <string>
#include <vector>

namespace acestudio::mcp {

struct ClientConfig {
    std::string id;          // claude-code, cursor, vscode, codex, opencode
    std::string name;        // display name
    std::string file;        // where it goes
    std::string content;     // what to put there
    std::string note;        // one or two sentences for the user
};

std::vector<ClientConfig> clientConfigs(const std::filesystem::path& mcpExe);

} // namespace acestudio::mcp
