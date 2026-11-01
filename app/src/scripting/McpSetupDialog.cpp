#include "McpSetupDialog.h"

#include "../QtBridge.h"

#include "ClientConfigs.h"

#include "acestudio/core/FileUtil.h"

#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFontDatabase>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QVBoxLayout>

namespace acestudio::ui {

McpSetupDialog::McpSetupDialog(QWidget* parent) : QDialog(parent) {
    setWindowTitle(tr("Set Up AI Assistants (MCP)"));
    resize(640, 480);
#ifdef _WIN32
    const auto exe = executableDir() / "acestudio-mcp.exe";
#else
    const auto exe = executableDir() / "acestudio-mcp";
#endif
    const auto configs = mcp::clientConfigs(exe);

    auto* lay = new QVBoxLayout(this);
    auto* intro = new QLabel(tr("AI coding assistants can use AceStudio through its MCP server, <b>acestudio-mcp</b>: "
                                "create and render projects, export for Godot and Unity, generate tilesets and UI kits, "
                                "run Lua scripts, and look at or edit the document open here (Scripts &gt; Allow AI "
                                "Assistants). Pick your assistant and paste this setup:"),
                             this);
    intro->setWordWrap(true);
    lay->addWidget(intro);
    auto* clientLabel = new QLabel(tr("AI &assistant:"), this);
    lay->addWidget(clientLabel);
    auto* client = new QComboBox(this);
    for (const auto& c : configs) client->addItem(qs(c.name));
    client->setAccessibleName(tr("AI assistant"));
    clientLabel->setBuddy(client);
    lay->addWidget(client);
    auto* where = new QLabel(this);
    where->setWordWrap(true);
    where->setTextInteractionFlags(Qt::TextSelectableByMouse);
    lay->addWidget(where);
    auto* text = new QPlainTextEdit(this);
    text->setReadOnly(true);
    text->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    lay->addWidget(text, 1);
    auto* note = new QLabel(this);
    note->setWordWrap(true);
    lay->addWidget(note);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    QPushButton* copy = buttons->addButton(tr("&Copy"), QDialogButtonBox::ActionRole);
    lay->addWidget(buttons);

    auto show = [=](int i) {
        if (i < 0 || i >= static_cast<int>(configs.size())) return;
        where->setText(tr("Put this in: %1").arg(qs(configs[static_cast<std::size_t>(i)].file)));
        text->setPlainText(qs(configs[static_cast<std::size_t>(i)].content));
        note->setText(qs(configs[static_cast<std::size_t>(i)].note));
    };
    connect(client, &QComboBox::currentIndexChanged, this, show);
    connect(copy, &QPushButton::clicked, this, [text, copy] {
        QApplication::clipboard()->setText(text->toPlainText());
        copy->setText(tr("Copied"));
    });
    connect(client, &QComboBox::currentIndexChanged, this, [copy] { copy->setText(tr("&Copy")); });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    show(0);
}

} // namespace acestudio::ui
