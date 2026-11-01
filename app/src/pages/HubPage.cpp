#include "HubPage.h"

#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QVBoxLayout>

namespace acestudio::ui {

HubPage::HubPage(const QString& eyebrow, const QString& title, const QString& subtitle, QWidget* parent) : QWidget(parent) {
    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto* content = new QWidget(scroll);
    scroll->setWidget(content);
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->addWidget(scroll);

    m_col = new QVBoxLayout(content);
    m_col->setContentsMargins(40, 28, 40, 32);
    m_col->setSpacing(12);
    auto* e = new QLabel(eyebrow.toUpper(), content);
    e->setProperty("role", "eyebrow");
    auto* h = new QLabel(title, content);
    h->setProperty("role", "h1");
    auto* s = new QLabel(subtitle, content);
    s->setProperty("role", "muted");
    s->setWordWrap(true);
    m_col->addWidget(e);
    m_col->addWidget(h);
    m_col->addWidget(s);
    m_col->addSpacing(8);
    m_grid = new QGridLayout;
    m_grid->setSpacing(14);
    m_col->addLayout(m_grid);
    m_col->addStretch();
}

void HubPage::addSection(const QString& title) {
    // Sections start a new row of cards with a heading.
    if (m_count % 3) m_count += 3 - m_count % 3;
    auto* l = new QLabel(title, this);
    l->setProperty("role", "h2");
    m_grid->addWidget(l, m_count / 3 * 2, 0, 1, 3);
    m_count += 3;
}

void HubPage::addCard(const HubCard& c) {
    auto* card = new QFrame(this);
    card->setProperty("card", true);
    card->setMinimumHeight(150);
    auto* lay = new QVBoxLayout(card);
    lay->setContentsMargins(18, 16, 18, 16);
    auto* head = new QHBoxLayout;
    auto* ic = new QLabel(card);
    ic->setPixmap(icon(c.glyph).pixmap(QSize(26, 26), QIcon::Normal, QIcon::On));
    head->addWidget(ic);
    auto* t = new QLabel(c.title, card);
    t->setProperty("role", "h2");
    head->addWidget(t, 1);
    lay->addLayout(head);
    auto* d = new QLabel(c.description, card);
    d->setProperty("role", "muted");
    d->setWordWrap(true);
    lay->addWidget(d, 1);
    auto* b = new QPushButton(c.action, card);
    b->setAccessibleName(c.title + QStringLiteral(": ") + c.action);
    b->setProperty("accent", true);
    connect(b, &QPushButton::clicked, this, c.run);
    auto* row = new QHBoxLayout;
    row->addWidget(b);
    row->addStretch();
    lay->addLayout(row);
    m_grid->addWidget(card, m_count / 3 * 2 + 1, m_count % 3);
    for (int col = 0; col < 3; ++col) m_grid->setColumnStretch(col, 1);
    ++m_count;
    m_buttons.emplace_back(b, c.needsDocument);
    if (c.needsDocument) {
        b->setEnabled(false);
        b->setToolTip(tr("Open or create a project first"));
    }
}

void HubPage::addWidget(QWidget* w, int stretch) { m_col->insertWidget(m_col->count() - 1, w, stretch); }

void HubPage::setDocumentOpen(bool open) {
    for (auto& [b, needs] : m_buttons)
        if (needs) {
            b->setEnabled(open);
            b->setToolTip(open ? QString() : tr("Open or create a project first"));
        }
}

} // namespace acestudio::ui
