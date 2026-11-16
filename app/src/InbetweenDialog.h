#pragma once

// Animation > Generate In-betweens: new frames between the selected key
// frames of the current layer (core/Inbetween.h). Pixels move between the
// keys and are never blended, so the palette and binary alpha are kept.
// The work runs on a worker thread, on a copy of the document; the result is
// applied as one undo step. If the document is edited (or closed) meanwhile,
// nothing is applied, so no edit is lost.

#include "acestudio/core/Document.h"

#include <QDialog>
#include <QPointer>

#include <atomic>
#include <memory>
#include <thread>

class QCheckBox;
class QComboBox;
class QLabel;
class QPlainTextEdit;
class QPushButton;
class QSpinBox;

namespace acestudio::ui {

class DocumentView;

class InbetweenDialog : public QDialog {
    Q_OBJECT
public:
    // Keys: frames first..last of `layer`.
    InbetweenDialog(DocumentView* view, LayerId layer, int first, int last, QWidget* parent = nullptr);
    ~InbetweenDialog() override;

protected:
    void reject() override;

private:
    void start();
    void finished(bool ok, const QString& error);
    void updateSummary();

    QPointer<DocumentView> m_view;
    LayerId m_layer;
    int m_first, m_last;
    std::size_t m_startMark = 0;
    std::atomic<bool> m_cancel{false};
    std::thread m_thread;
    bool m_running = false;
    struct Outcome;
    std::shared_ptr<Outcome> m_outcome;

    QSpinBox* m_count = nullptr;
    QCheckBox* m_loop = nullptr;
    QComboBox* m_timing = nullptr;
    QSpinBox* m_fixedMs = nullptr;
    QSpinBox* m_tolerance = nullptr;
    QLabel* m_summary = nullptr;
    QLabel* m_status = nullptr;
    QPlainTextEdit* m_notes = nullptr;
    QPushButton* m_generate = nullptr;
    QPushButton* m_cancelButton = nullptr;
    QPushButton* m_close = nullptr;
};

} // namespace acestudio::ui
