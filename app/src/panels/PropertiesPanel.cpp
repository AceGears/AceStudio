#include "PropertiesPanel.h"
#include "DocumentView.h"
#include "QtBridge.h"

#include <QDir>
#include <QFormLayout>
#include <QLabel>

namespace acestudio::ui {

PropertiesPanel::PropertiesPanel(QWidget* parent) : QWidget(parent) {
    m_form = new QFormLayout(this);
    m_form->setContentsMargins(10, 8, 10, 8);
    m_form->setLabelAlignment(Qt::AlignRight);
    rebuild();
}

void PropertiesPanel::setView(DocumentView* view) {
    if (m_view) disconnect(m_view, nullptr, this, nullptr);
    m_view = view;
    if (m_view) {
        connect(m_view, &DocumentView::documentChanged, this, &PropertiesPanel::rebuild);
        connect(m_view, &DocumentView::modifiedChanged, this, &PropertiesPanel::rebuild);
    }
    rebuild();
}

void PropertiesPanel::rebuild() {
    while (m_form->rowCount() > 0) m_form->removeRow(0);
    auto row = [this](const QString& k, const QString& v) {
        auto* value = new QLabel(v);
        value->setTextInteractionFlags(Qt::TextSelectableByMouse);
        value->setWordWrap(true);
        m_form->addRow(k, value);
    };
    if (!m_view) {
        row(tr("Document"), tr("None open"));
        return;
    }
    const Document& d = m_view->document();
    int totalMs = 0;
    for (const auto& f : d.frames()) totalMs += f.durationMs;

    row(tr("Canvas"), QStringLiteral("%1 × %2 px").arg(d.width()).arg(d.height()));
    row(tr("Color mode"), qs(std::string(toString(d.colorMode()))));
    row(tr("Pixel aspect"), QStringLiteral("%1 : %2").arg(d.pixelAspectX()).arg(d.pixelAspectY()));
    row(tr("Color profile"), qs(d.colorProfile()));
    row(tr("Frames"), tr("%1 (%2 ms total)").arg(d.frameCount()).arg(totalMs));
    row(tr("Layers"), QString::number(d.layers().size()));
    row(tr("Tags"), QString::number(d.tags().size()));
    row(tr("File"), m_view->filePath().isEmpty() ? tr("Not saved yet") : QDir::toNativeSeparators(m_view->filePath()));
    row(tr("Status"), m_view->isModified() ? tr("Unsaved changes") : tr("Saved"));
}

} // namespace acestudio::ui
