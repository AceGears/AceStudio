#include "McpSetupDialog.h"

#include "../QtBridge.h"

#include "ClientConfigs.h"

#include "acestudio/core/FileUtil.h"

#include <nlohmann/json.hpp>

#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFontDatabase>
#include <QLabel>
#include <QPlainTextEdit>
#include <QProcess>
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
    // "Add for Me": acestudio-cli connect merges the entry into the client's own config file (backup kept).
    QPushButton* addForMe = buttons->addButton(tr("&Add for Me"), QDialogButtonBox::ActionRole);
    addForMe->setToolTip(tr("Adds AceStudio to this assistant's user-wide settings file, keeping everything else in it "
                            "(the previous file is kept as .bak)."));
    connect(addForMe, &QPushButton::clicked, this, [this, client, note, configs] {
        const int i = client->currentIndex();
        if (i < 0 || i >= static_cast<int>(configs.size())) return;
        std::string id = configs[static_cast<std::size_t>(i)].id;
        if (id == "claude-code") id = "claude";
#ifdef _WIN32
        const auto cli = executableDir() / "acestudio-cli.exe";
#else
        const auto cli = executableDir() / "acestudio-cli";
#endif
        QProcess p;
        p.start(fromPath(cli), {QStringLiteral("--json"), QStringLiteral("connect"), qs(id), QStringLiteral("--global")});
        if (!p.waitForFinished(15000)) {
            note->setText(tr("Could not run acestudio-cli: %1").arg(p.errorString()));
            return;
        }
        const auto j = nlohmann::json::parse(p.readAllStandardOutput().toStdString(), nullptr, false);
        if (j.is_discarded() || !j.value("ok", false)) {
            note->setText(tr("Not added: %1").arg(j.is_discarded() ? QString::fromLocal8Bit(p.readAllStandardError()) : qs(j.value("error", std::string()))));
        } else if (j.contains("command")) {
            QApplication::clipboard()->setText(qs(j["command"].get<std::string>()));
            note->setText(tr("Claude Code keeps user-wide servers in a file it manages itself. This command is on your "
                             "clipboard; run it in a terminal:\n%1").arg(qs(j["command"].get<std::string>())));
        } else {
            note->setText(tr("Added to %1. Restart the assistant (or reload its MCP servers) to use it.").arg(qs(j.value("file", std::string()))));
        }
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    show(0);
}

} // namespace acestudio::ui
