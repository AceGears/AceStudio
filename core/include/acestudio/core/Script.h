#pragma once

// Lua scripting (Phase 7): Lua 5.4 through sol2, sandboxed.
//
// The sandbox: only the base, string, table, math, utf8 and coroutine
// libraries, without dofile/loadfile and with `load` limited to text chunks;
// a safe subset of os (time, clock, date); no io, package or debug. Scripts
// touch files only through the `ace` API, and only inside the folders the
// host allows. Memory and running time are limited, and the host can cancel.
//
// The API is documented in docs/lua-api.md. Frames, layers and pixels are
// 0-based, like the CLI, the JSON formats and the MCP server.

#include "acestudio/core/Result.h"

#include <atomic>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace acestudio {

class Editor;

struct ScriptLimits {
    std::size_t memoryBytes = 512u * 1024u * 1024u;
    double seconds = 30.0;                 // wall-clock limit; 0 = none
};

struct ScriptOptions {
    ScriptLimits limits;
    // Folders the script may read and write through the API. Relative paths
    // in the script are resolved against the first one. Empty = no files.
    std::vector<std::filesystem::path> allowedRoots;
    // The document `ace.document()` returns (the app's current document),
    // or null. Changes go through its Editor (so they can be undone).
    Editor* document = nullptr;
    // print() and ace.log() output.
    std::function<void(const std::string&)> output;
    // Set to true from another thread to stop the script at the next check.
    const std::atomic<bool>* cancel = nullptr;
    // Not empty: writing files fails with this message (reading still works).
    std::string readOnlyReason;
    // Arguments, visible to the script as `arg` (a list of strings).
    std::vector<std::string> args;
};

struct ScriptResult {
    std::string output;                    // everything printed
    std::vector<std::filesystem::path> written;   // files the script wrote
    bool documentChanged = false;          // the host document was edited
};

// Runs one script. Fails with the Lua error (file:line: message) when the
// script raises one, exceeds a limit, is cancelled, or touches a file
// outside the allowed folders.
Result<ScriptResult> runScript(const std::string& source, const std::string& chunkName, const ScriptOptions& opts);
Result<ScriptResult> runScriptFile(const std::filesystem::path& file, ScriptOptions opts);

// The Lua version string ("Lua 5.4.9").
std::string luaVersion();

} // namespace acestudio
