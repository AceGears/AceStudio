#include "SettingsPage.h"

#include "../AppContext.h"
#include "../Theme.h"
#include "../ui/Icons.h"

#include "acestudio/core/Color.h"

#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QSlider>
#include <QSpinBox>
#include <QStackedWidget>
#include <QVBoxLayout>

namespace acestudio::ui {

namespace {

struct FormPage {
    QWidget* page;
    QFormLayout* form;
};

FormPage makePage(const QString& title, const QString& subtitle, QWidget* parent) {
    auto* scroll = new QScrollArea(parent);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto* inner = new QWidget(scroll);
    scroll->setWidget(inner);
    auto* col = new QVBoxLayout(inner);
    col->setContentsMargins(28, 22, 28, 22);
    auto* h = new QLabel(title, inner);
    h->setProperty("role", "h1");
    col->addWidget(h);
    if (!subtitle.isEmpty()) {
        auto* s = new QLabel(subtitle, inner);
        s->setProperty("role", "muted");
        s->setWordWrap(true);
        col->addWidget(s);
    }
    col->addSpacing(10);
    auto* form = new QFormLayout;
    form->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
    form->setHorizontalSpacing(18);
    form->setVerticalSpacing(10);
    form->setFieldGrowthPolicy(QFormLayout::FieldsStayAtSizeHint);
    col->addLayout(form);
    col->addStretch();
    return {scroll, form};
}

QLabel* hint(const QString& text, QWidget* parent) {
    auto* l = new QLabel(text, parent);
    l->setProperty("role", "muted");
    l->setWordWrap(true);
    l->setMaximumWidth(460);
    return l;
}

} // namespace

SettingsPage::SettingsPage(AppContext& ctx, QWidget* parent) : QWidget(parent), m_ctx(ctx) {
    auto* lay = new QHBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(0);
    m_categories = new QListWidget(this);
    m_categories->setAccessibleName(tr("Settings categories"));
    m_categories->setFixedWidth(200);
    m_categories->setFrameShape(QFrame::NoFrame);
    m_categories->setStyleSheet(QStringLiteral("QListWidget { border: none; border-right: 1px solid palette(mid); border-radius: 0; padding: 12px 8px; }"
                                               "QListWidget::item { padding: 8px 12px; border-radius: 7px; }"));
    m_pages = new QStackedWidget(this);
    lay->addWidget(m_categories);
    lay->addWidget(m_pages, 1);
    connect(m_categories, &QListWidget::currentRowChanged, m_pages, &QStackedWidget::setCurrentIndex);

    addCategory(tr("Appearance"), appearance());
    addCategory(tr("Canvas and editing"), editor());
    addCategory(tr("Generation"), generation());
    addCategory(tr("Animation"), animation());
    addCategory(tr("Integrations"), integrations());
    addCategory(tr("Advanced"), advanced());
    m_categories->setCurrentRow(0);
}

void SettingsPage::addCategory(const QString& name, QWidget* page) {
    m_categories->addItem(name);
    m_pages->addWidget(page);
}

QWidget* SettingsPage::appearance() {
    auto [page, form] = makePage(tr("Appearance"), tr("Theme, colour and motion. Changes apply at once."), this);
    Settings& s = m_ctx.settings();

    auto* theme = new QComboBox(page);
    theme->addItem(tr("Dark (charcoal)"), QStringLiteral("dark"));
    theme->addItem(tr("Light"), QStringLiteral("light"));
    theme->addItem(tr("High contrast"), QStringLiteral("highContrast"));
    theme->setCurrentIndex(std::max(0, theme->findData(QString::fromStdString(s.get<std::string>("general.theme")))));
    connect(theme, &QComboBox::currentIndexChanged, this, [this, theme] {
        m_ctx.setSetting("general.theme", theme->currentData().toString().toStdString());
    });
    form->addRow(tr("&Theme:"), theme);

    auto* accentRow = new QHBoxLayout;
    auto* accent = new QPushButton(page);
    accent->setAccessibleName(tr("Accent colour"));
    auto paintSwatch = [accent](const QColor& c) {
        accent->setText(c.name().toUpper());
        accent->setStyleSheet(QStringLiteral("QPushButton { border-left: 18px solid %1; }").arg(c.name()));
    };
    const auto cur = Rgba8::fromHex(s.get<std::string>("general.accentColor"));
    paintSwatch(cur ? QColor(cur->r, cur->g, cur->b) : QColor(kBrandAccent));
    connect(accent, &QPushButton::clicked, this, [this, accent, paintSwatch] {
        const QColor c = QColorDialog::getColor(QColor(accent->text()), this, tr("Accent Colour"));
        if (!c.isValid()) return;
        paintSwatch(c);
        m_ctx.setSetting("general.accentColor", c.name().toUpper().toStdString());
    });
    auto* brand = new QPushButton(tr("AceStudio Blue"), page);
    connect(brand, &QPushButton::clicked, this, [this, paintSwatch] {
        paintSwatch(QColor(kBrandAccent));
        m_ctx.setSetting("general.accentColor", std::string(kBrandAccent));
    });
    accentRow->addWidget(accent);
    accentRow->addWidget(brand);
    form->addRow(tr("Accent colour:"), accentRow);

    auto* text = new QComboBox(page);
    for (int p : {100, 115, 125, 150, 175, 200}) text->addItem(QStringLiteral("%1%").arg(p), p);
    text->setCurrentIndex(std::max(0, text->findData(s.get<int>("general.textScale"))));
    connect(text, &QComboBox::currentIndexChanged, this, [this, text] { m_ctx.setSetting("general.textScale", text->currentData().toInt()); });
    form->addRow(tr("Te&xt size:"), text);

    auto* reduced = new QCheckBox(tr("Reduce motion (no transitions or animated indicators)"), page);
    reduced->setChecked(s.get<bool>("ui.reducedMotion"));
    connect(reduced, &QCheckBox::toggled, this, [this](bool on) { m_ctx.setSetting("ui.reducedMotion", on); });
    form->addRow(tr("Motion:"), reduced);

    auto* speed = new QComboBox(page);
    for (auto [label, pct] : {std::pair{tr("Quick"), 60}, std::pair{tr("Normal"), 100}, std::pair{tr("Relaxed"), 160}})
        speed->addItem(label, pct);
    speed->setCurrentIndex(std::max(0, speed->findData(s.get<int>("ui.animationSpeed"))));
    connect(speed, &QComboBox::currentIndexChanged, this, [this, speed] { m_ctx.setSetting("ui.animationSpeed", speed->currentData().toInt()); });
    form->addRow(tr("Animation &speed:"), speed);

    auto* splash = new QCheckBox(tr("Show the AceStudio splash screen at start"), page);
    splash->setChecked(s.get<bool>("ui.showSplash"));
    connect(splash, &QCheckBox::toggled, this, [this](bool on) { m_ctx.setSetting("ui.showSplash", on); });
    form->addRow(tr("Start:"), splash);

    auto* startup = new QComboBox(page);
    startup->addItem(tr("Home"), QStringLiteral("welcome"));
    startup->addItem(tr("The last project"), QStringLiteral("lastProject"));
    startup->setCurrentIndex(std::max(0, startup->findData(QString::fromStdString(s.get<std::string>("general.startup")))));
    connect(startup, &QComboBox::currentIndexChanged, this, [this, startup] {
        m_ctx.setSetting("general.startup", startup->currentData().toString().toStdString());
    });
    form->addRow(tr("Open at s&tart:"), startup);
    return page;
}

QWidget* SettingsPage::editor() {
    auto [page, form] = makePage(tr("Canvas and editing"), QString(), this);
    Settings& s = m_ctx.settings();

    auto* grid = new QCheckBox(tr("Show the pixel grid when zoomed in"), page);
    grid->setChecked(s.get<bool>("canvas.grid.visible"));
    connect(grid, &QCheckBox::toggled, this, [this](bool on) { m_ctx.setSetting("canvas.grid.visible", on); });
    form->addRow(tr("Grid:"), grid);

    auto* minZoom = new QSpinBox(page);
    minZoom->setRange(2, 64);
    minZoom->setSuffix(QStringLiteral("×"));
    minZoom->setValue(s.get<int>("canvas.grid.minZoom"));
    connect(minZoom, &QSpinBox::valueChanged, this, [this](int v) { m_ctx.setSetting("canvas.grid.minZoom", v); });
    form->addRow(tr("Grid from &zoom:"), minZoom);

    auto* checker = new QSpinBox(page);
    checker->setRange(2, 64);
    checker->setSuffix(tr(" px"));
    checker->setValue(s.get<int>("canvas.checker.size"));
    connect(checker, &QSpinBox::valueChanged, this, [this](int v) { m_ctx.setSetting("canvas.checker.size", v); });
    form->addRow(tr("Transparency &checker:"), checker);

    auto* zoomStep = new QDoubleSpinBox(page);
    zoomStep->setRange(1.05, 2.0);
    zoomStep->setSingleStep(0.05);
    zoomStep->setValue(s.get<double>("canvas.zoomStep"));
    connect(zoomStep, &QDoubleSpinBox::valueChanged, this, [this](double v) { m_ctx.setSetting("canvas.zoomStep", v); });
    form->addRow(tr("Zoom &step per wheel notch:"), zoomStep);

    auto* autosave = new QSpinBox(page);
    autosave->setRange(0, 3600);
    autosave->setSingleStep(30);
    autosave->setSuffix(tr(" s"));
    autosave->setSpecialValueText(tr("Off"));
    autosave->setValue(s.get<int>("general.autosaveIntervalSec"));
    connect(autosave, &QSpinBox::valueChanged, this, [this](int v) { m_ctx.setSetting("general.autosaveIntervalSec", v); });
    form->addRow(tr("&Autosave every:"), autosave);
    form->addRow(QString(), hint(tr("Autosave writes recovery copies in the background; your project file changes only when you save."), page));

    auto* shortcuts = new QPushButton(tr("Keyboard Shortcuts…"), page);
    connect(shortcuts, &QPushButton::clicked, this, &SettingsPage::openShortcutsRequested);
    form->addRow(tr("Keyboard:"), shortcuts);
    return page;
}

QWidget* SettingsPage::generation() {
    auto [page, form] = makePage(tr("Generation"), tr("Local AI runs on this PC. Nothing is uploaded."), this);
    Settings& s = m_ctx.settings();

    auto* setup = new QPushButton(tr("Set Up AI Runtime…"), page);
    connect(setup, &QPushButton::clicked, this, &SettingsPage::openAiSetupRequested);
    auto* models = new QPushButton(tr("Models…"), page);
    connect(models, &QPushButton::clicked, this, &SettingsPage::openModelsRequested);
    auto* row = new QHBoxLayout;
    row->addWidget(setup);
    row->addWidget(models);
    row->addStretch();
    form->addRow(tr("Runtime:"), row);

    auto* autoStart = new QCheckBox(tr("Start the AI worker when AceStudio opens"), page);
    autoStart->setChecked(s.get<bool>("ai.autoStart"));
    connect(autoStart, &QCheckBox::toggled, this, [this](bool on) { m_ctx.setSetting("ai.autoStart", on); });
    form->addRow(tr("Worker:"), autoStart);

    auto* lora = new QCheckBox(tr("Use the pixel-art style model with SDXL when installed"), page);
    lora->setChecked(s.get<bool>("ai.generate.lora"));
    connect(lora, &QCheckBox::toggled, this, [this](bool on) { m_ctx.setSetting("ai.generate.lora", on); });
    form->addRow(tr("Pixel art:"), lora);

    auto* strength = new QSpinBox(page);
    strength->setRange(0, 200);
    strength->setSuffix(QStringLiteral("%"));
    strength->setValue(s.get<int>("ai.generate.loraStrength"));
    connect(strength, &QSpinBox::valueChanged, this, [this](int v) { m_ctx.setSetting("ai.generate.loraStrength", v); });
    form->addRow(tr("Style &strength:"), strength);

    auto* crop = new QCheckBox(tr("Frame the subject before reducing to pixel size"), page);
    crop->setChecked(s.get<bool>("ai.generate.cropSubject"));
    connect(crop, &QCheckBox::toggled, this, [this](bool on) { m_ctx.setSetting("ai.generate.cropSubject", on); });
    form->addRow(tr("Framing:"), crop);

    auto* modelDir = new QLineEdit(QString::fromStdString(s.get<std::string>("ai.modelDirectory")), page);
    modelDir->setPlaceholderText(tr("Default (AceStudio's AI folder)"));
    modelDir->setAccessibleName(tr("Model folder"));
    modelDir->setMinimumWidth(320);
    auto* browse = new QPushButton(tr("Browse…"), page);
    connect(browse, &QPushButton::clicked, this, [this, modelDir] {
        const QString d = QFileDialog::getExistingDirectory(this, tr("Model Folder"), modelDir->text());
        if (!d.isEmpty()) modelDir->setText(QDir::toNativeSeparators(d));
        m_ctx.setSetting("ai.modelDirectory", modelDir->text().toStdString());
    });
    connect(modelDir, &QLineEdit::editingFinished, this, [this, modelDir] { m_ctx.setSetting("ai.modelDirectory", modelDir->text().toStdString()); });
    auto* dirRow = new QHBoxLayout;
    dirRow->addWidget(modelDir, 1);
    dirRow->addWidget(browse);
    form->addRow(tr("Model folder:"), dirRow);
    form->addRow(QString(), hint(tr("Models are large (up to ~16 GB each). Changing the folder takes effect when the AI worker restarts."), page));
    return page;
}

QWidget* SettingsPage::animation() {
    auto [page, form] = makePage(tr("Animation"), QString(), this);
    Settings& s = m_ctx.settings();

    auto* fps = new QSpinBox(page);
    fps->setRange(1, 60);
    fps->setSuffix(tr(" fps"));
    fps->setValue(s.get<int>("animation.defaultFps"));
    connect(fps, &QSpinBox::valueChanged, this, [this](int v) { m_ctx.setSetting("animation.defaultFps", v); });
    form->addRow(tr("New projects &play at:"), fps);

    auto* before = new QSpinBox(page);
    before->setRange(0, 8);
    before->setValue(s.get<int>("animation.onionSkin.before"));
    connect(before, &QSpinBox::valueChanged, this, [this](int v) { m_ctx.setSetting("animation.onionSkin.before", v); });
    form->addRow(tr("Onion skin frames &before:"), before);
    auto* after = new QSpinBox(page);
    after->setRange(0, 8);
    after->setValue(s.get<int>("animation.onionSkin.after"));
    connect(after, &QSpinBox::valueChanged, this, [this](int v) { m_ctx.setSetting("animation.onionSkin.after", v); });
    form->addRow(tr("Onion skin frames a&fter:"), after);
    auto* opacity = new QSpinBox(page);
    opacity->setRange(5, 100);
    opacity->setSuffix(QStringLiteral("%"));
    opacity->setValue(static_cast<int>(std::lround(s.get<double>("animation.onionSkin.opacity") * 100)));
    connect(opacity, &QSpinBox::valueChanged, this, [this](int v) { m_ctx.setSetting("animation.onionSkin.opacity", v / 100.0); });
    form->addRow(tr("Onion skin &opacity:"), opacity);
    auto* tint = new QCheckBox(tr("Tint earlier frames red and later frames blue"), page);
    tint->setChecked(s.get<bool>("animation.onionSkin.tint"));
    connect(tint, &QCheckBox::toggled, this, [this](bool on) { m_ctx.setSetting("animation.onionSkin.tint", on); });
    form->addRow(tr("Onion skin:"), tint);
    auto* wrap = new QCheckBox(tr("Wrap around at the ends (for loops)"), page);
    wrap->setChecked(s.get<bool>("animation.onionSkin.wrap"));
    connect(wrap, &QCheckBox::toggled, this, [this](bool on) { m_ctx.setSetting("animation.onionSkin.wrap", on); });
    form->addRow(QString(), wrap);
    return page;
}

QWidget* SettingsPage::integrations() {
    auto [page, form] = makePage(tr("Integrations"), tr("AI coding assistants use AceStudio through its MCP server."), this);
    auto* bridge = new QCheckBox(tr("Allow AI assistants to read and edit the open document"), page);
    bridge->setChecked(m_ctx.settings().get<bool>("mcp.bridgeEnabled"));
    connect(bridge, &QCheckBox::toggled, this, [this](bool on) { m_ctx.setSetting("mcp.bridgeEnabled", on); });
    form->addRow(tr("Open document:"), bridge);
    form->addRow(QString(), hint(tr("Each change an assistant makes is one Edit › Undo step. Connect assistants in the Integrations workspace."), page));
    return page;
}

QWidget* SettingsPage::advanced() {
    auto [page, form] = makePage(tr("Advanced"), QString(), this);
    Settings& s = m_ctx.settings();
    auto* level = new QComboBox(page);
    for (const char* l : {"debug", "info", "warn", "error"}) level->addItem(QString::fromLatin1(l), QString::fromLatin1(l));
    level->setCurrentIndex(std::max(0, level->findData(QString::fromStdString(s.get<std::string>("advanced.logLevel")))));
    connect(level, &QComboBox::currentIndexChanged, this, [this, level] {
        m_ctx.setSetting("advanced.logLevel", level->currentData().toString().toStdString());
    });
    form->addRow(tr("&Log detail:"), level);
    form->addRow(QString(), hint(tr("Takes effect at the next start."), page));

    auto* recent = new QSpinBox(page);
    recent->setRange(1, 50);
    recent->setValue(s.get<int>("general.maxRecentProjects"));
    connect(recent, &QSpinBox::valueChanged, this, [this](int v) { m_ctx.setSetting("general.maxRecentProjects", v); });
    form->addRow(tr("&Recent projects kept:"), recent);

    auto* logs = new QPushButton(tr("Open Logs Folder"), page);
    connect(logs, &QPushButton::clicked, this, &SettingsPage::openLogsRequested);
    auto* layout = new QPushButton(tr("Reset Window Layout"), page);
    connect(layout, &QPushButton::clicked, this, &SettingsPage::resetLayoutRequested);
    auto* row = new QHBoxLayout;
    row->addWidget(logs);
    row->addWidget(layout);
    row->addStretch();
    form->addRow(tr("Diagnostics:"), row);

    auto* reset = new QPushButton(tr("Reset All Preferences…"), page);
    connect(reset, &QPushButton::clicked, this, [this] {
        if (QMessageBox::question(this, tr("Reset Preferences"),
                                  tr("Reset every preference to its default? Your projects, recent files, characters and models are kept. "
                                     "Some changes apply at the next start.")) != QMessageBox::Yes)
            return;
        Settings& st = m_ctx.settings();
        for (const std::string& k : st.keys())
            if (k != "general.recentProjects" && k.rfind("window.", 0) != 0 && k.rfind("library.", 0) != 0 && k != "keyboard.shortcuts")
                st.reset(k);
        m_ctx.saveSettings();
        Q_EMIT m_ctx.settingChanged(QStringLiteral("*"));
    });
    form->addRow(tr("Preferences:"), reset);
    return page;
}

} // namespace acestudio::ui
