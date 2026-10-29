find_package(Qt6 6.4 REQUIRED COMPONENTS Core Gui Widgets)
include(QtLicenseGuard)

set(CMAKE_AUTOMOC ON)

add_executable(AceStudio WIN32
    src/main.cpp
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
)

target_include_directories(AceStudio PRIVATE src)
target_link_libraries(AceStudio PRIVATE AceStudio::core Qt6::Core Qt6::Gui Qt6::Widgets)
target_compile_definitions(AceStudio PRIVATE QT_NO_KEYWORDS QT_DISABLE_DEPRECATED_BEFORE=0x060400)
acestudio_set_warnings(AceStudio)
acestudio_check_qt_licenses(AceStudio)

if(WIN32)
    set_target_properties(AceStudio PROPERTIES OUTPUT_NAME "AceStudio")
endif()
