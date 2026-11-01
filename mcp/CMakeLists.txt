# acestudio-mcp: the MCP server (Phase 7).
add_library(acestudio_mcp_proto STATIC McpProtocol.h McpProtocol.cpp AceTools.h AceTools.cpp
    ConformanceFixtures.h ConformanceFixtures.cpp ClientConfigs.h ClientConfigs.cpp Hmac.cpp)
target_include_directories(acestudio_mcp_proto PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(acestudio_mcp_proto PUBLIC AceStudio::core nlohmann_json::nlohmann_json)
acestudio_set_warnings(acestudio_mcp_proto)

# The executable needs Qt Core/Network (stdio + HTTP transports, app bridge);
# the protocol library and its tests build without Qt.
find_package(Qt6 6.4 QUIET COMPONENTS Core Network)
if(Qt6_FOUND)
    add_executable(acestudio-mcp main.cpp)
    target_link_libraries(acestudio-mcp PRIVATE acestudio_mcp_proto Qt6::Core Qt6::Network)
    acestudio_set_warnings(acestudio-mcp)
    if(ACESTUDIO_BUILD_CLI)
        add_dependencies(acestudio-mcp acestudio-cli)
    endif()
else()
    message(STATUS "Qt6 Core/Network not found: building the MCP protocol library only (no acestudio-mcp)")
endif()

# The Lua API reference, built into the server as a resource. A byte array
# (MSVC limits string literals to 16 KB).
file(READ "${PROJECT_SOURCE_DIR}/docs/lua-api.md" _lua_api_hex HEX)
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," _lua_api_bytes "${_lua_api_hex}")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/generated/LuaApiDoc.inc" "${_lua_api_bytes}")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/docs/lua-api.md")
target_include_directories(acestudio_mcp_proto PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
