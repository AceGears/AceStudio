#include "ConformanceFixtures.h"

#include <chrono>
#include <thread>

namespace acestudio::mcp {

namespace {

// 1x1 red PNG and a 44-byte silent WAV header + 2 samples.
const char* kRedPixel = "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mP8z8DwHwAFBQIAX8jx0gAAAABJRU5ErkJggg==";
const char* kSilentWav = "UklGRigAAABXQVZFZm10IBAAAAABAAEAQB8AAIA+AAACABAAZGF0YQQAAAAAAAAA";

json noArgs() { return {{"type", "object"}, {"properties", json::object()}}; }

json embedded() {
    return {{"type", "resource"},
            {"resource", {{"uri", "test://embedded-resource"}, {"mimeType", "text/plain"}, {"text", "This is an embedded resource content."}}}};
}

} // namespace

std::vector<Tool> conformanceFixtures() {
    std::vector<Tool> t;
    t.push_back({"test_simple_text", "", "Returns simple text (conformance fixture)", noArgs(), {}, [](const json&, const ToolCall&) {
                     return ToolResult::text("This is a simple text response for testing.");
                 }});
    t.push_back({"test_image_content", "", "Returns an image (conformance fixture)", noArgs(), {}, [](const json&, const ToolCall&) {
                     ToolResult r;
                     r.addImage(kRedPixel);
                     return r;
                 }});
    t.push_back({"test_audio_content", "", "Returns audio (conformance fixture)", noArgs(), {}, [](const json&, const ToolCall&) {
                     ToolResult r;
                     r.content.push_back({{"type", "audio"}, {"data", kSilentWav}, {"mimeType", "audio/wav"}});
                     return r;
                 }});
    t.push_back({"test_embedded_resource", "", "Returns an embedded resource (conformance fixture)", noArgs(), {},
                 [](const json&, const ToolCall&) {
                     ToolResult r;
                     r.content.push_back(embedded());
                     return r;
                 }});
    t.push_back({"test_multiple_content_types", "", "Returns text, image and resource (conformance fixture)", noArgs(), {},
                 [](const json&, const ToolCall&) {
                     ToolResult r;
                     r.addText("Multiple content types test:");
                     r.addImage(kRedPixel);
                     r.content.push_back(embedded());
                     return r;
                 }});
    t.push_back({"test_error_handling", "", "Always fails (conformance fixture)", noArgs(), {}, [](const json&, const ToolCall&) {
                     return ToolResult::text("This tool intentionally returns an error for testing", true);
                 }});
    t.push_back({"test_tool_with_progress", "", "Reports progress (conformance fixture)", noArgs(), {}, [](const json&, const ToolCall& c) {
                     for (int i = 0; i <= 100; i += 50) {
                         if (c.progress) c.progress(i, 100.0, "step " + std::to_string(i / 50 + 1));
                         std::this_thread::sleep_for(std::chrono::milliseconds(50));
                     }
                     return ToolResult::text("Progress complete");
                 }});
    t.push_back({"test_tool_with_logging", "", "Sends log messages (conformance fixture)", noArgs(), {}, [](const json&, const ToolCall& c) {
                     for (const char* m : {"Tool execution started", "Tool processing data", "Tool execution completed"}) {
                         if (c.log) c.log("info", m);
                         std::this_thread::sleep_for(std::chrono::milliseconds(50));
                     }
                     return ToolResult::text("Logging complete");
                 }});
    t.push_back({"json_schema_2020_12_tool", "", "Tool with JSON Schema 2020-12 features",
                 {{"$schema", "https://json-schema.org/draft/2020-12/schema"},
                  {"type", "object"},
                  {"$defs", {{"address", {{"type", "object"}, {"properties", {{"street", {{"type", "string"}}}, {"city", {{"type", "string"}}}}}}}}},
                  {"properties", {{"name", {{"type", "string"}}}, {"address", {{"$ref", "#/$defs/address"}}}}},
                  {"additionalProperties", false}},
                 {},
                 [](const json& a, const ToolCall&) { return ToolResult::text("Received: " + a.dump()); }});
    return t;
}

} // namespace acestudio::mcp
