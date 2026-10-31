#include "LogPanel.h"

#include "acestudio/core/Log.h"

#include <QPlainTextEdit>
#include <QScrollBar>
#include <QVBoxLayout>

namespace acestudio::ui {

LogPanel::LogPanel(QWidget* parent) : QWidget(parent) {
    m_text = new QPlainTextEdit(this);
    m_text->setReadOnly(true);
    m_text->setMaximumBlockCount(2000);
    m_text->setFont(QFont(QStringLiteral("Consolas"), 9));
    m_text->setLineWrapMode(QPlainTextEdit::NoWrap);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(6, 6, 6, 6);
    layout->addWidget(m_text);

    // Follow the tail. QPlainTextEdit only does this when the view is
    // already at the bottom, which fails while the panel is hidden in a tab
    // (no scroll range yet), so the view stayed on the first lines.
    QScrollBar* bar = m_text->verticalScrollBar();
    connect(bar, &QScrollBar::valueChanged, this, [this, bar](int v) { m_follow = v >= bar->maximum() - 2; });
    connect(bar, &QScrollBar::rangeChanged, this, [this, bar](int, int max) {
        if (m_follow) bar->setValue(max);
    });

    connect(this, &LogPanel::lineArrived, this, [this](int level, const QString& line) {
        const QString color = level >= static_cast<int>(LogLevel::Error)   ? QStringLiteral("#FF6B6B")
                            : level >= static_cast<int>(LogLevel::Warning) ? QStringLiteral("#F2B84B")
                                                                           : QString();
        if (color.isEmpty()) m_text->appendPlainText(line);
        else m_text->appendHtml(QStringLiteral("<span style='color:%1'>%2</span>").arg(color, line.toHtmlEscaped()));
        if (m_follow) m_text->verticalScrollBar()->setValue(m_text->verticalScrollBar()->maximum());
    }, Qt::QueuedConnection);

    Logger::instance().setListener([this](LogLevel level, std::string_view, std::string_view line) {
        Q_EMIT lineArrived(static_cast<int>(level), QString::fromUtf8(line.data(), static_cast<qsizetype>(line.size())));
    });
}

LogPanel::~LogPanel() { Logger::instance().setListener({}); }

} // namespace acestudio::ui
