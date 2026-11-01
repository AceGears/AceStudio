#include "Shortcuts.h"

#include <QAction>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QLineEdit>
#include <QMap>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPushButton>
#include <QRegularExpression>
#include <QTableWidget>
#include <QVBoxLayout>

namespace acestudio::ui {

namespace {
constexpr const char* kDefaultProp = "aceDefaultShortcut";
constexpr const char* kPathProp = "aceMenuPath";

QString slug(QString text) {
    text.remove(QLatin1Char('&'));
    text.remove(QStringLiteral("…"));
    text = text.toLower().trimmed();
    text.replace(QRegularExpression(QStringLiteral("[^a-z0-9]+")), QStringLiteral("-"));
    while (text.endsWith(QLatin1Char('-'))) text.chop(1);
    while (text.startsWith(QLatin1Char('-'))) text.remove(0, 1);
    return text;
}

QString clean(QString text) {
    text.remove(QLatin1Char('&'));
    return text;
}

void walk(QMenu* menu, const QString& idPrefix, const QString& pathPrefix, const QObject* skip, QList<QAction*>& out) {
    for (QAction* a : menu->actions()) {
        if (a->isSeparator()) continue;
        if (QMenu* sub = a->menu()) {
            if (sub == skip) continue;
            walk(sub, idPrefix + QLatin1Char('.') + slug(sub->title()), pathPrefix + QStringLiteral(" › ") + clean(sub->title()), skip, out);
            continue;
        }
        if (a->text().isEmpty() || a->property("acePlanned").toBool()) continue;
        if (a->objectName().isEmpty()) a->setObjectName(idPrefix + QLatin1Char('.') + slug(a->text()));
        a->setProperty(kDefaultProp, a->shortcut().toString(QKeySequence::PortableText));
        a->setProperty(kPathProp, pathPrefix + QStringLiteral(" › ") + clean(a->text()));
        out << a;
    }
}
} // namespace

void ShortcutRegistry::collect(QMenuBar* bar, const QObject* skipMenu) {
    QList<QAction*> found;
    for (QAction* top : bar->actions())
        if (QMenu* m = top->menu()) walk(m, slug(m->title()), clean(m->title()), skipMenu, found);
    m_actions.clear();
    for (QAction* a : found) m_actions.append(a);
}

QList<QAction*> ShortcutRegistry::actions() const {
    QList<QAction*> live;
    for (const QPointer<QAction>& a : m_actions)
        if (a) live.append(a);
    return live;
}

void ShortcutRegistry::apply(const nlohmann::json& overrides) {
    for (QAction* a : actions()) {
        QString seq = defaultShortcut(a);
        const std::string id = a->objectName().toStdString();
        if (overrides.is_object() && overrides.contains(id) && overrides[id].is_string())
            seq = QString::fromStdString(overrides[id].get<std::string>());
        // Keep any secondary default shortcuts (e.g. Redo's Ctrl+Shift+Z) only
        // while the primary is unchanged.
        if (seq == defaultShortcut(a) && a->shortcuts().size() > 1) continue;
        a->setShortcut(QKeySequence(seq, QKeySequence::PortableText));
    }
}

QStringList ShortcutRegistry::conflicts() const {
    // Exact duplicates, plus Shift+<letter> vs <letter>, which Qt may resolve
    // to the unshifted action.
    QMap<QString, QStringList> byKey;
    for (QAction* a : actions())
        for (const QKeySequence& k : a->shortcuts()) {
            if (k.isEmpty()) continue;
            QString key = k.toString(QKeySequence::PortableText);
            if (key.startsWith(QStringLiteral("Shift+")) && key.size() == 7 && key.at(6).isLetter()) key = key.mid(6);
            byKey[key] << displayName(a);
        }
    QStringList out;
    for (auto it = byKey.cbegin(); it != byKey.cend(); ++it)
        if (it.value().size() > 1) out << QStringLiteral("%1: %2").arg(it.key(), it.value().join(QStringLiteral(", ")));
    return out;
}

QString ShortcutRegistry::defaultShortcut(const QAction* a) { return a->property(kDefaultProp).toString(); }
QString ShortcutRegistry::displayName(const QAction* a) { return a->property(kPathProp).toString(); }

// ------------------------------------------------------------------ dialog

ShortcutsDialog::ShortcutsDialog(const ShortcutRegistry& reg, const nlohmann::json& overrides, QWidget* parent)
    : QDialog(parent), m_reg(reg) {
    setWindowTitle(tr("Keyboard Shortcuts"));
    resize(620, 560);
    m_search = new QLineEdit(this);
    m_search->setPlaceholderText(tr("Filter actions…"));
    m_search->setClearButtonEnabled(true);
    connect(m_search, &QLineEdit::textChanged, this, &ShortcutsDialog::filter);

    m_table = new QTableWidget(static_cast<int>(reg.actions().size()), 3, this);
    m_table->setAccessibleName(tr("Commands and their shortcuts"));
    m_table->setHorizontalHeaderLabels({tr("Action"), tr("Shortcut"), QString()});
    m_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Fixed);
    m_table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_table->setColumnWidth(1, 180);
    m_table->verticalHeader()->setVisible(false);
    m_table->setSelectionMode(QAbstractItemView::NoSelection);

