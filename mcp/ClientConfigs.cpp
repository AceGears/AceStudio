#include "ClientConfigs.h"

#include <nlohmann/json.hpp>

namespace acestudio::mcp {

namespace {

std::string utf8(const std::filesystem::path& p) {
    const auto u = p.u8string();
    return std::string(u.begin(), u.end());
}

// TOML literal string: no escapes, so Windows paths stay readable.
std::string tomlLiteral(const std::string& s) {
    if (s.find('\'') == std::string::npos) return "'" + s + "'";
    return nlohmann::json(s).dump();   // a basic string with JSON-compatible escapes
}

std::string shellQuote(const std::string& s) { return "\"" + s + "\""; }

} // namespace

std::vector<ClientConfig> clientConfigs(const std::filesystem::path& mcpExe) {
    using nlohmann::ordered_json;
    const std::string exe = utf8(mcpExe);
    std::vector<ClientConfig> out;

    ordered_json claude;
    claude["mcpServers"]["acestudio"] = {{"type", "stdio"}, {"command", exe}, {"args", {"--root", "${CLAUDE_PROJECT_DIR:-.}"}}};
    out.push_back({"claude-code", "Claude Code", ".mcp.json in your project folder (shared), or run the command below",
                   claude.dump(2) + "\n\n# Or, for every project (run in a terminal):\nclaude mcp add --transport stdio --scope user acestudio -- " +
                       shellQuote(exe) + "\n",
                   "Claude Code asks you to approve project servers the first time. The tools can use files in the project "
                   "folder only."});

    ordered_json cursor;
    cursor["mcpServers"]["acestudio"] = {{"type", "stdio"}, {"command", exe}, {"args", {"--root", "${workspaceFolder}"}}};
    out.push_back({"cursor", "Cursor", ".cursor/mcp.json in your project folder (or ~/.cursor/mcp.json for every project)",
                   cursor.dump(2) + "\n",
                   "The tools can use files in the project folder only."});

    ordered_json vscode;
    vscode["servers"]["acestudio"] = {{"type", "stdio"}, {"command", exe}, {"args", {"--root", "${workspaceFolder}"}}};
    out.push_back({"vscode", "VS Code (GitHub Copilot)", ".vscode/mcp.json in your workspace (or MCP: Open User Configuration)",
                   vscode.dump(2) + "\n",
                   "VS Code uses a top-level \"servers\" key, unlike the other clients. The tools can use files in the "
                   "workspace folder only."});

    std::string codex = "[mcp_servers.acestudio]\ncommand = " + tomlLiteral(exe) +
                        "\nargs = []\n# Local AI generation can take minutes (Codex's default is 60 s).\n"
                        "tool_timeout_sec = 900\nstartup_timeout_sec = 20\n";
    out.push_back({"codex", "Codex", "~/.codex/config.toml (or .codex/config.toml in a trusted project)", codex,
                   "The tools can use files in the folder Codex starts the server in. To choose folders, put them in args: "
                   "args = ['--root', 'C:\\path\\to\\project']."});
    ordered_json opencode;
    opencode["$schema"] = "https://opencode.ai/config.json";
    opencode["mcp"]["acestudio"] = {{"type", "local"}, {"command", {exe}}, {"enabled", true}, {"timeout", 900000}};
    out.push_back({"opencode", "OpenCode", "opencode.json in your project folder (or ~/.config/opencode/opencode.json)",
                   opencode.dump(2) + "\n",
                   "OpenCode starts the server in the project folder; the tools use files there. \"timeout\" is in "
                   "milliseconds (local AI generation can take minutes)."});
    return out;
}

} // namespace acestudio::mcp
