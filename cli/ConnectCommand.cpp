// acestudio-cli connect: registers acestudio-mcp with an AI coding client by
// editing that client's own configuration file (merged, never replaced; the
// previous file is kept as <file>.bak). Formats checked against each
// client's documentation, November 2026:
//   Claude Code     .mcp.json {"mcpServers": {...}}           https://code.claude.com/docs/en/mcp
//   Claude Desktop  claude_desktop_config.json {"mcpServers"}
//   Cursor          .cursor/mcp.json {"mcpServers": {...}}     https://cursor.com/docs/context/mcp
//   VS Code         .vscode/mcp.json {"servers": {...}}        https://code.visualstudio.com/docs/agents/reference/mcp-configuration
//   Codex           ~/.codex/config.toml [mcp_servers.<name>]  https://learn.chatgpt.com/docs/extend/mcp
//   OpenCode        opencode.json {"mcp": {"<name>": {"type": "local", "command": [...]}}}  https://opencode.ai/docs/mcp-servers

#include "CliCommon.h"

#include "acestudio/core/FileUtil.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace acestudio::cli {

namespace fs = std::filesystem;
using nlohmann::ordered_json;

namespace {

fs::path fromUtf8(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }

std::string utf8(const fs::path& p) {
    const auto u = p.u8string();
    return std::string(u.begin(), u.end());
}

std::string envOr(const char* name, const std::string& fallback) {
#ifdef _WIN32
    char* v = nullptr;
    std::size_t n = 0;
    std::string out = fallback;
    if (_dupenv_s(&v, &n, name) == 0 && v && *v) out = v;
    std::free(v);
    return out;
#else
    const char* v = std::getenv(name);
    return v && *v ? std::string(v) : fallback;
#endif
}

// Home folder; ACESTUDIO_CONNECT_HOME overrides it (tests).
fs::path home() {
    const std::string over = envOr("ACESTUDIO_CONNECT_HOME", "");
    if (!over.empty()) return fromUtf8(over);
#ifdef _WIN32
    return fromUtf8(envOr("USERPROFILE", "."));
#else
    return fromUtf8(envOr("HOME", "."));
#endif
}

fs::path appData() {
    const std::string over = envOr("ACESTUDIO_CONNECT_HOME", "");
    if (!over.empty()) return fromUtf8(over) / "AppData";
#ifdef _WIN32
    return fromUtf8(envOr("APPDATA", utf8(home() / "AppData" / "Roaming")));
#elif defined(__APPLE__)
    return home() / "Library" / "Application Support";
#else
    return fromUtf8(envOr("XDG_CONFIG_HOME", utf8(home() / ".config")));
#endif
}

Result<std::string> readText(const fs::path& p) {
    std::error_code ec;
    if (!fs::exists(p, ec)) return std::string();
    auto bytes = readFileBytes(p, 16u << 20);
    if (!bytes) return bytes.error();
    return std::string(bytes.value().begin(), bytes.value().end());
}

Status writeWithBackup(const fs::path& p, const std::string& text) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    const fs::path backup = fs::path(p).concat(".bak");
    return writeFileAtomic(p, std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(text.data()), text.size()),
                           fs::exists(p, ec) ? backup : fs::path());
}

// Sets (or removes) obj[section][name] in a JSON config, keeping everything else.
Result<std::string> mergeJson(const std::string& existing, const std::vector<std::string>& section, const std::string& name,
                              const ordered_json* server) {
    ordered_json root = ordered_json::object();
    if (!existing.empty()) {
        root = ordered_json::parse(existing, nullptr, false, /*ignore_comments=*/true);
        if (root.is_discarded() || !root.is_object())
            return Error(ErrorCode::CorruptData, "The existing file is not a JSON object; it was left unchanged.");
    }
    ordered_json* node = &root;
    for (const std::string& key : section) {
        if (!node->contains(key)) {
            if (!server) return root.dump(2) + "\n";   // nothing to remove
            (*node)[key] = ordered_json::object();
        }
        node = &(*node)[key];
        if (!node->is_object()) return Error(ErrorCode::CorruptData, "\"" + key + "\" in the existing file is not an object; it was left unchanged.");
    }
    if (server) (*node)[name] = *server;
    else node->erase(name);
    return root.dump(2) + "\n";
}

