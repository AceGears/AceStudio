#include "AiService.h"

#include "AppContext.h"
#include "QtBridge.h"

#include "acestudio/core/FileUtil.h"
#include "acestudio/core/Log.h"

#include <QCoreApplication>
#include <QMetaObject>
#include <QTimer>

#include <set>

namespace acestudio::ui {

using namespace acestudio::ai;
namespace fs = std::filesystem;

AiService::AiService(AppContext& ctx, QObject* parent) : QObject(parent), m_ctx(ctx) {
    auto local = userLocalDataDir();
    if (!local) {
        m_envError = qs(local.error().message);
    } else {
        auto env = resolveAiEnvironment(local.value(), ctx.settings().get<std::string>("ai.modelDirectory"),
                                        toPath(QCoreApplication::applicationDirPath()));
        if (!env) m_envError = qs(env.error().message);
        else m_env = env.value();
    }
    if (m_env) {
        auto cat = ModelCatalog::load(m_env->catalogFile);
        if (cat) m_catalog = std::move(cat).value();
        else m_envError = tr("Model catalogue: %1").arg(qs(cat.error().message));
        m_store = std::make_unique<ModelStore>(m_env->modelsDir);
    }
    m_timer = new QTimer(this);
    m_timer->setInterval(50);
    connect(m_timer, &QTimer::timeout, this, &AiService::poll);
    rebuildSupervisor();
}

AiService::~AiService() {
    if (m_venvThread.joinable()) m_venvThread.join();
    if (m_sup) m_sup->stop();
}

void AiService::rebuildSupervisor() {
    if (m_sup) m_sup->stop();
    m_sup.reset();
    if (!m_env) return;
    const std::string py = m_ctx.settings().get<std::string>("ai.pythonPath");
    auto opt = workerOptionsFor(*m_env, py.empty() ? fs::path() : toPath(qs(py)));
    if (opt) m_sup = std::make_unique<WorkerSupervisor>(std::move(opt).value());
}

bool AiService::runtimeInstalled() const {
    if (!m_env) return false;
    if (!m_ctx.settings().get<std::string>("ai.pythonPath").empty()) return true;
    std::error_code ec;
    return fs::exists(venvPython(m_env->venvDir), ec);
}

WorkerState AiService::state() const { return m_sup ? m_sup->state() : WorkerState::Stopped; }

const nlohmann::json& AiService::capabilities() const {
    static const nlohmann::json empty = nlohmann::json::object();
    if (!m_sup || !m_sup->hello().contains("capabilities")) return empty;
    return m_sup->hello()["capabilities"];
}

QString AiService::statusText() const {
    if (!m_env) return tr("AI unavailable: %1").arg(m_envError);
    if (m_settingUp) return tr("Setting up the AI runtime…");
    if (!runtimeInstalled()) return tr("AI runtime not set up");
    switch (state()) {
    case WorkerState::Stopped: return tr("AI worker stopped");
    case WorkerState::Starting: return tr("Starting AI worker…");
    case WorkerState::Restarting: return tr("Restarting AI worker…");
    case WorkerState::Failed: return tr("AI worker failed: %1").arg(qs(m_sup->lastError()).section(QLatin1Char('\n'), 0, 0));
    case WorkerState::Ready: break;
    }
    const auto& caps = capabilities();
    QString device = tr("CPU only");
    if (!caps.value("probed", false)) device = tr("checking GPU…");
    for (const auto& d : caps.value("devices", nlohmann::json::array())) {
        device = tr("%1 (%2 GB)").arg(qs(d.value("name", std::string()))).arg(d.value("vramMb", 0) / 1024.0, 0, 'f', 1);
        break;
    }
    if (caps.value("devices", nlohmann::json::array()).empty())
        for (const auto& p : caps.value("onnxProviders", nlohmann::json::array()))
            if (p == "DmlExecutionProvider") device = tr("DirectML GPU (ONNX)");
    return tr("AI ready · Python %1 · %2").arg(qs(m_sup->hello().value("python", std::string()))).arg(device);
}

bool AiService::ensureStarted(QString* why) {
    if (!m_env) { if (why) *why = statusText(); return false; }
    if (!runtimeInstalled()) { if (why) *why = tr("Set up the AI runtime first (AI Studio › Set Up AI Runtime…)."); return false; }
    if (!m_sup) rebuildSupervisor();
    if (!m_sup) { if (why) *why = tr("The AI runtime could not be found."); return false; }
    if (m_sup->state() == WorkerState::Stopped || m_sup->state() == WorkerState::Failed) {
        m_sup->start();
        Q_EMIT stateChanged();
    }
    m_timer->start();
    return true;
}

void AiService::restartWorker() {
    // Keep the job history (the queue UI shows it); only rebuild when there
    // is no supervisor yet, e.g. right after the environment was created.
    if (m_sup) m_sup->restart();
    else rebuildSupervisor();
    QString why;
    if (!ensureStarted(&why)) Q_EMIT message(why);
    Q_EMIT stateChanged();
}

QStringList AiService::defaultProfiles(bool withDiffusion, bool nvidia) {
    QStringList p;
#ifdef _WIN32
    p << QStringLiteral("onnx-windows");
#else
    p << QStringLiteral("onnx-cpu");
#endif
    if (withDiffusion) p << (nvidia ? QStringLiteral("diffusion-cuda") : QStringLiteral("diffusion-cpu"));
    return p;
}

void AiService::setUpRuntime(const QStringList& profiles) {
    if (m_settingUp || !m_env) return;
    m_settingUp = true;
    m_pendingProfiles = profiles;
    Q_EMIT stateChanged();
    const fs::path venv = m_env->venvDir;
    const std::string configured = m_ctx.settings().get<std::string>("ai.pythonPath");
    if (m_venvThread.joinable()) m_venvThread.join();
    // Finding Python and creating the venv block for up to minutes.
    m_venvThread = std::thread([this, venv, configured] {
        QString error;
        if (configured.empty()) {
            auto py = findPython();
            if (!py) error = qs(py.error().message);
            else if (auto r = ensureVenv(py.value(), venv); !r) error = qs(r.error().message);
        }
        QMetaObject::invokeMethod(this, [this, error] {
            if (!error.isEmpty()) {
                m_settingUp = false;
                m_pendingProfiles.clear();
                Q_EMIT message(tr("AI setup failed: %1").arg(error));
                Q_EMIT stateChanged();
                return;
            }
            if (!m_sup) rebuildSupervisor();
            QString why;
            if (!ensureStarted(&why)) {
                m_settingUp = false;
                Q_EMIT message(why);
                Q_EMIT stateChanged();
                return;
            }
            runNextSetupStep();
        }, Qt::QueuedConnection);
    });
}

void AiService::runNextSetupStep() {
    if (m_pendingProfiles.isEmpty()) {
        m_settingUp = false;
        ACE_LOG_INFO("ai", "AI runtime setup finished; restarting the worker");
        restartWorker();   // pick up the new packages
        Q_EMIT message(tr("AI runtime installed."));
        return;
    }
    const QString profile = m_pendingProfiles.takeFirst();
    auto id = m_sup->submit("runtime.install", {{"profile", profile.toStdString()}}, tr("Install %1").arg(profile).toStdString());
    if (!id) {
        m_settingUp = false;
        Q_EMIT message(qs(id.error().message));
        Q_EMIT stateChanged();
        return;
    }
    m_setupJob = id.value();
    Q_EMIT jobChanged(qs(m_setupJob));
}

Result<JobId> AiService::submit(const std::string& task, nlohmann::json params, const QString& label,
                                const std::vector<std::pair<std::string, const Image*>>& images) {
    QString why;
    if (!ensureStarted(&why)) return Error{ErrorCode::NotFound, why.toStdString()};
    auto id = m_sup->submit(task, std::move(params), label.toStdString(), images);
    if (id) Q_EMIT jobChanged(qs(id.value()));
    return id;
}

void AiService::cancel(const JobId& id) {
    if (m_sup) (void)m_sup->cancel(id);
    poll();
}

const Job* AiService::job(const JobId& id) const { return m_sup ? m_sup->job(id) : nullptr; }

Result<JobId> AiService::download(const ModelEntry& m) {
    auto params = m_store->downloadParams(m);
    if (!params) return params.error();
    auto id = submit("model.download", params.value(), tr("Download %1").arg(qs(m.name)));
    if (id) m_downloads[id.value()] = m.id;
    return id;
}

void AiService::poll() {
    if (!m_sup) return;
    const WorkerState before = m_sup->state();
    std::vector<Event> events;
    try {
        events = m_sup->poll();
    } catch (const std::exception& e) {
        ACE_LOG_ERROR("ai", std::string("poll threw: ") + e.what());
        return;
    }
    std::set<JobId> changed;
    bool stateEvent = before != m_sup->state();
    for (const auto& e : events) {
        if (e.kind == Event::Kind::Job) changed.insert(e.job);
        if (e.kind == Event::Kind::WorkerState) stateEvent = true;
    }
    if (stateEvent) Q_EMIT stateChanged();
    for (const auto& id : changed) {
        const Job* j = m_sup->job(id);
        if (!j) continue;
        try {
            Q_EMIT jobChanged(qs(id));
        } catch (const std::exception& e) {
            ACE_LOG_ERROR("ai", std::string("jobChanged threw: ") + e.what());
        } catch (...) {
            ACE_LOG_ERROR("ai", "jobChanged threw an unknown exception");
        }
        if (!isFinished(j->state)) continue;
        if (auto it = m_downloads.find(id); it != m_downloads.end()) {
            if (j->state == JobState::Succeeded && m_catalog)
                if (const ModelEntry* m = m_catalog->find(it->second)) {
                    if (auto st = m_store->recordDownload(*m, j->info.value("sha256", nlohmann::json::object())); !st)
                        Q_EMIT message(qs(st.error().message));
                }
            m_downloads.erase(it);
            Q_EMIT modelsChanged();
        }
        if (id == m_setupJob) {
            m_setupJob.clear();
            if (j->state == JobState::Succeeded) {
                runNextSetupStep();
            } else {
                m_settingUp = false;
                m_pendingProfiles.clear();
                Q_EMIT message(tr("AI setup failed: %1").arg(qs(j->error)));
                Q_EMIT stateChanged();
            }
        }
        Q_EMIT jobFinished(qs(id));
    }
    // Idle and nothing pending: slow down polling.
    m_timer->setInterval(m_sup->idle() && m_sup->state() == WorkerState::Ready ? 250 : 50);
}

} // namespace acestudio::ui
