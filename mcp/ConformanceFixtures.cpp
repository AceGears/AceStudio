#include "ConformanceFixtures.h"

#include <chrono>
#include <thread>

namespace acestudio::mcp {

namespace {

// 1x1 red PNG and a 48-byte WAV (44-byte header + 2 silent samples).
const char* kRedPixel = "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mP8z8DwHwAFBQIAX8jx0gAAAABJRU5ErkJggg==";
const char* kSilentWav = "UklGRigAAABXQVZFZm10IBAAAAABAAEAQB8AAIA+AAACABAAZGF0YQQAAAAAAAAA";

json noArgs() { return {{"type", "object"}, {"properties", json::object()}}; }
json stringArg(const char* name) {
    return {{"type", "object"}, {"properties", {{name, {{"type", "string"}}}}}, {"required", {name}}};
}

json embedded() {
    return {{"type", "resource"},
            {"resource", {{"uri", "test://embedded-resource"}, {"mimeType", "text/plain"}, {"text", "This is an embedded resource content."}}}};
}

json elicit(const std::string& message, const json& properties, const json& required) {
    json schema{{"type", "object"}, {"properties", properties}};
    if (!required.empty()) schema["required"] = required;
    return {{"message", message}, {"requestedSchema", schema}};
}

json sample(const std::string& text, int maxTokens) {
    return {{"messages", {{{"role", "user"}, {"content", {{"type", "text"}, {"text", text}}}}}}, {"maxTokens", maxTokens}};
}

std::string sampledText(const json& r) {
    if (r.contains("content") && r["content"].is_object()) return r["content"].value("text", std::string());
    return r.dump();
}

std::string elicitSummary(const json& r) {
    return "action=" + r.value("action", std::string("?")) + ", content=" + r.value("content", json::object()).dump();
}

// A tool that needs the client: an error when the client cannot answer
// (legacy: the request failed; modern: unsupported capability). In the
// modern era a missing answer just returns; the server sends input_required.
ToolResult needsClient(const ToolCall& c, const std::string& what) {
    if (!c.input->modern() || !c.input->error().empty())
        return ToolResult::text(what + ": " + (c.input->error().empty() ? std::string("no answer") : c.input->error()), true);
    return ToolResult{};
}

} // namespace

Fixtures conformanceFixtures() {
    Fixtures f;
    auto& t = f.tools;
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
                  {"$defs",
                   {{"address",
                     {{"$anchor", "addressDef"},
                      {"type", "object"},
                      {"properties", {{"street", {{"type", "string"}}}, {"city", {{"type", "string"}}}}}}}}},
                  {"properties",
                   {{"name", {{"type", "string"}}},
                    {"address", {{"$ref", "#/$defs/address"}}},
                    {"contactMethod", {{"type", "string"}, {"enum", {"phone", "email"}}}},
                    {"phone", {{"type", "string"}}},
                    {"email", {{"type", "string"}}}}},
                  {"allOf", {{{"anyOf", {{{"required", {"phone"}}}, {{"required", {"email"}}}}}}}},
                  {"if", {{"properties", {{"contactMethod", {{"const", "phone"}}}}}, {"required", {"contactMethod"}}}},
                  {"then", {{"required", {"phone"}}}},
                  {"else", {{"required", {"email"}}}},
                  {"additionalProperties", false}},
                 {},
                 [](const json& a, const ToolCall&) { return ToolResult::text("Received: " + a.dump()); }});
    t.push_back({"test_custom_header", "", "A tool with an x-mcp-header argument (conformance fixture)",
                 {{"type", "object"},
                  {"properties",
                   {{"region", {{"type", "string"}, {"description", "Region, mirrored in Mcp-Param-Region"}, {"x-mcp-header", "Region"}}},
                    {"query", {{"type", "string"}, {"description", "Query"}}}}},
                  {"required", {"region"}}},
                 {},
                 [](const json& a, const ToolCall&) { return ToolResult::text("Region: " + a.value("region", std::string())); }});
    t.push_back({"test_missing_capability", "", "Needs the client's sampling capability (conformance fixture)", noArgs(), {},
                 [](const json&, const ToolCall& c) {
                     if (!c.input->supports("sampling/createMessage")) return ToolResult::missingCapability("sampling");
                     return ToolResult::text("The client declared sampling");
                 }});
    t.push_back({"test_streaming_elicitation", "", "Asks the user while streaming progress (conformance fixture)", noArgs(), {},
                 [](const json&, const ToolCall& c) {
                     if (c.progress) c.progress(1, 2.0, "about to ask");
                     auto r = c.input->ask("answer", "elicitation/create", elicit("Continue?", {{"ok", {{"type", "boolean"}}}}, {"ok"}));
                     if (!r) return needsClient(c, "Elicitation failed");
                     return ToolResult::text("Answered: " + elicitSummary(*r));
                 }});
    t.push_back({"test_logging_tool", "", "Logs while it works (conformance fixture)", noArgs(), {}, [](const json&, const ToolCall& c) {
                     for (const char* m : {"first", "second", "third"})
                         if (c.log) c.log("info", m);
                     return ToolResult::text("Logged three messages (only sent when the request asked for logs)");
                 }});

    // ---- client input, both eras
    t.push_back({"test_sampling", "", "Asks the client's LLM (conformance fixture)", stringArg("prompt"), {}, [](const json& a, const ToolCall& c) {
                     auto r = c.input->ask("sample", "sampling/createMessage", sample(a["prompt"].get<std::string>(), 100));
                     if (!r) return needsClient(c, "Sampling failed");
                     return ToolResult::text("LLM response: " + sampledText(*r));
                 }});
    t.push_back({"test_elicitation", "", "Asks the user (conformance fixture)", stringArg("message"), {}, [](const json& a, const ToolCall& c) {
                     auto r = c.input->ask("user", "elicitation/create",
                                           elicit(a["message"].get<std::string>(),
                                                  {{"username", {{"type", "string"}, {"description", "User's response"}}},
                                                   {"email", {{"type", "string"}, {"description", "User's email address"}}}},
                                                  {"username", "email"}));
                     if (!r) return needsClient(c, "Elicitation failed");
                     return ToolResult::text("User response: " + elicitSummary(*r));
                 }});
    t.push_back({"test_elicitation_sep1034_defaults", "", "Elicitation with defaults (conformance fixture)", noArgs(), {},
                 [](const json&, const ToolCall& c) {
                     auto r = c.input->ask("defaults", "elicitation/create",
                                           elicit("Please review your details",
                                                  {{"name", {{"type", "string"}, {"default", "John Doe"}}},
                                                   {"age", {{"type", "integer"}, {"default", 30}}},
                                                   {"score", {{"type", "number"}, {"default", 95.5}}},
                                                   {"status", {{"type", "string"}, {"enum", {"active", "inactive", "pending"}}, {"default", "active"}}},
                                                   {"verified", {{"type", "boolean"}, {"default", true}}}},
                                                  json::array()));
                     if (!r) return needsClient(c, "Elicitation failed");
                     return ToolResult::text("Elicitation completed: " + elicitSummary(*r));
                 }});
    t.push_back({"test_elicitation_sep1330_enums", "", "Elicitation with enum variants (conformance fixture)", noArgs(), {},
                 [](const json&, const ToolCall& c) {
                     json props{
                         {"untitledSingle", {{"type", "string"}, {"enum", {"option1", "option2", "option3"}}}},
                         {"titledSingle",
                          {{"type", "string"},
                           {"oneOf",
                            {{{"const", "value1"}, {"title", "First Option"}}, {{"const", "value2"}, {"title", "Second Option"}},
                             {{"const", "value3"}, {"title", "Third Option"}}}}}},
                         {"legacyEnum",
                          {{"type", "string"}, {"enum", {"opt1", "opt2", "opt3"}}, {"enumNames", {"Option One", "Option Two", "Option Three"}}}},
                         {"untitledMulti", {{"type", "array"}, {"items", {{"type", "string"}, {"enum", {"option1", "option2", "option3"}}}}}},
                         {"titledMulti",
                          {{"type", "array"},
                           {"items",
                            {{"anyOf",
                              {{{"const", "value1"}, {"title", "First Choice"}}, {{"const", "value2"}, {"title", "Second Choice"}},
                               {{"const", "value3"}, {"title", "Third Choice"}}}}}}}}};
                     auto r = c.input->ask("enums", "elicitation/create", elicit("Choose your options", props, json::array()));
                     if (!r) return needsClient(c, "Elicitation failed");
                     return ToolResult::text("Elicitation completed: " + elicitSummary(*r));
                 }});

    // ---- multi round-trip requests (2026-07-28)
    const json askName = elicit("What is your name?", {{"name", {{"type", "string"}}}}, {"name"});
    t.push_back({"test_input_required_result_elicitation", "", "MRTR elicitation (conformance fixture)", noArgs(), {},
                 [askName](const json&, const ToolCall& c) {
                     auto r = c.input->ask("user_name", "elicitation/create", askName);
                     if (!r) return needsClient(c, "Elicitation failed");
                     return ToolResult::text("Hello, " + r->value("content", json::object()).value("name", std::string("?")) + "!");
                 }});
    t.push_back({"test_input_required_result_sampling", "", "MRTR sampling (conformance fixture)", noArgs(), {}, [](const json&, const ToolCall& c) {
                     auto r = c.input->ask("capital_question", "sampling/createMessage", sample("What is the capital of France?", 100));
                     if (!r) return needsClient(c, "Sampling failed");
                     return ToolResult::text(sampledText(*r));
                 }});
    t.push_back({"test_input_required_result_list_roots", "", "MRTR roots (conformance fixture)", noArgs(), {}, [](const json&, const ToolCall& c) {
                     auto r = c.input->ask("client_roots", "roots/list", json::object());
                     if (!r) return needsClient(c, "roots/list failed");
                     std::string list;
                     for (const auto& root : r->value("roots", json::array())) list += (list.empty() ? "" : ", ") + root.value("uri", std::string());
                     return ToolResult::text("Roots: " + list);
                 }});
    t.push_back({"test_input_required_result_request_state", "", "MRTR request state (conformance fixture)", noArgs(), {},
                 [](const json&, const ToolCall& c) {
                     auto r = c.input->ask("confirm", "elicitation/create", elicit("Please confirm", {{"ok", {{"type", "boolean"}}}}, {"ok"}));
                     if (!r) {
                         c.input->setState("round-1");
                         return needsClient(c, "Elicitation failed");
                     }
                     return ToolResult::text(c.input->state() == json("round-1") ? "Confirmed: state-ok" : "Confirmed (state missing)");
                 }});
    t.push_back({"test_input_required_result_tampered_state", "", "MRTR signed state (conformance fixture)", noArgs(), {},
                 [](const json&, const ToolCall& c) {
                     auto r = c.input->ask("confirm", "elicitation/create", elicit("Please confirm", {{"ok", {{"type", "boolean"}}}}, {"ok"}));
                     if (!r) {
                         c.input->setState("signed");
                         return needsClient(c, "Elicitation failed");
                     }
                     return ToolResult::text("Confirmed: state-ok");
                 }});
    t.push_back({"test_input_required_result_multiple_inputs", "", "MRTR several inputs (conformance fixture)", noArgs(), {},
                 [askName](const json&, const ToolCall& c) {
                     auto name = c.input->ask("user_name", "elicitation/create", askName);
                     auto greeting = c.input->ask("greeting", "sampling/createMessage", sample("Generate a greeting", 50));
                     auto roots = c.input->ask("client_roots", "roots/list", json::object());
                     if (!name || !greeting || !roots) {
                         c.input->setState("multi");
                         return needsClient(c, "Input failed");
                     }
                     return ToolResult::text("Got name, greeting and " + std::to_string(roots->value("roots", json::array()).size()) + " root(s)");
                 }});
    t.push_back({"test_input_required_result_multi_round", "", "MRTR several rounds (conformance fixture)", noArgs(), {},
                 [](const json&, const ToolCall& c) {
                     const int round = c.input->state().is_number_integer() ? c.input->state().get<int>() : 0;
                     if (round < 2) {
                         auto step1 = c.input->ask("step1", "elicitation/create",
                                                   elicit("Step 1: What is your name?", {{"name", {{"type", "string"}}}}, {"name"}));
                         if (!step1) {
                             c.input->setState(1);
                             return needsClient(c, "Elicitation failed");
                         }
                         if (c.input->modern()) {
                             auto step2 = c.input->ask("step2", "elicitation/create",
                                                       elicit("Step 2: What is your favorite color?", {{"color", {{"type", "string"}}}}, {"color"}));
                             if (!step2) {
                                 c.input->setState(2);
                                 return ToolResult{};
                             }
                         }
                     }
                     auto step2 = c.input->ask("step2", "elicitation/create",
                                               elicit("Step 2: What is your favorite color?", {{"color", {{"type", "string"}}}}, {"color"}));
                     if (!step2) {
                         c.input->setState(2);
                         return needsClient(c, "Elicitation failed");
                     }
                     return ToolResult::text("Done in three rounds");
                 }});
    t.push_back({"test_input_required_result_capabilities", "", "MRTR only for declared capabilities (conformance fixture)", noArgs(), {},
                 [askName](const json&, const ToolCall& c) {
                     std::optional<json> a, b;
                     const bool e = c.input->supports("elicitation/create"), s = c.input->supports("sampling/createMessage");
                     if (e) a = c.input->ask("user_name", "elicitation/create", askName);
                     if (s) b = c.input->ask("greeting", "sampling/createMessage", sample("Generate a greeting", 50));
                     if ((e && !a) || (s && !b)) return needsClient(c, "Input failed");
                     return ToolResult::text("Done");
                 }});

    // ---- prompts
    auto& p = f.prompts;
    p.push_back({"test_simple_prompt", "", "A simple prompt (conformance fixture)", {}, [](const json&, InputContext&) -> Result<json> {
                     return json{{"messages", {{{"role", "user"}, {"content", textBlock("This is a simple prompt for testing.")}}}}};
                 }, {}});
    p.push_back({"test_prompt_with_arguments", "", "A prompt with arguments (conformance fixture)",
                 {{"arg1", "First test argument", true}, {"arg2", "Second test argument", true}},
                 [](const json& a, InputContext&) -> Result<json> {
                     return json{{"messages",
                                  {{{"role", "user"},
                                    {"content", textBlock("Prompt with arguments: arg1='" + a["arg1"].get<std::string>() + "', arg2='" +
                                                          a["arg2"].get<std::string>() + "'")}}}}};
                 },
                 [](const std::string& name, const std::string& value, const json&) {
                     std::vector<std::string> out;
                     if (name != "arg1") return out;
                     for (const char* v : {"paris", "park", "party"})
                         if (std::string(v).rfind(value, 0) == 0) out.push_back(v);
                     return out;
                 }});
    p.push_back({"test_prompt_with_embedded_resource", "", "A prompt with an embedded resource (conformance fixture)",
                 {{"resourceUri", "URI of the resource to embed", true}},
                 [](const json& a, InputContext&) -> Result<json> {
                     return json{{"messages",
                                  {{{"role", "user"},
                                    {"content",
                                     {{"type", "resource"},
                                      {"resource",
                                       {{"uri", a["resourceUri"]}, {"mimeType", "text/plain"}, {"text", "Embedded resource content for testing."}}}}}},
                                   {{"role", "user"}, {"content", textBlock("Please process the embedded resource above.")}}}}};
                 }, {}});
    p.push_back({"test_prompt_with_image", "", "A prompt with an image (conformance fixture)", {}, [](const json&, InputContext&) -> Result<json> {
                     return json{{"messages",
                                  {{{"role", "user"}, {"content", imageBlock(kRedPixel)}},
                                   {{"role", "user"}, {"content", textBlock("Please analyze the image above.")}}}}};
                 }, {}});
    p.push_back({"test_input_required_result_prompt", "", "A prompt that asks the user first (conformance fixture)", {},
                 [](const json&, InputContext& in) -> Result<json> {
                     auto r = in.ask("user_context", "elicitation/create",
                                     elicit("What context should the prompt use?", {{"context", {{"type", "string"}}}}, {"context"}));
                     if (!r) return Error{ErrorCode::InvalidArgument, in.error().empty() ? "input needed" : in.error()};
                     return json{{"messages",
                                  {{{"role", "user"},
                                    {"content", textBlock("Context: " + r->value("content", json::object()).value("context", std::string()))}}}}};
                 }, {}});

    // ---- resources
    auto& r = f.resources;
    r.push_back({"test://static-text", "Static text", "", "A static text resource (conformance fixture)", "text/plain",
                 [](const std::string& uri, InputContext&) -> Result<json> {
                     return json::array({{{"uri", uri}, {"mimeType", "text/plain"}, {"text", "This is the content of the static text resource."}}});
                 }, 3600000, true});
    r.push_back({"test://static-binary", "Static binary", "", "A static PNG resource (conformance fixture)", "image/png",
                 [](const std::string& uri, InputContext&) -> Result<json> {
                     return json::array({{{"uri", uri}, {"mimeType", "image/png"}, {"blob", kRedPixel}}});
                 }, 3600000, true});
    r.push_back({"test://watched-resource", "Watched resource", "", "A resource to subscribe to (conformance fixture)", "text/plain",
                 [](const std::string& uri, InputContext&) -> Result<json> {
                     return json::array({{{"uri", uri}, {"mimeType", "text/plain"}, {"text", "Watched resource content."}}});
                 }, 0, false});
    f.templates.push_back({"test://template/{id}/data", "Template data", "", "A templated resource (conformance fixture)", "application/json",
                           [](const std::string& uri, const std::map<std::string, std::string>& vars, InputContext&) -> Result<json> {
                               const std::string id = vars.at("id");
                               json body{{"id", id}, {"templateTest", true}, {"data", "Data for ID: " + id}};
                               return json::array({{{"uri", uri}, {"mimeType", "application/json"}, {"text", body.dump()}}});
                           },
                           [](const std::string&, const std::string& value, const json&) {
                               std::vector<std::string> out;
                               for (const char* v : {"123", "456"})
                                   if (std::string(v).rfind(value, 0) == 0) out.push_back(v);
                               return out;
                           },
                           3600000, true});

    // ---- tasks extension (io.modelcontextprotocol/tasks)
    auto sleepFor = [](const ToolCall& c, double seconds) {
        const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(static_cast<long long>(seconds * 1000));
        while (std::chrono::steady_clock::now() < end) {
            if (c.cancel && *c.cancel) return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        return true;
    };
    t.push_back({"greet", "", "Greets someone (sync only; conformance fixture)", stringArg("name"), {}, [](const json& a, const ToolCall&) {
                     return ToolResult::text("Hello, " + a["name"].get<std::string>() + "!");
                 }});
    t.push_back({"slow_compute", "", "Sleeps, then answers (task; conformance fixture)",
                 {{"type", "object"}, {"properties", {{"seconds", {{"type", "number"}}}}}}, {},
                 [sleepFor](const json& a, const ToolCall& c) {
                     const double seconds = a.value("seconds", 1.0);
                     if (!sleepFor(c, seconds)) return ToolResult::text("Cancelled", true);
                     return ToolResult::text("Computed after " + std::to_string(seconds) + " s");
                 },
                 "optional"});
    t.push_back({"failing_job", "", "Fails with a tool error (task required; conformance fixture)", noArgs(), {},
                 [sleepFor](const json&, const ToolCall& c) {
                     sleepFor(c, 1.0);
                     return ToolResult::text("The job failed (intentional tool error)", true);
                 },
                 "required"});
    t.push_back({"protocol_error_job", "", "Fails with a protocol error (task; conformance fixture)", noArgs(), {},
                 [](const json&, const ToolCall&) {
                     ToolResult out;
                     out.protocolError = json{{"code", -32603}, {"message", "Internal error (intentional)"}, {"data", {{"fixture", "protocol_error_job"}}}};
                     return out;
                 },
                 "optional"});
    t.push_back({"confirm_delete", "", "Asks for confirmation (task; conformance fixture)", noArgs(), {}, [](const json&, const ToolCall& c) {
                     auto answer = c.input->ask("confirm", "elicitation/create",
                                           elicit("Delete the item?", {{"confirm", {{"type", "boolean"}}}}, {"confirm"}));
                     if (!answer) return needsClient(c, "Elicitation failed");
                     const bool yes = answer->value("action", std::string()) == "accept" &&
                                      answer->value("content", json::object()).value("confirm", false);
                     return ToolResult::text(yes ? "Deleted" : "Kept");
                 },
                 "optional"});
    t.push_back({"multi_input", "", "Asks two questions at once (task; conformance fixture)", noArgs(), {}, [](const json&, const ToolCall& c) {
                     auto answers = c.input->askAll({{"first", "elicitation/create", elicit("First value?", {{"value", {{"type", "string"}}}}, {"value"})},
                                               {"second", "elicitation/create", elicit("Second value?", {{"value", {{"type", "string"}}}}, {"value"})}});
                     if (!answers[0] || !answers[1]) return needsClient(c, "Elicitation failed");
                     return ToolResult::text("first=" + answers[0]->value("content", json::object()).dump() +
                                             ", second=" + answers[1]->value("content", json::object()).dump());
                 },
                 "optional"});
    t.push_back({"test_tool_with_task", "", "Asks a name (MRTR), then finishes as a task (conformance fixture)", noArgs(), {},
                 [askName, sleepFor](const json&, const ToolCall& c) {
                     auto answer = c.input->ask("user_name", "elicitation/create", askName);
                     if (!answer) return needsClient(c, "Elicitation failed");
                     const std::string name = answer->value("content", json::object()).value("name", std::string("?"));
                     ToolResult later;
                     later.asyncWork = [name, sleepFor](const ToolCall& w) {
                         sleepFor(w, 0.3);
                         return ToolResult::text("Hello, " + name + "! (finished as a task)");
                     };
                     return later;
                 },
                 "required", true});
    return f;
}

} // namespace acestudio::mcp
