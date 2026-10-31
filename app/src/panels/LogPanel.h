#pragma once

#include <QWidget>

class QPlainTextEdit;

namespace acestudio::ui {

// Live view of the application log (thread-safe: lines are queued to the UI thread).
class LogPanel : public QWidget {
    Q_OBJECT
public:
    explicit LogPanel(QWidget* parent = nullptr);
    ~LogPanel() override;

Q_SIGNALS:
    void lineArrived(int level, const QString& line);

private:
    QPlainTextEdit* m_text = nullptr;
    bool m_follow = true;   // keep the newest line in view unless the user scrolled up
};

} // namespace acestudio::ui
