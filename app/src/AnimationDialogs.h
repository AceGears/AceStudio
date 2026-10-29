#pragma once

#include "acestudio/core/Animation.h"

#include <QDialog>
#include <QPointer>

class QCheckBox;
class QComboBox;
class QLineEdit;
class QListWidget;
class QPushButton;
class QSpinBox;

namespace acestudio::ui {

class DocumentView;

// Edits one tag's fields. Frame numbers are shown 1-based.
class TagEditDialog : public QDialog {
    Q_OBJECT
public:
    TagEditDialog(const AnimationTag& tag, int frameCount, QWidget* parent = nullptr);
    AnimationTag tag() const;

private:
    void updateColorButton();

    QLineEdit* m_name = nullptr;
    QSpinBox* m_from = nullptr;
    QSpinBox* m_to = nullptr;
    QComboBox* m_direction = nullptr;
    QSpinBox* m_repeat = nullptr;
    QPushButton* m_color = nullptr;
    QString m_colorValue;
};

// Lists a document's tags; add, edit and remove go through the Editor, so
// every change is undoable and applied immediately.
class TagsDialog : public QDialog {
    Q_OBJECT
public:
    TagsDialog(DocumentView* view, QWidget* parent = nullptr);

private:
    void rebuild();
    void addTag();
    void editTag();
    void removeTag();

    QPointer<DocumentView> m_view;
    QListWidget* m_list = nullptr;
    QPushButton* m_edit = nullptr;
    QPushButton* m_remove = nullptr;
};

// Onion-skin range, opacity and tint.
class OnionSkinDialog : public QDialog {
    Q_OBJECT
public:
    OnionSkinDialog(const OnionSkin& opts, QWidget* parent = nullptr);
    OnionSkin options() const;

private:
    QSpinBox* m_before = nullptr;
    QSpinBox* m_after = nullptr;
    QSpinBox* m_opacity = nullptr;
    QCheckBox* m_tint = nullptr;
    QCheckBox* m_wrap = nullptr;
};

QString directionName(PlaybackDirection d);

} // namespace acestudio::ui
