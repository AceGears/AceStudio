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

#include <cstdint>
#include <string>

namespace acestudio {

// The socket name for the current user. It is predictable on purpose (the
// app and acestudio-mcp find each other without shared state), so it is not
// a secret: the server restricts the socket to the current user
// (QLocalServer::UserAccessOption) and both ends check who is at the other
// end with localPeerIsCurrentUser() before trusting it (SEC-007).
std::string appBridgeName();

// True when the process at the other end of a connected local socket runs as
// the current user. `nativeHandle` is QLocalSocket::socketDescriptor(): the
// named-pipe HANDLE on Windows, a Unix-domain socket descriptor elsewhere.
// `peerIsServer`: true on the client side (the peer is the pipe server).
// Any failure to find out counts as "no".
bool localPeerIsCurrentUser(std::intptr_t nativeHandle, bool peerIsServer);

} // namespace acestudio
