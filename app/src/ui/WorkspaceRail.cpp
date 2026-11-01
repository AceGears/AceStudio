#include "WorkspaceRail.h"

#include "../Theme.h"
#include "Motion.h"

#include <QButtonGroup>
#include <QLabel>
#include <QLinearGradient>
#include <QPainter>
#include <QToolButton>
#include <QVariantAnimation>
#include <QVBoxLayout>

namespace acestudio::ui {

WorkspaceRail::WorkspaceRail(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("workspaceRail"));
    setAttribute(Qt::WA_StyledBackground, false);
    setFixedWidth(84);
    m_layout = new QVBoxLayout(this);
    m_layout->setContentsMargins(6, 10, 6, 10);
    m_layout->setSpacing(2);

    auto* logo = new QLabel(this);
    logo->setPixmap(QPixmap(QStringLiteral(":/branding/logo-128.png")).scaled(56, 56, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    logo->setAlignment(Qt::AlignCenter);
    logo->setAccessibleName(tr("AceStudio"));
    logo->setFixedHeight(64);
    m_layout->addWidget(logo);
    m_layout->addSpacing(6);

    m_group = new QButtonGroup(this);
    m_group->setExclusive(true);
    connect(m_group, &QButtonGroup::idClicked, this, [this](int id) {
        if (id == m_current) return;
        setCurrentIndex(id);
        Q_EMIT currentChanged(id);
    });

    m_anim = new QVariantAnimation(this);
    m_anim->setEasingCurve(QEasingCurve::OutCubic);
    connect(m_anim, &QVariantAnimation::valueChanged, this, [this](const QVariant& v) {
        m_indicatorY = v.toReal();
        update();
    });
}

int WorkspaceRail::addWorkspace(Glyph glyph, const QString& label, const QString& toolTip) {
    auto* b = new QToolButton(this);
    b->setIcon(icon(glyph));
    b->setIconSize(QSize(22, 22));
    b->setText(label);
    b->setToolTip(toolTip);
    b->setAccessibleName(label);
    b->setAccessibleDescription(toolTip);
    b->setCheckable(true);
    b->setAutoRaise(true);
    b->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
    b->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    b->setMinimumHeight(54);
    b->setStyleSheet(QStringLiteral("QToolButton { font-size: 8pt; padding: 4px 2px; border-radius: 8px; }"));
    const int id = static_cast<int>(m_buttons.size());
    m_group->addButton(b, id);
    m_buttons.push_back(b);
    m_layout->addWidget(b);
    return id;
}

void WorkspaceRail::addStretch() { m_layout->addStretch(1); }

QToolButton* WorkspaceRail::button(int index) const {
    return index >= 0 && index < static_cast<int>(m_buttons.size()) ? m_buttons[static_cast<std::size_t>(index)] : nullptr;
}

void WorkspaceRail::setCurrentIndex(int index) {
    if (!button(index)) return;
    const bool first = m_current < 0;
    m_current = index;
    button(index)->setChecked(true);
    moveIndicator(!first);
}

void WorkspaceRail::moveIndicator(bool animate) {
    QToolButton* b = button(m_current);
    if (!b) return;
    const QRect g = b->geometry();
    m_indicatorH = g.height() * 0.5;
    const qreal target = g.center().y() - m_indicatorH / 2;
    const int ms = animate && m_indicatorY >= 0 ? motion::duration(220) : 0;
    m_anim->stop();
    if (ms <= 0) {
        m_indicatorY = target;
        update();
        return;
    }
    m_anim->setDuration(ms);
    m_anim->setStartValue(m_indicatorY);
    m_anim->setEndValue(target);
    m_anim->start();
}

void WorkspaceRail::resizeEvent(QResizeEvent* e) {
    QWidget::resizeEvent(e);
    moveIndicator(false);
}

void WorkspaceRail::paintEvent(QPaintEvent*) {
    const ThemeTokens& t = themeTokens();
    QPainter p(this);
    // Graphite rail with a faint vertical sheen and a hairline on the right.
    QLinearGradient bg(0, 0, 0, height());
    bg.setColorAt(0, t.metalHi);
    bg.setColorAt(1, t.window);
    p.fillRect(rect(), t.highContrast ? QBrush(t.window) : QBrush(bg));
    p.fillRect(QRect(width() - 1, 0, 1, height()), t.border);
    if (m_indicatorY >= 0 && m_current >= 0) {
        p.setRenderHint(QPainter::Antialiasing);
        QLinearGradient g(0, m_indicatorY, 0, m_indicatorY + m_indicatorH);
        g.setColorAt(0, t.accentHi);
        g.setColorAt(1, t.accent);
        p.setPen(Qt::NoPen);
        p.setBrush(g);
        p.drawRoundedRect(QRectF(1, m_indicatorY, 3.5, m_indicatorH), 1.75, 1.75);
    }
}

} // namespace acestudio::ui
