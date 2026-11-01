cmake_minimum_required(VERSION 3.25)

project(AceStudio
    VERSION 0.9.0
    DESCRIPTION "AI-assisted game asset creation and 2D animation studio"
    LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

option(ACESTUDIO_BUILD_APP   "Build the Qt desktop application" ON)
option(ACESTUDIO_BUILD_CLI   "Build acestudio-cli"              ON)
option(ACESTUDIO_BUILD_MCP   "Build acestudio-mcp (needs Qt Core/Network)" ON)
option(ACESTUDIO_BUILD_TESTS "Build unit tests"                 ON)
option(ACESTUDIO_BUILD_TOOLS "Build AceGears tools (licence keys)" ON)
option(ACESTUDIO_BUILD_BENCH "Build micro-benchmarks"         ON)
option(ACESTUDIO_WARNINGS_AS_ERRORS "Treat warnings as errors"  OFF)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(CompilerWarnings)
include(Dependencies)

# Version header shared by every target.
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/core/include/acestudio/core/Version.h.in"
    "${CMAKE_CURRENT_BINARY_DIR}/generated/acestudio/core/Version.h"
    @ONLY)

# Licence keys are checked against this Ed25519 public key (64 hex digits).
# Release builds set it to AceGears' key (tools/licence/README.md); empty means
# the built-in development key, whose secret half is public, and the app says so.
set(ACESTUDIO_LICENCE_PUBLIC_KEY "" CACHE STRING "Ed25519 public key for licence keys (hex); empty = development key")
if(ACESTUDIO_LICENCE_PUBLIC_KEY AND NOT ACESTUDIO_LICENCE_PUBLIC_KEY MATCHES "^[0-9a-fA-F]+$")
    message(FATAL_ERROR "ACESTUDIO_LICENCE_PUBLIC_KEY must be 64 hex digits")
endif()
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/core/include/acestudio/core/LicenceConfig.h.in"
    "${CMAKE_CURRENT_BINARY_DIR}/generated/acestudio/core/LicenceConfig.h"
    @ONLY)

add_subdirectory(core)

if(ACESTUDIO_BUILD_CLI)
    add_subdirectory(cli)
endif()

if(ACESTUDIO_BUILD_MCP)
    add_subdirectory(mcp)
endif()

if(ACESTUDIO_BUILD_APP)
    add_subdirectory(app)
endif()

if(ACESTUDIO_BUILD_TOOLS)
    add_subdirectory(tools/licence)
endif()

if(ACESTUDIO_BUILD_BENCH)
    add_subdirectory(bench)
endif()

if(ACESTUDIO_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
