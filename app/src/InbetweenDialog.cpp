#include "InbetweenDialog.h"

#include "DocumentView.h"
#include "QtBridge.h"

#include "acestudio/core/Editor.h"
#include "acestudio/core/Inbetween.h"
#include "acestudio/core/Log.h"

#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QStringList>
#include <QVBoxLayout>

#include <optional>

namespace acestudio::ui {

struct InbetweenDialog::Outcome {
    std::optional<inbetween::DocumentResult> result;
};

InbetweenDialog::InbetweenDialog(DocumentView* view, LayerId layer, int first, int last, QWidget* parent)
    : QDialog(parent), m_view(view), m_layer(layer), m_first(first), m_last(last) {
    setWindowTitle(tr("Generate In-betweens"));
    setObjectName(QStringLiteral("inbetweenDialog"));
    auto* root = new QVBoxLayout(this);
    auto* intro = new QLabel(tr("New frames between the key frames of the current layer. Pixels move between the keys and are "
                                "never blended, so the palette and hard edges are kept. Other layers hold their key's cel."),
                             this);
    intro->setWordWrap(true);
    root->addWidget(intro);

    auto* form = new QFormLayout;
    m_count = new QSpinBox(this);
    m_count->setRange(1, 7);
    m_count->setValue(1);
    m_count->setObjectName(QStringLiteral("inbetween.count"));
    m_count->setToolTip(tr("New frames between each pair of keys"));
    form->addRow(tr("In-betweens per pair:"), m_count);
    m_loop = new QCheckBox(tr("Loop (also between the last key and the first)"), this);
    m_loop->setObjectName(QStringLiteral("inbetween.loop"));
    form->addRow(QString(), m_loop);
    m_timing = new QComboBox(this);
    m_timing->setObjectName(QStringLiteral("inbetween.timing"));
    m_timing->addItem(tr("Split each key's time with its in-betweens"));
    m_timing->addItem(tr("Every frame the same duration"));
    form->addRow(tr("Timing:"), m_timing);
    m_fixedMs = new QSpinBox(this);
    m_fixedMs->setObjectName(QStringLiteral("inbetween.fixedMs"));
    m_fixedMs->setRange(1, 65535);
    m_fixedMs->setValue(80);
    m_fixedMs->setSuffix(tr(" ms"));
    m_fixedMs->setEnabled(false);
    form->addRow(tr("Frame duration:"), m_fixedMs);
    m_tolerance = new QSpinBox(this);
    m_tolerance->setObjectName(QStringLiteral("inbetween.tolerance"));
    m_tolerance->setRange(1, 100);
    m_tolerance->setValue(25);
    m_tolerance->setSuffix(QStringLiteral(" %"));
    m_tolerance->setToolTip(tr("Flag an in-between whose size is this far from what the keys suggest"));
    form->addRow(tr("Flag size changes over:"), m_tolerance);
    root->addLayout(form);

    m_summary = new QLabel(this);
    m_summary->setWordWrap(true);
    root->addWidget(m_summary);
    m_status = new QLabel(this);
    m_status->setWordWrap(true);
    m_status->setObjectName(QStringLiteral("inbetween.status"));
    root->addWidget(m_status);
    m_notes = new QPlainTextEdit(this);
    m_notes->setReadOnly(true);
    m_notes->setMaximumHeight(110);
    m_notes->hide();
    root->addWidget(m_notes);

    auto* buttons = new QDialogButtonBox(this);
    m_generate = buttons->addButton(tr("&Generate"), QDialogButtonBox::AcceptRole);
    m_generate->setObjectName(QStringLiteral("inbetween.generate"));
    m_generate->setDefault(true);
    m_cancelButton = buttons->addButton(tr("Cancel Work"), QDialogButtonBox::ActionRole);
    m_cancelButton->setEnabled(false);
    m_close = buttons->addButton(QDialogButtonBox::Close);
    root->addWidget(buttons);

    connect(m_timing, &QComboBox::currentIndexChanged, this, [this](int i) {
        m_fixedMs->setEnabled(i == 1);
        updateSummary();
    });
    connect(m_count, &QSpinBox::valueChanged, this, [this] { updateSummary(); });
    connect(m_loop, &QCheckBox::toggled, this, [this] { updateSummary(); });
    connect(m_generate, &QPushButton::clicked, this, [this] { start(); });
    connect(m_cancelButton, &QPushButton::clicked, this, [this] {
        m_cancel = true;
        m_status->setText(tr("Stopping…"));
    });
    connect(m_close, &QPushButton::clicked, this, &QDialog::reject);
    updateSummary();
}

InbetweenDialog::~InbetweenDialog() {
    m_cancel = true;
    if (m_thread.joinable()) m_thread.join();
}

void InbetweenDialog::updateSummary() {
    const int keys = m_last - m_first + 1;
    const int pairs = m_loop->isChecked() ? keys : keys - 1;
    m_summary->setText(tr("Keys: frames %1 to %2 (%3 keys). This adds %4 frame(s).")
                           .arg(m_first + 1)
                           .arg(m_last + 1)
                           .arg(keys)
                           .arg(pairs * m_count->value()));
}

void InbetweenDialog::start() {
    if (m_running || !m_view) return;
    if (m_thread.joinable()) m_thread.join();
    m_running = true;
    m_cancel = false;
    m_generate->setEnabled(false);
    m_close->setEnabled(false);
    m_cancelButton->setEnabled(true);
    m_notes->clear();
    m_notes->hide();
    m_status->setText(tr("Matching pixels between the keys…"));

    inbetween::Options o;
    o.intermediates = m_count->value();
    o.areaTolerance = m_tolerance->value() / 100.0;
    o.cancelled = [this] { return m_cancel.load(); };
    const bool loop = m_loop->isChecked();
    const auto timing = m_timing->currentIndex() == 1 ? inbetween::Timing::Fixed : inbetween::Timing::Split;
    const int fixedMs = m_fixedMs->value();
    // The work reads a copy, so the canvas stays usable meanwhile.
    auto doc = std::make_shared<Document>(m_view->editor().document());
    m_startMark = m_view->editor().history().mark();
    auto outcome = std::make_shared<Outcome>();
    m_outcome = outcome;
    const LayerId layer = m_layer;
    const int first = m_first, last = m_last;
    QPointer<InbetweenDialog> self(this);
    m_thread = std::thread([self, doc, outcome, o, layer, first, last, loop, timing, fixedMs] {
        auto r = inbetween::inbetweenLayer(*doc, layer, first, last, loop, timing, fixedMs, o);
        const bool ok = static_cast<bool>(r);
        const QString err = ok ? QString() : qs(r.error().message);
        if (ok) outcome->result = std::move(r).value();
        QMetaObject::invokeMethod(qApp, [self, ok, err] { if (self) self->finished(ok, err); }, Qt::QueuedConnection);
    });
}

void InbetweenDialog::finished(bool ok, const QString& error) {
    if (m_thread.joinable()) m_thread.join();
    m_running = false;
    m_generate->setEnabled(true);
    m_close->setEnabled(true);
    m_cancelButton->setEnabled(false);
    auto outcome = std::move(m_outcome);
    if (!ok) {
        m_status->setText(m_cancel ? tr("Stopped. The document was not changed.") : tr("Nothing was made: %1").arg(error));
        return;
    }
    if (!m_view) {
        m_status->setText(tr("The document was closed meanwhile, so nothing was applied."));
        return;
    }
    if (m_view->editor().history().mark() != m_startMark) {
        m_status->setText(tr("The document was edited meanwhile, so the in-betweens were not applied (nothing was lost). "
                             "Generate again to apply them."));
        return;
    }
    inbetween::DocumentResult& res = *outcome->result;
    const Status st = m_view->editor().replaceDocument(std::move(res.document), "Generate In-betweens");
    if (!st) {
        m_status->setText(tr("The in-betweens could not be applied: %1").arg(qs(st.error().message)));
        return;
    }
    if (!res.newFrames.empty()) m_view->setCurrentFrame(res.newFrames.front());
    QString text = tr("Added %n frame(s) (Edit > Undo takes them back in one step).", nullptr, static_cast<int>(res.newFrames.size()));
    if (!res.flagged.empty()) {
        QStringList frames;
        for (int f : res.flagged) frames << QString::number(f + 1);
        text += QLatin1Char(' ') + tr("Check frame(s) %1: the pose may change too much for an in-between.").arg(frames.join(QStringLiteral(", ")));
        m_notes->setPlainText(qs([&] {
            std::string all;
            for (const auto& n : res.notes) all += n + "\n";
            return all;
        }()));
        m_notes->show();
    }
    m_status->setText(text);
    Logger::instance().log(LogLevel::Info, "inbetween",
                           "Generated " + std::to_string(res.newFrames.size()) + " in-betweens; " + std::to_string(res.flagged.size()) + " flagged");
    // The keys moved, so the dialog's range no longer names them: running
    // again needs a fresh selection (reopen the dialog).
    m_generate->setEnabled(false);
}

void InbetweenDialog::reject() {
    if (m_running) {
        m_cancel = true;
        m_status->setText(tr("Stopping…"));
        return;
    }
    QDialog::reject();
}

} // namespace acestudio::ui
