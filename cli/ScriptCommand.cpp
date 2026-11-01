// acestudio-cli run: sandboxed Lua scripts (Phase 7).

#include "CliCommon.h"

#include "acestudio/core/Editor.h"
#include "acestudio/core/ProjectIO.h"
#include "acestudio/core/Script.h"

#include <filesystem>
#include <iostream>

namespace acestudio::cli {

namespace fs = std::filesystem;

namespace {

std::vector<std::string> splitPaths(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == ';') {
            if (!cur.empty()) out.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

} // namespace

int cmdRun(const Args& a, Output& out) {
    if (a.positional.size() < 2) return out.fail(kUsage, "run: give the script, e.g. run make.lua");
    const fs::path script = a.positional[1];
    std::error_code ec;
    if (!fs::is_regular_file(script, ec)) return out.fail(kIo, "run: no script at " + script.string());

    ScriptOptions o;
    o.args.assign(a.positional.begin() + 2, a.positional.end());
    // Relative paths in the script resolve against --base (default: the
    // script's folder), which is allowed too.
    if (auto base = a.opt("base")) {
        if (!fs::is_directory(*base, ec)) return out.fail(kIo, "run: --base " + *base + " is not a folder");
        o.allowedRoots.push_back(fs::absolute(*base, ec));
    }
    o.allowedRoots.push_back(fs::absolute(script, ec).parent_path());
    if (auto allow = a.opt("allow"))
        for (const auto& p : splitPaths(*allow)) {
            if (!fs::is_directory(p, ec)) return out.fail(kIo, "run: --allow " + p + " is not a folder");
            o.allowedRoots.push_back(fs::absolute(p, ec));
        }
    if (auto t = optInt(a, "timeout")) {
        if (*t < 0 || *t > 86400) return out.fail(kUsage, "run: --timeout must be 0..86400 seconds (0 = none)");
        o.limits.seconds = *t;
    }
    if (auto m = optInt(a, "memory")) {
        if (*m < 8 || *m > 16384) return out.fail(kUsage, "run: --memory must be 8..16384 MB");
        o.limits.memoryBytes = static_cast<std::size_t>(*m) << 20;
    }
    // In human mode the script's output streams as it runs; in JSON mode it
    // is returned in the result.
    // A failed script's output is kept too (JSON: the "output" field).
    std::string printed;
    if (!out.isJson()) o.output = [](const std::string& s) { std::cout << s << std::flush; };
    else o.output = [&printed](const std::string& s) { printed += s; };

    std::unique_ptr<Editor> editor;
    const auto docPath = a.opt("doc");
    const auto savePath = a.opt("save");
    if (savePath && !docPath) return out.fail(kUsage, "run: --save needs --doc");
    if (docPath) {
        auto doc = loadProject(*docPath);
        if (!doc) return out.fail(doc.error());
        editor = std::make_unique<Editor>(std::move(doc).value());
        o.document = editor.get();
    }

    auto r = runScriptFile(script, o);
    if (!r) return out.fail(kScript, r.error().message, {{"output", printed}});
    const ScriptResult& res = r.value();

    json saved = nullptr;
    if (savePath && res.documentChanged) {
        if (fs::exists(*savePath, ec) && !a.force)
            return out.fail(kIo, *savePath + " already exists (use --force to replace it; a .bak copy is kept)");
        if (auto s = saveProject(editor->document(), *savePath); !s) return out.fail(s.error());
        saved = *savePath;
    }

    json files = json::array();
    for (const auto& f : res.written) files.push_back(f.string());
    std::string human;
    for (const auto& f : res.written) human += "wrote " + f.string() + "\n";
    if (!saved.is_null()) human += "saved " + *savePath + "\n";
    else if (docPath && res.documentChanged) human += "the script changed the document; add --save out.acestudio to keep it\n";
    json payload{{"written", files}, {"documentChanged", res.documentChanged}, {"saved", saved}};
    if (out.isJson()) payload["output"] = res.output;
    return out.success(payload, human);
}

} // namespace acestudio::cli
