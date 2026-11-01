#pragma once

// Scripts > Run Script: runs a sandboxed Lua script (core/Script.h) on a
// worker thread, on a copy of the current document, then applies the result
// to the document as one undo step.

#include <QDialog>

#include <atomic>
#include <filesystem>
#include <memory>
#include <thread>

class QLabel;
class QPlainTextEdit;
class QPushButton;

namespace acestudio {
class Editor;
}

namespace acestudio::ui {

class DocumentView;

class ScriptDialog : public QDialog {
    Q_OBJECT
public:
    // `view` may be null (the script then has no ace.document()).
    ScriptDialog(std::filesystem::path script, DocumentView* view, QWidget* parent = nullptr);
    ~ScriptDialog() override;

    void start();

protected:
    void reject() override;

private:
    void append(const QString& text);
    void finished(bool ok, const QString& error, bool changed, int written);

    std::filesystem::path m_script;
    DocumentView* m_view;
    std::unique_ptr<Editor> m_copy;
    std::atomic<bool> m_cancel{false};
    std::thread m_thread;
    bool m_running = false;
    QLabel* m_status = nullptr;
    QPlainTextEdit* m_output = nullptr;
    QPushButton* m_cancelButton = nullptr;
    QPushButton* m_closeButton = nullptr;
    QPushButton* m_againButton = nullptr;
};

// The user's scripts folder (<data>/scripts), created with an example script
// the first time.
std::filesystem::path scriptsFolder(const std::filesystem::path& dataDir);

} // namespace acestudio::ui
