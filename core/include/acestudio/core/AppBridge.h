#pragma once

// The local bridge between acestudio-mcp and a running AceStudio (Phase 7):
// a per-user local socket (a named pipe on Windows) carrying JSON lines.
//
//   request:  {"id": 1, "method": "document" | "run_lua" | "open", "params": {...}}
//   response: {"id": 1, "result": {...}}  or  {"id": 1, "error": "message"}
//
//   document  -> {open, name, path, width, height, frames, frame, layers, tags, png}
//                (png: the current frame, flattened, base64)
//   run_lua   {code, args} -> {output, documentChanged, written}
//                (runs on a copy of the current document; one undo step)
//   open      {path} -> {opened}

#include <string>

namespace acestudio {

// The socket name for the current user.
std::string appBridgeName();

} // namespace acestudio
