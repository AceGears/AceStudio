#pragma once

#include "Icons.h"

#include <QCoreApplication>
#include <QString>

#include <array>

namespace acestudio::ui {

// The ten workspaces, in rail order. Editor and Animation share the document
// area (with their own dock arrangement); the others are full pages.
enum class Workspace { Home, AiStudio, Characters, Animation, World, Library, Editor, Export, Integrations, Settings };
inline constexpr int kWorkspaceCount = 10;

struct WorkspaceInfo {
    Workspace id;
    const char* key;        // settings / CLI name
    Glyph glyph;
    const char* label;      // rail text (translated with tr)
    const char* tooltip;
};

inline const std::array<WorkspaceInfo, kWorkspaceCount>& workspaceInfos() {
    static const std::array<WorkspaceInfo, kWorkspaceCount> k{{
        {Workspace::Home, "home", Glyph::Home, QT_TRANSLATE_NOOP("Workspace", "Home"), QT_TRANSLATE_NOOP("Workspace", "Dashboard, recent projects and templates")},
        {Workspace::AiStudio, "ai", Glyph::AiStudio, QT_TRANSLATE_NOOP("Workspace", "AI Studio"), QT_TRANSLATE_NOOP("Workspace", "Generate assets from text or images")},
        {Workspace::Characters, "characters", Glyph::Characters, QT_TRANSLATE_NOOP("Workspace", "Characters"), QT_TRANSLATE_NOOP("Workspace", "Character profiles and animation packs")},
        {Workspace::Animation, "animation", Glyph::Animation, QT_TRANSLATE_NOOP("Workspace", "Animation"), QT_TRANSLATE_NOOP("Workspace", "Timeline, frames and preview")},
        {Workspace::World, "world", Glyph::World, QT_TRANSLATE_NOOP("Workspace", "World"), QT_TRANSLATE_NOOP("Workspace", "Tilesets, terrain and backgrounds")},
        {Workspace::Library, "library", Glyph::Library, QT_TRANSLATE_NOOP("Workspace", "Library"), QT_TRANSLATE_NOOP("Workspace", "Your projects and assets")},
        {Workspace::Editor, "editor", Glyph::Editor, QT_TRANSLATE_NOOP("Workspace", "Pixel Editor"), QT_TRANSLATE_NOOP("Workspace", "Draw, layers and palettes")},
        {Workspace::Export, "export", Glyph::Export, QT_TRANSLATE_NOOP("Workspace", "Export"), QT_TRANSLATE_NOOP("Workspace", "Sprite sheets, animations and engines")},
        {Workspace::Integrations, "integrations", Glyph::Integrations, QT_TRANSLATE_NOOP("Workspace", "Connect"), QT_TRANSLATE_NOOP("Workspace", "MCP server and AI coding assistants")},
        {Workspace::Settings, "settings", Glyph::Settings, QT_TRANSLATE_NOOP("Workspace", "Settings"), QT_TRANSLATE_NOOP("Workspace", "Preferences")},
    }};
    return k;
}

inline bool isDocumentWorkspace(Workspace w) { return w == Workspace::Editor || w == Workspace::Animation; }

} // namespace acestudio::ui
