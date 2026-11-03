#include "Benchmark.h"

#include "AiService.h"
#include "SystemMonitor.h"
#include "../AppContext.h"
#include "../QtBridge.h"
#include "../pages/GeneratorPage.h"

#include "acestudio/core/AnimCodec.h"
#include "acestudio/core/AssetGen.h"
#include "acestudio/core/Effects.h"
#include "acestudio/core/FileUtil.h"
#include "acestudio/core/PngCodec.h"
#include "acestudio/core/SpriteMotion.h"
#include "acestudio/core/Version.h"

#include <QCheckBox>
#include <QDateTime>
#include <QDir>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QStandardPaths>
#include <QTimer>
#include <QVBoxLayout>

#include <thread>

namespace acestudio::ui {

using nlohmann::json;

namespace {

// Fixed cases: the same prompts and seeds every run.
const std::vector<std::pair<std::string, std::string>>& packItems() {
    static const std::vector<std::pair<std::string, std::string>> k = {
        {"pack_potion", "a red health potion in a round glass bottle with a cork"},
        {"pack_key", "an ornate silver key"},
        {"pack_shield", "a round wooden shield with an iron rim"},
        {"pack_pouch", "a leather coin pouch with gold coins spilling out"},
    };
    return k;
}

json statsJson(const SystemStats& s) {
    json j{{"samples", s.samples}, {"cpuAvgPercent", s.cpuAvg}, {"cpuPeakPercent", s.cpuPeak}, {"ramPeakGb", s.ramPeakGb},
           {"uiLagAvgMs", s.uiLagAvgMs}, {"uiLagPeakMs", s.uiLagPeakMs}};
    if (s.gpuKnown) j.update({{"gpuAvgPercent", s.gpuAvg}, {"gpuPeakPercent", s.gpuPeak}, {"vramPeakGb", s.vramPeakGb}});
    return j;
}

void writeText(const QString& path, const std::string& text) {
    (void)writeFileAtomic(toPath(path), std::span(reinterpret_cast<const std::uint8_t*>(text.data()), text.size()));
}

Image onBackground(const Image& img, Rgba8 bg) {
    Image out(img.width(), img.height(), bg);
    out.blit(img, 0, 0);
    return out;
}

} // namespace

BenchmarkRunner::BenchmarkRunner(GeneratorPage& page, QWidget* parent) : QDialog(parent), m_page(page) {
    setWindowTitle(tr("Benchmark"));
    setMinimumWidth(560);
    auto* col = new QVBoxLayout(this);
    auto* intro = new QLabel(tr("Runs fixed cases (a 32×32 character, a 64×64 RPG sprite, a 128×128 monster, idle and walk "
                                "cycles, a dragon wing flap, a campfire and a four-item asset pack) with fixed seeds, and "
                                "measures time, CPU, GPU, RAM, VRAM and UI responsiveness. Each pipeline starts from a fresh "
                                "AI worker. Expect several minutes per pipeline."),
                             this);
    intro->setWordWrap(true);
    col->addWidget(intro);
    auto* form = new QFormLayout;
    auto* folderRow = new QHBoxLayout;
    m_folder = new QLineEdit(this);
    m_folder->setAccessibleName(tr("Report folder"));
    const QString downloads = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
    m_folder->setText(QDir(downloads.isEmpty() ? QDir::homePath() : downloads)
                          .filePath(QStringLiteral("AceStudio Benchmark %1").arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HHmm")))));
    auto* browse = new QPushButton(tr("Browse…"), this);
    connect(browse, &QPushButton::clicked, this, [this] {
        const QString d = QFileDialog::getExistingDirectory(this, tr("Report Folder"), QDir::homePath());
        if (!d.isEmpty()) m_folder->setText(d);
    });
    folderRow->addWidget(m_folder, 1);
    folderRow->addWidget(browse);
    form->addRow(tr("Report &folder:"), folderRow);
    m_original = new QCheckBox(tr("Original pipeline (optimisations off)"), this);
    m_optimised = new QCheckBox(tr("Optimised pipeline"), this);
    m_original->setChecked(true);
    m_optimised->setChecked(true);
    form->addRow(tr("Run:"), m_original);
    form->addRow(QString(), m_optimised);
    col->addLayout(form);
    m_progress = new QProgressBar(this);
    m_progress->setAccessibleName(tr("Benchmark progress"));
    col->addWidget(m_progress);
    m_log = new QPlainTextEdit(this);
    m_log->setReadOnly(true);
    m_log->setAccessibleName(tr("Benchmark log"));
    m_log->setMinimumHeight(220);
    col->addWidget(m_log, 1);
    auto* buttons = new QHBoxLayout;
    buttons->addStretch();
    m_start = new QPushButton(tr("Start"), this);
    m_cancel = new QPushButton(tr("Close"), this);
    buttons->addWidget(m_start);
    buttons->addWidget(m_cancel);
    col->addLayout(buttons);
    connect(m_start, &QPushButton::clicked, this, &BenchmarkRunner::start);
    connect(m_cancel, &QPushButton::clicked, this, [this] {
        if (m_running) stop(tr("Stopped."));
        else reject();
    });
}

BenchmarkRunner::~BenchmarkRunner() {
    if (m_running) {
        if (!m_job.empty()) m_page.m_ai.cancel(m_job);
        if (systemMonitor().recording()) (void)systemMonitor().stopRecording();
    }
}

void BenchmarkRunner::log(const QString& line) {
    m_log->appendPlainText(QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss  ")) + line);
}

QString BenchmarkRunner::outPath(const std::string& name) const { return QDir(m_dir).filePath(QString::fromStdString(name)); }

void BenchmarkRunner::start() {
    if (m_running) return;
    if (!m_original->isChecked() && !m_optimised->isChecked()) return;
    m_dir = m_folder->text().trimmed();
    if (!QDir().mkpath(m_dir)) return log(tr("Could not create %1").arg(m_dir));
    const std::vector<Gen> gens = {
        {"char32", "a knight in blue plate armour with a round shield, full body", 32, 4101, false},
        {"rpg64", "a hooded rogue with twin daggers and a dark green leather cloak, full body", 64, 4102, false},
        {"monster128", "a horned swamp troll with mossy green skin and a heavy wooden club, full body", 128, 4103, false},
        {"dragon128", "a small red dragon seen from the side with large bat wings raised above its back, full body", 128, 4104, false},
        {"campfire64", "a campfire: a ring of grey stones, crossed logs and a tall orange and yellow flame", 64, 4105, false},
    };
    m_steps.clear();
    for (const char* mode : {"original", "optimised"}) {
        if ((std::string(mode) == "original" && !m_original->isChecked()) || (std::string(mode) == "optimised" && !m_optimised->isChecked()))
            continue;
        m_steps.push_back({mode, "restart", {}});
        m_steps.push_back({mode, "gen", gens[0]});
        m_steps.push_back({mode, "gen", gens[1]});
        m_steps.push_back({mode, "gen", gens[2]});
        m_steps.push_back({mode, "idle", gens[1]});
        m_steps.push_back({mode, "walk", gens[1]});
        m_steps.push_back({mode, "gen", gens[3]});
        m_steps.push_back({mode, "flap", gens[3]});
        m_steps.push_back({mode, "gen", gens[4]});
        m_steps.push_back({mode, "fire", gens[4]});
        for (std::size_t i = 0; i < packItems().size(); ++i)
            m_steps.push_back({mode, "packItem", {packItems()[i].first, packItems()[i].second, 32, static_cast<long long>(4201 + i), false}});
        m_steps.push_back({mode, "packDone", {}});
    }
    AiService& ai = m_page.m_ai;
    const auto& caps = ai.capabilities();
    json machine{{"cpuThreads", std::thread::hardware_concurrency()}, {"profile", ai.profileName().toStdString()},
                 {"app", kVersionString}, {"devices", caps.value("devices", json::array())}};
    m_report = json{{"format", "acestudio.benchmark"}, {"version", 1},
                    {"started", QDateTime::currentDateTime().toString(Qt::ISODate).toStdString()},
                    {"machine", machine}, {"cases", json::array()}};
    m_images.clear();
    m_next = 0;
    m_running = true;
    m_start->setEnabled(false);
    m_cancel->setText(tr("Stop"));
    m_progress->setRange(0, static_cast<int>(m_steps.size()));
    m_progress->setValue(0);
    m_finishedConn = connect(&ai, &AiService::jobFinished, this, &BenchmarkRunner::onJobFinished);
    log(tr("Writing to %1").arg(QDir::toNativeSeparators(m_dir)));
    next();
}

void BenchmarkRunner::stop(const QString& why) {
    if (!m_job.empty()) m_page.m_ai.cancel(m_job);
    m_job.clear();
    m_running = false;
    disconnect(m_finishedConn);
    disconnect(m_stateConn);
    if (systemMonitor().recording()) (void)systemMonitor().stopRecording();
    m_start->setEnabled(true);
    m_cancel->setText(tr("Close"));
    log(why);
}

void BenchmarkRunner::next() {
    if (!m_running) return;
    m_progress->setValue(static_cast<int>(m_next));
    if (m_next >= m_steps.size()) return finish();
    const Step s = m_steps[m_next];
    AiService& ai = m_page.m_ai;
    if (s.kind == "restart") {
        log(tr("— %1 pipeline: restarting the AI worker (cold start, as after opening AceStudio)").arg(qs(s.mode)));
        m_stepClock.start();
        disconnect(m_stateConn);
        m_stateConn = connect(&ai, &AiService::stateChanged, this, [this] {
            if (m_page.m_ai.state() != ai::WorkerState::Ready) return;
            disconnect(m_stateConn);
            json c{{"mode", m_steps[m_next].mode}, {"case", "workerStart"}, {"appSeconds", static_cast<double>(m_stepClock.elapsed()) / 1000.0}};
            m_report["cases"].push_back(c);
            log(tr("AI worker ready in %1 s").arg(static_cast<double>(m_stepClock.elapsed()) / 1000.0, 0, 'f', 1));
            ++m_next;
            QTimer::singleShot(1500, this, &BenchmarkRunner::next);   // let the GPU probe settle
        });
        ai.restartWorker();
        return;
    }
    if (s.kind == "gen" || s.kind == "packItem") {
        if (s.kind == "packItem" && s.gen.id == packItems().front().first) m_packClock.start();
        GeneratorPage::JobOptions o;
        o.description = s.gen.description;
        o.type = s.kind == "packItem" ? gen::AssetType::Item : s.gen.id == "campfire64" ? gen::AssetType::Prop
                                                              : s.gen.id == "monster128" || s.gen.id == "dragon128" ? gen::AssetType::Monster
                                                                                                                  : gen::AssetType::Character;
        o.style = gen::ArtStyle::DetailedPixel;
        o.preset = gen::QualityPreset::Balanced;
        o.transparent = true;
        o.colors = 32;
        o.seed = s.gen.seed;
        o.size = std::pair{s.gen.size, s.gen.size};
        o.count = 1;
        o.optimize = s.mode == "optimised";
        systemMonitor().startRecording();
        m_stepClock.start();
        m_job = m_page.submitJob({}, o);
        if (m_job.empty()) return stop(tr("Could not start %1 (is FLUX.2 klein installed?)").arg(qs(s.gen.id)));
        log(tr("%1 · %2: generating %3×%3").arg(qs(s.mode), qs(s.gen.id)).arg(s.gen.size));
        return;
    }
    if (s.kind == "packDone") {
        json c{{"mode", s.mode}, {"case", "assetPack4"}, {"appSeconds", static_cast<double>(m_packClock.elapsed()) / 1000.0}};
        m_report["cases"].push_back(c);
        log(tr("%1 · asset pack (4 items): %2 s").arg(qs(s.mode)).arg(static_cast<double>(m_packClock.elapsed()) / 1000.0, 0, 'f', 1));
        ++m_next;
        QTimer::singleShot(0, this, &BenchmarkRunner::next);
        return;
    }
    runAnimation(s);
}

void BenchmarkRunner::onJobFinished(const QString& qid) {
    if (!m_running || qid.toStdString() != m_job) return;
    const Step s = m_steps[m_next];
    const double appSeconds = static_cast<double>(m_stepClock.elapsed()) / 1000.0;
    const SystemStats stats = systemMonitor().stopRecording();
    const ai::Job* j = m_page.m_ai.job(m_job);
    m_job.clear();
    json c{{"mode", s.mode}, {"case", s.gen.id}, {"size", s.gen.size}, {"seed", s.gen.seed}, {"appSeconds", appSeconds},
           {"system", statsJson(stats)}};
    if (!j || j->state != ai::JobState::Succeeded) {
        c["error"] = j ? j->error : std::string("job lost");
        m_report["cases"].push_back(c);
        return stop(tr("%1 failed: %2").arg(qs(s.gen.id), qs(c["error"].get<std::string>()).section(QLatin1Char('\n'), 0, 0)));
    }
    c["worker"] = j->info;
    c["generationWidth"] = j->params.value("width", 0);
    c["generationHeight"] = j->params.value("height", 0);
    if (auto o = j->outputs.find("image0"); o != j->outputs.end()) {
        if (auto img = readPng(o->second)) {
            gen::CheckOptions chk;
            chk.pixelArt = true;
            chk.expectTransparent = true;
            chk.expectWidth = chk.expectHeight = s.gen.size;
            chk.maxColors = 32;
            const gen::QualityReport q = gen::checkAsset(img.value(), chk);
            c["qualityScore"] = q.score();
            json issues = json::array();
            for (const auto& i : q.issues) issues.push_back(i.code);
            c["qualityIssues"] = issues;
            const std::string file = s.mode + "_" + s.gen.id + ".png";
            (void)writePng(img.value(), toPath(outPath(file)));
            c["image"] = file;
            m_images[s.mode + "/" + s.gen.id] = std::move(img).value();
        }
    }
    m_report["cases"].push_back(c);
    const json& m = j->info.value("metrics", json::object());
    log(tr("%1 · %2: %3 s (worker %4 s, load %5 s, %6 s/step, peak VRAM %7 GB), score %8")
            .arg(qs(s.mode), qs(s.gen.id))
            .arg(appSeconds, 0, 'f', 1)
            .arg(m.value("wallSeconds", 0.0), 0, 'f', 1)
            .arg(j->info.value("loadSeconds", 0.0), 0, 'f', 1)
            .arg(j->info.value("secondsPerStep", 0.0), 0, 'f', 2)
            .arg(m.value("peakDeviceVramGb", j->info.value("peakVramGb", 0.0)), 0, 'f', 2)
            .arg(c.value("qualityScore", 0)));
    ++m_next;
    QTimer::singleShot(0, this, &BenchmarkRunner::next);
}

void BenchmarkRunner::runAnimation(const Step& s) {
    const auto it = m_images.find(s.mode + "/" + s.gen.id);
    if (it == m_images.end()) {
        log(tr("%1 · %2: no sprite to animate, skipped").arg(qs(s.mode), qs(s.kind)));
        ++m_next;
        return void(QTimer::singleShot(0, this, &BenchmarkRunner::next));
    }
    const Image& sprite = it->second;
    std::vector<Image> frames;
    std::vector<int> durations;
    json c{{"mode", s.mode}, {"case", s.kind}, {"source", s.gen.id}};
    QElapsedTimer t;
    t.start();
    if (s.kind == "fire" && s.mode == "optimised") {
        // Procedural fire (no AI): the flame region flickers in its own colours.
        fx::EffectOptions o;
        o.effect = fx::Effect::Fire;
        o.frames = 8;
        const fx::EffectClip clip = fx::animateEffect(sprite, o);
        frames = clip.frames;
        durations = clip.durations;
        c["method"] = "procedural fire";
    } else {
        // The original app had no fire effect: Animate Sprite (an object rocks) was the way to animate it.
        motion::Options o;
        o.action = s.kind == "walk" ? motion::Action::Walk : s.kind == "flap" ? motion::Action::Flap : motion::Action::Idle;
        o.frames = 8;
        auto clip = motion::animate(sprite, o);
        if (clip) {
            frames = clip.value().frames;
            durations = clip.value().durations;
            const motion::ClipCheck chk = motion::checkClip(clip.value());
            c["body"] = motion::toString(clip.value().body);
            c["check"] = {{"maxAreaChange", chk.maxAreaChange}, {"minNeighbourIoU", chk.minNeighbourIoU}, {"loopIoU", chk.loopIoU}, {"flicker", chk.flicker}, {"floatingFrames", chk.floatingFrames},
                          {"issues", chk.issues}};
        } else {
            c["error"] = clip.error().message;
        }
        c["method"] = s.kind == "fire" ? "Animate Sprite (idle)" : "Animate Sprite";
    }
    c["animateSeconds"] = static_cast<double>(t.nsecsElapsed()) / 1e9;
    c["frames"] = frames.size();
    if (!frames.empty()) {
        const int fw = frames[0].width(), fh = frames[0].height();
        Image strip(fw * static_cast<int>(frames.size()), fh);
        std::vector<AnimFrame> anim;
        for (std::size_t i = 0; i < frames.size(); ++i) {
            strip.blit(frames[i], static_cast<int>(i) * fw, 0);
            anim.push_back({onBackground(frames[i], Rgba8{34, 38, 48, 255}), durations[i]});
        }
        const std::string stem = s.mode + "_" + s.kind;
        (void)writePng(strip, toPath(outPath(stem + "_strip.png")));
        if (auto gif = encodeGif(anim, 0)) (void)writeFileAtomic(toPath(outPath(stem + ".gif")), gif.value());
        c["strip"] = stem + "_strip.png";
        c["gif"] = stem + ".gif";
    }
    m_report["cases"].push_back(c);
    log(tr("%1 · %2: %3 frames in %4 ms").arg(qs(s.mode), qs(s.kind)).arg(frames.size()).arg(c["animateSeconds"].get<double>() * 1000, 0, 'f', 1));
    ++m_next;
    QTimer::singleShot(0, this, &BenchmarkRunner::next);
}

void BenchmarkRunner::finish() {
    m_running = false;
    disconnect(m_finishedConn);
    m_report["finished"] = QDateTime::currentDateTime().toString(Qt::ISODate).toStdString();
    writeText(outPath("report.json"), m_report.dump(2));
    // A readable summary: one row per case, original next to optimised.
    auto find = [&](const std::string& mode, const std::string& name) -> const json* {
        for (const json& c : m_report["cases"])
            if (c.value("mode", "") == mode && c.value("case", "") == name) return &c;
        return nullptr;
    };
    auto num = [](const json* c, const char* key, int prec = 1) -> std::string {
        if (!c || !c->contains(key) || !(*c)[key].is_number()) return "–";
        return QString::number((*c)[key].get<double>(), 'f', prec).toStdString();
    };
    auto worker = [](const json* c, const char* key, int prec = 1) -> std::string {
        if (!c || !c->contains("worker")) return "–";
        const json& w = (*c)["worker"];
        const json& m = w.value("metrics", json::object());
        const json* v = m.contains(key) ? &m[key] : w.contains(key) ? &w[key] : nullptr;
        if (!v && m.contains("stages") && m["stages"].contains(key)) v = &m["stages"][key];
        if (!v || !v->is_number()) return "–";
        return QString::number(v->get<double>(), 'f', prec).toStdString();
    };
    std::string md = "# AceStudio benchmark\n\nApp " + std::string(kVersionString) + ", profile " +
                     m_report["machine"].value("profile", std::string()) + ". Times in seconds; VRAM and RAM in GB.\n\n";
    md += "| Case | Pipeline | Total | Load | Diffusion | Cutout | Pixel | Peak VRAM | Worker RAM | CPU avg % | GPU avg % | UI lag peak ms | Score |\n";
    md += "|---|---|---|---|---|---|---|---|---|---|---|---|---|\n";
    for (const char* name : {"workerStart", "char32", "rpg64", "monster128", "dragon128", "campfire64", "pack_potion", "pack_key",
                             "pack_shield", "pack_pouch", "assetPack4"})
        for (const char* mode : {"original", "optimised"}) {
            const json* c = find(mode, name);
            if (!c) continue;
            const json sys = c->value("system", json::object());
            md += std::string("| ") + name + " | " + mode + " | " + num(c, "appSeconds") + " | " + worker(c, "load") + " | " +
                  worker(c, "diffusion") + " | " + worker(c, "cutout") + " | " + worker(c, "pixel", 2) + " | " +
                  worker(c, "peakDeviceVramGb", 2) + " | " + worker(c, "peakRssGb", 2) + " | " + num(&sys, "cpuAvgPercent", 0) + " | " +
                  num(&sys, "gpuAvgPercent", 0) + " | " + num(&sys, "uiLagPeakMs", 0) + " | " +
                  (c->contains("qualityScore") ? std::to_string((*c)["qualityScore"].get<int>()) : "–") + " |\n";
        }
    md += "\n## Animation\n\n| Case | Pipeline | Method | Frames | Time ms | Issues | Min neighbour IoU | Loop IoU | Flicker | Floating frames |\n|---|---|---|---|---|---|---|---|---|---|\n";
    for (const char* name : {"idle", "walk", "flap", "fire"})
        for (const char* mode : {"original", "optimised"}) {
            const json* c = find(mode, name);
            if (!c) continue;
            const json chk = c->value("check", json::object());
            md += std::string("| ") + name + " | " + mode + " | " + c->value("method", std::string()) + " | " +
                  std::to_string(c->value("frames", 0)) + " | " + QString::number(c->value("animateSeconds", 0.0) * 1000, 'f', 1).toStdString() +
                  " | " + (chk.contains("issues") ? std::to_string(chk["issues"].size()) : "–") + " | " + num(&chk, "minNeighbourIoU", 2) +
                  " | " + num(&chk, "loopIoU", 2) + " | " + num(&chk, "flicker", 3) + " | " + num(&chk, "floatingFrames", 0) + " |\n";
        }
    writeText(outPath("report.md"), md);
    m_start->setEnabled(true);
    m_cancel->setText(tr("Close"));
    m_progress->setValue(m_progress->maximum());
    log(tr("Done. Report: %1").arg(QDir::toNativeSeparators(outPath("report.md"))));
}

} // namespace acestudio::ui