// TOML: replaces the [mcp_servers.<name>] table (and its sub-tables) or appends it.
std::string mergeToml(const std::string& existing, const std::string& name, const std::string* table) {
    std::istringstream in(existing);
    std::string line, out;
    bool skipping = false;
    const std::string head = "[mcp_servers." + name + "]", sub = "[mcp_servers." + name + ".";
    while (std::getline(in, line)) {
        std::string t = line;
        t.erase(0, t.find_first_not_of(" \t"));
        if (!t.empty() && t[0] == '[') skipping = t.rfind(head, 0) == 0 || t.rfind(sub, 0) == 0;
        if (!skipping) out += line + "\n";
    }
    while (out.size() >= 2 && out[out.size() - 1] == '\n' && out[out.size() - 2] == '\n') out.pop_back();
    if (table) out += (out.empty() ? "" : "\n") + *table;
    return out;
}

std::string tomlString(const std::string& s) {
    if (s.find('\'') == std::string::npos && s.find('\n') == std::string::npos) return "'" + s + "'";
    return nlohmann::json(s).dump();
}

fs::path defaultMcpExe() {
    const fs::path dir = executableDir();
#ifdef _WIN32
    return dir / "acestudio-mcp.exe";
#else
    return dir / "acestudio-mcp";
#endif
}

} // namespace

