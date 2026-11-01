add_executable(acestudio_core_tests
    TestSupport.h
    test_color_image.cpp
    test_blend.cpp
    test_document.cpp
    test_layers_advanced.cpp
    test_png.cpp
    test_project_io.cpp
    test_fileutil.cpp
    test_settings.cpp
    test_log.cpp
    test_undo.cpp
    test_editor.cpp
    test_paint.cpp
    test_paint2.cpp
    test_pose.cpp
    test_character.cpp
    test_recovery.cpp
    test_transform.cpp
    test_palette_io.cpp
    test_animation.cpp
    test_animation_qa.cpp
    test_export.cpp
    test_atlas.cpp
    test_autotile.cpp
    test_asset_tools.cpp
    test_engine_export.cpp
    test_script.cpp
    test_licence.cpp
    test_subprocess.cpp
    test_ai_worker.cpp
    test_model_catalog.cpp
    test_regression.cpp
)
add_executable(acestudio_test_child test_child.cpp)
acestudio_set_warnings(acestudio_test_child)
add_dependencies(acestudio_core_tests acestudio_test_child)
target_compile_definitions(acestudio_core_tests PRIVATE
    ACESTUDIO_TEST_CHILD="$<TARGET_FILE:acestudio_test_child>"
    ACESTUDIO_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
target_link_libraries(acestudio_core_tests PRIVATE AceStudio::core miniz Catch2::Catch2WithMain)
acestudio_set_warnings(acestudio_core_tests)

include(Catch)
catch_discover_tests(acestudio_core_tests DISCOVERY_MODE PRE_TEST)

if(ACESTUDIO_BUILD_CLI)
    # End-to-end CLI checks driven through CTest.
    add_test(NAME cli_smoke
        COMMAND ${CMAKE_COMMAND}
            -DCLI=$<TARGET_FILE:acestudio-cli>
            -DWORKDIR=${CMAKE_CURRENT_BINARY_DIR}/cli_smoke
            -DTEXTURES=${CMAKE_CURRENT_SOURCE_DIR}/engine/textures
            "-DLICENCE_TOOL=$<$<TARGET_EXISTS:acestudio-licence>:$<TARGET_FILE:acestudio-licence>>"
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cli_smoke.cmake)
endif()

if(TARGET AceStudio AND ACESTUDIO_BUILD_CLI)
    # Runs every menu command and checks every window for accessibility.
    add_test(NAME ui_audit
        COMMAND ${CMAKE_COMMAND}
            -DAPP=$<TARGET_FILE:AceStudio>
            -DCLI=$<TARGET_FILE:acestudio-cli>
            -DWORKDIR=${CMAKE_CURRENT_BINARY_DIR}/ui_audit
            -P ${CMAKE_CURRENT_SOURCE_DIR}/ui_audit.cmake)
    set_tests_properties(ui_audit PROPERTIES TIMEOUT 1200)
endif()

# Engine-export fixture for the real-engine checks (not a CTest test itself:
# it needs Godot / Unity, see tests/engine/README.md).
add_executable(engine_fixture engine/fixture.cpp)
target_link_libraries(engine_fixture PRIVATE AceStudio::core)

# MCP protocol layer (no Qt needed for these tests).
if(TARGET acestudio_mcp_proto)
    target_sources(acestudio_core_tests PRIVATE test_mcp.cpp test_fuzz_regressions.cpp)
    target_link_libraries(acestudio_core_tests PRIVATE acestudio_mcp_proto)
endif()

# Performance budgets: generous ceilings (bench/perf_budgets.json) that catch
# large regressions; CI machines vary. Only meaningful for optimised builds.
if(TARGET acestudio_bench AND (CMAKE_BUILD_TYPE STREQUAL "Release" OR CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo"))
    add_test(NAME perf_budget
             COMMAND acestudio_bench --budget ${PROJECT_SOURCE_DIR}/bench/perf_budgets.json
                     --json ${CMAKE_CURRENT_BINARY_DIR}/bench_results.json)
    set_tests_properties(perf_budget PROPERTIES LABELS perf RUN_SERIAL TRUE TIMEOUT 600)
endif()
