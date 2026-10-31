# acestudio_core: platform-neutral engine. Must never depend on Qt so the
# CLI, MCP server, tests and plugins can all share it.
add_library(acestudio_core STATIC
    src/Adjust.cpp
    src/AiWorker.cpp
    src/AnimCodec.cpp
    src/Animation.cpp
    src/BlendMode.cpp
    src/Color.cpp
    src/Document.cpp
    src/Editor.cpp
    src/Export.cpp
    src/FileUtil.cpp
    src/Image.cpp
    src/Log.cpp
    src/ModelCatalog.cpp
    src/Paint.cpp
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
    PRIVATE miniz acestudio_stb webp webpdemux libwebpmux Threads::Threads)

# Lets development builds find ai_worker/ in the source tree.
set_source_files_properties(src/AiWorker.cpp PROPERTIES
    COMPILE_DEFINITIONS "ACESTUDIO_DEV_SOURCE_DIR=\"${PROJECT_SOURCE_DIR}\"")

if(WIN32)
    target_link_libraries(acestudio_core PRIVATE shell32 ole32)
    target_compile_definitions(acestudio_core PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN)
endif()

acestudio_set_warnings(acestudio_core)
# Third-party implementation unit: silence its warnings only.
if(MSVC)
    set_source_files_properties(src/StbImageImpl.cpp PROPERTIES COMPILE_OPTIONS "/w")
else()
    set_source_files_properties(src/StbImageImpl.cpp PROPERTIES COMPILE_OPTIONS "-w")
endif()
