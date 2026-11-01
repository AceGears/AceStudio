# acestudio_core: platform-neutral engine. Must never depend on Qt so the
# CLI, MCP server, tests and plugins can all share it.
add_library(acestudio_core STATIC
    src/Adjust.cpp
    src/Atlas.cpp
    src/Autotile.cpp
    src/AiWorker.cpp
    src/AnimCodec.cpp
    src/Animation.cpp
    src/AnimationQA.cpp
    src/AssetTools.cpp
    src/EngineExport.cpp
    src/AssetFiles.cpp
    src/Script.cpp
    src/AppBridge.cpp
    src/BlendMode.cpp
    src/Character.cpp
    src/Color.cpp
    src/Document.cpp
    src/Editor.cpp
    src/Export.cpp
    src/FileUtil.cpp
    src/Image.cpp
    src/Log.cpp
    src/ModelCatalog.cpp
    src/Paint.cpp
    src/Pose.cpp
    src/Palette.cpp
    src/PaletteIO.cpp
    src/PngCodec.cpp
    src/ProjectIO.cpp
    src/Raster.cpp
    src/Recovery.cpp
    src/Settings.cpp
    src/StbImageImpl.cpp
    src/Subprocess.cpp
    src/Transform.cpp
    src/UndoStack.cpp
)
add_library(AceStudio::core ALIAS acestudio_core)

target_include_directories(acestudio_core
    PUBLIC  "${CMAKE_CURRENT_SOURCE_DIR}/include"
            "${PROJECT_BINARY_DIR}/generated"
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")

find_package(Threads REQUIRED)
target_link_libraries(acestudio_core
    PUBLIC  nlohmann_json::nlohmann_json
    PRIVATE miniz acestudio_stb webp webpdemux libwebpmux Threads::Threads acestudio_sol2)

# Lets development builds find ai_worker/ in the source tree.
set_source_files_properties(src/AiWorker.cpp PROPERTIES
    COMPILE_DEFINITIONS "ACESTUDIO_DEV_SOURCE_DIR=\"${PROJECT_SOURCE_DIR}\"")

if(WIN32)
    target_link_libraries(acestudio_core PRIVATE shell32 ole32 advapi32)
    target_compile_definitions(acestudio_core PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN)
endif()

acestudio_set_warnings(acestudio_core)
# Third-party implementation unit: silence its warnings only.
if(MSVC)
    set_source_files_properties(src/StbImageImpl.cpp PROPERTIES COMPILE_OPTIONS "/w")
else()
    set_source_files_properties(src/StbImageImpl.cpp PROPERTIES COMPILE_OPTIONS "-w")
endif()

# The Unity tile scripts (core/unity/*.cs) that engine export writes next to
# tilesets, built in as byte arrays (MSVC limits string literals to 16 KB).
foreach(_script AceStudioAutoTile AceStudioWangSet AceStudioDualGrid)
    set(_cs "${CMAKE_CURRENT_SOURCE_DIR}/unity/${_script}.cs")
    file(READ "${_cs}" _hex HEX)
    string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," _bytes "${_hex}")
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/generated/${_script}.cs.inc" "${_bytes}")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_cs}")
endforeach()
target_include_directories(acestudio_core PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
