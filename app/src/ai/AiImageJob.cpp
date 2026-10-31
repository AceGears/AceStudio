#include "AiImageJob.h"

#include "AiService.h"
#include "QtBridge.h"

#include "acestudio/core/AiWorker.h"
#include "acestudio/core/ModelCatalog.h"
#include "acestudio/core/PngCodec.h"

namespace acestudio::ui {

using namespace acestudio::ai;
using nlohmann::json;

namespace {
std::string utf8(const std::filesystem::path& p) {
    const auto u = p.u8string();
    return std::string(u.begin(), u.end());
}
} // namespace

AiImageJob::AiImageJob(AiService& ai, QObject* parent) : QObject(parent), m_ai(ai) {
    connect(&m_ai, &AiService::jobChanged, this, [this](const QString& qid) {
        if (qid.toStdString() != m_job) return;
        const Job* j = m_ai.job(m_job);
        if (!j || j->state != JobState::Running) return;
        QString preview;
        if (j->previewSerial > 0 && j->previewSerial != m_previewSerial) {
            m_previewSerial = j->previewSerial;
            preview = fromPath(j->preview);
        }
        Q_EMIT progress(qs(j->message), preview);
    });
    connect(&m_ai, &AiService::jobFinished, this, [this](const QString& qid) {
        if (qid.toStdString() != m_job) return;
        const Job* j = m_ai.job(m_job);
        m_job.clear();
        if (!j) return Q_EMIT failed(tr("The job disappeared"));
        if (j->state == JobState::Cancelled) return Q_EMIT failed(tr("Cancelled"));
        if (j->state != JobState::Succeeded) return Q_EMIT failed(qs(j->error).section(QLatin1Char('\n'), 0, 0));
        auto load = [&](const char* key) -> Image {
            auto o = j->outputs.find(key);
            if (o == j->outputs.end()) return {};
            auto img = readPng(o->second);
            return img ? std::move(img).value() : Image();
        };
        Image raw = load("raw0"), pixel = load("image0");
        if (pixel.isNull()) return Q_EMIT failed(tr("The worker returned no image"));
        if (raw.isNull()) raw = pixel;
        Q_EMIT finished(pixel, raw);
    });
}

AiImageJob::~AiImageJob() { cancel(); }

bool AiImageJob::start(const Request& r, QString* why) {
    const ModelCatalog* cat = m_ai.catalog();
    ModelStore* store = m_ai.store();
    const ModelEntry* sdxl = cat ? cat->find("sdxl-base-1.0") : nullptr;
    if (!sdxl || !store || store->state(*sdxl) != InstallState::Installed) {
        if (why) *why = tr("This needs Stable Diffusion XL. Download it in AI Studio › Models…");
        return false;
    }
    if (!m_ai.ensureStarted(why)) return false;
    json p = {{"model_dir", utf8(store->entryPath(*sdxl))},
              {"prompt", r.prompt.toStdString()},
              {"negative_prompt", r.negative.toStdString()},
              {"width", r.width}, {"height", r.height},
              {"steps", r.steps}, {"guidance", 6.0},
              {"tileable", r.tileable.toStdString()},
              {"seed", r.seed}};
    if (const ModelEntry* l = cat->find("pixel-art-xl"); l && store->state(*l) == InstallState::Installed)
        p["lora"] = {{"path", utf8(store->entryPath(*l))}, {"scale", 1.0}};
    if (const ModelEntry* tae = cat->find("taesdxl"); tae && store->state(*tae) == InstallState::Installed)
        p["preview"] = {{"dir", utf8(store->entryPath(*tae))}, {"every", 4}};
    if (r.pixelWidth > 0 && r.pixelHeight > 0) {
        p["pixel"] = {{"width", r.pixelWidth}, {"height", r.pixelHeight}, {"crop", false}};
        if (!r.palette.empty()) {
            json pal = json::array();
            for (const auto& c : r.palette) pal.push_back(c.toHex(false));
            p["pixel"]["palette"] = pal;
        } else {
            p["pixel"]["colors"] = r.colors;
        }
    }
    auto id = m_ai.submit("image.generate", std::move(p), r.label.isEmpty() ? tr("Generate") : r.label);
    if (!id) {
        if (why) *why = qs(id.error().message);
        return false;
    }
    m_job = id.value();
    m_previewSerial = 0;
    return true;
}

void AiImageJob::cancel() {
    if (!m_job.empty()) m_ai.cancel(m_job);
}

} // namespace acestudio::ui
