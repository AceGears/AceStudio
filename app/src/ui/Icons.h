#pragma once
// AceStudio's own icon set, drawn as vector paths on a 24-unit grid, so
// every icon is sharp at any size and DPI and follows the theme colours
// (muted normally, accent when selected or checked, greyed when disabled).

#include <QIcon>

namespace acestudio::ui {

enum class Glyph {
    Home, AiStudio, Characters, Animation, World, Library, Editor, Export, Integrations, Settings,
    New, Open, Sparkle, Folder, Play, Search, Star, Tag, Grid, Image, Check, Warning, Plus, Trash, Refresh
};

QIcon icon(Glyph g);

} // namespace acestudio::ui
