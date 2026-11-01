#pragma once

// The fixture tools the MCP conformance suite (@modelcontextprotocol/
// conformance) calls. Only registered with `acestudio-mcp --conformance-fixtures`,
// so the suite can exercise this server's protocol plumbing (content types,
// tool errors, progress and log notifications, JSON Schema 2020-12
// pass-through). They are not part of the AceStudio tool set.

#include "McpProtocol.h"

namespace acestudio::mcp {

std::vector<Tool> conformanceFixtures();

} // namespace acestudio::mcp
