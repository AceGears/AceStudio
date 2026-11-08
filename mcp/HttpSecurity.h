#pragma once

// Security checks for acestudio-mcp's HTTP transport (SEC-006): bearer token
// creation and loading, constant-time comparison, and the localhost Host /
// Origin checks that stop DNS rebinding. No Qt, so the tests can use it.

#include "acestudio/core/Result.h"

#include <filesystem>
#include <optional>
#include <string>

namespace acestudio::mcp {

// Compares secrets without leaking how many leading bytes matched.
bool constantTimeEquals(const std::string& a, const std::string& b);

// A Host header value ("localhost:39200", "127.0.0.1", "[::1]:80") names this
// computer's loopback interface by a name a browser cannot be tricked into
// using for another site.
bool isLocalHost(std::string host);

// An Origin header value ("http://localhost:3000") is a localhost page.
bool isLocalOrigin(const std::string& origin);

// A new random bearer token: 256 bits from the OS random source, as 64
// lower-case hex characters.
std::string generateHttpToken();

// Reads a token from a file (first line, surrounding whitespace removed).
// An empty or unreadable file is an error.
Result<std::string> readTokenFile(const std::filesystem::path& path);

// Writes `token` to `path`, readable by the current user only (POSIX 0600;
// on Windows the per-user data folder's ACL already limits it to the user).
Status writeTokenFile(const std::filesystem::path& path, const std::string& token);

// Where acestudio-mcp records the token it generated: <user data>/mcp-http-token.
std::optional<std::filesystem::path> defaultTokenFile();

} // namespace acestudio::mcp
