#pragma once

#include "acestudio/core/Character.h"

#include <QDialog>
#include <QPointer>

#include <filesystem>
#include <optional>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPlainTextEdit;
class QPushButton;
class QSpinBox;

namespace acestudio::ui {

class DocumentView;

// The character library (Phase 5): create and edit character profiles —
// description, palette, reference images, size, seed and strengths — stored
// as .acechar files in <data>/characters.
class CharacterDialog : public QDialog {
    Q_OBJECT
public:
    CharacterDialog(std::filesystem::path libraryDir, DocumentView* view, QWidget* parent = nullptr);
    // The profile selected when the dialog closed (if any).
    std::optional<std::filesystem::path> selectedPath() const;

    static std::filesystem::path pathFor(const std::filesystem::path& dir, const CharacterProfile& c);

private:
    void reloadList(const QString& selectId = {});
    void showProfile(const CharacterProfile& c);
    CharacterProfile collect() const;
    bool saveCurrent();
    bool maybeSave();
    void refreshReferences();
    void refreshPalette();

    std::filesystem::path m_dir;
    QPointer<DocumentView> m_view;
    std::optional<CharacterProfile> m_current;
    bool m_dirty = false;
    bool m_loading = false;

    QListWidget* m_list = nullptr;
    QLineEdit* m_name = nullptr;
    QPlainTextEdit* m_prompt = nullptr;
    QLineEdit* m_negative = nullptr;
    QSpinBox* m_width = nullptr;
    QSpinBox* m_height = nullptr;
    QCheckBox* m_fixedSeed = nullptr;
    QSpinBox* m_seed = nullptr;
    QSpinBox* m_style = nullptr;
    QSpinBox* m_refStrength = nullptr;
    QComboBox* m_facing = nullptr;
    QLabel* m_palette = nullptr;
    QListWidget* m_refs = nullptr;
    QPlainTextEdit* m_notes = nullptr;
    QWidget* m_form = nullptr;
    QPushButton* m_save = nullptr;
};

} // namespace acestudio::ui
