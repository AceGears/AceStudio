find_package(Qt6 6.4 REQUIRED COMPONENTS Core Gui Widgets Network)
include(QtLicenseGuard)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

add_executable(AceStudio WIN32
    resources/app.qrc
    src/main.cpp
    src/ui/Icons.h
    src/ui/Icons.cpp
    src/ui/Motion.h
    src/ui/Motion.cpp
    src/ui/WorkspaceRail.h
    src/ui/WorkspaceRail.cpp
    src/ui/Workspaces.h
    src/pages/GeneratorPage.h
    src/pages/GeneratorPage.cpp
    src/pages/HomePage.h
    src/pages/HomePage.cpp
    src/pages/HubPage.h
    src/pages/HubPage.cpp
    src/pages/LibraryPage.h
    src/pages/LibraryPage.cpp
    src/pages/SettingsPage.h
    src/pages/SettingsPage.cpp
    src/AppContext.h
    src/AppContext.cpp
    src/Theme.h
    src/Theme.cpp
    src/MainWindow.h
    src/MainWindow.cpp
    src/DocumentView.h
    src/DocumentView.cpp
    src/CanvasWidget.h
    src/CanvasWidget.cpp
    src/NewProjectDialog.h
    src/NewProjectDialog.cpp
    src/QtBridge.h
    src/ImageDialogs.h
    src/ImageDialogs.cpp
    src/LayerDialogs.cpp
    src/AnimationDialogs.h
    src/AnimationDialogs.cpp
    src/ExportDialog.h
    src/ExportDialog.cpp
    src/ai/AiService.h
    src/ai/AiService.cpp
    src/ai/AiDialogs.h
    src/ai/AiDialogs.cpp
    src/ai/AiPanel.h
    src/ai/AiPanel.cpp
    src/ai/CharacterDialog.h
    src/ai/CharacterDialog.cpp
    src/ai/PoseEditor.h
    src/ai/PoseEditor.cpp
    src/ai/AnimationDialog.h
    src/ai/AnimationDialog.cpp
    src/ai/AiImageJob.h
    src/ai/AiImageJob.cpp
    src/assets/AssetCommon.h
    src/assets/AssetCommon.cpp
    src/assets/TilesetDialog.h
    src/assets/TilesetDialog.cpp
    src/assets/AssetDialogs.h
    src/assets/AssetDialogs.cpp
    src/scripting/ScriptDialog.h
    src/scripting/ScriptDialog.cpp
    src/scripting/AppBridgeServer.h
    src/scripting/AppBridgeServer.cpp
    src/scripting/McpSetupDialog.h
    src/scripting/McpSetupDialog.cpp
    ${PROJECT_SOURCE_DIR}/mcp/ClientConfigs.h
    ${PROJECT_SOURCE_DIR}/mcp/ClientConfigs.cpp
    src/SingleInstance.h
    src/SingleInstance.cpp
    src/licensing/Licensing.h
    src/Accessibility.h
    src/Accessibility.cpp
    src/selftest/UiAudit.h
    src/selftest/UiAudit.cpp
    src/licensing/Licensing.cpp
    src/Shortcuts.h
    src/Shortcuts.cpp
    src/ToolState.h
    src/ToolState.cpp
    src/panels/ToolsPanel.h
    src/panels/ToolsPanel.cpp
    src/panels/LayersPanel.h
    src/panels/LayersPanel.cpp
    src/panels/PalettePanel.h
    src/panels/PalettePanel.cpp
    src/panels/PropertiesPanel.h
    src/panels/PropertiesPanel.cpp
    src/panels/TimelinePanel.h
    src/panels/TimelinePanel.cpp
    src/panels/LogPanel.h
    src/panels/LogPanel.cpp
    src/panels/PreviewPanel.h
    src/panels/PreviewPanel.cpp
)

target_include_directories(AceStudio PRIVATE src)
target_include_directories(AceStudio PRIVATE "${PROJECT_SOURCE_DIR}/mcp")
target_link_libraries(AceStudio PRIVATE AceStudio::core Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Network)
target_compile_definitions(AceStudio PRIVATE QT_NO_KEYWORDS QT_DISABLE_DEPRECATED_BEFORE=0x060400)
acestudio_set_warnings(AceStudio)
acestudio_check_qt_licenses(AceStudio)

if(WIN32)
    set_target_properties(AceStudio PROPERTIES OUTPUT_NAME "AceStudio")
endif()
acestudio_add_win_resource(AceStudio "AceStudio")

# Help › User Guide and Help › Privacy read these from docs/ next to the
# executable (CI copies them into the distribution the same way).
add_custom_command(TARGET AceStudio POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:AceStudio>/docs
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${PROJECT_SOURCE_DIR}/docs/user-guide.md
            ${PROJECT_SOURCE_DIR}/PRIVACY.md $<TARGET_FILE_DIR:AceStudio>/docs/
    VERBATIM)