    int row = 0;
    for (QAction* a : reg.actions()) {
        auto* name = new QTableWidgetItem(ShortcutRegistry::displayName(a));
        name->setFlags(Qt::ItemIsEnabled);
        name->setData(Qt::UserRole, a->objectName());
        m_table->setItem(row, 0, name);
        QString current = ShortcutRegistry::defaultShortcut(a);
        const std::string id = a->objectName().toStdString();
        if (overrides.is_object() && overrides.contains(id) && overrides[id].is_string())
            current = QString::fromStdString(overrides[id].get<std::string>());
        auto* edit = new QKeySequenceEdit(QKeySequence(current, QKeySequence::PortableText), m_table);
        m_table->setCellWidget(row, 1, edit);
        auto* reset = new QPushButton(tr("Default"), m_table);
        const QString def = ShortcutRegistry::defaultShortcut(a);
        connect(reset, &QPushButton::clicked, edit, [edit, def] { edit->setKeySequence(QKeySequence(def, QKeySequence::PortableText)); });
        m_table->setCellWidget(row, 2, reset);
        ++row;
    }

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel | QDialogButtonBox::RestoreDefaults, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &ShortcutsDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(buttons->button(QDialogButtonBox::RestoreDefaults), &QPushButton::clicked, this, [this] {
        for (int r = 0; r < m_table->rowCount(); ++r)
            static_cast<QKeySequenceEdit*>(m_table->cellWidget(r, 1))
                ->setKeySequence(QKeySequence(ShortcutRegistry::defaultShortcut(m_reg.actions()[r]), QKeySequence::PortableText));
    });

    auto* hint = new QLabel(tr("Click a shortcut and press the new key combination. Clear it with Backspace while editing."), this);
    hint->setWordWrap(true);
    hint->setEnabled(false);
    auto* root = new QVBoxLayout(this);
    root->addWidget(m_search);
    root->addWidget(m_table, 1);
    root->addWidget(hint);
    root->addWidget(buttons);
}

void ShortcutsDialog::filter(const QString& text) {
    for (int r = 0; r < m_table->rowCount(); ++r)
        m_table->setRowHidden(r, !text.isEmpty() && !m_table->item(r, 0)->text().contains(text, Qt::CaseInsensitive));
}

void ShortcutsDialog::accept() {
    QMap<QString, QStringList> byKey;
    nlohmann::json result = nlohmann::json::object();
    for (int r = 0; r < m_table->rowCount(); ++r) {
        QAction* a = m_reg.actions()[r];
        const QKeySequence seq = static_cast<QKeySequenceEdit*>(m_table->cellWidget(r, 1))->keySequence();
        const QString text = seq.toString(QKeySequence::PortableText);
        if (!text.isEmpty()) byKey[text] << ShortcutRegistry::displayName(a);
        if (text != ShortcutRegistry::defaultShortcut(a)) result[a->objectName().toStdString()] = text.toStdString();
    }
    QStringList conflicts;
    for (auto it = byKey.cbegin(); it != byKey.cend(); ++it)
        if (it.value().size() > 1)
            conflicts << QStringLiteral("%1: %2").arg(QKeySequence(it.key(), QKeySequence::PortableText).toString(QKeySequence::NativeText),
                                                      it.value().join(QStringLiteral(", ")));
    if (!conflicts.isEmpty()) {
        QMessageBox::warning(this, tr("Shortcut Conflicts"),
                             tr("These shortcuts are assigned to more than one action:\n\n%1\n\nChange them before saving.")
                                 .arg(conflicts.join(QLatin1Char('\n'))));
        return;
    }
    m_result = result;
    QDialog::accept();
}

} // namespace acestudio::ui
