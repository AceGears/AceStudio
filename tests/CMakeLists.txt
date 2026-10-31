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
    test_recovery.cpp
    test_transform.cpp
    test_palette_io.cpp
    test_animation.cpp
    test_export.cpp
    test_subprocess.cpp
    test_ai_worker.cpp
    test_model_catalog.cpp
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
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cli_smoke.cmake)
endif()
