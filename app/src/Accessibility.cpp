#include "Accessibility.h"

#include <QAbstractButton>
#include <QAbstractItemView>
#include <QAbstractSlider>
#include <QAbstractSpinBox>
#include <QApplication>
#include <QBoxLayout>
#include <QComboBox>
#include <QEvent>
#include <QFormLayout>
#include <QGridLayout>
#include <QHash>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPointer>
#include <QScrollBar>
#include <QTextEdit>
#include <QTimer>

namespace acestudio::ui {

namespace {

QString clean(QString s) {
    s.remove(QLatin1Char('&'));
    s = s.trimmed();
    while (s.endsWith(QLatin1Char(':')) || s.endsWith(QChar(0x2026)) || s.endsWith(QStringLiteral("...")))
        s = s.endsWith(QStringLiteral("...")) ? s.chopped(3).trimmed() : s.chopped(1).trimmed();
    // Tooltips may be long: the first sentence names the control.
    if (const qsizetype dot = s.indexOf(QStringLiteral(". ")); dot > 0) s.truncate(dot);
    return s.left(120);
}

QString labelText(QWidget* w) {
    if (auto* l = qobject_cast<QLabel*>(w)) return clean(l->text().contains(QLatin1Char('<')) ? QString() : l->text());
    return {};
}

// The text a sighted user reads beside `w` in its layout: its form label
// (also when it sits in a row layout inside the form), the grid cell to its
// left, or the label right before it in a row.
QString layoutLabel(QWidget* w) {
    QWidget* parent = w->parentWidget();
    if (!parent || !parent->layout()) return {};
    QHash<QLayout*, QLayout*> parentOf;
    QList<QLayout*> todo{parent->layout()};
    QLayout* holder = nullptr;
    while (!todo.isEmpty() && !holder) {
        QLayout* lay = todo.takeFirst();
        for (int i = 0; i < lay->count(); ++i)
            if (QLayout* sub = lay->itemAt(i)->layout()) {
                parentOf.insert(sub, lay);
                todo.append(sub);
            }
        if (lay->indexOf(w) >= 0) holder = lay;
    }
    if (!holder) return {};
    const int idx = holder->indexOf(w);
    if (auto* form = qobject_cast<QFormLayout*>(holder))
        if (QWidget* lbl = form->labelForField(w)) return labelText(lbl);
    if (auto* grid = qobject_cast<QGridLayout*>(holder)) {
        int row = 0, col = 0, rs = 0, cs = 0;
        grid->getItemPosition(idx, &row, &col, &rs, &cs);
        for (int c = col - 1; c >= 0; --c)
            if (QLayoutItem* it = grid->itemAtPosition(row, c); it && it->widget()) {
                const QString t = labelText(it->widget());
                if (!t.isEmpty()) return t;
                break;
            }
    }
    if (qobject_cast<QBoxLayout*>(holder))
        for (int j = idx - 1; j >= 0; --j)
            if (QWidget* before = holder->itemAt(j)->widget()) {
                const QString t = labelText(before);
                if (!t.isEmpty()) return t;
                break;   // only the widget right before counts
            }
    // A row layout used as a form field: the form's label names it.
    for (QLayout* child = holder, *up = parentOf.value(holder); up; child = up, up = parentOf.value(up))
        if (auto* form = qobject_cast<QFormLayout*>(up))
            if (QWidget* lbl = form->labelForField(child)) return labelText(lbl);
    return {};
}

bool needsName(QWidget* w) {
    if (qobject_cast<QScrollBar*>(w) || qobject_cast<QHeaderView*>(w)) return false;
    return qobject_cast<QAbstractButton*>(w) || qobject_cast<QComboBox*>(w) || qobject_cast<QAbstractSpinBox*>(w) ||
           qobject_cast<QAbstractSlider*>(w) || qobject_cast<QLineEdit*>(w) || qobject_cast<QPlainTextEdit*>(w) ||
           qobject_cast<QTextEdit*>(w) || qobject_cast<QAbstractItemView*>(w);
}

class Helper : public QObject {
public:
    using QObject::QObject;
    bool eventFilter(QObject* o, QEvent* e) override {
        if (e->type() == QEvent::Show && o->isWidgetType()) {
            auto* w = static_cast<QWidget*>(o);
            if (w->isWindow()) {
                // After the window's own showEvent has filled it in.
                QPointer<QWidget> p(w);
                QTimer::singleShot(0, w, [p] {
                    if (p) nameControls(p);
                });
            }
        }
        return false;
    }
};

} // namespace

void nameControls(QWidget* root) {
    // Buddies first: an explicit label wins.
    QHash<QWidget*, QString> buddies;
    for (QLabel* l : root->findChildren<QLabel*>())
        if (l->buddy()) buddies.insert(l->buddy(), labelText(l));
    QList<QWidget*> all = root->findChildren<QWidget*>();
    all.prepend(root);
    for (QWidget* w : all) {
        if (!needsName(w) || !w->accessibleName().isEmpty()) continue;
        if (auto* b = qobject_cast<QAbstractButton*>(w); b && !clean(b->text()).isEmpty()) continue;   // its text names it
        QString name = buddies.value(w);
        if (name.isEmpty()) name = layoutLabel(w);
        if (name.isEmpty()) name = clean(w->toolTip());
        if (name.isEmpty())
            if (auto* le = qobject_cast<QLineEdit*>(w)) name = clean(le->placeholderText());
        if (name.isEmpty())
            if (auto* pe = qobject_cast<QPlainTextEdit*>(w)) name = clean(pe->placeholderText());
        if (name.isEmpty())
            if (auto* cb = qobject_cast<QComboBox*>(w); cb && cb->isEditable() && cb->lineEdit()) name = clean(cb->lineEdit()->placeholderText());
        if (!name.isEmpty()) w->setAccessibleName(name);
    }
}

void installAccessibilityHelper(QApplication& app) { app.installEventFilter(new Helper(&app)); }

} // namespace acestudio::ui
