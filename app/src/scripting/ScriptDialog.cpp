#include "ScriptDialog.h"

#include "../DocumentView.h"
#include "../QtBridge.h"

#include "acestudio/core/Editor.h"
#include "acestudio/core/Log.h"
#include "acestudio/core/Script.h"

#include <QCoreApplication>
#include <QDialogButtonBox>
#include <QFontDatabase>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QVBoxLayout>

#include <fstream>

namespace acestudio::ui {

namespace fs = std::filesystem;

namespace {

const char* kExample = R"(-- outline_frame.lua: an AceStudio example script (see the Lua API reference,
-- docs/lua-api.md). It adds a layer with a 1 px outline around the current
-- document's first frame. Run it from Scripts > Run Script; Edit > Undo
-- takes the whole script back in one step.

local doc = ace.document()
if doc == nil then error("open a document first") end

local flat = doc:composite(0)
local outline = flat:outline("#202020")
-- outline() keeps the original pixels: clear them so only the line remains.
for y = 0, flat.height - 1 do
  for x = 0, flat.width - 1 do
    local _, _, _, a = flat:get(x, y)
    if a > 0 then outline:set(x, y, "#00000000") end
  end
end
local layer = doc:add_layer("Outline")
doc:set_cel(layer, 0, outline)
print("added an outline to frame 0 of " .. doc.width .. "x" .. doc.height)
)";

} // namespace

fs::path scriptsFolder(const fs::path& dataDir) {
    const fs::path dir = dataDir / "scripts";
    std::error_code ec;
    if (!fs::exists(dir, ec)) {
        fs::create_directories(dir, ec);
        std::ofstream(dir / "outline_frame.lua", std::ios::binary) << kExample;
    }
    return dir;
}

ScriptDialog::ScriptDialog(fs::path script, DocumentView* view, QWidget* parent)
    : QDialog(parent), m_script(std::move(script)), m_view(view) {
    setWindowTitle(tr("Run Script — %1").arg(fromPath(m_script.filename())));
    resize(620, 420);
    auto* lay = new QVBoxLayout(this);
    m_status = new QLabel(this);
    m_status->setWordWrap(true);
    m_status->setTextInteractionFlags(Qt::TextSelectableByMouse);
    lay->addWidget(m_status);
    m_output = new QPlainTextEdit(this);
    m_output->setReadOnly(true);
    m_output->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    m_output->setMaximumBlockCount(5000);
    lay->addWidget(m_output, 1);
    auto* buttons = new QDialogButtonBox(this);
    m_againButton = buttons->addButton(tr("Run &Again"), QDialogButtonBox::ActionRole);
    m_cancelButton = buttons->addButton(tr("Cancel Script"), QDialogButtonBox::ActionRole);
    m_closeButton = buttons->addButton(QDialogButtonBox::Close);
    lay->addWidget(buttons);
    connect(m_againButton, &QPushButton::clicked, this, [this] {
        m_output->clear();
        start();
    });
    connect(m_cancelButton, &QPushButton::clicked, this, [this] {
        m_cancel = true;
        m_status->setText(tr("Stopping…"));
    });
    connect(m_closeButton, &QPushButton::clicked, this, &QDialog::reject);
}

ScriptDialog::~ScriptDialog() {
    m_cancel = true;
    if (m_thread.joinable()) m_thread.join();
}

void ScriptDialog::start() {
    if (m_running) return;
    if (m_thread.joinable()) m_thread.join();
    m_running = true;
    m_cancel = false;
    m_againButton->setEnabled(false);
    m_closeButton->setEnabled(false);
    m_cancelButton->setEnabled(true);
    m_status->setText(tr("Running %1…").arg(fromPath(m_script)));

    ScriptOptions o;
    std::error_code ec;
    o.allowedRoots.push_back(fs::absolute(m_script, ec).parent_path());
    if (m_view && !m_view->filePath().isEmpty()) o.allowedRoots.push_back(toPath(m_view->filePath()).parent_path());
    // The script edits a copy, so the canvas can keep painting while it runs.
    m_copy.reset();
    m_hadView = m_view != nullptr;
    if (m_view) {
        m_copy = std::make_unique<Editor>(m_view->editor().document());
        m_startMark = m_view->editor().history().mark();
        o.document = m_copy.get();
    }
    o.cancel = &m_cancel;
    QPointer<ScriptDialog> self(this);
    o.output = [self](const std::string& s) {
        const QString text = qs(s);
        QMetaObject::invokeMethod(qApp, [self, text] { if (self) self->append(text); }, Qt::QueuedConnection);
    };
    const fs::path script = m_script;
    m_thread = std::thread([self, script, o] {
        auto r = runScriptFile(script, o);
        const bool ok = static_cast<bool>(r);
        const QString err = ok ? QString() : qs(r.error().message);
        const bool changed = ok && r.value().documentChanged;
        const int written = ok ? static_cast<int>(r.value().written.size()) : 0;
        QMetaObject::invokeMethod(qApp, [self, ok, err, changed, written] {
            if (self) self->finished(ok, err, changed, written);
        }, Qt::QueuedConnection);
    });
}

void ScriptDialog::append(const QString& text) {
    QString t = text;
    if (t.endsWith('\n')) t.chop(1);
    m_output->appendPlainText(t);
}

void ScriptDialog::finished(bool ok, const QString& error, bool changed, int written) {
    if (m_thread.joinable()) m_thread.join();
    m_running = false;
    m_againButton->setEnabled(true);
    m_closeButton->setEnabled(true);
    m_cancelButton->setEnabled(false);
    if (!ok) {
        m_status->setText(tr("The script failed. The document was not changed."));
        append(tr("error: %1").arg(error));
        Logger::instance().log(LogLevel::Warning, "script", "Script " + m_script.string() + " failed: " + error.toStdString());
        m_copy.reset();
        return;
    }
    QStringList parts;
    if (changed && m_hadView && !m_view) {
        m_status->setText(tr("The script ran, but its document was closed meanwhile, so its changes were not applied."));
        m_copy.reset();
        return;
    }
    if (changed && m_view && m_copy && m_view->editor().history().mark() != m_startMark) {
        m_status->setText(tr("The script ran, but the document was edited meanwhile, so its changes were not applied "
                             "(nothing was lost). Run it again to apply it to the current document."));
        m_copy.reset();
        return;
    }
    if (changed && m_view && m_copy) {
        const Status s = m_view->editor().replaceDocument(m_copy->document(), "Run Script: " + m_script.filename().string());
        if (!s) {
            m_status->setText(tr("The script ran, but its changes could not be applied: %1").arg(qs(s.error().message)));
            m_copy.reset();
            return;
        }
        parts << tr("The document was changed (Edit > Undo takes it back in one step).");
    }
    if (written > 0) parts << tr("%n file(s) written.", nullptr, written);
    if (parts.isEmpty()) parts << tr("Done.");
    m_status->setText(parts.join(' '));
    m_copy.reset();
}

void ScriptDialog::reject() {
    if (m_running) {
        // Closing while running cancels first; the dialog closes when it stops.
        m_cancel = true;
        m_status->setText(tr("Stopping…"));
        return;
    }
    QDialog::reject();
}

} // namespace acestudio::ui
