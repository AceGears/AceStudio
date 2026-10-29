#include "ToolState.h"

namespace acestudio::ui {

QString toolName(Tool t) {
    switch (t) {
    case Tool::Pencil: return QObject::tr("Pencil");
    case Tool::Eraser: return QObject::tr("Eraser");
    case Tool::Line: return QObject::tr("Line");
    case Tool::Rectangle: return QObject::tr("Rectangle");
    case Tool::Ellipse: return QObject::tr("Ellipse");
    case Tool::Fill: return QObject::tr("Paint Bucket");
    case Tool::Eyedropper: return QObject::tr("Eyedropper");
    case Tool::Select: return QObject::tr("Rectangle Select");
    case Tool::Hand: return QObject::tr("Hand");
    }
    return {};
}

QString toolShortcut(Tool t) {
    switch (t) {
    case Tool::Pencil: return QStringLiteral("B");
    case Tool::Eraser: return QStringLiteral("E");
    case Tool::Line: return QStringLiteral("L");
    case Tool::Rectangle: return QStringLiteral("U");
    case Tool::Ellipse: return QStringLiteral("O");
    case Tool::Fill: return QStringLiteral("G");
    case Tool::Eyedropper: return QStringLiteral("I");
    case Tool::Select: return QStringLiteral("M");
    case Tool::Hand: return QStringLiteral("H");
    }
    return {};
}

} // namespace acestudio::ui