// connect <client> [--project DIR | --global] [--remove] [--dry-run] [--mcp PATH] [--name acestudio]
int cmdConnect(const Args& a, Output& out) {
    static const std::vector<std::string> clients{"claude", "claude-desktop", "codex", "cursor", "opencode", "vscode"};
    if (a.positional.size() != 2 || std::find(clients.begin(), clients.end(), a.positional[1]) == clients.end())
        return out.fail(kUsage, "connect: name a client: claude, claude-desktop, codex, cursor, opencode or vscode");
    const std::string client = a.positional[1];
    const bool remove = a.flags.count("remove") > 0;
    const bool dry = a.flags.count("dry-run") > 0;
    const std::string name = a.opt("name").value_or("acestudio");
    for (char c : name)
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-' && c != '_') return out.fail(kUsage, "connect: --name may use letters, digits, - and _");
    const fs::path exe = a.opt("mcp") ? fromUtf8(*a.opt("mcp")) : defaultMcpExe();
    std::error_code ec;
    if (!remove && !fs::exists(exe, ec))
        return out.fail(kIo, "connect: the MCP server was not found at " + utf8(exe) + " (use --mcp PATH)");
    const bool global = a.flags.count("global") > 0 || client == "claude-desktop";
    const fs::path project = fs::absolute(fromUtf8(a.opt("project").value_or(".")), ec);
    const std::string exeStr = utf8(exe);

    fs::path file;
    std::string text;
    std::string note;
    if (client == "codex") {
        file = global ? fromUtf8(envOr("CODEX_HOME", utf8(home() / ".codex"))) / "config.toml" : project / ".codex" / "config.toml";
        std::string table = "[mcp_servers." + name + "]\ncommand = " + tomlString(exeStr) + "\nargs = [" +
                            (global ? std::string() : "'--root', " + tomlString(utf8(project))) +
                            "]\n# Local AI generation can take minutes (the default tool timeout is 60 s).\ntool_timeout_sec = 900\nstartup_timeout_sec = 20\n";
        auto existing = readText(file);
        if (!existing) return out.fail(existing.error());
        text = mergeToml(existing.value(), name, remove ? nullptr : &table);
        if (!global) note = "Codex reads a project's .codex/config.toml only when the project is trusted.";
    } else {
        std::vector<std::string> section{"mcpServers"};
        ordered_json server;
        const std::string projectRoot = utf8(project);
        if (client == "claude") {
            if (global) {
                // User-scope servers live in ~/.claude.json, which Claude Code itself rewrites while running; use its CLI.
                const std::string cmd = remove ? "claude mcp remove --scope user " + name
                                               : "claude mcp add --transport stdio --scope user " + name + " -- \"" + exeStr + "\"";
                return out.success({{"client", client}, {"command", cmd}, {"written", false}},
                                   "Claude Code keeps user-wide servers in a file it rewrites itself; run this instead:\n  " + cmd + "\n");
            }
            file = project / ".mcp.json";
            server = {{"type", "stdio"}, {"command", exeStr}, {"args", {"--root", "${CLAUDE_PROJECT_DIR:-.}"}}};
            note = "Claude Code asks you to approve a project's servers the first time.";
        } else if (client == "claude-desktop") {
            file = appData() / "Claude" / "claude_desktop_config.json";
            server = {{"command", exeStr}, {"args", ordered_json::array()}};
            note = "Restart Claude Desktop to load it. The tools work in the folder the server starts in; add \"--root\", \"<folder>\" to args to choose one.";
        } else if (client == "cursor") {
            file = global ? home() / ".cursor" / "mcp.json" : project / ".cursor" / "mcp.json";
            server = {{"type", "stdio"}, {"command", exeStr}, {"args", global ? ordered_json::array() : ordered_json{"--root", "${workspaceFolder}"}}};
        } else if (client == "vscode") {
            section = {"servers"};
            // %APPDATA%\Code\User, ~/Library/Application Support/Code/User or ~/.config/Code/User.
            const fs::path user = appData() / "Code" / "User" / "mcp.json";
            file = global ? user : project / ".vscode" / "mcp.json";
            server = {{"type", "stdio"}, {"command", exeStr}, {"args", global ? ordered_json::array() : ordered_json{"--root", "${workspaceFolder}"}}};
        } else if (client == "opencode") {
            section = {"mcp"};
            file = global ? home() / ".config" / "opencode" / "opencode.json" : project / "opencode.json";
            if (!global && fs::exists(project / "opencode.jsonc", ec))
                return out.fail(kUsage, "connect: this project uses opencode.jsonc; its comments would be lost. Add this under \"mcp\":\n" +
                                            ordered_json{{name, {{"type", "local"}, {"command", {exeStr, "--root", projectRoot}}, {"enabled", true}, {"timeout", 900000}}}}.dump(2));
            server = {{"type", "local"},
                      {"command", global ? ordered_json{exeStr} : ordered_json{exeStr, "--root", projectRoot}},
                      {"enabled", true},
                      {"timeout", 900000}};
        }
        auto existing = readText(file);
        if (!existing) return out.fail(existing.error());
        if (client == "opencode" && existing.value().empty() && !remove) existing = std::string("{\"$schema\": \"https://opencode.ai/config.json\"}");
        auto merged = mergeJson(existing.value(), section, name, remove ? nullptr : &server);
        if (!merged) return out.fail(kCorrupt, "connect: " + utf8(file) + ": " + merged.error().message);
        text = merged.value();
    }
    if (dry) return out.success({{"client", client}, {"file", utf8(file)}, {"content", text}, {"written", false}}, utf8(file) + ":\n" + text);
    if (auto st = writeWithBackup(file, text); !st) return out.fail(st.error());
    return out.success({{"client", client}, {"file", utf8(file)}, {"written", true}, {"removed", remove}, {"note", note}},
                       std::string(remove ? "Removed " : "Added ") + name + (remove ? " from " : " to ") + utf8(file) +
                           (fs::exists(fs::path(file).concat(".bak"), ec) ? " (previous version: " + utf8(file) + ".bak)" : "") + "\n" +
                           (note.empty() ? "" : note + "\n"));
}

} // namespace acestudio::cli
