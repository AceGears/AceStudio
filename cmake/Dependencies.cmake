# Third-party dependencies, pinned to exact versions for reproducible builds.
# Every entry here must also appear in LICENSE_AUDIT.md.
include(FetchContent)
set(FETCHCONTENT_QUIET ON)

# nlohmann/json — MIT
FetchContent_Declare(nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.12.0
    GIT_SHALLOW    TRUE
    SYSTEM)
set(JSON_BuildTests OFF CACHE INTERNAL "")
set(JSON_Install    OFF CACHE INTERNAL "")

# miniz — MIT (zip container + deflate + PNG writer)
FetchContent_Declare(miniz
    GIT_REPOSITORY https://github.com/richgel999/miniz.git
    GIT_TAG        3.1.2
    GIT_SHALLOW    TRUE
    SYSTEM)
set(BUILD_EXAMPLES     OFF CACHE INTERNAL "")
set(BUILD_FUZZERS      OFF CACHE INTERNAL "")
set(BUILD_TESTS        OFF CACHE INTERNAL "")
set(INSTALL_PROJECT    OFF CACHE INTERNAL "")
set(AMALGAMATE_SOURCES OFF CACHE INTERNAL "")

# stb (stb_image for PNG decoding) — MIT / public domain, pinned commit
FetchContent_Declare(stb
    GIT_REPOSITORY https://github.com/nothings/stb.git
    GIT_TAG        003501f6d7a8668ce8b6d875e34dd97ad0d5d5b5)

# libwebp — BSD-3-Clause (animated WebP encode/decode via libwebpmux/webpdemux)
FetchContent_Declare(libwebp
    GIT_REPOSITORY https://github.com/webmproject/libwebp.git
    GIT_TAG        v1.6.0
    GIT_SHALLOW    TRUE
    SYSTEM)
foreach(opt WEBP_BUILD_ANIM_UTILS WEBP_BUILD_CWEBP WEBP_BUILD_DWEBP WEBP_BUILD_GIF2WEBP
            WEBP_BUILD_IMG2WEBP WEBP_BUILD_VWEBP WEBP_BUILD_WEBPINFO WEBP_BUILD_WEBPMUX
            WEBP_BUILD_EXTRAS WEBP_BUILD_WEBP_JS WEBP_BUILD_FUZZTEST)
    set(${opt} OFF CACHE INTERNAL "")
endforeach()
set(WEBP_BUILD_LIBWEBPMUX ON CACHE INTERNAL "")
set(WEBP_LINK_STATIC ON CACHE INTERNAL "")

FetchContent_MakeAvailable(nlohmann_json miniz stb libwebp)

# Lua 5.4.9 — MIT (scripting, Phase 7). The official repository has no build
# system; the library is compiled here, as C++ so Lua errors unwind C++ frames
# (sol2's recommended setup, SOL_USING_CXX_LUA).
FetchContent_Declare(lua
    GIT_REPOSITORY https://github.com/lua/lua.git
    GIT_TAG        v5.4.9
    GIT_SHALLOW    TRUE)
# sol2 3.3.0 — MIT (C++ binding for Lua), headers only.
FetchContent_Declare(sol2
    GIT_REPOSITORY https://github.com/ThePhD/sol2.git
    GIT_TAG        v3.3.0
    GIT_SHALLOW    TRUE)
FetchContent_GetProperties(lua)
if(NOT lua_POPULATED)
    FetchContent_Populate(lua)
endif()
FetchContent_GetProperties(sol2)
if(NOT sol2_POPULATED)
    FetchContent_Populate(sol2)
endif()
if(NOT TARGET acestudio_lua)
    file(GLOB _lua_sources "${lua_SOURCE_DIR}/*.c")
    # Not part of the library: interpreters, the all-in-one build and the
    # test library.
    list(FILTER _lua_sources EXCLUDE REGEX "/(lua|luac|onelua|ltests)\\.c$")
    add_library(acestudio_lua STATIC ${_lua_sources})
    set_source_files_properties(${_lua_sources} PROPERTIES LANGUAGE CXX)
    target_include_directories(acestudio_lua SYSTEM PUBLIC "${lua_SOURCE_DIR}")
    target_compile_features(acestudio_lua PUBLIC cxx_std_20)
    if(MSVC)
        target_compile_options(acestudio_lua PRIVATE /W0 /TP)
    else()
        target_compile_options(acestudio_lua PRIVATE -w -x c++)
    endif()
    if(UNIX AND NOT APPLE)
        target_compile_definitions(acestudio_lua PRIVATE LUA_USE_LINUX)
    endif()
    set_target_properties(acestudio_lua PROPERTIES POSITION_INDEPENDENT_CODE ON)
    add_library(acestudio_sol2 INTERFACE)
    target_include_directories(acestudio_sol2 SYSTEM INTERFACE "${sol2_SOURCE_DIR}/include")
    target_compile_definitions(acestudio_sol2 INTERFACE SOL_USING_CXX_LUA=1 SOL_ALL_SAFETIES_ON=1 SOL_PRINT_ERRORS=0)
    target_link_libraries(acestudio_sol2 INTERFACE acestudio_lua)
endif()

if(NOT TARGET acestudio_stb)
    add_library(acestudio_stb INTERFACE)
    target_include_directories(acestudio_stb SYSTEM INTERFACE "${stb_SOURCE_DIR}")
endif()

if(ACESTUDIO_BUILD_TESTS)
    # Catch2 — BSL-1.0 (test-only, never shipped)
    FetchContent_Declare(Catch2
        GIT_REPOSITORY https://github.com/catchorg/Catch2.git
        GIT_TAG        v3.16.1
        GIT_SHALLOW    TRUE
        SYSTEM)
    FetchContent_MakeAvailable(Catch2)
endif()
