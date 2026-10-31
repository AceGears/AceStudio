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
    case Tool::Gradient: return QObject::tr("Gradient");
    case Tool::Lasso: return QObject::tr("Lasso Select");
    case Tool::MagicWand: return QObject::tr("Magic Wand");
    case Tool::Eyedropper: return QObject::tr("Eyedropper");
    case Tool::Select: return QObject::tr("Rectangle Select");
    case Tool::Hand: return QObject::tr("Hand");
    case Tool::Curve: return QObject::tr("Curve");
    case Tool::Polygon: return QObject::tr("Polygon");
    case Tool::Clone: return QObject::tr("Clone Stamp");
    case Tool::Move: return QObject::tr("Move");
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
    case Tool::Gradient: return QStringLiteral("D");
    case Tool::Lasso: return QStringLiteral("Q");
    case Tool::MagicWand: return QStringLiteral("W");
    case Tool::Eyedropper: return QStringLiteral("I");
    case Tool::Select: return QStringLiteral("M");
    case Tool::Hand: return QStringLiteral("H");
    case Tool::Curve: return QStringLiteral("C");
    case Tool::Polygon: return QStringLiteral("P");
    case Tool::Clone: return QStringLiteral("S");
    case Tool::Move: return QStringLiteral("V");
    }
    return {};
}

} // namespace acestudio::ui
