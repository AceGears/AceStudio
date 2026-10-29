add_executable(acestudio_core_tests
    TestSupport.h
    test_color_image.cpp
    test_blend.cpp
    test_document.cpp
    test_png.cpp
    test_project_io.cpp
    test_fileutil.cpp
    test_settings.cpp
    test_log.cpp
)
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
