#pragma once

#include "acestudio/core/AiWorker.h"
#include "acestudio/core/ModelCatalog.h"

#include <QObject>
#include <QString>

#include <atomic>
#include <memory>
#include <optional>
#include <thread>

class QTimer;

namespace acestudio::ui {

class AppContext;

// App-side owner of the AI worker: wraps WorkerSupervisor (polled on a
// timer), the model catalogue and store, and runtime setup. Lives on the
// GUI thread; only venv creation runs on a helper thread.
class AiService : public QObject {
    Q_OBJECT
public:
    explicit AiService(AppContext& ctx, QObject* parent = nullptr);
    ~AiService() override;

    // Paths and catalogue; error text if the worker files are missing.
    bool environmentOk() const { return m_env.has_value(); }
    QString environmentError() const { return m_envError; }
    const ai::AiEnvironment* environment() const { return m_env ? &*m_env : nullptr; }
    const ai::ModelCatalog* catalog() const { return m_catalog ? &*m_catalog : nullptr; }
    ai::ModelStore* store() { return m_store.get(); }

    bool runtimeInstalled() const;   // the AI Python environment exists
    bool settingUp() const { return m_settingUp; }
    ai::WorkerSupervisor* supervisor() { return m_sup.get(); }
    ai::WorkerState state() const;
    QString statusText() const;      // one line for the panel
    const nlohmann::json& capabilities() const;

    // Starts the worker if the runtime exists. Returns false with a reason otherwise.
    bool ensureStarted(QString* why = nullptr);
    void restartWorker();

    // Creates the Python environment (if needed) and installs `profiles`
    // in order, then restarts the worker. Progress arrives as jobChanged.
    void setUpRuntime(const QStringList& profiles);

    Result<ai::JobId> submit(const std::string& task, nlohmann::json params, const QString& label,
                             const std::vector<std::pair<std::string, const Image*>>& images = {});
    void cancel(const ai::JobId& id);
    const ai::Job* job(const ai::JobId& id) const;

    // Model downloads (approval must already be recorded in the store).
    Result<ai::JobId> download(const ai::ModelEntry& m);

    // Profiles suited to this platform: ONNX first, then diffusion.
    static QStringList defaultProfiles(bool withDiffusion, bool nvidia);

Q_SIGNALS:
    void stateChanged();
    void jobChanged(const QString& id);
    void jobFinished(const QString& id);
    void modelsChanged();
    void message(const QString& text);

private:
    void poll();
    void rebuildSupervisor();
    void runNextSetupStep();

    AppContext& m_ctx;
    std::optional<ai::AiEnvironment> m_env;
    QString m_envError;
    std::optional<ai::ModelCatalog> m_catalog;
    std::unique_ptr<ai::ModelStore> m_store;
    std::unique_ptr<ai::WorkerSupervisor> m_sup;
    QTimer* m_timer = nullptr;
    // Runtime setup state.
    bool m_settingUp = false;
    QStringList m_pendingProfiles;
    ai::JobId m_setupJob;
    std::thread m_venvThread;
    std::map<ai::JobId, std::string> m_downloads;   // job -> model id
};

} // namespace acestudio::ui
