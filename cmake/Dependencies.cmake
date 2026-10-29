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

FetchContent_MakeAvailable(nlohmann_json miniz stb)

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
