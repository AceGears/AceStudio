#pragma once

// The fixtures the MCP conformance suite (@modelcontextprotocol/conformance)
// calls: tools, prompts and resources with fixed names and answers. Only
// registered with `acestudio-mcp --conformance-fixtures`, so the suite can
// exercise this server's protocol plumbing (content types, errors, progress,
// logging, elicitation / sampling / roots in both eras, prompts, resources,
// templates, completions, subscriptions). Not part of the AceStudio set.

#include "McpProtocol.h"

namespace acestudio::mcp {

struct Fixtures {
    std::vector<Tool> tools;
    std::vector<Prompt> prompts;
    std::vector<Resource> resources;
    std::vector<ResourceTemplate> templates;
};

Fixtures conformanceFixtures();

} // namespace acestudio::mcp
