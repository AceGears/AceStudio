#pragma once

#include <QDialog>

namespace acestudio::ui {

// Scripts > Set Up AI Assistants: ready-to-paste acestudio-mcp setups for
// Claude Code, Cursor, VS Code and Codex (mcp/ClientConfigs.h).
class McpSetupDialog : public QDialog {
    Q_OBJECT
public:
    explicit McpSetupDialog(QWidget* parent = nullptr);
};

} // namespace acestudio::ui
